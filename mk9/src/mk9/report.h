#pragma once
// F3: a player who sees something drawn wrong presses it in the game. What is on the screen and the next 20 frames'
// pictures are saved under MK9_CAPTURE_DIR (the launcher points it at the session's captures folder: no launcher, no
// reports), a line at the top of the screen says so, and the launcher's ReportSender sends them to the server with the
// end of the game's log (docs/PROTOCOL.md, "F3 reports"). It only takes pictures: nothing of the game changes, and the
// other player's game knows nothing of it. MK9_TEST_F3_AT=<frame>,...: a test's presses, at those frames.
#include <cstdint>

namespace mk9::report {

void Init();
// at the end of each frame, on the main thread
void OnFrame(uint64_t frame);

}  // namespace mk9::report
