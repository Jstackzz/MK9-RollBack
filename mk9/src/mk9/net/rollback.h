#pragma once
// The state machinery rollback netplay needs: put the simulation back to the top of an earlier
// frame. Adapted from the MK3D project's rollback (the same copy-on-write design).
//
// Guest memory is tracked copy-on-write: at the top of each frame every tracked page is
// write-protected on the host; the first write to a page in a frame copies its old contents into
// that frame's undo log. Rolling back to frame r plays the undo logs of the frames since r
// backwards. Pages other threads write first (the render thread, sound, loaders) are "foreign":
// never logged or restored, so those threads are not pulled back in time under their feet; bytes
// other threads write on pages the main thread uses are "volatile": a restore leaves them as they
// are. Tracked: the committed writable pages of the virtual heaps (0x00010000-0x6FFFFFFF) and the
// executable's data (0x80000000-0x8FFFFFFF). The main thread's registers at each frame's top are
// kept too.
//
// All calls are made on the game's main thread at the top of a frame (mk9/frame.h).
#include <cstdint>
#include <vector>

struct PPCContext;

namespace mk9::rb {

bool Arm(PPCContext& ctx, uint32_t frame);
void Disarm();
bool Armed();
bool Restoring();

// at the top of each frame while armed, in this order:
//   CloseFrame()        the frame that just ran is complete: its undo log is closed
//   [Restore(r, ctx)]   optionally go back to the top of an earlier frame r
//   OpenFrame(f, ctx)   the frame about to run: its registers, a new undo log
void CloseFrame();
bool Restore(uint32_t frame, PPCContext& ctx);
void OpenFrame(uint32_t frame, PPCContext& ctx);

uint32_t OldestFrame();
constexpr int kKeepFrames = 16;

// hash of the pages the last closed frame wrote, their contents now (sync tests; off by default:
// it reads every page a frame wrote)
void SetHashing(bool on);
uint64_t LastFrameHash();
uint32_t LastFramePages();
// the pages the last closed frame wrote (sync test details)
std::vector<uint32_t> LastFramePageList();

// bytes another thread owns on pages that otherwise hold simulation state
void AddVolatile(uint32_t va, uint32_t size);

struct Stats {
  uint64_t faults = 0, foreign_pages = 0, tracked_pages = 0, logged_bytes = 0;
  double close_ms = 0, restore_ms = 0, arm_ms = 0;
  uint32_t last_pages = 0;
};
Stats GetStats();

}  // namespace mk9::rb
