#pragma once
// What rollback needs to know about UE3's threads in MK9 (see engine.cpp).
#include <cstdint>
#include <string>

struct PPCContext;

namespace mk9::engine {

// The rendering thread has run every command the main thread queued (the ring is empty). Waits
// up to timeout_ms; false on timeout. Main thread, top of a frame.
bool WaitRenderIdle(int timeout_ms);

// before rb::Arm: the guest memory a rollback must never put back (the rendering command ring and
// its control block: the rendering thread only moves forward through it; the D3D device; other
// threads' globals on the simulation's pages); the rendering thread, the job workers and PhysX's
// threads made simulation threads
void ExcludeFromRollback();

// the rendering thread or a job worker (the main thread is mk9::frame's)
bool IsSimulationThread(uint32_t host_tid);
// diagnostics (hang.txt): the hold's flags and which threads say they wait
std::string HoldState();

// The other simulation threads held still while rollback keeps or puts back memory: the rendering
// thread done with everything queued and asleep in its idle loop, every job worker in its idle
// wait. False when one did not get there within timeout_ms (the others are held anyway). Release
// lets them go. Main thread, top of a frame; a second Hold is the same hold (true once complete).
bool Hold(int timeout_ms = 200);
// The hold complete, waiting up to timeout_ms more for the threads still running (Hold first if there is none; once a
// hold: a second call says how it stands without waiting again).
// A restore must not go on otherwise: it would put memory back under a thread that runs (until 2026-11-04 one
// went on after a hold timed out: the rendering thread, in a D3D call, crashed at sub_82820828+0x402, the stack
// of a player's 0.5.2 crash report). MK9_RB_NO_HOLD_WAIT=1 (comparisons): true at once, as before
bool WaitHeld(int timeout_ms);
void Release();

// Where the rendering thread was while Hold waited for it, sampled every 100 us while on: the guest's
// return address and its two callers, most seen first, with the share of samples ("" when none)
void SampleRenderWaits(bool on);
std::string TakeRenderWaitSamples(int top);

// the rendering thread's guest registers while it runs (diagnostics: crash.cpp's watch), else null
const PPCContext* RenderContext();

}  // namespace mk9::engine
