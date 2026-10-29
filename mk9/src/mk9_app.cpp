// mk9 - the app layer, see mk9_app.h.

#include "mk9_app.h"

#include <cstdlib>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>

#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include "mk9/controls.h"
#include "mk9/crash.h"
#include "mk9/diag.h"
#include "mk9/link.h"
#include "mk9/names.h"
#include "mk9/net/netplay.h"
#include "mk9/snap.h"

namespace mk9::synctest {
bool Init();
}
#include "mk9/testinput.h"

// after the SDK's headers: Windows' macros stay out of them
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#pragma comment(lib, "dxgi.lib")

REXCVAR_DECLARE(std::string, game_data_root);
REXCVAR_DECLARE(std::string, user_data_root);
REXCVAR_DECLARE(std::string, cache_root);

namespace fs = std::filesystem;

// The game on a laptop's fast graphics card: NVIDIA's and AMD's drivers for PCs with two (Optimus, PowerXpress) look
// for these in the exe (the SDK has them in rexgpu-xenos.dll, where they do nothing)
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

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

std::string Narrow(const wchar_t* w) {
  char out[256];
  return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, sizeof out, nullptr, nullptr) ? std::string(out) : std::string();
}

// The SDK draws on the first graphics adapter in Windows' list, which on a laptop with two is most often the slow one
// in the processor (the one its screen hangs off): the one Windows counts as the high-performance one instead, when it
// can run the game (Direct3D 12), unless d3d12_adapter says which
void PickFastAdapter() {
  if (rex::cvar::HasNonDefaultValue("d3d12_adapter")) return;
  using CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
  const auto create = d3d12 ? reinterpret_cast<CreateDevice>(reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice")))
                            : nullptr;
  IDXGIFactory6* factory = nullptr;
  if (!create || FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return;
  IDXGIAdapter1* fast = nullptr;
  DXGI_ADAPTER_DESC1 want{};
  if (SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&fast)))) {
    if (SUCCEEDED(fast->GetDesc1(&want)) && !(want.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
        SUCCEEDED(create(fast, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr))) {
      IDXGIAdapter1* a = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d{};
        const bool same = SUCCEEDED(a->GetDesc1(&d)) && d.AdapterLuid.LowPart == want.AdapterLuid.LowPart &&
                          d.AdapterLuid.HighPart == want.AdapterLuid.HighPart;
        a->Release();
        if (!same) continue;
        if (i != 0) {
          REXCVAR_SET(d3d12_adapter, int32_t(i));
          REXLOG_INFO("mk9: graphics on {} (Windows' high-performance adapter, number {} in its list)",
                      Narrow(want.Description), i);
        }
        break;
      }
    }
    fast->Release();
  }
  factory->Release();
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
  PickFastAdapter();
}

void Mk9App::OnPostSetup() {
  REXLOG_INFO("mk9: game data {}, saves {}", game_data_root().string(), user_data_root().string());
  mk9::crash::Install();  // again: the runtime's libraries are loaded now, and one may have put its own in
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

// the players' names over an online fight (mk9/names.h)
void Mk9App::OnConfigureFonts(ImFontAtlas* atlas) { mk9::names::ConfigureFonts(atlas); }
void Mk9App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  mk9::names::UseDrawer(drawer, [this](std::function<void()> task) { app_context().CallInUIThreadDeferred(std::move(task)); });
}
