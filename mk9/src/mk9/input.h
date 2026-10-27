#pragma once
// What the game reads from its controllers. MK9 polls XamInputGetState for user indices 0-3
// every frame; this layer answers instead of the SDK: from the PC's own controllers and keyboard
// (remapped by the launcher's controls file, mk9/controls.h), or from a source netplay installs
// (both players' inputs for the frame, the same on every PC in the match).
#include <cstdint>

namespace mk9::input {

// one Xbox 360 controller as the game sees it
struct Pad {
  bool connected = false;
  uint16_t buttons = 0;  // XINPUT_GAMEPAD_* bits
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  // the XInput packet number when a source decides it (netplay: it must come out the same when a
  // frame runs again); 0: this layer counts changes itself
  uint32_t packet = 0;
};

constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kGuide = 0x0400;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

// The PC's own controller for a user index as the SDK reads it (SDL pads, the keyboard as
// user 0), with the launcher's remapping applied. Any thread.
Pad ReadPhysical(uint32_t user);

// A source answers for user indices it owns (returns true) and leaves the rest to the PC's own
// controllers. Installed by netplay on the main thread at the top of a frame.
using Source = bool (*)(uint32_t user, Pad& out);
void SetSource(Source source);
// asked first, before the source: the launcher's autopilot outside netplay (it presses for player
// 1 and keeps everyone else's controller out of the menus it steers; in a match it answers nothing
// and feeds its presses into netplay instead)
void SetOverride(Source override_source);

// how often the game polled each user this run (diagnostics)
uint64_t Polls(uint32_t user);

}  // namespace mk9::input
