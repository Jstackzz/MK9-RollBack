// The players' names over an online fight (names.h). At the end of every frame the main thread reads from
// the game where the fight's health bars are; the overlay draws from that copy on the UI thread, in the
// frame's place in the window (the presenter shows the game's 16:9 frame letterboxed in the window:
// rex ui/presenter.cpp).
//
// The fight's HUD: 0x833CED48 holds the object sub_8243D050 builds as a fight's first round begins (0 before,
// and again once sub_8243A2C0 has taken it down). Its first words are where its top bar is: +4 the bar's
// offset down the frame's 720 lines, a float, -146 out of sight above it, 0 in place; the bar slides in at 3
// lines a frame as ROUND 1 is called.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk9/names.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <imgui.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>

#include "mk9/game.h"

namespace mk9::names {
namespace {

constexpr uint32_t kFightHud = 0x833CED48;
constexpr float kFontPixels = 64.0f;  // the atlas' size: sharp up to a 4K window
// in the frame's 1280x720 pixels, the top bar in place (its top at line 35, the timer's ring from 600 to
// 672): P1's name starts where the left bar's lettering does and P2's ends where the right bar's does, both
// standing on the line just above the bars, and a name too long for its room (up to the timer's) is drawn
// smaller to fit
constexpr float kP1Start = 145, kP1End = 592, kP2Start = 682, kP2End = 1132, kLine = 33;
constexpr float kTextHeight = 26;  // the names' line height: capitals about 14 lines tall, as the bars' own
constexpr int kSettleFrames = 3, kFadeInFrames = 15;

std::mutex g_lock;
std::string g_match[2], g_test[2];  // the match's (the link's), the test switch's
// this frame's, copied under g_lock: how far the names show (0 to 1), how far down the bar is
float g_shade = 0, g_offset = 0;
ImFont* g_font = nullptr;
// main thread only
int g_still = 0;  // frames the bar has been in place
int g_shown = 0;  // frames since the names came up, 0: not up

float F32(uint32_t address) {
  const uint32_t v = game::ReadU32(address);
  float f;
  std::memcpy(&f, &v, sizeof f);
  return f;
}

class Overlay final : public rex::ui::ImGuiDialog {
 public:
  explicit Overlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  // it changes only with the game's frames, each of which repaints it
  bool RepaintsContinuously() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    std::string names[2];
    float shade, offset;
    {
      std::lock_guard<std::mutex> lock(g_lock);
      const std::string* source = g_match[0].empty() && g_match[1].empty() ? g_test : g_match;
      if (g_shade <= 0 || (source[0].empty() && source[1].empty())) return;
      names[0] = source[0];
      names[1] = source[1];
      shade = g_shade;
      offset = g_offset;
    }
    // the frame's place in the window: 16:9, as large as fits, centred
    const float fw = std::min(io.DisplaySize.x, io.DisplaySize.y * 16.0f / 9.0f), fh = fw * 9.0f / 16.0f;
    const float fx = (io.DisplaySize.x - fw) / 2, fy = (io.DisplaySize.y - fh) / 2;
    const float sx = fw / 1280.0f, sy = fh / 720.0f;
    ImFont* font = g_font ? g_font : ImGui::GetFont();
    const int alpha = int(std::lround(255.0f * std::clamp(shade, 0.0f, 1.0f)));
    if (alpha == 0) return;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();  // under any ImGui window (the SDK's overlays)
    for (int side = 0; side < 2; side++) {
      if (names[side].empty()) continue;
      const char* text = names[side].c_str();
      float size = kTextHeight * sy;
      ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      // a long name (16 wide letters) shrinks to its room rather than run into the timer
      const float room = (side == 0 ? kP1End - kP1Start : kP2End - kP2Start) * sx;
      if (extent.x > room) {
        size *= room / extent.x;
        extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
      }
      const float outline = std::max(1.0f, size / 12.0f);
      const float x = std::floor(side == 0 ? fx + kP1Start * sx : fx + kP2End * sx - extent.x);
      const float y = std::floor(fy + (kLine + offset) * sy - extent.y);  // standing on the line
      // a dark outline, then the letters: readable over any arena, like the game's own outlined lettering
      for (int dx = -1; dx <= 1; dx++)
        for (int dy = -1; dy <= 1; dy++)
          if (dx || dy)
            draw->AddText(font, size, ImVec2(x + dx * outline, y + dy * outline),
                          IM_COL32(0, 0, 0, alpha * 230 / 255), text);
      draw->AddText(font, size, ImVec2(x, y), IM_COL32(245, 245, 245, alpha), text);
    }
    if (!drawn_) {  // once each time the overlay opens: the names are on the screen (a test reads it)
      drawn_ = true;
      REXLOG_INFO("mk9 names: drawn over the fight");
    }
  }

 private:
  bool drawn_ = false;
};

// The overlay is open only while names are up: an open ImGui dialog makes the presenter paint every frame on the
// UI thread instead of straight from the guest output thread (rex ui/presenter.cpp
// GetDesiredPaintModeFromUIThread), which outside a fight with names costs for nothing.
rex::ui::ImGuiDrawer* g_drawer = nullptr;                    // UI thread
std::function<void(std::function<void()>)> g_in_ui_thread;  // the app's, set once before any frame
std::unique_ptr<Overlay> g_overlay;                          // UI thread
std::atomic<bool> g_open{false};                             // the main thread's: the overlay wanted open

// UI thread: the overlay as the main thread last wanted it
void Follow() {
  if (g_open && !g_overlay && g_drawer) g_overlay = std::make_unique<Overlay>(g_drawer);
  else if (!g_open && g_overlay) g_overlay.reset();
}

}  // namespace

void SetPlayers(const std::string& p1, const std::string& p2) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (p1 == g_match[0] && p2 == g_match[1]) return;
  g_match[0] = p1;
  g_match[1] = p2;
  if (p1.empty() && p2.empty()) REXLOG_INFO("mk9 names: none");
  else REXLOG_INFO("mk9 names: {} and {} over the fight", p1.empty() ? "-" : p1, p2.empty() ? "-" : p2);
}

void OnFrame(uint64_t) {
  static const bool test = [] {
    const char* v = std::getenv("MK9_TEST_NAMES");
    if (!v || !*v) return false;
    const char* bar = std::strchr(v, '|');
    std::lock_guard<std::mutex> lock(g_lock);
    g_test[0] = bar ? std::string(v, bar) : std::string(v);
    g_test[1] = bar ? std::string(bar + 1) : std::string();
    return true;
  }();
  (void)test;
  // up in a fight's rounds, with its top bar in place a few frames; the names then fade in, and go with the
  // bar when it slides away, or at once when the fight is over (its finisher: the game's phase moves on)
  const uint32_t hud = game::Phase() == game::kPhaseFight ? game::ReadU32(kFightHud) : 0;
  const float offset = hud ? F32(hud + 4) : -FLT_MAX;
  const bool up = offset > -kLine && offset <= 0;
  g_still = up && offset == 0 ? g_still + 1 : 0;
  if (!up) g_shown = 0;
  else if (g_shown || g_still >= kSettleFrames) g_shown = std::min(g_shown + 1, kFadeInFrames);
  std::lock_guard<std::mutex> lock(g_lock);
  // once a fight: the names came up (a test reads it; the game's own snapshots leave the overlay out)
  static uint32_t logged_hud = 0;
  if (g_shown == 1 && hud != logged_hud && !(g_match[0].empty() && g_match[1].empty() && g_test[0].empty() &&
                                             g_test[1].empty())) {
    logged_hud = hud;
    REXLOG_INFO("mk9 names: up over the fight (round {})", game::Round());
  }
  g_shade = float(g_shown) / kFadeInFrames;
  g_offset = up ? offset : 0;
  const bool open = g_shown > 0 && !(g_match[0].empty() && g_match[1].empty() && g_test[0].empty() && g_test[1].empty());
  if (open != g_open.exchange(open) && g_in_ui_thread) g_in_ui_thread(Follow);
}

void ConfigureFonts(ImFontAtlas* atlas) {
  // Segoe UI Black Italic, heavy and slanted like the game's own HUD lettering, else Bold Italic, else
  // Bold; the SDK's default font stays the first
  if (atlas->Fonts.empty()) atlas->AddFontDefault();
  char dir[MAX_PATH];
  const UINT n = GetWindowsDirectoryA(dir, MAX_PATH);
  if (!n || n >= MAX_PATH - 32) return;
  for (const char* file : {"seguibli.ttf", "segoeuiz.ttf", "segoeuib.ttf"}) {
    char path[MAX_PATH];
    std::snprintf(path, sizeof path, "%s\\Fonts\\%s", dir, file);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
    if ((g_font = atlas->AddFontFromFileTTF(path, kFontPixels))) break;
  }
}

void UseDrawer(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> in_ui_thread) {
  g_drawer = drawer;
  g_in_ui_thread = std::move(in_ui_thread);
}

}  // namespace mk9::names
