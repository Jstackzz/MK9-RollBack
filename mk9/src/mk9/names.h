#pragma once
// The players' names over an online fight, and over one watched: each player's account name stands just
// above their health bar, where the game draws nothing, P1's from where the left bar starts and P2's to
// where the right bar ends, and only while the fight's health bars are up. The launcher's start line
// carries the names (p1_name, p2_name: docs/PROTOCOL.md), which the link hands over for an online or a
// watched match. MK9_TEST_NAMES=Kitana|Sindel shows names in any fight (screenshots of an offline one).
#include <cstdint>
#include <string>

struct ImFontAtlas;
namespace rex::ui {
class ImGuiDrawer;
}

namespace mk9::names {

// a match's two names, as the launcher's start line gives them; empty: none (an offline match, parked)
void SetPlayers(const std::string& p1, const std::string& p2);

// at the end of each frame, on the main thread: where the bars are, read from the game
void OnFrame(uint64_t frame);

// Mk9App: the font (before the renderer builds the atlas), and the overlay that draws the names
void ConfigureFonts(ImFontAtlas* atlas);
void CreateOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace mk9::names
