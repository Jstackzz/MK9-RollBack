// The autopilot, see autopilot.h.
//
// Where the game is comes from game::Screen() (an MKScript global: -1 booting or loading, 0 the
// title, 3 the front end's menus, 10 the select screens, 25 a fight). Inside the menus it steers
// by steps of known length from the hold point, as a fresh save starts them:
//   main menu, cursor on FIGHT:  UP (the "1 PLAYER" header), RIGHT ("2 PLAYERS"), DOWN (VERSUS
//   MODE), A: ONE VS ONE / TWO VS TWO / TEST YOUR LUCK, DOWN x n, A: the select screen
// and checks it got there (Screen() 10) before handing the controllers to the players.

#include "mk9/autopilot.h"

#include <cstdlib>
#include <cstring>
#include <deque>

#include <rex/logging.h>

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/input.h"
#include "mk9/net/netplay.h"

namespace mk9::autopilot {
namespace {

enum class Phase { Off, Boot, Title, ToMenu, Hold, Wait, Steer, Follow, Pick, Players, Lost };

struct Step {
  uint16_t buttons;
  int hold;   // frames pressed
  int after;  // frames released after
};

bool g_launcher = false;
Phase g_phase = Phase::Off;
uint64_t g_since = 0;  // frame the phase began
Plan g_plan;
bool g_online = false;
std::deque<Step> g_steps;
int g_step_left = 0;  // frames left of the current step's press (>0) or release (<0 count)
input::Pad g_pad;     // what the autopilot presses now
uint64_t g_frame = 0;

void Enter(Phase p) {
  g_phase = p;
  g_since = g_frame;
}

void Push(uint16_t buttons, int hold = 8, int after = 30) { g_steps.push_back({buttons, hold, after}); }

// the menu path from the hold point to the plan's select screen
void PlanSteps(const Plan& plan) {
  g_steps.clear();
  Push(input::kDpadUp, 8, 60);      // the "1 PLAYER" header
  Push(input::kDpadRight, 8, 120);  // "2 PLAYERS"
  Push(input::kDpadDown, 8, 60);    // VERSUS MODE
  Push(input::kA, 8, 120);          // ONE VS ONE / TWO VS TWO / TEST YOUR LUCK
  const int down = plan.mode == Mode::Tag ? 1 : plan.mode == Mode::TestYourLuck ? 2 : 0;
  for (int i = 0; i < down; i++) Push(input::kDpadDown, 8, 40);
  Push(input::kA, 8, 200);  // the select screen
}

// slot 0 steers online; offline (practice) the autopilot steers player 1
bool Steering() { return !g_online || g_plan.slot == 0; }

void AtSelect(uint64_t frame) {
  const bool pick = !g_plan.picks[g_plan.slot].empty();
  Enter(pick ? Phase::Pick : Phase::Players);
  REXLOG_INFO("mk9 autopilot: at the select screen at frame {}; {}", frame,
              pick ? "picking " + g_plan.picks[g_plan.slot] : std::string("the player picks"));
}

}  // namespace

Mode ModeFromName(const std::string& n) {
  if (n == "versus" || n == "kombat" || n == "1v1" || n == "koth") return Mode::Versus;
  if (n == "tag" || n == "2v2") return Mode::Tag;
  if (n == "tyl" || n == "testyourluck") return Mode::TestYourLuck;
  if (n == "practice") return Mode::Practice;
  return Mode::Unknown;
}

const char* ModeName(Mode m) {
  switch (m) {
    case Mode::Versus: return "versus";
    case Mode::Tag: return "tag";
    case Mode::TestYourLuck: return "tyl";
    case Mode::Practice: return "practice";
    default: return "?";
  }
}

namespace {

// offline the autopilot is player 1's controller; while it holds the menus nobody else's
// controller reaches them either
bool Override(uint32_t user, input::Pad& out) {
  if (net::InSync()) return false;  // netplay's inputs (the autopilot's presses go in as its player's)
  if (user == 0) return LocalPad(out);
  if (!g_launcher || g_phase == Phase::Players || g_phase == Phase::Off || g_phase == Phase::Lost) return false;
  out = input::Pad{};
  return true;
}

}  // namespace

void Init(bool launcher) {
  g_launcher = launcher;
  if (launcher) {
    Enter(Phase::Boot);
    input::SetOverride(&Override);
  }
}

bool AtHoldPoint() { return g_phase == Phase::Hold; }

void Begin(const Plan& plan, bool online) {
  g_plan = plan;
  g_online = online;
  if (plan.mode == Mode::Practice) {
    // practice is the game's own: the player gets the controller at the main menu
    Enter(Phase::Players);
    return;
  }
  PlanSteps(plan);
  g_step_left = 0;
  // online the steps start once netplay is in sync (every game at its first frame together)
  Enter(online ? Phase::Wait : Phase::Steer);
  REXLOG_INFO("mk9 autopilot: to the {} select screen, slot {} of {}{}", ModeName(plan.mode), plan.slot,
              plan.players, Steering() ? " (steering)" : "");
}

void Rematch(const Plan& plan) {
  g_plan = plan;
  Enter(Phase::Players);  // the game's own rematch menu: the players choose
}

void Park() {
  // back at the hold point: for now the launcher starts the game again when the menus are elsewhere
  if (game::Screen() == 3) Enter(Phase::Hold);
  else Enter(Phase::Lost);
}

void OnFrame(uint64_t frame) {
  g_frame = frame;
  const int screen = game::Screen();
  static int last_screen = -2;
  if (screen != last_screen) {
    REXLOG_INFO("mk9 autopilot: screen {} at frame {}{}", screen, frame, game::TitleUp() ? " (title)" : "");
    last_screen = screen;
  }
  g_pad = input::Pad{};
  g_pad.connected = true;
  switch (g_phase) {
    case Phase::Off:
    case Phase::Players:
    case Phase::Lost:
      return;
    case Phase::Boot:
      if (screen == 0 && game::TitleUp()) Enter(Phase::Title);
      // the attract mode's demo fight (the title left alone): START takes it back to the title
      if (screen == 25 && (frame - g_since) % 60 < 8) g_pad.buttons = input::kStart;
      return;
    case Phase::Title:
      // START until the title takes it (it ignores presses while it fades in)
      if (screen == 0 && (frame - g_since) % 45 >= 30 && (frame - g_since) % 45 < 38) g_pad.buttons = input::kStart;
      if (screen == 3) Enter(Phase::ToMenu);
      if (screen != 0 && screen != 3 && frame - g_since > 60) Enter(Phase::Boot);  // the attract mode came first
      return;
    case Phase::ToMenu:
      if (frame - g_since > 90) {
        Enter(Phase::Hold);
        REXLOG_INFO("mk9 autopilot: at the hold point (main menu) at frame {}", frame);
      }
      return;
    case Phase::Hold:
      return;
    case Phase::Wait:
      if (net::InSync()) {
        Enter(Steering() ? Phase::Steer : Phase::Follow);
        if (!Steering()) REXLOG_INFO("mk9 autopilot: slot {} waits for slot 0 to steer the menus", g_plan.slot);
      }
      return;
    case Phase::Follow:
      if (screen == 10) AtSelect(frame);
      return;
    case Phase::Pick: {
      // a given fighter: A on the cursor's (roster navigation comes with the roster's layout). A
      // picks, another A confirms; again for the second fighter of a two-versus-two of two players.
      // A every second until the select screen goes (to the arena's loading)
      const uint64_t t = frame - g_since;
      if (screen != 10 || t > 900) {
        Enter(Phase::Players);
        return;
      }
      if (t >= 90 && (t - 90) % 60 < 8) g_pad.buttons = input::kA;
      return;
    }
    case Phase::Steer:
      if (g_step_left > 0) {
        g_pad.buttons = g_steps.front().buttons;
        if (--g_step_left == 0) g_step_left = -g_steps.front().after;
        return;
      }
      if (g_step_left < 0) {
        if (++g_step_left == 0) g_steps.pop_front();
        return;
      }
      if (!g_steps.empty()) {
        g_step_left = g_steps.front().hold;
        g_pad.buttons = g_steps.front().buttons;
        return;
      }
      if (screen == 10) {
        AtSelect(frame);
      } else if (frame - g_since > 3000) {
        REXLOG_WARN("mk9 autopilot: the select screen did not come (screen {})", screen);
        Enter(Phase::Lost);
      }
      return;
  }
}

bool LocalPad(input::Pad& out) {
  switch (g_phase) {
    case Phase::Title:
    case Phase::Steer:
    case Phase::Pick:
      out = g_pad;
      return true;
    case Phase::Boot:
    case Phase::ToMenu:
    case Phase::Hold:
    case Phase::Wait:
    case Phase::Follow:
      // the player's controller does not reach the menus while the launcher holds them
      out = input::Pad{};
      out.connected = true;
      return g_launcher;
    default:
      return false;
  }
}

}  // namespace mk9::autopilot
