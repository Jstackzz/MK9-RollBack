// The game's controllers, see input.h. Replaces the SDK's XamInputGetState and
// XamInputGetCapabilities (strong definitions here win over the runtime DLL's for every call
// the recompiled code makes).

#include "mk9/input.h"

#include <array>
#include <atomic>
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

namespace mk9::input {
namespace {

constexpr uint32_t kFlagGamepad = 0x01;
constexpr uint32_t kFlagAnyUser = 1u << 30;
constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
constexpr uint32_t kErrorBadArguments = 0xA0;

std::atomic<Source> g_source{nullptr};
std::array<std::atomic<uint64_t>, 4> g_polls{};

// the packet number XInput reports changes whenever the state does
std::mutex g_packet_lock;
std::array<Pad, 4> g_last{};
std::array<uint32_t, 4> g_packet{};

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

uint64_t Polls(uint32_t user) { return user < 4 ? g_polls[user].load() : 0; }

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
  if (!pad.connected) return kErrorDeviceNotConnected;
  if (input_state) {
    uint32_t packet;
    {
      std::lock_guard<std::mutex> lock(g_packet_lock);
      if (!Same(pad, g_last[user])) {
        g_last[user] = pad;
        g_packet[user]++;
      }
      packet = g_packet[user];
    }
    input_state->packet_number = pad.packet ? pad.packet : packet;
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

}  // namespace

REX_HOOK(__imp__XamInputGetState, Mk9XamInputGetState)
REX_HOOK(__imp__XamInputGetCapabilities, Mk9XamInputGetCapabilities)
REX_HOOK(__imp__XamInputGetCapabilitiesEx, Mk9XamInputGetCapabilitiesEx)
