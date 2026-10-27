#pragma once
// Scripted controllers for tests, from the environment:
//   MK9_INPUT_P1..P4 = "frame:BUTTONS,frame:BUTTONS,..."   what user index 0..3 holds from that
//                      frame on (frames counted by mk9::frame); "+n:" is n frames after the step
//                      before; BUTTONS is '+'-joined names (UP DOWN LEFT RIGHT A B X Y LB RB LT RT
//                      START BACK LS RS) or '-' for nothing
//   MK9_PADS = n       users 0..n-1 report a controller even with nothing plugged in (a scripted
//                      user always does)
//   MK9_NO_PHYSICAL=1  the PC's own controllers and keyboard reach no user (test runs a person
//                      at the PC must not steer)
// A scripted user ignores the PC's own controller.
#include <cstdint>

namespace mk9::input {
struct Pad;
}

namespace mk9::testinput {

// installs the input source when the environment asks for one; at start
void Init();
bool Active();
// the scripted pad of a user index at the current frame, when that user has a script (netplay
// reads its local player this way in tests)
bool ScriptedPad(uint32_t user, mk9::input::Pad& out);

}  // namespace mk9::testinput
