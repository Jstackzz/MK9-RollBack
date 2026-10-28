#pragma once
// The game's frame boundary. GuardedMain (sub_823667E8) runs `while (!GIsRequestingExit)
// FEngineLoop::Tick()` on the main guest thread; the hook on FEngineLoop::Tick (sub_823621F8)
// is the top of each frame: no game code is on the guest stack below GuardedMain's own frame,
// so the simulation can be saved and put back there (rollback.h), and inputs for the frame are
// settled before the engine polls the pads.
//
// No more than 60 frames a second start there (MK9_FRAME_CAP=<n> caps at n instead, 0 not at all):
// the frames the game draws wait for the GPU's 60 Hz vertical blank anyway, the cap holds back the
// ones that do not (loading screens, some menus), and frames a rollback runs again are not capped.
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

// Rollback: from the top hook, after putting the game back to an earlier frame (and the count
// with SetCount), run one frame again: the engine's tick and the end hook, no top hook. Frames
// run this way are unseen catch-up work.
void RunAgain(PPCContext& ctx, uint8_t* base);
void SetCount(uint64_t n);
bool Replaying();

}  // namespace mk9::frame
