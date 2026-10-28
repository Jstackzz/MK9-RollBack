#pragma once
// The state machinery rollback netplay needs: put the simulation back to the top of an earlier
// frame. Adapted from the MK3D project's rollback (the same copy-on-write design).
//
// Guest memory is tracked copy-on-write: at the top of each frame every tracked page is
// write-protected on the host; the first write to a page in a frame copies its old contents into
// that frame's undo log. Rolling back to frame r plays the undo logs of the frames since r
// backwards. The simulation is the main thread and the threads it hands its work (see
// AddSimulationThread). Pages other threads write first (sound, loaders, the GPU's callbacks) are
// "foreign": never logged or restored, so those threads are not pulled back in time under their
// feet; bytes other threads write on pages the simulation uses are "volatile": a restore leaves
// them as they are, and a page with volatile bytes is logged whoever writes it first. The simulation
// heap (SetSimulationRegion) is the simulation's whoever writes it. Tracked: the committed writable
// pages of the virtual heaps (0x00010000-0x6FFFFFFF) and the executable's data
// (0x80000000-0x8FFFFFFF). The main thread's registers at each frame's top are kept too.
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
// the frame open now (the one running), or the last one closed
uint32_t CurrentFrame();
constexpr int kKeepFrames = 16;

// other bookkeeping that goes back with the guest memory: fn(frame) at the end of every Restore
// to the top of `frame`, and at every OpenFrame (the top of `frame` as it is kept) (call once each,
// at startup)
void OnRestore(void (*fn)(uint32_t frame));
void OnOpen(void (*fn)(uint32_t frame));

// hash of the pages the last closed frame wrote, their contents now (sync tests; off by default:
// it reads every page a frame wrote)
void SetHashing(bool on);
uint64_t LastFrameHash();
uint32_t LastFramePages();
// the pages the last closed frame wrote (sync test details)
std::vector<uint32_t> LastFramePageList();

// bytes another thread owns on pages that otherwise hold simulation state; and no longer. Kept
// from one Arm to the next (and set before any page is protected), so they can be given early
void AddVolatile(uint32_t va, uint32_t size);
void RemoveVolatile(uint32_t va, uint32_t size);
// where other threads wrote the simulation region since the last call (a page's first write each)
struct OtherWrite {
  uint32_t va, host_tid;
  uint32_t returns[8];  // the guest's last call and the return addresses above it
};
std::vector<OtherWrite> TakeOtherThreadsWrites();
// guest memory rollback leaves alone altogether, from now on and in every later Arm (whole pages)
void Untrack(uint32_t va, uint32_t size);
// guest memory that is the simulation's whoever writes it (the simulation heap): a page another
// thread writes first is logged and put back like any other, never foreign
void SetSimulationRegion(uint32_t va, uint32_t size);
// a thread whose writes are part of the simulation like the main thread's (logged and put back):
// one that only runs what the main thread hands it and is idle at the top of every frame
void AddSimulationThread(uint32_t host_tid);
// one that ended
void RemoveSimulationThread(uint32_t host_tid);

// sync test diagnostics: a copy of every tracked page now, and later which pages differ from it
// (volatile bytes aside), with what rollback thinks of them
void DebugSnapshot();
void DebugCompare();
// the same for some pages only (a frame's), and their differences listed by 16-byte runs
void DebugSnapshotPages(const std::vector<uint32_t>& pages);
void DebugComparePages(const char* what);

struct Stats {
  uint64_t faults = 0, foreign_pages = 0, tracked_pages = 0, logged_bytes = 0;
  double close_ms = 0, restore_ms = 0, arm_ms = 0;
  uint32_t last_pages = 0;
};
Stats GetStats();

}  // namespace mk9::rb
