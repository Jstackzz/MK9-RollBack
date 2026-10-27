#pragma once
// Scripted controllers for tests, from the environment:
//   MK9_INPUT_P1..P4 = "frame:BUTTONS,frame:BUTTONS,..."   what user index 0..3 holds from that
//                      frame on (frames counted by mk9::frame); "+n:" is n frames after the step
//                      before; BUTTONS is '+'-joined names (UP DOWN LEFT RIGHT A B X Y LB RB LT RT
//                      START BACK LS RS) or '-' for nothing
//   MK9_PADS = n       users 0..n-1 report a controller even with nothing plugged in (a scripted
//                      user always does)
// A scripted user ignores the PC's own controller.
namespace mk9::testinput {

// installs the input source when the environment asks for one; at start
void Init();
bool Active();

}  // namespace mk9::testinput
