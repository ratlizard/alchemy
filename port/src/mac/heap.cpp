#include "mac/heap.h"

namespace cyt {
namespace {
// One megabyte of master pointers -- 262144 handles, far more than a 1999
// application will ever hold open at once.
constexpr u32 kMasterPointerRegion = 1u << 20;
}  // namespace

Heap::Heap(Mem& mem) : mem_(mem) {
  mpBase_  = layout::kHeapBase;
  mpNext_  = mpBase_;
  mpLimit_ = mpBase_ + kMasterPointerRegion;
  blockBase_ = mpLimit_;
  bump_ = blockBase_;
  limit_ = layout::kHeapEnd;
}

GuestAddr Heap::allocBlock(u32 wanted) {
  u32 size = round(wanted);
  if (size == 0) size = 4;

  // Best fit over the free list keeps fragmentation low without needing a
  // compaction pass, which is what lets blocks stay pinned forever.
  auto best = free_.end();
  for (auto it = free_.begin(); it != free_.end(); ++it) {
    if (it->second >= size &&
        (best == free_.end() || it->second < best->second)) {
      best = it;
      if (it->second == size) break;
    }
  }
  if (best != free_.end()) {
    GuestAddr addr = best->first;
    u32 have = best->second;
    free_.erase(best);
    // Split the tail back into the free list when it is worth tracking.
    if (have >= size + 16) free_[addr + size] = have - size;
    else size = have;
    blocks_[addr] = {size, wanted, false, 0};
    allocated_ += size;
    return addr;
  }

  if (u64(bump_) + size > limit_) return 0;
  GuestAddr addr = bump_;
  bump_ += size;
  blocks_[addr] = {size, wanted, false, 0};
  allocated_ += size;
  return addr;
}

void Heap::freeBlock(GuestAddr addr) {
  auto it = blocks_.find(addr);
  if (it == blocks_.end()) return;
  u32 size = it->second.size;
  allocated_ -= size;
  blocks_.erase(it);

  // Coalesce with the neighbours on either side.
  auto next = free_.lower_bound(addr);
  if (next != free_.end() && next->first == addr + size) {
    size += next->second;
    next = free_.erase(next);
  }
  if (next != free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == addr) {
      prev->second += size;
      return;
    }
  }
  free_[addr] = size;
}

GuestAddr Heap::newPtr(u32 size, bool clear) {
  GuestAddr p = allocBlock(size);
  if (p && clear) mem_.fill(p, 0, blocks_[p].size);
  return p;
}

bool Heap::disposePtr(GuestAddr p) {
  if (!p || blocks_.find(p) == blocks_.end()) return false;
  freeBlock(p);
  return true;
}

u32 Heap::ptrSize(GuestAddr p) const {
  auto it = blocks_.find(p);
  return it == blocks_.end() ? 0 : it->second.logical;
}

bool Heap::setPtrSize(GuestAddr p, u32 size) {
  auto it = blocks_.find(p);
  if (it == blocks_.end()) return false;
  // Growing in place is only possible when the block is the most recent one or
  // is followed by enough free space; otherwise the request must fail, exactly
  // as SetPtrSize does for a non-relocatable block.
  if (round(size) <= it->second.size) { it->second.logical = size; return true; }
  u32 want = round(size), have = it->second.size;
  GuestAddr tail = p + have;
  if (tail == bump_ && u64(bump_) + (want - have) <= limit_) {
    bump_ += want - have;
    allocated_ += want - have;
    it->second.size = want;
    it->second.logical = size;
    return true;
  }
  auto f = free_.find(tail);
  if (f != free_.end() && have + f->second >= want) {
    u32 extra = want - have;
    u32 rest = f->second - extra;
    free_.erase(f);
    if (rest) free_[tail + extra] = rest;
    allocated_ += extra;
    it->second.size = want;
    it->second.logical = size;
    return true;
  }
  return false;
}

GuestAddr Heap::newHandle(u32 size, bool clear) {
  GuestAddr mp;
  if (!mpFree_.empty()) {
    mp = mpFree_.back();
    mpFree_.pop_back();
  } else {
    if (mpNext_ + 4 > mpLimit_) return 0;
    mp = mpNext_;
    mpNext_ += 4;
  }
  GuestAddr blk = allocBlock(size);
  if (!blk) {
    mpFree_.push_back(mp);
    return 0;
  }
  blocks_[blk].isHandleBlock = true;
  blocks_[blk].handle = mp;
  mem_.w32(mp, blk);
  mpState_[mp] = 0;
  if (clear) mem_.fill(blk, 0, blocks_[blk].size);
  return mp;
}

bool Heap::validHandle(GuestAddr h) const {
  return h >= mpBase_ && h < mpNext_ && (h & 3) == 0 &&
         mpState_.find(h) != mpState_.end();
}

bool Heap::disposeHandle(GuestAddr h) {
  if (!validHandle(h)) return false;
  GuestAddr blk = mem_.r32(h);
  if (blk) freeBlock(blk);
  mem_.w32(h, 0);
  mpState_.erase(h);
  mpFree_.push_back(h);
  return true;
}

u32 Heap::handleSize(GuestAddr h) const {
  if (!validHandle(h)) return 0;
  return ptrSize(mem_.r32(h));
}

bool Heap::setHandleSize(GuestAddr h, u32 size) {
  if (!validHandle(h)) return false;
  GuestAddr blk = mem_.r32(h);
  if (!blk) return false;
  if (setPtrSize(blk, size)) return true;
  // A relocatable block may move, so fall back to allocate-copy-release. This
  // is why handles must be dereferenced through the master pointer every time.
  u32 old = ptrSize(blk);
  GuestAddr fresh = allocBlock(size);
  if (!fresh) return false;
  blocks_[fresh].isHandleBlock = true;
  blocks_[fresh].handle = h;
  mem_.move(fresh, blk, old < size ? old : size);
  if (size > old) mem_.fill(fresh + old, 0, size - old);
  freeBlock(blk);
  mem_.w32(h, fresh);
  return true;
}

u8 Heap::handleState(GuestAddr h) const {
  auto it = mpState_.find(h);
  return it == mpState_.end() ? 0 : it->second;
}

void Heap::setHandleState(GuestAddr h, u8 state) {
  if (validHandle(h)) mpState_[h] = state;
}

GuestAddr Heap::recoverHandle(GuestAddr blockPtr) const {
  auto it = blocks_.find(blockPtr);
  if (it == blocks_.end() || !it->second.isHandleBlock) return 0;
  return it->second.handle;
}

u32 Heap::freeBytes() const {
  u64 unused = u64(limit_) - bump_;
  for (const auto& [addr, size] : free_) unused += size;
  return u32(unused > 0xFFFFFFFFull ? 0xFFFFFFFFull : unused);
}

u32 Heap::largestFreeBlock() const {
  u64 best = u64(limit_) - bump_;
  for (const auto& [addr, size] : free_) best = size > best ? size : best;
  return u32(best > 0xFFFFFFFFull ? 0xFFFFFFFFull : best);
}

}  // namespace cyt
