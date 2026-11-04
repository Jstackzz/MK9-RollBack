// MK9's state, see game.h.

#include "mk9/game.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
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
// made after it, and for those made before it as long as what one PC made more came first. The site
// shows it as 8247061C ^ ((id - base) << 4) (82A23504/824790DC: a fighter's own script 0x96C after
// the base). Netplay's "match N in sync" line names the base (next object id)
constexpr uint32_t kScriptRandomCaller = 0x8247061C;
constexpr uint32_t kScriptContext = 0x8342CDF0;
uint32_t g_id_base = 0;

uint64_t DrawSite(const PPCContext& ctx) {
  uint32_t up = 0;
  StackReturns(ctx, &up, 1);
  if (up == kScriptRandomCaller) {
    const uint32_t context = ReadU32(kScriptContext);
    if (context) up ^= (ReadU32(context + 4) - g_id_base) << 4;
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

void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed) {
  const uint32_t before = GameRandState(), crt_before = MainRandState(ctx, base);
  g_agreed_seed = agreed.seed;
  g_id_base = NextObjectId();  // script processes count from here (DrawSite)
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

// MK's counted objects (script processes among them) take their id from one counter (0x83394B44):
// the "new" lines of MK9_TRACE_SCRIPT
REX_EXTERN(__imp__sub_829DBEF0);
REX_HOOK_RAW(sub_829DBEF0) {
  const uint32_t obj = ctx.r3.u32, caller = uint32_t(ctx.lr);
  __imp__sub_829DBEF0(ctx, base);
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
