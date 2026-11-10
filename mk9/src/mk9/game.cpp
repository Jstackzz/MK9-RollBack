// MK9's state, see game.h.

#include "mk9/game.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9/frame.h"
#include "mk9/input.h"
#include "mk9/net/netplay.h"
#include "mk9/net/rollback.h"
#include "mk9/randtrace.h"

REX_EXTERN(sub_82B29580);  // the C runtime's _getptd
REX_EXTERN(__imp__sub_826D3088);  // seeds the game's random number generator
REX_EXTERN(__imp__sub_826D91C0);  // draws an int from it
REX_EXTERN(__imp__sub_826D91E8);  // draws a float from it

namespace mk9::game {
namespace {

constexpr uint32_t kRoundTimer = 0x833CED24;
constexpr uint32_t kGameRand = 0x834161D8;
uint32_t g_last_rand = 0, g_last_timer = 0;
uint32_t g_agreed_seed = 0;  // the last barrier's

// A draw must not depend on when it happens: loading takes longer on one PC than another, and
// what draws every frame (sub_82382E40) then draws more often on one before, say, the arena
// variant is picked; in a fight Test Your Luck's screens draw a frame earlier or later from one PC
// to the next. So in a match each call site draws from a stream of its own: the agreed seed, the
// site, and how many times it drew since the barrier. The journal lets a rollback take back the
// counts of the frames it puts back.
std::unordered_map<uint64_t, uint32_t> g_site_draws;
struct SiteDraw {
  uint32_t frame;
  uint64_t site;
};
std::deque<SiteDraw> g_site_journal;
// what the last checks counted, the draws at each call site (CheckDraws)
std::map<uint32_t, std::vector<std::pair<uint64_t, uint32_t>>> g_check_draws;
// the MKScript globals around the timer (types, values, the fighters' names)
constexpr uint32_t kScriptGlobalsLo = 0x833CE000, kScriptGlobalsHi = 0x833CF000;

uint8_t* Host(uint32_t a) {
  auto* ks = REX_KERNEL_STATE();
  return ks ? ks->memory()->TranslateVirtual<uint8_t*>(a) : nullptr;
}

uint32_t Fnv(const uint8_t* p, size_t n, uint32_t h = 2166136261u) {
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

uint32_t Mix(uint32_t a, uint32_t b) {
  uint32_t h = a ^ (b * 0x9E3779B9u);
  h ^= h >> 16;
  h *= 0x85EBCA6Bu;
  h ^= h >> 13;
  h *= 0xC2B2AE35u;
  return h ^ (h >> 16);
}

}  // namespace

uint32_t ReadU32(uint32_t a) {
  uint8_t* p = Host(a);
  if (!p) return 0;
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

void WriteU32(uint32_t a, uint32_t value) {
  uint8_t* p = Host(a);
  if (!p) return;
  value = __builtin_bswap32(value);
  std::memcpy(p, &value, 4);
}

int StackReturns(const PPCContext& ctx, uint32_t* out, int max) {
  const uint32_t pcr = ctx.r13.u32;
  if (!pcr) return 0;
  const uint32_t hi = ReadU32(pcr + 0x70), lo = ReadU32(pcr + 0x74);  // stack_base_ptr, stack_end_ptr
  if (hi <= lo) return 0;
  int n = 0;
  uint32_t sp = ctx.r1.u32;
  while (n < max && sp >= lo && sp + 4 <= hi) {
    const uint32_t back = ReadU32(sp);
    if (back <= sp || back > hi || back - 8 < lo) break;
    out[n++] = ReadU32(back - 8);
    sp = back;
  }
  return n;
}

int32_t RoundTimer() { return int32_t(ReadU32(kRoundTimer)); }
int32_t Phase() { return int32_t(ReadU32(0x833C6C58)); }
uint32_t UiScreen() { return ReadU32(0x833AEA80); }
bool TitleUp() { return ReadU32(0x833C77B8) == 1; }
int RoundWinner() { return int(ReadU32(0x833C6C74)); }
int RoundsWon(int player) { return int(ReadU32(0x833C7A38 + 4 * uint32_t(player & 1))); }
int Round() { return int(ReadU32(0x833CED28)); }
uint32_t NextObjectId() { return ReadU32(0x83394B44); }

#ifndef MK9_VERSION
#define MK9_VERSION "0.0.0"
#endif

uint32_t ReleaseStamp() {
  static const uint32_t stamp = Fnv(reinterpret_cast<const uint8_t*>(MK9_VERSION), sizeof(MK9_VERSION) - 1);
  return stamp;
}

// The game state's frame counter (0x833C6C54 + 112), which counts from when a fight starts loading: two PCs come to the
// fight's barrier with it as many frames apart as their loading was. Bit 0 of it decides each frame whose script
// processes run first (sub_823BCC70 puts P1's in run list 12 + that bit and P2's in the other, and the lists run in
// order), so the fighters' scripts ran in opposite orders on two PCs one frame apart, and at a hit where both queue
// effects on one list (sub_829ED270) the order of the list told one game to spawn a pair of effects a frame later
// (0.5.4's remaining desync). Every PC takes slot 0's at the barrier (Agree)
constexpr uint32_t kFrameCounter = 0x833C6CC4;

std::vector<uint32_t> ReadyWords() { return {ReleaseStamp(), uint32_t(RoundTimer()), ReadU32(kFrameCounter)}; }

bool SameRelease(const mk9::net::Agreement& agreed) {
  for (int s = 0; s < mk9::net::kMaxSlots; s++) {
    const auto& w = agreed.words[s];
    if (w.empty()) continue;
    if (w[0] != ReleaseStamp()) {
      REXLOG_ERROR("mk9 game: slot {} runs another release of the game (stamp {:08X}, this one {} = {:08X})", s,
                   w[0], MK9_VERSION, ReleaseStamp());
      return false;
    }
  }
  return true;
}

namespace {

// _getptd: the calling thread's C runtime data (allocated on first use), r3
uint32_t MainPtd(PPCContext& ctx, uint8_t* base) {
  rex::CallFrame frame(ctx);
  frame.ctx.r1.u32 -= 0x200;
  sub_82B29580(frame.ctx, base);
  return frame.ctx.r3.u32;
}

}  // namespace

uint32_t MainRandState(PPCContext& ctx, uint8_t* base) {
  uint32_t ptd = MainPtd(ctx, base);
  return ptd ? ReadU32(ptd + 20) : 0;
}

void SetMainRandState(PPCContext& ctx, uint8_t* base, uint32_t state) {
  if (uint32_t ptd = MainPtd(ctx, base)) WriteU32(ptd + 20, state);
}

uint32_t GameRandState() { return ReadU32(kGameRand); }
void SetGameRandState(uint32_t state) { WriteU32(kGameRand, state); }

namespace {

// where a draw comes from: its return address and the caller's own (draws mostly go through a
// helper, such as sub_82A234E0 for a number below n). The generator's functions make no stack
// frame, so r1 is the caller's frame: its back chain word leads to the frame above, under which
// (at -8) the caller saved its return address
// MK's script VM draws all its scripts' random(n) through one native (sub_824705F8, its call into
// the helper returning to 0x8247061C): those draws are told apart by the script process (the VM's
// current context at 0x8342CDF0, its object id at +4), so one script drawing more often (an arena's
// ambience reacting to a sound ending) leaves the others' numbers alone. The id comes from the one
// counter every MK object takes its id from (0x83394B44, sub_829DBEF0), in the order the main thread
// makes them; but two PCs' menus and loading do not make the same number of objects before a fight
// (0.5.3: the same script drew on frames 0-29 in both games as process 10000BB7 in one and 10000BBA
// in the other, its stream and the checks then apart by frame 30), so a process counts from the id
// the next object took at the last barrier (g_id_base, Agree): the same on every PC for the objects
// made after it, but not for those made before it (0.5.5: the same fighter script process -0x13D from the
// barrier on one PC and -0x13B on the other, one draw and two by the same check), whatever the PCs' loading made
// around them. So a process that was already alive at the barrier is told apart by what it runs, not by its id: its script
// function's hash (the function's record, process +48, holds it at +4: the same on every PC) and its place, in the order
// of ids, among the processes of that function alive then (ProcessesAtBarrier, Agree). Its draws show at 8247061C ^ (stream << 4);
// one asleep at a fight's barrier (on no run list until a move or event wakes it) draws by its script alone, one stream for all its function's sleepers;
// a process made after a fight's barrier as ever counts from the base (`id - base`), the same on every PC, and one made during the
// menus and loading, since the barrier at the launcher's start, is told apart by its script function and the order that function's processes
// first drew in (its ids, again, differ by what each PC's loading made). Netplay's "match N in sync" line names the base (next object id)
constexpr uint32_t kScriptRandomCaller = 0x8247061C;
constexpr uint32_t kScriptContext = 0x8342CDF0;
// the VM's 21 run lists (sub_82A164C8 runs them in order), 20 bytes each: +4 the first node; a node: +4 the process,
// +8 its object id (the node of a process that went away is left until the VM comes by), +12 the next node
constexpr uint32_t kScriptRunLists = 0x8342CDFC;
uint32_t g_id_base = 0;
// the processes on the run lists at the last barrier: their object ids and the stream each draws from
std::unordered_map<uint32_t, uint32_t> g_barrier_streams;
// whether the last barrier was a fight's (not the launcher's start, where menus and loading follow), and, since a barrier that wasn't,
// the processes that have drawn by id, their streams, and per script function hash how many have
bool g_fight_barrier = false;
std::unordered_map<uint32_t, uint32_t> g_loading_streams, g_loading_counts;

// What each PC made since the last barrier, for reports (the two players' lines side by side say where their loading went apart):
// how many objects each caller of the one id-giving constructor made, and a hash of their order. Only the main thread, only a match in sync
uint32_t g_objects_made = 0, g_objects_order = 2166136261u;
std::map<uint32_t, uint32_t> g_objects_by_maker;
std::string g_last_barrier_objects;

void NoteObjectMade(uint32_t caller) {
  g_objects_made++;
  g_objects_order = (g_objects_order ^ caller) * 16777619u;
  g_objects_by_maker[caller]++;
}

void LogObjectsMade() {
  std::vector<std::pair<uint32_t, uint32_t>> makers(g_objects_by_maker.begin(), g_objects_by_maker.end());
  std::sort(makers.begin(), makers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string line;
  for (size_t i = 0; i < makers.size() && line.size() < 96; i++) line += fmt::format(" {:08X}={}", makers[i].first, makers[i].second);
  g_last_barrier_objects = fmt::format("objects made since the last barrier: {}, order hash {:08X}, by{}", g_objects_made, g_objects_order, line);
  REXLOG_INFO("mk9 game: {}", g_last_barrier_objects);
  g_objects_made = 0;
  g_objects_order = 2166136261u;
  g_objects_by_maker.clear();
}

// the stream a script process draws from (see above): a process made after a fight's barrier by its place after it, one
// alive at the last barrier by its script and its place among that script's processes, one made before the barrier and woken in the
// fight by its script alone, one made during the menus and loading by its script and the order that script's processes first drew in
uint32_t ProcessStream(uint32_t context) {
  const uint32_t id = ReadU32(context + 4);
  static const bool old = [] {  // comparisons: MK9_TEST_ID_STREAMS=1 counts every process from the base, as until 0.5.6
    const char* v = std::getenv("MK9_TEST_ID_STREAMS");
    return v && *v == '1';
  }();
  if (old) return id - g_id_base;
  if (const auto it = g_barrier_streams.find(id); it != g_barrier_streams.end()) return it->second;
  const uint32_t function = ReadU32(context + 48);
  const uint32_t hash = function ? ReadU32(function + 4) : 0;
  if (g_fight_barrier && Phase() == kPhaseFight) {
    // made before the barrier and not running at it (on no run list: a process that sleeps until a move or an event wakes it):
    // its script's stream, shared with that script's other sleepers in the order the VM runs them, the same on every PC, and rollbacks take its draws back
    // with the rest. Made after the barrier: a fight's objects are made alike on every PC from its barrier on, so as ever by its place after it
    return id < g_id_base ? Mix(hash, 0x7FFFFFFFu) : id - g_id_base;
  }
  // made since a barrier in the menus, the select screen or a fight's loading (also after a fight, towards the next one): each PC makes other
  // objects, so not its id either: its script function's hash and the order its function's processes first drew in. Taken back by no rollback
  if (const auto it = g_loading_streams.find(id); it != g_loading_streams.end()) return it->second;
  const uint32_t stream = Mix(hash ^ 0x4C4F4144u, g_loading_counts[hash]++);
  g_loading_streams[id] = stream;
  return stream;
}

void ProcessesAtBarrier() {
  g_barrier_streams.clear();
  std::map<uint32_t, std::vector<uint32_t>> by_function;
  for (uint32_t list = 0; list < 21; list++) {
    uint32_t seen = 0;  // a broken list can't hang the game
    for (uint32_t node = ReadU32(kScriptRunLists + 20 * list + 4); node && seen < 8192; node = ReadU32(node + 12), seen++) {
      const uint32_t process = ReadU32(node + 4);
      if (!process || ReadU32(process + 4) != ReadU32(node + 8)) continue;
      const uint32_t function = ReadU32(process + 48);
      if (function) by_function[ReadU32(function + 4)].push_back(ReadU32(process + 4));
    }
  }
  std::string line;
  for (auto& [hash, ids] : by_function) {
    // a process moved between lists has a node in each until the VM comes by
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (size_t place = 0; place < ids.size(); place++) {
      g_barrier_streams[ids[place]] = Mix(hash, uint32_t(place));
      if (line.size() < 140) line += fmt::format(" {:X}:{:08X}/{}", ids[place], hash, place);
    }
  }
  // read with a report's draws (8247061C ^ (stream << 4)): which process each stream belonged to, by id, script and place
  REXLOG_INFO("mk9 game: script processes at the barrier ({}):{}", g_barrier_streams.size(), line);
}

uint64_t DrawSite(const PPCContext& ctx) {
  uint32_t up = 0;
  StackReturns(ctx, &up, 1);
  if (up == kScriptRandomCaller) {
    const uint32_t context = ReadU32(kScriptContext);
    if (context) up ^= ProcessStream(context) << 4;
  }
  return (uint64_t(uint32_t(ctx.lr)) << 32) | up;
}

// MK9_TRACE_SCRIPT=<file> (diagnostics): what the main thread's scripts do in a match, one line each:
//   [r]<netplay frame> draw id <process> ctx <context> ip <code> n <n> up <return addresses>
//   [r]<netplay frame> new id <id> obj <object> by <caller> thread m|o up <return addresses>
//   [r]<netplay frame> pads <each player's pad as the game read it before the frame's tick>
//   rollback to <frame> from <frame>
// ("r": a rollback runs the frame again). tools/scriptcmp.py takes back what rollbacks took back
// and lists the frames where two games' scripts did something different
FILE* ScriptTrace() {
  static FILE* f = [] {
    const char* v = std::getenv("MK9_TRACE_SCRIPT");
    return v && *v ? std::fopen(v, "w") : nullptr;
  }();
  return f;
}

void TraceScriptDraw(const PPCContext& ctx) {
  FILE* f = ScriptTrace();
  if (!f) return;
  uint32_t returns[8] = {};
  const int n = StackReturns(ctx, returns, 8);
  if (n < 1 || returns[0] != kScriptRandomCaller) return;
  const uint32_t context = ReadU32(kScriptContext);
  const uint32_t args = context ? ReadU32(context + 60) : 0;
  std::fprintf(f, "%s%u draw id %X ctx %08X ip %08X n %u up", frame::Replaying() ? "r" : "", net::Frame(),
               context ? ReadU32(context + 4) : 0, context, context ? ReadU32(context + 8) : 0,
               args ? ReadU32(args + 4) : 0);
  for (int i = 1; i < n; i++) std::fprintf(f, " %08X", returns[i]);
  std::fputc('\n', f);
  std::fflush(f);
}

void BeforeDraw(PPCContext& ctx) {
  if (!net::InSync() || !frame::OnMainThread()) return;
  TraceScriptDraw(ctx);
  const uint64_t site = DrawSite(ctx);
  const uint32_t n = g_site_draws[site]++;
  // kept for as far back as a rollback can go (a fight draws hundreds a frame), under the frame rollback keeps them
  // with: a frame's end, after its tick, where netplay already counts the next frame, is still that frame's, and a
  // rollback to the next frame keeps what it drew. Kept under the next frame, a rollback there took those draws off
  // the counts while it kept what they did, and the game that rolled back counted differently from then on
  const uint32_t now = rb::Armed() ? rb::CurrentFrame() : net::Frame();
  g_site_journal.push_back({now, site});
  while (g_site_journal.size() > (1u << 22) ||
         (!g_site_journal.empty() && g_site_journal.front().frame + 64 < now))
    g_site_journal.pop_front();
  SetGameRandState(Mix(g_agreed_seed ^ Mix(uint32_t(site), uint32_t(site >> 32)), n));
}

}  // namespace

void RolledBack(uint32_t frame) {
  if (FILE* f = ScriptTrace()) {
    std::fprintf(f, "rollback to %u from %u\n", frame, net::Frame());
    std::fflush(f);
  }
  // a site whose first draws are all taken back goes too: kept at 0 it was one more site in this game's checks than in
  // a game that never drew there, the same draws hashing differently (0.5.2's desyncs with the same draws on both sides)
  while (!g_site_journal.empty() && g_site_journal.back().frame >= frame) {
    auto it = g_site_draws.find(g_site_journal.back().site);
    if (it != g_site_draws.end() && it->second && --it->second == 0) g_site_draws.erase(it);
    g_site_journal.pop_back();
  }
}

// tests: MK9_TEST_BURN_IDS=<n>[:<frames>]: this game's next object id goes n ahead, right at the barrier if no <frames> is said, else
// that many frames after a fight starts loading: objects made before then are n further from the barrier's next id than
// on a game that doesn't, as in 0.5.5's reports (the same process -0x13D from the barrier on one PC and -0x13B on the other)
void BurnIds(bool at_barrier, int32_t phase) {
  static const auto burn = []() -> std::pair<uint32_t, int> {
    const char* v = std::getenv("MK9_TEST_BURN_IDS");
    if (!v || !*v) return {0, 0};
    const char* colon = std::strchr(v, ':');
    return {uint32_t(std::atoi(v)), colon ? std::atoi(colon + 1) : 0};
  }();
  if (!burn.first || !net::InSync()) return;
  static int loading_frames = -1;
  bool now = false;
  if (at_barrier) {
    now = burn.second == 0 && phase == kPhaseFight;
  } else if (burn.second > 0) {
    if (phase != kPhaseLoading) loading_frames = -1;
    else now = ++loading_frames == burn.second;
  }
  if (!now) return;
  REXLOG_INFO("mk9 game: the next object id goes {} ahead ({:X}) at frame {} (MK9_TEST_BURN_IDS)", burn.first, NextObjectId(), net::Frame());
  WriteU32(0x83394B44, NextObjectId() + burn.first);
}

void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed) {
  const uint32_t before = GameRandState(), crt_before = MainRandState(ctx, base);
  g_agreed_seed = agreed.seed;
  BurnIds(true, Phase());
  g_fight_barrier = Phase() == kPhaseFight;
  g_loading_streams.clear();
  g_loading_counts.clear();
  g_id_base = NextObjectId();  // script processes count from here (DrawSite)
  ProcessesAtBarrier();
  LogObjectsMade();
  g_site_draws.clear();
  g_site_journal.clear();
  // each match numbers its frames from 0: the menus' checks, at higher frames, kept a fight's first ones out
  g_check_draws.clear();
  SetGameRandState(agreed.seed);
  SetMainRandState(ctx, base, Mix(agreed.seed, 0xC0FFEEu));
  // a fight's barrier: the frame counter as the lowest slot that sent one has it (a watcher sends none). Not at the
  // launcher's start, where each game counts the menus since it was parked
  const uint32_t counter_before = ReadU32(kFrameCounter);
  for (int s = 0; Phase() == kPhaseFight && s < mk9::net::kMaxSlots; s++) {
    if (agreed.words[s].size() < 3) continue;
    WriteU32(kFrameCounter, agreed.words[s][2]);
    break;
  }
  REXLOG_INFO("mk9 game: agreed seed {:08X} (the game's generator was {:08X}, rand() {:08X}; next object id {:08X}; "
              "frame counter {:X}, was {:X})",
              agreed.seed, before, crt_before, ReadU32(0x83394B44), ReadU32(kFrameCounter), counter_before);
}

bool SyncPoint() {
  static int32_t last = -1;
  const int32_t now = Phase();
  BurnIds(false, now);
  const bool begins = now == kPhaseFight && last != kPhaseFight && last != -1;
  last = now;
  if (begins) REXLOG_INFO("mk9 game: a fight begins (round timer {})", RoundTimer());
  return begins;
}

bool Comparable() {
  const int32_t phase = Phase();
  return phase == kPhaseFight || phase == kPhaseOver;
}

// The fight's HUD (mk9/names.cpp: 0x833CED48 holds it from a fight's first round to its end, 0 otherwise), an object of
// the simulation heap that rollback puts back: the two fighters' health at +0x20 and +0x24 (floats, 1 full, back at 1 each
// round), their super meters at +0x7C and +0x80 and the meters' whole bars at +0x8C and +0x90. It moves in fixed steps a
// frame (test games at different frame rates held the same values on the same frames; the bars' animation at +0x40 is
// 0.005 a frame, its timers 1/60).
constexpr uint32_t kFightHud = 0x833CED48;
// Not in a fight's first frames: the HUD is made as the first round begins, and it comes with values of no meaning
constexpr uint32_t kHudFromFrame = 60;

float F32At(uint32_t address) {
  const uint32_t v = ReadU32(address);
  float f;
  std::memcpy(&f, &v, sizeof f);
  return f;
}

FightBars FightNow() {
  FightBars b;
  const uint32_t hud = Phase() == kPhaseFight ? ReadU32(kFightHud) : 0;
  if (!hud) return b;
  b.health[0] = F32At(hud + 0x20);
  b.health[1] = F32At(hud + 0x24);
  b.meter[0] = F32At(hud + 0x7C);
  b.meter[1] = F32At(hud + 0x80);
  return b;
}

StateCheck StateHash(PPCContext& ctx, uint8_t* base) {
  // only values that do not depend on where this process's heaps put things (the script globals
  // hold pointers): the round timer and the fight's HUD, and below the draws per call site. The game's
  // random number generator is logged beside it (LastCheckDetail): other threads draw from it in
  // their own order, so it is no check of its own
  (void)ctx, (void)base;
  g_last_rand = GameRandState();
  g_last_timer = ReadU32(kRoundTimer);
  // what the simulation drew at each call site so far: a fight that went differently draws differently, and so may
  // a script that only dresses the arena (netplay.cpp, CompareChecks)
  std::vector<std::pair<uint64_t, uint32_t>> draws(g_site_draws.begin(), g_site_draws.end());
  std::sort(draws.begin(), draws.end());
  // MK9_NET_SITES_LOG=<file> (diagnostics): the draws per call site at every check
  static FILE* sites_log = [] {
    const char* v = std::getenv("MK9_NET_SITES_LOG");
    return v && *v ? std::fopen(v, "w") : nullptr;
  }();
  if (sites_log) {
    std::fprintf(sites_log, "frame %u phase %d\n", net::Frame(), Phase());
    for (const auto& [site, n] : draws)
      std::fprintf(sites_log, "%08X %08X %u\n", uint32_t(site >> 32), uint32_t(site), n);
    std::fflush(sites_log);
  }
  uint32_t sites = 0x811C9DC5u;
  for (const auto& [site, n] : draws) {
    const uint64_t w[2] = {site, n};
    sites = Fnv(reinterpret_cast<const uint8_t*>(w), sizeof w) ^ (sites * 0x01000193u);
  }
  g_check_draws[net::Frame()] = draws;
  while (g_check_draws.size() > 16) g_check_draws.erase(g_check_draws.begin());
  // the fight's values only in a fight and at its end: outside one they still hold the last fight's,
  // which differ from game to game (a demo fight the title played, a match watched, none since boot).
  // With the round and the rounds each player won: a fight that went differently (a different
  // winner, a round more) shows at the next check. Not the phase: loading ends on its own time.
  // And what the fighters have left: a hit, a blocked hit or a special that landed in one game only
  const int32_t phase = Phase();
  const bool fight = phase == kPhaseFight || phase == kPhaseOver;
  const uint32_t hud = phase == kPhaseFight && net::Frame() >= kHudFromFrame ? ReadU32(kFightHud) : 0;
  uint32_t v[10] = {fight ? g_last_timer : 0u,
                    fight ? uint32_t(Round()) : 0u,
                    fight ? uint32_t(RoundsWon(0)) : 0u,
                    fight ? uint32_t(RoundsWon(1)) : 0u,
                    hud ? ReadU32(hud + 0x20) : 0u,
                    hud ? ReadU32(hud + 0x24) : 0u,
                    hud ? ReadU32(hud + 0x7C) : 0u,
                    hud ? ReadU32(hud + 0x80) : 0u,
                    hud ? ReadU32(hud + 0x8C) : 0u,
                    hud ? ReadU32(hud + 0x90) : 0u};
  // tests: MK9_TEST_CHECK_DIFF=draws:<frame> or fight:<frame>: from that frame of the fight on this game's check says
  // it differs in that part, what a script that only dresses the arena or a fight gone differently look like
  static const auto test_diff = []() -> std::pair<char, uint32_t> {
    const char* e = std::getenv("MK9_TEST_CHECK_DIFF");
    if (!e || !*e) return {0, 0};
    const char* colon = std::strchr(e, ':');
    return {e[0], colon ? uint32_t(std::strtoul(colon + 1, nullptr, 10)) : 0u};
  }();
  if (test_diff.first && phase == kPhaseFight && net::Frame() >= test_diff.second) {
    if (test_diff.first == 'd') sites ^= 0x5A5A5A5Au;
    if (test_diff.first == 'f') v[4] ^= 1;
  }
  return {Fnv(reinterpret_cast<const uint8_t*>(v), sizeof v), sites};
}

void LastCheckDetail(uint32_t& timer, uint32_t& rand_state) {
  timer = g_last_timer;
  rand_state = g_last_rand;
}

std::string LastBarrierObjects() { return g_last_barrier_objects; }

std::vector<std::string> CheckDraws(uint32_t frame) {
  std::vector<std::string> lines;
  const auto it = g_check_draws.find(frame);
  if (it == g_check_draws.end()) return lines;
  // a report's line holds 200 characters, about 90 of them the log line's own start; and of the game's log a report
  // has the last 12 lines, which a desync's own lines after these fill once these are more than 4. By return address:
  // "82A23504: 824790DC=40 8247063C=8" (a script's draws: 8247061C ^ its process id << 4)
  std::string line;
  uint32_t last_lr = 0;
  for (const auto& [site, n] : it->second) {
    const uint32_t lr = uint32_t(site >> 32);
    const std::string draw = fmt::format("{:08X}={}", uint32_t(site), n);
    const size_t more = (line.empty() ? 0 : 1) + (line.empty() || lr != last_lr ? 10 : 0) + draw.size();
    if (!line.empty() && line.size() + more > 105) {
      if (lines.size() == 3) {
        line += " ...";
        break;
      }
      lines.push_back(std::move(line));
      line.clear();
    }
    if (!line.empty()) line += ' ';
    if (line.empty() || lr != last_lr) line += fmt::format("{:08X}: ", lr);
    line += draw;
    last_lr = lr;
  }
  if (!line.empty()) lines.push_back(std::move(line));
  return lines;
}

}  // namespace mk9::game

// The game seeds its generator from the clock at boot and from a value it keeps when a fight is
// set up; in a netplay match every PC takes the seed from the agreed one and the netplay frame
REX_HOOK_RAW(sub_826D3088) {
  const uint32_t caller = uint32_t(ctx.lr), asked = ctx.r3.u32;
  if (mk9::net::InSync()) {
    ctx.r3.u32 = mk9::game::Mix(mk9::game::g_agreed_seed, mk9::net::Frame());
    REXLOG_INFO("mk9 game: the game seeded its generator with {:08X} at netplay frame {} (it asked "
                "for {:08X}, from {:08X})",
                ctx.r3.u32, mk9::net::Frame(), asked, caller);
  }
  __imp__sub_826D3088(ctx, base);
  mk9::randtrace::Log('S', caller, mk9::game::GameRandState());
}

REX_HOOK_RAW(sub_826D91C0) {
  const uint64_t site = mk9::game::DrawSite(ctx);
  mk9::game::BeforeDraw(ctx);
  __imp__sub_826D91C0(ctx, base);
  mk9::randtrace::Log('G', uint32_t(site >> 32), mk9::game::GameRandState(), uint32_t(site));
}

REX_HOOK_RAW(sub_826D91E8) {
  const uint64_t site = mk9::game::DrawSite(ctx);
  mk9::game::BeforeDraw(ctx);
  __imp__sub_826D91E8(ctx, base);
  mk9::randtrace::Log('F', uint32_t(site >> 32), mk9::game::GameRandState(), uint32_t(site));
}

// A streaming level goes visible in UWorld::AddToWorld (sub_827D19E0, from the tick's UpdateLevelStreaming sub_827D5DC0),
// which keeps to 5 ms of real time a call and goes on in the next frame's; a level behind it waits. In a fight's frame 0
// the game-flow script makes the fighters' intro cinemas (SeqAct_Cinema, Subzero_CharIntro say) and sleeps a frame at a
// time until their levels are visible (sub_8247D5A0 -> sub_82A435D0 reads bIsVisible, ULevelStreaming +92), so a stall of
// 5 ms on one PC in that call (0.2 ms of work here) put all the fight's scripts a frame behind the other PC's for good.
// The game asks three things before it keeps to the limit: its replay recorder (sub_82A1C6E8) and two flags of its session
// object (sub_8239CCF8, sub_8239CD58), none of which a match here sets, being the game's local versus on each PC. So a
// match says yes to the first of them, there alone: what goes visible does so in the frame it asks, on every PC
// (docs/GAME.md, "Netplay"). MK9_TEST_ADD_LIMIT=1 keeps the limit (comparisons)
REX_EXTERN(__imp__sub_82A1C6E8);
REX_HOOK_RAW(sub_82A1C6E8) {
  constexpr uint32_t kFromAddToWorld = 0x827D1A1C;
  const bool add_to_world = uint32_t(ctx.lr) == kFromAddToWorld;
  __imp__sub_82A1C6E8(ctx, base);
  static const bool keep_limit = [] {
    const char* v = std::getenv("MK9_TEST_ADD_LIMIT");
    return v && *v == '1';
  }();
  if (add_to_world && !keep_limit && mk9::net::InSync()) ctx.r3.u64 = 1;
}

// the log says when a fight's level took longer than that limit: what would have put this PC's scripts a frame late
REX_EXTERN(__imp__sub_827D19E0);
REX_HOOK_RAW(sub_827D19E0) {
  const auto started = std::chrono::steady_clock::now();
  __imp__sub_827D19E0(ctx, base);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  if (ms > 5.0 && mk9::game::Phase() == mk9::game::kPhaseFight && mk9::net::InSync())
    REXLOG_INFO("mk9 game: a streaming level took {:.1f} ms to go visible at frame {} (the game's own limit is 5 ms)", ms,
                mk9::net::Frame());
}

// How many frames a playing sound has left (the script native sub_824A5610: sub_82A33C00, (length - position) * 60 by FMOD):
// real time again. The round's script asks 75 frames into the announcer's "Round N" and, if that is over 90, sleeps the
// difference before its "Fight!", so that a voice line started late on one PC (or a long one, read a few frames apart: two games
// read 13 and 7, 56 and 48) put the fight's scripts as many frames apart from there on. In a match no sound has frames left
// for the scripts: the next line may start over the end of a long one, on every PC alike. MK9_TEST_SOUND_LEFT=<frames> answers
// that in this game's fights instead, MK9_TEST_ADD_LIMIT=1 keeps the game's own answer too (comparisons)
REX_EXTERN(__imp__sub_82A33C00);
REX_HOOK_RAW(sub_82A33C00) {
  static const int test_left = [] {
    const char* v = std::getenv("MK9_TEST_SOUND_LEFT");
    return v && *v ? std::atoi(v) : -1;
  }();
  static const bool real = [] {
    const char* v = std::getenv("MK9_TEST_ADD_LIMIT");
    return v && *v == '1';
  }();
  if (test_left >= 0 && mk9::game::Phase() == mk9::game::kPhaseFight && mk9::net::InSync()) {
    ctx.r3.u64 = uint32_t(test_left);
    return;
  }
  if (!real && mk9::net::InSync()) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82A33C00(ctx, base);
}

// tests: MK9_TEST_SLOW_ADD=<ms>[:<times>]: in a fight's first 10 frames AddToWorld's update of a level's components
// (sub_828FD8A8) stands still that long, the first time or the first <times> times, as on a PC stalled just then
REX_EXTERN(__imp__sub_828FD8A8);
REX_HOOK_RAW(sub_828FD8A8) {
  static int stall_ms = 0, stalls = 0;
  static const bool asked = [] {
    const char* v = std::getenv("MK9_TEST_SLOW_ADD");
    if (!v || !*v) return false;
    stall_ms = std::atoi(v);
    const char* colon = std::strchr(v, ':');
    stalls = colon ? std::atoi(colon + 1) : 1;
    return true;
  }();
  if (asked && stalls > 0 && mk9::game::Phase() == mk9::game::kPhaseFight && mk9::net::InSync() && mk9::net::Frame() < 10) {
    stalls--;
    REXLOG_INFO("mk9 game: AddToWorld stands still {} ms at frame {} (MK9_TEST_SLOW_ADD)", stall_ms, mk9::net::Frame());
    std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
  }
  __imp__sub_828FD8A8(ctx, base);
}

// tests: MK9_TEST_SLOW_READ=<ms>: once a match is in sync the file loader's thread stands still that long before every
// request it takes (sub_827325A0, the file handle looked up in its loop before the read), as on a PC with a slow disc
REX_EXTERN(__imp__sub_827325A0);
REX_HOOK_RAW(sub_827325A0) {
  static const int stall_ms = [] {
    const char* v = std::getenv("MK9_TEST_SLOW_READ");
    return v && *v ? std::atoi(v) : 0;
  }();
  static int held = 0;
  if (stall_ms > 0 && mk9::net::InSync() && mk9::game::Phase() != mk9::game::kPhaseFight) {
    if (++held % 64 == 1) REXLOG_INFO("mk9 game: the file loader stands still {} ms a request (MK9_TEST_SLOW_READ), {} so far", stall_ms, held);
    std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
  }
  __imp__sub_827325A0(ctx, base);
}

// MK's counted objects (script processes among them) take their id from one counter (0x83394B44):
// the "new" lines of MK9_TRACE_SCRIPT
REX_EXTERN(__imp__sub_829DBEF0);
REX_HOOK_RAW(sub_829DBEF0) {
  const uint32_t obj = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_829DBEF0(ctx, base);
  if (mk9::net::InSync() && mk9::frame::OnMainThread()) mk9::game::NoteObjectMade(caller);
  FILE* f = mk9::game::ScriptTrace();
  if (!f || !mk9::net::InSync()) return;
  uint32_t returns[4] = {};
  const int n = mk9::game::StackReturns(ctx, returns, 4);
  std::fprintf(f, "%s%u new id %X obj %08X by %08X thread %c up", mk9::frame::Replaying() ? "r" : "",
               mk9::net::Frame(), mk9::game::ReadU32(obj + 4), obj, caller, mk9::frame::OnMainThread() ? 'm' : 'o');
  for (int i = 0; i < n; i++) std::fprintf(f, " %08X", returns[i]);
  std::fputc('\n', f);
  std::fflush(f);
}

// GuardedMain's loop reads the pads before each tick (sub_829D5230, from 0x823668C0): the "pads" lines of
// MK9_TRACE_SCRIPT, what XamInputGetState gave the game for each player there, written as net.log writes a
// frame's inputs (the same as the frame's own line there: the frame plays on its own inputs)
REX_EXTERN(__imp__sub_829D5230);
REX_HOOK_RAW(sub_829D5230) {
  const bool loop = uint32_t(ctx.lr) == 0x823668C4;
  __imp__sub_829D5230(ctx, base);
  FILE* f = mk9::game::ScriptTrace();
  if (!f || !loop || !mk9::net::InSync()) return;
  std::fprintf(f, "%s%u pads", mk9::frame::Replaying() ? "r" : "", mk9::net::Frame());
  for (uint32_t user = 0; user < 4; user++) {
    const mk9::input::Pad p = mk9::input::LastRead(user);
    if (!p.connected) continue;
    std::fprintf(f, " %04X:%02X%02X:%02X%02X%02X%02X", p.buttons, p.left_trigger, p.right_trigger, uint8_t(p.lx >> 8),
                 uint8_t(p.ly >> 8), uint8_t(p.rx >> 8), uint8_t(p.ry >> 8));
  }
  std::fputc('\n', f);
  std::fflush(f);
}
