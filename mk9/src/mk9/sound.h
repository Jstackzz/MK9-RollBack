#pragma once
// Sounds across rollbacks, see sound.cpp. FMOD is not rolled back: what it plays goes on through a rollback, and what a
// frame run again asks of it must not play again.
#include <cstdint>

struct PPCContext;

namespace mk9::sound {

// a frame's tick returned (a frame run again too): what the corrected frames did not start again stops
void OnFrameEnd(PPCContext& ctx, uint8_t* base);
// the top of a frame, on the main thread
void OnFrameTop(PPCContext& ctx, uint8_t* base);
// before a frame runs again
void OnRunAgain(PPCContext& ctx, uint8_t* base);

struct Stats {
  uint64_t first = 0;     // starts on a frame's first run
  uint64_t again = 0;     // starts asked for by frames run again
  uint64_t kept = 0;      // ... that found their sound still playing: no start
  uint64_t silent = 0;    // ... whose sound had ended already: started muted
  uint64_t moved = 0;     // ... whose sound plays on another instance than the game now holds: muted, a stop stops both
  uint64_t fresh = 0;     // ... that no earlier run of the frame asked for: played
  uint64_t near = 0;      // of those that found their sound, the ones off the frame it was started in
  uint64_t stopped = 0;   // sounds of frames taken back that the corrected frames did not start again: stopped
  uint64_t told = 0;      // ends the game had been told of in frames taken back, told again
};
Stats GetStats();

}  // namespace mk9::sound
