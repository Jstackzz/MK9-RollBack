// UE3's threads as rollback sees them, and engine switches.
//
// The rendering thread (RenderingThreadMain, sub_82740FE8) runs the commands the main thread
// queues in GRenderCommandBuffer, an FRingBuffer at 0x8341E4E0: +0 the data, +4 its end, +8 the
// write pointer (the main thread), +20 the read pointer (the rendering thread, which moves it on
// once a command ran). It lags up to a frame behind the main thread.
//
// MK's job manager: three worker threads (procedure sub_825617A8 -> sub_8255F188) take jobs the
// main thread queues during its tick (sub_827783B0 gets one, sub_82778508 runs it) and write the
// simulation's memory (animation, effects). Between jobs a worker marks itself idle in the
// manager (+56, a bit per worker) and waits 2 ms on its event (sub_82775F80 -> sub_8255D798), then
// looks again: even idle they write the manager's lock and bits. The rendering thread, its ring
// empty, sleeps (sub_8255D030) and looks again. Both are part of the simulation: their writes go
// back with a rollback, and while the frame's top keeps or puts back memory they are held in those
// waits (Hold).
//
// PhysX 2.8.4 simulates its scenes on threads of its own (NX_SF_SIMULATE_SEPARATE_THREAD; they start
// at sub_83103F38, which runs the thread object's vtable +4): UE3 starts a scene's step during the
// tick and fetches its results before the frame ends. They write the simulation's memory like the
// job workers and are simulation threads too. Between steps they wait in PhysX's Event::wait
// (sub_83101A58) or, the helpers that poll its task queue, go round their loops; every loop asks
// whether the thread should quit (sub_831040A8) between tasks. They are held in either place.
//
// A PhysX scene's step can run across a frame's top (UE3 fetches an async scene's results in a render
// command of the next frame): whether its events (start, done) are set is part of the state, and
// they are kept with each frame's top and put back with it. They are the events PhysX's
// Event::wait / Event::reset see (sub_83101A58 / sub_83101A50, the handle at +4).
//
// UE3's thread pool (GThreadPool, FQueuedThreadPoolWin: AddQueuedWork sub_82779818) runs work the
// simulation hands it (a PhysX scene's step queued from a render command, PhysX tasks from jobs) on
// threads of its own (FQueuedThreadWin::Run, sub_8255EF30): they are simulation threads too. An idle
// one waits on its DoWorkEvent (auto reset: a thread that took a signal is no longer idle), so the
// hold waits for every one of them in that wait with its event not set: then none has work queued
// and none can get any while the simulation is held.
//
// MK9_ONETHREAD=1: no rendering thread, as with the engine's own -ONETHREAD (FEngineLoop::PreInit
// checks it at 0x823660C4 and otherwise starts it with sub_82741218 once the hardware has more
// than one thread). Experimental: other threads then run render commands inline and the game
// crashes during boot.

#include "mk9/engine.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xevent.h>
#include <rex/system/xthread.h>

#include "mk9/game.h"
#include "mk9/net/rollback.h"
#include "mk9/simheap.h"

REX_EXTERN(__imp__sub_82741218);  // StartRenderingThread
REX_EXTERN(__imp__sub_82740FE8);  // RenderingThreadMain
REX_EXTERN(__imp__sub_82BF65B0);  // a D3D device call of every frame (r3: the device)
REX_EXTERN(__imp__sub_825617A8);  // a job worker thread's procedure
REX_EXTERN(__imp__sub_8255D798);  // a job worker's idle wait (2 ms on its event)
REX_EXTERN(__imp__sub_8255D030);  // appSleep(seconds)
REX_EXTERN(__imp__sub_8255EF30);  // FQueuedThreadWin::Run(thread)
REX_EXTERN(__imp__sub_8237C7B8);  // WaitForSingleObject(handle, timeout)
REX_EXTERN(__imp__sub_83103F38);  // a PhysX thread's entry: this->Run()
REX_EXTERN(__imp__sub_83101A58);  // PhysX's Event::wait(event, timeout)
REX_EXTERN(__imp__sub_83101A50);  // PhysX's Event::reset(event)
REX_EXTERN(__imp__sub_831040A8);  // a PhysX thread loop's "quit?" between tasks

namespace {
constexpr uint32_t kUseThreadedRendering = 0x8341C20C;  // set before the call
constexpr uint32_t kRenderRing = 0x8341E4E0;
// The Xbox 360 D3D device (the library is linked in around 0x82BF0000-0x82C10000): its ring
// buffer position and fences go with what the GPU already took, so a rollback never puts it
// back. Its fields seen so far reach past +21800.
constexpr uint32_t kD3DDeviceSize = 0x6000;
// Guest memory other threads keep writing that shares pages with the simulation's globals, left as
// it is by every restore (whatever rollback logs of those pages):
constexpr std::pair<uint32_t, uint32_t> kOtherThreadsGlobals[] = {
    // UE3's frame time on the GPU, which it measures in a callback on the GPU's interrupt thread
    // (sub_826B5750), on pages that hold the rendering thread's state too
    {0x833F3520, 1},  // "a GPU frame is being timed"
    {0x833DD564, 4},  // its cycles
    {0x833DD568, 8},  // its milliseconds (a double)
    // the engine heap's own state: GMalloc (the MemoryManager, built in place by sub_829AED30 up to
    // its pointer at 0x834243F0) and the lock and tree of its block registry (sub_82A4C4D0). While
    // rollback is armed only other threads allocate from the engine heap (mk9/gmalloc.cpp)
    {0x83423BA8, 0x848},
    {0x8342EA60, 0x40},
};
std::atomic<uint32_t> g_d3d_device{0};
std::atomic<uint32_t> g_render_tid{0};
std::atomic<bool> g_render_alive{false};
std::atomic<PPCContext*> g_render_ctx{nullptr};  // the rendering thread's registers (diagnostics)
// host threads of a kind; a thread that ended (PhysX's end in ExitThread, never returning to the
// hooks here) is dropped when one is looked for, and rollback forgets it (its id may come back)
class Threads {
 public:
  void Add() {
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0);
    std::lock_guard lock(lock_);
    PruneLocked();
    all_.push_back({GetCurrentThreadId(), h});
  }
  bool Has(uint32_t tid) {
    std::lock_guard lock(lock_);
    for (const auto& t : all_)
      if (t.tid == tid) return Alive(t);
    return false;
  }
  size_t Count() {
    std::lock_guard lock(lock_);
    PruneLocked();
    return all_.size();
  }
  template <class F>
  void ForEach(F&& fn) {
    std::lock_guard lock(lock_);
    PruneLocked();
    for (const auto& t : all_) fn(t.tid);
  }

 private:
  struct Entry {
    uint32_t tid;
    HANDLE handle;
  };
  static bool Alive(const Entry& t) { return !t.handle || WaitForSingleObject(t.handle, 0) == WAIT_TIMEOUT; }
  void PruneLocked() {
    std::erase_if(all_, [](const Entry& t) {
      if (Alive(t)) return false;
      mk9::rb::RemoveSimulationThread(t.tid);
      CloseHandle(t.handle);
      return true;
    });
  }
  std::mutex lock_;
  std::vector<Entry> all_;
};
// Every critical section in guest memory outside the simulation heap is volatile: a lock's state
// must never go back (at a frame's top the simulation's threads hold none, other threads may hold
// any). Those in the simulation heap go with their objects (mk9/gmalloc.cpp). Seen ones are cached.
void OnCriticalSection(uint32_t cs) {
  static std::atomic<uint32_t> seen[1 << 15];
  std::atomic<uint32_t>& slot = seen[(cs >> 2) & ((1 << 15) - 1)];
  if (slot.load(std::memory_order_relaxed) == cs) return;
  slot.store(cs, std::memory_order_relaxed);
  if (mk9::simheap::Sim().Owns(cs)) return;
  mk9::rb::AddVolatile(cs, 28);
}

// PhysX's events and how they stood at the top of each kept frame
std::mutex g_events_lock;
std::vector<uint32_t> g_events;
std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> g_event_states;  // frame -> (handle, set)

void NoteEvent(uint32_t event) {
  const uint32_t handle = mk9::game::ReadU32(event + 4);
  if (!handle) return;
  std::lock_guard lock(g_events_lock);
  if (std::find(g_events.begin(), g_events.end(), handle) == g_events.end()) g_events.push_back(handle);
}

rex::system::object_ref<rex::system::XEvent> EventOf(uint32_t handle) {
  return REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(handle);
}

void SaveEvents(uint32_t frame) {
  std::lock_guard lock(g_events_lock);
  auto& states = g_event_states[frame];
  states.clear();
  for (uint32_t h : g_events) {
    auto ev = EventOf(h);
    if (!ev) continue;
    uint32_t set = 0;
    ev->Query(nullptr, &set);
    states.emplace_back(h, set);
  }
  while (g_event_states.size() > size_t(mk9::rb::kKeepFrames) + 4) g_event_states.erase(g_event_states.begin());
}

void RestoreEvents(uint32_t frame) {
  std::lock_guard lock(g_events_lock);
  auto it = g_event_states.find(frame);
  if (it == g_event_states.end()) return;
  for (const auto& [h, set] : it->second) {
    auto ev = EventOf(h);
    if (!ev) continue;
    uint32_t now = 0;
    ev->Query(nullptr, &now);
    if (now == set) continue;
    if (set) ev->Set(0, false);
    else ev->Reset();
  }
  g_event_states.erase(std::next(it), g_event_states.end());
}

const bool g_events_hooked = [] {
  mk9::rb::OnOpen(&SaveEvents);
  mk9::rb::OnRestore(&RestoreEvents);
  return true;
}();

const bool g_cs_hooked = [] {
  rex::kernel::xboxkrnl::SetCriticalSectionHook(&OnCriticalSection);
  return true;
}();

Threads g_workers;  // the job manager's
Threads g_physics;  // PhysX's
Threads g_pool;     // the thread pool's
std::mutex g_pool_lock;
std::vector<uint32_t> g_pool_events;  // their DoWorkEvents' handles
std::atomic<int> g_pool_waiting{0};
thread_local bool t_pool = false;
constexpr uint32_t kPoolIdleWait = 0x8255EF68;  // return address of the DoWorkEvent wait in Run
// the hold: set by the main thread; the others say when they are in their waits (see Park)
std::atomic<bool> g_hold{false};
std::atomic<bool> g_render_waiting{false};
std::atomic<int> g_workers_waiting{0};
std::atomic<int> g_physics_waiting{0};
thread_local bool t_physics = false;
bool g_holding = false;
constexpr uint32_t kWorkerIdleWait = 0x82775FD4;   // return address of the idle wait in sub_82775F80
constexpr uint32_t kRenderIdleSleep = 0x8274117C;  // return address of the idle sleep in RenderingThreadMain

// leaving a wait: if the main thread holds, stay (counted as waiting) until it lets go. Waiting is
// said before the hold is looked at and the main thread sets the hold before it looks at who
// waits, so either it sees this thread leave or this thread sees the hold
template <class Leave, class Back>
void Park(Leave leave, Back back) {
  for (;;) {
    leave();
    if (!g_hold.load()) return;
    back();
    while (g_hold.load()) std::this_thread::yield();
  }
}


}  // namespace

namespace mk9::engine {

bool WaitRenderIdle(int timeout_ms) {
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (int spins = 0;; spins++) {
    if (game::ReadU32(kRenderRing + 20) == game::ReadU32(kRenderRing + 8)) return true;
    if (std::chrono::steady_clock::now() > until) return false;
    if (spins > 64) std::this_thread::yield();
  }
}

void ExcludeFromRollback() {
  rb::AddVolatile(kRenderRing, 0x40);
  const uint32_t data = game::ReadU32(kRenderRing), end = game::ReadU32(kRenderRing + 4);
  if (data && end > data && end - data < 0x4000000) rb::AddVolatile(data, end - data);
  const uint32_t device = g_d3d_device.load();
  if (device) rb::AddVolatile(device, kD3DDeviceSize);
  for (const auto& [va, n] : kOtherThreadsGlobals) rb::AddVolatile(va, n);
  // the rendering thread runs what the main thread queued: what it writes (the renderer's scene)
  // goes back with the game; so do the job workers' writes
  if (const uint32_t tid = g_render_tid.load()) rb::AddSimulationThread(tid);
  g_workers.ForEach([](uint32_t tid) { rb::AddSimulationThread(tid); });
  g_physics.ForEach([](uint32_t tid) { rb::AddSimulationThread(tid); });
  g_pool.ForEach([](uint32_t tid) { rb::AddSimulationThread(tid); });
  const size_t workers = g_workers.Count(), physics = g_physics.Count();
  REXLOG_INFO("mk9 engine: rollback leaves the rendering command ring ({:08X}-{:08X}) and the D3D device ({:08X}) "
              "alone; rendering thread {}, {} job workers, {} PhysX threads",
              data, end, device, g_render_tid.load(), workers, physics);
}

bool IsSimulationThread(uint32_t host_tid) {
  if (host_tid == g_render_tid.load(std::memory_order_relaxed)) return true;
  return g_workers.Has(host_tid) || g_physics.Has(host_tid) || g_pool.Has(host_tid);
}

// a pool thread's DoWorkEvent is set: it has work it has not taken yet
static bool PoolWorkPending() {
  std::lock_guard lock(g_pool_lock);
  for (uint32_t h : g_pool_events) {
    auto ev = EventOf(h);
    if (!ev) continue;
    uint32_t set = 0;
    ev->Query(nullptr, &set);
    if (set) return true;
  }
  return false;
}

bool Hold(int timeout_ms) {
  if (g_holding) return true;
  const auto t0 = std::chrono::steady_clock::now();
  // the ring drained first: once held, the rendering thread would run no more of it
  bool ok = WaitRenderIdle(timeout_ms);
  g_hold = true;
  g_holding = true;
  const size_t workers = g_workers.Count(), physics = g_physics.Count(), pool = g_pool.Count();
  const auto until = t0 + std::chrono::milliseconds(timeout_ms);
  for (int spins = 0;; spins++) {
    const bool render = !g_render_alive.load() || g_render_waiting.load();
    if (render && size_t(g_workers_waiting.load()) >= workers && size_t(g_physics_waiting.load()) >= physics &&
        size_t(g_pool_waiting.load()) >= pool && !PoolWorkPending())
      break;
    if (std::chrono::steady_clock::now() > until) {
      static int warned = 0;
      if (warned++ < 20) {
        REXLOG_WARN(
            "mk9 engine: hold timed out (rendering thread {}, {} of {} job workers, {} of {} PhysX threads and {} of {} "
            "pool threads waiting)",
            g_render_waiting.load() ? "waiting" : "busy", g_workers_waiting.load(), workers, g_physics_waiting.load(),
            physics, g_pool_waiting.load(), pool);
        if (PPCContext* rc = g_render_ctx.load(); rc && !g_render_waiting.load()) {
          uint32_t returns[16];
          const int n = game::StackReturns(*rc, returns, 16);
          std::string chain;
          for (int i = 0; i < n; i++) chain += fmt::format(" {:08X}", returns[i]);
          REXLOG_WARN("mk9 engine: the rendering thread is at lr {:08X} r3 {:08X}, stack{}", uint32_t(rc->lr), rc->r3.u32,
                      chain);
        }
      }
      ok = false;
      break;
    }
    if (spins > 64) std::this_thread::yield();
  }
  return ok;
}

void Release() {
  if (!g_holding) return;
  g_holding = false;
  g_hold = false;
}

}  // namespace mk9::engine

REX_HOOK_RAW(sub_825617A8) {
  g_workers.Add();
  REXLOG_INFO("mk9 engine: a job worker runs on host thread {}", GetCurrentThreadId());
  __imp__sub_825617A8(ctx, base);
}

REX_HOOK_RAW(sub_83103F38) {
  const uint32_t tid = GetCurrentThreadId();
  g_physics.Add();
  if (mk9::rb::Armed()) mk9::rb::AddSimulationThread(tid);
  t_physics = true;
  REXLOG_INFO("mk9 engine: a PhysX thread runs on host thread {} (object {:08X}, run {:08X})", tid, ctx.r3.u32,
              mk9::game::ReadU32(mk9::game::ReadU32(ctx.r3.u32) + 4));
  __imp__sub_83103F38(ctx, base);
}

REX_HOOK_RAW(sub_8255EF30) {
  const uint32_t tid = GetCurrentThreadId();
  g_pool.Add();
  if (mk9::rb::Armed()) mk9::rb::AddSimulationThread(tid);
  t_pool = true;
  const uint32_t event = mk9::game::ReadU32(mk9::game::ReadU32(ctx.r3.u32));
  {
    std::lock_guard lock(g_pool_lock);
    g_pool_events.push_back(event);
  }
  REXLOG_INFO("mk9 engine: a thread pool thread runs on host thread {} (its event {:08X})", tid, event);
  __imp__sub_8255EF30(ctx, base);
}

REX_HOOK_RAW(sub_8237C7B8) {
  if (!t_pool || uint32_t(ctx.lr) != kPoolIdleWait) return __imp__sub_8237C7B8(ctx, base);
  g_pool_waiting.fetch_add(1);
  __imp__sub_8237C7B8(ctx, base);
  g_pool_waiting.fetch_sub(1);
}

REX_HOOK_RAW(sub_83101A50) {
  NoteEvent(ctx.r3.u32);
  __imp__sub_83101A50(ctx, base);
}

REX_HOOK_RAW(sub_83101A58) {
  NoteEvent(ctx.r3.u32);
  if (!t_physics) return __imp__sub_83101A58(ctx, base);
  g_physics_waiting.fetch_add(1);
  __imp__sub_83101A58(ctx, base);
  Park([] { g_physics_waiting.fetch_sub(1); }, [] { g_physics_waiting.fetch_add(1); });
}

REX_HOOK_RAW(sub_831040A8) {
  // between tasks: stay while the main thread holds (counted as waiting once in, then the hold
  // looked at again, so the main thread either counts this thread or this thread sees the hold)
  if (t_physics) {
    while (g_hold.load()) {
      g_physics_waiting.fetch_add(1);
      while (g_hold.load()) std::this_thread::yield();
      g_physics_waiting.fetch_sub(1);
    }
  }
  __imp__sub_831040A8(ctx, base);
}

REX_HOOK_RAW(sub_8255D798) {
  if (uint32_t(ctx.lr) != kWorkerIdleWait) return __imp__sub_8255D798(ctx, base);
  g_workers_waiting.fetch_add(1);
  __imp__sub_8255D798(ctx, base);
  Park([] { g_workers_waiting.fetch_sub(1); }, [] { g_workers_waiting.fetch_add(1); });
}

REX_HOOK_RAW(sub_8255D030) {
  if (uint32_t(ctx.lr) != kRenderIdleSleep) return __imp__sub_8255D030(ctx, base);
  g_render_waiting = true;
  __imp__sub_8255D030(ctx, base);
  Park([] { g_render_waiting = false; }, [] { g_render_waiting = true; });
}

REX_HOOK_RAW(sub_82740FE8) {
  const uint32_t tid = GetCurrentThreadId();
  g_render_tid = tid;
  REXLOG_INFO("mk9 engine: the rendering thread runs on host thread {}", tid);
  // started again (the engine stops it around some loads): a simulation thread like the last one
  if (mk9::rb::Armed()) mk9::rb::AddSimulationThread(tid);
  g_render_alive = true;
  g_render_ctx = &ctx;
  __imp__sub_82740FE8(ctx, base);
  g_render_ctx = nullptr;
  g_render_alive = false;
  g_render_waiting = false;
}

REX_HOOK_RAW(sub_82BF65B0) {
  if (!g_d3d_device.load(std::memory_order_relaxed)) {
    g_d3d_device = ctx.r3.u32;
    REXLOG_INFO("mk9 engine: the D3D device is at {:08X}", ctx.r3.u32);
  }
  __imp__sub_82BF65B0(ctx, base);
}

REX_HOOK_RAW(sub_82741218) {
  static const bool one_thread = [] {
    const char* v = std::getenv("MK9_ONETHREAD");
    return v && std::strcmp(v, "1") == 0;
  }();
  if (one_thread) {
    mk9::game::WriteU32(kUseThreadedRendering, 0);
    REXLOG_INFO("mk9: rendering stays on the main thread (MK9_ONETHREAD)");
    return;
  }
  __imp__sub_82741218(ctx, base);
}
