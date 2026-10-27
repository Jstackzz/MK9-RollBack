// The game's fibers (MKScript's latent actions run on them, see mk9_codegen.toml [rexcrt]): the
// SDK runs them on host fibers; this counts the switches, which rollback has to know about (a
// fiber's host stack is part of the simulation state).

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

#include <windows.h>

namespace mk9::fibers {

std::atomic<uint64_t> g_switches{0};
uint64_t Switches() { return g_switches.load(std::memory_order_relaxed); }

}  // namespace mk9::fibers

namespace {

using Fn = void (*)(PPCContext&, uint8_t*);

// the SDK's own implementation, in the runtime DLL
Fn SdkSwitchToFiber() {
  static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("rexruntime.dll"), "rexcrt_SwitchToFiber"));
  return fn;
}

}  // namespace

extern "C" REX_FUNC(rexcrt_SwitchToFiber) {
  if (mk9::fibers::g_switches.fetch_add(1, std::memory_order_relaxed) == 0) {
    REXLOG_INFO("mk9: the game switched fibers for the first time");
  }
  if (Fn fn = SdkSwitchToFiber()) fn(ctx, base);
}
