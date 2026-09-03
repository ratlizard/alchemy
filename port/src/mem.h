// Guest address space for the emulated 32-bit big-endian PowerPC Macintosh.
//
// The whole guest is one lazily-committed mmap reservation, so a guest address
// is just an offset into it and translation is a single add. Classic Mac code
// reads low-memory globals at fixed absolute addresses (Ticks at 0x16A and so
// on), so page zero is real, addressable memory rather than a trap page --
// dereferencing guest NULL is caught in the Toolbox shims, where a null handle
// or pointer is a meaningful and checkable condition, not down here.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cyt {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// A guest (emulated) address. Kept distinct from host pointers in signatures
// so that mixing the two is a compile error rather than a silent corruption.
using GuestAddr = u32;

// Fixed layout of the guest address space. The PEF's sections declare a
// default address of 0, meaning "loader's choice", so the loader places them
// here; everything is page-aligned and generously spaced so a section growing
// never collides with the heap.
namespace layout {
constexpr GuestAddr kLowMemBase   = 0x00000000;  // Mac low-memory globals
constexpr GuestAddr kLowMemEnd    = 0x00002000;
constexpr GuestAddr kCodeBase     = 0x00100000;  // PEF code section
constexpr GuestAddr kDataBase     = 0x00800000;  // PEF data section(s)
constexpr GuestAddr kHeapBase     = 0x01000000;  // Memory Manager application zone
constexpr GuestAddr kHeapEnd      = 0x0E000000;
constexpr GuestAddr kStackTop     = 0x0EF00000;  // grows down
constexpr GuestAddr kStackLimit   = 0x0E100000;
constexpr GuestAddr kShimBase     = 0x0F000000;  // native-call trampoline space
constexpr GuestAddr kShimEnd      = 0x0F100000;
constexpr u64       kSize         = 0x10000000;  // 256 MiB total
}  // namespace layout

// The guest address space. One instance exists per process; Mem::get() is the
// access point for the interpreter and the Toolbox shims alike.
class Mem {
 public:
  static Mem& get();

  bool init(std::string* err);
  void shutdown();

  // True when [addr, addr+len) lies entirely inside the guest.
  bool valid(GuestAddr addr, u64 len) const {
    return u64(addr) + len <= layout::kSize;
  }

  // Host pointer for a guest address. Returns nullptr when out of range so
  // callers can report a fault instead of walking off the reservation.
  void* host(GuestAddr addr, u64 len = 1) {
    return valid(addr, len) ? base_ + addr : nullptr;
  }
  const void* host(GuestAddr addr, u64 len = 1) const {
    return valid(addr, len) ? base_ + addr : nullptr;
  }

  // Big-endian scalar access. Out-of-range reads yield zero and out-of-range
  // writes are dropped, both recorded in fault_count() -- the interpreter
  // checks that counter at instruction granularity so a stray access is
  // reported with its PC rather than crashing the host.
  u8  r8 (GuestAddr a)       { return chk(a, 1) ? base_[a] : 0; }
  u16 r16(GuestAddr a)       { return chk(a, 2) ? __builtin_bswap16(ld<u16>(a)) : 0; }
  u32 r32(GuestAddr a)       { return chk(a, 4) ? __builtin_bswap32(ld<u32>(a)) : 0; }
  u64 r64(GuestAddr a)       { return chk(a, 8) ? __builtin_bswap64(ld<u64>(a)) : 0; }

  void w8 (GuestAddr a, u8  v) { if (chk(a, 1)) base_[a] = v; }
  void w16(GuestAddr a, u16 v) { if (chk(a, 2)) st<u16>(a, __builtin_bswap16(v)); }
  void w32(GuestAddr a, u32 v) { if (chk(a, 4)) st<u32>(a, __builtin_bswap32(v)); }
  void w64(GuestAddr a, u64 v) { if (chk(a, 8)) st<u64>(a, __builtin_bswap64(v)); }

  s8  rs8 (GuestAddr a) { return s8 (r8 (a)); }
  s16 rs16(GuestAddr a) { return s16(r16(a)); }
  s32 rs32(GuestAddr a) { return s32(r32(a)); }

  // Doubles are stored big-endian like everything else.
  double rf64(GuestAddr a) { u64 b = r64(a); double d; std::memcpy(&d, &b, 8); return d; }
  void   wf64(GuestAddr a, double d) { u64 b; std::memcpy(&b, &d, 8); w64(a, b); }
  float  rf32(GuestAddr a) { u32 b = r32(a); float f; std::memcpy(&f, &b, 4); return f; }
  void   wf32(GuestAddr a, float f) { u32 b; std::memcpy(&b, &f, 4); w32(a, b); }

  void copyIn(GuestAddr dst, const void* src, u64 n) {
    if (chk(dst, n)) std::memcpy(base_ + dst, src, n);
  }
  void copyOut(void* dst, GuestAddr src, u64 n) {
    if (chk(src, n)) std::memcpy(dst, base_ + src, n);
    else std::memset(dst, 0, n);
  }
  void move(GuestAddr dst, GuestAddr src, u64 n) {
    if (chk(dst, n) && chk(src, n)) std::memmove(base_ + dst, base_ + src, n);
  }
  void fill(GuestAddr dst, u8 v, u64 n) {
    if (chk(dst, n)) std::memset(base_ + dst, v, n);
  }

  // Pascal strings (length byte first) are the Toolbox's native string form;
  // C strings appear in the few places CFM uses them (library names, symbols).
  std::string pstr(GuestAddr a);
  void writePstr(GuestAddr a, const std::string& s, u64 cap);
  std::string cstr(GuestAddr a, u64 maxLen = 4096);

  u64 faultCount() const { return faults_; }
  GuestAddr lastFault() const { return lastFault_; }
  void clearFaults() { faults_ = 0; lastFault_ = 0; }

 private:
  template <typename T> T ld(GuestAddr a) const {
    T v; std::memcpy(&v, base_ + a, sizeof(T)); return v;
  }
  template <typename T> void st(GuestAddr a, T v) {
    std::memcpy(base_ + a, &v, sizeof(T));
  }
  bool chk(GuestAddr a, u64 len) {
    if (valid(a, len)) return true;
    ++faults_;
    lastFault_ = a;
    // Out-of-range accesses are counted rather than fatal, because a classic
    // application reads a few bytes it does not own here and there. Set
    // CYT_DEBUG_MEM to see the first of them: interleaved with a call trace,
    // the neighbouring Toolbox calls say which structure was misread.
    static const bool debug = std::getenv("CYT_DEBUG_MEM") != nullptr;
    if (debug && faults_ <= 40)
      std::fprintf(stderr, "  [mem] out of range: 0x%08x (%llu bytes)\n", a,
                   (unsigned long long)len);
    return false;
  }

  u8* base_ = nullptr;
  u64 faults_ = 0;
  GuestAddr lastFault_ = 0;
};

}  // namespace cyt
