// Sounds across rollbacks, see sound.h and docs/GAME.md, "Sounds".
//
// MK9 plays its sounds with FMOD's event system through UE3's audio device (UFmodAudioDevice, whose update runs in the
// tick on the main thread). A script or an actor takes a UFmodAudioComponent (C) from the device's pool, and the device's
// update starts it (InternalPlay, sub_82607A48): C takes an instance of its event (C+160, a UFmodEvent) from the event's
// list of instances (+200) or, for a complex event, asks FMOD for one of its pool (getEventByIndex), keeps the handle at
// C+264, sets the event callback (sub_82603360, with C as its user data) and calls FMOD::Event::start. FMOD tells the
// callback when the sound ends (C+88 |= 0x10000000), and the device's next update gives C and the instance back.
//
// Rollback puts back all of that but FMOD: C, its handle, the event's list. FMOD's memory is the audio heap's, which is
// never tracked, its threads mix on, and what a frame started goes on playing. So until 0.5.6 a frame run again started
// its sounds once more: an instance from the list is the one the first run took, and FMOD's start, which does nothing
// while its channels are active, started it over once it had ended; one from FMOD's pool played beside the first, which
// stayed taken for good. A frame taken back left its sounds playing even when the corrected frames never started them,
// the callback's user data a component that by then might be another sound's.
// And an end FMOD told of in a frame taken back was never told again: that component went on playing for the game, and
// the pool of 64 filled up.
//
// What is done here happens on FMOD's side and at FMOD's interface, and the simulation cannot see it:
//  - Every start while rollback is armed is noted: its frame, its event, how many of that event's starts came before it
//    in the frame, its handle. A restore moves the notes of the frames it took back aside (g_back), and clears those
//    sounds' callbacks: the components they tell of went back with the frames.
//  - A frame run again that asks for a noted start finds it still playing: the start does nothing and answers 0 as FMOD
//    did the first time, and when the game asks FMOD for an instance (getEventByIndex) it gets the one that plays. So the
//    handle the game keeps is the one the first run kept, and what the game then does with it (the callback, volume and
//    pitch, parameters, a stop) goes to the sound that plays.
//  - One whose sound has ended meanwhile starts for real, muted (EventI's setMute): the game's component then lives on
//    as it would, and FMOD tells of its end. An instance is unmuted when it next starts for real. So does one that holds
//    another instance than the one that plays (the event's list came out otherwise), and a stop of it stops the sound too.
//  - A start within kNearFrames of a noted one of its event counts as that one, on a frame's first run too (the note of the
//    instance the game starts first, then of the very frame, then the oldest): a corrected input moves a sound by a few
//    frames, and cut and started over, or started beside the first, it would be heard twice.
//  - A start nothing noted (the real inputs made it) just plays.
//  - What the corrected frames did not start again stops only when every frame up to kNearFrames past it has run and can no
//    longer change (net::Settled), so a sound a later rollback's frames would start is never cut first. It stops with its
//    event's fade out, and an instance of FMOD's pool goes back to it once silent.
//  - The ends FMOD told of in frames taken back are told again as those frames run again, if the component still holds
//    that handle.
// What the simulation could see of a start left out is the first run's: start's answer (0, as when it was noted), the
// handle (the same), the callback during start: type 9 (started), which it ignores, and 11 (a sound definition's
// selection), which reads C+164 and at most clamps it, a field only that callback reads. FMOD's own state is its own: the
// C runtime's rand() it draws from is FMOD's alone (game.h), and its code was seen to write no tracked memory on the
// simulation's threads but through that callback and its memory pool's lock in the executable's data (MK9_RB_TRACE_FMOD).
//
// MK9_RB_SOUND_AGAIN=1: frames run again start their sounds again, as until 0.5.6 (comparisons; the trace goes on).
// MK9_TRACE_SOUND=<file>: every event instance taken, started, stopped or released and the callback's calls, one line each:
//   <frame> <run again 0/1> <restores so far> <thread m/o> <what> ...
// tools/soundcount.py counts them.

#include "mk9/sound.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "mk9/frame.h"
#include "mk9/game.h"
#include "mk9/net/netplay.h"
#include "mk9/net/rollback.h"

REX_EXTERN(__imp__sub_82E46DE8);  // FMOD::Event::start(handle)
REX_EXTERN(__imp__sub_82E46E28);  // FMOD::Event::stop(handle, immediate)
REX_EXTERN(__imp__sub_82E46D90);  // FMOD::Event::release(handle, ...)
REX_EXTERN(__imp__sub_82E55D98);  // EventGroupI::getEventByIndex(group, index, mode, &handle)
REX_EXTERN(__imp__sub_82603360);  // the audio component's event callback (handle, type, param 1, param 2, component)
REX_EXTERN(sub_82603360);
REX_EXTERN(__imp__sub_82607A48);  // UFmodAudioComponent::InternalPlay(component)
REX_EXTERN(__imp__sub_82A30588);  // answers a script (native sub_824A5268): has the audio device loaded what it was loading?
REX_EXTERN(__imp__sub_82A30498);  // readies an instance of an event ahead, started and paused (r4: the event)
REX_EXTERN(sub_82E46F78);         // FMOD::Event::setCallback(handle, callback, user)
REX_EXTERN(sub_82E46B68);         // EventI::validate(handle, &event)
REX_EXTERN(sub_82E47298);         // FMOD::Event::getState(handle, &state)
REX_EXTERN(sub_82E46330);         // EventI::setMute(event, mute)
REX_EXTERN(sub_82602A98);         // the audio device

namespace mk9::sound {
namespace {

constexpr uint32_t kEventInstance = 172;  // EventI's: an instance's block, its parent first
constexpr uint32_t kEventFlags = 96;       // and its flags
constexpr uint32_t kEventRunning = 0x1000, kEventMuted = 0x4000;
constexpr uint32_t kEventFlags2 = 100;     // and in its upper half: 0x20 an instance of the project's pool (a complex event)
constexpr uint32_t kEventPooled = 0x200000;
constexpr uint32_t kComponentHandle = 264;  // UFmodAudioComponent: the FMOD::Event it holds
constexpr uint32_t kComponentEvent = 160;   // and its UFmodEvent
constexpr uint32_t kDeviceActive = 640;     // the audio device: the components in use (a UE3 array)
constexpr uint32_t kDeviceFree = 664;       // and those free
constexpr uint32_t kNames = 0x834161DC;     // UE3's table of names
constexpr uint32_t kCallback = 0x82603360;  // the audio component's event callback
constexpr uint32_t kEventFinished = 4;      // FMOD_EVENT_CALLBACKTYPE_EVENTFINISHED
// How far from a noted start of its event a start still counts as that sound. In lagged fights and a link test with rhythmic
// attacks a sound a rollback stopped came back 1 to 3 frames off far more often than a sound of its event follows any other;
// from 4 on no more often. 8 frames (133 ms) also takes as one what would be heard as one sound twice: any two of an event
// that close, one of them a wrong guess's
constexpr uint32_t kNearFrames = 8;

const bool g_on = [] {
  const char* v = std::getenv("MK9_RB_SOUND_AGAIN");
  return !(v && *v == '1');
}();

FILE* Trace() {
  static FILE* f = [] {
    const char* p = std::getenv("MK9_TRACE_SOUND");
    return p && *p ? std::fopen(p, "w") : nullptr;
  }();
  return f;
}

struct Start {
  uint32_t frame, event, nth, handle;
  uint32_t parent;  // what the instance is an instance of (FMOD's event, not the handle: the same value comes back when a
                    // pool's instance is used again, for any event of that pool)
};
struct Told {
  uint32_t frame, handle, type, component;
};

// A lock around the notes alone, never across a guest call: FMOD may hold locks of its own in another thread's call
// that lead to its callback, and so here
std::mutex g_lock;
std::vector<Start> g_starts;  // the starts of the frames rollback keeps
std::vector<Start> g_back;    // those of frames taken back, until the frames have run again
std::vector<Told> g_told;     // the ends told in the frames rollback keeps
std::vector<Told> g_tell;     // those of frames taken back, to be told again
std::vector<std::pair<uint32_t, uint32_t>> g_nth;  // this frame's run: per event the starts so far
std::vector<uint32_t> g_fading;  // stopped instances of FMOD's pool, to give back once they are silent
struct Stand {
  uint32_t held, playing;  // a muted instance the game holds, and the instance that plays its sound
};
std::vector<Stand> g_stands;
bool g_quiet_back = false;  // the callbacks of g_back to be cleared
std::atomic<uint32_t> g_restores{0};
std::atomic<bool> g_muted_any{false};
Stats g_stats;
thread_local uint32_t t_event = 0;  // the event being played (InternalPlay, sub_82A30498)

template <class... A>
void Line(const char* fmt, A... args) {
  FILE* f = Trace();
  if (!f) return;
  static std::mutex lock;
  std::lock_guard guard(lock);
  std::fprintf(f, "%u %d %u %c ", rb::CurrentFrame(), frame::Replaying() ? 1 : 0, g_restores.load(),
               frame::OnMainThread() ? 'm' : 'o');
  std::fprintf(f, fmt, args...);
  std::fputc('\n', f);
  std::fflush(f);  // the games are killed, not closed, when a test ends
}

// the name of a UE3 object (its FName at +40), for the trace
std::string NameOf(uint32_t object) {
  if (!object) return "-";
  const uint32_t table = game::ReadU32(kNames), index = game::ReadU32(object + 40);
  const uint32_t entry = table && index < 0x100000 ? game::ReadU32(table + index * 4) : 0;
  if (!entry) return "?";
  auto* mem = REX_KERNEL_STATE()->memory();
  const auto* s = mem->TranslateVirtual<const uint8_t*>(entry + 16);
  std::string out;
  if (s[0]) {
    for (int i = 0; i < 80 && s[i]; i++) out += char(s[i]);
  } else {  // wide, big-endian
    for (int i = 0; i < 80 && s[i * 2 + 1]; i++) out += char(s[i * 2 + 1]);
  }
  return out;
}

bool Active() { return g_on && rb::Armed(); }

// the event behind a handle (EventI), 0 when FMOD no longer knows it (each new use of an instance makes a new handle)
uint32_t EventOf(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  const uint32_t out = f.ctx.r1.u32 + 0x80;
  game::WriteU32(out, 0);
  f.ctx.r3.u64 = handle;
  f.ctx.r4.u64 = out;
  sub_82E46B68(f.ctx, base);
  return f.ctx.r3.u32 == 0 ? game::ReadU32(out) : 0;
}

// what an instance (EventI) is an instance of: its parent, in the block at +172
uint32_t ParentOf(uint32_t event) {
  const uint32_t instance = game::ReadU32(event + kEventInstance);
  return instance ? game::ReadU32(instance) : event;
}

// the instance a note's handle means now is still that sound's event's
bool StillIts(PPCContext& ctx, uint8_t* base, const Start& s) {
  if (!s.handle) return false;
  const uint32_t event = EventOf(ctx, base, s.handle);
  return event && ParentOf(event) == s.parent;
}

// a note's sound still plays: the instance has been started and has not ended, what FMOD's own start looks at
bool StillPlaying(PPCContext& ctx, uint8_t* base, const Start& s) {
  if (!s.handle) return false;
  const uint32_t event = EventOf(ctx, base, s.handle);
  return event && ParentOf(event) == s.parent && (game::ReadU32(event + kEventFlags) & kEventRunning) != 0;
}

// for the trace: an instance's state (FMOD_EVENT_STATE: 8 playing, 0x10 channels active). FMOD's start of an instance
// that plays with its channels active does nothing, one that has ended starts over
uint32_t StateOf(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  const uint32_t out = f.ctx.r1.u32 + 0x80;
  game::WriteU32(out, 0);
  f.ctx.r3.u64 = handle;
  f.ctx.r4.u64 = out;
  sub_82E47298(f.ctx, base);
  return f.ctx.r3.u32 == 0 ? game::ReadU32(out) : 0x80000000u;
}

void SetMute(PPCContext& ctx, uint8_t* base, uint32_t event, bool mute) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  f.ctx.r3.u64 = event;
  f.ctx.r4.u64 = mute ? 1 : 0;
  sub_82E46330(f.ctx, base);
}

void ClearCallback(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  f.ctx.r3.u64 = handle;
  f.ctx.r4.u64 = 0;
  f.ctx.r5.u64 = 0;
  sub_82E46F78(f.ctx, base);
}

// An instance of a complex event comes from a pool of FMOD's (a fighter's voices: two) and goes back with release, which
// the game calls when its component is done with it. One nothing holds any more stays taken for good: until 0.5.6 every
// duplicate of such an event left one behind, and FMOD then stole instances that were playing
void GiveBack(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  f.ctx.r3.u64 = handle;
  f.ctx.r4.u64 = 0;
  f.ctx.r5.u64 = 1;  // as the game releases them
  __imp__sub_82E46D90(f.ctx, base);
}

bool Pooled(uint32_t event) { return (game::ReadU32(event + kEventFlags2) & kEventPooled) != 0; }

// the instance that plays the sound of one the game holds muted, 0 when there is none (no more after this)
uint32_t TakeStand(uint32_t held) {
  std::lock_guard lock(g_lock);
  const auto it = std::find_if(g_stands.begin(), g_stands.end(), [&](const Stand& s) { return s.held == held; });
  if (it == g_stands.end()) return 0;
  const uint32_t playing = it->playing;
  g_stands.erase(it);
  return playing;
}

// a sound nothing of the simulation holds any more ends: no one is told, and its event's fade out plays (a stop's)
void Drop(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  if (const uint32_t playing = TakeStand(handle)) Drop(ctx, base, playing);
  const uint32_t event = EventOf(ctx, base, handle);
  if (!event) return;
  ClearCallback(ctx, base, handle);
  if (game::ReadU32(event + kEventFlags) & kEventRunning) {
    rex::CallFrame f(ctx);
    f.ctx.r1.u32 -= 0x200;
    f.ctx.r3.u64 = handle;
    f.ctx.r4.u64 = 0;
    __imp__sub_82E46E28(f.ctx, base);
  }
  if (Pooled(event)) {
    std::lock_guard lock(g_lock);
    g_fading.push_back(handle);
  }
}

// g_lock held: the starts of this event so far in this run of the frame
uint32_t& NthOf(uint32_t event) {
  for (auto& n : g_nth)
    if (n.first == event) return n.second;
  g_nth.emplace_back(event, 0);
  return g_nth.back().second;
}

// what g_back holds of a start in this frame on `handle` (0: not known yet), of its event within kNearFrames: the note of that
// instance (an instance plays one sound: this start is that sound); else the very frame and place among the event's starts;
// else the oldest, the first that would stop if nothing took it
bool TakenBack(uint32_t frame, uint32_t event, uint32_t nth, uint32_t handle, Start* out) {
  const Start* same = nullptr;
  const Start* exact = nullptr;
  const Start* oldest = nullptr;
  for (const Start& s : g_back) {
    if (s.event != event) continue;
    const uint32_t off = s.frame > frame ? s.frame - frame : frame - s.frame;
    if (off > kNearFrames) continue;
    if (handle && s.handle == handle && (!same || s.frame < same->frame)) same = &s;
    if (s.frame == frame && s.nth == nth) exact = &s;
    if (!oldest || s.frame < oldest->frame) oldest = &s;
  }
  const Start* best = same ? same : exact ? exact : oldest;
  if (best) *out = *best;
  return best != nullptr;
}

void Forget(const Start& was) {
  std::erase_if(g_back, [&](const Start& s) { return s.frame == was.frame && s.event == was.event && s.nth == was.nth; });
}

void FrameOpen(uint32_t) {
  std::lock_guard lock(g_lock);
  g_nth.clear();
}

// a rollback went back to the top of `frame`: what the frames since started waits to see if they start it again
void RolledBack(uint32_t frame) {
  g_restores++;
  Line("restore %u", frame);
  std::lock_guard lock(g_lock);
  g_nth.clear();
  const auto taken = [&](const auto& e) { return e.frame >= frame; };
  for (const Start& s : g_starts)
    if (taken(s)) g_back.push_back(s);
  std::erase_if(g_starts, taken);
  for (const Told& t : g_told)
    if (taken(t)) g_tell.push_back(t);
  std::erase_if(g_told, taken);
  g_quiet_back = !g_back.empty();
}

const bool g_listening = [] {
  rb::OnRestore(&RolledBack);
  rb::OnOpen(&FrameOpen);
  return true;
}();

// the audio component in use that holds a handle, 0 when none does
uint32_t HolderOf(PPCContext& ctx, uint8_t* base, uint32_t handle) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  sub_82602A98(f.ctx, base);
  const uint32_t device = f.ctx.r3.u32;
  if (!device) return 0;
  const uint32_t data = game::ReadU32(device + kDeviceActive), count = game::ReadU32(device + kDeviceActive + 4);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t c = game::ReadU32(data + i * 4);
    if (c && game::ReadU32(c + kComponentHandle) == handle) return c;
  }
  return 0;
}

void Callback(PPCContext& ctx, uint8_t* base, uint32_t handle, uint32_t type, uint32_t component) {
  rex::CallFrame f(ctx);
  f.ctx.r1.u32 -= 0x200;
  f.ctx.r3.u64 = handle;
  f.ctx.r4.u64 = type;
  f.ctx.r5.u64 = 0;
  f.ctx.r6.u64 = 0;
  f.ctx.r7.u64 = component;
  sub_82603360(f.ctx, base);  // its hook notes it for the frame running now
}

// tell the audio component of an end again: the callback as FMOD called it, if the component is in use and still holds
// the handle (a rollback left it one that ended; a component given to another sound since is that sound's)
void TellAgain(PPCContext& ctx, uint8_t* base, const Told& t) {
  if (HolderOf(ctx, base, t.handle) != t.component) {
    Line("told-not-again %08X type %u component %08X", t.handle, t.type, t.component);
    return;
  }
  Line("told-again %08X type %u component %08X", t.handle, t.type, t.component);
  {
    std::lock_guard lock(g_lock);
    g_stats.told++;
  }
  Callback(ctx, base, t.handle, t.type, t.component);
}

// a sound of a frame taken back that the corrected frame did not start again
void TakeBack(PPCContext& ctx, uint8_t* base, const Start& s) {
  {
    // a later frame's sound has the instance by now, which that frame will find or take back itself
    std::lock_guard lock(g_lock);
    if (std::any_of(g_back.begin(), g_back.end(), [&](const Start& b) { return b.handle == s.handle && b.frame > s.frame; }))
      return;
  }
  if (!StillIts(ctx, base, s)) return;  // it plays another event's sound by now, which has a note of its own
  // the game holds the instance from before that frame, which started it over (a component played again): it goes on as
  // the game's sound, told of its end as the game expects
  if (const uint32_t holder = HolderOf(ctx, base, s.handle)) {
    const bool playing = StillPlaying(ctx, base, s);
    Line("left %08X event %08X of frame %u with component %08X playing %d", s.handle, s.event, s.frame, holder, playing ? 1 : 0);
    if (playing) {
      rex::CallFrame f(ctx);
      f.ctx.r1.u32 -= 0x200;
      f.ctx.r3.u64 = s.handle;
      f.ctx.r4.u64 = kCallback;
      f.ctx.r5.u64 = holder;
      sub_82E46F78(f.ctx, base);
    } else {
      Callback(ctx, base, s.handle, kEventFinished, holder);  // it ended while no one was to be told
    }
    return;
  }
  Drop(ctx, base, s.handle);
  Line("stopped %08X event %08X of frame %u", s.handle, s.event, s.frame);
  std::lock_guard lock(g_lock);
  g_stats.stopped++;
}

}  // namespace

Stats GetStats() {
  std::lock_guard lock(g_lock);
  return g_stats;
}

void OnRunAgain(PPCContext& ctx, uint8_t* base) {
  if (!g_on) return;
  std::vector<Start> back;
  {
    std::lock_guard lock(g_lock);
    if (!g_quiet_back) return;
    g_quiet_back = false;
    back = g_back;
  }
  // nothing the simulation holds answers for these sounds now: the components they tell of went back with the frames
  for (const Start& s : back)
    if (s.handle) ClearCallback(ctx, base, s.handle);
}

void OnFrameEnd(PPCContext& ctx, uint8_t* base) {
  if (!g_on || !rb::Armed()) return;
  const uint32_t now = rb::CurrentFrame(), settled = net::Settled();
  std::vector<Start> gone;
  std::vector<Told> tell;
  {
    std::lock_guard lock(g_lock);
    if (g_back.empty() && g_tell.empty()) return;
    // the frames that could start it again have run and no later rollback will run them otherwise
    const auto past = [&](const Start& s) { return s.frame + kNearFrames <= now && s.frame + kNearFrames < settled; };
    for (const Start& s : g_back)
      if (past(s)) gone.push_back(s);
    std::erase_if(g_back, past);
    for (const Told& t : g_tell)
      if (t.frame <= now) tell.push_back(t);
    std::erase_if(g_tell, [&](const Told& t) { return t.frame <= now; });
  }
  for (const Start& s : gone) TakeBack(ctx, base, s);
  for (const Told& t : tell) TellAgain(ctx, base, t);
}

void OnFrameTop(PPCContext& ctx, uint8_t* base) {
  if (Trace() && rb::Armed() && rb::CurrentFrame() % 300 == 0) {
    rex::CallFrame f(ctx);
    f.ctx.r1.u32 -= 0x200;
    sub_82602A98(f.ctx, base);
    if (const uint32_t device = f.ctx.r3.u32)
      Line("components in-use %u free %u", game::ReadU32(device + kDeviceActive + 4), game::ReadU32(device + kDeviceFree + 4));
  }
  std::vector<uint32_t> fading;
  {
    std::lock_guard lock(g_lock);
    fading.swap(g_fading);
  }
  for (const uint32_t handle : fading) {
    const uint32_t event = EventOf(ctx, base, handle);
    if (!event) continue;
    if (game::ReadU32(event + kEventFlags) & kEventRunning) {
      std::lock_guard lock(g_lock);
      g_fading.push_back(handle);
    } else {
      GiveBack(ctx, base, handle);
    }
  }
  if (!rb::Armed()) {
    // a match's frames start from 0 again; a sound still waiting to see if a frame starts it ends
    std::vector<Start> left;
    {
      std::lock_guard lock(g_lock);
      left.swap(g_back);
      g_starts.clear();
      g_told.clear();
      g_tell.clear();
    }
    for (const Start& s : left) TakeBack(ctx, base, s);
    return;
  }
  std::lock_guard lock(g_lock);
  // rollback goes back no further than this
  const uint32_t now = rb::CurrentFrame();
  const uint32_t oldest = now > uint32_t(rb::kKeepFrames) + 1 ? now - uint32_t(rb::kKeepFrames) - 1 : 0;
  const auto old = [&](const auto& e) { return e.frame < oldest; };
  std::erase_if(g_starts, old);
  std::erase_if(g_told, old);
  static uint32_t last_logged = 0;
  if (g_on && now - last_logged >= 600 && g_stats.again != 0) {
    last_logged = now;
    REXLOG_INFO("mk9 sound: {} starts on frames' first runs, {} asked for by frames run again: {} still playing, {} on another "
                "instance, {} ended already (muted), {} new; {} of those off their frame; {} sounds of frames taken back stopped, {} "
                "ends told again",
                g_stats.first, g_stats.again, g_stats.kept, g_stats.moved, g_stats.silent, g_stats.fresh, g_stats.near,
                g_stats.stopped, g_stats.told);
  }
}

}  // namespace mk9::sound

using namespace mk9;
using namespace mk9::sound;

// the event being played for what is called inside
struct PlayingEvent {
  explicit PlayingEvent(uint32_t event) { t_event = event; }
  ~PlayingEvent() { t_event = 0; }
};

REX_HOOK_RAW(sub_82607A48) {
  const uint32_t event = game::ReadU32(ctx.r3.u32 + kComponentEvent);
  PlayingEvent playing(event);
  __imp__sub_82607A48(ctx, base);
}

REX_HOOK_RAW(sub_82A30498) {
  PlayingEvent playing(ctx.r4.u32);
  __imp__sub_82A30498(ctx, base);
}

// Asked for an instance: when a frame taken back started this event (this frame, or up to kNearFrames off), the one that still
// plays. It never gets one nobody asked for in place of it: the instance would play beside it, or steal it
REX_HOOK_RAW(sub_82E55D98) {
  const uint32_t group = ctx.r3.u32, index = ctx.r4.u32, mode = ctx.r5.u32, out = ctx.r6.u32, lr = uint32_t(ctx.lr);
  if (Active() && t_event && out) {
    Start was{};
    bool found;
    {
      std::lock_guard lock(g_lock);
      found = TakenBack(rb::CurrentFrame(), t_event, NthOf(t_event), 0, &was);
    }
    if (found && StillPlaying(ctx, base, was)) {
      game::WriteU32(out, was.handle);
      Line("get group %08X index %u mode %X lr %08X = kept %08X", group, index, mode, lr, was.handle);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82E55D98(ctx, base);
  Line("get group %08X index %u mode %X lr %08X = %X %08X", group, index, mode, lr, ctx.r3.u32,
       out ? game::ReadU32(out) : 0);
}

REX_HOOK_RAW(sub_82E46DE8) {
  const uint32_t handle = ctx.r3.u32, lr = uint32_t(ctx.lr), event = t_event;
  const bool active = Active(), again = frame::Replaying();
  const uint32_t now = rb::CurrentFrame();
  const char* how = again ? "fresh" : "first";
  bool muted = false;
  uint32_t stand_for = 0;
  if (active) {
    Start was{};
    uint32_t nth = 0;
    bool found = false;
    {
      std::lock_guard lock(g_lock);
      nth = NthOf(event);
      found = TakenBack(now, event, nth, handle, &was);
    }
    char of[32] = "";
    if (found && was.frame != now) std::snprintf(of, sizeof(of), " of %u", was.frame);
    if (found) {
      const bool playing = StillPlaying(ctx, base, was);
      if (playing && was.handle == handle) {
        // the same instance plays this very start: nothing to do, and what FMOD answered the first time
        {
          std::lock_guard lock(g_lock);
          Forget(was);
          g_starts.push_back({now, event, nth, handle, was.parent});
          NthOf(event)++;
          (again ? g_stats.again : g_stats.first)++;
          g_stats.kept++;
          if (was.frame != now) g_stats.near++;
        }
        if (Trace()) Line("start %08X event %08X %s lr %08X = 0 kept%s", handle, event, NameOf(event).c_str(), lr, of);
        ctx.r3.u64 = 0;
        return;
      }
      if (playing) {
        // another instance than the one that plays (the event's list of instances came out otherwise): the sound goes on
        // where it is, this one starts muted for the game to hold, and a stop of it stops the sound too
        how = "moved";
      } else {
        // it ended already: this start lives on for the game (FMOD tells of its end), and is not heard
        if (was.handle != handle) TakeBack(ctx, base, was);
        how = "silent";
      }
      muted = true;
      stand_for = playing ? was.handle : 0;
    }
    const uint32_t before = Trace() ? StateOf(ctx, base, handle) : 0;
    const uint32_t instance = EventOf(ctx, base, handle);
    // an instance that plays already goes on as it is: FMOD's start does nothing to it, and muted it would cut that sound
    const bool running = instance && (game::ReadU32(instance + kEventFlags) & kEventRunning) != 0;
    const bool mute = muted && !running;
    if (mute) g_muted_any = true;
    if (instance && !running && (mute || (game::ReadU32(instance + kEventFlags) & kEventMuted))) SetMute(ctx, base, instance, mute);
    __imp__sub_82E46DE8(ctx, base);
    const uint32_t result = ctx.r3.u32;
    if (result == 0) {
      if (mute && instance) SetMute(ctx, base, instance, true);  // its channel group is there now
      const uint32_t parent = instance ? ParentOf(instance) : 0;
      std::lock_guard lock(g_lock);
      if (found) Forget(was);
      // A start of this instance ends what it played before: no later frame's stop for that, no end to tell of it. It takes
      // over a sound that plays; a note of a sound that is over stays without its instance, for a start of its event in the
      // frames to come not to play that again
      if (running)
        std::erase_if(g_back, [&](const Start& s) { return s.handle == handle; });
      else
        for (Start& s : g_back)
          if (s.handle == handle) s.handle = 0;
      std::erase_if(g_tell, [&](const Told& t) { return t.handle == handle; });
      std::erase_if(g_stands, [&](const Stand& s) { return s.held == handle; });
      if (stand_for) g_stands.push_back({handle, stand_for});
      g_starts.push_back({now, event, nth, handle, parent});
      NthOf(event)++;
      (again ? g_stats.again : g_stats.first)++;
      if (stand_for) g_stats.moved++;
      else if (muted) g_stats.silent++;
      else if (again) g_stats.fresh++;
      if (found && was.frame != now) g_stats.near++;
    }
    if (Trace()) {
      Line("start %08X event %08X %s lr %08X = %X %s state %X%s", handle, event, NameOf(event).c_str(), lr, result, how, before, of);
      ctx.r3.u64 = result;
    }
    return;
  }
  const uint32_t before = Trace() ? StateOf(ctx, base, handle) : 0;
  // not armed (a match's end): an instance muted during the fight is heard again
  if (g_muted_any.load(std::memory_order_relaxed)) {
    const uint32_t instance = EventOf(ctx, base, handle);
    if (instance && (game::ReadU32(instance + kEventFlags) & kEventMuted)) SetMute(ctx, base, instance, false);
  }
  __imp__sub_82E46DE8(ctx, base);
  if (Trace()) {
    const uint32_t result = ctx.r3.u32;
    Line("start %08X event %08X %s lr %08X = %X %s state %X", handle, event, NameOf(event).c_str(), lr, result, how, before);
    ctx.r3.u64 = result;
  }
}

// A frame stops or releases what its component held then. When a sound of a frame taken back has that instance by now and
// waits to see if a frame starts it (a handle's value comes back with the instance, and an instance of an event's list goes
// on to the next sound), that would cut the sound, which the corrected frames may yet come to. No component that started
// it in those frames holds it: that start would have found the note
bool WaitingSoundHas(uint32_t handle) {
  if (!Active() || !handle) return false;
  std::lock_guard lock(g_lock);
  return std::any_of(g_back.begin(), g_back.end(), [&](const Start& s) { return s.handle == handle; });
}

REX_HOOK_RAW(sub_82E46E28) {
  const uint32_t handle = ctx.r3.u32, immediate = ctx.r4.u32, lr = uint32_t(ctx.lr);
  if (WaitingSoundHas(handle)) {
    Line("stop %08X now %u lr %08X left to a sound that waits", handle, immediate, lr);
    ctx.r3.u64 = 0;
    return;
  }
  uint32_t up[6] = {};
  if (Trace()) game::StackReturns(ctx, up, 6);
  const uint32_t playing = TakeStand(handle);  // first: the stop tells of the end, below
  __imp__sub_82E46E28(ctx, base);
  const uint32_t result = ctx.r3.u32;
  Line("stop %08X now %u lr %08X = %X up %08X %08X %08X %08X %08X %08X", handle, immediate, lr, result, up[0], up[1], up[2],
       up[3], up[4], up[5]);
  if (playing) Drop(ctx, base, playing);
  ctx.r3.u64 = result;
}

REX_HOOK_RAW(sub_82E46D90) {
  const uint32_t handle = ctx.r3.u32, lr = uint32_t(ctx.lr);
  if (WaitingSoundHas(handle)) {
    Line("release %08X lr %08X left to a sound that waits", handle, lr);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82E46D90(ctx, base);
  Line("release %08X lr %08X = %X", handle, lr, ctx.r3.u32);
}

// FMOD telling the audio component of an event. Ends (and stolen instances, and sync points) are noted: a rollback takes
// back what they set, and FMOD does not tell twice
REX_HOOK_RAW(sub_82603360) {
  const uint32_t handle = ctx.r3.u32, type = ctx.r4.u32, user = ctx.r7.u32;
  __imp__sub_82603360(ctx, base);
  const bool end = type == 0 || type == 3 || type == 4;
  if (end || type == 9 || type == 11) Line("callback %08X type %u user %08X", handle, type, user);
  if (type == 3 || type == 4) TakeStand(handle);
  if (end && Active()) {
    std::lock_guard lock(g_lock);
    g_told.push_back({rb::CurrentFrame(), handle, type, user});
  }
}

REX_HOOK_RAW(sub_82A30588) {
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82A30588(ctx, base);
  Line("loaded-check boot-frame %u phase %d lr %08X = %u", unsigned(frame::Count()), game::Phase(), lr, ctx.r3.u32);
}
