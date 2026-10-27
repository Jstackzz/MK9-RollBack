#pragma once
// The autopilot: takes the game through its menus for the launcher (mk9/link.h). From boot to the
// hold point (the main menu, cursor on FIGHT, as a fresh save has it), and from there to a mode's
// select screen, where the players pick with their own controllers.
#include <cstdint>
#include <string>

namespace mk9::input {
struct Pad;
}

namespace mk9::autopilot {

enum class Mode { Unknown, Versus, Tag, TestYourLuck, Practice };
Mode ModeFromName(const std::string& name);
const char* ModeName(Mode m);

struct Plan {
  Mode mode = Mode::Versus;
  int slot = 0;     // this player's (user index in the match)
  int players = 2;  // 2, or 4 for a two-versus-two with four people
  uint32_t seed = 0;
  std::string picks[4];  // roster ids the autopilot picks; empty: the player picks
};

// at start (launcher=true: the boot goes to the hold point by itself)
void Init(bool launcher);
// end of every frame on the main thread
void OnFrame(uint64_t frame);
bool AtHoldPoint();

// from the hold point to the plan's select screen; online, slot 0's autopilot steers both games
// (its presses are its player's netplay input) and the others wait
void Begin(const Plan& plan, bool online);
void Rematch(const Plan& plan);
void Park();

// while it steers: the local player's pad as the autopilot presses it (true), the PC's own
// controller held back; false: the player's own controller
bool LocalPad(input::Pad& out);

}  // namespace mk9::autopilot
