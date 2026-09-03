#include "mac/qd_surface.h"

namespace cyt {

Palette readPalette(Mem& m, GuestAddr ctabHandle) {
  Palette p;
  if (!ctabHandle) return p;
  GuestAddr t = m.r32(ctabHandle);
  if (!t) return p;
  const u16 size = m.r16(t + qd::kCtSize);
  const u16 flags = m.r16(t + qd::kCtFlags);
  const u32 count = u32(size) + 1;
  if (count == 0 || count > 256) return p;
  p.entries.assign(256, Rgb{});
  for (u32 i = 0; i < count; ++i) {
    GuestAddr e = t + qd::kCtTable + i * qd::kColorSpecSize;
    // In a device colour table the `value` field is ignored and the entry's
    // position is its index; elsewhere `value` names the index explicitly.
    u32 idx = (flags & 0x8000) ? i : m.r16(e + 0);
    idx &= 0xFF;
    p.entries[idx] = Rgb{m.r16(e + 2), m.r16(e + 4), m.r16(e + 6)};
  }
  return p;
}

u8 nearestIndex(const Palette& p, Rgb c) {
  if (p.empty()) return 0;
  u64 best = ~0ull;
  u8 bestIdx = 0;
  for (size_t i = 0; i < p.entries.size(); ++i) {
    const Rgb& e = p.entries[i];
    // Squared distance on the 16-bit channels, narrowed to 8 bits first so the
    // arithmetic stays small and the result matches what an 8-bit screen shows.
    s32 dr = s32(e.r >> 8) - s32(c.r >> 8);
    s32 dg = s32(e.g >> 8) - s32(c.g >> 8);
    s32 db = s32(e.b >> 8) - s32(c.b >> 8);
    u64 d = u64(dr * dr + dg * dg + db * db);
    if (d < best) {
      best = d;
      bestIdx = u8(i);
      if (d == 0) break;
    }
  }
  return bestIdx;
}

ColorMap::ColorMap(const Palette& from, const Palette& to) {
  if (from.empty() || to.empty()) return;
  map_.resize(from.entries.size());
  identity_ = true;
  for (size_t i = 0; i < from.entries.size(); ++i) {
    const Rgb& c = from.entries[i];
    // An exact hit at the same index is overwhelmingly the common case, and
    // checking it first turns most blits into a straight byte copy.
    if (i < to.entries.size() && to.entries[i].r == c.r &&
        to.entries[i].g == c.g && to.entries[i].b == c.b) {
      map_[i] = u8(i);
      continue;
    }
    map_[i] = nearestIndex(to, c);
    if (map_[i] != u8(i)) identity_ = false;
  }
}

Rect readRect(Mem& m, GuestAddr p) {
  Rect r;
  r.top = s16(m.r16(p + 0));
  r.left = s16(m.r16(p + 2));
  r.bottom = s16(m.r16(p + 4));
  r.right = s16(m.r16(p + 6));
  return r;
}

void writeRect(Mem& m, GuestAddr p, const Rect& r) {
  m.w16(p + 0, u16(r.top));
  m.w16(p + 2, u16(r.left));
  m.w16(p + 4, u16(r.bottom));
  m.w16(p + 6, u16(r.right));
}

Rect intersect(const Rect& a, const Rect& b) {
  Rect r;
  r.top = a.top > b.top ? a.top : b.top;
  r.left = a.left > b.left ? a.left : b.left;
  r.bottom = a.bottom < b.bottom ? a.bottom : b.bottom;
  r.right = a.right < b.right ? a.right : b.right;
  if (r.empty()) r = Rect{};
  return r;
}

PixSurface PixSurface::fromHandle(Mem& m, GuestAddr pixMapHandle) {
  PixSurface s;
  if (!pixMapHandle) return s;
  GuestAddr pm = m.r32(pixMapHandle);
  if (!pm) return s;
  s.base_ = m.r32(pm + qd::kPmBaseAddr);
  const u16 rb = m.r16(pm + qd::kPmRowBytes);
  s.rowBytes_ = rb & 0x3FFF;
  s.bounds_ = readRect(m, pm + qd::kPmBounds);
  s.depth_ = (rb & 0x8000) ? m.r16(pm + qd::kPmPixelSize) : 1;
  s.ctab_ = (rb & 0x8000) ? m.r32(pm + qd::kPmTable) : 0;
  return s;
}

PixSurface PixSurface::fromPort(Mem& m, GuestAddr port) {
  PixSurface s;
  if (!port) return s;
  // A CGrafPort is marked by the top two bits of portVersion; in that form the
  // field at offset 2 is a PixMapHandle rather than an inline BitMap.
  const u16 version = m.r16(port + qd::kPortVersion);
  if (version & 0xC000) return fromHandle(m, m.r32(port + qd::kPortPixMap));
  // Old-style GrafPort: an inline BitMap at offset 2.
  s.base_ = m.r32(port + 2);
  s.rowBytes_ = m.r16(port + 6) & 0x3FFF;
  s.bounds_ = readRect(m, port + 8);
  s.depth_ = 1;
  return s;
}

}  // namespace cyt
