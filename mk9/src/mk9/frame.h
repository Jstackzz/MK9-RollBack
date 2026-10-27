#pragma once
// The game's frame boundary. GuardedMain (sub_823667E8) runs `while (!GIsRequestingExit)
// FEngineLoop::Tick()` on the main guest thread; the hook on FEngineLoop::Tick (sub_823621F8)
// is the top of each frame: no game code is on the guest stack below GuardedMain's own frame,
// so the simulation can be saved and put back there (rollback.h), and inputs for the frame are
// settled before the engine polls the pads.
#include <cstdint>

struct PPCContext;

namespace mk9::frame {

// frames run so far on the main thread (FEngineLoop::Tick calls); re-run frames count too
uint64_t Count();
// the host thread id of the game's main guest thread (the one that ticks the engine), 0 before
// the first frame
uint32_t MainThreadId();
bool OnMainThread();

// hooks other modules register (each at most once): the top of a frame, before the engine's
// tick runs, and its end, after the tick returned
using Hook = void (*)(PPCContext& ctx, uint8_t* base);
void SetTopHook(Hook hook);
void SetEndHook(Hook hook);

}  // namespace mk9::frame
