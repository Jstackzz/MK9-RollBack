// The game's pool of effects, see fx.h.

#include "mk9/fx.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/net/netplay.h"

REX_EXTERN(__imp__sub_82606510);  // takes a free effect off the manager's list (r3 manager, r4 template, r5 owner)
REX_EXTERN(__imp__sub_82606650);  // releases an effect (r3 manager, r4 effect)
REX_EXTERN(sub_82606650);

namespace mk9::fx {
namespace {

// the manager's lists (UE3 arrays: data, count, capacity)
constexpr uint32_t kActive = 640;  // the effects in use, oldest first
constexpr uint32_t kFree = 664;    // the ones ready to be taken
constexpr uint32_t kKeep = 676;    // how many released ones the free list keeps (64)

const bool g_trace = [] {
  const char* v = std::getenv("MK9_FX_TRACE");
  return v && *v == '1';
}();
// tests: MK9_FX_TEST_RECLAIM=<n> makes room as if the pool were full whenever fewer than n are free
const uint32_t g_test_reclaim = [] {
  const char* v = std::getenv("MK9_FX_TEST_RECLAIM");
  return v && *v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
}();

std::atomic<uint64_t> g_taken{0}, g_released{0}, g_returned{0}, g_reclaimed{0}, g_empty{0};
std::atomic<uint64_t> g_empty_logged{0};
std::atomic<uint32_t> g_last_logged_free{0xFFFFFFFFu};

uint32_t Read(uint32_t a) { return game::ReadU32(a); }
uint32_t FreeCount(uint32_t manager) { return Read(manager + kFree + 4); }

// the oldest effect in use released, through the game's own release
void ReleaseOldest(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  const uint32_t active = Read(manager + kActive), count = Read(manager + kActive + 4);
  if (!active || !count) return;
  ctx.r3.u64 = manager;
  ctx.r4.u64 = Read(active);
  sub_82606650(ctx, base);
  g_reclaimed++;
}

}  // namespace

Stats GetStats() {
  return {g_taken.load(), g_released.load(), g_returned.load(), g_reclaimed.load(), g_empty.load()};
}

}  // namespace mk9::fx

// what the release does with an effect: back on the free list or not (counted for MK9_FX_TRACE)
REX_HOOK_RAW(sub_82606650) {
  using namespace mk9::fx;
  const uint32_t manager = ctx.r3.u32;
  const uint32_t free_before = manager ? FreeCount(manager) : 0;
  __imp__sub_82606650(ctx, base);
  g_released++;
  if (manager && FreeCount(manager) > free_before) g_returned++;
}

// With every effect in use the spawn found none: the oldest in use makes room, through the game's own
// release, before the spawn gives up. On the main thread only: the job workers that take effects as
// well run beside each other, and a release there could meet another worker's take.
REX_HOOK_RAW(sub_82606510) {
  using namespace mk9::fx;
  const uint32_t manager = ctx.r3.u32, tmpl = ctx.r4.u32, owner = ctx.r5.u32;
  const uint32_t free_before = manager ? FreeCount(manager) : 0;
  if (g_test_reclaim && manager && tmpl && free_before < g_test_reclaim && mk9::frame::OnMainThread()) {
    ReleaseOldest(ctx, base, manager);
    ctx.r3.u64 = manager;
    ctx.r4.u64 = tmpl;
    ctx.r5.u64 = owner;
  }
  __imp__sub_82606510(ctx, base);
  const uint64_t n = ++g_taken;
  if (ctx.r3.u32 == 0 && manager && tmpl) {
    const uint64_t empty = ++g_empty;
    if (mk9::frame::OnMainThread()) {
      for (int tries = 0; tries < 8 && FreeCount(manager) == 0 && Read(manager + kActive + 4); tries++)
        ReleaseOldest(ctx, base, manager);
      ctx.r3.u64 = manager;
      ctx.r4.u64 = tmpl;
      ctx.r5.u64 = owner;
      __imp__sub_82606510(ctx, base);
    }
    // the first time and every hundredth: the end of the game's log, which a crash report quotes
    if (empty == 1 || empty - g_empty_logged.load() >= 100) {
      g_empty_logged = empty;
      REXLOG_WARN("mk9 fx: every effect in use ({} kept): {} (frame {}, {} times so far, {} reclaimed)",
                  Read(manager + kKeep), ctx.r3.u32 ? "the oldest made room" : "none could be freed",
                  mk9::frame::Count(), empty, g_reclaimed.load());
    }
  }
  if (!g_trace) return;
  const uint32_t last = g_last_logged_free.load();
  if (last == 0xFFFFFFFFu || free_before + 16 <= last || last + 16 <= free_before || n % 500 == 0) {
    g_last_logged_free = free_before;
    REXLOG_INFO("mk9 fx: manager {:08X}, {} free before take {} (frame {}, netplay frame {}, {} rollbacks); "
                "{} released, {} back on the free list, {} made room",
                manager, free_before, n, mk9::frame::Count(), mk9::net::Frame(), mk9::net::GetStats().rollbacks,
                g_released.load(), g_returned.load(), g_reclaimed.load());
  }
}
