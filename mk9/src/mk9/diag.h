#pragma once
// Diagnostics for finding the game's moving parts, from the environment:
//   MK9_DIAG_THREADS = <frame>   at that frame, log every guest thread (handle, id, start
//                                address, priority, affinity core, host thread)
//   MK9_DIAG_HEAPS   = <frame>   at that frame, log the guest memory heaps' committed regions
//   MK9_DIAG_DUMP    = <frame>,<frame>...   at those frames, write every committed guest region to
//                                <MK9_SNAP_DIR or cwd>\dump-<frame>.bin as records of
//                                u32 address, u32 size, bytes (tools/memdiff.py compares them)
#include <cstdint>
#include <string>

namespace mk9::diag {

void Init();
// at the end of each frame, on the main thread
void OnFrame(uint64_t frame);
void LogThreads();
void LogHeaps();
bool DumpMemory(const std::string& path);

}  // namespace mk9::diag
