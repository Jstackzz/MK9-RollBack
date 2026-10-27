// Scripted controllers for tests, see testinput.h.

#include "mk9/testinput.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "mk9/frame.h"
#include "mk9/input.h"

namespace mk9::testinput {
namespace {

using mk9::input::Pad;

struct Step {
  uint64_t frame;
  Pad pad;
};

std::array<std::vector<Step>, 4> g_script;
std::array<bool, 4> g_scripted{};
int g_pads = 0;
bool g_no_physical = false;
bool g_active = false;

bool ParseButtons(const std::string& s, Pad& pad) {
  pad = Pad{};
  pad.connected = true;
  if (s == "-" || s.empty()) return true;
  std::stringstream ss(s);
  std::string name;
  while (std::getline(ss, name, '+')) {
    struct Named {
      const char* name;
      uint16_t bit;
    };
    static const Named kNames[] = {
        {"UP", input::kDpadUp},         {"DOWN", input::kDpadDown},
        {"LEFT", input::kDpadLeft},     {"RIGHT", input::kDpadRight},
        {"A", input::kA},               {"B", input::kB},
        {"X", input::kX},               {"Y", input::kY},
        {"LB", input::kLeftShoulder},   {"RB", input::kRightShoulder},
        {"START", input::kStart},       {"BACK", input::kBack},
        {"LS", input::kLeftThumb},      {"RS", input::kRightThumb},
    };
    bool known = false;
    for (const Named& n : kNames) {
      if (_stricmp(n.name, name.c_str()) == 0) {
        pad.buttons |= n.bit;
        known = true;
      }
    }
    if (_stricmp(name.c_str(), "LT") == 0) {
      pad.left_trigger = 0xFF;
      known = true;
    }
    if (_stricmp(name.c_str(), "RT") == 0) {
      pad.right_trigger = 0xFF;
      known = true;
    }
    if (!known) return false;
  }
  return true;
}

bool ParseScript(const char* text, std::vector<Step>& out) {
  std::stringstream ss(text);
  std::string item;
  uint64_t last = 0;
  while (std::getline(ss, item, ',')) {
    size_t colon = item.find(':');
    if (colon == std::string::npos) return false;
    std::string when = item.substr(0, colon);
    uint64_t frame = 0;
    if (!when.empty() && when[0] == '+') {
      frame = last + std::strtoull(when.c_str() + 1, nullptr, 10);
    } else {
      frame = std::strtoull(when.c_str(), nullptr, 10);
    }
    Step step{frame, {}};
    if (!ParseButtons(item.substr(colon + 1), step.pad)) return false;
    out.push_back(step);
    last = frame;
  }
  return true;
}

Pad ScriptAt(uint32_t user, uint64_t now) {
  Pad pad{};
  pad.connected = true;
  for (const Step& s : g_script[user]) {
    if (s.frame > now) break;
    pad = s.pad;
  }
  return pad;
}

bool Source(uint32_t user, Pad& out) {
  if (user >= 4) return false;
  if (g_scripted[user]) {
    out = ScriptAt(user, frame::Count());
    return true;
  }
  if (int(user) < g_pads) {
    out = g_no_physical ? Pad{} : input::ReadPhysical(user);
    out.connected = true;
    return true;
  }
  if (g_no_physical) {
    out = Pad{};
    return true;
  }
  return false;
}

}  // namespace

void Init() {
  for (int u = 0; u < 4; u++) {
    char name[16];
    std::snprintf(name, sizeof name, "MK9_INPUT_P%d", u + 1);
    const char* text = std::getenv(name);
    if (!text || !*text) continue;
    if (!ParseScript(text, g_script[u])) {
      REXLOG_ERROR("mk9: {} could not be read: {}", name, text);
      g_script[u].clear();
      continue;
    }
    g_scripted[u] = true;
    g_active = true;
    REXLOG_INFO("mk9: user {} plays a script of {} steps", u, g_script[u].size());
  }
  if (const char* p = std::getenv("MK9_PADS"); p && *p) {
    g_pads = std::atoi(p);
    g_active = true;
  }
  if (const char* p = std::getenv("MK9_NO_PHYSICAL"); p && *p == '1') {
    g_no_physical = true;
    g_active = true;
  }
  if (g_active) input::SetSource(&Source);
}

bool Active() { return g_active; }

bool ScriptedPad(uint32_t user, Pad& out) {
  if (user >= 4 || !g_scripted[user]) return false;
  out = ScriptAt(user, frame::Count());
  return true;
}

}  // namespace mk9::testinput
