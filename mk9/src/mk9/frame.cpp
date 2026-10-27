// The frame boundary, see frame.h.

#include "mk9/frame.h"
#include "mk9/diag.h"
#include "mk9/game.h"
#include "mk9/snap.h"

namespace mk9::fibers {
uint64_t Switches();
}

#include <atomic>
#include <chrono>

#include <rex/hook.h>
#include <rex/logging.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mk9::frame {
namespace {

std::atomic<uint64_t> g_frames{0};
std::atomic<uint32_t> g_main_tid{0};
Hook g_top = nullptr;
Hook g_end = nullptr;

uint32_t CurrentTid() {
#if defined(_WIN32)
  return GetCurrentThreadId();
#else
  return 0;
#endif
}

}  // namespace

uint64_t Count() { return g_frames.load(std::memory_order_relaxed); }
uint32_t MainThreadId() { return g_main_tid.load(std::memory_order_relaxed); }
bool OnMainThread() { return CurrentTid() == MainThreadId(); }
void SetTopHook(Hook hook) { g_top = hook; }
void SetEndHook(Hook hook) { g_end = hook; }

}  // namespace mk9::frame

// FEngineLoop::Tick, called once per frame from GuardedMain's loop (0x823668E8).
REX_EXTERN(__imp__sub_823621F8);
REX_HOOK_RAW(sub_823621F8) {
  using namespace mk9::frame;
  if (g_main_tid.load(std::memory_order_relaxed) == 0) {
    g_main_tid.store(CurrentTid());
    REXLOG_INFO("mk9: first frame on host thread {}", CurrentTid());
  }
  if (g_top) g_top(ctx, base);
  __imp__sub_823621F8(ctx, base);
  const uint64_t n = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (g_end) g_end(ctx, base);
  mk9::snap::OnFrame(n);
  mk9::diag::OnFrame(n);

  // the frame rate, every ten seconds of frames
  static auto since = std::chrono::steady_clock::now();
  static uint64_t since_frames = 0;
  if (n - since_frames >= 600) {
    auto now = std::chrono::steady_clock::now();
    double s = std::chrono::duration<double>(now - since).count();
    REXLOG_INFO("mk9: frame {}: {:.1f} frames/s, timer {}, {} fiber switches", n,
                double(n - since_frames) / s, mk9::game::RoundTimer(), mk9::fibers::Switches());
    since = now;
    since_frames = n;
  }
}
