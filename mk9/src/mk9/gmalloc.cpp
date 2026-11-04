// GMalloc routing: the simulation's allocations go to the simulation heap, FMOD's to the audio heap
// (simheap.h).
//
// UE3's GMalloc is MK9's MemoryManager (the object 0x83423BA8, vtable 0x82150C90); every engine
// allocation goes through three of its methods:
//   +8  sub_829AEB30  Malloc(this, size, alignment)
//   +12 sub_829AEB38  Realloc(this, block, size, alignment)
//   +16 sub_829AEB40  Free(this, block)
// From boot on:
//   - FMOD's allocations (made from its code, on any thread) come from the audio heap, which rollback
//     never tracks: FMOD's threads mix on through rollbacks. So does what UE3's file callbacks for
//     FMOD allocate (open sub_82602B30, read sub_82602D00, seek sub_82602D88: the file reader FMOD's
//     stream thread reads sound banks through)
//   - the main thread's, the rendering thread's and the job workers' other allocations come from
//     the simulation heap
//   - everything else (loaders, the thread pool's own threads, ...) stays in the engine's heap
// Frees and reallocations go to the heap the block came from. While rollback is armed no thread
// changes the engine heap's bookkeeping: it keeps its free lists in its free blocks, on pages it
// shares with the simulation's own engine blocks (made before the simulation heap was there), and a
// restore of such a page, logged for the simulation's write, put back whatever other threads had
// freed or taken there since (2026-11-04: the async loader's thread, copying a request's file name,
// read a free list's link that was text, `..\A`, and crashed in the engine's Malloc, in every ghost
// sync test run at the same moment of a fight). So while armed: other threads allocate from the audio heap;
// an engine block anyone frees is freed once rollback is disarmed (the simulation's not if a rollback
// took back the frame that freed it: the frame then runs again and frees it again, or not); an engine
// block anyone reallocates moves to the simulation heap (the simulation's threads) or the audio heap
// (the others), its free waiting likewise. The simulation heap is the simulation's own: a block of it
// another thread frees is freed by the main thread at the next frame's top, one another thread
// reallocates moves to the engine's heap (the audio heap while armed). A block of it another thread
// writes (UE3's streaming file reader FMOD's stream thread fills, a counter the loader counts down) is
// that thread's from then on: rollback leaves it as it is (volatile) until it is freed. A block the
// other threads took from the audio heap while armed is freed at once by them, and by the simulation's
// threads as an engine block is (the restored simulation may still hold it).
// MK9_SIMHEAP=0 turns the routing off.

#include "mk9/gmalloc.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "mk9/engine.h"
#include "mk9/game.h"
#include "mk9/net/rollback.h"
#include "mk9/simheap.h"

REX_EXTERN(__imp__sub_829AEB30);  // GMalloc->Malloc
REX_EXTERN(__imp__sub_829AEB38);  // GMalloc->Realloc
REX_EXTERN(__imp__sub_829AEB40);  // GMalloc->Free
REX_EXTERN(__imp__sub_82602B30);  // FMOD's file open callback (UE3's)
REX_EXTERN(__imp__sub_82602D00);  // its read callback
REX_EXTERN(__imp__sub_82602D88);  // its seek callback
REX_EXTERN(__imp__sub_82741730);  // ~FPendingCleanupObjects: FinishCleanup (vtable +4) on each object

namespace mk9::gmalloc {
namespace {

constexpr uint32_t kGMalloc = 0x83423BA8;
// FMOD's code (its threads start at 0x82DEB0B8; its functions seen from 0x82DD2090 to 0x82E72CE0; the HUD's UI library
// from 0x82E81248 on)
constexpr uint32_t kFmodLo = 0x82DC0000, kFmodHi = 0x82E80000;

const bool g_on = [] {
  const char* v = std::getenv("MK9_SIMHEAP");
  return !(v && *v == '0');
}();

struct EngineFree {
  uint32_t frame, block;
};

std::mutex g_lock;
std::vector<EngineFree> g_engine_frees;  // engine blocks (and others' audio blocks) simulation threads freed while armed
std::vector<uint32_t> g_other_frees;     // engine blocks other threads freed while armed
std::vector<uint32_t> g_sim_frees;       // simulation blocks other threads freed
std::unordered_set<uint32_t> g_others_audio;  // audio heap blocks other threads took while armed
std::atomic<uint64_t> g_routed{0}, g_audio{0}, g_fallback{0}, g_engine_reallocs{0};
std::atomic<bool> g_audio_untracked{false};
thread_local int t_audio_file = 0;  // in one of UE3's file callbacks for FMOD
std::unordered_map<uint32_t, uint32_t> g_volatile_blocks;  // payload -> size; g_lock

simheap::Heap& Sim() { return simheap::Sim(); }
simheap::Heap& Audio() { return simheap::Audio(); }

bool SimThread() {
  thread_local int cached = -1;
  if (cached >= 0) return cached != 0;
  bool sim = engine::IsSimulationThread(GetCurrentThreadId());
  if (!sim) {
    auto* t = rex::system::XThread::GetCurrentThread();
    sim = t && t->main_thread();
  }
  // the rendering thread and the workers say so first thing; anything else that is not yet one
  // asks again next time
  if (sim) cached = 1;
  return sim;
}

// FMOD's code allocating: its allocator calls GMalloc (from 0x82DCFE24), or through one wrapper. Not deeper: the code
// right above FMOD's (from about 0x82E80000) is the UI library the HUD is drawn with, and until 2026-11-04 any of 16
// frames in FMOD's range counted, so the HUD's allocations made through that library (its allocator 0x82EF4384) came
// from the audio heap, which rollback never tracks: the HUD's geometry records (one at 0x540471E0 in every report of a
// rendering command crash there, the first in 0.4.0) stayed as they were through every rollback while the HUD's state
// and the entries they point at went back. UE3's file callbacks for FMOD, which go deep into UE3's file code, are
// told apart by their own hooks (t_audio_file)
bool FromFmod(const PPCContext& ctx) {
  auto in = [](uint32_t a) { return a >= kFmodLo && a < kFmodHi; };
  if (in(uint32_t(ctx.lr))) return true;
  uint32_t up = 0;
  return game::StackReturns(ctx, &up, 1) == 1 && in(up);
}

// MK9_GMALLOC_TRACE=1 (diagnostics): the simulation heap's last frees, and for a block freed twice
// where it was freed before (frame, thread, guest return addresses)
const bool g_trace = [] {
  const char* v = std::getenv("MK9_GMALLOC_TRACE");
  return v && *v == '1';
}();
struct FreeRecord {
  uint32_t block, frame, tid, restores;
  uint32_t returns[12];
};
std::mutex g_trace_lock;
std::vector<FreeRecord> g_trace_ring(1 << 16);
size_t g_trace_at = 0;
std::atomic<uint32_t> g_restores{0};

// the last free of a block in the trace ring
std::string LastFree(uint32_t block) {
  std::lock_guard lock(g_trace_lock);
  for (size_t i = 1; i < g_trace_ring.size(); i++) {
    const FreeRecord& r = g_trace_ring[(g_trace_at - i) & (g_trace_ring.size() - 1)];
    if (r.block != block) continue;
    std::string s = fmt::format("freed in frame {} by thread {} after {} restores, from", r.frame, r.tid, r.restores);
    for (uint32_t a : r.returns) s += fmt::format(" {:08X}", a);
    return s;
  }
  return "no free of it traced";
}

std::string Returns(const uint32_t* r, int n) {
  std::string s;
  for (int i = 0; i < n; i++) s += fmt::format(" {:08X}", r[i]);
  return s;
}

void SimFree(const PPCContext& ctx, uint32_t block) {
  if (!g_trace) {
    Sim().Free(block);
    return;
  }
  FreeRecord rec{block, rb::CurrentFrame(), GetCurrentThreadId(), g_restores.load(), {}};
  rec.returns[0] = uint32_t(ctx.lr);
  game::StackReturns(ctx, rec.returns + 1, 11);
  const bool ok = Sim().Free(block);
  std::lock_guard lock(g_trace_lock);
  if (!ok) {
    static int told = 0;
    if (told++ < 12) {
      std::string before = "never";
      for (size_t i = 1; i < g_trace_ring.size(); i++) {
        const FreeRecord& r = g_trace_ring[(g_trace_at - i) & (g_trace_ring.size() - 1)];
        if (r.block != block) continue;
        before = fmt::format("frame {} thread {} after {} restores, from{}", r.frame, r.tid, r.restores,
                             Returns(r.returns, 12));
        break;
      }
      REXLOG_ERROR("mk9 gmalloc: {:08X} freed again in frame {} by thread {} after {} restores, from{}; before: {}",
                   block, rec.frame, rec.tid, rec.restores, Returns(rec.returns, 12), before);
    }
    return;
  }
  g_trace_ring[g_trace_at++ & (g_trace_ring.size() - 1)] = rec;
}

bool AudioReady() {
  if (!Audio().Ready()) return false;
  if (!g_audio_untracked.exchange(true)) rb::Untrack(Audio().RegionBase(), Audio().RegionSize());
  return true;
}

bool SimReady() {
  static std::atomic<bool> told{false};
  if (!Sim().Ready()) return false;
  if (!told.exchange(true)) rb::SetSimulationRegion(Sim().RegionBase(), Sim().RegionSize());
  return true;
}

// the heap a new block of this call comes from: 0 the engine's. *others: it is another thread's block taken from the
// audio heap while armed (not FMOD's)
simheap::Heap* Route(const PPCContext& ctx, bool* others = nullptr) {
  if (!g_on) return nullptr;
  if (t_audio_file || FromFmod(ctx)) return AudioReady() ? &Audio() : nullptr;
  if (SimThread()) return SimReady() ? &Sim() : nullptr;
  // another thread while armed: the engine heap's bookkeeping stays as it is (see the top)
  if (!rb::Armed() || !AudioReady()) return nullptr;
  if (others) *others = true;
  return &Audio();
}

void NoteOthers(uint32_t block) {
  std::lock_guard lock(g_lock);
  g_others_audio.insert(block);
}

// guest bytes from va to the end of the committed run of pages it is in (0: not committed). The run's size counts from
// the start of va's page
uint32_t CommittedFrom(uint32_t va) {
  auto* heap = REX_KERNEL_STATE()->memory()->LookupHeap(va);
  rex::memory::HeapAllocationInfo info{};
  if (!heap || !heap->QueryRegionInfo(va, &info) || !(info.state & 1)) return 0;
  const uint32_t page = va & ~(heap->page_size() - 1);
  return uint32_t(uint64_t(page) + info.region_size - va);
}

// an engine block freed while armed: freed once disarmed (see the top)
void DeferEngineFree(uint32_t block) {
  std::lock_guard lock(g_lock);
  if (SimThread()) g_engine_frees.push_back({rb::CurrentFrame(), block});
  else g_other_frees.push_back(block);
}

// an audio heap block freed: one the other threads took while armed is freed by the simulation's threads as an engine
// block is (the restored simulation may still hold it), by anyone else at once
void FreeAudioBlock(uint32_t block) {
  {
    std::lock_guard lock(g_lock);
    auto it = g_others_audio.find(block);
    if (it != g_others_audio.end()) {
      if (rb::Armed() && SimThread()) {
        g_engine_frees.push_back({rb::CurrentFrame(), block});
        return;
      }
      g_others_audio.erase(it);
    }
  }
  Audio().Free(block);
}

bool OthersAudio(uint32_t block) {
  std::lock_guard lock(g_lock);
  return g_others_audio.count(block) != 0;
}

uint32_t Allocate(simheap::Heap* heap, uint32_t size, uint32_t alignment) {
  const uint32_t p = heap->Malloc(size, alignment);
  if (p) (heap == &Audio() ? g_audio : g_routed)++;
  else g_fallback++;
  return p;
}

// a simulation block freed: no longer another thread's
void Forget(uint32_t block) {
  uint32_t size = 0;
  {
    std::lock_guard lock(g_lock);
    auto it = g_volatile_blocks.find(block);
    if (it == g_volatile_blocks.end()) return;
    size = it->second;
    g_volatile_blocks.erase(it);
  }
  rb::RemoveVolatile(block, size);
}

void FreeSimBlock(const PPCContext& ctx, uint32_t block) {
  if (SimThread()) {
    Forget(block);
    SimFree(ctx, block);
  } else {
    std::lock_guard lock(g_lock);
    g_sim_frees.push_back(block);
  }
}

// a rollback went back to the top of `frame`: what the frames since freed is theirs to free again
void RolledBack(uint32_t frame) {
  g_restores++;
  std::lock_guard lock(g_lock);
  std::erase_if(g_engine_frees, [&](const EngineFree& f) { return f.frame >= frame; });
}

const bool g_listening = [] {
  rb::OnRestore(&RolledBack);
  return true;
}();

}  // namespace

void OnFrameTop(PPCContext& ctx, uint8_t* base) {
  std::vector<uint32_t> sim;
  std::vector<EngineFree> engine;
  {
    std::lock_guard lock(g_lock);
    sim.swap(g_sim_frees);
    if (!rb::Armed()) {
      engine.swap(g_engine_frees);
      for (const uint32_t b : g_other_frees) engine.push_back({0, b});
      g_other_frees.clear();
    }
  }
  for (uint32_t p : sim) {
    Forget(p);
    Sim().Free(p);
  }
  // blocks other threads wrote: theirs
  for (const rb::OtherWrite& w : rb::TakeOtherThreadsWrites()) {
    uint32_t payload = 0, size = 0;
    if (!Sim().BlockOf(w.va, payload, size)) continue;
    {
      std::lock_guard lock(g_lock);
      if (!g_volatile_blocks.emplace(payload, size).second) continue;
    }
    rb::AddVolatile(payload, size);
    static int told = 0;
    if (told++ < 40) {
      std::string from;
      for (uint32_t r : w.returns) from += fmt::format(" {:08X}", r);
      REXLOG_INFO("mk9 gmalloc: simulation block {:08X} ({} bytes) is another thread's from now on (host thread {} "
                  "wrote {:08X}, from{})",
                  payload, size, w.host_tid, w.va, from);
    }
  }
  for (const EngineFree& f : engine) {
    if (Audio().Owns(f.block)) {
      FreeAudioBlock(f.block);
      continue;
    }
    rex::CallFrame frame(ctx);
    frame.ctx.r1.u32 -= 0x200;
    frame.ctx.r3.u32 = kGMalloc;
    frame.ctx.r4.u32 = f.block;
    __imp__sub_829AEB40(frame.ctx, base);
  }
  static uint64_t frames = 0;
  if (++frames % 3600 == 0) {
    const auto st = Sim().GetStats(), au = Audio().GetStats();
    REXLOG_INFO(
        "mk9 gmalloc: simulation heap {} MB in {} blocks (peak {} MB), audio heap {} MB (peak {} MB); {} allocations "
        "routed, {} to audio, {} fell back, {} engine blocks moved while armed",
        st.in_use >> 20, st.blocks, st.peak >> 20, au.in_use >> 20, au.peak >> 20, g_routed.load(), g_audio.load(),
        g_fallback.load(), g_engine_reallocs.load());
  }
}

}  // namespace mk9::gmalloc

using namespace mk9;

// the objects UE3's deferred cleanup is about to finish, looked at first (diagnostics, with
// MK9_GMALLOC_TRACE=1): one that is not an object any more says how it got there
REX_HOOK_RAW(sub_82741730) {
  if (gmalloc::g_trace) {
    const uint32_t list = ctx.r3.u32;
    const uint32_t data = game::ReadU32(list), count = game::ReadU32(list + 4);
    for (uint32_t i = 0; i < count && i < 4096; i++) {
      const uint32_t obj = game::ReadU32(data + 4 * i);
      const uint32_t vt = obj ? game::ReadU32(obj) : 0;
      const uint32_t fn = vt >= 0x82000000 && vt < 0x84000000 ? game::ReadU32(vt + 4) : 0;
      if (fn >= 0x82000000 && fn < 0x84000000) continue;
      const char* heap = gmalloc::Sim().Owns(obj) ? "simulation" : gmalloc::Audio().Owns(obj) ? "audio" : "engine";
      REXLOG_ERROR("mk9 gmalloc: deferred cleanup {:08X}[{}] = {:08X} ({} heap) has vtable {:08X} in frame {}; {}",
                   data, i, obj, heap, vt, rb::CurrentFrame(), gmalloc::LastFree(obj));
    }
  }
  __imp__sub_82741730(ctx, base);
}

// UE3's file callbacks for FMOD: what they allocate is the audio heap's
#define MK9_AUDIO_FILE_HOOK(fn)       \
  REX_HOOK_RAW(fn) {                  \
    gmalloc::t_audio_file++;          \
    __imp__##fn(ctx, base);           \
    gmalloc::t_audio_file--;          \
  }
MK9_AUDIO_FILE_HOOK(sub_82602B30)
MK9_AUDIO_FILE_HOOK(sub_82602D00)
MK9_AUDIO_FILE_HOOK(sub_82602D88)

REX_HOOK_RAW(sub_829AEB30) {
  bool others = false;
  if (auto* heap = gmalloc::Route(ctx, &others)) {
    if (const uint32_t p = gmalloc::Allocate(heap, ctx.r4.u32, ctx.r5.u32)) {
      if (others) gmalloc::NoteOthers(p);
      ctx.r3.u64 = p;
      return;
    }
  }
  __imp__sub_829AEB30(ctx, base);
}

REX_HOOK_RAW(sub_829AEB38) {
  const uint32_t block = ctx.r4.u32, size = ctx.r5.u32, alignment = ctx.r6.u32;
  auto& sim = gmalloc::Sim();
  auto& audio = gmalloc::Audio();
  auto* mem = REX_KERNEL_STATE()->memory();
  if (block == 0) {
    bool others = false;
    if (auto* heap = gmalloc::Route(ctx, &others)) {
      if (const uint32_t p = gmalloc::Allocate(heap, size, alignment)) {
        if (others) gmalloc::NoteOthers(p);
        ctx.r3.u64 = p;
        return;
      }
    }
    return __imp__sub_829AEB38(ctx, base);
  }
  // While armed a block the restored simulation may still hold goes to a new one, and is freed as an engine block is
  // (see the top): an engine block (the engine heap's bookkeeping stays as it is then), and an audio block the other
  // threads took that a simulation thread reallocates. The simulation's threads take the new one from the simulation
  // heap, the others from the audio heap
  const bool sim_thread = gmalloc::SimThread();
  const bool engine = !audio.Owns(block) && !sim.Owns(block);
  if (rb::Armed() && (engine || (sim_thread && audio.Owns(block) && gmalloc::OthersAudio(block)))) {
    if (size == 0) {
      if (engine) gmalloc::DeferEngineFree(block);
      else gmalloc::FreeAudioBlock(block);
      ctx.r3.u64 = 0;
      return;
    }
    simheap::Heap* heap = sim_thread ? (gmalloc::SimReady() ? &sim : nullptr) : (gmalloc::AudioReady() ? &audio : nullptr);
    const uint32_t have = engine ? gmalloc::CommittedFrom(block) : audio.UsableSize(block);
    if (heap && have) {
      if (const uint32_t p = gmalloc::Allocate(heap, size, alignment)) {
        // an engine block's size is the engine's to know: what follows it up to `size` comes along, unused
        std::memcpy(mem->TranslateVirtual<uint8_t*>(p), mem->TranslateVirtual<uint8_t*>(block), std::min(size, have));
        if (!sim_thread) gmalloc::NoteOthers(p);
        if (engine) gmalloc::DeferEngineFree(block);
        else gmalloc::FreeAudioBlock(block);
        if (engine && gmalloc::g_engine_reallocs++ < 10)
          REXLOG_INFO("mk9 gmalloc: engine block {:08X} moved to {:08X} ({} bytes) while armed (host thread {}, lr {:08X})",
                      block, p, size, GetCurrentThreadId(), uint32_t(ctx.lr));
        ctx.r3.u64 = p;
        return;
      }
    }
    // no room: the engine's own realloc, as before
    if (engine) {
      REXLOG_WARN("mk9 gmalloc: engine block {:08X} reallocated to {} bytes while armed, no room elsewhere (lr {:08X})",
                  block, size, uint32_t(ctx.lr));
      return __imp__sub_829AEB38(ctx, base);
    }
  }
  if (audio.Owns(block) || sim.Owns(block)) {
    auto& heap = audio.Owns(block) ? audio : sim;
    // the simulation heap's blocks move only on the simulation's threads
    const bool here = &heap == &audio || sim_thread;
    if (size == 0) {
      if (&heap == &audio) gmalloc::FreeAudioBlock(block);
      else gmalloc::FreeSimBlock(ctx, block);
      ctx.r3.u64 = 0;
      return;
    }
    if (here) {
      if (const uint32_t p = heap.Realloc(block, size, alignment)) {
        if (p != block && &heap == &sim) gmalloc::Forget(block);
        if (p != block && &heap == &audio && gmalloc::OthersAudio(block)) {
          std::lock_guard lock(gmalloc::g_lock);
          gmalloc::g_others_audio.erase(block);
          gmalloc::g_others_audio.insert(p);
        }
        ctx.r3.u64 = p;
        return;
      }
    }
    // another thread's, or no room left: the engine's heap takes it (another thread's while armed: the audio heap)
    const uint32_t have = heap.UsableSize(block);
    uint32_t p = 0;
    if (rb::Armed() && !sim_thread && gmalloc::AudioReady() && (p = gmalloc::Allocate(&audio, size, alignment))) {
      gmalloc::NoteOthers(p);
    } else {
      ctx.r4.u64 = 0;
      __imp__sub_829AEB38(ctx, base);
      p = ctx.r3.u32;
    }
    if (p) {
      std::memcpy(mem->TranslateVirtual<uint8_t*>(p), mem->TranslateVirtual<uint8_t*>(block), std::min(size, have));
      if (&heap == &audio) gmalloc::FreeAudioBlock(block);
      else gmalloc::FreeSimBlock(ctx, block);
    }
    ctx.r3.u64 = p;
    return;
  }
  // an engine block, not armed
  __imp__sub_829AEB38(ctx, base);
}

REX_HOOK_RAW(sub_829AEB40) {
  const uint32_t block = ctx.r4.u32;
  if (!block) return;
  if (gmalloc::Audio().Owns(block)) return gmalloc::FreeAudioBlock(block);
  if (gmalloc::Sim().Owns(block)) return gmalloc::FreeSimBlock(ctx, block);
  if (rb::Armed()) return gmalloc::DeferEngineFree(block);
  __imp__sub_829AEB40(ctx, base);
}
