// The Macintosh Memory Manager's application zone.
//
// Classic code distinguishes pointers (fixed blocks) from handles (a pointer to
// a master pointer, so the block can be relocated during compaction). This heap
// allocates from a large guest region and never moves a block, which makes
// HLock, HUnlock, MoveHHi and friends genuine no-ops while staying completely
// compatible: an application may not assume a block *will* move, only that it
// may. Block bookkeeping is kept host-side rather than in guest headers so that
// a stray guest write cannot corrupt the allocator itself.
#pragma once

#include <map>
#include <unordered_map>

#include "mem.h"

namespace cyt {

class Heap {
 public:
  explicit Heap(Mem& mem);

  // Non-relocatable blocks.
  GuestAddr newPtr(u32 size, bool clear);
  bool disposePtr(GuestAddr p);
  u32 ptrSize(GuestAddr p) const;
  bool setPtrSize(GuestAddr p, u32 size);

  // Relocatable blocks. A handle is the address of its master pointer.
  GuestAddr newHandle(u32 size, bool clear);
  bool disposeHandle(GuestAddr h);
  u32 handleSize(GuestAddr h) const;
  bool setHandleSize(GuestAddr h, u32 size);
  bool validHandle(GuestAddr h) const;

  // HGetState / HSetState carry the lock, purge and resource flags.
  u8 handleState(GuestAddr h) const;
  void setHandleState(GuestAddr h, u8 state);

  GuestAddr recoverHandle(GuestAddr blockPtr) const;

  u32 freeBytes() const;
  u32 largestFreeBlock() const;
  u32 totalAllocated() const { return allocated_; }

 private:
  // Rounds up to keep every block four-byte aligned, as the Memory Manager did.
  static u32 round(u32 n) { return (n + 3) & ~3u; }
  GuestAddr allocBlock(u32 size);
  void freeBlock(GuestAddr addr);

  // `size` is the physical block, rounded up and possibly enlarged to swallow
  // a free-list tail too small to track. `logical` is the size the application
  // asked for, and is what GetPtrSize and GetHandleSize must report: an
  // application that reads a resource by its handle's size would otherwise
  // read the padding as data.
  struct Block { u32 size; u32 logical; bool isHandleBlock; GuestAddr handle; };

  Mem& mem_;
  GuestAddr blockBase_ = 0;   // first byte available for block payloads
  GuestAddr bump_ = 0;        // next never-yet-used address
  GuestAddr limit_ = 0;

  std::unordered_map<GuestAddr, Block> blocks_;   // payload address -> block
  std::map<GuestAddr, u32> free_;                 // address -> size, ordered
                                                 // so neighbours can coalesce
  // Master pointers live in their own region, which makes a handle trivially
  // distinguishable from a pointer and keeps them stable forever.
  GuestAddr mpBase_ = 0, mpNext_ = 0, mpLimit_ = 0;
  std::unordered_map<GuestAddr, u8> mpState_;
  std::vector<GuestAddr> mpFree_;

  u32 allocated_ = 0;
};

}  // namespace cyt
