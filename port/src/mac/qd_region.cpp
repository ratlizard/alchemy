#include "mac/qd_region.h"

#include "mac/heap.h"
#include "toolbox.h"

namespace cyt {
namespace {

// Splits `a` by removing `b`, appending the surviving pieces. Up to four
// rectangles remain: above, below, left and right of the overlap.
void subtractInto(const Rect& a, const Rect& b, RectList* out) {
  Rect o = intersect(a, b);
  if (o.empty()) {
    out->push_back(a);
    return;
  }
  if (o.top > a.top)       out->push_back(Rect{a.top, a.left, o.top, a.right});
  if (o.bottom < a.bottom) out->push_back(Rect{o.bottom, a.left, a.bottom, a.right});
  if (o.left > a.left)     out->push_back(Rect{o.top, a.left, o.bottom, o.left});
  if (o.right < a.right)   out->push_back(Rect{o.top, o.right, o.bottom, a.right});
}

}  // namespace

RectList rectDifference(const RectList& a, const RectList& b) {
  RectList cur = a;
  for (const Rect& sub : b) {
    RectList next;
    for (const Rect& r : cur) subtractInto(r, sub, &next);
    cur.swap(next);
  }
  return cur;
}

RectList rectUnion(const RectList& a, const RectList& b) {
  // Keeping the result disjoint means removing the overlap from one side first.
  RectList out = rectDifference(a, b);
  for (const Rect& r : b)
    if (!r.empty()) out.push_back(r);
  return out;
}

RectList rectIntersect(const RectList& a, const RectList& b) {
  RectList out;
  for (const Rect& x : a)
    for (const Rect& y : b) {
      Rect o = intersect(x, y);
      if (!o.empty()) out.push_back(o);
    }
  return out;
}

Rect rectListBounds(const RectList& a) {
  Rect r{};
  bool first = true;
  for (const Rect& x : a) {
    if (x.empty()) continue;
    if (first) { r = x; first = false; continue; }
    if (x.top < r.top) r.top = x.top;
    if (x.left < r.left) r.left = x.left;
    if (x.bottom > r.bottom) r.bottom = x.bottom;
    if (x.right > r.right) r.right = x.right;
  }
  return r;
}

bool rectListContains(const RectList& a, s16 x, s16 y) {
  for (const Rect& r : a)
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return true;
  return false;
}

Regions& Regions::get() {
  static Regions instance;
  return instance;
}

GuestAddr Regions::create(Toolbox& tb) {
  GuestAddr h = tb.heap().newHandle(10, true);
  if (!h) return 0;
  shapes_[h] = RectList{};
  sync(tb.mem(), h);
  return h;
}

void Regions::destroy(Toolbox& tb, GuestAddr rgn) {
  if (!rgn) return;
  shapes_.erase(rgn);
  tb.heap().disposeHandle(rgn);
}

const RectList& Regions::shape(GuestAddr rgn) {
  auto it = shapes_.find(rgn);
  return it == shapes_.end() ? empty_ : it->second;
}

void Regions::setShape(Mem& m, GuestAddr rgn, RectList shape) {
  if (!rgn) return;
  shapes_[rgn] = std::move(shape);
  sync(m, rgn);
}

void Regions::sync(Mem& m, GuestAddr rgn) {
  GuestAddr p = m.r32(rgn);
  if (!p) return;
  m.w16(p, 10);                                  // rectangular region size
  writeRect(m, p + 2, rectListBounds(shapes_[rgn]));
}

}  // namespace cyt
