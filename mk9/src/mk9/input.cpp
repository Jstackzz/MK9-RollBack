// The game's controllers, see input.h. Replaces the SDK's XamInputGetState and
// XamInputGetCapabilities (strong definitions here win over the runtime DLL's for every call
// the recompiled code makes).

#include "mk9/input.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <mutex>

#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>

#include "mk9/controls.h"
#include "mk9/frame.h"
#include "mk9/net/rollback.h"

namespace mk9::input {
namespace {

constexpr uint32_t kFlagGamepad = 0x01;
constexpr uint32_t kFlagAnyUser = 1u << 30;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorBadArguments = 0xA0;

std::atomic<Source> g_source{nullptr};
std::atomic<Source> g_override{nullptr};
std::array<std::atomic<uint64_t>, 4> g_polls{};

// the packet number XInput reports changes whenever the state does
std::mutex g_packet_lock;
std::array<Pad, 4> g_last{};
std::array<uint32_t, 4> g_packet{};
std::array<Pad, 4> g_read{};  // each user's last answer

rex::input::InputSystem* System() {
  auto* ks = REX_KERNEL_STATE();
  if (!ks || !ks->emulator()) return nullptr;
  return static_cast<rex::input::InputSystem*>(ks->emulator()->input_system());
}

uint32_t UserIndex(uint32_t user_index, uint32_t flags) {
  if ((user_index & 0xFF) == 0xFF || (flags & kFlagAnyUser)) return 0;
  return user_index;
}

bool Same(const Pad& a, const Pad& b) {
  return a.connected == b.connected && a.buttons == b.buttons && a.left_trigger == b.left_trigger &&
         a.right_trigger == b.right_trigger && a.lx == b.lx && a.ly == b.ly && a.rx == b.rx &&
         a.ry == b.ry && a.packet == b.packet;
}

Pad Current(uint32_t user) {
  Pad pad;
  if (user >= 4) return pad;
  Source over = g_override.load(std::memory_order_acquire);
  if (over && over(user, pad)) return pad;
  Source source = g_source.load(std::memory_order_acquire);
  if (source && source(user, pad)) return pad;
  return ReadPhysical(user);
}

}  // namespace

Pad ReadPhysical(uint32_t user) {
  Pad pad;
  auto* is = System();
  if (!is || user >= 4) return pad;
  rex::input::X_INPUT_STATE state{};
  if (is->GetState(user, &state) == kErrorSuccess) {
    pad.connected = true;
    pad.buttons = state.gamepad.buttons;
    pad.left_trigger = state.gamepad.left_trigger;
    pad.right_trigger = state.gamepad.right_trigger;
    pad.lx = state.gamepad.thumb_lx;
    pad.ly = state.gamepad.thumb_ly;
    pad.rx = state.gamepad.thumb_rx;
    pad.ry = state.gamepad.thumb_ry;
    controls::Apply(pad);
  } else {
    controls::Poll();
  }
  // the keyboard is player 1's controller too (and keeps user 0 connected without a pad)
  if (user == 0) controls::AddKeyboard(pad);
  return pad;
}

void SetSource(Source source) { g_source.store(source, std::memory_order_release); }
void SetOverride(Source over) { g_override.store(over, std::memory_order_release); }

uint64_t Polls(uint32_t user) { return user < 4 ? g_polls[user].load() : 0; }

Pad LastRead(uint32_t user) {
  std::lock_guard<std::mutex> lock(g_packet_lock);
  return user < 4 ? g_read[user] : Pad{};
}

}  // namespace mk9::input

namespace {

using namespace mk9::input;
using rex::input::X_INPUT_CAPABILITIES;
using rex::input::X_INPUT_STATE;

uint32_t Mk9XamInputGetState(rex::u32 user_index, rex::u32 flags,
                             ppc_ptr_t<X_INPUT_STATE> input_state) {
  if ((flags & 0xFF) && (flags & kFlagGamepad) == 0) return kErrorDeviceNotConnected;
  const uint32_t user = UserIndex(user_index, flags);
  if (user >= 4) return kErrorDeviceNotConnected;
  if (g_polls[user].fetch_add(1, std::memory_order_relaxed) == 0) {
    // netplay needs the pads read by the game's main thread, inside the frame
    REXLOG_INFO("mk9: user {} first polled on the {} thread (frame {})", user,
                mk9::frame::OnMainThread() ? "main" : "another", mk9::frame::Count());
  }
  const Pad pad = Current(user);
  {
    std::lock_guard<std::mutex> lock(g_packet_lock);
    g_read[user] = pad;
  }
  if (!pad.connected) return kErrorDeviceNotConnected;
  if (input_state) {
    uint32_t packet;
    {
      std::lock_guard<std::mutex> lock(g_packet_lock);
      if (pad.packet) {
        // the source numbers them (netplay); once it stops, the count goes on from its last one
        g_packet[user] = std::max(g_packet[user], pad.packet);
        g_last[user] = pad;
      } else if (!Same(pad, g_last[user])) {
        g_last[user] = pad;
        g_packet[user]++;
      }
      packet = pad.packet ? pad.packet : g_packet[user];
    }
    input_state->packet_number = packet;
    input_state->gamepad.buttons = pad.buttons;
    input_state->gamepad.left_trigger = pad.left_trigger;
    input_state->gamepad.right_trigger = pad.right_trigger;
    input_state->gamepad.thumb_lx = pad.lx;
    input_state->gamepad.thumb_ly = pad.ly;
    input_state->gamepad.thumb_rx = pad.rx;
    input_state->gamepad.thumb_ry = pad.ry;
  }
  return kErrorSuccess;
}

uint32_t Caps(uint32_t user, X_INPUT_CAPABILITIES* caps) {
  if (!caps) return kErrorBadArguments;
  if (user >= 4 || !Current(user).connected) return kErrorDeviceNotConnected;
  // a wired Xbox 360 gamepad with every button and both sticks
  std::memset(caps, 0, sizeof(*caps));
  caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  caps->gamepad.buttons = 0xF3FF;
  caps->gamepad.left_trigger = 0xFF;
  caps->gamepad.right_trigger = 0xFF;
  caps->gamepad.thumb_lx = int16_t(0xFFC0);
  caps->gamepad.thumb_ly = int16_t(0xFFC0);
  caps->gamepad.thumb_rx = int16_t(0xFFC0);
  caps->gamepad.thumb_ry = int16_t(0xFFC0);
  return kErrorSuccess;
}

uint32_t Mk9XamInputGetCapabilities(rex::u32 user_index, rex::u32 flags,
                                    ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  if ((flags & 0xFF) && (flags & kFlagGamepad) == 0) return kErrorDeviceNotConnected;
  return Caps(UserIndex(user_index, flags), caps);
}

uint32_t Mk9XamInputGetCapabilitiesEx(rex::u32 unk, rex::u32 user_index, rex::u32 flags,
                                      ppc_ptr_t<X_INPUT_CAPABILITIES> caps) {
  (void)unk;
  if ((flags & 0xFF) && (flags & kFlagGamepad) == 0) return kErrorDeviceNotConnected;
  return Caps(UserIndex(user_index, flags), caps);
}

// XamInputGetKeystrokeEx: some menus (the CONTINUE? menu after a match among them) read key
// presses rather than the pad's state. They come from the same pads XamInputGetState answers with
// (netplay's, the autopilot's, scripts', the PC's own), one event per call: a key goes up or down
// when its button's state changed since that user's last event; a held key repeats after 24
// frames, every 6 (counted in frames, so every PC in a match repeats alike).
constexpr uint32_t kErrorEmpty = 0x10D2;
constexpr uint16_t kKeyDown = 0x0001, kKeyUp = 0x0002, kKeyRepeat = 0x0004;
struct KeyDef {
  uint16_t vk;
  uint16_t button;  // XINPUT_GAMEPAD_* bit, or 0 for the stick directions below
  int stick;        // 1 up, 2 down, 3 right, 4 left (the left stick)
};
constexpr KeyDef kKeys[] = {
    {0x5800, kA, 0},          {0x5801, kB, 0},          {0x5802, kX, 0},
    {0x5803, kY, 0},          {0x5804, kRightShoulder, 0}, {0x5805, kLeftShoulder, 0},
    {0x5810, kDpadUp, 0},     {0x5811, kDpadDown, 0},   {0x5812, kDpadLeft, 0},
    {0x5813, kDpadRight, 0},  {0x5814, kStart, 0},      {0x5815, kBack, 0},
    {0x5816, kLeftThumb, 0},  {0x5817, kRightThumb, 0}, {0x5806, 0, 5},
    {0x5807, 0, 6},           {0x5820, 0, 1},           {0x5821, 0, 2},
    {0x5822, 0, 3},           {0x5823, 0, 4},
};
constexpr int kKeyCount = int(sizeof kKeys / sizeof kKeys[0]);

struct KeyUser {
  uint32_t held = 0;            // keys down as last reported
  uint64_t down_at[kKeyCount] = {};
  uint64_t repeat_at[kKeyCount] = {};
};
std::array<KeyUser, 4> g_keys;
std::mutex g_keys_lock;

// what the keys had reported at the top of each frame rollback keeps, before the frame reads them: a
// frame run again reads them again (mk9/frame.cpp), and must see the same key events as the first time
std::map<uint32_t, std::array<KeyUser, 4>> g_keys_kept;

void KeepKeys(uint32_t frame) {
  std::lock_guard<std::mutex> lock(g_keys_lock);
  // what is kept from this frame on is a frame being run again or an earlier match's (each match numbers its
  // frames from 0, and trimming from the front would otherwise throw this frame's away)
  g_keys_kept.erase(g_keys_kept.lower_bound(frame), g_keys_kept.end());
  g_keys_kept[frame] = g_keys;
  while (g_keys_kept.size() > size_t(mk9::rb::kKeepFrames) + 4) g_keys_kept.erase(g_keys_kept.begin());
}

void KeysBack(uint32_t frame) {
  std::lock_guard<std::mutex> lock(g_keys_lock);
  auto it = g_keys_kept.find(frame);
  if (it == g_keys_kept.end()) return;
  g_keys = it->second;
  g_keys_kept.erase(std::next(it), g_keys_kept.end());
}

const bool g_keys_rolled_back = [] {
  mk9::rb::OnOpen(&KeepKeys);
  mk9::rb::OnRestore(&KeysBack);
  return true;
}();

uint32_t KeyMask(const Pad& p) {
  uint32_t m = 0;
  for (int i = 0; i < kKeyCount; i++) {
    const KeyDef& k = kKeys[i];
    bool on = false;
    if (k.button) on = (p.buttons & k.button) != 0;
    else if (k.stick == 1) on = p.ly > 16000;
    else if (k.stick == 2) on = p.ly < -16000;
    else if (k.stick == 3) on = p.lx > 16000;
    else if (k.stick == 4) on = p.lx < -16000;
    else if (k.stick == 5) on = p.left_trigger > 30;
    else if (k.stick == 6) on = p.right_trigger > 30;
    if (on) m |= 1u << i;
  }
  return m;
}

// the next event of one user, if any
bool NextKey(uint32_t user, rex::input::X_INPUT_KEYSTROKE& out) {
  const Pad pad = Current(user);
  if (!pad.connected) return false;
  const uint32_t now = KeyMask(pad);
  const uint64_t frame = mk9::frame::Count();
  KeyUser& u = g_keys[user];
  const uint32_t changed = now ^ u.held;
  // released keys first, then pressed ones, then repeats
  for (int pass = 0; pass < 3; pass++) {
    for (int i = 0; i < kKeyCount; i++) {
      const uint32_t bit = 1u << i;
      uint16_t flags = 0;
      if (pass == 0 && (changed & bit) && !(now & bit)) {
        flags = kKeyUp;
        u.held &= ~bit;
      } else if (pass == 1 && (changed & bit) && (now & bit)) {
        flags = kKeyDown;
        u.held |= bit;
        u.down_at[i] = frame;
        u.repeat_at[i] = frame + 24;
      } else if (pass == 2 && (now & bit) && (u.held & bit) && frame >= u.repeat_at[i]) {
        flags = kKeyDown | kKeyRepeat;
        u.repeat_at[i] = frame + 6;
      } else {
        continue;
      }
      out.virtual_key = kKeys[i].vk;
      out.unicode = 0;
      out.flags = flags;
      out.user_index = uint8_t(user);
      out.hid_code = 0;
      return true;
    }
  }
  return false;
}

uint32_t Mk9XamInputGetKeystrokeEx(mapped_u32 user_index_ptr, rex::u32 flags,
                                   ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  if (!keystroke) return kErrorBadArguments;
  if ((flags & 0xFF) && (flags & kFlagGamepad) == 0) return kErrorDeviceNotConnected;
  const uint32_t asked = *user_index_ptr;
  const bool any = (asked & 0xFF) == 0xFF || (flags & kFlagAnyUser);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    // netplay needs the keys read by the game's main thread too
    REXLOG_INFO("mk9: keystrokes first read on the {} thread (frame {}, user {:X})",
                mk9::frame::OnMainThread() ? "main" : "another", mk9::frame::Count(), asked);
  }
  std::lock_guard<std::mutex> lock(g_keys_lock);
  bool connected = false;
  for (uint32_t user = any ? 0 : asked; user < (any ? 4u : asked + 1); user++) {
    if (user >= 4) break;
    connected = connected || Current(user).connected;
    rex::input::X_INPUT_KEYSTROKE k{};
    if (NextKey(user, k)) {
      *keystroke = k;
      *user_index_ptr = user;
      return kErrorSuccess;
    }
  }
  return connected ? kErrorEmpty : kErrorDeviceNotConnected;
}

}  // namespace

REX_HOOK(__imp__XamInputGetState, Mk9XamInputGetState)
REX_HOOK(__imp__XamInputGetCapabilities, Mk9XamInputGetCapabilities)
REX_HOOK(__imp__XamInputGetCapabilitiesEx, Mk9XamInputGetCapabilitiesEx)
REX_HOOK(__imp__XamInputGetKeystrokeEx, Mk9XamInputGetKeystrokeEx)
