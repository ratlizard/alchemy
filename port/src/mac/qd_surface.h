// A drawable surface: a guest PixMap plus the operations drawing needs.
//
// Everything QuickDraw draws into is described by a PixMap in guest memory --
// the screen, an offscreen GWorld, a picture being decoded. Rather than copy
// pixels into host structures, this wraps the guest PixMap in place so that the
// game observes writes exactly where it expects them.
#pragma once

#include <vector>

#include "mac/quickdraw.h"
#include "mem.h"

namespace cyt {

// An RGB triple at QuickDraw's 16-bits-per-channel precision.
struct Rgb {
  u16 r = 0, g = 0, b = 0;
};

// A colour table read out of guest memory into host form, so that per-pixel
// lookups during a blit do not each cost several guest reads.
struct Palette {
  std::vector<Rgb> entries;
  bool empty() const { return entries.empty(); }
};

Palette readPalette(Mem& m, GuestAddr ctabHandle);

// Maps a source palette onto a destination palette. When the two agree the
// mapping is the identity, which is the common case once the game has installed
// its own palette on the screen.
class ColorMap {
 public:
  ColorMap() = default;
  ColorMap(const Palette& from, const Palette& to);

  u8 operator[](u8 index) const {
    return index < map_.size() ? map_[index] : index;
  }
  bool identity() const { return identity_; }

 private:
  std::vector<u8> map_;
  bool identity_ = true;
};

// Nearest entry in `p` to a colour, by squared distance in RGB. Used both for
// colour matching during a blit and for resolving an RGBColor to an index.
u8 nearestIndex(const Palette& p, Rgb c);

struct Rect {
  s16 top = 0, left = 0, bottom = 0, right = 0;
  s16 width() const { return s16(right - left); }
  s16 height() const { return s16(bottom - top); }
  bool empty() const { return right <= left || bottom <= top; }
};

Rect readRect(Mem& m, GuestAddr p);
void writeRect(Mem& m, GuestAddr p, const Rect& r);
Rect intersect(const Rect& a, const Rect& b);

// An indexed drawing surface backed by a guest PixMap.
class PixSurface {
 public:
  // Wraps the PixMap a PixMapHandle refers to. Invalid until valid() is true.
  static PixSurface fromHandle(Mem& m, GuestAddr pixMapHandle);
  // Wraps the PixMap embedded in a CGrafPort, or the BitMap of an old GrafPort.
  static PixSurface fromPort(Mem& m, GuestAddr port);

  bool valid() const { return base_ != 0 && rowBytes_ != 0 && depth_ == 8; }

  s16 width() const { return bounds_.width(); }
  s16 height() const { return bounds_.height(); }
  const Rect& bounds() const { return bounds_; }
  GuestAddr colorTable() const { return ctab_; }

  // Pixel access in the surface's own coordinate space, which is offset by
  // bounds().top/left -- a PixMap's bounds need not start at the origin.
  void put(Mem& m, s16 x, s16 y, u8 index) {
    if (x < bounds_.left || x >= bounds_.right ||
        y < bounds_.top || y >= bounds_.bottom)
      return;
    m.w8(addr(x, y), index);
  }
  u8 get(Mem& m, s16 x, s16 y) const {
    if (x < bounds_.left || x >= bounds_.right ||
        y < bounds_.top || y >= bounds_.bottom)
      return 0;
    return m.r8(addr(x, y));
  }
  GuestAddr addr(s16 x, s16 y) const {
    return base_ + u32(y - bounds_.top) * rowBytes_ + u32(x - bounds_.left);
  }

 private:
  GuestAddr base_ = 0;
  u32 rowBytes_ = 0;
  u16 depth_ = 0;
  Rect bounds_;
  GuestAddr ctab_ = 0;
};

}  // namespace cyt
