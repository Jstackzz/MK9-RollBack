#pragma once
// Guest heaps of their own for GMalloc's allocations (mk9/gmalloc.cpp routes them here).
//
// The simulation heap: what the simulation's threads (the main thread, the rendering thread, the job
// workers) allocate. The engine's general heap mixes every thread's objects on the same pages, which
// page-level rollback cannot take apart; here only the simulation's threads write, and the heap's
// own bookkeeping lives in the region too, so a restore (mk9/net/rollback.h) puts objects and
// bookkeeping back together.
//
// The audio heap: what FMOD allocates, from any thread. FMOD's own threads mix sound on while the
// simulation goes back and forth, so rollback never tracks this region at all.
//
// A two-level segregated fit allocator (TLSF): blocks of multiples of 16 bytes with a 16-byte
// header, free blocks in 22 x 32 size-class lists, neighbours merged on free. Thread-safe (a host
// lock: nothing of it is in guest memory but the region).
#include <cstdint>
#include <mutex>

namespace mk9::simheap {

struct Stats {
  uint64_t in_use = 0, peak = 0, blocks = 0, failures = 0;
};

class Heap {
 public:
  // size_var: the environment variable that sets its size in MB
  Heap(const char* name, const char* size_var, uint32_t default_mb);

  // reserves the region on first use (from the 0x40000000 heap, from the top); false when it could not
  bool Ready();
  bool Owns(uint32_t guest_address) const { return host_ && guest_address - base_ < size_; }
  uint32_t RegionBase() const { return base_; }
  uint32_t RegionSize() const { return size_; }

  // 0 when it has no room (the caller falls back on the engine's heap)
  uint32_t Malloc(uint32_t size, uint32_t alignment);
  // the block's usable size (at least what was asked)
  uint32_t UsableSize(uint32_t guest_address);
  // the block in use that holds guest_address (its payload and usable size); false when none does.
  // Walks the blocks: for the odd lookup only
  bool BlockOf(uint32_t guest_address, uint32_t& payload, uint32_t& size);
  // false: it was not in use (freed twice)
  bool Free(uint32_t guest_address);
  // in place when it fits, else a new block with the old contents (0: no room, the old block stays)
  uint32_t Realloc(uint32_t guest_address, uint32_t size, uint32_t alignment);

  Stats GetStats();

 private:
  struct Control;
  uint32_t& U32(uint32_t off) { return *reinterpret_cast<uint32_t*>(host_ + off); }
  Control& Ctl() { return *reinterpret_cast<Control*>(host_); }
  uint32_t SizeOf(uint32_t b) { return U32(b + 4) & ~15u; }
  uint32_t FlagsOf(uint32_t b) { return U32(b + 4) & 15u; }
  void SetSize(uint32_t b, uint32_t size, uint32_t flags) { U32(b + 4) = size | flags; }
  uint32_t Next(uint32_t b) { return b + SizeOf(b); }
  void Insert(uint32_t b);
  void Remove(uint32_t b);
  uint32_t Find(uint32_t size);
  void Use(uint32_t b, uint32_t size);
  void Release(uint32_t b);
  bool Reserve();
  bool ReadyLocked() { return host_ || (!tried_ && Reserve()); }
  uint32_t AllocLocked(uint32_t size, uint32_t alignment);
  bool FreeLocked(uint32_t ptr);

  const char* name_;
  const char* size_var_;
  uint32_t default_mb_;
  std::mutex lock_;
  bool tried_ = false;
  uint32_t base_ = 0, size_ = 0;
  uint8_t* host_ = nullptr;
  Stats stats_;
};

Heap& Sim();
Heap& Audio();

}  // namespace mk9::simheap
