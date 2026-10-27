// Diagnostics, see diag.h.

#include "mk9/diag.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/object_table.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <windows.h>

namespace mk9::diag {
namespace {

uint64_t g_threads_at = 0;
uint64_t g_heaps_at = 0;
std::set<uint64_t> g_dump_at;

// committed guest regions, in address order
template <typename F>
void ForEachCommitted(F&& f) {
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return;
  auto* mem = ks->memory();
  uint32_t addr = 0x00010000;
  while (true) {
    auto* heap = mem->LookupHeap(addr);
    if (!heap) {
      if (addr >= 0xFFFF0000) break;
      addr += 0x10000;
      continue;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(addr, &info) || info.region_size == 0) {
      addr += heap->page_size();
      continue;
    }
    if (info.state & 1) f(info);
    uint64_t next = uint64_t(info.base_address) + info.region_size;
    if (next > 0xFFFFFFFFull) break;
    addr = uint32_t(next);
  }
}

uint64_t EnvFrame(const char* name) {
  const char* v = std::getenv(name);
  return v && *v ? std::strtoull(v, nullptr, 10) : 0;
}

}  // namespace

void Init() {
  g_threads_at = EnvFrame("MK9_DIAG_THREADS");
  g_heaps_at = EnvFrame("MK9_DIAG_HEAPS");
  if (const char* d = std::getenv("MK9_DIAG_DUMP"); d && *d) {
    std::stringstream ss(d);
    std::string item;
    while (std::getline(ss, item, ',')) g_dump_at.insert(std::strtoull(item.c_str(), nullptr, 10));
  }
}

void OnFrame(uint64_t frame) {
  if (g_threads_at && frame == g_threads_at) LogThreads();
  if (g_heaps_at && frame == g_heaps_at) LogHeaps();
  if (!g_dump_at.empty() && g_dump_at.count(frame)) {
    const char* dir = std::getenv("MK9_SNAP_DIR");
    std::filesystem::path p = dir && *dir ? std::filesystem::path(dir) : std::filesystem::current_path();
    DumpMemory((p / ("dump-" + std::to_string(frame) + ".bin")).string());
  }
}

bool DumpMemory(const std::string& path) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  auto* mem = REX_KERNEL_STATE()->memory();
  uint64_t total = 0;
  ForEachCommitted([&](const rex::memory::HeapAllocationInfo& info) {
    // each physical heap (0xA0000000 64 KB pages, 0xC0000000 16 MB, 0xE0000000 4 KB) holds its
    // own allocations: all of them are written; pages without read access (stack guards) not
    if (!(info.protect & rex::memory::kMemoryProtectRead)) return;
    // and only what the host can read (guard pages, regions committed in the guest's books only)
    uint32_t a = info.base_address, end = info.base_address + info.region_size;
    while (a < end) {
      MEMORY_BASIC_INFORMATION mbi{};
      uint8_t* host = mem->TranslateVirtual<uint8_t*>(a);
      if (!VirtualQuery(host, &mbi, sizeof mbi)) break;
      uint64_t span = std::min<uint64_t>(uint64_t(end) - a,
                                         uint64_t(static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - host));
      const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
      if (mbi.State == MEM_COMMIT && (mbi.Protect & readable) && !(mbi.Protect & PAGE_GUARD)) {
        uint32_t hdr[2] = {a, uint32_t(span)};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(host, 1, size_t(span), f);
        total += span;
      }
      a += uint32_t(span);
    }
  });
  std::fclose(f);
  REXLOG_INFO("mk9: dumped {} MB of guest memory to {}", total >> 20, path);
  return true;
}

void LogThreads() {
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return;
  auto threads = ks->object_table()->GetObjectsByType<rex::system::XThread>();
  REXLOG_INFO("mk9: {} threads", threads.size());
  for (auto& t : threads) {
    const auto* cp = t->creation_params();
    auto* host = t->thread();
    REXLOG_INFO(
        "mk9:   thread {:08X} id {:4} {}{} start {:08X} ctx {:08X} stack {:6X} prio {:3} cpu {} "
        "host {} '{}'",
        t->handle(), t->thread_id(), t->is_guest_thread() ? "guest" : "host ",
        t->main_thread() ? " main" : "     ", cp ? cp->start_address : 0,
        cp ? cp->start_context : 0, cp ? cp->stack_size : 0, t->priority(), t->active_cpu(),
        host ? host->system_id() : 0, host ? host->name() : std::string());
  }
}

void LogHeaps() {
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return;
  auto* mem = ks->memory();
  // committed regions of every heap: what the game allocated where
  uint64_t total = 0;
  uint32_t addr = 0x00010000;
  while (true) {
    auto* heap = mem->LookupHeap(addr);
    if (!heap) {
      if (addr >= 0xFFFF0000) break;
      addr += 0x10000;
      continue;
    }
    rex::memory::HeapAllocationInfo info{};
    if (!heap->QueryRegionInfo(addr, &info) || info.region_size == 0) {
      addr += heap->page_size();
      continue;
    }
    if (info.state & 1) {  // committed
      REXLOG_INFO("mk9:   region {:08X}-{:08X} {:8} KB prot {:X} alloc base {:08X}",
                  info.base_address, info.base_address + info.region_size,
                  info.region_size / 1024, info.protect, info.allocation_base);
      total += info.region_size;
    }
    uint64_t next = uint64_t(info.base_address) + info.region_size;
    if (next > 0xFFFFFFFFull) break;
    addr = uint32_t(next);
  }
  REXLOG_INFO("mk9: {} MB committed in guest memory", total >> 20);
}

}  // namespace mk9::diag
