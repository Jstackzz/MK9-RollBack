#pragma once
// The autopilot: takes the game through its menus for the launcher (mk9/link.h). From boot to the
// hold point (the main menu, as a fresh save has it), from there to a mode's select screen, where
// the players pick with their own controllers, and after a match through the CONTINUE? menu.
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
  int slot = 0;     // this player's (user index in the match); -1 a spectator
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
// after a match, at the CONTINUE? menu: the same fighters again, both pick again, or back to the
// main menu (the hold point)
void Rematch(const Plan& plan);
void SelectAgain(const Plan& plan);
void Park();

// once per match, at the CONTINUE? menu: who won (1 or 2) and the rounds each won
bool MatchOver(int& winner, int& p1_rounds, int& p2_rounds);

// while it steers: the local player's pad as the autopilot presses it (true), the PC's own
// controller held back; false: the player's own controller
bool LocalPad(input::Pad& out);

}  // namespace mk9::autopilot
