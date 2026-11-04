#pragma once
// GMalloc routing to the simulation heap (see gmalloc.cpp).
#include <cstdint>

struct PPCContext;

namespace mk9::gmalloc {

// main thread, top of every frame before the rollback bookkeeping: the frees that waited
void OnFrameTop(PPCContext& ctx, uint8_t* base);

// While one lives on a thread, what that thread allocates is UE3's async loader's (its request queue, a request's
// copy of a file name, which the loader's thread frees): from the audio heap, as another thread's block, whoever
// allocates it (mk9/asyncio.cpp, around QueueIORequest)
class LoaderMemory {
 public:
  LoaderMemory();
  ~LoaderMemory();
  LoaderMemory(const LoaderMemory&) = delete;
  LoaderMemory& operator=(const LoaderMemory&) = delete;
};

}  // namespace mk9::gmalloc
