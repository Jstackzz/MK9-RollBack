// The simulation's file reads while rollback is armed.
//
// UE3 reads files on a thread of its own (FAsyncIOSystem, the object at 0x833DD540's IO system, a 208-byte block of the
// simulation heap): QueueIORequest (sub_827288E0, from LoadData and LoadCompressedData, its vtable +8 and +12) queues a
// request, and the thread (sub_827327B8) reads it into the asker's memory (`dest`) and then counts the asker's
// counter down (its loop at 0x82732A70). A package the simulation loads asks that way: FArchiveAsync::Precache
// (sub_8270C5E8) takes a buffer from the simulation heap (+168), counts its request counter (+176) up and asks; the
// loader looks at the counter in later frames. In a fight's last frame, as the match ends, the engine starts such a
// load (lr 8270C780 under 826F6310 82704238 827286C4 8272888C 82870AB8: ULinkerLoad's async load from the tick), and
// netplay's last rollbacks of the fight go back over that frame: the restore frees the buffer and puts the counter
// back while the read is under way or done (the block written then was "another thread's" from the next frame's top
// on, so it was left as the thread wrote it, under memory the heap called free again), and the frame run again asks
// again. The reads land in memory the frames run again used for other things, the counter ends one off, and the loader
// reads a package that is not there: 0.5.5's crashes at a fight's end (the archive's Malloc, lr 8270C780, on a heap
// written over; a package tag that was not 0x9E2A83C1, sub_826FE460 under the same load).
//
// So while rollback is armed a simulation thread's read is done before QueueIORequest returns, and the reading thread
// never writes the simulation's memory: it reads into a buffer of the audio heap (untracked) holding a copy of what
// was at `dest`, and counts down a counter there. Once that reaches 0 the simulation's thread copies the buffer to
// `dest` and counts the asker's counter down itself: the simulation's writes, in the frame that asked, which a
// restore puts back like any other, and a frame run again asks again. Reads other threads ask for (FMOD's streams,
// through UE3's file callbacks) and reads that write no memory rollback keeps go on as before.
// What QueueIORequest allocates (the queue's growth, the request's copy of the file name, which the loader's thread
// frees as it takes the request) is the loader's own memory, from the audio heap (gmalloc.h, LoaderMemory): in the
// simulation heap such a block was given back by a rollback while the loader held it, and the loader's free of it
// came at a frame's top after the frames run again had put something else there.
// MK9_ASYNC_READS=1 leaves the simulation's reads as the game makes them (comparisons).

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "mk9/engine.h"
#include "mk9/game.h"
#include "mk9/gmalloc.h"
#include "mk9/net/rollback.h"
#include "mk9/simheap.h"

REX_EXTERN(__imp__sub_827288E0);  // FAsyncIOSystemBase::QueueIORequest

namespace mk9::asyncio {
namespace {

using Clock = std::chrono::steady_clock;

// QueueIORequest(this, file name (an FString), offset, size, uncompressed size, dest, compression flags, ?, counter,
// priority): the counter and the priority are the 9th and 10th arguments, in the caller's parameter area. The thread
// writes `uncompressed size` bytes to dest when that is not 0 (it decompresses), else `size`
constexpr uint32_t kCounterArg = 84;

const bool g_off = [] {
  const char* v = std::getenv("MK9_ASYNC_READS");
  return v && *v == '1';
}();

bool SimThread() {
  thread_local int cached = -1;
  if (cached >= 0) return cached != 0;
  bool sim = engine::IsSimulationThread(GetCurrentThreadId());
  if (!sim) {
    auto* t = rex::system::XThread::GetCurrentThread();
    sim = t && t->main_thread();
  }
  if (sim) cached = 1;  // anything else may become one later (mk9/gmalloc.cpp does the same)
  return sim;
}

// memory rollback keeps (rollback.h): the virtual heaps below the stacks and the executable's data, but the audio heap
bool Kept(uint32_t va, uint32_t n) {
  if (!va || !n) return false;
  const uint64_t end = uint64_t(va) + n;
  const bool tracked = (va >= 0x00010000u && end <= 0x70000000u) || (va >= 0x80000000u && end <= 0x90000000u);
  return tracked && !simheap::Audio().Owns(va);
}

// an FString's text (MK9's TCHAR is a byte: the loader's file names in memory read `..\A`)
std::string FileName(uint32_t fstring) {
  const uint32_t data = game::ReadU32(fstring), num = game::ReadU32(fstring + 4);
  std::string s;
  if (!data) return s;
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint8_t* p = mem->TranslateVirtual<const uint8_t*>(data);
  for (uint32_t i = 0; i + 1 < num && i < 160; i++) s += p[i] >= 32 && p[i] < 127 ? char(p[i]) : '?';
  return s;
}

// the guest's counter (big-endian) counted down, as the reading thread would (lwarx/stwcx.)
void CountDown(uint32_t va) {
  auto* p = REX_KERNEL_STATE()->memory()->TranslateVirtual<volatile LONG*>(va);
  for (;;) {
    const LONG old = *p;
    const LONG want = LONG(_byteswap_ulong(_byteswap_ulong(ULONG(old)) - 1));
    if (InterlockedCompareExchange(p, want, old) == old) return;
  }
}

// the original, what it allocates (the queue's growth, the request's copy of the file name, which the loader's thread
// frees) the loader's own memory, whoever asks (gmalloc.h)
void Queue(PPCContext& ctx, uint8_t* base) {
  gmalloc::LoaderMemory loader;
  __imp__sub_827288E0(ctx, base);
}

std::atomic<uint64_t> g_reads{0}, g_bytes{0}, g_wait_us{0}, g_longest_us{0};
std::mutex g_log_lock;
Clock::time_point g_last_summary{};

}  // namespace
}  // namespace mk9::asyncio

using namespace mk9;

REX_HOOK_RAW(sub_827288E0) {
  using namespace mk9::asyncio;
  if (g_off || !rb::Armed() || !SimThread()) return Queue(ctx, base);
  const uint32_t name = ctx.r4.u32, offset = ctx.r5.u32, size = ctx.r6.u32, uncompressed = ctx.r7.u32;
  const uint32_t dest = ctx.r8.u32;
  const uint32_t counter_at = ctx.r1.u32 + kCounterArg;
  const uint32_t counter = game::ReadU32(counter_at);
  const uint32_t n = uncompressed ? uncompressed : size;
  const bool kept_dest = Kept(dest, n), kept_counter = Kept(counter, 4);
  if (!kept_dest && !kept_counter) return Queue(ctx, base);
  auto& audio = simheap::Audio();
  const uint32_t shadow = audio.Malloc(16, 16);
  const uint32_t buffer = kept_dest && shadow ? audio.Malloc(n, 128) : 0;
  if (!shadow || (kept_dest && !buffer)) {
    if (shadow) audio.Free(shadow);
    static int told = 0;
    if (told++ < 10)
      REXLOG_WARN("mk9 asyncio: no room in the audio heap for a read of {} bytes; it stays the game's own", n);
    return Queue(ctx, base);
  }
  // who asked (logged below; the call changes lr)
  const uint32_t lr = uint32_t(ctx.lr);
  uint32_t returns[8];
  const int depth = game::StackReturns(ctx, returns, 8);
  auto* mem = REX_KERNEL_STATE()->memory();
  if (buffer) std::memcpy(mem->TranslateVirtual<uint8_t*>(buffer), mem->TranslateVirtual<uint8_t*>(dest), n);
  game::WriteU32(shadow, 1);
  if (buffer) ctx.r8.u64 = buffer;
  game::WriteU32(counter_at, shadow);
  const auto t0 = Clock::now();
  Queue(ctx, base);
  const uint64_t index = ctx.r3.u64;
  game::WriteU32(counter_at, counter);
  auto told = t0;
  for (int spins = 0; game::ReadU32(shadow) != 0; spins++) {
    if (spins > 20000) Sleep(1);
    else if (spins > 64) std::this_thread::yield();
    if (spins % 1024 == 0 && Clock::now() - told > std::chrono::seconds(5)) {
      told = Clock::now();
      REXLOG_WARN("mk9 asyncio: still waiting for the read of {} ({} bytes at {}) after {:.0f} s", FileName(name), n,
                  offset, std::chrono::duration<double>(told - t0).count());
    }
  }
  const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
  if (buffer) {
    std::memcpy(mem->TranslateVirtual<uint8_t*>(dest), mem->TranslateVirtual<uint8_t*>(buffer), n);
    audio.Free(buffer);
  }
  if (counter) CountDown(counter);
  audio.Free(shadow);
  ctx.r3.u64 = index;

  const uint64_t k = ++g_reads;
  g_bytes += n;
  g_wait_us += uint64_t(waited);
  for (uint64_t l = g_longest_us.load(); uint64_t(waited) > l && !g_longest_us.compare_exchange_weak(l, uint64_t(waited));) {
  }
  std::lock_guard lock(g_log_lock);
  if (k <= 40 || k % 200 == 0) {
    std::string from;
    for (int i = 0; i < depth; i++) from += fmt::format(" {:08X}", returns[i]);
    REXLOG_INFO("mk9 asyncio: read {} of the simulation done in frame {} ({} bytes at {} of {}{} into {:08X}, counter "
                "{:08X}; host thread {}, {:.2f} ms; from {:08X}{})",
                k, rb::CurrentFrame(), n, offset, FileName(name), uncompressed ? ", compressed" : "", dest, counter,
                GetCurrentThreadId(), waited / 1000.0, lr, from);
  }
  if (Clock::now() - g_last_summary > std::chrono::seconds(30)) {
    g_last_summary = Clock::now();
    REXLOG_INFO("mk9 asyncio: {} reads of the simulation done in their frames so far, {:.1f} MB, waited {:.1f} ms in all, "
                "longest {:.1f} ms",
                k, g_bytes.load() / 1048576.0, g_wait_us.load() / 1000.0, g_longest_us.load() / 1000.0);
  }
}
