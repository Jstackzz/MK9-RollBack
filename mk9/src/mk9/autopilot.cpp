// The autopilot, see autopilot.h.
//
// Where the game is comes from game::Phase() (2 title, 3 menus, 7 select screens, 1 loading, 8 a
// fight, 11 the match is over) and game::UiScreen() (0x190 the main menu as "1 PLAYER", 0x194 as
// "2 PLAYERS", 0x198 VERSUS MODE's submenu, 0x62 the CONTINUE? menu). Inside a menu it steers by
// steps of known length:
//   main menu, cursor on FIGHT:  UP (the "1 PLAYER" header), RIGHT ("2 PLAYERS"), DOWN (VERSUS
//   MODE), A: ONE VS ONE / TWO VS TWO / TEST YOUR LUCK, DOWN x n, A: the select screen
//   CONTINUE?:  REMATCH, SELECT A NEW FIGHTER, QUIT TO THE MAIN MENU (the loser's controller
//               steers it, as at an arcade's continue)
// and checks it got where it went before handing the controllers to the players.
//
// The boot plays its movies (the WB and NetherRealm logos...) before the title, about 23 s; the movie player
// skips one on A (Coalesced.ini, [MwyMoviePlayer] ControllerSkipKey=XboxTypeS_A), so the autopilot taps A until
// the title first comes up, then START there. Single player gets the same way to the main menu, where the
// player takes over (Phase::Skip).
//
// At the hold point the game would not stay by itself: 60 s after the last press MK9 goes back to its title,
// and about 30 s later plays its attract mode's demo fight, while the launcher still took it as ready (a lobby
// match then started from the title or the demo's fight, against a game at its main menu). A tap of the left
// trigger every 20 s, which the main menu does nothing with, keeps it there; a game that leaves the main menu
// all the same is taken back, and the link says booting meanwhile.

#include "mk9/autopilot.h"

#include <cstdlib>
#include <cstring>
#include <deque>

#include <rex/logging.h>

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/input.h"
#include "mk9/net/netplay.h"
#include "mk9/testinput.h"

namespace mk9::autopilot {
namespace {

enum class Phase { Off, Skip, Boot, Title, ToMenu, Hold, Wait, Steer, Follow, Pick, Players, Continue, Leave, Lost };

constexpr uint64_t kHoldTapEvery = 1200, kHoldTapFrames = 6;  // at the hold point: the left trigger, 6 frames in 20 s
constexpr uint64_t kSkipGiveUp = 3600;  // single player: the main menu not there after a minute, the player's anyway

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
// the rounds each player won in this match, counted from each round's winner as the round number goes on to the
// next (the game's own count stays 0 in a lobby match, game.h); the round number the fight showed last, and up to
// which round number the rounds are counted
int g_rounds[2] = {};
int g_round_last = 0, g_round_counted = 0;
int g_loser = 1;               // the last match's loser (0 player 1, 1 player 2): the CONTINUE? menu's
int g_steer_slot = 0;          // the slot whose autopilot steers online
uint32_t g_pad_user = 0;       // offline, the user index the autopilot presses for
Phase g_after_steps = Phase::Players;  // where Steer goes once its steps are done and it got there
int32_t g_steer_to = game::kPhaseSelect;
bool g_title_seen = false;  // the title was up once: the boot's movies are over
bool g_show_intro = false;  // MK9_SHOW_INTRO=1: the movies play

void Enter(Phase p) {
  g_phase = p;
  g_since = g_frame;
}

// the boot's movies: A taps (4 frames in 40) until the title first comes up. None after that: on its way from
// the title to the main menu the front end shows a submenu's screen id (0x198) for a moment, and an A there
// could pick something
void SkipMovies(uint64_t t, int32_t phase) {
  if (game::TitleUp()) g_title_seen = true;
  if (g_show_intro || g_title_seen || phase != game::kPhaseTitle) return;
  if (t % 40 >= 20 && t % 40 < 24) g_pad.buttons = input::kA;
}

// a match is to come: no result for it yet
void NewMatch() {
  g_over_reported = false;
  g_fought = false;
  g_rounds[0] = g_rounds[1] = 0;
  g_round_last = g_round_counted = 0;
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

// online the steering slot's autopilot steers (slot 0, or the loser at the CONTINUE? menu);
// offline the autopilot presses for g_pad_user
bool Steering() { return !g_online || g_plan.slot == g_steer_slot; }

void AtSelect(uint64_t frame) {
  const bool pick = g_plan.slot >= 0 && g_plan.slot < 4 && !g_plan.picks[g_plan.slot].empty();
  Enter(pick ? Phase::Pick : Phase::Players);
  NewMatch();
  REXLOG_INFO("mk9 autopilot: at the select screen at frame {}; {}", frame,
              pick ? "picking " + g_plan.picks[g_plan.slot]
                   : std::string(g_plan.slot < 0 ? "watching" : "the player picks"));
}

// from the CONTINUE? menu (cursor on REMATCH): n times DOWN, then A
void ContinueSteps(int downs, Phase after, int32_t to) {
  g_steer_slot = g_loser;
  g_pad_user = uint32_t(g_loser);
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
  if (user == g_pad_user) return LocalPad(out);
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
  const char* show = std::getenv("MK9_SHOW_INTRO");
  g_show_intro = show && *show == '1';
  if (launcher) {
    Enter(Phase::Boot);
    input::SetOverride(&Override);
  } else if (!g_show_intro && !testinput::Active()) {
    Enter(Phase::Skip);
    input::SetOverride(&Override);
    REXLOG_INFO("mk9 autopilot: single player, past the intro to the main menu");
  }
}

bool AtHoldPoint() { return g_phase == Phase::Hold; }

bool Skipping() { return g_phase == Phase::Skip; }

void Begin(const Plan& plan, bool online) {
  g_plan = plan;
  g_online = online;
  NewMatch();
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
  NewMatch();
  ContinueSteps(0, Phase::Players, game::kPhaseLoading);  // REMATCH: the same fighters, the fight loads
}

void SelectAgain(const Plan& plan) {
  g_plan = plan;
  NewMatch();
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
  // the last round's winner won the match: that round counts too
  const int last = game::RoundWinner();
  if (last == 1 || last == 2) g_rounds[last - 1]++;
  p1 = g_rounds[0];
  p2 = g_rounds[1];
  winner = last == 1 || last == 2 ? last : p1 > p2 ? 1 : 2;
  g_loser = winner == 1 ? 1 : 0;
  REXLOG_INFO("mk9 autopilot: the match is over, player {} won {}-{}", winner, p1, p2);
  return true;
}

void OnFrame(uint64_t frame) {
  g_frame = frame;
  const int32_t phase = game::Phase();
  const uint32_t ui = game::UiScreen();
  static int32_t last_phase = -2;
  static uint32_t last_ui = 0xFFFFFFFF;
  const bool fight_began = phase == game::kPhaseFight && last_phase != game::kPhaseFight;
  if (phase != last_phase || ui != last_ui) {
    REXLOG_INFO("mk9 autopilot: phase {} screen {:X} at frame {}{}", phase, ui, frame,
                game::TitleUp() ? " (title)" : "");
    last_phase = phase;
    last_ui = ui;
  }
  if (phase == game::kPhaseFight) {
    g_fought = true;
    // a fight's rounds count from its start (a spectator's game goes from one match's CONTINUE? menu to the
    // rematch's fight as the players steer it, with no word from the launcher)
    if (fight_began) {
      g_rounds[0] = g_rounds[1] = 0;
      g_round_last = g_round_counted = 0;
    }
    // a round is over once the round number goes on to the next (its winner was set seconds before), each once (a
    // rollback may show a round's end, take it back and show it again; a rematch's first frames may still show the
    // last match's number); the match's last round counts at the CONTINUE? menu (MatchOver)
    const int round = game::Round();
    if (g_round_last && round == g_round_last + 1 && round > g_round_counted) {
      g_round_counted = round;
      const int won = game::RoundWinner();
      if (won == 1 || won == 2) g_rounds[won - 1]++;
      REXLOG_INFO("mk9 autopilot: round {} won by player {} at frame {} ({}-{})", g_round_last, won, frame,
                  g_rounds[0], g_rounds[1]);
    }
    g_round_last = round;
  }
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
    case Phase::Skip:
      // single player: the main menu is the player's (so is a game still elsewhere after a minute)
      if (phase == game::kPhaseMenus || frame - g_since > kSkipGiveUp) {
        REXLOG_INFO("mk9 autopilot: {} at frame {}: the player's controller",
                    phase == game::kPhaseMenus ? "at the main menu" : "no main menu yet", frame);
        Enter(Phase::Off);
        return;
      }
      SkipMovies(frame - g_since, phase);
      // the title: START once it takes presses (it ignores them while it fades in)
      if (game::TitleUp() && (frame - g_since) % 45 >= 30 && (frame - g_since) % 45 < 38) g_pad.buttons = input::kStart;
      return;
    case Phase::Boot:
      if (phase == game::kPhaseMenus) Enter(Phase::ToMenu);
      else if (phase == game::kPhaseTitle && game::TitleUp()) Enter(Phase::Title);
      SkipMovies(frame - g_since, phase);
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
      if (phase != game::kPhaseMenus) {
        REXLOG_WARN("mk9 autopilot: the game left its hold point (phase {}, screen {:X}) at frame {}: back to the main menu",
                    phase, ui, frame);
        Enter(Phase::Boot);
        return;
      }
      if ((frame - g_since) % kHoldTapEvery >= kHoldTapEvery - kHoldTapFrames) g_pad.left_trigger = 0xFF;
      return;
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
        g_steer_slot = 0;
        g_pad_user = 0;
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
        g_steer_slot = 0;
        g_pad_user = 0;
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
  // tests (MK9_AUTOPILOT_ATTACK=1, or 2 for player 2): that player walks in and jabs through every fight, so
  // matches end (and the other player is the loser, who steers the CONTINUE? menu)
  static const int attacker = [] {
    const char* v = std::getenv("MK9_AUTOPILOT_ATTACK");
    return v && (*v == '1' || *v == '2') ? *v - '1' : -1;
  }();
  if (attacker >= 0 && g_phase == Phase::Players && g_plan.slot == attacker && game::Phase() == game::kPhaseFight) {
    out = input::Pad{};
    out.connected = true;
    const uint64_t t = g_frame % 128;
    if (t < 20) out.buttons = attacker == 0 ? input::kDpadRight : input::kDpadLeft;  // player 2 starts on the right
    else if (t >= 20 && (t - 20) % 18 < 12) out.buttons = input::kX;
    return true;
  }
  switch (g_phase) {
    case Phase::Skip:
    case Phase::Title:
    case Phase::Steer:
    case Phase::Pick:
      out = g_pad;
      return true;
    case Phase::Boot:
    case Phase::Hold:
      // the launcher's presses alone: START out of an attract demo's fight, the hold point's trigger taps
      out = g_pad;
      return g_launcher;
    case Phase::ToMenu:
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
