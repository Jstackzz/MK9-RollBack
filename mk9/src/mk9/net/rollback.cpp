// Rollback state machinery, see rollback.h.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "mk9/net/rollback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>
#include <rex/system/xmemory.h>

#include "mk9/frame.h"
#include "mk9/game.h"

namespace mk9::rb {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kPage = 0x1000;
// what is tracked: the virtual heaps below the stacks, and the executable's writable sections
struct Span {
  uint32_t lo, hi;
};
constexpr Span kTracked[] = {{0x00010000, 0x70000000}, {0x80000000, 0x90000000}};

enum : uint8_t { kUntracked = 0, kClean = 1, kDirty = 2, kForeign = 3 };

std::vector<uint8_t> g_page(0x100000, kUntracked);  // per 4 KB guest page
std::mutex g_lock;
bool g_armed = false;
bool g_restoring = false;
DWORD g_main_tid = 0;
std::vector<DWORD> g_sim_tids;  // other threads whose writes are the simulation's (the rendering thread)

// MK9_RB_TRACE_FOREIGN=1 (diagnostics): pages another thread wrote first stay protected, and every
// write a simulation thread makes to one of them is counted and the first ones logged with the
// guest's last call: what a restore would miss
const bool g_trace_foreign = [] {
  const char* v = std::getenv("MK9_RB_TRACE_FOREIGN");
  return v && *v == '1';
}();
// MK9_RB_TRACE_FMOD=1 (diagnostics): the tracked pages a simulation thread first writes from FMOD's code (0x82DC0000 to
// 0x82F00000, with its memory module), with the guest's stack: FMOD's state a restore would put back without its heap
const bool g_trace_fmod = [] {
  const char* v = std::getenv("MK9_RB_TRACE_FMOD");
  return v && *v == '1';
}();
std::vector<uint32_t> g_foreign_list;  // to protect again at each CloseFrame
uint64_t g_foreign_hits = 0;

bool SimThread(DWORD tid) {
  return tid == g_main_tid || std::find(g_sim_tids.begin(), g_sim_tids.end(), tid) != g_sim_tids.end();
}
uint8_t* g_base = nullptr;
PVOID g_veh = nullptr;
std::vector<uint32_t> g_dirty;  // pages made writable since the last CloseFrame

// 64 KB blocks of the simulation heap: the pages first written in this frame (each one a fault), and which blocks had
// most of their pages written when last measured, whose clean pages a fault takes all at once (Take). Every 16th frame
// takes none ahead and measures again (pages taken ahead are written without a fault, so they cannot be counted).
// About 2,200 pages a fight frame were first written, 95 % of them in the simulation heap, five to a block, each fault
// some 7 us. MK9_RB_NO_AHEAD=1 takes pages one at a time.
constexpr uint16_t kDenseMin = 4;  // of a block's 16 pages
std::vector<uint16_t> g_block_writes(0x10000, 0);
std::vector<uint32_t> g_written_blocks;  // the blocks g_block_writes counts in this frame
std::vector<uint8_t> g_dense(0x10000, 0);
std::vector<uint32_t> g_dense_list;
bool g_speculating = false;
uint32_t g_closed_frames = 0;
const bool g_no_ahead = [] {
  const char* v = std::getenv("MK9_RB_NO_AHEAD");
  return v && *v == '1';
}();

// bytes another thread owns on pages the main thread also writes
struct Mask {
  uint64_t bits[kPage / 64] = {};
};
std::vector<uint8_t> g_shared(0x100000, 0);
std::unordered_map<uint32_t, Mask> g_masks;

struct FrameState {
  uint32_t frame = 0;
  std::vector<uint32_t> pages;  // undo log: pages first written in this frame
  std::vector<uint8_t> data;    // their contents at the top of the frame
  PPCContext ctx;
  bool closed = false;
  uint64_t hash = 0;
};
std::deque<FrameState> g_frames;
// frames no longer kept (the oldest past kKeepFrames, those a restore went back over): their buffers taken again by
// new frames, so a page taken is copied into memory already there (a new frame's growing log reallocated, and every
// page of it was first touched in the fault handler, at about 14 MB of pages a frame)
std::vector<FrameState> g_spare_frames;

void Retire(FrameState&& f) {
  f.pages.clear();
  f.data.clear();
  f.closed = false;
  f.hash = 0;
  g_spare_frames.push_back(std::move(f));
}

FrameState& NewFrame() {
  if (g_spare_frames.empty()) return g_frames.emplace_back();
  g_frames.push_back(std::move(g_spare_frames.back()));
  g_spare_frames.pop_back();
  return g_frames.back();
}
std::vector<uint32_t> g_pending_pages;  // taken while no frame is open
std::vector<uint8_t> g_pending_data;
Stats g_stats;
uint64_t g_last_hash = 0;
bool g_hashing = false;
std::atomic<uint32_t> g_current{0};
std::vector<Span> g_untracked;
Span g_sim_region{0, 0};
std::vector<OtherWrite> g_other_writes;  // other threads' first writes of simulation region pages
std::map<uint32_t, uint32_t> g_volatile;  // the volatile ranges given: start -> size

std::vector<void (*)(uint32_t)>& RestoreListeners() {
  static std::vector<void (*)(uint32_t)> fns;
  return fns;
}

std::vector<void (*)(uint32_t)>& OpenListeners() {
  static std::vector<void (*)(uint32_t)> fns;
  return fns;
}

template <class F>
void ForVolatile(uint32_t page, F&& fn) {
  auto it = g_masks.find(page);
  if (it == g_masks.end()) return;
  const uint64_t* bits = it->second.bits;
  uint32_t b = 0;
  while (b < kPage) {
    uint64_t w = bits[b >> 6] >> (b & 63);
    if (!w) {
      b = (b | 63) + 1;
      continue;
    }
    if (!(w & 1)) {
      b++;
      continue;
    }
    uint32_t start = b;
    while (b < kPage && ((bits[b >> 6] >> (b & 63)) & 1)) b++;
    fn(start, b - start);
  }
}

void Protect(uint32_t page, bool read_only) {
  DWORD old;
  VirtualProtect(g_base + page, kPage, read_only ? PAGE_READONLY : PAGE_READWRITE, &old);
}

void ProtectRun(uint32_t first, uint32_t count, bool read_only) {
  DWORD old;
  VirtualProtect(g_base + first, size_t(count) * kPage, read_only ? PAGE_READONLY : PAGE_READWRITE, &old);
}

// g_lock held
void UntrackLocked(const Span& s) {
  for (uint32_t p = s.lo & ~(kPage - 1); p < s.hi; p += kPage) {
    const uint8_t st = g_page[p >> 12];
    if (st == kUntracked) continue;
    g_page[p >> 12] = kUntracked;
    if (st == kClean || st == kForeign) Protect(p, false);
  }
}

// g_lock held
void MarkVolatile(uint32_t va, uint32_t size) {
  for (uint64_t at = va; at < uint64_t(va) + size;) {
    uint32_t page = uint32_t(at) & ~(kPage - 1), off = uint32_t(at) - page;
    uint32_t n = uint32_t(std::min<uint64_t>(uint64_t(va) + size - at, kPage - off));
    at += n;
    if (g_page[page >> 12] == kUntracked) continue;
    Mask& m = g_masks[page];
    for (uint32_t b = off; b < off + n; b++) m.bits[b >> 6] |= 1ull << (b & 63);
    g_shared[page >> 12] = 1;
  }
}

// the first write of a page since it was protected (at va); g_lock held
void Take(uint32_t page, uint32_t va) {
  g_stats.faults++;
  const DWORD tid = GetCurrentThreadId();
  const bool sim = SimThread(tid);
  if (tid == g_main_tid) g_stats.faults_main++;
  else if (!g_sim_tids.empty() && tid == g_sim_tids.front()) g_stats.faults_render++;
  else if (sim) g_stats.faults_sim++;
  else g_stats.faults_other++;
  const bool region = page >= g_sim_region.lo && page < g_sim_region.hi;
  if (region && !sim && g_other_writes.size() < 4096) {
    OtherWrite w{va, GetCurrentThreadId(), {}};
    if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
      w.returns[0] = uint32_t(ts->context()->lr);
      game::StackReturns(*ts->context(), w.returns + 1, 7);
    }
    g_other_writes.push_back(w);
  }
  if (g_trace_foreign && region && !sim) {
    // another thread writing the simulation heap: what of the simulation's it writes
    static int logged = 0;
    if (logged++ < 200) {
      if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
        uint32_t returns[12];
        const int n = game::StackReturns(*ts->context(), returns, 12);
        std::string chain;
        for (int i = 0; i < n; i++) chain += fmt::format(" {:08X}", returns[i]);
        REXLOG_WARN("mk9 rollback: host thread {} wrote the simulation heap at page {:08X} (guest lr {:08X}, stack{})",
                    GetCurrentThreadId(), page, uint32_t(ts->context()->lr), chain);
      }
    }
  }
  if (g_trace_fmod && sim) {
    // a simulation thread writing tracked memory from FMOD's code (FMOD's own memory and the audio heap are FMOD's:
    // rollback puts back what the simulation wrote, never what FMOD keeps for itself)
    if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
      uint32_t returns[10];
      returns[0] = uint32_t(ts->context()->lr);
      const int n = 1 + game::StackReturns(*ts->context(), returns + 1, 9);
      static std::vector<uint32_t> told;
      if (std::any_of(returns, returns + n, [](uint32_t a) { return a >= 0x82DC0000 && a < 0x82F00000; }) &&
          told.size() < 400 && std::find(told.begin(), told.end(), page) == told.end()) {
        told.push_back(page);
        std::string chain;
        for (int i = 0; i < n; i++) chain += fmt::format(" {:08X}", returns[i]);
        REXLOG_WARN("mk9 rollback: FMOD's code on host thread {} first wrote page {:08X} ({}) in frame {}, stack{}",
                    tid, page, region ? "simulation heap" : "other", g_current.load(), chain);
      }
    }
  }
  if (sim || region || g_shared[page >> 12]) {
    std::vector<uint32_t>& pages = g_frames.empty() || g_frames.back().closed ? g_pending_pages : g_frames.back().pages;
    std::vector<uint8_t>& data = g_frames.empty() || g_frames.back().closed ? g_pending_data : g_frames.back().data;
    const auto take = [&](uint32_t p) {
      pages.push_back(p);
      size_t at = data.size();
      data.resize(at + kPage);
      std::memcpy(data.data() + at, g_base + p, kPage);
      g_stats.logged_bytes += kPage;
      g_page[p >> 12] = kDirty;
      g_dirty.push_back(p);
    };
    if (g_block_writes[page >> 16]++ == 0) g_written_blocks.push_back(page >> 16);
    if (region && g_speculating && g_dense[page >> 16]) {
      // a 64 KB block of the simulation heap whose pages were mostly written when last measured: every clean page of
      // it now, made writable in runs, one fault for what would be several. Each is copied as the frame's top left it
      // (protected since, so written by no one); in the simulation heap a page is kept whoever writes it, so what a
      // restore puts back is the same
      const uint32_t lo = page & ~0xFFFFu;
      uint32_t run = 0, count = 0;
      for (uint32_t q = lo; q < lo + 0x10000; q += kPage) {
        const bool mine = q == page || (g_page[q >> 12] == kClean && q >= g_sim_region.lo && q < g_sim_region.hi);
        if (mine) {
          take(q);
          if (q != page) g_stats.pages_ahead++;
          if (!count) run = q;
          count++;
          continue;
        }
        if (count) ProtectRun(run, count, false);
        count = 0;
      }
      if (count) ProtectRun(run, count, false);
      return;
    }
    take(page);
  } else {
    g_page[page >> 12] = kForeign;
    g_stats.foreign_pages++;
    if (g_trace_foreign) {
      g_foreign_list.push_back(page);
      // which thread and code wrote it first (diagnostics: what shares pages with the simulation)
      static int logged = 0;
      if (logged++ < 150) {
        uint32_t lr = 0;
        if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) lr = uint32_t(ts->context()->lr);
        REXLOG_WARN("mk9 rollback: host thread {} made {:08X} foreign (guest lr {:08X})", GetCurrentThreadId(), page,
                    lr);
      }
      // each such thread's whole guest stack, once: what it is
      static std::vector<DWORD> told;
      if (std::find(told.begin(), told.end(), GetCurrentThreadId()) == told.end()) {
        told.push_back(GetCurrentThreadId());
        if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) {
          uint32_t returns[24];
          const int n = game::StackReturns(*ts->context(), returns, 24);
          std::string chain;
          for (int i = 0; i < n; i++) chain += fmt::format(" {:08X}", returns[i]);
          REXLOG_WARN("mk9 rollback: host thread {} stack{}", GetCurrentThreadId(), chain);
        }
      }
    }
  }
  Protect(page, false);
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2 || er->ExceptionInformation[0] != 1)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t a = er->ExceptionInformation[1];
  if (!g_base || a < uintptr_t(g_base) || a - uintptr_t(g_base) >= (uint64_t(1) << 32)) return EXCEPTION_CONTINUE_SEARCH;
  const uint32_t va = uint32_t(a - uintptr_t(g_base));
  const uint32_t page = va & ~(kPage - 1);
  const auto entered = Clock::now();
  std::lock_guard<std::mutex> lock(g_lock);
  struct Timed {
    Clock::time_point t;
    ~Timed() { g_stats.fault_us += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t).count()); }
  } timed{entered};
  const uint8_t st = g_page[page >> 12];
  if (st == kUntracked) {
    // Disarm may have let the page go while this thread waited for the lock: retry the write
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
        (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)))
      return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
  }
  // another thread writing a page the main thread uses: those bytes are that thread's from now on
  if (!SimThread(GetCurrentThreadId()) && st != kForeign && st == kDirty) MarkVolatile(va & ~7u, 8);
  if (st == kClean) {
    Take(page, va);
  } else {
    if (g_trace_foreign && st == kForeign && SimThread(GetCurrentThreadId())) {
      if (g_foreign_hits++ < 60) {
        uint32_t lr = 0;
        if (auto* ts = rex::runtime::ThreadState::Get(); ts && ts->context()) lr = uint32_t(ts->context()->lr);
        REXLOG_WARN("mk9 rollback: {} wrote {:08X} on a page another thread wrote first (guest lr {:08X})",
                    GetCurrentThreadId() == g_main_tid ? "the main thread" : "a simulation thread", va, lr);
      }
    }
    Protect(page, false);
  }
  return EXCEPTION_CONTINUE_EXECUTION;
}

void ProtectDirty() {
  std::sort(g_dirty.begin(), g_dirty.end());
  uint32_t run = 0, count = 0;
  for (uint32_t p : g_dirty) {
    if (g_page[p >> 12] != kDirty) continue;
    g_page[p >> 12] = kClean;
    if (count && p == run + count * kPage) {
      count++;
      continue;
    }
    if (count) ProtectRun(run, count, true);
    run = p;
    count = 1;
  }
  if (count) ProtectRun(run, count, true);
  g_dirty.clear();
}

uint64_t HashPages(const std::vector<uint32_t>& pages) {
  std::vector<uint32_t> sorted(pages);
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  uint64_t h = 0x9E3779B97F4A7C15ull;
  alignas(8) uint8_t tmp[kPage];
  for (uint32_t p : sorted) {
    if (g_page[p >> 12] == kForeign) continue;
    std::memcpy(tmp, g_base + p, kPage);
    ForVolatile(p, [&](uint32_t off, uint32_t n) { std::memset(tmp + off, 0, n); });
    h = (h ^ p) * 0x100000001B3ull;
    const uint64_t* w = reinterpret_cast<const uint64_t*>(tmp);
    for (size_t i = 0; i < kPage / 8; i++) h = (h ^ w[i]) * 0x9E3779B97F4A7C15ull + (h >> 29);
  }
  return h;
}

}  // namespace

bool Arm(PPCContext& ctx, uint32_t frame) {
  if (g_armed) return true;
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return false;
  auto* mem = ks->memory();
  g_base = mem->virtual_membase();
  g_main_tid = GetCurrentThreadId();
  if (!g_veh) g_veh = AddVectoredExceptionHandler(1, OnFault);
  const auto t0 = Clock::now();
  std::lock_guard<std::mutex> lock(g_lock);
  std::fill(g_page.begin(), g_page.end(), kUntracked);
  std::fill(g_shared.begin(), g_shared.end(), uint8_t(0));
  g_masks.clear();
  // the first frame measures which blocks to take whole
  for (const uint32_t b : g_dense_list) g_dense[b] = 0;
  g_dense_list.clear();
  for (const uint32_t b : g_written_blocks) g_block_writes[b] = 0;
  g_written_blocks.clear();
  g_speculating = false;
  g_closed_frames = 0;
  g_pending_pages.clear();
  g_pending_data.clear();
  g_stats = Stats{};
  // committed writable pages of the tracked spans that the host can write, protected run by run
  for (const Span& s : kTracked) {
    uint32_t addr = s.lo;
    while (addr < s.hi) {
      auto* heap = mem->LookupHeap(addr);
      rex::memory::HeapAllocationInfo info{};
      if (!heap || !heap->QueryRegionInfo(addr, &info) || !info.region_size) {
        addr += kPage;
        continue;
      }
      uint32_t end = uint32_t(std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, s.hi));
      if ((info.state & 1) && (info.protect & rex::memory::kMemoryProtectWrite)) {
        // the host's view, one query per run of pages alike
        uint32_t p = std::max(addr, info.base_address) & ~(kPage - 1);
        while (p < end) {
          MEMORY_BASIC_INFORMATION mbi{};
          if (!VirtualQuery(g_base + p, &mbi, sizeof mbi)) break;
          const uint64_t region_end = uint64_t(reinterpret_cast<uint8_t*>(mbi.BaseAddress) - g_base) + mbi.RegionSize;
          const uint32_t run_end = uint32_t(std::min<uint64_t>(region_end, end));
          const bool writable =
              mbi.State == MEM_COMMIT && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD);
          if (writable && run_end > p) {
            const uint32_t count = (run_end - p) / kPage;
            std::fill(g_page.begin() + (p >> 12), g_page.begin() + (p >> 12) + count, uint8_t(kClean));
            g_stats.tracked_pages += count;
          }
          p = std::max(run_end, p + kPage);
        }
      }
      addr = std::max(end, addr + kPage);
    }
  }
  for (const Span& s : g_untracked) UntrackLocked(s);
  // the volatile bytes before any page is protected: no other thread's first write can make a
  // page with them foreign
  for (const auto& [va, size] : g_volatile) MarkVolatile(va, size);
  {
    uint32_t run = 0, count = 0;
    for (uint64_t p = 0; p < 0x100000000ull; p += kPage) {
      if (g_page[size_t(p >> 12)] == kClean) {
        if (!count) run = uint32_t(p);
        count++;
      } else if (count) {
        ProtectRun(run, count, true);
        count = 0;
      }
    }
    if (count) ProtectRun(run, count, true);
  }
  g_frames.clear();
  g_dirty.clear();
  g_armed = true;
  g_frames.emplace_back();
  g_frames.back().frame = frame;
  g_frames.back().ctx = ctx;
  g_current = frame;
  g_stats.arm_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  REXLOG_INFO("mk9 rollback: armed at frame {}, {} pages tracked ({:.1f} MB), {:.1f} ms", frame,
              g_stats.tracked_pages, double(g_stats.tracked_pages) * 4 / 1024, g_stats.arm_ms);
  return true;
}

void Disarm() {
  if (!g_armed) return;
  std::lock_guard<std::mutex> lock(g_lock);
  uint32_t run = 0, count = 0;
  for (uint64_t p = 0; p < 0x100000000ull; p += kPage) {
    const uint8_t st = g_page[size_t(p >> 12)];
    const bool take = st == kClean || st == kDirty;
    if (take) {
      if (!count) run = uint32_t(p);
      count++;
    } else if (count) {
      ProtectRun(run, count, false);
      count = 0;
    }
    g_page[size_t(p >> 12)] = kUntracked;
  }
  if (count) ProtectRun(run, count, false);
  g_frames.clear();
  g_spare_frames.clear();  // the logs' memory given back between fights
  std::vector<uint32_t>().swap(g_pending_pages);
  std::vector<uint8_t>().swap(g_pending_data);
  g_dirty.clear();
  g_armed = false;
  REXLOG_INFO("mk9 rollback: disarmed ({} faults, {} foreign pages)", g_stats.faults, g_stats.foreign_pages);
}

bool Armed() { return g_armed; }
bool Restoring() { return g_restoring; }

void CloseFrame() {
  if (!g_armed || g_frames.empty() || g_frames.back().closed) return;
  const auto t0 = Clock::now();
  std::lock_guard<std::mutex> lock(g_lock);
  FrameState& f = g_frames.back();
  if (g_hashing) {
    f.hash = HashPages(f.pages);
    g_last_hash = f.hash;
  }
  g_stats.last_pages = uint32_t(f.pages.size());
  {
    uint32_t region = 0;
    std::vector<uint32_t> blocks;
    blocks.reserve(f.pages.size());
    for (const uint32_t p : f.pages) {
      if (p >= g_sim_region.lo && p < g_sim_region.hi) region++;
      blocks.push_back(p >> 16);
    }
    std::sort(blocks.begin(), blocks.end());
    g_stats.last_region_pages = region;
    g_stats.last_blocks = uint32_t(std::unique(blocks.begin(), blocks.end()) - blocks.begin());
  }
  // a measuring frame: the blocks with most of their pages written are taken whole from the next frame on
  if (!g_speculating) {
    for (const uint32_t b : g_dense_list) g_dense[b] = 0;
    g_dense_list.clear();
    for (const uint32_t b : g_written_blocks)
      if (g_block_writes[b] >= kDenseMin) {
        g_dense[b] = 1;
        g_dense_list.push_back(b);
      }
    g_stats.dense_blocks = uint32_t(g_dense_list.size());
  }
  for (const uint32_t b : g_written_blocks) g_block_writes[b] = 0;
  g_written_blocks.clear();
  g_speculating = !g_no_ahead && ++g_closed_frames % 16 != 0;
  f.closed = true;
  ProtectDirty();
  if (g_trace_foreign)
    for (uint32_t p : g_foreign_list) Protect(p, true);
  while (g_frames.size() > size_t(kKeepFrames)) {
    Retire(std::move(g_frames.front()));
    g_frames.pop_front();
  }
  g_stats.close_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

bool RestoreLocked(uint32_t frame, PPCContext& ctx);

bool Restore(uint32_t frame, PPCContext& ctx) {
  if (!g_armed) return false;
  bool ok;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    ok = RestoreLocked(frame, ctx);
  }
  if (ok) {
    g_current = frame;
    for (auto fn : RestoreListeners()) fn(frame);
  }
  return ok;
}

bool RestoreLocked(uint32_t frame, PPCContext& ctx) {
  const auto t0 = Clock::now();
  auto it = std::find_if(g_frames.begin(), g_frames.end(), [&](const FrameState& f) { return f.frame == frame; });
  if (it == g_frames.end()) {
    REXLOG_ERROR("mk9 rollback: frame {} is no longer kept (oldest {})", frame,
                 g_frames.empty() ? 0 : g_frames.front().frame);
    return false;
  }
  struct Flag {
    Flag() { g_restoring = true; }
    ~Flag() { g_restoring = false; }
  } restoring;
  std::vector<uint32_t> touched;
  static std::vector<uint8_t> mark(0x100000, 0);
  // every page to put back made writable first, runs of neighbours in one call each (a call a page, twice, was
  // most of a restore's time: about 9 ms for a rollback of 3-4 frames)
  for (auto f = g_frames.rbegin(); f != g_frames.rend(); ++f) {
    for (const uint32_t p : f->pages) {
      if (g_page[p >> 12] == kForeign || g_page[p >> 12] == kUntracked || mark[p >> 12]) continue;
      mark[p >> 12] = 1;
      touched.push_back(p);
    }
    if (f->frame == frame) break;
  }
  std::sort(touched.begin(), touched.end());
  const auto protect_runs = [&](bool read_only) {
    uint32_t run = 0, count = 0;
    for (const uint32_t p : touched) {
      if (count && p == run + count * kPage) {
        count++;
        continue;
      }
      if (count) ProtectRun(run, count, read_only);
      run = p;
      count = 1;
    }
    if (count) ProtectRun(run, count, read_only);
  };
  protect_runs(false);
  // each page put back once, from the oldest of these frames that kept it: its contents at that frame's top are its
  // contents at this one's, nothing having written it in between (a copy from every frame that kept it, newest first,
  // copied a page written every frame once for each frame gone back)
  for (auto f = it; f != g_frames.end(); ++f) {
    for (size_t i = 0; i < f->pages.size(); i++) {
      const uint32_t p = f->pages[i];
      if (mark[p >> 12] != 1) continue;  // foreign, untracked, or put back already
      mark[p >> 12] = 2;
      if (g_shared[p >> 12]) {
        alignas(8) uint8_t now[kPage];
        std::memcpy(now, g_base + p, kPage);
        std::memcpy(g_base + p, f->data.data() + i * kPage, kPage);
        ForVolatile(p, [&](uint32_t off, uint32_t n) { std::memcpy(g_base + p + off, now + off, n); });
      } else {
        std::memcpy(g_base + p, f->data.data() + i * kPage, kPage);
      }
    }
  }
  for (uint32_t p : touched) {
    mark[p >> 12] = 0;
    g_page[p >> 12] = kClean;
  }
  protect_runs(true);
  ctx = it->ctx;
  for (auto f = it; f != g_frames.end(); ++f) Retire(std::move(*f));
  g_frames.erase(it, g_frames.end());
  g_dirty.erase(std::remove_if(g_dirty.begin(), g_dirty.end(), [](uint32_t p) { return g_page[p >> 12] != kDirty; }),
                g_dirty.end());
  g_stats.restore_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  return true;
}

void OpenFrameLocked(uint32_t frame, PPCContext& ctx);

void OpenFrame(uint32_t frame, PPCContext& ctx) {
  if (!g_armed) return;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    OpenFrameLocked(frame, ctx);
  }
  for (auto fn : OpenListeners()) fn(frame);
}

void OpenFrameLocked(uint32_t frame, PPCContext& ctx) {
  g_current = frame;
  if (!g_frames.empty() && !g_frames.back().closed && g_frames.back().frame == frame) return;
  FrameState& f = NewFrame();
  f.frame = frame;
  f.ctx = ctx;
  // pages taken between the last close and now: written before this frame ran, their contents then belong here
  f.pages.swap(g_pending_pages);
  f.data.swap(g_pending_data);
  g_pending_pages.clear();
  g_pending_data.clear();
}

uint32_t OldestFrame() {
  std::lock_guard<std::mutex> lock(g_lock);
  return g_frames.empty() ? 0 : g_frames.front().frame;
}

uint32_t CurrentFrame() { return g_current.load(std::memory_order_relaxed); }

void OnRestore(void (*fn)(uint32_t frame)) { RestoreListeners().push_back(fn); }
void OnOpen(void (*fn)(uint32_t frame)) { OpenListeners().push_back(fn); }

void SetHashing(bool on) { g_hashing = on; }
uint64_t LastFrameHash() { return g_last_hash; }
uint32_t LastFramePages() { return g_stats.last_pages; }

std::vector<uint32_t> LastFramePageList() {
  std::lock_guard<std::mutex> lock(g_lock);
  for (auto it = g_frames.rbegin(); it != g_frames.rend(); ++it)
    if (it->closed) return it->pages;
  return {};
}

void AddVolatile(uint32_t va, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  auto& n = g_volatile[va];
  n = std::max(n, size);
  if (g_armed) MarkVolatile(va, size);
}

void SetSimulationRegion(uint32_t va, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_sim_region = {va, uint32_t(std::min<uint64_t>(uint64_t(va) + size, 0xFFFFF000u))};
}

void RemoveVolatile(uint32_t va, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_volatile.erase(va);
  for (uint64_t at = va; at < uint64_t(va) + size;) {
    const uint32_t page = uint32_t(at) & ~(kPage - 1), off = uint32_t(at) - page;
    const uint32_t n = uint32_t(std::min<uint64_t>(uint64_t(va) + size - at, kPage - off));
    at += n;
    auto it = g_masks.find(page);
    if (it == g_masks.end()) continue;
    for (uint32_t b = off; b < off + n; b++) it->second.bits[b >> 6] &= ~(1ull << (b & 63));
  }
}

std::vector<OtherWrite> TakeOtherThreadsWrites() {
  std::lock_guard<std::mutex> lock(g_lock);
  std::vector<OtherWrite> out;
  out.swap(g_other_writes);
  return out;
}

void Untrack(uint32_t va, uint32_t size) {
  std::lock_guard<std::mutex> lock(g_lock);
  const Span s{va, uint32_t(std::min<uint64_t>(uint64_t(va) + size, 0xFFFFF000u))};
  g_untracked.push_back(s);
  if (g_armed) UntrackLocked(s);
}

void AddSimulationThread(uint32_t host_tid) {
  std::lock_guard<std::mutex> lock(g_lock);
  if (!SimThread(host_tid)) g_sim_tids.push_back(host_tid);
}

void RemoveSimulationThread(uint32_t host_tid) {
  std::lock_guard<std::mutex> lock(g_lock);
  std::erase(g_sim_tids, DWORD(host_tid));
}

namespace {
std::vector<uint32_t> g_snap_pages;
std::vector<uint8_t> g_snap_data;
}  // namespace

void DebugSnapshot() {
  std::lock_guard<std::mutex> lock(g_lock);
  g_snap_pages.clear();
  g_snap_data.clear();
  for (uint64_t p = 0; p < 0x100000000ull; p += kPage)
    if (g_page[size_t(p >> 12)] != kUntracked) g_snap_pages.push_back(uint32_t(p));
  g_snap_data.resize(g_snap_pages.size() * size_t(kPage));
  for (size_t i = 0; i < g_snap_pages.size(); i++)
    std::memcpy(g_snap_data.data() + i * kPage, g_base + g_snap_pages[i], kPage);
  REXLOG_INFO("mk9 rollback: snapshot of {} pages", g_snap_pages.size());
}

void DebugSnapshotPages(const std::vector<uint32_t>& pages) {
  std::lock_guard<std::mutex> lock(g_lock);
  g_snap_pages = pages;
  std::sort(g_snap_pages.begin(), g_snap_pages.end());
  g_snap_pages.erase(std::unique(g_snap_pages.begin(), g_snap_pages.end()), g_snap_pages.end());
  g_snap_data.resize(g_snap_pages.size() * size_t(kPage));
  for (size_t i = 0; i < g_snap_pages.size(); i++)
    std::memcpy(g_snap_data.data() + i * kPage, g_base + g_snap_pages[i], kPage);
}

void DebugComparePages(const char* what) {
  std::lock_guard<std::mutex> lock(g_lock);
  size_t differ = 0, told = 0;
  alignas(8) uint8_t now[kPage], then[kPage];
  for (size_t i = 0; i < g_snap_pages.size(); i++) {
    const uint32_t p = g_snap_pages[i];
    if (g_page[p >> 12] == kForeign) continue;
    // the simulation's own memory only: its heap and the executable's data (the D3D's resources
    // count GPU fences, which a frame run again sees further on)
    if (!(p >= g_sim_region.lo && p < g_sim_region.hi) && !(p >= 0x80000000u && p < 0x90000000u)) continue;
    std::memcpy(now, g_base + p, kPage);
    std::memcpy(then, g_snap_data.data() + i * kPage, kPage);
    ForVolatile(p, [&](uint32_t off, uint32_t n) {
      std::memset(now + off, 0, n);
      std::memset(then + off, 0, n);
    });
    if (!std::memcmp(now, then, kPage)) continue;
    differ++;
    for (uint32_t b = 0; b < kPage && told < 240; b += 16) {
      if (!std::memcmp(now + b, then + b, 16)) continue;
      told++;
      std::string a, c;
      for (uint32_t k = 0; k < 16; k += 4) {
        uint32_t x, y;
        std::memcpy(&x, then + b + k, 4);
        std::memcpy(&y, now + b + k, 4);
        a += fmt::format(" {:08X}", __builtin_bswap32(x));
        c += fmt::format(" {:08X}", __builtin_bswap32(y));
      }
      REXLOG_WARN("mk9 rollback: {} {:08X}:{} ->{}", what, p + b, a, c);
    }
  }
  REXLOG_WARN("mk9 rollback: {}: {} of {} pages differ", what, differ, g_snap_pages.size());
  g_snap_pages.clear();
  g_snap_data.clear();
}

void DebugCompare() {
  std::lock_guard<std::mutex> lock(g_lock);
  static const char* kState[] = {"untracked", "clean", "dirty", "foreign"};
  size_t differ = 0, by_state[4] = {};
  std::unordered_map<uint32_t, size_t> by_region;  // 16 MB regions
  alignas(8) uint8_t now[kPage], then[kPage];
  for (size_t i = 0; i < g_snap_pages.size(); i++) {
    const uint32_t p = g_snap_pages[i];
    std::memcpy(now, g_base + p, kPage);
    std::memcpy(then, g_snap_data.data() + i * kPage, kPage);
    ForVolatile(p, [&](uint32_t off, uint32_t n) {
      std::memset(now + off, 0, n);
      std::memset(then + off, 0, n);
    });
    if (!std::memcmp(now, then, kPage)) continue;
    differ++;
    const uint8_t st = g_page[p >> 12];
    by_state[st & 3]++;
    by_region[p >> 24]++;
    if (differ <= 120) {
      uint32_t first = 0, bytes = 0;
      for (uint32_t b = 0; b < kPage; b++)
        if (now[b] != then[b]) {
          if (!bytes) first = b;
          bytes++;
        }
      uint32_t was = 0, is = 0;
      std::memcpy(&was, then + (first & ~3u), 4);
      std::memcpy(&is, now + (first & ~3u), 4);
      REXLOG_WARN("mk9 rollback: after the restore {:08X} differs ({}{}): {} bytes from +{:03X}, {:08X} was {:08X}", p,
                  kState[st & 3], g_shared[p >> 12] ? ", has volatile bytes" : "", bytes, first,
                  __builtin_bswap32(is), __builtin_bswap32(was));
    }
  }
  std::string regions;
  for (const auto& [r, n] : by_region) regions += fmt::format(" {:02X}xxxxxx:{}", r, n);
  REXLOG_WARN("mk9 rollback: after the restore {} of {} pages differ (clean {}, dirty {}, foreign {}); by region{}",
              differ, g_snap_pages.size(), by_state[1], by_state[2], by_state[3], regions);
  g_snap_pages.clear();
  g_snap_data.clear();
}

Stats GetStats() { return g_stats; }

}  // namespace mk9::rb
