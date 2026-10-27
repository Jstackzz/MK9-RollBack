// mk9 - the app layer, see mk9_app.h.

#include "mk9_app.h"

#include <cstdlib>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "mk9/controls.h"
#include "mk9/diag.h"
#include "mk9/link.h"
#include "mk9/net/netplay.h"
#include "mk9/snap.h"

namespace mk9::synctest {
bool Init();
}
#include "mk9/testinput.h"

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, cache_root);

namespace fs = std::filesystem;

namespace {

// Window::presenter() is protected; a member pointer taken through a derived class reaches it
struct WindowPresenter : rex::ui::Window {
  static rex::ui::Presenter* Of(rex::ui::Window* window) {
    return window ? (window->*(&WindowPresenter::presenter))() : nullptr;
  }
};

fs::path EnvPath(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? fs::path(v) : fs::path();
}

}  // namespace

// Defaults, unless the command line (--game_data_root=...) or the environment says otherwise:
//   game data  <exe>\game (a player package: the launcher extracts the disc there), MK9_GAME_DIR
//   saves      <exe>\user, MK9_USER_DIR; the shader cache in <saves>\cache
void Mk9App::OnConfigurePaths(rex::PathConfig& paths) {
  const fs::path exe = rex::filesystem::GetExecutableFolder();
  if (REXCVAR_GET(game_data_root).empty()) {
    if (auto env = EnvPath("MK9_GAME_DIR"); !env.empty()) {
      paths.game_data_root = env;
    } else if (fs::exists(exe / "game" / "default.xex")) {
      paths.game_data_root = exe / "game";
    }
  }
  if (REXCVAR_GET(user_data_root).empty()) {
    auto env = EnvPath("MK9_USER_DIR");
    paths.user_data_root = env.empty() ? exe / "user" : env;
  }
  if (REXCVAR_GET(cache_root).empty()) {
    paths.cache_root = paths.user_data_root / "cache";
  }
}

void Mk9App::OnPreSetup(rex::RuntimeConfig& config) {
  // The game draws through the Xenos GPU emulation; the SDK leaves it off unless asked.
  if (config.gpu_plugin.empty()) {
    config.gpu_plugin = "xenos";
  }
}

void Mk9App::OnPostSetup() {
  REXLOG_INFO("mk9: game data {}, saves {}", game_data_root().string(), user_data_root().string());
  mk9::controls::Poll();
  mk9::testinput::Init();
  mk9::net::InitFromEnvironment();
  mk9::link::Init();
  mk9::synctest::Init();  // a sync test takes the frame hooks over from netplay
  mk9::diag::Init();
  mk9::snap::Init();
  mk9::snap::SetCapture([this](mk9::snap::Image& out) {
    auto* presenter = WindowPresenter::Of(window());
    if (!presenter) return false;
    rex::ui::RawImage raw;
    if (!presenter->CaptureGuestOutput(raw)) return false;
    out.width = raw.width;
    out.height = raw.height;
    out.stride = raw.stride;
    out.data = std::move(raw.data);
    return true;
  });
}

void Mk9App::OnShutdown() {}
