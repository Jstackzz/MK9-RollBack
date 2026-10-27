#pragma once
// What netplay and the launcher link need to know about MK9's state, read from guest memory.
// Every address is for default.xex 575207FD version 0.0.0.8 (MK9Game-Retail.exe, 2011-12-05).
//
// Known so far:
//   0x833CED24  the round timer, seconds left (an MKScript global: int, its table entry starts
//               at 0x833CED18 with the type 0x820F3B60, -1, value)
//   sub_82B2CFB0 the C runtime's rand(): state at +20 of the calling thread's CRT data
#include <cstdint>
#include <vector>

struct PPCContext;

namespace mk9::net {
struct Agreement;
}

namespace mk9::game {

uint32_t ReadU32(uint32_t guest_address);
void WriteU32(uint32_t guest_address, uint32_t value);

// the round timer as the HUD shows it (0 outside fights)
int32_t RoundTimer();

// what the barrier tells the other players (the match's settings this game would use)
std::vector<uint32_t> ReadyWords();
// make this game's state what everyone agreed on at the barrier (main thread, top of a frame):
// the C runtime's rand() of the main thread from the agreed seed
void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed);
// the main thread's rand() state (its CRT per-thread data +20), and setting it
uint32_t MainRandState(PPCContext& ctx, uint8_t* base);
void SetMainRandState(PPCContext& ctx, uint8_t* base, uint32_t state);
// what every PC in the match must have alike at the top of a frame
uint32_t StateHash(PPCContext& ctx, uint8_t* base);

}  // namespace mk9::game
