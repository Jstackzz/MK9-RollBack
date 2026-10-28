#pragma once
// GMalloc routing to the simulation heap (see gmalloc.cpp).
#include <cstdint>

struct PPCContext;

namespace mk9::gmalloc {

// main thread, top of every frame before the rollback bookkeeping: the frees that waited
void OnFrameTop(PPCContext& ctx, uint8_t* base);

}  // namespace mk9::gmalloc
