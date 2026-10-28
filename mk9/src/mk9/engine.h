#pragma once
// What rollback needs to know about UE3's threads in MK9 (see engine.cpp).
#include <cstdint>

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

// The other simulation threads held still while rollback keeps or puts back memory: the rendering
// thread done with everything queued and asleep in its idle loop, every job worker in its idle
// wait. False when one did not get there within timeout_ms (the others are held anyway). Release
// lets them go. Main thread, top of a frame; a second Hold is the same hold.
bool Hold(int timeout_ms = 200);
void Release();

}  // namespace mk9::engine
