// The simulation and audio heaps, see simheap.h.
//
// Region layout: a 4 KB control block (the size-class bitmaps and list heads), then blocks, then a
// 16-byte end marker. A block at offset b (16-aligned, from the region's base):
//   +0  u32 prev     offset of the block before it (0: none)
//   +4  u32 size     the block's bytes, header included (a multiple of 16) | kFree | kPrevFree
//   +8  u32 next     free blocks: the next one in its list (0: none)
//   +12 u32 back     free blocks: the one before it in its list
//   +16 the payload (16-aligned)
// Offsets are the region's own, kept in host byte order: only this file reads them.

#include "mk9/simheap.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace mk9::simheap {
namespace {

constexpr uint32_t kFree = 1, kPrevFree = 2;
constexpr uint32_t kHeader = 16, kMinBlock = 32, kControl = 4096;
constexpr int kSlLog2 = 5, kSlCount = 1 << kSlLog2;   // 32 second-level lists per first level
constexpr uint32_t kSmall = kSlCount * 16;             // below 512 bytes: one list per 16 bytes
constexpr int kSmallLog2 = 9;
constexpr int kFlCount = 22;                           // up to 1 GB blocks

int Log2(uint32_t v) { return 31 - __builtin_clz(v); }

void Mapping(uint32_t size, int& fl, int& sl) {
  if (size < kSmall) {
    fl = 0;
    sl = int(size / 16);
  } else {
    const int l = Log2(size);
    fl = l - kSmallLog2 + 1;
    sl = int((size >> (l - kSlLog2)) - kSlCount);
  }
}

// the class to look in so any block found fits `size`
void MappingSearch(uint32_t size, int& fl, int& sl) {
  if (size >= kSmall) size += (1u << (Log2(size) - kSlLog2)) - 1;
  Mapping(size, fl, sl);
}

}  // namespace

struct Heap::Control {
  uint32_t fl_bitmap;
  uint32_t sl_bitmap[kFlCount];
  uint32_t heads[kFlCount][kSlCount];
  uint32_t end;  // the end marker's offset
};

Heap::Heap(const char* name, const char* size_var, uint32_t default_mb)
    : name_(name), size_var_(size_var), default_mb_(default_mb) {}

void Heap::Insert(uint32_t b) {
  int fl, sl;
  Mapping(SizeOf(b), fl, sl);
  Control& c = Ctl();
  const uint32_t head = c.heads[fl][sl];
  U32(b + 8) = head;
  U32(b + 12) = 0;
  if (head) U32(head + 12) = b;
  c.heads[fl][sl] = b;
  c.fl_bitmap |= 1u << fl;
  c.sl_bitmap[fl] |= 1u << sl;
}

void Heap::Remove(uint32_t b) {
  int fl, sl;
  Mapping(SizeOf(b), fl, sl);
  Control& c = Ctl();
  const uint32_t next = U32(b + 8), back = U32(b + 12);
  if (next) U32(next + 12) = back;
  if (back) U32(back + 8) = next;
  else {
    c.heads[fl][sl] = next;
    if (!next) {
      c.sl_bitmap[fl] &= ~(1u << sl);
      if (!c.sl_bitmap[fl]) c.fl_bitmap &= ~(1u << fl);
    }
  }
}

// a free block of at least `size` bytes, out of its list; 0 when there is none
uint32_t Heap::Find(uint32_t size) {
  int fl, sl;
  MappingSearch(size, fl, sl);
  if (fl >= kFlCount) return 0;
  Control& c = Ctl();
  uint32_t sl_map = c.sl_bitmap[fl] & (~0u << sl);
  if (!sl_map) {
    const uint32_t fl_map = fl + 1 < 32 ? c.fl_bitmap & (~0u << (fl + 1)) : 0;
    if (!fl_map) return 0;
    fl = __builtin_ctz(fl_map);
    sl_map = c.sl_bitmap[fl];
  }
  sl = __builtin_ctz(sl_map);
  const uint32_t b = c.heads[fl][sl];
  if (b) Remove(b);
  return b;
}

// a free block b (out of the lists) made used, with what is left over as a free block of its own
void Heap::Use(uint32_t b, uint32_t size) {
  const uint32_t have = SizeOf(b);
  const uint32_t prev_flag = FlagsOf(b) & kPrevFree;
  if (have - size >= kMinBlock) {
    SetSize(b, size, prev_flag);
    const uint32_t rest = b + size;
    U32(rest) = b;
    SetSize(rest, have - size, kFree);
    U32(Next(rest)) = rest;
    U32(Next(rest) + 4) |= kPrevFree;
    Insert(rest);
  } else {
    SetSize(b, have, prev_flag);
    U32(Next(b) + 4) &= ~kPrevFree;
  }
}

// b made free and merged with free neighbours, then listed
void Heap::Release(uint32_t b) {
  uint32_t size = SizeOf(b);
  uint32_t flags = FlagsOf(b) & kPrevFree;
  const uint32_t next = b + size;
  if (FlagsOf(next) & kFree) {
    Remove(next);
    size += SizeOf(next);
  }
  if (flags & kPrevFree) {
    const uint32_t prev = U32(b);
    Remove(prev);
    size += SizeOf(prev);
    flags = FlagsOf(prev) & kPrevFree;
    b = prev;
  }
  SetSize(b, size, flags | kFree);
  const uint32_t after = b + size;
  U32(after) = b;
  U32(after + 4) |= kPrevFree;
  Insert(b);
}

bool Heap::Reserve() {
  static_assert(sizeof(Control) <= kControl);
  tried_ = true;
  auto* ks = REX_KERNEL_STATE();
  if (!ks) return false;
  auto* mem = ks->memory();
  auto* heap = mem->LookupHeap(0x40000000);
  if (!heap) return false;
  uint32_t mb = default_mb_;
  if (const char* v = std::getenv(size_var_); v && *v) mb = uint32_t(std::clamp(std::atoi(v), 16, 1024));
  uint32_t addr = 0;
  for (; mb >= 16; mb /= 2) {
    if (heap->Alloc(mb << 20, 64 * 1024, rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
                    rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite, true, &addr))
      break;
  }
  if (!addr) {
    REXLOG_ERROR("mk9 {} heap: no room for it", name_);
    return false;
  }
  base_ = addr;
  size_ = mb << 20;
  host_ = mem->TranslateVirtual<uint8_t*>(addr);
  std::memset(host_, 0, kControl);
  Control& c = Ctl();
  c.end = size_ - kHeader;
  // one free block over everything, then the end marker (used, size 0: no merging past it)
  const uint32_t first = kControl;
  U32(first) = 0;
  SetSize(first, c.end - first, kFree);
  U32(c.end) = first;
  SetSize(c.end, 0, kPrevFree);
  Insert(first);
  REXLOG_INFO("mk9 {} heap: {} MB at {:08X}", name_, mb, addr);
  return true;
}

uint32_t Heap::AllocLocked(uint32_t size, uint32_t alignment) {
  if (!ReadyLocked()) return 0;
  if (size == 0) size = 1;
  if (size > 0x3FF00000u) return 0;
  const uint32_t need = std::max(kMinBlock, ((size + 15) & ~15u) + kHeader);
  alignment = std::max<uint32_t>(alignment, 16);
  if ((alignment & (alignment - 1)) || alignment > (1u << 20)) alignment = 16;
  if (alignment == 16) {
    const uint32_t b = Find(need);
    if (!b) return 0;
    Use(b, need);
    stats_.in_use += SizeOf(b);
    stats_.blocks++;
    stats_.peak = std::max(stats_.peak, stats_.in_use);
    return base_ + b + kHeader;
  }
  // aligned: room for the alignment and a free block before it
  const uint32_t b = Find(need + alignment + kMinBlock);
  if (!b) return 0;
  uint32_t payload = (base_ + b + kHeader + alignment - 1) & ~(alignment - 1);
  if (payload - base_ - kHeader - b != 0 && payload - base_ - kHeader - b < kMinBlock) payload += alignment;
  const uint32_t at = payload - base_ - kHeader;
  if (at != b) {
    // the gap before becomes a free block of its own
    const uint32_t total = SizeOf(b), prev_flag = FlagsOf(b) & kPrevFree;
    SetSize(b, at - b, prev_flag | kFree);
    U32(at) = b;
    SetSize(at, total - (at - b), kPrevFree);
    U32(Next(at)) = at;
    Insert(b);
  }
  Use(at, need);
  stats_.in_use += SizeOf(at);
  stats_.blocks++;
  stats_.peak = std::max(stats_.peak, stats_.in_use);
  return payload;
}

bool Heap::FreeLocked(uint32_t ptr) {
  const uint32_t b = ptr - base_ - kHeader;
  if (FlagsOf(b) & kFree) {
    REXLOG_ERROR("mk9 {} heap: {:08X} freed twice", name_, ptr);
    return false;
  }
  stats_.in_use -= SizeOf(b);
  stats_.blocks--;
  Release(b);
  return true;
}

bool Heap::Ready() {
  std::lock_guard lock(lock_);
  return ReadyLocked();
}

uint32_t Heap::Malloc(uint32_t size, uint32_t alignment) {
  std::lock_guard lock(lock_);
  const uint32_t p = AllocLocked(size, alignment);
  if (!p) stats_.failures++;
  return p;
}

uint32_t Heap::UsableSize(uint32_t ptr) {
  if (!Owns(ptr)) return 0;
  std::lock_guard lock(lock_);
  return SizeOf(ptr - base_ - kHeader) - kHeader;
}

bool Heap::BlockOf(uint32_t a, uint32_t& payload, uint32_t& size) {
  if (!Owns(a)) return false;
  std::lock_guard lock(lock_);
  const uint32_t off = a - base_, end = Ctl().end;
  for (uint32_t b = kControl; b < end;) {
    const uint32_t n = SizeOf(b);
    if (!n) return false;
    if (off < b + n) {
      if ((FlagsOf(b) & kFree) || off < b + kHeader) return false;
      payload = base_ + b + kHeader;
      size = n - kHeader;
      return true;
    }
    b += n;
  }
  return false;
}

bool Heap::Free(uint32_t ptr) {
  if (!Owns(ptr)) return true;
  std::lock_guard lock(lock_);
  return FreeLocked(ptr);
}

uint32_t Heap::Realloc(uint32_t ptr, uint32_t size, uint32_t alignment) {
  std::lock_guard lock(lock_);
  if (!ptr) return AllocLocked(size, alignment);
  const uint32_t have = SizeOf(ptr - base_ - kHeader) - kHeader;
  if (size <= have && (ptr & (std::max<uint32_t>(alignment, 16) - 1)) == 0) return ptr;
  const uint32_t p = AllocLocked(size, alignment);
  if (!p) {
    stats_.failures++;
    return 0;
  }
  std::memcpy(host_ + (p - base_), host_ + (ptr - base_), std::min(have, size));
  FreeLocked(ptr);
  return p;
}

Stats Heap::GetStats() {
  std::lock_guard lock(lock_);
  return stats_;
}

Heap& Sim() {
  static Heap heap("simulation", "MK9_SIMHEAP_MB", 384);
  return heap;
}

Heap& Audio() {
  static Heap heap("audio", "MK9_AUDIOHEAP_MB", 64);
  return heap;
}

}  // namespace mk9::simheap
