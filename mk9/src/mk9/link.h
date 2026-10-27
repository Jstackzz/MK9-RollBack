#pragma once
// The game's side of the launcher protocol (docs/PROTOCOL.md section 3; see link.cpp).
#include <cstdint>
#include <string>

namespace mk9::link {

// MK9_LINK=1 and MK9_CMD / MK9_STATUS set: the launcher owns this game. At start.
void Init();
bool On();

// once per frame on the main thread (from the frame hook): takes commands, reports the state
void OnFrame(uint64_t frame);

// what the game layer reports back (main thread)
void ReportResult(int winner, int p1_rounds, int p2_rounds);
void ReportProblem(const std::string& status);  // "desync frame=N" / "disconnected"

}  // namespace mk9::link
