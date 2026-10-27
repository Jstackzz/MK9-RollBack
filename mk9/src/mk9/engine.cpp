// UE3's threads as rollback sees them, and engine switches.
//
// The rendering thread (RenderingThreadMain, sub_82740FE8) runs the commands the main thread
// queues in GRenderCommandBuffer, an FRingBuffer at 0x8341E4E0: +0 the data, +4 its end, +8 the
// write pointer (the main thread), +20 the read pointer (the rendering thread, which moves it on
// once a command ran). It lags up to a frame behind the main thread.
//
// MK9_ONETHREAD=1: no rendering thread, as with the engine's own -ONETHREAD (FEngineLoop::PreInit
// checks it at 0x823660C4 and otherwise starts it with sub_82741218 once the hardware has more
// than one thread). Experimental: other threads then run render commands inline and the game
// crashes during boot.

#include "mk9/engine.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "mk9/game.h"
#include "mk9/net/rollback.h"

REX_EXTERN(__imp__sub_82741218);  // StartRenderingThread
REX_EXTERN(__imp__sub_82740FE8);  // RenderingThreadMain
REX_EXTERN(__imp__sub_82BF65B0);  // a D3D device call of every frame (r3: the device)

namespace {
constexpr uint32_t kUseThreadedRendering = 0x8341C20C;  // set before the call
constexpr uint32_t kRenderRing = 0x8341E4E0;
// The Xbox 360 D3D device (the library is linked in around 0x82BF0000-0x82C10000): its ring
// buffer position and fences go with what the GPU already took, so a rollback never puts it
// back. Its fields seen so far reach past +21800.
constexpr uint32_t kD3DDeviceSize = 0x6000;
std::atomic<uint32_t> g_d3d_device{0};
std::atomic<uint32_t> g_render_tid{0};
}  // namespace

namespace mk9::engine {

bool WaitRenderIdle(int timeout_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (int spins = 0;; spins++) {
    if (game::ReadU32(kRenderRing + 20) == game::ReadU32(kRenderRing + 8)) return true;
    if (std::chrono::steady_clock::now() > until) return false;
    if (spins > 64) std::this_thread::yield();
  }
}

void ExcludeFromRollback() {
  rb::AddVolatile(kRenderRing, 0x40);
  const uint32_t data = game::ReadU32(kRenderRing), end = game::ReadU32(kRenderRing + 4);
  if (data && end > data && end - data < 0x4000000) rb::AddVolatile(data, end - data);
  const uint32_t device = g_d3d_device.load();
  if (device) rb::AddVolatile(device, kD3DDeviceSize);
  // the rendering thread runs what the main thread queued: what it writes (the renderer's scene)
  // goes back with the game
  if (const uint32_t tid = g_render_tid.load()) rb::AddSimulationThread(tid);
  REXLOG_INFO("mk9 engine: rollback leaves the rendering command ring ({:08X}-{:08X}) and the D3D device ({:08X}) "
              "alone; rendering thread {}",
              data, end, device, g_render_tid.load());
}

}  // namespace mk9::engine

REX_HOOK_RAW(sub_82740FE8) {
  g_render_tid = GetCurrentThreadId();
  REXLOG_INFO("mk9 engine: the rendering thread runs on host thread {}", g_render_tid.load());
  __imp__sub_82740FE8(ctx, base);
}

REX_HOOK_RAW(sub_82BF65B0) {
  if (!g_d3d_device.load(std::memory_order_relaxed)) {
    g_d3d_device = ctx.r3.u32;
    REXLOG_INFO("mk9 engine: the D3D device is at {:08X}", ctx.r3.u32);
  }
  __imp__sub_82BF65B0(ctx, base);
}

REX_HOOK_RAW(sub_82741218) {
  static const bool one_thread = [] {
    const char* v = std::getenv("MK9_ONETHREAD");
    return v && std::strcmp(v, "1") == 0;
  }();
  if (one_thread) {
    mk9::game::WriteU32(kUseThreadedRendering, 0);
    REXLOG_INFO("mk9: rendering stays on the main thread (MK9_ONETHREAD)");
    return;
  }
  __imp__sub_82741218(ctx, base);
}
