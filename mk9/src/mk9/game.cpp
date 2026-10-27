// MK9's state, see game.h.

#include "mk9/game.h"

#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9/net/netplay.h"

REX_EXTERN(sub_82B29580);  // the C runtime's _getptd

namespace mk9::game {
namespace {

constexpr uint32_t kRoundTimer = 0x833CED24;
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

void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed) {
  const uint32_t before = MainRandState(ctx, base);
  SetMainRandState(ctx, base, agreed.seed);
  REXLOG_INFO("mk9 game: agreed seed {:08X} (rand state was {:08X})", agreed.seed, before);
}

uint32_t StateHash(PPCContext& ctx, uint8_t* base) {
  // only values that do not depend on where this process's heaps put things (the script globals
  // hold pointers): the round timer and the main thread's rand() so far; the fighters' state
  // joins them once it is found
  uint32_t v[2] = {ReadU32(kRoundTimer), MainRandState(ctx, base)};
  return Fnv(reinterpret_cast<const uint8_t*>(v), sizeof v);
}

}  // namespace mk9::game
