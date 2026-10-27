// MK9's state, see game.h.

#include "mk9/game.h"

#include <cstring>
#include <deque>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9/frame.h"
#include "mk9/net/netplay.h"
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

// Outside fights a draw must not depend on when it happens: loading takes longer on one PC than
// another, and what draws every frame (sub_82382E40) then draws more often on one before, say,
// the arena variant is picked. So there each call site draws from a stream of its own: the agreed
// seed, the site, and how many times it drew since the barrier. Fights draw from the game's one
// generator (every frame of a fight runs the same everywhere). The journal lets a rollback take
// back the counts of the frames it puts back.
std::unordered_map<uint64_t, uint32_t> g_site_draws;
struct SiteDraw {
  uint32_t frame;
  uint64_t site;
};
std::deque<SiteDraw> g_site_journal;
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

int32_t RoundTimer() { return int32_t(ReadU32(kRoundTimer)); }
int32_t Phase() { return int32_t(ReadU32(0x833C6C58)); }
uint32_t UiScreen() { return ReadU32(0x833AEA80); }
bool TitleUp() { return ReadU32(0x833C77B8) == 1; }
int RoundsWon(int player) { return int(ReadU32(0x833C7A38 + 4 * uint32_t(player & 1))); }
int Round() { return int(ReadU32(0x833CED28)); }

std::vector<uint32_t> ReadyWords() { return {uint32_t(RoundTimer())}; }

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
uint64_t DrawSite(const PPCContext& ctx) {
  const uint32_t back = ReadU32(ctx.r1.u32);
  const uint32_t up = back ? ReadU32(back - 8) : 0;
  return (uint64_t(uint32_t(ctx.lr)) << 32) | up;
}

void BeforeDraw(PPCContext& ctx) {
  if (!net::InSync() || Phase() == kPhaseFight || !frame::OnMainThread()) return;
  const uint64_t site = DrawSite(ctx);
  const uint32_t n = g_site_draws[site]++;
  g_site_journal.push_back({net::Frame(), site});
  if (g_site_journal.size() > 4096) g_site_journal.pop_front();
  SetGameRandState(Mix(g_agreed_seed ^ Mix(uint32_t(site), uint32_t(site >> 32)), n));
}

}  // namespace

void RolledBack(uint32_t frame) {
  while (!g_site_journal.empty() && g_site_journal.back().frame >= frame) {
    auto it = g_site_draws.find(g_site_journal.back().site);
    if (it != g_site_draws.end() && it->second) it->second--;
    g_site_journal.pop_back();
  }
}

void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed) {
  const uint32_t before = GameRandState(), crt_before = MainRandState(ctx, base);
  g_agreed_seed = agreed.seed;
  g_site_draws.clear();
  g_site_journal.clear();
  SetGameRandState(agreed.seed);
  SetMainRandState(ctx, base, Mix(agreed.seed, 0xC0FFEEu));
  REXLOG_INFO("mk9 game: agreed seed {:08X} (the game's generator was {:08X}, rand() {:08X})",
              agreed.seed, before, crt_before);
}

bool SyncPoint() {
  static int32_t last = -1;
  const int32_t now = Phase();
  const bool begins = now == kPhaseFight && last != kPhaseFight && last != -1;
  last = now;
  if (begins) REXLOG_INFO("mk9 game: a fight begins (round timer {})", RoundTimer());
  return begins;
}

uint32_t StateHash(PPCContext& ctx, uint8_t* base) {
  // only values that do not depend on where this process's heaps put things (the script globals
  // hold pointers): the round timer so far; the fighters' state joins it once it is found. The
  // game's random number generator is logged beside it (LastCheckDetail)
  (void)ctx, (void)base;
  g_last_rand = GameRandState();
  g_last_timer = ReadU32(kRoundTimer);
  uint32_t v[1] = {g_last_timer};
  return Fnv(reinterpret_cast<const uint8_t*>(v), sizeof v);
}

void LastCheckDetail(uint32_t& timer, uint32_t& rand_state) {
  timer = g_last_timer;
  rand_state = g_last_rand;
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
  const uint32_t caller = uint32_t(ctx.lr);
  mk9::game::BeforeDraw(ctx);
  __imp__sub_826D91C0(ctx, base);
  mk9::randtrace::Log('G', caller, mk9::game::GameRandState());
}

REX_HOOK_RAW(sub_826D91E8) {
  const uint32_t caller = uint32_t(ctx.lr);
  mk9::game::BeforeDraw(ctx);
  __imp__sub_826D91E8(ctx, base);
  mk9::randtrace::Log('F', caller, mk9::game::GameRandState());
}
