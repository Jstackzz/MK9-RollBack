// The frame boundary, see frame.h.

#include "mk9/frame.h"
#include "mk9/crash.h"
#include "mk9/diag.h"
#include "mk9/fx.h"
#include "mk9/game.h"
#include "mk9/gmalloc.h"
#include "mk9/link.h"
#include "mk9/names.h"
#include "mk9/report.h"
#include "mk9/snap.h"
#include "mk9/sound.h"

namespace mk9::fibers {
uint64_t Switches();
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/ui/presenter.h>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

// FEngineLoop::Tick, called once per frame from GuardedMain's loop (0x823668D4)
REX_EXTERN(__imp__sub_823621F8);
// reads the time base into [r3]: called from all over, and first thing each time round GuardedMain's
// loop (from 0x82366864), the top of a frame
REX_EXTERN(__imp__sub_82366BF8);
// what else GuardedMain's loop runs each time round (BeforeTick, AfterTick)
REX_EXTERN(sub_82360F10);
REX_EXTERN(sub_8270A038);  // appSprintf(buffer, size, format, ...)
REX_EXTERN(sub_829B0430);
REX_EXTERN(sub_829D5230);  // reads the pads (XamInputGetState) into the game's pad state
REX_EXTERN(sub_829D7748);  // system notifications, sign-in changes
REX_EXTERN(sub_8239D550);
REX_EXTERN(sub_829D49F0);  // reads key presses (XamInputGetKeystrokeEx)

namespace mk9::frame {
namespace {

constexpr uint32_t kLoopTop = 0x82366868;  // the return address of the loop's first call

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_main_tid{0};
Hook g_top = nullptr;
Hook g_end = nullptr;
std::atomic<bool> g_replaying{false};
std::atomic<int64_t> g_top_ns{0};  // when the last frame started (steady clock, ns): the stall sampler reads it
std::atomic<bool> g_in_cap_wait{false};

uint32_t CurrentTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

// The cap (frame.h): a frame starts at least a period after the one before, counted from when that one was due. A frame
// that comes late by up to kCapSlack lets the next ones make that up: they start as soon as the game is done with the
// one before, close together, until the cap has caught up with the clock (the frames wait for the GPU's blank anyway,
// but the rendering thread and the GPU are a frame or two behind the main thread, which can run ahead into that room).
// Until 0.5.10 the slack was 2 ms: a PC that lost 20 ms to a rollback or a stall lost them for good, was a frame or two
// behind from then on, and the other PC held frames back to wait for it (more than half of the late frames in the
// server's match log of 0.5.10's first day were those holds, on the PC that had no hiccup). The price is that the
// screen shows the frames of a PC that came late up to the slack later than it did, for as long as nothing empties that
// room (test fights, one PC stalling 40 ms every 4 s: +0.5 frames with 12 ms, +0.8 with 17, +1.3 with 25; 17 is a frame
// that missed a whole blank, 16.7 ms, the usual late frame). A frame later than the
// slack starts the count again from itself, so no frames run back to back after a long stall. Netplay holds frames
// back on purpose (a hold counts from now) and a frame that waited for the other PC's inputs, or was not a fight's, is
// not made up for either (DropCatchUp).
using Steady = std::chrono::steady_clock;
constexpr auto kCapLate = std::chrono::milliseconds(2);   // late by less than this: the cap of 0.5.10 and before did not mind
constexpr auto kCapSlack = std::chrono::milliseconds(17);
Steady::duration g_cap_slack = kCapSlack;  // MK9_CAP_SLACK_MS=<ms>; 2 is the cap of 0.5.10 and before
// for the frame rate line: frames that came late by more than kCapLate, and by how much the cap trailed the clock at most
uint32_t g_late_frames = 0;
double g_trail_max_ms = 0;
Steady::duration g_cap_period{};
Steady::time_point g_cap_next{};
#if defined(_WIN32)
HANDLE g_cap_timer = nullptr;
#endif

void InitCap() {
  double fps = 60.0;
  if (const char* v = std::getenv("MK9_FRAME_CAP"); v && *v) fps = std::atof(v);
  if (fps <= 0) {
    REXLOG_INFO("mk9: frames not capped (MK9_FRAME_CAP)");
    return;
  }
  // a few microseconds under 1/fps: at 60 the GPU's vertical blank (833,333 ticks of the 50 MHz
  // guest clock, 16.66666 ms) paces every frame the game draws and waits for, and those must never
  // wait here as well; what the cap holds back are the frames that do not wait for one (loading
  // screens and menus ran at up to 69 a second)
  g_cap_period = std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double>(1.0 / fps)) -
                 std::chrono::microseconds(3);
  if (const char* v = std::getenv("MK9_CAP_SLACK_MS"); v && *v)
    g_cap_slack = std::chrono::duration_cast<Steady::duration>(
        std::chrono::duration<double, std::milli>(std::clamp(std::atof(v), 0.0, 100.0)));
#if defined(_WIN32)
  // Windows' sleeps in 1 ms steps rather than 15.6 ms ones: the SDK's vertical blank thread sleeps
  // 1 ms at a time between blanks, which the default turned into uneven bursts
  timeBeginPeriod(1);
  g_cap_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
  REXLOG_INFO("mk9: frames capped at {} a second", fps);
}

void WaitUntil(Steady::time_point t) {
  for (;;) {
    const auto left = t - Steady::now();
    if (left <= Steady::duration::zero()) return;
#if defined(_WIN32)
    // a timer for all but the last half millisecond (it may wake that much late), then yield
    if (left > std::chrono::microseconds(1000)) {
      const auto nap = std::chrono::duration_cast<std::chrono::nanoseconds>(left - std::chrono::microseconds(500));
      LARGE_INTEGER due;
      due.QuadPart = -std::max<int64_t>(1, nap.count() / 100);
      if (g_cap_timer && SetWaitableTimerEx(g_cap_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(g_cap_timer, INFINITE);
      } else {
        Sleep(DWORD(std::chrono::duration_cast<std::chrono::milliseconds>(nap).count()));
      }
      continue;
    }
    SwitchToThread();
#else
    std::this_thread::yield();
#endif
  }
}

// frames held back (HoldNext): the next frame starts that many periods later than it would. Not a sleep in the frame:
// the cap starts a late frame's next one as if it had been on time, so a sleep only took away what the frame waited
// here anyway (a frame of 6 ms of work slept 16.7 and lost 5: a game held back every other frame still ran 52 a second)
int g_hold = 0;
bool g_drop_catch_up = false;
CapInfo g_cap_last;

void Cap() {
  if (g_cap_period == Steady::duration::zero()) {
    g_cap_last = {};
    g_cap_last.hold = g_hold;
    if (g_hold) {
      g_in_cap_wait.store(true, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::microseconds(16667) * g_hold);
      g_in_cap_wait.store(false, std::memory_order_relaxed);
    }
    g_hold = 0;
    return;
  }
  auto now = Steady::now();
  g_cap_last = {};
  g_cap_last.hold = g_hold;
  if (g_drop_catch_up) {
    g_cap_next = std::max(g_cap_next, now);  // what the cap trails the clock by is forgotten
    g_drop_catch_up = false;
  }
  if (g_hold) {
    // a hold counts from now (the cap may trail the clock by the slack)
    g_cap_next = std::max(g_cap_next, now - kCapLate) + g_cap_period * g_hold;
    g_hold = 0;
  }
  if (now < g_cap_next) {
    g_in_cap_wait.store(true, std::memory_order_relaxed);
    WaitUntil(g_cap_next);
    g_in_cap_wait.store(false, std::memory_order_relaxed);
    const auto after = Steady::now();
    g_cap_last.slept_ms = std::chrono::duration<double, std::milli>(after - now).count();
    now = after;
  }
  if (g_cap_next.time_since_epoch().count() != 0 && now - g_cap_next > kCapLate) {  // not the first frame's
    g_late_frames++;
    g_trail_max_ms = std::max(g_trail_max_ms, std::chrono::duration<double, std::milli>(now - g_cap_next).count());
  }
  g_cap_next = std::max(g_cap_next, now - g_cap_slack) + g_cap_period;
}

// tests: MK9_TEST_HICCUP=<every ms>:<busy ms>[:<from frame>] keeps this game's main thread busy that long now and
// then in a fight (every time a random 0.7-1.3 of the interval after the one before), as a PC that stalls
void Hiccup() {
  struct Config {
    double every_ms = 0, busy_ms = 0;
    uint64_t from = 0;
  };
  static const Config cfg = [] {
    Config c;
    if (const char* v = std::getenv("MK9_TEST_HICCUP"); v && *v) {
      double e = 0, b = 0;
      unsigned long long f = 0;
      if (std::sscanf(v, "%lf:%lf:%llu", &e, &b, &f) >= 2) {
        c.every_ms = e;
        c.busy_ms = b;
        c.from = f;
      }
    }
    return c;
  }();
  if (cfg.every_ms <= 0 || game::Phase() != game::kPhaseFight || g_frames.load() < cfg.from) return;
  static Steady::time_point next{};
  static uint32_t rng = 12345;
  const auto now = Steady::now();
  if (next == Steady::time_point{}) {
    next = now + std::chrono::milliseconds(3000);
    return;
  }
  if (now < next) return;
  rng = rng * 1664525u + 1013904223u;
  const double k = 0.7 + 0.6 * double(rng >> 8) / double(1u << 24);
  next = now + std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double, std::milli>(cfg.every_ms * k));
  const auto until = now + std::chrono::duration_cast<Steady::duration>(std::chrono::duration<double, std::milli>(cfg.busy_ms));
  while (Steady::now() < until) {
  }
}

// how evenly the frames came, for the frame rate line: frames that started under 15 ms or over
// 20 ms after the one before, and the longest gap
struct Pacing {
  Steady::time_point last{};
  uint32_t early = 0, late = 0;
  Steady::duration longest{};

  void Top() {
    const auto now = Steady::now();
    if (last != Steady::time_point{}) {
      const auto gap = now - last;
      if (gap < std::chrono::milliseconds(15)) early++;
      if (gap > std::chrono::milliseconds(20)) late++;
      longest = std::max(longest, gap);
    }
    last = now;
  }
};
Pacing g_pacing;

}  // namespace

uint64_t Count() { return g_frames.load(std::memory_order_relaxed); }
void SetCount(uint64_t n) { g_frames.store(n, std::memory_order_relaxed); }
uint32_t MainThreadId() { return g_main_tid.load(std::memory_order_relaxed); }
bool OnMainThread() { return CurrentTid() == MainThreadId(); }
void SetTopHook(Hook hook) { g_top = hook; }
void SetEndHook(Hook hook) { g_end = hook; }
bool Replaying() { return g_replaying.load(std::memory_order_relaxed); }
void HoldNext(int periods) { g_hold = std::max(g_hold, periods); }
CapInfo LastCap() { return g_cap_last; }
void DropCatchUp() { g_drop_catch_up = true; }
int64_t LastTopNs() { return g_top_ns.load(std::memory_order_relaxed); }
bool InCapWait() { return g_in_cap_wait.load(std::memory_order_relaxed); }

namespace {

// GuardedMain's loop (sub_823667E8, 0x82366860-0x82366920) runs more than the engine's tick each time
// round, and a frame run again must run all of it, or it is not the frame that ran first: before the
// tick it counts the frame, reads the pads and the key presses (the tick only reads what that put in
// the game's pad state) and updates the game's systems, and after it it measures the frame and takes
// a flag another thread sets. These are those two parts, with GuardedMain's registers (ctx at the top
// of a frame holds them: r21-r31 its constants, r1 its frame; r30, the time base when the loop came
// round, is read again first) and the return addresses its calls have. Until 2026-10-28 a frame run
// again ran the tick alone: a rollback from frame n to r played frames r+1..n on the pads read before
// r, and the inputs that came in for them took effect only after n.

// from the loop's top (0x82366860) to the tick's call
void BeforeTick(PPCContext& ctx, uint8_t* base) {
  using game::ReadU32;
  // the time base, the frame counted
  ctx.r3.u64 = ctx.r1.u32 + 80;
  ctx.lr = kLoopTop;
  __imp__sub_82366BF8(ctx, base);
  ctx.r30.u64 = ReadU32(ctx.r1.u32 + 84);
  const uint32_t counters = ReadU32(ctx.r31.u32 + 18644);
  game::WriteU32(counters + 108, ReadU32(counters + 108) + 1);
  ctx.r3.u64 = 12;
  ctx.r4.u64 = ctx.r26.u64;
  ctx.r5.u64 = ctx.r25.u64;
  ctx.r6.u64 = ReadU32(ReadU32(ctx.r31.u32 + 18644) + 108);
  ctx.lr = 0x82366898;
  sub_8270A038(ctx, base);
  ctx.lr = 0x8236689C;
  sub_829B0430(ctx, base);
  if (ctx.r3.u32 == 0) {
    const uint32_t c = ReadU32(ctx.r31.u32 + 18644);
    game::WriteU32(c + 112, ReadU32(c + 112) + 1);
  }
  // the pads and the key presses the tick plays on
  if (ReadU32(ctx.r21.u32 - 10956) != 0) {
    ctx.lr = 0x823668C4;
    sub_829D5230(ctx, base);
    ctx.lr = 0x823668C8;
    sub_829D7748(ctx, base);
  }
  ctx.lr = 0x823668CC;
  sub_8239D550(ctx, base);
  ctx.lr = 0x823668D0;
  sub_829D49F0(ctx, base);
  ctx.r3.u64 = ctx.r24.u64;  // the tick's argument
}

// from the tick's return (0x823668D8) to the loop's end
void AfterTick(PPCContext& ctx, uint8_t* base) {
  using game::ReadU32;
  // the frame's length in time base ticks
  ctx.r3.u64 = ctx.r1.u32 + 88;
  ctx.lr = 0x823668E0;
  __imp__sub_82366BF8(ctx, base);
  game::WriteU32(ctx.r28.u32 - 10916, ReadU32(ctx.r1.u32 + 92) - ctx.r30.u32);
  // a word another thread sets, taken and cleared at once (lwarx/stwcx. in the guest)
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint32_t taken = uint32_t(InterlockedExchange(mem->TranslateVirtual<volatile LONG*>(ctx.r29.u32), 0));
  game::WriteU32(ctx.r27.u32 - 10912, __builtin_bswap32(taken));
  ctx.lr = 0x82366918;
  sub_82360F10(ctx, base);
}

// the top of a frame, before GuardedMain's loop runs anything of it
void Top(PPCContext& ctx, uint8_t* base) {
  if (g_main_tid.load(std::memory_order_relaxed) == 0) {
    g_main_tid.store(CurrentTid());
    REXLOG_INFO("mk9: first frame on host thread {}", CurrentTid());
    InitCap();
    // UE3's GMalloc (0x833D9074): its object and vtable, for the allocator hooks (mk9/simheap)
    const uint32_t gmalloc = game::ReadU32(0x833D9074);
    const uint32_t vt = gmalloc ? game::ReadU32(gmalloc) : 0;
    std::string entries;
    for (int i = 0; vt && i < 12; i++) entries += fmt::format(" +{}={:08X}", i * 4, game::ReadU32(vt + 4 * i));
    REXLOG_INFO("mk9: GMalloc {:08X} vtable {:08X}{}", gmalloc, vt, entries);
  }
  crash::OnFrame(g_frames.load(std::memory_order_relaxed) + 1);
  // the cap first: netplay then reads the pads and the network as late as it can before the frame
  Cap();
  Hiccup();
  g_top_ns.store(Steady::now().time_since_epoch().count(), std::memory_order_relaxed);
  g_pacing.Top();
  gmalloc::OnFrameTop(ctx, base);
  sound::OnFrameTop(ctx, base);
  if (g_top) g_top(ctx, base);
}

}  // namespace

void RunAgain(PPCContext& ctx, uint8_t* base) {
  g_replaying.store(true, std::memory_order_relaxed);
  sound::OnRunAgain(ctx, base);
  BeforeTick(ctx, base);
  ctx.lr = 0x823668D8;  // the tick's return address, as GuardedMain calls it (0x823668D4)
  __imp__sub_823621F8(ctx, base);
  sound::OnFrameEnd(ctx, base);
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (g_end) g_end(ctx, base);
  AfterTick(ctx, base);
  g_replaying.store(false, std::memory_order_relaxed);
}

}  // namespace mk9::frame

// the top of GuardedMain's loop, before it reads the pads: the top of a frame (a frame run again reads
// the time base itself)
REX_HOOK_RAW(sub_82366BF8) {
  if (uint32_t(ctx.lr) == mk9::frame::kLoopTop) mk9::frame::Top(ctx, base);
  __imp__sub_82366BF8(ctx, base);
}

REX_HOOK_RAW(sub_823621F8) {
  using namespace mk9::frame;
  __imp__sub_823621F8(ctx, base);
  mk9::sound::OnFrameEnd(ctx, base);
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_end) g_end(ctx, base);
  mk9::snap::OnFrame(n);
  mk9::diag::OnFrame(n);
  mk9::link::OnFrame(n);
  mk9::names::OnFrame(n);
  mk9::report::OnFrame(n);

  // the frame rate, every ten seconds of frames, and how often the window was presented meanwhile (what a
  // frame counter overlay shows: once a frame unless something repaints the window on its own)
  static auto since = std::chrono::steady_clock::now();
  static uint64_t since_frames = 0, since_presents = 0;
  if (n - since_frames >= 600) {
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - since).count();
    const mk9::fx::Stats fx = mk9::fx::GetStats();
    const uint64_t presents = rex::ui::Presenter::PresentCount();
    // and how long a frame the game finished waited until the window was presented with it
    uint64_t shown = 0;
    double waited_ms = 0, longest_wait_ms = 0;
    rex::ui::Presenter::TakePresentWaits(shown, waited_ms, longest_wait_ms);
    // and what the screen got meanwhile: the gaps between successive guest frames' presents
    static uint64_t gaps_before = 0, over25_before = 0, over50_before = 0;
    uint64_t gaps = 0, over25 = 0, over50 = 0;
    double longest_gap_ms = 0;
    rex::ui::Presenter::ShownGaps(gaps, over25, over50, longest_gap_ms);
    REXLOG_INFO("mk9: frame {}: {:.1f} frames/s ({} under 15 ms, {} over 20 ms, longest {:.1f} ms), {:.1f} presents/s "
                "(a frame shown {:.2f} ms after it was ready, at most {:.1f}), screen: {} gaps between frames over 25 "
                "ms, {} over 50 (longest so far {:.0f} ms), timer {}, {} fiber switches, {} effects "
                "taken ({} with every one in use, {} made room)",
                n, double(n - since_frames) / s, g_pacing.early, g_pacing.late,
                std::chrono::duration<double, std::milli>(g_pacing.longest).count(),
                double(presents - since_presents) / s, shown ? waited_ms / double(shown) : 0.0, longest_wait_ms,
                over25 - over25_before, over50 - over50_before, longest_gap_ms,
                mk9::game::RoundTimer(), mk9::fibers::Switches(), fx.taken, fx.empty, fx.reclaimed);
    gaps_before = gaps;
    over25_before = over25;
    over50_before = over50;
    if (g_late_frames)
      REXLOG_INFO("mk9: the frame cap: {} frames came more than 2 ms late (the cap trailed the clock by {:.0f} ms at most, "
                  "of the {:.0f} ms it makes up)",
                  g_late_frames, g_trail_max_ms, std::chrono::duration<double, std::milli>(g_cap_slack).count());
    g_late_frames = 0;
    g_trail_max_ms = 0;
    since = now;
    since_frames = n;
    since_presents = presents;
    g_pacing.early = g_pacing.late = 0;
    g_pacing.longest = {};
  }
}
