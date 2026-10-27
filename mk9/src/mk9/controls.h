#pragma once
// The launcher's controls file: which keys press each Xbox 360 button (the keyboard plays as
// player 1 while the game window is in front), and what each button of a PC controller presses.
// Path: MK9_CONTROLS, else controls.toml beside the game. Read at start and again within a
// second whenever the file changes, so the launcher's Controls page applies at once.
//
//   [keyboard]
//   enabled = true
//   [keys]                       # Windows virtual key names, at most two per button
//   up = ["Up", "W"]
//   x = ["J"]
//   [buttons]                    # a PC controller's button (where it sits) -> the button it presses
//   south = "a"                  # the bottom face button (Xbox A, PlayStation cross)
//   left_trigger = "rt"          # "" presses nothing
//
// Xbox 360 buttons: up down left right a b x y lb rb lt rt start back ls rs
// PC controller buttons: south east west north left_shoulder right_shoulder left_trigger
// right_trigger back start left_stick right_stick dpad_up dpad_down dpad_left dpad_right
#include <cstdint>
#include <string>

namespace mk9::input {
struct Pad;
}

namespace mk9::controls {

// remap a pad's buttons as the file says (the SDK reports them in Xbox 360 positions)
void Apply(mk9::input::Pad& pad);
// the keyboard's buttons for player 1, OR'ed into user 0 (nothing while another window is in
// front or the file turns it off)
void AddKeyboard(mk9::input::Pad& pad);
// reload now if the file changed (also done by Apply at most once a second)
void Poll();
std::string FilePath();

}  // namespace mk9::controls
