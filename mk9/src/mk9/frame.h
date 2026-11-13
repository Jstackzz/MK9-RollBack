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
// ones that do not (loading screens, some menus), and frames a rollback runs again are not capped. A fight frame that
// came late (a rollback, a stall) is made up for, up to 17 ms of it, by starting the next ones close together.
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

// Netplay keeping the sides even: the next frame starts `periods` whole frames later than the cap would start it (the
// screen shows this frame that many times more). From the main thread
void HoldNext(int periods = 1);
// The same smoothly: the next frame starts `ms` later than the cap would start it. The lead over the others is taken
// off over many frames, a few ms each (a tenth of a frame, a slowdown nobody sees), where whole frames held back were
// frames shown twice. Calls add up. From the main thread
void StretchNext(double ms);

// The cap's last wait, which Top made first thing: how long it slept and the periods a hold added to it (what made a
// frame long, for netplay's stats). From the main thread
struct CapInfo {
  double slept_ms = 0;
  int hold = 0;
  double stretch_ms = 0;
};
CapInfo LastCap();
// The frame that just ran was late because it waited for the others (their inputs at the prediction limit), or the
// frames are not a fight's (loading, menus): the cap does not make up for it (see the cap's comment in frame.cpp).
// From the main thread
void DropCatchUp();
// when the last frame started, in steady clock nanoseconds (0 before the first): the stall sampler reads it
int64_t LastTopNs();
// true while the main thread sleeps in the frame cap or a hold (idle by design)
bool InCapWait();

}  // namespace mk9::frame
