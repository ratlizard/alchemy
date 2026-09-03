// QuickDraw drawing: ports, rectangles, regions, solid shapes, CopyBits,
// picture playback, cursors, palettes and text metrics.
//
// All drawing goes through the current port's PixMap, clipped to the
// intersection of its visRgn and clipRgn, which is what makes windows that
// overlap behave. Colour is resolved to an index in the destination's colour
// table at draw time rather than cached, because the game installs its own
// palette part-way through startup.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

#include "mac/heap.h"
#include "mac/pict.h"
#include "mac/qd_region.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {

// Pen and text state live in the port itself; only the pattern needs a host-side
// home, since a Pattern is eight bytes the game may pass by value.
struct PenPattern {
  u8 rows[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  bool solid() const {
    for (u8 r : rows) if (r != 0xFF && r != 0x00) return false;
    return true;
  }
  bool blackAt(s16 x, s16 y) const {
    return (rows[y & 7] >> (7 - (x & 7))) & 1;
  }
};
std::map<GuestAddr, PenPattern> g_penPat;    // per port
std::map<GuestAddr, PenPattern> g_backPat;

// Pictures the game has asked for, so DrawPicture can find the decoded form.
struct PictCache {
  DecodedPict img;
  ColorMap toScreen;                       // indexed pictures: index -> index
  std::map<u32, u8> rgbToIndex;            // direct pictures, memoised
  bool mapped = false;
  // The screen colour table the mapping above was built against. A port that
  // changes palettes -- and this game changes them five times before its start
  // screen -- must not keep serving a translation made for a table that is no
  // longer installed.
  u32 clutFingerprint = 0;
};

// Cheap identity for a colour table, so a cached mapping can tell whether the
// table it was built against is still the one installed.
u32 paletteFingerprint(const Palette& p) {
  u32 fp = 2166136261u;
  for (const Rgb& e : p.entries) {
    fp = (fp ^ e.r) * 16777619u;
    fp = (fp ^ e.g) * 16777619u;
    fp = (fp ^ e.b) * 16777619u;
  }
  return fp;
}
std::map<GuestAddr, PictCache> g_picts;

GuestAddr currentPort(Toolbox& tb) {
  return graphics().currentPort;
}

// Short names for the shared port helpers, which the Font Manager uses too.
RectList clipOf(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  return portClipArea(tb, port, s);
}
u8 foreIndex(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  return portForeIndex(tb, port, s);
}
u8 backIndex(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  return portBackIndex(tb, port, s);
}

// Fills the parts of `r` that survive clipping. `mode` follows QuickDraw's
// transfer modes: the even ones copy, srcXor inverts, and the "not" variants
// invert the source first.
void fillRect(Toolbox& tb, const Rect& r, u8 index, u16 mode,
              const PenPattern* pat, u8 patBack) {
  GuestAddr port = currentPort(tb);
  PixSurface s = PixSurface::fromPort(tb.mem(), port);
  if (!s.valid()) return;
  RectList clip = rectIntersect(clipOf(tb, port, s), RectList{r});
  Mem& m = tb.mem();
  for (const Rect& c : clip) {
    for (s16 y = c.top; y < c.bottom; ++y) {
      for (s16 x = c.left; x < c.right; ++x) {
        u8 v = index;
        if (pat && !pat->solid()) v = pat->blackAt(x, y) ? index : patBack;
        else if (pat && pat->rows[0] == 0x00) v = patBack;
        if (mode == 10 /* patXor */ || mode == 2 /* srcXor */)
          v = u8(m.r8(s.addr(x, y)) ^ v);
        m.w8(s.addr(x, y), v);
      }
    }
  }
}

void frameRect(Toolbox& tb, const Rect& r, u8 index, s16 penW, s16 penH) {
  if (r.empty()) return;
  if (penW < 1) penW = 1;
  if (penH < 1) penH = 1;
  fillRect(tb, Rect{r.top, r.left, s16(r.top + penH), r.right}, index, 8, nullptr, 0);
  fillRect(tb, Rect{s16(r.bottom - penH), r.left, r.bottom, r.right}, index, 8, nullptr, 0);
  fillRect(tb, Rect{r.top, r.left, r.bottom, s16(r.left + penW)}, index, 8, nullptr, 0);
  fillRect(tb, Rect{r.top, s16(r.right - penW), r.bottom, r.right}, index, 8, nullptr, 0);
}

Rect argRect(Toolbox& tb, Args& a) { return readRect(tb.mem(), a.ptr()); }

}  // namespace

// ---------------------------------------------------------------------------
// Shared port state, declared in qd_region.h
// ---------------------------------------------------------------------------

RectList portClipArea(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  RectList area{s.bounds()};
  if (!port) return area;
  Mem& m = tb.mem();
  for (u32 field : {qd::kPortVisRgn, qd::kPortClipRgn}) {
    GuestAddr rgn = m.r32(port + field);
    if (!rgn) continue;
    if (Regions::get().known(rgn)) {
      const RectList& shape = Regions::get().shape(rgn);
      // An empty tracked region genuinely clips everything away.
      area = rectIntersect(area, shape);
    } else {
      // A region the game built itself: fall back to its bounding box.
      GuestAddr p = m.r32(rgn);
      if (p) area = rectIntersect(area, RectList{readRect(m, p + 2)});
    }
  }
  return area;
}

// Resolves a port colour to an index in the surface's palette. `field` picks
// the foreground or background RGB; an old monochrome GrafPort has neither, so
// it falls back to the black-on-white the classic port implied.
static u8 portColorIndex(Toolbox& tb, GuestAddr port, const PixSurface& s,
                         u32 field, Rgb fallback) {
  Mem& m = tb.mem();
  Palette dst = readPalette(m, s.colorTable());
  if (dst.empty()) return 0;
  if (port && (m.r16(port + qd::kPortVersion) & 0xC000)) {
    Rgb c{m.r16(port + field + 0), m.r16(port + field + 2),
          m.r16(port + field + 4)};
    return nearestIndex(dst, c);
  }
  return nearestIndex(dst, fallback);
}

u8 portForeIndex(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  return portColorIndex(tb, port, s, qd::kPortRgbFgColor, Rgb{0, 0, 0});
}

u8 portBackIndex(Toolbox& tb, GuestAddr port, const PixSurface& s) {
  return portColorIndex(tb, port, s, qd::kPortRgbBkColor,
                        Rgb{0xFFFF, 0xFFFF, 0xFFFF});
}

void registerQuickDrawDrawing(Toolbox& tb) {
  Mem& m = tb.mem();

  // ---- ports --------------------------------------------------------------
  tb.add("GetPort", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, graphics().currentPort);
  });
  tb.add("SetPort", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    if (p) graphics().currentPort = p;
  });
  tb.add("OpenPort", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    if (!p) return;
    // An opened port starts out describing the whole screen.
    tb.mem().fill(p, 0, qd::kCGrafPortSize);
    tb.mem().w16(p + qd::kPortVersion, 0xC000);
    tb.mem().w32(p + qd::kPortPixMap, graphics().screenPixMapH);
    writeRect(tb.mem(), p + qd::kPortRect,
              Rect{0, 0, kScreenHeight, kScreenWidth});
    GuestAddr vis = Regions::get().create(tb);
    Regions::get().setShape(tb.mem(), vis,
                            RectList{Rect{0, 0, kScreenHeight, kScreenWidth}});
    GuestAddr clip = Regions::get().create(tb);
    Regions::get().setShape(tb.mem(), clip,
                            RectList{Rect{-32767, -32767, 32767, 32767}});
    tb.mem().w32(p + qd::kPortVisRgn, vis);
    tb.mem().w32(p + qd::kPortClipRgn, clip);
    tb.mem().w16(p + qd::kPortPnSize + 0, 1);
    tb.mem().w16(p + qd::kPortPnSize + 2, 1);
    tb.mem().w16(p + qd::kPortPnMode, 8);
    tb.mem().w16(p + qd::kPortTxSize, 12);
    for (int i = 0; i < 3; ++i)
      tb.mem().w16(p + qd::kPortRgbBkColor + u32(i) * 2, 0xFFFF);
    graphics().currentPort = p;
  });
  for (const char* nop : {"ClosePort", "SetPortBits", "SetOrigin",
                          "InitPort", "GrafDevice"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  // ---- rectangles ---------------------------------------------------------
  tb.add("SetRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr r = a.ptr();
    s16 l = a.s16v(), t = a.s16v(), ri = a.s16v(), b = a.s16v();
    writeRect(tb.mem(), r, Rect{t, l, b, ri});
  });
  tb.add("OffsetRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    s16 dh = a.s16v(), dv = a.s16v();
    Rect r = readRect(tb.mem(), p);
    r.top = s16(r.top + dv); r.bottom = s16(r.bottom + dv);
    r.left = s16(r.left + dh); r.right = s16(r.right + dh);
    writeRect(tb.mem(), p, r);
  });
  tb.add("InsetRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    s16 dh = a.s16v(), dv = a.s16v();
    Rect r = readRect(tb.mem(), p);
    r.top = s16(r.top + dv); r.bottom = s16(r.bottom - dv);
    r.left = s16(r.left + dh); r.right = s16(r.right - dh);
    writeRect(tb.mem(), p, r);
  });
  tb.add("SectRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect x = argRect(tb, a), y = argRect(tb, a);
    GuestAddr out = a.ptr();
    Rect o = intersect(x, y);
    if (out) writeRect(tb.mem(), out, o);
    Toolbox::ret(c, o.empty() ? 0u : 1u);
  });
  tb.add("UnionRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect x = argRect(tb, a), y = argRect(tb, a);
    GuestAddr out = a.ptr();
    if (out) writeRect(tb.mem(), out, rectListBounds(RectList{x, y}));
  });
  tb.add("EqualRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect x = argRect(tb, a), y = argRect(tb, a);
    Toolbox::ret(c, (x.top == y.top && x.left == y.left &&
                     x.bottom == y.bottom && x.right == y.right) ? 1u : 0u);
  });
  tb.add("EmptyRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, argRect(tb, a).empty() ? 1u : 0u);
  });
  tb.add("PtInRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 pt = a.u32v();                 // a Point arrives packed: v then h
    s16 v = s16(pt >> 16), h = s16(pt & 0xFFFF);
    Rect r = argRect(tb, a);
    Toolbox::ret(c, (h >= r.left && h < r.right && v >= r.top && v < r.bottom)
                        ? 1u : 0u);
  });
  tb.add("Pt2Rect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 p1 = a.u32v(), p2 = a.u32v();
    GuestAddr out = a.ptr();
    s16 v1 = s16(p1 >> 16), h1 = s16(p1 & 0xFFFF);
    s16 v2 = s16(p2 >> 16), h2 = s16(p2 & 0xFFFF);
    if (out) writeRect(tb.mem(), out,
                       Rect{std::min(v1, v2), std::min(h1, h2),
                            std::max(v1, v2), std::max(h1, h2)});
  });
  auto mapPoint = [](Toolbox& tb, PpcCpu& c, Args& a) {
    // Coordinates are already global in this port's single-screen world.
    a.ptr();
  };
  tb.add("LocalToGlobal", mapPoint);
  tb.add("GlobalToLocal", mapPoint);
  tb.add("MapPt", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); a.ptr(); a.ptr(); });
  tb.add("ScalePt", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); a.ptr(); a.ptr(); });

  // ---- regions ------------------------------------------------------------
  tb.add("NewRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, Regions::get().create(tb));
  });
  tb.add("DisposeRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Regions::get().destroy(tb, a.ptr());
  });
  tb.add("SetEmptyRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Regions::get().setShape(tb.mem(), a.ptr(), RectList{});
  });
  tb.add("RectRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    Rect r = argRect(tb, a);
    Regions::get().setShape(tb.mem(), rgn,
                            r.empty() ? RectList{} : RectList{r});
  });
  tb.add("SetRectRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    s16 l = a.s16v(), t = a.s16v(), ri = a.s16v(), b = a.s16v();
    Rect r{t, l, b, ri};
    Regions::get().setShape(tb.mem(), rgn,
                            r.empty() ? RectList{} : RectList{r});
  });
  tb.add("CopyRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr src = a.ptr(), dst = a.ptr();
    Regions::get().setShape(tb.mem(), dst, Regions::get().shape(src));
  });
  auto binaryRgn = [](RectList (*op)(const RectList&, const RectList&)) {
    return [op](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr x = a.ptr(), y = a.ptr(), out = a.ptr();
      Regions::get().setShape(tb.mem(), out,
                              op(Regions::get().shape(x),
                                 Regions::get().shape(y)));
    };
  };
  tb.add("UnionRgn", binaryRgn(rectUnion));
  tb.add("SectRgn", binaryRgn(rectIntersect));
  tb.add("DiffRgn", binaryRgn(rectDifference));
  tb.add("XorRgn", binaryRgn([](const RectList& x, const RectList& y) {
    return rectUnion(rectDifference(x, y), rectDifference(y, x));
  }));
  tb.add("OffsetRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    s16 dh = a.s16v(), dv = a.s16v();
    RectList s = Regions::get().shape(rgn);
    for (Rect& r : s) {
      r.top = s16(r.top + dv); r.bottom = s16(r.bottom + dv);
      r.left = s16(r.left + dh); r.right = s16(r.right + dh);
    }
    Regions::get().setShape(tb.mem(), rgn, std::move(s));
  });
  tb.add("EmptyRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, rectListBounds(Regions::get().shape(a.ptr())).empty() ? 1u : 0u);
  });
  tb.add("PtInRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 pt = a.u32v();
    GuestAddr rgn = a.ptr();
    Toolbox::ret(c, rectListContains(Regions::get().shape(rgn),
                                     s16(pt & 0xFFFF), s16(pt >> 16)) ? 1u : 0u);
  });
  tb.add("RectInRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect r = argRect(tb, a);
    GuestAddr rgn = a.ptr();
    Toolbox::ret(c, rectIntersect(Regions::get().shape(rgn), RectList{r}).empty()
                        ? 0u : 1u);
  });
  tb.add("EqualRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr x = a.ptr(), y = a.ptr();
    Toolbox::ret(c, rectDifference(Regions::get().shape(x),
                                   Regions::get().shape(y)).empty() &&
                    rectDifference(Regions::get().shape(y),
                                   Regions::get().shape(x)).empty() ? 1u : 0u);
  });
  tb.add("BitMapToRegion", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr(), bmp = a.ptr();
    // Approximated by the bitmap's bounds; Cythera uses this only for cursor
    // and mask shapes whose bounding box is the operative part.
    Regions::get().setShape(tb.mem(), rgn,
                            RectList{readRect(tb.mem(), bmp + 8)});
    Toolbox::ret(c, 0);
  });
  // Region recording: OpenRgn/CloseRgn capture drawing into a region. Cythera
  // uses it for rounded window frames, where the bounding box suffices.
  tb.add("OpenRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {});
  tb.add("CloseRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    if (rgn && !Regions::get().known(rgn))
      Regions::get().setShape(tb.mem(), rgn, RectList{});
  });

  tb.add("GetClip", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    GuestAddr port = graphics().currentPort;
    GuestAddr clip = port ? tb.mem().r32(port + qd::kPortClipRgn) : 0;
    Regions::get().setShape(tb.mem(), rgn, Regions::get().shape(clip));
  });
  tb.add("SetClip", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    GuestAddr port = graphics().currentPort;
    GuestAddr clip = port ? tb.mem().r32(port + qd::kPortClipRgn) : 0;
    if (clip) Regions::get().setShape(tb.mem(), clip, Regions::get().shape(rgn));
  });
  tb.add("ClipRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect r = argRect(tb, a);
    GuestAddr port = graphics().currentPort;
    GuestAddr clip = port ? tb.mem().r32(port + qd::kPortClipRgn) : 0;
    if (clip) Regions::get().setShape(tb.mem(), clip,
                                      r.empty() ? RectList{} : RectList{r});
  });

  // ---- pen and colour -----------------------------------------------------
  tb.add("PenSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 w = a.s16v(), h = a.s16v();
    GuestAddr p = graphics().currentPort;
    if (p) {
      tb.mem().w16(p + qd::kPortPnSize + 0, u16(h));
      tb.mem().w16(p + qd::kPortPnSize + 2, u16(w));
    }
  });
  tb.add("PenMode", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 mode = a.s16v();
    GuestAddr p = graphics().currentPort;
    if (p) tb.mem().w16(p + qd::kPortPnMode, u16(mode));
  });
  tb.add("PenNormal", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = graphics().currentPort;
    if (p) {
      tb.mem().w16(p + qd::kPortPnSize + 0, 1);
      tb.mem().w16(p + qd::kPortPnSize + 2, 1);
      tb.mem().w16(p + qd::kPortPnMode, 8);
      g_penPat[p] = PenPattern{};
    }
  });
  auto setPat = [](std::map<GuestAddr, PenPattern>* store) {
    return [store](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr pat = a.ptr();
      GuestAddr p = graphics().currentPort;
      if (!p) return;
      PenPattern pp;
      for (int i = 0; i < 8; ++i) pp.rows[i] = tb.mem().r8(pat + u32(i));
      (*store)[p] = pp;
    };
  };
  tb.add("PenPat", setPat(&g_penPat));
  tb.add("BackPat", setPat(&g_backPat));

  // ---- pixel patterns -----------------------------------------------------
  // A PixPat is the colour pattern: a PixMap and its pixels, tiled. Cythera
  // builds three of them during start-up -- the 128x128 wooden backdrop behind
  // its windows among them -- by allocating an empty one and filling in the
  // PixMap and pattern data itself. So what NewPixPat has to get right is the
  // shape of the record: the application resizes (**pp).patData and writes
  // through (**pp).patMap directly, and a null handle sends both of those into
  // unmapped memory.
  //
  // PixPat: { short patType; PixMapHandle patMap; Handle patData;
  //           Handle patXData; short patXValid; Handle patXMap;
  //           Pattern pat1Data; } -- 28 bytes.
  tb.add("NewPixPat", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr pph = tb.heap().newHandle(28, true);
    GuestAddr mapH = tb.heap().newHandle(qd::kPixMapSize, true);
    GuestAddr dataH = tb.heap().newHandle(8, true);
    if (!pph || !mapH || !dataH) { Toolbox::ret(c, 0); return; }
    GuestAddr pp = m.r32(pph);
    m.w16(pp + 0, 1);              // a full-colour pattern
    m.w32(pp + 2, mapH);
    m.w32(pp + 6, dataH);
    m.w32(pp + 10, 0);             // no expanded data yet
    m.w16(pp + 14, u16(0xFFFF));   // patXValid: nothing expanded
    m.w32(pp + 16, 0);
    for (u32 i = 0; i < 8; ++i) m.w8(pp + 20 + i, 0xFF);
    // The new pattern describes itself as the smallest legal one -- eight by
    // eight at one bit, as the Pattern it stands in for -- and the caller
    // reshapes it. The high bit of rowBytes is what marks this a PixMap.
    GuestAddr pm = m.r32(mapH);
    m.w16(pm + qd::kPmRowBytes, 0x8000u | 2u);
    writeRect(m, pm + qd::kPmBounds, Rect{0, 0, 8, 8});
    m.w16(pm + qd::kPmPixelSize, 1);
    m.w16(pm + qd::kPmCmpCount, 1);
    m.w16(pm + qd::kPmCmpSize, 1);
    m.w32(pm + qd::kPmHRes, 0x00480000u);
    m.w32(pm + qd::kPmVRes, 0x00480000u);
    m.w32(pm + qd::kPmTable, graphics().colorTableH);
    Toolbox::ret(c, pph);
  });
  tb.add("DisposePixPat", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr pph = a.ptr();
    if (!pph) return;
    GuestAddr pp = tb.mem().r32(pph);
    if (pp) {
      for (u32 field : {2u, 6u, 10u, 16u})
        if (GuestAddr h = tb.mem().r32(pp + field)) tb.heap().disposeHandle(h);
    }
    tb.heap().disposeHandle(pph);
  });
  // The expanded form is what PixPatChanged invalidates. Nothing here caches
  // one -- a pattern is read from the record when it is drawn -- so the record
  // is already up to date by the time the application says so.
  tb.add("PixPatChanged", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });
  // Installing a colour pattern in a port is recorded but does not yet change
  // what is drawn: see the note in POWERPC-NOTES.md. Nothing in the game has drawn
  // through one yet, so tiling is written when there is something to check it
  // against rather than guessed at now.
  tb.add("PenPixPat", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });
  tb.add("BackPixPat", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });
  tb.add("GetPenState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr st = a.ptr();
    GuestAddr p = graphics().currentPort;
    if (!st || !p) return;
    tb.mem().w32(st + 0, tb.mem().r32(p + qd::kPortPnLoc));
    tb.mem().w32(st + 4, tb.mem().r32(p + qd::kPortPnSize));
    tb.mem().w16(st + 8, tb.mem().r16(p + qd::kPortPnMode));
    for (int i = 0; i < 8; ++i)
      tb.mem().w8(st + 10 + u32(i), g_penPat[p].rows[i]);
  });
  tb.add("SetPenState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr st = a.ptr();
    GuestAddr p = graphics().currentPort;
    if (!st || !p) return;
    tb.mem().w32(p + qd::kPortPnLoc, tb.mem().r32(st + 0));
    tb.mem().w32(p + qd::kPortPnSize, tb.mem().r32(st + 4));
    tb.mem().w16(p + qd::kPortPnMode, tb.mem().r16(st + 8));
    PenPattern pp;
    for (int i = 0; i < 8; ++i) pp.rows[i] = tb.mem().r8(st + 10 + u32(i));
    g_penPat[p] = pp;
  });
  for (const char* nop : {"HidePen", "ShowPen", "PenIdle"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  auto setRgbColor = [](u32 field) {
    return [field](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr rgb = a.ptr();
      GuestAddr p = graphics().currentPort;
      if (!p || !rgb) return;
      for (int i = 0; i < 3; ++i)
        tb.mem().w16(p + field + u32(i) * 2, tb.mem().r16(rgb + u32(i) * 2));
    };
  };
  tb.add("RGBForeColor", setRgbColor(qd::kPortRgbFgColor));
  tb.add("RGBBackColor", setRgbColor(qd::kPortRgbBkColor));
  auto getRgbColor = [](u32 field) {
    return [field](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr rgb = a.ptr();
      GuestAddr p = graphics().currentPort;
      if (!p || !rgb) return;
      for (int i = 0; i < 3; ++i)
        tb.mem().w16(rgb + u32(i) * 2, tb.mem().r16(p + field + u32(i) * 2));
    };
  };
  tb.add("GetForeColor", getRgbColor(qd::kPortRgbFgColor));
  tb.add("GetBackColor", getRgbColor(qd::kPortRgbBkColor));
  // The eight classic QuickDraw colour constants.
  auto classicColor = [](u32 field) {
    return [field](Toolbox& tb, PpcCpu& c, Args& a) {
      u32 code = a.u32v();
      GuestAddr p = graphics().currentPort;
      if (!p) return;
      Rgb v{0, 0, 0};
      switch (code) {
        case 30: v = Rgb{0xFFFF, 0xFFFF, 0xFFFF}; break;  // whiteColor
        case 33: v = Rgb{0, 0, 0}; break;                 // blackColor
        case 69: v = Rgb{0xFFFF, 0xFFFF, 0}; break;       // yellowColor
        case 137: v = Rgb{0xFFFF, 0, 0xFFFF}; break;      // magentaColor
        case 205: v = Rgb{0xDDDD, 0x0808, 0x0666}; break;  // redColor
        case 273: v = Rgb{0, 0xFFFF, 0xFFFF}; break;      // cyanColor
        case 341: v = Rgb{0, 0x8000, 0x1111}; break;      // greenColor
        case 409: v = Rgb{0, 0, 0xD400}; break;           // blueColor
        default: break;
      }
      tb.mem().w16(p + field + 0, v.r);
      tb.mem().w16(p + field + 2, v.g);
      tb.mem().w16(p + field + 4, v.b);
    };
  };
  tb.add("ForeColor", classicColor(qd::kPortRgbFgColor));
  tb.add("BackColor", classicColor(qd::kPortRgbBkColor));
  tb.add("OpColor", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });
  tb.add("Index2Color", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s32 idx = a.s32v();
    GuestAddr rgb = a.ptr();
    Palette pal = readPalette(tb.mem(), graphics().colorTableH);
    Rgb v = (!pal.empty() && idx >= 0 && idx < 256)
                ? pal.entries[size_t(idx)] : Rgb{};
    if (rgb) {
      tb.mem().w16(rgb + 0, v.r);
      tb.mem().w16(rgb + 2, v.g);
      tb.mem().w16(rgb + 4, v.b);
    }
  });
  tb.add("Color2Index", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgb = a.ptr();
    Palette pal = readPalette(tb.mem(), graphics().colorTableH);
    Rgb v{tb.mem().r16(rgb + 0), tb.mem().r16(rgb + 2), tb.mem().r16(rgb + 4)};
    Toolbox::ret(c, nearestIndex(pal, v));
  });

  // ---- Color Picker colour-space conversion --------------------------------
  //
  // RGB2HSL and HSL2RGB come from the Color Picker package rather than
  // QuickDraw proper, but they are pure arithmetic over an RGBColor and they
  // belong beside Index2Color rather than in a file of their own. Cythera
  // calls them while loading a saved game -- nineteen and eighteen times -- so
  // they are on the critical path even though nothing is drawn with them here.
  //
  // Both structures are three unsigned 16-bit components. In an HSLColor the
  // hue runs 0..65535 for a full turn, and saturation and lightness run
  // 0..65535 for 0..1.
  tb.add("RGB2HSL", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr in = a.ptr(), out = a.ptr();
    if (!in || !out) return;
    const double r = m.r16(in + 0) / 65535.0;
    const double g = m.r16(in + 2) / 65535.0;
    const double b = m.r16(in + 4) / 65535.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    const double l = (mx + mn) / 2.0;
    double h = 0.0, sat = 0.0;
    if (mx > mn) {
      const double d = mx - mn;
      sat = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
      if (mx == r)      h = (g - b) / d + (g < b ? 6.0 : 0.0);
      else if (mx == g) h = (b - r) / d + 2.0;
      else              h = (r - g) / d + 4.0;
      h /= 6.0;
    }
    auto q = [](double v) {
      return u16(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0));
    };
    m.w16(out + 0, q(h));
    m.w16(out + 2, q(sat));
    m.w16(out + 4, q(l));
  });
  tb.add("HSL2RGB", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr in = a.ptr(), out = a.ptr();
    if (!in || !out) return;
    const double h = m.r16(in + 0) / 65535.0;
    const double sat = m.r16(in + 2) / 65535.0;
    const double l = m.r16(in + 4) / 65535.0;
    double r = l, g = l, b = l;
    if (sat > 0.0) {
      const double q2 = l < 0.5 ? l * (1.0 + sat) : l + sat - l * sat;
      const double p = 2.0 * l - q2;
      auto hue = [&](double t) {
        if (t < 0.0) t += 1.0;
        if (t > 1.0) t -= 1.0;
        if (t < 1.0 / 6.0) return p + (q2 - p) * 6.0 * t;
        if (t < 1.0 / 2.0) return q2;
        if (t < 2.0 / 3.0) return p + (q2 - p) * (2.0 / 3.0 - t) * 6.0;
        return p;
      };
      r = hue(h + 1.0 / 3.0);
      g = hue(h);
      b = hue(h - 1.0 / 3.0);
    }
    auto q = [](double v) {
      return u16(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0));
    };
    m.w16(out + 0, q(r));
    m.w16(out + 2, q(g));
    m.w16(out + 4, q(b));
  });

  // ---- solid shapes -------------------------------------------------------
  auto rectOp = [](int kind) {
    // kind: 0 paint (fore), 1 erase (back), 2 invert, 3 frame, 4 fill w/ pattern
    return [kind](Toolbox& tb, PpcCpu& c, Args& a) {
      Rect r = readRect(tb.mem(), a.ptr());
      GuestAddr port = graphics().currentPort;
      PixSurface s = PixSurface::fromPort(tb.mem(), port);
      if (!s.valid()) return;
      PenPattern* pat = nullptr;
      u8 idx = 0, back = 0;
      if (kind == 4) {
        GuestAddr patPtr = a.ptr();
        static PenPattern tmp;
        for (int i = 0; i < 8; ++i) tmp.rows[i] = tb.mem().r8(patPtr + u32(i));
        pat = &tmp;
        idx = foreIndex(tb, port, s);
        back = backIndex(tb, port, s);
      }
      switch (kind) {
        case 0: fillRect(tb, r, foreIndex(tb, port, s), 8, nullptr, 0); break;
        case 1: fillRect(tb, r, backIndex(tb, port, s), 8, nullptr, 0); break;
        case 2: fillRect(tb, r, 0xFF, 10, nullptr, 0); break;
        case 3: {
          s16 ph = s16(tb.mem().r16(port + qd::kPortPnSize + 0));
          s16 pw = s16(tb.mem().r16(port + qd::kPortPnSize + 2));
          frameRect(tb, r, foreIndex(tb, port, s), pw, ph);
          break;
        }
        case 4: fillRect(tb, r, idx, 8, pat, back); break;
      }
    };
  };
  tb.add("PaintRect", rectOp(0));
  tb.add("EraseRect", rectOp(1));
  tb.add("InvertRect", rectOp(2));
  tb.add("FrameRect", rectOp(3));
  tb.add("FillRect", rectOp(4));
  tb.add("FillCRect", rectOp(4));
  // Rounded rectangles and ovals are approximated by their bounding rectangle:
  // the shapes Cythera draws this way are window frames and button surrounds
  // that its own artwork already covers.
  tb.add("PaintRoundRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect r = readRect(tb.mem(), a.ptr());
    a.s16v(); a.s16v();
    GuestAddr port = graphics().currentPort;
    PixSurface s = PixSurface::fromPort(tb.mem(), port);
    if (s.valid()) fillRect(tb, r, foreIndex(tb, port, s), 8, nullptr, 0);
  });
  tb.add("FrameRoundRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect r = readRect(tb.mem(), a.ptr());
    a.s16v(); a.s16v();
    GuestAddr port = graphics().currentPort;
    PixSurface s = PixSurface::fromPort(tb.mem(), port);
    if (s.valid()) frameRect(tb, r, foreIndex(tb, port, s), 1, 1);
  });
  tb.add("EraseRoundRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Rect r = readRect(tb.mem(), a.ptr());
    a.s16v(); a.s16v();
    GuestAddr port = graphics().currentPort;
    PixSurface s = PixSurface::fromPort(tb.mem(), port);
    if (s.valid()) fillRect(tb, r, backIndex(tb, port, s), 8, nullptr, 0);
  });
  auto rgnOp = [](int kind) {
    return [kind](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr rgn = a.ptr();
      if (kind == 4) a.ptr();
      GuestAddr port = graphics().currentPort;
      PixSurface s = PixSurface::fromPort(tb.mem(), port);
      if (!s.valid()) return;
      u8 idx = (kind == 1) ? backIndex(tb, port, s) : foreIndex(tb, port, s);
      for (const Rect& r : Regions::get().shape(rgn)) {
        if (kind == 3) frameRect(tb, r, idx, 1, 1);
        else fillRect(tb, r, kind == 2 ? 0xFF : idx, kind == 2 ? 10 : 8,
                      nullptr, 0);
      }
    };
  };
  tb.add("PaintRgn", rgnOp(0));
  tb.add("EraseRgn", rgnOp(1));
  tb.add("InvertRgn", rgnOp(2));
  tb.add("FrameRgn", rgnOp(3));
  tb.add("FillRgn", rgnOp(4));
  tb.add("FillCRgn", rgnOp(4));

  // ---- lines --------------------------------------------------------------
  auto moveTo = [](bool relative) {
    return [relative](Toolbox& tb, PpcCpu& c, Args& a) {
      s16 h = a.s16v(), v = a.s16v();
      GuestAddr p = graphics().currentPort;
      if (!p) return;
      if (relative) {
        h = s16(s16(tb.mem().r16(p + qd::kPortPnLoc + 2)) + h);
        v = s16(s16(tb.mem().r16(p + qd::kPortPnLoc + 0)) + v);
      }
      tb.mem().w16(p + qd::kPortPnLoc + 0, u16(v));
      tb.mem().w16(p + qd::kPortPnLoc + 2, u16(h));
    };
  };
  tb.add("MoveTo", moveTo(false));
  tb.add("Move", moveTo(true));
  auto lineTo = [](bool relative) {
    return [relative](Toolbox& tb, PpcCpu& c, Args& a) {
      s16 h = a.s16v(), v = a.s16v();
      GuestAddr port = graphics().currentPort;
      PixSurface s = PixSurface::fromPort(tb.mem(), port);
      if (!port || !s.valid()) return;
      s16 y0 = s16(tb.mem().r16(port + qd::kPortPnLoc + 0));
      s16 x0 = s16(tb.mem().r16(port + qd::kPortPnLoc + 2));
      s16 x1 = relative ? s16(x0 + h) : h;
      s16 y1 = relative ? s16(y0 + v) : v;
      // A Bresenham walk, drawing one pen-sized rectangle per step so that pen
      // width behaves as QuickDraw's does.
      const u8 idx = foreIndex(tb, port, s);
      s16 ph = s16(tb.mem().r16(port + qd::kPortPnSize + 0));
      s16 pw = s16(tb.mem().r16(port + qd::kPortPnSize + 2));
      if (ph < 1) ph = 1;
      if (pw < 1) pw = 1;
      s32 dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
      s32 sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
      s32 err = dx + dy;
      s16 x = x0, y = y0;
      for (int guard = 0; guard < 8192; ++guard) {
        fillRect(tb, Rect{y, x, s16(y + ph), s16(x + pw)}, idx, 8, nullptr, 0);
        if (x == x1 && y == y1) break;
        s32 e2 = 2 * err;
        if (e2 >= dy) { err += dy; x = s16(x + sx); }
        if (e2 <= dx) { err += dx; y = s16(y + sy); }
      }
      tb.mem().w16(port + qd::kPortPnLoc + 0, u16(y1));
      tb.mem().w16(port + qd::kPortPnLoc + 2, u16(x1));
    };
  };
  tb.add("LineTo", lineTo(false));
  tb.add("Line", lineTo(true));

  // ---- CopyBits -----------------------------------------------------------
  // The workhorse: move a rectangle of pixels between two PixMaps, scaling when
  // the source and destination rectangles differ, and translating colour when
  // the two colour tables do not agree.
  auto copyBits = [](bool withMask) {
    return [withMask](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr srcBits = a.ptr();
      GuestAddr dstBits = a.ptr();
      if (withMask) a.ptr();               // mask BitMap
      Rect srcR = readRect(tb.mem(), a.ptr());
      Rect dstR = readRect(tb.mem(), a.ptr());
      if (withMask) a.ptr();               // second rect for CopyDeepMask
      u16 mode = a.u16v();
      GuestAddr maskRgn = a.ptr();
      // Neither the transfer mode nor the mask region is honoured yet. Every
      // copy is srcCopy. Nothing on the start screen notices -- that artwork is
      // DrawPicture, not CopyBits -- but the arithmetic and transparent modes
      // are how a sprite composites, so this will matter as soon as the
      // gameplay rendering path runs.
      (void)mode;
      (void)maskRgn;

      Mem& m = tb.mem();
      // A BitMap and a PixMap share their first fields, so the same reader works
      // when handed the struct's address rather than a handle to it.
      auto direct = [&m](GuestAddr pm) {
        struct R { GuestAddr base; u32 rowBytes; u16 depth; Rect bounds;
                   GuestAddr ctab; };
        R r{};
        r.base = m.r32(pm + qd::kPmBaseAddr);
        u16 rb = m.r16(pm + qd::kPmRowBytes);
        r.rowBytes = rb & 0x3FFF;
        r.bounds = readRect(m, pm + qd::kPmBounds);
        r.depth = (rb & 0x8000) ? m.r16(pm + qd::kPmPixelSize) : 1;
        r.ctab = (rb & 0x8000) ? m.r32(pm + qd::kPmTable) : 0;
        return r;
      };
      auto S = direct(srcBits), D = direct(dstBits);
      if (!S.base || !D.base || S.depth != 8 || D.depth != 8) return;

      Palette sp = readPalette(m, S.ctab), dp = readPalette(m, D.ctab);
      ColorMap cm(sp, dp);

      // Clip the destination to the current port's drawable area.
      GuestAddr port = graphics().currentPort;
      PixSurface ds = PixSurface::fromPort(m, port);
      RectList clip{dstR};
      if (ds.valid()) clip = rectIntersect(clipOf(tb, port, ds), RectList{dstR});

      const s16 sw = srcR.width(), sh = srcR.height();
      const s16 dw = dstR.width(), dh = dstR.height();
      if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

      for (const Rect& piece : clip) {
        for (s16 y = piece.top; y < piece.bottom; ++y) {
          const s16 sy = s16(srcR.top + s32(y - dstR.top) * sh / dh);
          if (sy < S.bounds.top || sy >= S.bounds.bottom) continue;
          for (s16 x = piece.left; x < piece.right; ++x) {
            const s16 sx = s16(srcR.left + s32(x - dstR.left) * sw / dw);
            if (sx < S.bounds.left || sx >= S.bounds.right) continue;
            const GuestAddr sa = S.base +
                u32(sy - S.bounds.top) * S.rowBytes + u32(sx - S.bounds.left);
            const GuestAddr da = D.base +
                u32(y - D.bounds.top) * D.rowBytes + u32(x - D.bounds.left);
            m.w8(da, cm[m.r8(sa)]);
          }
        }
      }
    };
  };
  tb.add("CopyBits", copyBits(false));
  tb.add("CopyMask", copyBits(true));
  tb.add("CopyDeepMask", copyBits(true));
  tb.add("SeedFill", [](Toolbox& tb, PpcCpu& c, Args& a) {});
  tb.add("CalcMask", [](Toolbox& tb, PpcCpu& c, Args& a) {});
  tb.add("ScrollRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.s16v(); a.s16v(); a.ptr();
  });

  // ---- pictures -----------------------------------------------------------
  tb.add("GetPicture", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    // A PicHandle is an ordinary resource handle; DrawPicture decodes lazily.
    Args again(c, tb.mem());
    (void)again;
    std::vector<u8> data;
    if (!peekResource(resType("PICT"), id, &data)) {
      Toolbox::ret(c, 0);
      return;
    }
    GuestAddr h = tb.heap().newHandle(u32(data.size()), false);
    if (!h) { Toolbox::ret(c, 0); return; }
    tb.mem().copyIn(tb.mem().r32(h), data.data(), u32(data.size()));
    Toolbox::ret(c, h);
  });
  tb.add("DrawPicture", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr pic = a.ptr();
    Rect dst = readRect(tb.mem(), a.ptr());
    if (!pic) return;
    GuestAddr data = tb.mem().r32(pic);
    if (!data) return;
    const u32 len = tb.heap().handleSize(pic);

    auto it = g_picts.find(pic);
    if (it == g_picts.end()) {
      PictCache pc;
      pc.img = decodePict(tb.mem(), data, len ? len : 0x100000);
      if (!pc.img.ok) {
        std::fprintf(stderr, "  [QuickDraw] DrawPicture: %s\n",
                     pc.img.error.c_str());
      }
      if (const char* dir = std::getenv("CYT_DUMP_PICT")) {
        // Write the raw picture bytes out so they can be read with the other
        // PICT decoders in this repository -- utilities/quickdraw_pict_decoder.py
        // and js/mac-rsrc-types.js -- which is the cheapest way to tell a bug in
        // this decoder from a picture that is genuinely unusual.
        char path[512];
        std::snprintf(path, sizeof path, "%s/pic_%08x.pict", dir, pic);
        if (std::FILE* f = std::fopen(path, "wb")) {
          for (u32 i = 0; i < (len ? len : 0u); ++i)
            std::fputc(tb.mem().r8(data + i), f);
          std::fclose(f);
        }
      }
      it = g_picts.emplace(pic, std::move(pc)).first;
    }
    PictCache& pc = it->second;
    if (!pc.img.ok) return;

    GuestAddr port = graphics().currentPort;
    PixSurface s = PixSurface::fromPort(tb.mem(), port);
    if (!s.valid()) return;
    if (std::getenv("CYT_DEBUG_QD")) {
      RectList cl = rectIntersect(clipOf(tb, port, s), RectList{dst});
      Rect cb = rectListBounds(cl);
      std::fprintf(stderr,
                   "  [QD] DrawPicture %dx%d -> dst(%d,%d,%d,%d) port=%08x "
                   "clip=%zu piece(s) bounds(%d,%d,%d,%d)\n",
                   pc.img.width, pc.img.height, dst.top, dst.left, dst.bottom,
                   dst.right, port, cl.size(), cb.top, cb.left, cb.bottom,
                   cb.right);
    }
    const Palette screenPal = readPalette(tb.mem(), s.colorTable());
    const u32 fp = paletteFingerprint(screenPal);
    if (!pc.mapped || pc.clutFingerprint != fp) {
      pc.toScreen = ColorMap(pc.img.palette, screenPal);
      pc.rgbToIndex.clear();
      pc.mapped = true;
      pc.clutFingerprint = fp;
    }
    if (std::getenv("CYT_DEBUG_PICT"))
      std::fprintf(stderr,
                   "  [PICT] handle=%08x %dx%d %s dst(%d,%d) clut=%08x\n",
                   pic, pc.img.width, pc.img.height,
                   pc.img.direct ? "direct" : "indexed", dst.top, dst.left, fp);

    // The picture's frame maps onto the destination rectangle, scaling if they
    // differ, which is how DrawPicture is specified.
    const s16 dw = dst.width(), dh = dst.height();
    if (dw <= 0 || dh <= 0) return;
    Mem& m = tb.mem();
    for (const Rect& piece : rectIntersect(clipOf(tb, port, s), RectList{dst})) {
      for (s16 y = piece.top; y < piece.bottom; ++y) {
        const s32 sy = s32(y - dst.top) * pc.img.height / dh;
        if (sy < 0 || sy >= pc.img.height) continue;
        if (pc.img.direct) {
          // True colour, matched to the nearest entry the screen actually has.
          // The result is memoised: a torch frame holds a few hundred distinct
          // colours over thirteen thousand pixels, so the search runs once per
          // colour rather than once per pixel.
          const Rgb* row = pc.img.rgb.data() + size_t(sy) * size_t(pc.img.width);
          for (s16 x = piece.left; x < piece.right; ++x) {
            const s32 sx = s32(x - dst.left) * pc.img.width / dw;
            if (sx < 0 || sx >= pc.img.width) continue;
            const Rgb v = row[sx];
            const u32 key = (u32(v.r >> 8) << 16) | (u32(v.g >> 8) << 8) |
                            u32(v.b >> 8);
            auto hit = pc.rgbToIndex.find(key);
            if (hit == pc.rgbToIndex.end())
              hit = pc.rgbToIndex.emplace(key, nearestIndex(screenPal, v)).first;
            m.w8(s.addr(x, y), hit->second);
          }
        } else {
          const u8* row =
              pc.img.pixels.data() + size_t(sy) * size_t(pc.img.width);
          for (s16 x = piece.left; x < piece.right; ++x) {
            const s32 sx = s32(x - dst.left) * pc.img.width / dw;
            if (sx < 0 || sx >= pc.img.width) continue;
            m.w8(s.addr(x, y), pc.toScreen[row[sx]]);
          }
        }
      }
    }
  });
  tb.add("KillPicture", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr pic = a.ptr();
    g_picts.erase(pic);
    tb.heap().disposeHandle(pic);
  });
  tb.add("OpenPicture", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Toolbox::ret(c, 0);   // picture recording is not needed by this game
  });
  for (const char* nop : {"ClosePicture", "PicComment", "OpenPoly",
                          "ClosePoly", "KillPoly", "PaintPoly", "FramePoly",
                          "ErasePoly", "FillPoly", "OffsetPoly"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  // ---- cursors ------------------------------------------------------------
  // The pointer is drawn by the host window system, so a cursor change only has
  // to be remembered; the front end maps it onto a system cursor.
  tb.add("GetCursor", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    std::vector<u8> d;
    if (!peekResource(resType("CURS"), id, &d)) { Toolbox::ret(c, 0); return; }
    GuestAddr h = tb.heap().newHandle(u32(d.size()), false);
    if (h) tb.mem().copyIn(tb.mem().r32(h), d.data(), u32(d.size()));
    Toolbox::ret(c, h);
  });
  tb.add("GetCCursor", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    std::vector<u8> d;
    if (!peekResource(resType("crsr"), id, &d)) { Toolbox::ret(c, 0); return; }
    GuestAddr h = tb.heap().newHandle(u32(d.size()), false);
    if (h) tb.mem().copyIn(tb.mem().r32(h), d.data(), u32(d.size()));
    Toolbox::ret(c, h);
  });
  for (const char* nop : {"SetCursor", "SetCCursor", "DisposeCCursor",
                          "ShieldCursor", "Show_Cursor"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });

  // ---- palettes -----------------------------------------------------------
  // Shared ordering probe: which of SetPalette / SetEntries writes the screen
  // colour table last decides what every picture is matched into.
  // The screen has one colour table. Installing a palette rewrites it, which is
  // what lets the game's artwork show its own colours.
  tb.add("GetNewPalette", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    std::vector<u8> d;
    if (!peekResource(resType("pltt"), id, &d) || d.size() < 8) {
      Toolbox::ret(c, 0);
      return;
    }
    GuestAddr h = tb.heap().newHandle(u32(d.size()), false);
    if (h) tb.mem().copyIn(tb.mem().r32(h), d.data(), u32(d.size()));
    Toolbox::ret(c, h);
  });
  tb.add("NewPalette", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 entries = a.s16v();
    a.ptr(); a.s16v(); a.s16v();
    GuestAddr h = tb.heap().newHandle(8 + u32(entries) * 16, true);
    Toolbox::ret(c, h);
  });
  tb.add("SetPalette", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                  // the window
    GuestAddr pal = a.ptr();
    if (std::getenv("CYT_DEBUG_ORDER"))
      std::fprintf(stderr, "  [order] SetPalette pal=%08x\n", pal);
    a.u8v();
    if (!pal) return;
    // A 'pltt' is a sixteen-byte header (entry count plus reserved fields)
    // followed by one sixteen-byte ColorInfo per entry: the RGB triple, then
    // usage, tolerance, flags and private words. Cythera's 'pltt' 130 is 4112
    // bytes, which is exactly 16 + 256 * 16 and confirms both sizes.
    constexpr u32 kPaletteHeader = 16;
    constexpr u32 kColorInfoSize = 16;
    GuestAddr p = tb.mem().r32(pal);
    if (!p) return;
    const u16 count = tb.mem().r16(p + 0);
    GuestAddr ctab = tb.mem().r32(graphics().colorTableH);
    if (!ctab) return;
    for (u32 i = 0; i < count && i < 256; ++i) {
      GuestAddr e = p + kPaletteHeader + i * kColorInfoSize;
      GuestAddr d = ctab + qd::kCtTable + i * qd::kColorSpecSize;
      tb.mem().w16(d + 0, u16(i));
      tb.mem().w16(d + 2, tb.mem().r16(e + 0));
      tb.mem().w16(d + 4, tb.mem().r16(e + 2));
      tb.mem().w16(d + 6, tb.mem().r16(e + 4));
    }
    tb.mem().w16(ctab + qd::kCtSize, u16(count ? count - 1 : 255));
  });
  tb.add("SetEntries", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 start = a.s16v();
    s16 count = a.s16v();
    GuestAddr table = a.ptr();
    if (std::getenv("CYT_DEBUG_ORDER")) {
      static int seq = 0;
      std::fprintf(stderr, "  [order] #%d SetEntries\n", ++seq);
    }
    if (std::getenv("CYT_DEBUG_ENTRIES")) {
      static int n = 0;
      if (n++ < 4) {
        std::fprintf(stderr, "  [SetEntries] start=%d count=%d table=%08x  ",
                     start, count, table);
        for (int k : {0, 4, 7, 13, 167}) {
          GuestAddr e = table + u32(k) * qd::kColorSpecSize;
          std::fprintf(stderr, "[%d]=(%u,%u,%u) ", k,
                       unsigned(tb.mem().r16(e + 2) >> 8),
                       unsigned(tb.mem().r16(e + 4) >> 8),
                       unsigned(tb.mem().r16(e + 6) >> 8));
        }
        std::fprintf(stderr, "\n");
      }
    }
    GuestAddr ctab = tb.mem().r32(graphics().colorTableH);
    if (!ctab || !table) return;
    // Cythera hands this call the screen's OWN ctTable as the source -- the
    // table argument is ctab + kCtTable. On real hardware that is meaningful:
    // SetEntries pushes the colour table out to the video card, so a program
    // that scales the table in place and calls SetEntries is fading the
    // display. Here the framebuffer is read through that same table every
    // frame, so the copy is a no-op and correctly changes nothing.
    for (s16 i = 0; i <= count; ++i) {
      const s16 idx = s16(start + i);
      if (idx < 0 || idx > 255) continue;
      GuestAddr srcE = table + u32(i) * qd::kColorSpecSize;
      GuestAddr d = ctab + qd::kCtTable + u32(idx) * qd::kColorSpecSize;
      tb.mem().w16(d + 0, u16(idx));
      for (int k = 0; k < 3; ++k)
        tb.mem().w16(d + 2 + u32(k) * 2, tb.mem().r16(srcE + 2 + u32(k) * 2));
    }
  });
  tb.add("CTab2Palette", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr(); a.s16v(); a.s16v();
  });
  tb.add("Palette2CTab", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); a.ptr(); });
  tb.add("GetCTable", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v();
    Toolbox::ret(c, graphics().colorTableH);
  });
  for (const char* nop : {"ActivatePalette", "DisposePalette", "SetEntryUsage",
                          "PmForeColor", "PmBackColor", "GetSubTable",
                          "DisposeCTable", "RestoreDeviceClut", "GetAuxWin",
                          "GetPalette", "AnimatePalette"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });

  // ---- offscreen worlds ---------------------------------------------------
  tb.add("NewGWorld", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    s16 depth = a.s16v();
    Rect bounds = readRect(tb.mem(), a.ptr());
    a.ptr(); a.ptr(); a.s32v();
    if (depth == 0) depth = kScreenDepth;
    const s16 w = bounds.width(), h = bounds.height();
    if (w <= 0 || h <= 0 || depth != 8) { Toolbox::ret(c, u32(s32(-1))); return; }
    const u32 rowBytes = u32((w + 3) & ~3);
    GuestAddr bits = tb.heap().newPtr(rowBytes * u32(h), true);
    GuestAddr pmH = tb.heap().newHandle(qd::kPixMapSize, true);
    GuestAddr port = tb.heap().newPtr(qd::kCGrafPortSize, true);
    if (!bits || !pmH || !port) { Toolbox::ret(c, u32(s32(-108))); return; }
    Mem& m = tb.mem();
    GuestAddr pm = m.r32(pmH);
    m.w32(pm + qd::kPmBaseAddr, bits);
    m.w16(pm + qd::kPmRowBytes, u16(0x8000 | rowBytes));
    writeRect(m, pm + qd::kPmBounds, bounds);
    m.w32(pm + qd::kPmHRes, 72u << 16);
    m.w32(pm + qd::kPmVRes, 72u << 16);
    m.w16(pm + qd::kPmPixelSize, u16(depth));
    m.w16(pm + qd::kPmCmpCount, 1);
    m.w16(pm + qd::kPmCmpSize, u16(depth));
    m.w32(pm + qd::kPmTable, graphics().colorTableH);
    m.w16(port + qd::kPortVersion, 0xC000);
    m.w32(port + qd::kPortPixMap, pmH);
    writeRect(m, port + qd::kPortRect, bounds);
    GuestAddr vis = Regions::get().create(tb);
    Regions::get().setShape(m, vis, RectList{bounds});
    GuestAddr clip = Regions::get().create(tb);
    Regions::get().setShape(m, clip, RectList{Rect{-32767, -32767, 32767, 32767}});
    m.w32(port + qd::kPortVisRgn, vis);
    m.w32(port + qd::kPortClipRgn, clip);
    m.w16(port + qd::kPortPnSize + 0, 1);
    m.w16(port + qd::kPortPnSize + 2, 1);
    m.w16(port + qd::kPortTxSize, 12);
    for (int i = 0; i < 3; ++i)
      m.w16(port + qd::kPortRgbBkColor + u32(i) * 2, 0xFFFF);
    if (out) m.w32(out, port);
    Toolbox::ret(c, 0);
  });
  tb.add("DisposeGWorld", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr port = a.ptr();
    if (!port) return;
    GuestAddr pmH = tb.mem().r32(port + qd::kPortPixMap);
    if (pmH) {
      GuestAddr pm = tb.mem().r32(pmH);
      if (pm) tb.heap().disposePtr(tb.mem().r32(pm + qd::kPmBaseAddr));
      tb.heap().disposeHandle(pmH);
    }
    tb.heap().disposePtr(port);
  });
  tb.add("GetGWorld", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr portOut = a.ptr(), devOut = a.ptr();
    if (portOut) tb.mem().w32(portOut, graphics().currentPort);
    if (devOut) tb.mem().w32(devOut, graphics().currentDevice);
  });
  tb.add("SetGWorld", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr port = a.ptr();
    GuestAddr dev = a.ptr();
    if (port) graphics().currentPort = port;
    if (dev) graphics().currentDevice = dev;
  });
  tb.add("UpdateGWorld", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.s16v(); a.ptr(); a.ptr(); a.ptr(); a.s32v();
    Toolbox::ret(c, 0);
  });
  tb.add("DeviceLoop", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rgn = a.ptr();
    GuestAddr proc = a.ptr();
    s32 refCon = a.s32v();
    a.u32v();
    // One screen means the drawing procedure is invoked exactly once, with the
    // device's depth and flags, which is what the callback expects.
    if (!proc) return;
    GuestAddr tv = proc;
    if (tb.mem().r32(proc) == 0xAAFE0000u) tv = tb.mem().r32(proc + 4);
    u32 code = tv ? tb.mem().r32(tv) : 0;
    if (code < layout::kCodeBase || code >= layout::kHeapBase) return;
    tb.interp().callPpc(code, tb.mem().r32(tv + 4),
                        {u32(kScreenDepth), u32(0), graphics().mainDevice,
                         u32(refCon)});
    (void)rgn;
  });

  // Text drawing and measurement live in the Font Manager, which registers
  // DrawChar, DrawString, DrawText, the width calls and the port's text state.
  (void)m;
}

}  // namespace cyt
