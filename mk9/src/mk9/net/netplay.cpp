// Netplay core, see netplay.h.
//
// Packets (little endian): u32 magic "MK9N", u8 type, u8 slot, u16 match (barrier number), then
//   1 HELLO  -                                  presence while connecting (and between matches)
//   2 READY  u32 seed, u8 n, n x u32            "my match is about to be set up": the barrier ends
//                                               when every slot's READY of this match arrived
//   3 INPUT  u32 ack, u32 now, u32 first, u8 n, n x 8 bytes
//            the sender's own inputs for frames first..first+n-1; ack: how many of the receiver's
//            inputs the sender holds without a gap; now: the frame the sender runs
//   4 CHECK  u32 frame, u32 hash                the sender's state hash at the top of that frame
//   5 BYE    u32 frame                          the sender left the match

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

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/testinput.h"

#pragma comment(lib, "ws2_32.lib")

namespace mk9::net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x4E394B4D;  // "MK9N"
enum : uint8_t { kHello = 1, kReady = 2, kInput = 3, kCheck = 4, kBye = 5 };
constexpr int kWindow = 24;               // inputs repeated in every INPUT packet
constexpr int kMaxWords = 64;
constexpr auto kPeerTimeout = std::chrono::seconds(10);
constexpr auto kBarrierTimeout = std::chrono::seconds(60);

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
  bool ready = false;               // their READY for the barrier now
  uint32_t seed = 0;
  std::vector<uint32_t> words;
  std::map<uint32_t, uint32_t> checks;
  bool left = false;
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

int Delay() { return g_cfg.delay >= 0 ? g_cfg.delay : (g_cfg.rollback ? 1 : 2); }

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
  w.U32(first);
  w.U8(uint8_t(last - first + 1));
  for (uint32_t f = first; f <= last; f++) {
    auto it = g_mine.find(f);
    Wire in = it != g_mine.end() ? it->second : Wire{};
    w.Bytes(&in, sizeof in);
  }
  Send(p, w);
}

void Receive() {
  Pump();
  if (g_sock == INVALID_SOCKET) return;
  uint8_t b[1500];
  for (;;) {
    sockaddr_in from{};
    int fl = sizeof from;
    int n = recvfrom(g_sock, reinterpret_cast<char*>(b), sizeof b, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 8) return;
    uint32_t magic;
    std::memcpy(&magic, b, 4);
    if (magic != kMagic) continue;
    const uint8_t type = b[4];
    const int slot = b[5];
    uint16_t match;
    std::memcpy(&match, b + 6, 2);
    PeerState* p = PeerBySlot(slot);
    if (!p) continue;
    p->seen = true;
    p->last_heard = Clock::now();
    auto u32 = [&](int o) {
      uint32_t v;
      std::memcpy(&v, b + o, 4);
      return v;
    };
    if (type == kReady && n >= 13 && match == g_match) {
      p->ready = true;
      p->seed = u32(8);
      p->words.clear();
      for (int i = 0; i < b[12] && 13 + i * 4 + 4 <= n; i++) p->words.push_back(u32(13 + i * 4));
    } else if (type == kInput && n >= 21) {
      // a peer already in the match we are still meeting for: its READY was lost
      if (g_status == Status::Barrier && match == uint16_t(g_match)) p->ready = p->ready || !p->words.empty();
      if (match != g_match) continue;
      p->ack = std::max(p->ack, u32(8));
      p->now = std::max(p->now, u32(12));
      const uint32_t first = u32(16);
      const int count = b[20];
      for (int i = 0; i < count && 21 + i * 8 + 8 <= n; i++) {
        Wire in;
        std::memcpy(&in, b + 21 + i * 8, 8);
        p->inputs.emplace(first + uint32_t(i), in);
      }
      while (p->inputs.count(p->contiguous)) p->contiguous++;
    } else if (type == kCheck && n >= 16 && match == g_match) {
      p->checks[u32(8)] = u32(12);
    } else if (type == kBye && match == g_match) {
      p->left = true;
    }
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
      SendAll(w);
    }
    CloseSocket();
    g_status = Status::Off;
    input::SetSource(nullptr);
  }
  if (g_pending_set) {
    g_pending_set = false;
    Open(g_pending);
  }
}

void LogFrame() {
  if (!g_log) return;
  std::fprintf(g_log, "%u", g_frame);
  for (int s = 0; s < g_cfg.players; s++)
    std::fprintf(g_log, " %04X:%02X%02X:%02X%02X%02X%02X", g_now[s].buttons, g_now[s].lt, g_now[s].rt,
                 uint8_t(g_now[s].lx), uint8_t(g_now[s].ly), uint8_t(g_now[s].rx), uint8_t(g_now[s].ry));
  auto it = g_my_checks.find(g_frame);
  if (it != g_my_checks.end()) std::fprintf(g_log, " check %08X", it->second);
  std::fputc('\n', g_log);
}

// lockstep: every other player's input for frame f is here (false: they are gone)
bool WaitForInputs(uint32_t f) {
  auto started = Clock::now();
  bool waited = false;
  for (;;) {
    bool all = true;
    for (PeerState& p : g_peers) {
      if (p.inputs.count(f)) continue;
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
    if (Clock::now() - started > std::chrono::milliseconds(8)) {
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

uint64_t g_meet_frame = 0;  // tests: meet at the top of this frame (counted from boot)

void OnTop(PPCContext& ctx, uint8_t* base) {
  ApplyPending();
  if (g_status == Status::Off) return;
  Receive();
  if (g_meet_frame && frame::Count() == g_meet_frame && g_status == Status::Connecting) {
    Agreement agreed;
    Meet(game::ReadyWords(), 0x1234567u + uint32_t(g_cfg.slot), agreed);
    if (g_status == Status::InSync) game::Agree(ctx, base, agreed);
  }
  if (g_status == Status::Connecting || (g_status != Status::InSync && g_status != Status::Barrier)) {
    if (Clock::now() - g_last_hello > std::chrono::milliseconds(250)) {
      SendAll(Header(kHello));
      g_last_hello = Clock::now();
    }
    return;
  }
  if (g_status != Status::InSync) return;
  // this frame's local input waits `delay` frames (a test's script for this slot stands in for the pad)
  input::Pad local;
  if (!testinput::ScriptedPad(uint32_t(g_cfg.slot), local)) local = input::ReadPhysical(0);
  g_mine[g_frame + uint32_t(Delay())] = Pack(local);
  for (PeerState& p : g_peers) SendInputs(p);
  if (!WaitForInputs(g_frame)) {
    g_status = Status::Disconnected;
    input::SetSource(nullptr);
    return;
  }
  g_now[g_cfg.slot] = g_mine[g_frame];
  for (PeerState& p : g_peers) g_now[p.slot] = p.inputs[g_frame];
  // every 30 frames both sides compare what the game says its state is
  if (g_frame % 30 == 0) Check(g_frame, game::StateHash(ctx, base));
  CompareChecks();
}

void OnEnd(PPCContext&, uint8_t*) {
  if (g_status != Status::InSync) return;
  LogFrame();
  g_stats.frames++;
  // forget what everyone holds and no longer needs
  const uint32_t keep = g_frame > 120 ? g_frame - 120 : 0;
  g_mine.erase(g_mine.begin(), g_mine.lower_bound(std::min(keep, [] {
                 uint32_t m = UINT32_MAX;
                 for (const PeerState& p : g_peers) m = std::min(m, p.ack);
                 return m;
               }())));
  for (PeerState& p : g_peers) p.inputs.erase(p.inputs.begin(), p.inputs.lower_bound(keep));
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
  }
  return "?";
}

void InitFromEnvironment() {
  frame::SetTopHook(&OnTop);
  frame::SetEndHook(&OnEnd);
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

Status GetStatus() { return g_status.load(); }
bool InSync() { return g_status == Status::InSync; }
uint32_t Frame() { return g_frame; }

bool Meet(const std::vector<uint32_t>& words, uint32_t my_seed, Agreement& agreed) {
  ApplyPending();
  if (g_status == Status::Off || g_sock == INVALID_SOCKET) return false;
  g_match++;
  g_status = Status::Barrier;
  for (PeerState& p : g_peers) {
    p.ready = false;
    p.words.clear();
    p.inputs.clear();
    p.contiguous = 0;
    p.ack = 0;
    p.now = 0;
    p.checks.clear();
    p.left = false;
    p.last_heard = Clock::now();
  }
  g_mine.clear();
  g_my_checks.clear();
  Writer w = Header(kReady);
  w.U32(my_seed);
  const uint8_t n = uint8_t(std::min<size_t>(words.size(), kMaxWords));
  w.U8(n);
  for (uint8_t i = 0; i < n; i++) w.U32(words[i]);
  REXLOG_INFO("mk9 net: meeting for match {} ({} words)", g_match, n);
  const auto started = Clock::now();
  auto last_send = Clock::time_point{};
  for (;;) {
    if (Clock::now() - last_send > std::chrono::milliseconds(50)) {
      SendAll(w);
      last_send = Clock::now();
    }
    Receive();
    bool all = true;
    for (const PeerState& p : g_peers) all = all && p.ready;
    if (all) break;
    if (Clock::now() - started > kBarrierTimeout) {
      REXLOG_WARN("mk9 net: nobody met at match {}", g_match);
      g_status = Status::Disconnected;
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  // a few more READYs so a peer that lost ours leaves the barrier too
  for (int i = 0; i < 3; i++) SendAll(w);
  agreed = Agreement{};
  agreed.words[g_cfg.slot] = std::vector<uint32_t>(words.begin(), words.begin() + n);
  agreed.seed = g_cfg.slot == 0 ? my_seed : 0;
  for (const PeerState& p : g_peers) {
    agreed.words[p.slot] = p.words;
    if (p.slot == 0) agreed.seed = p.seed;
  }
  // the first `delay` frames run on neutral inputs for everyone
  g_frame = 0;
  for (int f = 0; f < Delay(); f++) g_mine[uint32_t(f)] = Wire{};
  for (PeerState& p : g_peers)
    for (int f = 0; f < Delay(); f++) p.inputs[uint32_t(f)] = Wire{};
  for (PeerState& p : g_peers) p.contiguous = uint32_t(Delay());
  g_status = Status::InSync;
  input::SetSource(&FrameInput);
  REXLOG_INFO("mk9 net: match {} in sync, seed {:08X}", g_match, agreed.seed);
  return true;
}

bool FrameInput(uint32_t user, input::Pad& out) {
  if (g_status != Status::InSync) return false;
  if (int(user) >= g_cfg.players) {
    out = input::Pad{};  // not in the match: no controller
    return true;
  }
  out = Unpack(g_now[user]);
  return true;
}

void Check(uint32_t frame, uint32_t hash) {
  if (g_status != Status::InSync) return;
  g_my_checks[frame] = hash;
  Writer w = Header(kCheck);
  w.U32(frame);
  w.U32(hash);
  SendAll(w);
}

Stats GetStats() { return g_stats; }

}  // namespace mk9::net
