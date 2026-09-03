#include "mem.h"

#include <sys/mman.h>

namespace cyt {

Mem& Mem::get() {
  static Mem instance;
  return instance;
}

bool Mem::init(std::string* err) {
  if (base_) return true;
  // MAP_NORESERVE keeps the 256 MiB reservation from counting against commit;
  // pages materialise zero-filled as the guest touches them, so a game that
  // uses a few megabytes costs a few megabytes of RSS.
  void* p = ::mmap(nullptr, layout::kSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) {
    if (err) *err = "could not reserve the 256 MiB guest address space";
    return false;
  }
  base_ = static_cast<u8*>(p);
  return true;
}

void Mem::shutdown() {
  if (base_) {
    ::munmap(base_, layout::kSize);
    base_ = nullptr;
  }
}

std::string Mem::pstr(GuestAddr a) {
  u8 n = r8(a);
  std::string s(n, '\0');
  for (u8 i = 0; i < n; ++i) s[i] = char(r8(a + 1 + i));
  return s;
}

void Mem::writePstr(GuestAddr a, const std::string& s, u64 cap) {
  // cap counts the length byte, matching Str255 and friends.
  u64 n = s.size();
  if (cap > 0 && n > cap - 1) n = cap - 1;
  if (n > 255) n = 255;
  w8(a, u8(n));
  for (u64 i = 0; i < n; ++i) w8(a + 1 + i, u8(s[i]));
}

std::string Mem::cstr(GuestAddr a, u64 maxLen) {
  std::string s;
  for (u64 i = 0; i < maxLen; ++i) {
    u8 c = r8(a + i);
    if (!c) break;
    s.push_back(char(c));
  }
  return s;
}

}  // namespace cyt
