// Netplay core, see netplay.h.
//
// Packets (little endian): u32 magic "MK9N", u8 type, u8 slot, u16 match (barrier number), then
//   1 HELLO  -                                  presence while connecting (and between matches)
//   2 READY  u32 seed, u8 n, n x u32, u8 delay  "my match is about to be set up": the barrier ends
//                                               when every slot's READY of this match arrived; the
//                                               match runs with the largest input delay any side asked
//   3 INPUT  u32 ack, u32 now, u32 seen, u32 first, u8 n, n x 8 bytes
//            the sender's own inputs for frames first..first+n-1; ack: how many of the receiver's
//            inputs the sender holds without a gap; now: the frame the sender runs; seen: the
//            frame the sender last heard the receiver run (for keeping rollback's sides even)
//   4 CHECK  u32 frame, u32 hash                the sender's state hash at the top of that frame
//   5 BYE    u32 frame                          the sender left the match
//
// The spectator feed (TCP to the launcher on this PC, each packet after its u16 length; the header's
// slot is the sender's): a player's game writes
//   6 MEET   u32 seed, u8 n, n x u32            its READY at a barrier
//   7 OWN    u32 first, u8 n, n x 8 bytes       its own inputs, frames first..first+n-1
//   8 CHECK  u32 frame, u32 hash                its checks
//   9 END    u32 frame                          it left the match
// and a spectator's game reads every player's from it (the launcher sends a spectator everything
// again when it connects again: duplicates are taken as they come) and plays the match in lockstep
// with no player of its own.
//
// Rollback (from match 2, while a round runs): a frame runs as soon as it is due, with a guess for
// the inputs that have not come yet (the player's last one). Inputs that come in for frames that
// ran on a wrong guess put the game back to the first of them (mk9/net/rollback.h), and the frames
// since run again, unseen, within the same frame. Checks wait until every input before their
// frame is known. Outside rounds (menus, loading, between rounds) the frames go in lockstep.

#include "mk9/net/netplay.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "mk9/autopilot.h"
#include "mk9/engine.h"
#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/net/rollback.h"
#include "mk9/snap.h"
#include "mk9/testinput.h"

#pragma comment(lib, "ws2_32.lib")

namespace mk9::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E394B4D;  // "MK9N"
enum : uint8_t { kHello = 1, kReady = 2, kInput = 3, kCheck = 4, kBye = 5 };
enum : uint8_t { kFeedMeet = 6, kFeedOwn = 7, kFeedCheck = 8, kFeedEnd = 9 };
constexpr int kWindow = 24;               // inputs repeated in every INPUT packet
constexpr int kMaxWords = 64;
constexpr auto kPeerTimeout = std::chrono::seconds(10);
constexpr auto kBarrierTimeout = std::chrono::seconds(60);
// rollback: frames run ahead of the last input someone sent, at most (well within what rollback
// keeps, rb::kKeepFrames)
constexpr uint32_t kMaxPredict = 8;
// how far ahead of this game's frame a peer's inputs and checks are kept, at most (a spectator's feed:
// it may watch far behind the players); what lies beyond is left out
constexpr uint32_t kMaxAhead = 600, kMaxFeedAhead = 60 * 3600;

struct Wire {
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int8_t lx = 0, ly = 0, rx = 0, ry = 0;
  bool operator==(const Wire& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
};
static_assert(sizeof(Wire) == 8);

Wire Pack(const input::Pad& p) {
  Wire w;
  w.buttons = p.buttons;
  w.lt = p.left_trigger;
  w.rt = p.right_trigger;
  w.lx = int8_t(p.lx >> 8);
  w.ly = int8_t(p.ly >> 8);
  w.rx = int8_t(p.rx >> 8);
  w.ry = int8_t(p.ry >> 8);
  return w;
}

input::Pad Unpack(const Wire& w) {
  input::Pad p;
  p.connected = true;
  p.buttons = w.buttons;
  p.left_trigger = w.lt;
  p.right_trigger = w.rt;
  p.lx = int16_t(w.lx * 256);
  p.ly = int16_t(w.ly * 256);
  p.rx = int16_t(w.rx * 256);
  p.ry = int16_t(w.ry * 256);
  return p;
}

struct PeerState {
  int slot = -1;
  sockaddr_in addr{};
  bool seen = false;
  Clock::time_point last_heard{};
  std::map<uint32_t, Wire> inputs;  // their own inputs in the match in sync
  uint32_t contiguous = 0;          // their inputs held without a gap: [0, contiguous)
  uint32_t ack = 0;                 // how many of ours they hold without a gap
  uint32_t now = 0;                 // the frame they said they run
  uint16_t ready_match = 0;         // the match their last READY was for (they wait there or are past it)
  uint32_t seed = 0;
  std::vector<uint32_t> words;
  int delay = -1;  // the input delay their READY asked for
  uint32_t old_ack = 0, old_now = 0;  // they are still in the match before ours: what they need of us
  std::map<uint32_t, uint32_t> checks;
  bool left = false;
  std::map<uint32_t, Wire> used;  // what each frame ran with for this player (theirs or a guess)
  uint32_t confirmed = 0;         // frames below this ran with their real inputs
  uint32_t heard = 0;             // the frame of ours they last heard of
};

struct Outgoing {
  Clock::time_point when;
  sockaddr_in to;
  std::vector<uint8_t> data;
};

std::mutex g_cfg_lock;
Settings g_pending;
bool g_pending_set = false, g_pending_off = false;

Settings g_cfg;
std::atomic<Status> g_status{Status::Off};
SOCKET g_sock = INVALID_SOCKET;
std::vector<PeerState> g_peers;
uint16_t g_match = 0;
uint32_t g_frame = 0;
std::map<uint32_t, Wire> g_mine;
Wire g_now[kMaxSlots];
std::map<uint32_t, uint32_t> g_my_checks;
std::deque<Outgoing> g_outbox;
std::mt19937 g_rng{12345};
Stats g_stats;
FILE* g_log = nullptr;
Clock::time_point g_last_hello{};
bool g_rolling = false;                    // rollback armed: frames may run on guesses
std::map<uint32_t, uint32_t> g_tentative;  // state hashes at frame tops, until every input before is known
// what each check frame's hash was made of (the log's "timer" and "rand"), as of its last run
std::map<uint32_t, std::pair<uint32_t, uint32_t>> g_check_detail;
uint32_t g_paced_at = 0;                   // the last frame held back to let the others catch up

// this side's own input delay, and the match's: the largest any side asked at the barrier (each
// side's inputs start `delay` frames in, so every side must use the same)
int OwnDelay() { return g_cfg.delay >= 0 ? g_cfg.delay : (g_cfg.rollback ? 1 : 2); }
int g_match_delay = -1;
int Delay() { return g_match_delay >= 0 ? g_match_delay : OwnDelay(); }

bool ParseAddress(const std::string& s, sockaddr_in& out) {
  auto colon = s.rfind(':');
  if (colon == std::string::npos) return false;
  std::memset(&out, 0, sizeof out);
  out.sin_family = AF_INET;
  out.sin_port = htons(uint16_t(std::atoi(s.c_str() + colon + 1)));
  return inet_pton(AF_INET, s.substr(0, colon).c_str(), &out.sin_addr) == 1;
}

struct Writer {
  uint8_t b[1400];
  int n = 0;
  void U8(uint8_t v) { b[n++] = v; }
  void U16(uint16_t v) { std::memcpy(b + n, &v, 2); n += 2; }
  void U32(uint32_t v) { std::memcpy(b + n, &v, 4); n += 4; }
  void Bytes(const void* p, int len) { std::memcpy(b + n, p, len); n += len; }
};

Writer Header(uint8_t type) {
  Writer w;
  w.U32(kMagic);
  w.U8(type);
  w.U8(uint8_t(g_cfg.slot));
  w.U16(g_match);
  return w;
}

void SendNow(const sockaddr_in& to, const uint8_t* b, int n) {
  sendto(g_sock, reinterpret_cast<const char*>(b), n, 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

void Send(const PeerState& p, const Writer& w) {
  if (g_cfg.loss_percent && int(g_rng() % 100) < g_cfg.loss_percent) return;
  if (!g_cfg.lag_ms && !g_cfg.jitter_ms) return SendNow(p.addr, w.b, w.n);
  int extra = g_cfg.jitter_ms ? int(g_rng() % uint32_t(g_cfg.jitter_ms + 1)) : 0;
  g_outbox.push_back({Clock::now() + std::chrono::milliseconds(g_cfg.lag_ms + extra), p.addr,
                      std::vector<uint8_t>(w.b, w.b + w.n)});
}

void SendAll(const Writer& w) {
  for (const PeerState& p : g_peers) Send(p, w);
}

void Pump() {
  const auto now = Clock::now();
  // the simulated network delivers in send order per destination once due
  while (!g_outbox.empty() && g_outbox.front().when <= now) {
    const Outgoing& o = g_outbox.front();
    SendNow(o.to, o.data.data(), int(o.data.size()));
    g_outbox.pop_front();
  }
}

PeerState* PeerBySlot(int slot) {
  for (PeerState& p : g_peers)
    if (p.slot == slot) return &p;
  return nullptr;
}

void SendInputs(PeerState& p) {
  if (g_mine.empty()) return;
  const uint32_t last = g_mine.rbegin()->first;
  uint32_t first = std::max(p.ack, last + 1 >= uint32_t(kWindow) ? last + 1 - kWindow : 0u);
  first = std::max(first, g_mine.begin()->first);
  if (first > last) first = last;
  Writer w = Header(kInput);
  w.U32(p.contiguous);
  w.U32(g_frame);
  w.U32(p.now);
  w.U32(first);
  w.U8(uint8_t(last - first + 1));
  for (uint32_t f = first; f <= last; f++) {
    auto it = g_mine.find(f);
    Wire in = it != g_mine.end() ? it->second : Wire{};
    w.Bytes(&in, sizeof in);
  }
  Send(p, w);
}

// what a spectator's feed brought for matches it has not reached yet (the feed sends each once)
std::map<uint16_t, std::vector<std::string>> g_feed_later;

uint32_t Horizon() { return g_frame + (g_cfg.watch ? kMaxFeedAhead : kMaxAhead); }

void TakeInputs(PeerState* p, uint32_t first, int count, const uint8_t* at, int room) {
  for (int i = 0; i < count && i * 8 + 8 <= room; i++) {
    if (first + uint32_t(i) > Horizon()) break;
    Wire in;
    std::memcpy(&in, at + i * 8, 8);
    p->inputs.emplace(first + uint32_t(i), in);
  }
  while (p->inputs.count(p->contiguous)) p->contiguous++;
}

// one packet from a peer (UDP) or, for a spectator, from the feed
void HandlePacket(const uint8_t* b, int n) {
  if (n < 8) return;
  uint32_t magic;
  std::memcpy(&magic, b, 4);
  if (magic != kMagic) return;
  const uint8_t type = b[4];
  const int slot = b[5];
  uint16_t match;
  std::memcpy(&match, b + 6, 2);
  PeerState* p = PeerBySlot(slot);
  if (!p) return;
  p->seen = true;
  p->last_heard = Clock::now();
  auto u32 = [&](int o) {
    uint32_t v;
    std::memcpy(&v, b + o, 4);
    return v;
  };
  const bool feed_type = type >= kFeedMeet && type <= kFeedEnd;
  // the feed sends each packet once: what is for a match this spectator has not reached waits
  const bool later = feed_type && int16_t(match - g_match) > (type == kFeedMeet ? 1 : 0);
  if (later) {
    g_feed_later[match].emplace_back(reinterpret_cast<const char*>(b), size_t(n));
    return;
  }
  if ((type == kReady || type == kFeedMeet) && n >= 13) {
    // the barrier of this match, or the next one when they got there first (they then need nothing more
    // of this match from us, and we play their part of it as neutral input until we get there too)
    if (match != g_match && match != uint16_t(g_match + 1)) return;
    if (match == p->ready_match) return;
    p->ready_match = match;
    p->seed = u32(8);
    p->words.clear();
    for (int i = 0; i < b[12] && 13 + i * 4 + 4 <= n; i++) p->words.push_back(u32(13 + i * 4));
    const int after = 13 + b[12] * 4;
    p->delay = after < n ? b[after] : -1;
  } else if (type == kInput && n >= 25) {
    if (match == uint16_t(g_match - 1)) {
      // they are still in the match we left for the next barrier: our part of it is neutral from here on
      p->old_ack = std::max(p->old_ack, u32(8));
      p->old_now = std::max(p->old_now, u32(12));
      return;
    }
    if (match != g_match) return;
    p->ack = std::max(p->ack, u32(8));
    p->now = std::max(p->now, u32(12));
    p->heard = std::max(p->heard, u32(16));
    TakeInputs(p, u32(20), b[24], b + 25, n - 25);
  } else if (type == kFeedOwn && n >= 13 && match == g_match) {
    TakeInputs(p, u32(8), b[12], b + 13, n - 13);
  } else if ((type == kCheck || type == kFeedCheck) && n >= 16 && match == g_match && u32(8) <= Horizon()) {
    p->checks[u32(8)] = u32(12);
  } else if ((type == kBye || type == kFeedEnd) && match == g_match) {
    p->left = true;
  }
}

// --- the spectator feed: a TCP connection to the launcher on this PC, each packet after its u16
// length. A player's game writes to it, a spectator's reads every player's packets from it.
SOCKET g_feed = INVALID_SOCKET;
bool g_feed_connected = false;
Clock::time_point g_feed_retry{};
sockaddr_in g_feed_addr{};
std::string g_feed_out, g_feed_in;
constexpr size_t kFeedOutMax = size_t(4) << 20;  // what may wait for a launcher that does not read

void FeedClose() {
  if (g_feed != INVALID_SOCKET) closesocket(g_feed);
  g_feed = INVALID_SOCKET;
  g_feed_connected = false;
  g_feed_in.clear();
}

void FeedPump() {
  if (g_cfg.feed.empty()) return;
  if (g_feed == INVALID_SOCKET) {
    if (Clock::now() < g_feed_retry) return;
    g_feed_retry = Clock::now() + std::chrono::milliseconds(500);
    g_feed = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_feed == INVALID_SOCKET) return;
    u_long nb = 1;
    ioctlsocket(g_feed, FIONBIO, &nb);
    BOOL nodelay = TRUE;
    setsockopt(g_feed, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
    if (connect(g_feed, reinterpret_cast<const sockaddr*>(&g_feed_addr), sizeof g_feed_addr) == 0) {
      g_feed_connected = true;
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
      FeedClose();
      return;
    }
  }
  if (!g_feed_connected) {
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(g_feed, &w);
    FD_SET(g_feed, &e);
    timeval t{0, 0};
    if (select(0, nullptr, &w, &e, &t) <= 0) return;
    if (FD_ISSET(g_feed, &e) || !FD_ISSET(g_feed, &w)) {
      FeedClose();
      return;
    }
    g_feed_connected = true;
    REXLOG_INFO("mk9 net: the spectator feed is connected");
  }
  while (!g_feed_out.empty()) {
    const int n = send(g_feed, g_feed_out.data(), int(std::min<size_t>(g_feed_out.size(), 1 << 16)), 0);
    if (n > 0) {
      g_feed_out.erase(0, size_t(n));
      continue;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) FeedClose();
    break;
  }
  if (!g_cfg.watch || g_feed == INVALID_SOCKET) return;
  char buf[8192];
  for (;;) {
    const int n = recv(g_feed, buf, sizeof buf, 0);
    if (n > 0) {
      g_feed_in.append(buf, size_t(n));
      continue;
    }
    if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK) FeedClose();
    break;
  }
  size_t at = 0;
  while (g_feed_in.size() - at >= 2) {
    uint16_t len;
    std::memcpy(&len, g_feed_in.data() + at, 2);
    if (g_feed_in.size() - at - 2 < len) break;
    HandlePacket(reinterpret_cast<const uint8_t*>(g_feed_in.data()) + at + 2, len);
    at += 2 + size_t(len);
  }
  g_feed_in.erase(0, at);
}

void FeedSend(const Writer& w) {
  if (g_cfg.feed.empty() || g_cfg.watch || g_feed_out.size() > kFeedOutMax) return;
  const uint16_t len = uint16_t(w.n);
  g_feed_out.append(reinterpret_cast<const char*>(&len), 2);
  g_feed_out.append(reinterpret_cast<const char*>(w.b), size_t(w.n));
}

void FeedOwn(uint32_t frame, const Wire& in) {
  if (g_cfg.feed.empty()) return;
  Writer w = Header(kFeedOwn);
  w.U32(frame);
  w.U8(1);
  w.Bytes(&in, sizeof in);
  FeedSend(w);
}

void Receive() {
  Pump();
  FeedPump();
  if (g_sock == INVALID_SOCKET) return;
  uint8_t b[1500];
  for (;;) {
    sockaddr_in from{};
    int fl = sizeof from;
    int n = recvfrom(g_sock, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 8) return;
    uint32_t magic;
    std::memcpy(&magic, b, 4);
    if (magic != kMagic) continue;  // not a game's (a launcher's punch that came late)
    // only the address this match set up for the player a packet names (their game, the server's relay or
    // the launcher's tunnel) is heard: a game that played this PC before on this port, or anyone else who
    // learned it, could otherwise put inputs into this match or end it
    const PeerState* p = PeerBySlot(b[5]);
    if (!p || from.sin_family != AF_INET || from.sin_addr.s_addr != p->addr.sin_addr.s_addr ||
        from.sin_port != p->addr.sin_port) {
      if (++g_stats.ignored == 1 || g_stats.ignored % 1000 == 0) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        REXLOG_WARN("mk9 net: a packet from {}:{}, not this match's peer: ignored ({} so far)", ip,
                    ntohs(from.sin_port), g_stats.ignored);
      }
      continue;
    }
    HandlePacket(b, n);
  }
}

void CloseSocket() {
  if (g_sock != INVALID_SOCKET) closesocket(g_sock);
  g_sock = INVALID_SOCKET;
}

bool Open(const Settings& st) {
  static bool wsa = [] {
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0;
  }();
  if (!wsa) return false;
  CloseSocket();
  FeedClose();
  g_feed_out.clear();
  g_feed_later.clear();
  if (!st.feed.empty() && !ParseAddress(st.feed, g_feed_addr)) {
    REXLOG_ERROR("mk9 net: bad spectator feed address {}", st.feed);
    return false;
  }
  if (st.watch) {
    // a spectator: no socket of its own, every player comes from the feed
    if (st.feed.empty()) return false;
    g_cfg = st;
    g_peers.clear();
    for (int slot = 0; slot < st.players; slot++) {
      PeerState p;
      p.slot = slot;
      p.last_heard = Clock::now();
      g_peers.push_back(p);
    }
    if (g_log) std::fclose(g_log);
    g_log = st.log.empty() ? nullptr : std::fopen(st.log.c_str(), "w");
    g_match = 0;
    g_status = Status::Connecting;
    REXLOG_INFO("mk9 net: watching {} players from the feed {}", st.players, st.feed);
    return true;
  }
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock == INVALID_SOCKET) return false;
  u_long nb = 1;
  ioctlsocket(g_sock, FIONBIO, &nb);
  int buf = 1 << 20;
  setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof buf);
  sockaddr_in me{};
  me.sin_family = AF_INET;
  me.sin_port = htons(st.bind);
  me.sin_addr.s_addr = st.loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);
  if (bind(g_sock, reinterpret_cast<sockaddr*>(&me), sizeof me) != 0) {
    REXLOG_ERROR("mk9 net: cannot bind UDP port {}", st.bind);
    CloseSocket();
    return false;
  }
  g_cfg = st;
  g_peers.clear();
  for (const Peer& pe : st.peers) {
    PeerState p;
    p.slot = pe.slot;
    if (!ParseAddress(pe.address, p.addr)) {
      REXLOG_ERROR("mk9 net: bad peer address {}", pe.address);
      CloseSocket();
      return false;
    }
    g_peers.push_back(p);
  }
  if (g_log) std::fclose(g_log);
  g_log = st.log.empty() ? nullptr : std::fopen(st.log.c_str(), "w");
  g_match = 0;
  g_match_delay = -1;
  g_status = Status::Connecting;
  REXLOG_INFO("mk9 net: slot {} of {}, port {}, {} peers, {} delay {}", st.slot, st.players, st.bind,
              g_peers.size(), st.rollback ? "rollback" : "lockstep", Delay());
  return true;
}

void ApplyPending() {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  if (g_pending_off) {
    g_pending_off = false;
    if (g_status != Status::Off) {
      Writer w = Header(kBye);
      w.U32(g_frame);
      if (!g_cfg.watch) SendAll(w);
      Writer end = Header(kFeedEnd);
      end.U32(g_frame);
      FeedSend(end);
      for (int i = 0; i < 20 && !g_feed_out.empty() && g_feed != INVALID_SOCKET; i++) {
        FeedPump();
        Sleep(5);
      }
    }
    CloseSocket();
    FeedClose();
    g_status = Status::Off;
    input::SetSource(nullptr);
  }
  if (g_pending_set) {
    g_pending_set = false;
    Open(g_pending);
  }
}

// one line per frame run: its number ("r" in front when a rollback ran it again), every slot's
// input, and its check when it has one (the last line of a frame is the one that counts)
void LogFrame(bool again) {
  if (!g_log) return;
  std::fprintf(g_log, again ? "r%u" : "%u", g_frame);
  for (int s = 0; s < g_cfg.players; s++)
    std::fprintf(g_log, " %04X:%02X%02X:%02X%02X%02X%02X", g_now[s].buttons, g_now[s].lt, g_now[s].rt,
                 uint8_t(g_now[s].lx), uint8_t(g_now[s].ly), uint8_t(g_now[s].rx), uint8_t(g_now[s].ry));
  auto it = g_my_checks.find(g_frame);
  auto t = g_tentative.find(g_frame);
  if (it != g_my_checks.end() || t != g_tentative.end()) {
    uint32_t timer = 0, rand_state = 0;
    if (auto d = g_check_detail.find(g_frame); d != g_check_detail.end()) {
      timer = d->second.first;
      rand_state = d->second.second;
    } else {
      game::LastCheckDetail(timer, rand_state);
    }
    std::fprintf(g_log, " check %08X timer %u rand %08X", it != g_my_checks.end() ? it->second : t->second,
                 timer, rand_state);
  }
  std::fputc('\n', g_log);
}

// every other player's inputs up to frame f are here (false: someone is gone)
bool WaitForInputs(uint32_t f) {
  auto started = Clock::now();
  bool waited = false;
  for (;;) {
    bool all = true;
    for (PeerState& p : g_peers) {
      // waiting at the next barrier already: they play no more of this match
      if (p.ready_match == uint16_t(g_match + 1)) {
        while (p.contiguous <= f) p.inputs.emplace(p.contiguous++, Wire{});
        while (p.inputs.count(p.contiguous)) p.contiguous++;
      }
      if (p.contiguous > f) continue;
      all = false;
      if (p.left || Clock::now() - p.last_heard > kPeerTimeout) {
        REXLOG_WARN("mk9 net: slot {} stopped answering at frame {}", p.slot, f);
        return false;
      }
    }
    if (all) {
      if (waited) g_stats.waits++;
      return true;
    }
    waited = true;
    // ask again: our latest inputs (theirs may wait on ours)
    if (!g_cfg.watch && Clock::now() - started > std::chrono::milliseconds(8)) {
      for (PeerState& p : g_peers) SendInputs(p);
      g_stats.resent++;
      started = Clock::now();
    }
    std::this_thread::sleep_for(std::chrono::microseconds(250));
    Receive();
  }
}

void CompareChecks() {
  for (PeerState& p : g_peers) {
    for (auto it = p.checks.begin(); it != p.checks.end();) {
      auto mine = g_my_checks.find(it->first);
      if (mine == g_my_checks.end()) {
        ++it;
        continue;
      }
      if (mine->second != it->second) {
        REXLOG_ERROR("mk9 net: DESYNC at frame {} with slot {}: {:08X} here, {:08X} there", it->first, p.slot,
                     mine->second, it->second);
        // tests may play on to see how far apart the games get
        static const bool keep_going = std::getenv("MK9_NET_DESYNC_CONTINUE") != nullptr;
        if (!keep_going) g_status = Status::Desynced;
      }
      it = p.checks.erase(it);
    }
  }
}

// the input a player's slot runs with at frame f: theirs when it is here, else a guess: the last one
// they sent (buttons are held for many frames)
Wire Guess(const PeerState& p, uint32_t f) {
  auto it = p.inputs.find(f);
  if (it != p.inputs.end()) return it->second;
  auto last = p.contiguous ? p.inputs.find(p.contiguous - 1) : p.inputs.end();
  return last != p.inputs.end() ? last->second : Wire{};
}

void SetFrameInputs(uint32_t f) {
  if (!g_cfg.watch) g_now[g_cfg.slot] = g_mine[f];
  for (PeerState& p : g_peers) {
    const Wire w = Guess(p, f);
    p.used[f] = w;
    g_now[p.slot] = w;
  }
}

// every 30 frames both sides compare what the game says its state is at the top of the frame (not
// while a fight loads: game::Comparable); with rollback the hash is sent once every input before the
// frame is known (and was used)
void Tentative(uint32_t f, PPCContext& ctx, uint8_t* base) {
  if (f % 30 == 0 && game::Comparable()) {
    g_tentative[f] = game::StateHash(ctx, base);
    uint32_t timer, rand_state;
    game::LastCheckDetail(timer, rand_state);
    g_check_detail[f] = {timer, rand_state};
    while (g_check_detail.size() > 64) g_check_detail.erase(g_check_detail.begin());
  }
}

void SettleChecks() {
  uint32_t settled = g_frame;
  for (const PeerState& p : g_peers) settled = std::min(settled, p.confirmed);
  while (!g_tentative.empty() && g_tentative.begin()->first <= settled) {
    Check(g_tentative.begin()->first, g_tentative.begin()->second);
    g_tentative.erase(g_tentative.begin());
  }
}

bool StartRolling(PPCContext& ctx) {
  engine::Hold();
  engine::ExcludeFromRollback();
  if (!rb::Arm(ctx, g_frame)) return false;
  g_rolling = true;
  for (PeerState& p : g_peers) p.confirmed = g_frame;
  REXLOG_INFO("mk9 net: rollback from frame {} of match {}", g_frame, g_match);
  return true;
}

void StopRolling() {
  if (!g_rolling) return;
  rb::Disarm();
  g_rolling = false;
  REXLOG_INFO("mk9 net: lockstep from frame {} of match {} ({} rollbacks so far, longest {} frames)", g_frame,
              g_match, g_stats.rollbacks, g_stats.max_rollback);
}

void Lost(Status s) {
  StopRolling();
  g_status = s;
  input::SetSource(nullptr);
}

// Inputs that came in for frames that ran on a guess: when a guess was wrong, put the game back to
// the top of that frame and run the frames since again with what is known now. False when the game
// cannot go back that far (it then no longer runs what the others run).
bool CorrectGuesses(PPCContext& ctx, uint8_t* base) {
  uint32_t wrong = g_frame;
  for (PeerState& p : g_peers) {
    const uint32_t known = std::min(p.contiguous, g_frame);
    for (uint32_t f = p.confirmed; f < known; f++) {
      auto u = p.used.find(f);
      if (u != p.used.end() && !(u->second == p.inputs[f])) {
        wrong = std::min(wrong, f);
        break;
      }
    }
    p.confirmed = std::max(p.confirmed, known);
  }
  if (wrong >= g_frame) return true;
  const uint32_t now = g_frame;
  const PPCContext top = ctx;
  if (!rb::Restore(wrong, ctx)) {
    ctx = top;
    REXLOG_ERROR("mk9 net: cannot go back to frame {} from {}", wrong, now);
    return false;
  }
  game::RolledBack(wrong);
  frame::SetCount(frame::Count() - (now - wrong));
  for (uint32_t f = wrong; f < now; f++) {
    g_frame = f;
    SetFrameInputs(f);
    ctx = top;  // the registers at the top of a frame are GuardedMain's loop's, the same every frame
    rb::OpenFrame(f, ctx);
    Tentative(f, ctx, base);
    engine::Release();
    frame::RunAgain(ctx, base);  // its end hook moves g_frame on
    engine::Hold();
    rb::CloseFrame();
  }
  ctx = top;
  g_frame = now;
  g_stats.rollbacks++;
  g_stats.max_rollback = std::max(g_stats.max_rollback, now - wrong);
  return true;
}

// Keep the sides even: when this PC runs further ahead of the others than they run ahead of it, it
// would do all the rolling back; hold a frame back now and then until it does not
void Pace() {
  int ahead = 0;
  for (const PeerState& p : g_peers) {
    const int mine = int(g_frame) - int(p.now);  // how far ahead of them we are, as we hear them
    const int theirs = int(p.now) - int(p.heard);  // how far ahead of us they were, as they heard us
    ahead = std::max(ahead, mine - theirs);       // twice the frames we are ahead by
  }
  if (ahead >= 3 && g_frame - g_paced_at >= 20) {
    g_paced_at = g_frame;
    std::this_thread::sleep_for(std::chrono::microseconds(16667));
    Receive();
  }
}

uint64_t g_meet_frame = 0;  // tests: meet at the top of this frame (counted from boot)
std::atomic<bool> g_meet_asked{false};  // the launcher's start
std::atomic<uint32_t> g_meet_seed{0};

void OnTop(PPCContext& ctx, uint8_t* base) {
  // the rendering thread and the job workers, held while rollback keeps or puts back memory, go
  // on once the frame does
  struct ReleaseAtEnd {
    ~ReleaseAtEnd() { engine::Release(); }
  } release_at_end;
  ApplyPending();
  if (g_status == Status::Off) return StopRolling();
  Receive();
  const bool meet_now = g_status == Status::Connecting &&
                        ((g_meet_frame && frame::Count() == g_meet_frame) || g_meet_asked.exchange(false));
  if (meet_now) {
    Agreement agreed;
    const uint32_t seed = g_meet_seed.load() ? g_meet_seed.load() : 0x1234567u + uint32_t(g_cfg.slot);
    Meet(game::ReadyWords(), seed, agreed);
    if (g_status == Status::InSync && !game::SameRelease(agreed)) return Lost(Status::Mismatch);
    if (g_status == Status::InSync) game::Agree(ctx, base, agreed);
  }
  if (g_status == Status::Connecting || (g_status != Status::InSync && g_status != Status::Barrier)) {
    StopRolling();
    if (Clock::now() - g_last_hello > std::chrono::milliseconds(250)) {
      SendAll(Header(kHello));
      g_last_hello = Clock::now();
    }
    return;
  }
  if (g_status != Status::InSync) return;
  if (g_rolling) {
    engine::Hold();
    rb::CloseFrame();  // the frame that just ran
    if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
    // the fight is over: lockstep again once every frame so far ran with the real inputs
    if (game::Phase() != game::kPhaseFight) {
      if (g_frame && !WaitForInputs(g_frame - 1)) return Lost(Status::Disconnected);
      if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
      if (game::Phase() != game::kPhaseFight) StopRolling();
    }
  }
  // The game reached a point where everyone must start alike (a fight, once it is loaded): meet again there,
  // whatever number of frames each PC needed to get there, and start the frames anew
  if (!g_rolling && game::SyncPoint()) {
    Agreement agreed;
    if (!Meet(game::ReadyWords(), 0x89ABCDEFu ^ uint32_t(g_match), agreed)) return;
    if (!game::SameRelease(agreed)) return Lost(Status::Mismatch);
    game::Agree(ctx, base, agreed);
  }
  // this frame's local input waits `delay` frames (a test's script for this slot stands in for the pad)
  if (!g_cfg.watch) {
    input::Pad local;
    if (!autopilot::LocalPad(local) && !testinput::ScriptedPad(uint32_t(g_cfg.slot), local))
      local = input::ReadPhysical(0);
    const uint32_t at = g_frame + uint32_t(Delay());
    g_mine[at] = Pack(local);
    FeedOwn(at, g_mine[at]);
    for (PeerState& p : g_peers) SendInputs(p);
  }
  if (g_rolling) {
    // never further ahead of what anyone sent than kMaxPredict frames
    if (g_frame >= kMaxPredict && !WaitForInputs(g_frame - kMaxPredict)) return Lost(Status::Disconnected);
    if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
    Pace();
    rb::OpenFrame(g_frame, ctx);
  } else if (!(g_cfg.rollback && !g_cfg.watch && g_match >= 2 && game::Phase() == game::kPhaseFight &&
               StartRolling(ctx))) {
    if (!WaitForInputs(g_frame)) return Lost(Status::Disconnected);
    for (PeerState& p : g_peers) p.confirmed = g_frame + 1;
  }
  SetFrameInputs(g_frame);
  Tentative(g_frame, ctx, base);
  SettleChecks();
  CompareChecks();
}

// tests: MK9_NET_SNAPS="2:600,2:1200" screenshots at frames of a match in sync (comparable across PCs)
std::vector<std::pair<uint16_t, uint32_t>> g_sync_snaps;

void OnEnd(PPCContext&, uint8_t*) {
  if (g_status != Status::InSync) return;
  if (frame::Replaying()) {  // a frame a rollback runs again: only its log line
    LogFrame(true);
    g_frame++;
    return;
  }
  LogFrame(false);
  for (const auto& s : g_sync_snaps)
    if (s.first == g_match && s.second == g_frame) {
      const std::string name = "sync-" + std::to_string(g_match) + "-" + std::to_string(g_frame);
      std::thread([name] { snap::Snap(name); }).detach();
    }
  g_stats.frames++;
  // forget what everyone holds and no longer needs
  const uint32_t keep = g_frame > 120 ? g_frame - 120 : 0;
  g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(keep, [] {
                 uint32_t m = UINT32_MAX;
                 for (const PeerState& p : g_peers) m = std::min(m, p.ack);
                 return m;
               }())));
  for (PeerState& p : g_peers) {
    p.inputs.erase(p.inputs.begin(), p.inputs.lower_bound(keep));
    p.used.erase(p.used.begin(), p.used.lower_bound(keep));
  }
  g_frame++;
}

}  // namespace

const char* StatusName(Status s) {
  switch (s) {
    case Status::Off: return "off";
    case Status::Connecting: return "connecting";
    case Status::Barrier: return "barrier";
    case Status::InSync: return "in sync";
    case Status::Desynced: return "desynced";
    case Status::Disconnected: return "disconnected";
    case Status::Mismatch: return "another release";
  }
  return "?";
}

void InitFromEnvironment() {
  frame::SetTopHook(&OnTop);
  frame::SetEndHook(&OnEnd);
  if (const char* v = std::getenv("MK9_NET_SNAPS")) {
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
      auto colon = item.find(':');
      if (colon == std::string::npos) continue;
      g_sync_snaps.push_back({uint16_t(std::atoi(item.substr(0, colon).c_str())),
                              uint32_t(std::strtoul(item.c_str() + colon + 1, nullptr, 10))});
    }
  }
  const char* bind = std::getenv("MK9_NET_BIND");
  if (!bind || !*bind) return;
  Settings st;
  st.bind = uint16_t(std::atoi(bind));
  if (const char* v = std::getenv("MK9_NET_SLOT")) st.slot = std::atoi(v);
  if (const char* v = std::getenv("MK9_NET_PLAYERS")) st.players = std::clamp(std::atoi(v), 2, kMaxSlots);
  if (const char* v = std::getenv("MK9_NET_DELAY")) st.delay = std::atoi(v);
  if (const char* v = std::getenv("MK9_NET_MODE")) st.rollback = std::strcmp(v, "rollback") == 0;
  if (const char* v = std::getenv("MK9_NET_LOG")) st.log = v;
  if (const char* v = std::getenv("MK9_NET_LAG")) st.lag_ms = std::atoi(v);
  if (const char* v = std::getenv("MK9_NET_JITTER")) st.jitter_ms = std::atoi(v);
  if (const char* v = std::getenv("MK9_NET_LOSS")) st.loss_percent = std::atoi(v);
  if (const char* v = std::getenv("MK9_NET_PEERS")) {
    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
      auto at = item.find('@');
      if (at == std::string::npos) continue;
      st.peers.push_back({std::atoi(item.substr(0, at).c_str()), item.substr(at + 1)});
    }
  }
  st.loopback_only = std::getenv("MK9_NET_LOOPBACK") != nullptr;
  if (const char* v = std::getenv("MK9_NET_FEED")) st.feed = v;
  st.watch = std::getenv("MK9_NET_WATCH") != nullptr && !st.feed.empty();
  if (const char* v = std::getenv("MK9_NET_MEET_FRAME")) g_meet_frame = std::strtoull(v, nullptr, 10);
  Configure(st);
}

void Configure(const Settings& st) {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  g_pending = st;
  g_pending_set = true;
}

void Disable() {
  std::lock_guard<std::mutex> lock(g_cfg_lock);
  g_pending_off = true;
}

void RequestMeet(uint32_t seed) {
  g_meet_seed = seed;
  g_meet_asked = true;
}

Status GetStatus() { return g_status.load(); }
bool InSync() { return g_status == Status::InSync; }
uint32_t Frame() { return g_frame; }

bool Meet(const std::vector<uint32_t>& words, uint32_t my_seed, Agreement& agreed) {
  ApplyPending();
  if (g_status == Status::Off || (g_sock == INVALID_SOCKET && !g_cfg.watch)) return false;
  StopRolling();
  g_match++;
  g_status = Status::Barrier;
  for (PeerState& p : g_peers) {
    // a READY of this match that came first stays (ready_match, seed, words)
    p.inputs.clear();
    p.used.clear();
    p.contiguous = 0;
    p.confirmed = 0;
    p.ack = 0;
    p.now = 0;
    p.heard = 0;
    p.checks.clear();
    p.left = false;
    p.old_ack = p.old_now = 0;
    p.last_heard = Clock::now();
  }
  g_mine.clear();
  g_my_checks.clear();
  g_tentative.clear();
  g_check_detail.clear();
  g_paced_at = 0;
  Writer w = Header(kReady);
  w.U32(my_seed);
  const uint8_t n = uint8_t(std::min<size_t>(words.size(), kMaxWords));
  w.U8(n);
  for (uint8_t i = 0; i < n; i++) w.U32(words[i]);
  w.U8(uint8_t(OwnDelay()));
  REXLOG_INFO("mk9 net: meeting for match {} ({} words) at frame {}", g_match, n, frame::Count());
  {
    Writer meet = Header(kFeedMeet);
    meet.U32(my_seed);
    meet.U8(n);
    for (uint8_t i = 0; i < n; i++) meet.U32(words[i]);
    meet.U8(uint8_t(OwnDelay()));
    FeedSend(meet);
  }
  // what the feed brought for this match before this spectator got here
  if (auto later = g_feed_later.find(g_match); later != g_feed_later.end()) {
    std::vector<std::string> packets = std::move(later->second);
    g_feed_later.erase(later);
    for (const std::string& pk : packets) HandlePacket(reinterpret_cast<const uint8_t*>(pk.data()), int(pk.size()));
  }
  if (auto later = g_feed_later.find(uint16_t(g_match + 1)); later != g_feed_later.end()) {
    std::vector<std::string> packets;
    for (auto& pk : later->second) {
      if (pk.size() > 4 && uint8_t(pk[4]) == kFeedMeet) HandlePacket(reinterpret_cast<const uint8_t*>(pk.data()), int(pk.size()));
      else packets.push_back(std::move(pk));
    }
    later->second = std::move(packets);
  }
  const auto started = Clock::now();
  auto last_send = Clock::time_point{};
  for (;;) {
    if (!g_cfg.watch && Clock::now() - last_send > std::chrono::milliseconds(50)) {
      SendAll(w);
      // a player still in the match we left plays our part of it as neutral input until it gets here
      for (PeerState& p : g_peers) {
        if (p.ready_match == g_match || !p.old_now) continue;
        Writer in;
        in.U32(kMagic);
        in.U8(kInput);
        in.U8(uint8_t(g_cfg.slot));
        in.U16(uint16_t(g_match - 1));
        in.U32(0);
        in.U32(0);
        in.U32(0);
        const uint32_t first = p.old_ack;
        const uint32_t last = std::max(p.old_now + uint32_t(Delay()) + 8, first);
        const uint8_t count = uint8_t(std::min<uint32_t>(last - first + 1, 120));
        in.U32(first);
        in.U8(count);
        for (uint8_t i = 0; i < count; i++) {
          Wire none;
          in.Bytes(&none, sizeof none);
        }
        Send(p, in);
      }
      last_send = Clock::now();
    }
    Receive();
    bool all = true;
    for (const PeerState& p : g_peers) all = all && p.ready_match == g_match;
    if (all) break;
    if (Clock::now() - started > kBarrierTimeout) {
      REXLOG_WARN("mk9 net: nobody met at match {}", g_match);
      g_status = Status::Disconnected;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // a few more READYs so a peer that lost ours leaves the barrier too
  if (!g_cfg.watch)
    for (int i = 0; i < 3; i++) SendAll(w);
  agreed = Agreement{};
  if (!g_cfg.watch) agreed.words[g_cfg.slot] = std::vector<uint32_t>(words.begin(), words.begin() + n);
  agreed.seed = g_cfg.slot == 0 && !g_cfg.watch ? my_seed : 0;
  for (const PeerState& p : g_peers) {
    agreed.words[p.slot] = p.words;
    if (p.slot == 0) agreed.seed = p.seed;
  }
  // the largest input delay any side asked for
  g_match_delay = g_cfg.watch ? 0 : OwnDelay();  // a spectator plays the players' delay
  for (const PeerState& p : g_peers) g_match_delay = std::max(g_match_delay, p.delay);
  // the first `delay` frames run on neutral inputs for everyone
  g_frame = 0;
  for (int f = 0; f < Delay(); f++) {
    g_mine[uint32_t(f)] = Wire{};
    FeedOwn(uint32_t(f), Wire{});
  }
  for (PeerState& p : g_peers)
    for (int f = 0; f < Delay(); f++) p.inputs[uint32_t(f)] = Wire{};
  for (PeerState& p : g_peers) p.contiguous = uint32_t(Delay());
  g_status = Status::InSync;
  input::SetSource(&FrameInput);
  REXLOG_INFO("mk9 net: match {} in sync, seed {:08X}, input delay {}", g_match, agreed.seed, Delay());
  return true;
}

bool FrameInput(uint32_t user, input::Pad& out) {
  if (g_status != Status::InSync) return false;
  if (int(user) >= g_cfg.players) {
    out = input::Pad{};  // not in the match: no controller
    return true;
  }
  out = Unpack(g_now[user]);
  // a new packet every frame, numbered by the frame: the same when a rollback runs it again
  out.packet = (uint32_t(g_match) << 24) + g_frame + 1;
  return true;
}

void Check(uint32_t frame, uint32_t hash) {
  if (g_status != Status::InSync) return;
  g_my_checks[frame] = hash;
  Writer w = Header(kCheck);
  w.U32(frame);
  w.U32(hash);
  if (!g_cfg.watch) SendAll(w);
  Writer fc = Header(kFeedCheck);
  fc.U32(frame);
  fc.U32(hash);
  FeedSend(fc);
}

Stats GetStats() { return g_stats; }

}  // namespace mk9::net
