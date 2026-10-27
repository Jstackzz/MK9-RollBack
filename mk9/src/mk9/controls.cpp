// The launcher's controls file, see controls.h.

#include "mk9/controls.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/logging.h>

#include "mk9/input.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace mk9::controls {
namespace {

namespace fs = std::filesystem;
using mk9::input::Pad;

// the Xbox 360 buttons as the file names them
enum Control { kUp, kDown, kLeft, kRight, kA, kB, kX, kY, kLB, kRB, kLT, kRT, kStart, kBack, kLS, kRS, kCount };
const char* const kControlIds[kCount] = {"up", "down", "left", "right", "a",  "b",     "x",    "y",
                                         "lb", "rb",   "lt",   "rt",    "start", "back", "ls", "rs"};
// their XInput bits (the triggers are analog, 0)
const uint16_t kControlBits[kCount] = {input::kDpadUp,       input::kDpadDown,      input::kDpadLeft,
                                       input::kDpadRight,    input::kA,             input::kB,
                                       input::kX,            input::kY,             input::kLeftShoulder,
                                       input::kRightShoulder, 0,                    0,
                                       input::kStart,        input::kBack,          input::kLeftThumb,
                                       input::kRightThumb};

// a PC controller's buttons as the SDK reports them (in Xbox 360 positions), and what each
// presses unless the file says otherwise
struct SourceButton {
  const char* id;
  uint16_t bit;  // 0: a trigger
  int trigger;   // 0 left, 1 right, -1 none
  int control;
};
const SourceButton kSources[] = {
    {"south", input::kA, -1, kA},
    {"east", input::kB, -1, kB},
    {"west", input::kX, -1, kX},
    {"north", input::kY, -1, kY},
    {"left_shoulder", input::kLeftShoulder, -1, kLB},
    {"right_shoulder", input::kRightShoulder, -1, kRB},
    {"left_trigger", 0, 0, kLT},
    {"right_trigger", 0, 1, kRT},
    {"back", input::kBack, -1, kBack},
    {"start", input::kStart, -1, kStart},
    {"left_stick", input::kLeftThumb, -1, kLS},
    {"right_stick", input::kRightThumb, -1, kRS},
    {"dpad_up", input::kDpadUp, -1, kUp},
    {"dpad_down", input::kDpadDown, -1, kDown},
    {"dpad_left", input::kDpadLeft, -1, kLeft},
    {"dpad_right", input::kDpadRight, -1, kRight},
};
constexpr int kSourceCount = int(sizeof(kSources) / sizeof(kSources[0]));
constexpr uint8_t kTriggerPressed = 30;

struct Map {
  bool keyboard = true;
  std::array<std::vector<int>, kCount> keys;
  std::array<int, kSourceCount> presses{};
};

Map Defaults() {
  Map m;
#if defined(_WIN32)
  m.keys[kUp] = {VK_UP, 'W'};
  m.keys[kDown] = {VK_DOWN, 'S'};
  m.keys[kLeft] = {VK_LEFT, 'A'};
  m.keys[kRight] = {VK_RIGHT, 'D'};
  m.keys[kX] = {'J'};        // MK9: 1, front punch
  m.keys[kY] = {'I'};        // 2, back punch
  m.keys[kA] = {'K'};        // 3, front kick
  m.keys[kB] = {'L'};        // 4, back kick
  m.keys[kRT] = {VK_SPACE};  // block
  m.keys[kRB] = {'O'};       // throw
  m.keys[kLB] = {'U'};       // tag
  m.keys[kLT] = {'P'};       // flip stance
  m.keys[kStart] = {VK_RETURN};
  m.keys[kBack] = {VK_BACK};
#endif
  for (int i = 0; i < kSourceCount; i++) m.presses[i] = kSources[i].control;
  return m;
}

std::mutex g_lock;
Map g_map = Defaults();
fs::file_time_type g_stamp{};
bool g_have_stamp = false;
std::chrono::steady_clock::time_point g_next_check{};

int ControlFromId(const std::string& id) {
  for (int c = 0; c < kCount; c++)
    if (id == kControlIds[c]) return c;
  return -1;
}

// key names as the launcher writes them (launcher/src/Controls.cpp keyName)
int KeyFromName(const std::string& n) {
#if defined(_WIN32)
  if (n.size() == 1) {
    char c = n[0];
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
  }
  if (n.size() >= 2 && (n[0] == 'F' || n[0] == 'f')) {
    int f = std::atoi(n.c_str() + 1);
    if (f >= 1 && f <= 24) return VK_F1 + f - 1;
  }
  if (n.rfind("0x", 0) == 0 || n.rfind("0X", 0) == 0) return int(std::strtol(n.c_str() + 2, nullptr, 16));
  struct Named {
    const char* name;
    int vk;
  };
  static const Named kNamed[] = {
      {"Up", VK_UP},           {"Down", VK_DOWN},         {"Left", VK_LEFT},
      {"Right", VK_RIGHT},     {"Space", VK_SPACE},       {"Return", VK_RETURN},
      {"Enter", VK_RETURN},    {"Backspace", VK_BACK},    {"Tab", VK_TAB},
      {"LShift", VK_LSHIFT},   {"RShift", VK_RSHIFT},     {"LCtrl", VK_LCONTROL},
      {"RCtrl", VK_RCONTROL},  {"Shift", VK_SHIFT},       {"Ctrl", VK_CONTROL},
      {"Insert", VK_INSERT},   {"Delete", VK_DELETE},     {"Home", VK_HOME},
      {"End", VK_END},         {"PageUp", VK_PRIOR},      {"PageDown", VK_NEXT},
      {"Num0", VK_NUMPAD0},    {"Num1", VK_NUMPAD1},      {"Num2", VK_NUMPAD2},
      {"Num3", VK_NUMPAD3},    {"Num4", VK_NUMPAD4},      {"Num5", VK_NUMPAD5},
      {"Num6", VK_NUMPAD6},    {"Num7", VK_NUMPAD7},      {"Num8", VK_NUMPAD8},
      {"Num9", VK_NUMPAD9},    {"NumAdd", VK_ADD},        {"NumSub", VK_SUBTRACT},
      {"NumMul", VK_MULTIPLY}, {"NumDiv", VK_DIVIDE},     {"NumDot", VK_DECIMAL},
      {"Semicolon", VK_OEM_1}, {"Slash", VK_OEM_2},       {"Grave", VK_OEM_3},
      {"LBracket", VK_OEM_4},  {"Backslash", VK_OEM_5},   {"RBracket", VK_OEM_6},
      {"Quote", VK_OEM_7},     {"Comma", VK_OEM_COMMA},   {"Period", VK_OEM_PERIOD},
      {"Minus", VK_OEM_MINUS}, {"Equals", VK_OEM_PLUS},
  };
  for (const Named& k : kNamed)
    if (_stricmp(k.name, n.c_str()) == 0) return k.vk;
#endif
  return 0;
}

std::string Trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string StripComment(const std::string& line) {
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    if (line[i] == '"') quoted = !quoted;
    if (line[i] == '#' && !quoted) return line.substr(0, i);
  }
  return line;
}

std::vector<std::string> Quoted(const std::string& v) {
  std::vector<std::string> out;
  size_t i = 0;
  while ((i = v.find('"', i)) != std::string::npos) {
    size_t j = v.find('"', i + 1);
    if (j == std::string::npos) break;
    out.push_back(v.substr(i + 1, j - i - 1));
    i = j + 1;
  }
  return out;
}

bool Load(const fs::path& path, Map& m) {
  std::ifstream f(path);
  if (!f) return false;
  m = Defaults();
  std::string section, line;
  bool keys_cleared[kCount] = {};
  while (std::getline(f, line)) {
    line = Trim(StripComment(line));
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      section = Trim(line.substr(1, line.size() - 2));
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = Trim(line.substr(0, eq)), value = Trim(line.substr(eq + 1));
    if (section == "keyboard" && key == "enabled") {
      m.keyboard = value != "false";
    } else if (section == "keys") {
      int c = ControlFromId(key);
      if (c < 0) continue;
      if (!keys_cleared[c]) {
        m.keys[c].clear();
        keys_cleared[c] = true;
      }
      for (const std::string& name : Quoted(value)) {
        int vk = KeyFromName(name);
        if (vk && m.keys[c].size() < 2) m.keys[c].push_back(vk);
      }
    } else if (section == "buttons") {
      for (int i = 0; i < kSourceCount; i++) {
        if (key != kSources[i].id) continue;
        auto names = Quoted(value);
        m.presses[i] = names.empty() || names[0].empty() ? -1 : ControlFromId(names[0]);
      }
    }
  }
  return true;
}

void ReloadIfChanged(bool force) {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(g_lock);
  if (!force && now < g_next_check) return;
  g_next_check = now + std::chrono::seconds(1);
  fs::path path = FilePath();
  std::error_code ec;
  auto stamp = fs::last_write_time(path, ec);
  if (ec) {
    if (g_have_stamp) {
      g_map = Defaults();
      g_have_stamp = false;
    }
    return;
  }
  if (g_have_stamp && stamp == g_stamp) return;
  Map m;
  if (Load(path, m)) {
    g_map = m;
    g_stamp = stamp;
    g_have_stamp = true;
    REXLOG_INFO("mk9: controls from {}", path.string());
  }
}

#if defined(_WIN32)
bool GameInFront() {
  HWND fg = GetForegroundWindow();
  if (!fg) return false;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}
#endif

}  // namespace

std::string FilePath() {
  if (const char* e = std::getenv("MK9_CONTROLS"); e && *e) return e;
  return (rex::filesystem::GetExecutableFolder() / "controls.toml").string();
}

void Poll() { ReloadIfChanged(true); }

void Apply(Pad& pad) {
  ReloadIfChanged(false);
  Map m;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    m = g_map;
  }
  uint16_t buttons = 0;
  uint8_t triggers[2] = {0, 0};
  for (int i = 0; i < kSourceCount; i++) {
    const SourceButton& s = kSources[i];
    bool down;
    uint8_t analog = 0xFF;
    if (s.trigger >= 0) {
      analog = s.trigger == 0 ? pad.left_trigger : pad.right_trigger;
      down = analog >= kTriggerPressed;
    } else {
      down = (pad.buttons & s.bit) != 0;
    }
    int target = m.presses[i];
    if (!down || target < 0) continue;
    if (target == kLT || target == kRT) {
      uint8_t& t = triggers[target == kLT ? 0 : 1];
      t = std::max<uint8_t>(t, s.trigger >= 0 ? analog : 0xFF);
    } else {
      buttons |= kControlBits[target];
    }
  }
  pad.buttons = uint16_t((pad.buttons & input::kGuide) | buttons);
  pad.left_trigger = triggers[0];
  pad.right_trigger = triggers[1];
}

void AddKeyboard(Pad& pad) {
#if defined(_WIN32)
  Map m;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    m = g_map;
  }
  if (!m.keyboard) return;
  // a keyboard is always there: the game keeps seeing a controller on user 0
  pad.connected = true;
  if (!GameInFront()) return;
  for (int c = 0; c < kCount; c++) {
    bool down = false;
    for (int vk : m.keys[c]) down = down || (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!down) continue;
    if (c == kLT) {
      pad.left_trigger = 0xFF;
    } else if (c == kRT) {
      pad.right_trigger = 0xFF;
    } else {
      pad.buttons |= kControlBits[c];
    }
  }
#else
  (void)pad;
#endif
}

}  // namespace mk9::controls
