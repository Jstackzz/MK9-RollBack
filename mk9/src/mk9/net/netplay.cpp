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
//   4 CHECK  u32 frame, u32 fight, u32 draws    the sender's state check at the top of that frame (game::StateCheck)
//   5 BYE    u32 frame                          the sender left the match
//
// The spectator feed (TCP to the launcher on this PC, each packet after its u16 length; the header's
// slot is the sender's): a player's game writes
//   6 MEET   u32 seed, u8 n, n x u32            its READY at a barrier
//   7 OWN    u32 first, u8 n, n x 8 bytes       its own inputs, frames first..first+n-1
//   8 CHECK  u32 frame, u32 fight, u32 draws    its checks
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
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "mk9/autopilot.h"
#include "mk9/crash.h"
#include "mk9/engine.h"
#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/net/rollback.h"
#include "mk9/snap.h"
#include "mk9/testinput.h"

#include <realtimeapiset.h>

#include <rex/ui/presenter.h>

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
// rollback: the first frames of a fight take no input on any PC, so no rollback goes back into them. There the game
// sets the fight's HUD up in its UI library (Scaleform GFx, long taken for FMOD) and readies its sounds, and every
// rollback to the fight's first frame crashed in that library (docs/GAME.md, "Known gaps"). Half a second, inside the
// fight's intro
constexpr uint32_t kQuietFrames = 30;
// rollback: how much longer a restore waits for a hold that timed out (engine::WaitHeld) before the guesses wait for a
// later frame's top; after such a wait in vain, how much the next one waits
constexpr int kRestoreHoldMs = 1500, kRestoreHoldShortMs = 100;
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
  // a READY of the next match that came while this match's barrier still waited (a spectator's feed brings a match
  // under way all at once, the next match's MEET with this one's): theirs once this barrier is over
  uint16_t next_match = 0;
  uint32_t next_seed = 0;
  std::vector<uint32_t> next_words;
  int next_delay = -1;
  uint32_t old_ack = 0, old_now = 0;  // they are still in the match before ours: what they need of us
  std::map<uint32_t, game::StateCheck> checks;
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
std::map<uint32_t, game::StateCheck> g_my_checks;
std::deque<Outgoing> g_outbox;
std::mutex g_outbox_mutex;  // the simulated network's queue: the game's threads and its pump thread
std::mt19937 g_rng{12345};
Stats g_stats;
FILE* g_log = nullptr;
Clock::time_point g_last_hello{};
bool g_rolling = false;                    // rollback armed: frames may run on guesses
bool g_correction_waits = false;           // a wrong guess found whose restore waits for a later frame's top
uint32_t g_restores_waited = 0;            // how often that happened
uint32_t g_hold_fails = 0;                 // restores in a row whose hold was not complete in time
std::map<uint32_t, game::StateCheck> g_tentative;  // state checks at frame tops, until every input before is known
// what each check frame's hash was made of (the log's "timer", "round", "rand" and "hp"), as of its last run
struct CheckDetail {
  uint32_t timer = 0, rand = 0;
  int round = 0, won1 = 0, won2 = 0;
  game::FightBars bars;
};
std::map<uint32_t, CheckDetail> g_check_detail;
// Pace(): how far ahead of the others this PC ran lately, and the frames it held back
struct Pacing {
  std::deque<int> samples;     // the last frames' mine - theirs (Pace), twice the frames ahead
  double round_trip = -1;      // mine + theirs, slowly smoothed: the round trip in this PC's frames
  std::deque<uint32_t> holds;  // the frames held back lately, for as long as they have not all shown
  // at our last frame tops: when, the other's frame as heard and the periods this PC had stood still so far (held
  // back or waiting at the prediction limit); from them the frames the other ran a 60 Hz period lately and the frames
  // this PC ran a period it did not stand still (Pace), and the part of a frame still to hold back for the other
  struct Top {
    std::chrono::steady_clock::time_point when;
    uint32_t theirs;
    double paused;
  };
  std::deque<Top> tops;
  double paused = 0, their_rate = 1.0, my_rate = 1.0, owed = 0;
  bool keeping = false;  // holding back to the other's pace, until no longer ahead
  uint32_t streak = 0;   // the frames in a row this PC has been ahead by the hold's threshold or more
  uint32_t last_hold = 0;  // the frame of the last lead hold (holds is emptied sooner), when had_hold
  bool had_hold = false;
  // for the log every 10 s: frames held back, the sum and count of the averages, the frames that waited at the
  // prediction limit (kMaxPredict) and how long
  uint32_t held = 0, ahead_n = 0, limit_waits = 0;
  double ahead_sum = 0, limit_ms = 0;
};
Pacing g_pace;

// One of the launcher's matches on this PC (ResetMatchStats, MatchStatsJson): its fight frames, the frames that ran
// while rollback was armed (in lockstep: while the game's phase was the fight). A frame's length is from its top to the
// next one's, what the player saw it for, with its frames run again, its wait at the prediction limit and its holds in
// it; the frame rollback was armed in (its Arm) does not count. Over 25 ms a 60 Hz screen showed the frame before again
constexpr double kLateMs = 25.0;
constexpr double kStallMs = 50.0;     // shown three times or more
constexpr size_t kStatSeconds = 900;  // the lines a second kept: 15 minutes
struct MatchStats {
  Clock::time_point start{}, last_top{};  // the match's first frame in sync; the top of the frame that runs now
  uint32_t frames = 0, late = 0, stalls = 0;
  double fight_ms = 0, longest_ms = 0;
  uint32_t rollbacks = 0, rerun = 0, depth[4] = {};  // how many went back 1, 2, 3-4, 5 or more frames
  double rollback_ms = 0, rollback_longest_ms = 0;
  uint32_t limit_waits = 0, held = 0;
  double limit_ms = 0;
  // the late frames by what made them long (StatTop): a hold the cap slept for, a wait at the prediction limit, a
  // rollback, the main thread's own work (its CPU cycles well over a normal frame's), and the rest: the main thread
  // blocked on something (the rendering thread, the GPU, a lock, a file, another process taking its core)
  uint32_t late_why[5] = {};
  // the gaps between successive frames' presents over 25 ms and over 50 ms, in fight frames: what the screen got
  // (the SDK's Presenter::ShownGaps), whatever the frames' own tops did
  uint32_t shown_late = 0, shown_stalls = 0;
  // where the main thread stood in late frames (crash::TakeStallSamples), summed over the match: the host code and the
  // guest return addresses, with their counts
  std::map<std::string, uint32_t> stall_where;
  uint32_t stall_samples = 0;
  // how many frames the game's frame tops are ahead of the frames the screen got, from the first fight frame's: a PC that
  // makes up a late frame by running the next ones close together fills its frame pipeline, and the screen shows each
  // frame that much later. Sum over `lag_n` fight frames (tests: MK9_TRACE_LAG)
  double lag_sum = 0;
  uint32_t lag_n = 0;
  int64_t lag_base = INT64_MIN;
  double their_rate = 0, my_rate = 0, round_trip = 0;  // sums of Pace's, over rates_n and trips_n frames
  uint32_t rates_n = 0, trips_n = 0;
  // every second from the start: fight frames, the late ones among them, frames run again, the deepest rollback, ms
  // waited at the prediction limit, frames held back
  struct Second {
    uint16_t frames = 0, late = 0, rerun = 0, deepest = 0, wait_ms = 0, held = 0;
  };
  std::vector<Second> seconds;
};
MatchStats g_ms;

// what the frame that runs now has spent on things that make a frame long, read and cleared at the next top (StatTop)
struct FrameParts {
  uint64_t shown_over_25 = 0, shown_over_50 = 0;  // the presenter's counts at the last top
  double rollback_ms = 0, wait_ms = 0;
  uint64_t cycles = 0;      // the main thread's cycle count at the last top
  double cycle_ema = 0;     // the cycles a normal frame takes it, slowly smoothed
};
FrameParts g_fp;

void Add(uint16_t& to, uint32_t n) { to = uint16_t(std::min<uint32_t>(0xFFFF, to + n)); }

// this second's line (none before the match's first frame in sync, or past kStatSeconds)
MatchStats::Second* StatSecond() {
  if (g_ms.start == Clock::time_point{}) return nullptr;
  const auto i = size_t(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - g_ms.start).count());
  if (i >= kStatSeconds) return nullptr;
  if (g_ms.seconds.size() <= i) g_ms.seconds.resize(i + 1);
  return &g_ms.seconds[i];
}

uint32_t g_barrier_id = 0;  // the id the game's next object took at the last barrier (the desync line has it)
uint32_t g_draws_apart = 0;  // checks of this match whose fight was the same and whose draws were not

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

void StartPumpThread();

void Send(const PeerState& p, const Writer& w) {
  if (g_cfg.loss_percent && int(g_rng() % 100) < g_cfg.loss_percent) return;
  if (!g_cfg.lag_ms && !g_cfg.jitter_ms) return SendNow(p.addr, w.b, w.n);
  int extra = g_cfg.jitter_ms ? int(g_rng() % uint32_t(g_cfg.jitter_ms + 1)) : 0;
  StartPumpThread();
  std::lock_guard<std::mutex> lock(g_outbox_mutex);
  g_outbox.push_back({Clock::now() + std::chrono::milliseconds(g_cfg.lag_ms + extra), p.addr,
                      std::vector<uint8_t>(w.b, w.b + w.n)});
}

void SendAll(const Writer& w) {
  for (const PeerState& p : g_peers) Send(p, w);
}

void Pump() {
  const auto now = Clock::now();
  std::lock_guard<std::mutex> lock(g_outbox_mutex);
  // the simulated network delivers in send order per destination once due
  while (!g_outbox.empty() && g_outbox.front().when <= now) {
    const Outgoing& o = g_outbox.front();
    SendNow(o.to, o.data.data(), int(o.data.size()));
    g_outbox.pop_front();
  }
}

// A real network delivers a packet when it is due, not when its sender next looks at a clock: a thread of the simulated
// one (tests) pumps every millisecond. Until 2026-11-12 only the sender's frame tops did, which added up to a frame to
// the lag, and more after frames that came close together.
void StartPumpThread() {
  static const bool started = [] {
    std::thread([] {
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        Pump();
      }
    }).detach();
    return true;
  }();
  (void)started;
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
    if (match == p->ready_match || match == p->next_match) return;
    std::vector<uint32_t> words;
    for (int i = 0; i < b[12] && 13 + i * 4 + 4 <= n; i++) words.push_back(u32(13 + i * 4));
    const int after = 13 + b[12] * 4;
    const int delay = after < n ? b[after] : -1;
    if (g_status == Status::Barrier && match != g_match) {
      // the next match's while this barrier waits: the barrier needs this match's READY (its seed, its words), so
      // the next one's waits for its end (taken now, it left a spectator handed a match under way at this barrier)
      p->next_match = match;
      p->next_seed = u32(8);
      p->next_words = std::move(words);
      p->next_delay = delay;
      return;
    }
    p->ready_match = match;
    p->seed = u32(8);
    p->words = std::move(words);
    p->delay = delay;
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
  } else if ((type == kCheck || type == kFeedCheck) && n >= 20 && match == g_match && u32(8) <= Horizon()) {
    p->checks[u32(8)] = {u32(12), u32(16)};
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
    CheckDetail d;
    if (auto found = g_check_detail.find(g_frame); found != g_check_detail.end()) {
      d = found->second;
    } else {
      game::LastCheckDetail(d.timer, d.rand);
    }
    // the round and the rounds won are in the hash too (in a fight): a desync's report shows them beside the timer,
    // and what the fighters had left
    const game::StateCheck c = it != g_my_checks.end() ? it->second : t->second;
    std::fprintf(g_log, " check %08X timer %u round %d won %d-%d rand %08X draws %08X hp %.4f %.4f meter %.4f %.4f", c.fight,
                 d.timer, d.round, d.won1, d.won2, d.rand, c.draws, d.bars.health[0], d.bars.health[1], d.bars.meter[0],
                 d.bars.meter[1]);
  }
  std::fputc('\n', g_log);
}

std::atomic<bool> g_waiting{false};  // WaitForInputs is waiting (Waiting)

// every other player's inputs up to frame f are here (false: someone is gone)
bool WaitForInputs(uint32_t f) {
  struct Waits {
    Waits() { g_waiting = true; }
    ~Waits() { g_waiting = false; }
  } waits;
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
      // what this check counted, for the logs and a desync's report: the other player's shows what drew differently
      const auto draws_lines = [&] {
        for (const std::string& line : game::CheckDraws(it->first))
          REXLOG_INFO("mk9 net: draws at frame {} by site: {}", it->first, line);
        // what this game's loading made before the fight: with the other player's report, where two PCs' object lists part
        if (const std::string objects = game::LastBarrierObjects(); !objects.empty())
          REXLOG_INFO("mk9 net: {}", objects);
      };
      if (mine->second.fight != it->second.fight) {
        // with the id the next object took at the barrier: the two players' reports show whether their loading made a
        // different number of objects before the fight (a script process's draws are told apart by its id from there)
        REXLOG_ERROR("mk9 net: DESYNC at frame {} with slot {}: {:08X} here, {:08X} there (next object id at the "
                     "barrier {:08X}, now {:08X})",
                     it->first, p.slot, mine->second.fight, it->second.fight, g_barrier_id, game::NextObjectId());
        if (g_status != Status::Desynced) {
          if (auto d = g_check_detail.find(it->first); d != g_check_detail.end())
            REXLOG_INFO("mk9 net: the fight here at frame {}: timer {} round {} health {:.4f} {:.4f} meter {:.4f} {:.4f}, "
                        "draws {:08X} here, {:08X} there",
                        it->first, d->second.timer, d->second.round, d->second.bars.health[0], d->second.bars.health[1],
                        d->second.bars.meter[0], d->second.bars.meter[1], mine->second.draws, it->second.draws);
          draws_lines();
        }
        // tests may play on to see how far apart the games get
        static const bool keep_going = std::getenv("MK9_NET_DESYNC_CONTINUE") != nullptr;
        if (!keep_going) g_status = Status::Desynced;
      } else if (mine->second.draws != it->second.draws && !g_draws_apart++) {
        // The timer and what the fighters have left are the same but a script drew differently: nothing the players see
        // of the fight (a script that only dresses the arena can). Until 0.5.6 this ended the match as a desync, and
        // both of 0.5.5's desyncs in round 2 differed in nothing else that check had (one process's draws). A fight that
        // does go differently shows in the fight's part at its next hit. Told once, with the counts; StopRolling says
        // how many checks there were
        REXLOG_WARN("mk9 net: scripts drew differently from slot {} at frame {}: {:08X} here, {:08X} there; the fight is "
                    "the same (next object id at the barrier {:08X}, now {:08X})",
                    p.slot, it->first, mine->second.draws, it->second.draws, g_barrier_id, game::NextObjectId());
        draws_lines();
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

// frame f's inputs, what the game's pad reads get until the next frame's top: GuardedMain reads the pads
// after the top of a frame and before its tick (mk9/frame.h), so f plays on them
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
    CheckDetail d;
    game::LastCheckDetail(d.timer, d.rand);
    d.round = game::Round();
    d.won1 = game::RoundsWon(0);
    d.won2 = game::RoundsWon(1);
    d.bars = game::FightNow();
    g_check_detail[f] = d;
    while (g_check_detail.size() > 64) g_check_detail.erase(g_check_detail.begin());
  }
}

void SettleChecks() {
  uint32_t settled = g_frame;
  for (const PeerState& p : g_peers) settled = std::min(settled, p.confirmed);
  while (!g_tentative.empty() && g_tentative.begin()->first <= settled) {
    Check(g_tentative.begin()->first, g_tentative.begin()->second.fight, g_tentative.begin()->second.draws);
    g_tentative.erase(g_tentative.begin());
  }
}

bool StartRolling(PPCContext& ctx) {
  engine::Hold();
  engine::ExcludeFromRollback();
  if (!rb::Arm(ctx, g_frame)) return false;
  rb::OpenFrame(g_frame, ctx);  // the frame Arm keeps first: what goes back with it is kept too (key presses)
  g_rolling = true;
  // lockstep kept the sides even until now: what Pace saw before it is past
  g_pace.samples.clear();
  g_pace.holds.clear();
  g_pace.tops.clear();
  g_pace.owed = 0;
  g_pace.keeping = false;
  for (PeerState& p : g_peers) p.confirmed = g_frame;
  g_ms.last_top = {};  // this frame, Arm's, is not counted (MatchStats)
  REXLOG_INFO("mk9 net: rollback from frame {} of match {}", g_frame, g_match);
  return true;
}

// What rollbacks cost, for the log every 10 s of a fight that rolls back (and at its end): putting the memory
// back, and in the frames run again the game's own frame, the wait for the rendering thread to have drawn it
// (engine::Hold), and the pages' undo logs
struct ReplayTimes {
  uint32_t rollbacks = 0, frames = 0;
  double restore_ms = 0, run_ms = 0, render_ms = 0, logs_ms = 0, longest_ms = 0;
};
ReplayTimes g_replay;
Clock::time_point g_replay_since{};

// the GPU told that frames run again are not shown (SetDrawSkip); MK9_RB_DRAW_ALL has them drawn (comparisons)
std::function<void(bool)> g_draw_skip;
void SkipDraws(bool skip) {
  static const bool draw_all = std::getenv("MK9_RB_DRAW_ALL") != nullptr;
  if (g_draw_skip && !draw_all) g_draw_skip(skip);
}

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// What the stall sampler saw since the last look (crash::TakeStallSamples): into the match's totals and the log, every
// 10 s of a match and when the stats are made
void CollectStalls(bool now) {
  static Clock::time_point since{};
  const auto t = Clock::now();
  if (since == Clock::time_point{}) since = t;
  if (!now && t - since < std::chrono::seconds(10)) return;
  since = t;
  uint32_t total = 0;
  const auto seen = crash::TakeStallSamples(40, &total);
  if (!total) return;
  g_ms.stall_samples += total;
  for (const auto& [n, text] : seen)
    if (g_ms.stall_where.size() < 120 || g_ms.stall_where.count(text)) g_ms.stall_where[text] += n;
  std::string where;
  for (size_t i = 0; i < seen.size() && i < 3; i++)
    where += fmt::format("{}{} % {}", i ? "; " : "", 100 * seen[i].first / total, seen[i].second);
  REXLOG_INFO("mk9 net: where the main thread stood in late frames ({} looks): {}", total, where);
}

constexpr uint32_t kCatchUpFrom = 180;  // the fight's frame the cap starts making up late frames from (frame.cpp)

// the top of a frame in a match in sync: the frame before it counts if it was a fight frame (MatchStats)
void StatTop() {
  const auto t = Clock::now();
  if (g_ms.start == Clock::time_point{}) g_ms.start = t;
  const bool fight = g_rolling || (!g_cfg.rollback && game::Phase() == game::kPhaseFight);
  crash::StallSampling(fight);
  // loading, menus and a fight's first seconds (both PCs come late alike) are not made up for
  if (!fight || g_frame < kCatchUpFrom) frame::DropCatchUp();
  if (fight) CollectStalls(false);
  if (fight && g_ms.last_top != Clock::time_point{}) {
    const double ms = Ms(t - g_ms.last_top);
    g_ms.frames++;
    g_ms.fight_ms += ms;
    g_ms.longest_ms = std::max(g_ms.longest_ms, ms);
    if (ms > kLateMs) g_ms.late++;
    if (ms > kStallMs) g_ms.stalls++;
    if (MatchStats::Second* s = StatSecond()) {
      Add(s->frames, 1);
      if (ms > kLateMs) Add(s->late, 1);
    }
    // what a late frame was spent on: the cycles this thread ran in it against a normal frame's
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    const double ran = g_fp.cycles ? double(cycles - g_fp.cycles) : 0.0;
    if (ms > kLateMs) {
      const frame::CapInfo cap = frame::LastCap();
      const int why = cap.hold > 0                                   ? 0
                      : g_fp.wait_ms >= 4.0                          ? 1
                      : g_fp.rollback_ms >= 4.0                      ? 2
                      : g_fp.cycle_ema > 0 && ran > 1.5 * g_fp.cycle_ema ? 3
                                                                     : 4;
      g_ms.late_why[why]++;
    } else if (ran > 0) {
      g_fp.cycle_ema = g_fp.cycle_ema > 0 ? g_fp.cycle_ema * 0.97 + ran * 0.03 : ran;
    }
    g_fp.cycles = cycles;
  }
  {
    uint64_t gaps = 0, over25 = 0, over50 = 0;
    double longest = 0;
    rex::ui::Presenter::ShownGaps(gaps, over25, over50, longest);
    if (fight && g_ms.last_top != Clock::time_point{}) {
      g_ms.shown_late += uint32_t(over25 - g_fp.shown_over_25);
      g_ms.shown_stalls += uint32_t(over50 - g_fp.shown_over_50);
      const int64_t ahead = int64_t(g_ms.frames) - int64_t(gaps);
      if (g_ms.lag_base == INT64_MIN) g_ms.lag_base = ahead;
      g_ms.lag_sum += double(ahead - g_ms.lag_base);
      g_ms.lag_n++;
    }
    g_fp.shown_over_25 = over25;
    g_fp.shown_over_50 = over50;
  }
  g_fp.rollback_ms = g_fp.wait_ms = 0;
  g_ms.last_top = t;
}

void LogReplayTimes(bool now) {
  const auto t = Clock::now();
  static rb::Stats last_rb{};
  static uint32_t last_frames = 0;
  if (g_replay_since == Clock::time_point{}) {
    g_replay_since = t;
    last_rb = rb::GetStats();
    last_frames = g_stats.frames;
  }
  if (!now && t - g_replay_since < std::chrono::seconds(10)) return;
  const ReplayTimes& r = g_replay;
  if (r.rollbacks)
    REXLOG_INFO("mk9 net: {} rollbacks in {:.0f} s, {} frames run again ({:.1f} ms a frame): putting memory back {:.1f} "
                "ms, the game's frames {:.1f} ms, waiting for the rendering thread {:.1f} ms, page logs {:.1f} ms; "
                "longest rollback {:.1f} ms",
                r.rollbacks, Ms(t - g_replay_since) / 1000.0, r.frames,
                r.frames ? (r.run_ms + r.render_ms + r.logs_ms) / r.frames : 0.0, r.restore_ms, r.run_ms, r.render_ms,
                r.logs_ms, r.longest_ms);
  if (const std::string where = engine::TakeRenderWaitSamples(4); !where.empty())
    REXLOG_INFO("mk9 net: the rendering thread while frames run again waited for it: {}", where);
  // how even the sides were (Pace)
  if (g_pace.ahead_n)
    REXLOG_INFO("mk9 net: {:.2f} frames ahead of the others on average (a round trip {:.1f} frames, the others at {:.0f} "
                "frames a second, this PC at {:.0f} when not held back), {} frames held back, {} waited for their "
                "inputs at the {}-frame limit ({:.0f} ms)",
                g_pace.ahead_sum / g_pace.ahead_n / 2.0, g_pace.round_trip, g_pace.their_rate * 60.0,
                g_pace.my_rate * 60.0, g_pace.held, g_pace.limit_waits, kMaxPredict, g_pace.limit_ms);
  g_pace.held = g_pace.ahead_n = g_pace.limit_waits = 0;
  g_pace.ahead_sum = g_pace.limit_ms = 0;
  // what keeping the frames costs every frame, rolled back or not: the pages first written (each a fault)
  const rb::Stats s = rb::GetStats();
  const uint32_t frames = g_stats.frames - last_frames;
  if (frames)
    REXLOG_INFO("mk9 net: frames kept for rollback: {:.0f} pages first written a frame ({:.0f} by the main thread, {:.0f} "
                "the rendering thread, {:.0f} other simulation threads, {:.0f} others; {:.2f} ms in the fault handler, "
                "{:.1f} MB logged, {:.0f} pages taken ahead with their block); the last frame {} pages, {} in the "
                "simulation heap, in {} 64 KB blocks, {} blocks taken whole; close {:.2f} ms",
                double(s.faults - last_rb.faults) / frames, double(s.faults_main - last_rb.faults_main) / frames,
                double(s.faults_render - last_rb.faults_render) / frames, double(s.faults_sim - last_rb.faults_sim) / frames,
                double(s.faults_other - last_rb.faults_other) / frames,
                double(s.fault_us - last_rb.fault_us) / 1000.0 / frames,
                double(s.logged_bytes - last_rb.logged_bytes) / frames / (1024.0 * 1024.0),
                double(s.pages_ahead - last_rb.pages_ahead) / frames, s.last_pages, s.last_region_pages, s.last_blocks,
                s.dense_blocks, s.close_ms);
  last_rb = s;
  last_frames = g_stats.frames;
  g_replay = {};
  g_replay_since = t;
}

void StopRolling() {
  if (!g_rolling) return;
  rb::Disarm();
  g_rolling = false;
  LogReplayTimes(true);
  REXLOG_INFO("mk9 net: lockstep from frame {} of match {} ({} rollbacks so far, longest {} frames; {} waited for a "
              "later frame's top; {} checks with the same fight and other draws)",
              g_frame, g_match, g_stats.rollbacks, g_stats.max_rollback, g_restores_waited, g_draws_apart);
}

void Lost(Status s) {
  StopRolling();
  g_status = s;
  input::SetSource(nullptr);
}

// A frame's undo log is closed with every simulation thread stopped. What a thread still running writes after the close
// goes to the pending log, and a restore that comes next puts back only pages some kept frame also wrote: the rest stays
// as that thread wrote it, half of the frames taken back (the heap's lists back and a block's free flag not: blocks then
// freed twice or handed out twice). So a hold that timed out (the rendering thread behind, a pipeline compiling) is waited
// for before the frame closes, briefly when the last such wait was in vain
void CloseHold() {
  if (engine::Hold() || engine::WaitHeld(g_hold_fails ? kRestoreHoldShortMs : kRestoreHoldMs)) g_hold_fails = 0;
  else g_hold_fails++;
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
  }
  // memory goes back only with every other simulation thread held still: a hold that timed out (the rendering thread
  // waiting for the GPU) is waited for a while longer (briefly when the last such wait was in vain too), and if it is
  // still not complete the guesses are put right at a later frame's top (the inputs are looked at again from where
  // they were confirmed) while the frame to go back to will still be kept then; past that the restore goes on, as
  // every restore did until 2026-11-04
  g_correction_waits = false;
  if (wrong < g_frame) {
    const bool held = engine::WaitHeld(g_hold_fails ? kRestoreHoldShortMs : kRestoreHoldMs);
    g_hold_fails = held ? 0 : g_hold_fails + 1;
    if (!held && wrong >= rb::OldestFrame() + 2) {
      g_correction_waits = true;
      g_restores_waited++;
      REXLOG_WARN("mk9 net: frame {}'s guess put right at a later frame's top: the other simulation threads still run",
                  wrong);
      return true;
    }
    if (!held) REXLOG_WARN("mk9 net: back to frame {} with other simulation threads still running", wrong);
  }
  for (PeerState& p : g_peers) p.confirmed = std::max(p.confirmed, std::min(p.contiguous, g_frame));
  if (wrong >= g_frame) return true;
  const uint32_t now = g_frame;
  const PPCContext top = ctx;
  const auto t0 = Clock::now();
  if (!rb::Restore(wrong, ctx)) {
    ctx = top;
    REXLOG_ERROR("mk9 net: cannot go back to frame {} from {}", wrong, now);
    return false;
  }
  g_replay.restore_ms += Ms(Clock::now() - t0);
  game::RolledBack(wrong);
  frame::SetCount(frame::Count() - (now - wrong));
  // nobody sees the frames run again: the GPU skips drawing them (the rendering thread is held, so all it was given
  // so far is of the frames before)
  SkipDraws(true);
  engine::SampleRenderWaits(true);
  for (uint32_t f = wrong; f < now; f++) {
    const auto a = Clock::now();
    g_frame = f;
    SetFrameInputs(f);
    ctx = top;  // the registers at the top of a frame are GuardedMain's loop's, the same every frame
    rb::OpenFrame(f, ctx);
    Tentative(f, ctx, base);
    engine::Release();
    const auto b = Clock::now();
    frame::RunAgain(ctx, base);  // its end hook moves g_frame on
    const auto c = Clock::now();
    CloseHold();
    const auto d = Clock::now();
    rb::CloseFrame();
    g_replay.logs_ms += Ms(b - a) + Ms(Clock::now() - d);
    g_replay.run_ms += Ms(c - b);
    g_replay.render_ms += Ms(d - c);
  }
  engine::SampleRenderWaits(false);
  SkipDraws(false);
  ctx = top;
  g_frame = now;
  g_stats.rollbacks++;
  g_stats.max_rollback = std::max(g_stats.max_rollback, now - wrong);
  g_replay.rollbacks++;
  g_replay.frames += now - wrong;
  g_replay.longest_ms = std::max(g_replay.longest_ms, Ms(Clock::now() - t0));
  const uint32_t depth = now - wrong;
  const double took = Ms(Clock::now() - t0);
  g_ms.rollbacks++;
  g_ms.rerun += depth;
  g_ms.depth[depth <= 2 ? depth - 1 : depth <= 4 ? 2 : 3]++;
  g_ms.rollback_ms += took;
  g_fp.rollback_ms += took;
  g_ms.rollback_longest_ms = std::max(g_ms.rollback_longest_ms, took);
  if (MatchStats::Second* s = StatSecond()) {
    Add(s->rerun, depth);
    s->deepest = uint16_t(std::max<uint32_t>(s->deepest, depth));
  }
  LogReplayTimes(false);
  return true;
}

// Keep the sides even: a PC that runs ahead of the others guesses their inputs further ahead, and every guess that
// was wrong is a rollback that long; the PC behind never guesses. So a PC ahead by more than kPaceAhead / 2 frames
// holds a frame back (the next frame starts a frame late, frame::HoldNext; the screen shows this one once more), as
// often as every kPaceGap frames. A PC that runs fewer frames a second than the other (a slower CPU or GPU, its own
// rollbacks) has the other hold back the frames it falls behind by. Until 0.5.6 a PC held back one frame in 20 at most,
// by sleeping in the frame, which the frame cap mostly made up for (frame.cpp): a player's 0.5.5 game ran ahead of one
// at 49 a second until it guessed 8 frames ahead, its rollbacks 4.5 frames long on average.
// The lead alone could not keep up with a PC that ran fewer than 40 frames a second (one frame held back every other
// frame still runs 40), nor follow one that ran slower for a while, as what it measures lags by the 8 frames of the
// average and a round trip: the faster PC ran to the prediction limit (kMaxPredict) and stayed there, waiting at every
// frame for the other's next input and rolling back 8 frames at every change of it, 40-60 ms each, its screen in fits
// and starts while the slower one's ran evenly (until 0.5.8; two-game test, one game held to 30 a second: the other
// 41 s of 164 waiting at the limit, 575 rollbacks of 5-8 frames, a 60 Hz capture of it 31 frames a second shown again
// in runs of up to 8). So a PC ahead of one that runs fewer than 54 frames a second, and itself clearly faster (not
// counting the frames it held back), also runs at the other's pace: it holds back as many frames as the other runs fewer
// than 60 a second (measured from the frame the other says it runs, over the last kPaceRateFrames frames of ours; a frame
// in two against 30 a second, one in four against 48), and the lead is corrected on top as before.
// How far ahead: mine - theirs is twice the frames (mine: our frame less theirs as we heard it; theirs: theirs less
// ours as they heard it; the trip each way counted in both, once with each sign). Averaged over kPaceWindow frames,
// as a packet's way varies by a frame or more. A frame held back shows in it in two halves: mine is one less from the
// next frame on, theirs one more once they heard of it (a round trip later). Whatever of the holds has not shown in
// the average yet counts as if it had, or the frames held back meanwhile would put this PC behind
constexpr int kPaceWindow = 8;
constexpr double kPaceAhead = 2.5;
constexpr uint32_t kPaceGap = 2;
// A lead holds a frame back only once it has lasted this many frames. What this PC knows of the other's frame is a trip
// old and averaged over kPaceWindow frames, so a PC that lost a frame or two and made them up at once (the cap lets
// the frames after a late one start close together, frame.cpp) showed as behind for about 9 frames after it was level
// again, and the PC ahead held for a lead that was gone: 7 holds in a test fight where the other lost 21 frames, and
// the PC that made up its frames then guessed the holder's inputs 2.3 times a second. A lead that stays (a PC running
// a little slower, or one that does not make up what it lost) is held after 14 frames, a quarter of a second, and then,
// while it lasts (a hold in the last kPaceFollow frames), as quickly as before: a PC that falls behind by a few frames a
// second needs a hold every few frames, and 14 frames of waiting after each would let it fall further behind.
constexpr uint32_t kPaceStreak = 14, kPaceFollow = 40;
// the other's pace: measured over this many of our frames; 54 a second and up is left to the lead alone (which keeps
// up with 40 and more), and so is a PC that runs no faster itself: two PCs both slowed down (both rolling back a lot,
// one GPU between two test games) must not take turns holding back, each slowing the other down in turn
constexpr int kPaceRateFrames = 30;
constexpr double kPaceRateFull = 0.9, kPaceRateMargin = 0.08;

void Pace() {
  Pacing& s = g_pace;
  int sample = std::numeric_limits<int>::min(), trip = 0;
  uint32_t their_frame = 0;
  for (const PeerState& p : g_peers) {
    if (!p.now) continue;  // not heard in this match yet
    const int mine = int(g_frame) - int(p.now);
    const int theirs = int(p.now) - int(p.heard);
    if (mine - theirs > sample) {
      sample = mine - theirs;
      trip = mine + theirs;
      their_frame = p.now;
    }
  }
  if (sample == std::numeric_limits<int>::min()) return;
  s.samples.push_back(sample);
  if (s.samples.size() > size_t(kPaceWindow)) s.samples.pop_front();
  s.round_trip = s.round_trip < 0 ? trip : s.round_trip + (trip - s.round_trip) / 32.0;
  double average = 0;
  for (int v : s.samples) average += v;
  average /= double(s.samples.size());
  // what the frames held back lately have not taken off the average yet
  while (!s.holds.empty() && double(g_frame - s.holds.front()) >= s.round_trip + kPaceWindow) s.holds.pop_front();
  double unseen = 0;
  for (uint32_t h : s.holds) {
    // the samples since it that show it: mine's half from the frame after it on, theirs' from the frame the
    // round trip after it on (they heard of the frame after it then)
    const double k = double(g_frame - h);
    unseen += double(kPaceWindow - std::clamp(k, 0.0, double(kPaceWindow))) / kPaceWindow;
    unseen += double(kPaceWindow - std::clamp(k - s.round_trip, 0.0, double(kPaceWindow))) / kPaceWindow;
  }
  const double ahead = average - unseen;
  s.ahead_sum += ahead;
  s.ahead_n++;
  static const double at_least = [] {  // tests: MK9_TEST_PACE_AHEAD=<twice the frames>, 0 never holds
    const char* v = std::getenv("MK9_TEST_PACE_AHEAD");
    return v && *v ? std::atof(v) : kPaceAhead;
  }();
  static const uint32_t streak_needed = [] {  // tests: MK9_TEST_PACE_STREAK=<frames>
    const char* v = std::getenv("MK9_TEST_PACE_STREAK");
    return v && *v ? uint32_t(std::atoi(v)) : kPaceStreak;
  }();
  s.streak = at_least > 0 && ahead >= at_least ? s.streak + 1 : 0;
  const bool following = s.had_hold && g_frame - s.last_hold < kPaceFollow;
  const bool hold = at_least > 0 && ahead >= at_least && (s.streak >= streak_needed || following) &&
                    (s.holds.empty() || g_frame - s.holds.back() >= kPaceGap);
  // the other's pace: the frames it ran a 60 Hz period over our last kPaceRateFrames frames, and ours not counting the
  // periods we stood still. Held back to only by a PC that runs clearly faster than the other, which runs clearly slower
  // than 60 a second; when more than half the threshold ahead (the one behind, or both near even, running slower too
  // would slow the other down in turn), and then for as long as it is not behind: stopped and started again at every
  // dip of the lead under the half (each correction of the lead makes one), the faster PC ran bursts at 60 a second
  // between frames shown three times
  s.tops.push_back({Clock::now(), their_frame, s.paused});
  while (s.tops.size() > size_t(kPaceRateFrames)) s.tops.pop_front();
  double rate = 1.0, mine = 1.0;
  if (s.tops.size() == size_t(kPaceRateFrames)) {
    const double span = Ms(s.tops.back().when - s.tops.front().when) * 60.0 / 1000.0;
    const double ran = double(int(s.tops.back().theirs - s.tops.front().theirs));
    const double ran_here = span - (s.tops.back().paused - s.tops.front().paused);
    if (span > 1.0) rate = std::clamp(ran / span, 0.25, 1.0);
    if (ran_here > 1.0) mine = std::clamp(double(kPaceRateFrames - 1) / ran_here, 0.25, 1.0);
    g_ms.their_rate += rate;
    g_ms.my_rate += mine;
    g_ms.rates_n++;
  }
  if (s.round_trip >= 0) {
    g_ms.round_trip += s.round_trip;
    g_ms.trips_n++;
  }
  s.their_rate = rate;
  s.my_rate = mine;
  s.keeping = at_least > 0 && rate < kPaceRateFull && rate < mine - kPaceRateMargin &&
              (ahead >= at_least / 2 || (s.keeping && ahead >= 0));
  if (s.keeping) s.owed += 1.0 / rate - 1.0;
  else s.owed = 0;
  const int keep_pace = int(s.owed);
  s.owed -= keep_pace;
  const int periods = std::min(3, keep_pace + (hold ? 1 : 0));
  // MK9_TRACE_PACE=<file> (diagnostics): every frame's sample, round trip, average, unseen part, hold, the time, the
  // other's pace, this PC's and the frames held back
  static FILE* trace = [] {
    const char* v = std::getenv("MK9_TRACE_PACE");
    return v && *v ? std::fopen(v, "w") : nullptr;
  }();
  if (trace) {
    static const auto t0 = Clock::now();
    std::fprintf(trace, "%u %d %d %.2f %.2f %.2f %d %.1f %.2f %.2f %d\n", g_frame, sample, trip, average, unseen,
                 ahead, hold ? 1 : 0, Ms(Clock::now() - t0), rate, mine, periods);
    std::fflush(trace);
  }
  // the lead's own correction shows in the average (unseen above); what keeps pace with the other does not move it
  if (hold) {
    s.holds.push_back(g_frame);
    s.last_hold = g_frame;
    s.had_hold = true;
  }
  if (periods) {
    s.held += uint32_t(periods);
    s.paused += periods;
    g_ms.held += uint32_t(periods);
    if (MatchStats::Second* sec = StatSecond()) Add(sec->held, uint32_t(periods));
    frame::HoldNext(periods);
  }
}

uint64_t g_meet_frame = 0;  // tests: meet at the top of this frame (counted from boot)
std::atomic<bool> g_meet_asked{false};  // the launcher's start
std::atomic<uint32_t> g_meet_seed{0};

// A stick or trigger that drifts (reads a little pushed while nobody touches it) changes the input every frame, and
// every change the other PC guessed wrong is a rollback: a player's 0.5.5 opponent, sticks a few steps off centre,
// had them roll back up to 8 frames at almost every frame. So each stick axis within 25 of 127 (6400, under the 0.2
// of full deflection UE3's axes ignore) and each trigger within 25 of 255 reads as untouched, on both PCs alike: no
// push the game itself reads is that small (a direction needs far more)
constexpr int kStickQuiet = 6400, kTriggerQuiet = 25;

input::Pad Quiet(input::Pad p) {
  static const bool off = [] {  // tests: MK9_TEST_NO_QUIET=1, what drift costs without it
    const char* v = std::getenv("MK9_TEST_NO_QUIET");
    return v && *v == '1';
  }();
  if (off) return p;
  const auto axis = [](int16_t v) { return std::abs(int(v)) <= kStickQuiet ? int16_t(0) : v; };
  p.lx = axis(p.lx);
  p.ly = axis(p.ly);
  p.rx = axis(p.rx);
  p.ry = axis(p.ry);
  if (p.left_trigger <= kTriggerQuiet) p.left_trigger = 0;
  if (p.right_trigger <= kTriggerQuiet) p.right_trigger = 0;
  return p;
}

// this frame's local input, which waits `delay` frames (a test's script for this slot stands in for the pad), to
// every other player
void SendOwn() {
  input::Pad local;
  if (!autopilot::LocalPad(local) && !testinput::ScriptedPad(uint32_t(g_cfg.slot), local))
    local = input::ReadPhysical(0);
  const uint32_t at = g_frame + uint32_t(Delay());
  if (g_cfg.rollback && g_match >= 2 && at < kQuietFrames) local = input::Pad{};  // a fight's first frames
  g_mine[at] = Pack(Quiet(local));
  FeedOwn(at, g_mine[at]);
  for (PeerState& p : g_peers) SendInputs(p);
}

void OnTop(PPCContext& ctx, uint8_t* base) {
  // the rendering thread and the job workers, held while rollback keeps or puts back memory, go
  // on once the frame does
  struct ReleaseAtEnd {
    ~ReleaseAtEnd() { engine::Release(); }
  } release_at_end;
  ApplyPending();
  if (g_status == Status::Off) return StopRolling();
  if (g_status == Status::InSync) StatTop();
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
  bool sent = false;
  if (g_rolling) {
    CloseHold();
    rb::CloseFrame();  // the frame that just ran
    // this frame's input goes before any frame runs again: a rollback 3 frames back takes 25 ms or more, and all
    // that time the others would guess it, and roll back themselves once it came (until 0.5.5 it went after)
    if (!g_cfg.watch) {
      SendOwn();
      sent = true;
    }
    if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
    // the fight is over: lockstep again once every frame so far ran with the real inputs
    if (game::Phase() != game::kPhaseFight) {
      if (g_frame && !WaitForInputs(g_frame - 1)) return Lost(Status::Disconnected);
      if (!CorrectGuesses(ctx, base)) return Lost(Status::Desynced);
      if (game::Phase() != game::kPhaseFight && !g_correction_waits) StopRolling();
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
  if (!g_cfg.watch && !sent) SendOwn();
  if (g_rolling) {
    // never further ahead of what anyone sent than kMaxPredict frames
    if (g_frame >= kMaxPredict) {
      const auto t = Clock::now();
      const uint32_t waits = g_stats.waits;
      if (!WaitForInputs(g_frame - kMaxPredict)) return Lost(Status::Disconnected);
      if (g_stats.waits != waits) {
        const double ms = Ms(Clock::now() - t);
        g_pace.limit_waits++;
        g_pace.limit_ms += ms;
        g_pace.paused += ms * 60.0 / 1000.0;  // standing still, as a frame held back (Pace)
        g_fp.wait_ms += ms;
        frame::DropCatchUp();  // waiting for the others is not time to make up
        g_ms.limit_waits++;
        g_ms.limit_ms += ms;
        if (MatchStats::Second* s = StatSecond()) Add(s->wait_ms, uint32_t(ms + 0.5));
      }
    }
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

void SetDrawSkip(std::function<void(bool skip)> skip) { g_draw_skip = std::move(skip); }

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
bool Waiting() { return g_waiting.load(); }
uint32_t Frame() { return g_frame; }

uint32_t Settled() {
  if (!g_rolling) return rb::Armed() ? rb::OldestFrame() : UINT32_MAX;
  uint32_t settled = g_frame;
  for (const PeerState& p : g_peers) settled = std::min(settled, p.confirmed);
  return settled;
}

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
  g_pace = {};
  g_draws_apart = 0;
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
  for (PeerState& p : g_peers) {
    p.contiguous = uint32_t(Delay());
    while (p.inputs.count(p.contiguous)) p.contiguous++;  // inputs that came during the barrier (a spectator's feed)
  }
  // the next match's READYs that came while this barrier waited: they are past this match (WaitForInputs plays the
  // rest of it as neutral input), and the next barrier has them
  for (PeerState& p : g_peers) {
    if (p.next_match == uint16_t(g_match + 1)) {
      p.ready_match = p.next_match;
      p.seed = p.next_seed;
      p.words = std::move(p.next_words);
      p.delay = p.next_delay;
    }
    p.next_match = 0;
    p.next_words.clear();
  }
  g_status = Status::InSync;
  input::SetSource(&FrameInput);
  // with the id the game's next object takes: each script process's draws are told apart by its id, so two PCs whose
  // loading made a different number of objects before a fight draw at different sites (a desync's report has it)
  g_barrier_id = game::NextObjectId();
  REXLOG_INFO("mk9 net: match {} in sync, seed {:08X}, input delay {}, next object id {:08X}", g_match, agreed.seed,
              Delay(), g_barrier_id);
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

void Check(uint32_t frame, uint32_t fight, uint32_t draws) {
  if (g_status != Status::InSync) return;
  g_my_checks[frame] = {fight, draws};
  Writer w = Header(kCheck);
  w.U32(frame);
  w.U32(fight);
  w.U32(draws);
  if (!g_cfg.watch) SendAll(w);
  Writer fc = Header(kFeedCheck);
  fc.U32(frame);
  fc.U32(fight);
  fc.U32(draws);
  FeedSend(fc);
}

Stats GetStats() { return g_stats; }

void ResetMatchStats() {
  g_ms = {};
  g_fp.rollback_ms = g_fp.wait_ms = 0;
  g_fp.cycles = 0;
}

std::string MatchStatsJson(const std::string& match, const char* end) {
  const MatchStats& m = g_ms;
  std::string id;
  for (char c : match) id += std::isalnum(uint8_t(c)) || c == '-' || c == '_' ? c : '_';
  const double secs = m.start == Clock::time_point{} ? 0.0 : Ms(Clock::now() - m.start) / 1000.0;
  const auto avg = [](double sum, uint32_t n, double scale) { return n ? sum / n * scale : 0.0; };
  std::string out = fmt::format(
      "{{\"v\":1,\"match\":\"{}\",\"end\":\"{}\",\"netcode\":\"{}\",\"delay\":{},\"secs\":{:.1f},\"fight_secs\":{:.1f},"
      "\"frames\":{},\"late\":{},\"stalls\":{},\"longest_ms\":{:.1f},\"rollbacks\":{},\"rerun\":{},\"depth\":[{},{},{},{}],"
      "\"rollback_ms\":{:.1f},\"rollback_longest_ms\":{:.1f},\"waits\":{},\"wait_ms\":{:.0f},\"held\":{},\"late_why\":[{},{},{},{},{}],\"shown_late\":{},\"shown_stalls\":{},\"screen_lag\":{:.2f},"
      "\"their_fps\":{:.1f},\"my_fps\":{:.1f},\"round_trip\":{:.1f},\"per_second\":[",
      id, end, g_cfg.rollback ? "rollback" : "lockstep", Delay(), secs, m.fight_ms / 1000.0, m.frames, m.late,
      m.stalls, m.longest_ms, m.rollbacks, m.rerun, m.depth[0], m.depth[1], m.depth[2], m.depth[3], m.rollback_ms,
      m.rollback_longest_ms, m.limit_waits, m.limit_ms, m.held, m.late_why[0], m.late_why[1], m.late_why[2],
      m.late_why[3], m.late_why[4], m.shown_late, m.shown_stalls, avg(m.lag_sum, m.lag_n, 1.0), avg(m.their_rate, m.rates_n, 60.0),
      avg(m.my_rate, m.rates_n, 60.0), avg(m.round_trip, m.trips_n, 1.0));
  for (size_t i = 0; i < m.seconds.size(); i++) {
    const MatchStats::Second& s = m.seconds[i];
    out += fmt::format("{}[{},{},{},{},{},{}]", i ? "," : "", s.frames, s.late, s.rerun, s.deepest, s.wait_ms, s.held);
  }
  out += "]";
  // where the main thread stood in the late frames: the most common ones, as "41% rexruntime.dll+0x247060 < ..."
  CollectStalls(true);
  std::vector<std::pair<uint32_t, std::string>> where;
  for (const auto& [text, n] : g_ms.stall_where) where.emplace_back(n, text);
  std::sort(where.begin(), where.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (!where.empty() && g_ms.stall_samples) {
    out += ",\"stall_where\":[";
    for (size_t i = 0; i < where.size() && i < 6; i++) {
      std::string text = where[i].second.substr(0, 230);
      for (char& c : text)
        if (c == '"' || c == '\\' || c < ' ') c = '/';
      out += fmt::format("{}\"{}% {}\"", i ? "," : "", 100 * where[i].first / g_ms.stall_samples, text);
    }
    out += "]";
  }
  out += "}";
  return out;
}

}  // namespace mk9::net
