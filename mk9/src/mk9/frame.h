#pragma once
// The game's frame boundary. GuardedMain (sub_823667E8) runs `while (!GIsRequestingExit) {...
// FEngineLoop::Tick() ...}` on the main guest thread. Each time round it first reads the time base
// (sub_82366BF8, called from 0x82366864), counts the frame, reads the pads (sub_829D5230) and the
// key presses (sub_829D49F0), updates the game's systems, and then ticks the engine (sub_823621F8).
// That first call is the top of each frame: no game code is on the guest stack below GuardedMain's
// own frame, so the simulation can be saved and put back there (rollback.h), and the pads are not
// read yet, so the inputs settled at the top of a frame (netplay) are the ones its tick plays on.
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

// hooks other modules register (each at most once): the top of a frame, before GuardedMain's loop
// reads the pads, and its end, right after the engine's tick returned
using Hook = void (*)(PPCContext& ctx, uint8_t* base);
void SetTopHook(Hook hook);
void SetEndHook(Hook hook);

// Rollback: from the top hook, after putting the game back to an earlier frame (and the count
// with SetCount), run one frame again: GuardedMain's loop once round from its top (the pads and
// key presses read, among other things), the engine's tick, the end hook, and the rest of the loop
// (no top hook), so a frame run again leaves the game where its first run would have with the
// same inputs. Frames run this way are unseen catch-up work.
void RunAgain(PPCContext& ctx, uint8_t* base);
void SetCount(uint64_t n);
bool Replaying();

}  // namespace mk9::frame
