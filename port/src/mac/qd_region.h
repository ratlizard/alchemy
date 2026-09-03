// Regions.
//
// A classic Region is a bounding box followed, for non-rectangular shapes, by a
// compact scanline encoding. Applications treat regions as opaque handles and
// read only rgnBBox, so the shape is kept host-side as a list of disjoint
// rectangles -- which makes exact union, intersection and difference easy -- and
// the guest sees a valid ten-byte rectangular region holding the bounding box.
#pragma once

#include <map>
#include <vector>

#include "mac/qd_surface.h"
#include "mem.h"

namespace cyt {

using RectList = std::vector<Rect>;

// Set operations producing disjoint rectangle lists.
RectList rectUnion(const RectList& a, const RectList& b);
RectList rectIntersect(const RectList& a, const RectList& b);
RectList rectDifference(const RectList& a, const RectList& b);
Rect rectListBounds(const RectList& a);
bool rectListContains(const RectList& a, s16 x, s16 y);

// ---- shared port drawing state --------------------------------------------
// Text is drawn by the Font Manager rather than by qd_draw, but it has to clip
// and colour itself identically or text would escape windows that shapes
// respect. These three are the whole of what a drawing operation needs to know
// about a port, so they are shared rather than reimplemented.

// The area drawing may touch: the surface narrowed by the port's visRgn and
// clipRgn.
RectList portClipArea(class Toolbox& tb, GuestAddr port, const PixSurface& s);
// The port's foreground and background colours, resolved to indices in the
// destination surface's own colour table.
u8 portForeIndex(class Toolbox& tb, GuestAddr port, const PixSurface& s);
u8 portBackIndex(class Toolbox& tb, GuestAddr port, const PixSurface& s);

class Regions {
 public:
  static Regions& get();

  // Creates a region handle, with the guest-visible ten bytes initialised.
  GuestAddr create(class Toolbox& tb);
  void destroy(class Toolbox& tb, GuestAddr rgn);

  const RectList& shape(GuestAddr rgn);
  void setShape(Mem& m, GuestAddr rgn, RectList shape);
  bool known(GuestAddr rgn) const { return shapes_.count(rgn) != 0; }

 private:
  // Writes rgnSize and rgnBBox so guest reads of the handle stay meaningful.
  void sync(Mem& m, GuestAddr rgn);

  std::map<GuestAddr, RectList> shapes_;
  RectList empty_;
};

}  // namespace cyt
