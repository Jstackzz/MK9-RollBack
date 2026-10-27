// The autopilot, see autopilot.h.
//
// Where the game is comes from game::Phase() (2 title, 3 menus, 7 select screens, 1 loading, 8 a
// fight, 11 the match is over) and game::UiScreen() (0x190 the main menu as "1 PLAYER", 0x194 as
// "2 PLAYERS", 0x198 VERSUS MODE's submenu, 0x62 the CONTINUE? menu). Inside a menu it steers by
// steps of known length:
//   main menu, cursor on FIGHT:  UP (the "1 PLAYER" header), RIGHT ("2 PLAYERS"), DOWN (VERSUS
//   MODE), A: ONE VS ONE / TWO VS TWO / TEST YOUR LUCK, DOWN x n, A: the select screen
//   CONTINUE?:  REMATCH, SELECT A NEW FIGHTER, QUIT TO THE MAIN MENU
// and checks it got where it went before handing the controllers to the players.

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

enum class Phase { Off, Boot, Title, ToMenu, Hold, Wait, Steer, Follow, Pick, Players, Continue, Leave, Lost };

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
bool g_over_reported = false;  // MatchOver said so for this match
bool g_fought = false;         // a fight ran since the last start or rematch (its result can come)
Phase g_after_steps = Phase::Players;  // where Steer goes once its steps are done and it got there
int32_t g_steer_to = game::kPhaseSelect;

void Enter(Phase p) {
  g_phase = p;
  g_since = g_frame;
}

void Push(uint16_t buttons, int hold = 8, int after = 30) { g_steps.push_back({buttons, hold, after}); }

int DownsFor(Mode m) { return m == Mode::Tag ? 1 : m == Mode::TestYourLuck ? 2 : 0; }

// the menu path from the main menu to the plan's select screen
void PlanSteps(const Plan& plan) {
  g_steps.clear();
  if (game::UiScreen() != 0x194) {
    Push(input::kDpadUp, 8, 60);      // the "1 PLAYER" header
    Push(input::kDpadRight, 8, 120);  // "2 PLAYERS"
    Push(input::kDpadDown, 8, 60);    // VERSUS MODE
  }
  Push(input::kA, 8, 120);  // ONE VS ONE / TWO VS TWO / TEST YOUR LUCK
  for (int i = 0; i < DownsFor(plan.mode); i++) Push(input::kDpadDown, 8, 40);
  Push(input::kA, 8, 200);  // the select screen
}

// slot 0 steers online; offline the autopilot steers player 1
bool Steering() { return !g_online || g_plan.slot == 0; }

void AtSelect(uint64_t frame) {
  const bool pick = g_plan.slot >= 0 && g_plan.slot < 4 && !g_plan.picks[g_plan.slot].empty();
  Enter(pick ? Phase::Pick : Phase::Players);
  g_over_reported = false;
  g_fought = false;
  REXLOG_INFO("mk9 autopilot: at the select screen at frame {}; {}", frame,
              pick ? "picking " + g_plan.picks[g_plan.slot]
                   : std::string(g_plan.slot < 0 ? "watching" : "the player picks"));
}

// from the CONTINUE? menu (cursor on REMATCH): n times DOWN, then A
void ContinueSteps(int downs, Phase after, int32_t to) {
  g_steps.clear();
  Push(0, 1, 120);  // the menu takes presses once it has come in
  for (int i = 0; i < downs; i++) Push(input::kDpadDown, 8, 40);
  Push(input::kA, 8, 120);
  g_step_left = 0;
  g_after_steps = after;
  g_steer_to = to;
  Enter(Steering() ? Phase::Steer : Phase::Follow);
}

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
  g_over_reported = false;
  g_fought = false;
  if (plan.mode == Mode::Practice) {
    // practice is the game's own: the player gets the controller at the main menu
    Enter(Phase::Players);
    return;
  }
  PlanSteps(plan);
  g_step_left = 0;
  g_after_steps = Phase::Players;
  g_steer_to = game::kPhaseSelect;
  // online the steps start once netplay is in sync (every game at its first frame together)
  Enter(online ? Phase::Wait : Phase::Steer);
  REXLOG_INFO("mk9 autopilot: to the {} select screen, slot {} of {}{}", ModeName(plan.mode), plan.slot,
              plan.players, Steering() ? " (steering)" : "");
}

void Rematch(const Plan& plan) {
  g_plan = plan;
  g_over_reported = false;
  g_fought = false;
  ContinueSteps(0, Phase::Players, game::kPhaseLoading);  // REMATCH: the same fighters, the fight loads
}

void SelectAgain(const Plan& plan) {
  g_plan = plan;
  g_over_reported = false;
  g_fought = false;
  ContinueSteps(1, Phase::Players, game::kPhaseSelect);  // SELECT A NEW FIGHTER
}

void Park() {
  if (g_phase == Phase::Continue || game::Phase() == game::kPhaseOver) {
    ContinueSteps(2, Phase::Hold, game::kPhaseMenus);  // QUIT TO THE MAIN MENU
    g_online = false;  // netplay is off from here: this game steers its own way back
    Enter(Phase::Steer);
  } else if (game::Phase() == game::kPhaseMenus) {
    Enter(Phase::Hold);
  } else {
    Enter(Phase::Lost);  // somewhere else: the launcher starts the game again
  }
}

bool MatchOver(int& winner, int& p1, int& p2) {
  if (g_over_reported || !g_fought || game::Phase() != game::kPhaseOver || game::UiScreen() != game::kUiContinue)
    return false;
  g_over_reported = true;
  p1 = game::RoundsWon(0);
  p2 = game::RoundsWon(1);
  winner = p1 > p2 ? 1 : 2;
  REXLOG_INFO("mk9 autopilot: the match is over, player {} won {}-{}", winner, p1, p2);
  return true;
}

void OnFrame(uint64_t frame) {
  g_frame = frame;
  const int32_t phase = game::Phase();
  const uint32_t ui = game::UiScreen();
  static int32_t last_phase = -2;
  static uint32_t last_ui = 0xFFFFFFFF;
  if (phase != last_phase || ui != last_ui) {
    REXLOG_INFO("mk9 autopilot: phase {} screen {:X} at frame {}{}", phase, ui, frame,
                game::TitleUp() ? " (title)" : "");
    last_phase = phase;
    last_ui = ui;
  }
  if (phase == game::kPhaseFight) g_fought = true;
  g_pad = input::Pad{};
  g_pad.connected = true;
  switch (g_phase) {
    case Phase::Off:
    case Phase::Lost:
      return;
    case Phase::Players:
      // the match ends at the CONTINUE? menu: the autopilot holds it until the launcher decides
      if (g_launcher && phase == game::kPhaseOver && ui == game::kUiContinue) Enter(Phase::Continue);
      return;
    case Phase::Continue:
      return;
    case Phase::Boot:
      if (phase == game::kPhaseMenus) Enter(Phase::ToMenu);
      else if (phase == game::kPhaseTitle && game::TitleUp()) Enter(Phase::Title);
      // the attract mode's demo fight (the title left alone): START takes it back to the title
      if (phase == game::kPhaseFight && (frame - g_since) % 60 < 8) g_pad.buttons = input::kStart;
      return;
    case Phase::Title:
      // START until the title takes it (it ignores presses while it fades in)
      if (game::TitleUp() && (frame - g_since) % 45 >= 30 && (frame - g_since) % 45 < 38) g_pad.buttons = input::kStart;
      if (phase == game::kPhaseMenus) Enter(Phase::ToMenu);
      else if (phase != game::kPhaseTitle) Enter(Phase::Boot);  // the attract mode came first
      return;
    case Phase::ToMenu:
      if (phase != game::kPhaseMenus) {
        Enter(Phase::Boot);
        return;
      }
      if (frame - g_since > 90) {
        Enter(Phase::Hold);
        REXLOG_INFO("mk9 autopilot: at the hold point (main menu, screen {:X}) at frame {}", ui, frame);
      }
      return;
    case Phase::Hold:
    case Phase::Leave:
      return;
    case Phase::Wait:
      if (net::InSync()) {
        Enter(Steering() ? Phase::Steer : Phase::Follow);
        if (!Steering()) REXLOG_INFO("mk9 autopilot: slot {} waits for slot 0 to steer the menus", g_plan.slot);
      }
      return;
    case Phase::Follow:
      if (phase == g_steer_to) {
        if (g_after_steps == Phase::Players && g_steer_to == game::kPhaseSelect) AtSelect(frame);
        else Enter(g_after_steps);
      }
      return;
    case Phase::Pick: {
      // a given fighter: A on the cursor's (roster navigation comes with the roster's layout). A
      // picks, another A confirms; again for the second fighter of a two-versus-two of two players.
      // A every second until the select screen goes (to the arena's loading)
      const uint64_t t = frame - g_since;
      if (phase != game::kPhaseSelect || t > 900) {
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
      if (phase == g_steer_to) {
        if (g_after_steps == Phase::Players && g_steer_to == game::kPhaseSelect) {
          AtSelect(frame);
        } else {
          Enter(g_after_steps);
          if (g_after_steps == Phase::Hold) REXLOG_INFO("mk9 autopilot: back at the hold point at frame {}", frame);
        }
      } else if (frame - g_since > 3000) {
        REXLOG_WARN("mk9 autopilot: phase {} did not come (phase {}, screen {:X})", g_steer_to, phase, ui);
        Enter(Phase::Lost);
      }
      return;
  }
}

bool LocalPad(input::Pad& out) {
  // tests (MK9_AUTOPILOT_ATTACK=1): player 1 walks in and jabs through every fight, so matches end
  static const bool attack = [] {
    const char* v = std::getenv("MK9_AUTOPILOT_ATTACK");
    return v && *v == '1';
  }();
  if (attack && g_phase == Phase::Players && g_plan.slot == 0 && game::Phase() == game::kPhaseFight) {
    out = input::Pad{};
    out.connected = true;
    const uint64_t t = g_frame % 128;
    if (t < 20) out.buttons = input::kDpadRight;
    else if (t >= 20 && (t - 20) % 18 < 12) out.buttons = input::kX;
    return true;
  }
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
    case Phase::Continue:
    case Phase::Leave:
      // the player's controller does not reach the menus while the launcher holds them
      out = input::Pad{};
      out.connected = true;
      return g_launcher;
    default:
      return false;
  }
}

}  // namespace mk9::autopilot
