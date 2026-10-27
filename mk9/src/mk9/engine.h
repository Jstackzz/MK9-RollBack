#pragma once
// What rollback needs to know about UE3's threads in MK9 (see engine.cpp).
#include <cstdint>

namespace mk9::engine {

// The rendering thread has run every command the main thread queued (the ring is empty). Waits
// up to timeout_ms; false on timeout. Main thread, top of a frame.
bool WaitRenderIdle(int timeout_ms);

// right after rb::Arm: the guest memory a rollback must never put back (the rendering command
// ring and its control block: the rendering thread only moves forward through it)
void ExcludeFromRollback();

}  // namespace mk9::engine
