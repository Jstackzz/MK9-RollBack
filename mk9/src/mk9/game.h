#pragma once
// What netplay and the launcher link need to know about MK9's state, read from guest memory.
// Every address is for default.xex 575207FD version 0.0.0.8 (MK9Game-Retail.exe, 2011-12-05).
//
// Known so far:
//   0x833CED24  the round timer, seconds left (an MKScript global: int, its table entry starts
//               at 0x833CED18 with the type 0x820F3B60, -1, value)
//   0x834161D8  the game's random number generator, one global for every thread (UE3's LCG,
//               x * 196314165 + 907633515): sub_826D91C0 draws an int, sub_826D91E8 a float,
//               sub_826D3088 seeds it (at boot from the clock, and when a fight is set up).
//               sub_823875B0 picks arenas with it (and Rooftop Day/Dawn/Dusk, Pit/Pit Day)
//   sub_82B2CFB0 the C runtime's rand(): state at +20 of the calling thread's CRT data. Only FMOD
//               draws from it (sound variations), after seeding it from the clock
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
// the game's random number generator and the main thread's rand() from the agreed seed. Until
// the next barrier, when the game seeds its generator again, the seed comes from the agreed one
// and the netplay frame (the same on every PC, and when a frame runs again)
void Agree(PPCContext& ctx, uint8_t* base, const mk9::net::Agreement& agreed);
// the game's random number generator
uint32_t GameRandState();
void SetGameRandState(uint32_t state);
// rollback put the game back to the top of netplay frame `frame`: host-side state of the frames
// since is taken back too
void RolledBack(uint32_t frame);
// the main thread's rand() state (its CRT per-thread data +20), and setting it
uint32_t MainRandState(PPCContext& ctx, uint8_t* base);
void SetMainRandState(PPCContext& ctx, uint8_t* base, uint32_t state);
// what every PC in the match must have alike at the top of a frame
uint32_t StateHash(PPCContext& ctx, uint8_t* base);
// the values behind the last StateHash, for the per-frame log (rand_state: the game's generator)
void LastCheckDetail(uint32_t& timer, uint32_t& rand_state);

// at the top of a frame in sync: true once when every PC must meet again before going on (a fight
// has been set up: its round timer was given its first value)
bool SyncPoint();

}  // namespace mk9::game
