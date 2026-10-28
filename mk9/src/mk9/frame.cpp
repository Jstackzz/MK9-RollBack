// The frame boundary, see frame.h.

#include "mk9/frame.h"
#include "mk9/crash.h"
#include "mk9/diag.h"
#include "mk9/game.h"
#include "mk9/gmalloc.h"
#include "mk9/link.h"
#include "mk9/snap.h"

namespace mk9::fibers {
uint64_t Switches();
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

// FEngineLoop::Tick, called once per frame from GuardedMain's loop (0x823668E8)
REX_EXTERN(__imp__sub_823621F8);

namespace mk9::frame {
namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_main_tid{0};
Hook g_top = nullptr;
Hook g_end = nullptr;
std::atomic<bool> g_replaying{false};

uint32_t CurrentTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

// The cap (frame.h): a frame starts at least a period after the one before. One up to kCapSlack
// late lets the next make that up; one later than that starts the count again from itself, so no
// frames run back to back after a stall (netplay holds frames back on purpose to keep the sides
// even, and a frame waiting for the other PC's inputs must not be made up for either).
using Steady = std::chrono::steady_clock;
constexpr auto kCapSlack = std::chrono::milliseconds(2);
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

void Cap() {
  if (g_cap_period == Steady::duration::zero()) return;
  auto now = Steady::now();
  if (now < g_cap_next) {
    WaitUntil(g_cap_next);
    now = Steady::now();
  }
  g_cap_next = std::max(g_cap_next, now - kCapSlack) + g_cap_period;
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

void RunAgain(PPCContext& ctx, uint8_t* base) {
  g_replaying.store(true, std::memory_order_relaxed);
  __imp__sub_823621F8(ctx, base);
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (g_end) g_end(ctx, base);
  g_replaying.store(false, std::memory_order_relaxed);
}

}  // namespace mk9::frame

REX_HOOK_RAW(sub_823621F8) {
  using namespace mk9::frame;
  if (g_main_tid.load(std::memory_order_relaxed) == 0) {
    g_main_tid.store(CurrentTid());
    REXLOG_INFO("mk9: first frame on host thread {}", CurrentTid());
    InitCap();
    // UE3's GMalloc (0x833D9074): its object and vtable, for the allocator hooks (mk9/simheap)
    const uint32_t gmalloc = mk9::game::ReadU32(0x833D9074);
    const uint32_t vt = gmalloc ? mk9::game::ReadU32(gmalloc) : 0;
    std::string entries;
    for (int i = 0; vt && i < 12; i++) entries += fmt::format(" +{}={:08X}", i * 4, mk9::game::ReadU32(vt + 4 * i));
    REXLOG_INFO("mk9: GMalloc {:08X} vtable {:08X}{}", gmalloc, vt, entries);
  }
  mk9::crash::OnFrame(g_frames.load(std::memory_order_relaxed) + 1);
  // the cap first: netplay then reads the pads and the network as late as it can before the frame
  Cap();
  g_pacing.Top();
  mk9::gmalloc::OnFrameTop(ctx, base);
  if (g_top) g_top(ctx, base);
  __imp__sub_823621F8(ctx, base);
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_end) g_end(ctx, base);
  mk9::snap::OnFrame(n);
  mk9::diag::OnFrame(n);
  mk9::link::OnFrame(n);

  // the frame rate, every ten seconds of frames
  static auto since = std::chrono::steady_clock::now();
  static uint64_t since_frames = 0;
  if (n - since_frames >= 600) {
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - since).count();
    REXLOG_INFO("mk9: frame {}: {:.1f} frames/s ({} under 15 ms, {} over 20 ms, longest {:.1f} ms), timer {}, "
                "{} fiber switches",
                n, double(n - since_frames) / s, g_pacing.early, g_pacing.late,
                std::chrono::duration<double, std::milli>(g_pacing.longest).count(), mk9::game::RoundTimer(),
                mk9::fibers::Switches());
    since = now;
    since_frames = n;
    g_pacing.early = g_pacing.late = 0;
    g_pacing.longest = {};
  }
}
