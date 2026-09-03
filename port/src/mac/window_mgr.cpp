// Window Manager.
//
// Classic windows draw straight into the screen, so a WindowRecord embeds a
// CGrafPort whose PixMap is the screen's and whose visRgn is the part of the
// window not covered by anything in front. Keeping that invariant is what makes
// overlapping windows work, and it is why the region layer had to come first.
//
// Cythera runs one full-screen game window plus dialogs, and draws its own
// frames and widgets (the T7Widget family), so no window definition procedure is
// invoked here.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mac/heap.h"
#include "mac/qd_region.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "mac/window_mgr.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {

// A WindowRecord begins with its CGrafPort, so the window pointer and its port
// pointer are the same address -- which is exactly why SetPort(window) works.
constexpr u32 kWinHilited     = qd::kCGrafPortSize + 3;
constexpr u32 kWinGoAway      = qd::kCGrafPortSize + 4;
[[maybe_unused]] constexpr u32 kWinSpare       = qd::kCGrafPortSize + 5;
constexpr u32 kWinStrucRgn    = qd::kCGrafPortSize + 6;
constexpr u32 kWinContRgn     = qd::kCGrafPortSize + 10;
constexpr u32 kWinUpdateRgn   = qd::kCGrafPortSize + 14;
[[maybe_unused]] constexpr u32 kWinDefProc     = qd::kCGrafPortSize + 18;
[[maybe_unused]] constexpr u32 kWinDataHandle  = qd::kCGrafPortSize + 22;
constexpr u32 kWinTitleHandle = qd::kCGrafPortSize + 26;
[[maybe_unused]] constexpr u32 kWinTitleWidth  = qd::kCGrafPortSize + 30;
constexpr u32 kWinNextWindow  = qd::kCGrafPortSize + 36;
[[maybe_unused]] constexpr u32 kWinPicture     = qd::kCGrafPortSize + 40;
constexpr u32 kWinRefCon      = qd::kCGrafPortSize + 44;

// Front to back.
std::vector<GuestAddr> g_windows;

void relinkList(Mem& m) {
  for (size_t i = 0; i < g_windows.size(); ++i) {
    GuestAddr next = (i + 1 < g_windows.size()) ? g_windows[i + 1] : 0;
    m.w32(g_windows[i] + kWinNextWindow, next);
  }
}

Rect windowRect(Mem& m, GuestAddr w) {
  return readRect(m, w + qd::kPortRect);
}

// Recomputes every window's visible region: its own rectangle minus the
// rectangles of all the visible windows in front of it.
void recomputeVisRegions(Toolbox& tb) {
  Mem& m = tb.mem();
  RectList covered;
  for (GuestAddr w : g_windows) {
    if (!m.r8(w + kWinVisible)) continue;
    GuestAddr vis = m.r32(w + qd::kPortVisRgn);
    RectList own{windowRect(m, w)};
    if (vis) Regions::get().setShape(m, vis, rectDifference(own, covered));
    covered = rectUnion(covered, own);
  }
}

}  // namespace

// Shared with the Dialog Manager, which builds a DialogRecord by extending a
// WindowRecord: a dialogue *is* a window, and creating one any other way would
// leave it out of g_windows and so out of the visible-region computation.
GuestAddr createWindow(Toolbox& tb, const Rect& bounds, const std::string& title,
                       bool visible, s16 procId, GuestAddr behind, bool goAway,
                       s32 refCon, u32 recordSize) {
  Mem& m = tb.mem();
  GuestAddr w = tb.heap().newPtr(
      recordSize < kWindowRecordSize ? kWindowRecordSize : recordSize, true);
  if (!w) return 0;

  // The embedded port: same PixMap as the screen, so drawing lands on screen.
  m.w16(w + qd::kPortVersion, 0xC000);
  m.w32(w + qd::kPortPixMap, graphics().screenPixMapH);
  writeRect(m, w + qd::kPortRect, bounds);
  m.w16(w + qd::kPortPnSize + 0, 1);
  m.w16(w + qd::kPortPnSize + 2, 1);
  m.w16(w + qd::kPortPnMode, 8);
  m.w16(w + qd::kPortTxSize, 12);
  m.w32(w + qd::kPortFgColor, 33);
  m.w32(w + qd::kPortBkColor, 30);
  for (int i = 0; i < 3; ++i)
    m.w16(w + qd::kPortRgbBkColor + u32(i) * 2, 0xFFFF);

  GuestAddr vis = Regions::get().create(tb);
  Regions::get().setShape(m, vis, visible ? RectList{bounds} : RectList{});
  GuestAddr clip = Regions::get().create(tb);
  Regions::get().setShape(m, clip, RectList{Rect{-32767, -32767, 32767, 32767}});
  m.w32(w + qd::kPortVisRgn, vis);
  m.w32(w + qd::kPortClipRgn, clip);

  m.w16(w + kWinKind, 8);                    // userKind
  m.w8(w + kWinVisible, visible ? 1 : 0);
  m.w8(w + kWinGoAway, goAway ? 1 : 0);
  m.w32(w + kWinRefCon, u32(refCon));
  GuestAddr strucRgn = Regions::get().create(tb);
  Regions::get().setShape(m, strucRgn, RectList{bounds});
  GuestAddr contRgn = Regions::get().create(tb);
  Regions::get().setShape(m, contRgn, RectList{bounds});
  GuestAddr updRgn = Regions::get().create(tb);
  // A new window needs its whole content drawn, which is what the first update
  // event reports.
  Regions::get().setShape(m, updRgn, visible ? RectList{bounds} : RectList{});
  m.w32(w + kWinStrucRgn, strucRgn);
  m.w32(w + kWinContRgn, contRgn);
  m.w32(w + kWinUpdateRgn, updRgn);
  (void)procId;

  if (!title.empty()) {
    GuestAddr th = tb.heap().newHandle(u32(title.size()) + 1, true);
    if (th) m.writePstr(m.r32(th), title, u32(title.size()) + 1);
    m.w32(w + kWinTitleHandle, th);
  }

  // behind == -1 means front; 0 means back.
  if (std::getenv("CYT_DEBUG_QD"))
    std::fprintf(stderr,
                 "  [QD] window %08x bounds(%d,%d,%d,%d) visible=%d "
                 "behind=%08x title=\"%s\"\n",
                 w, bounds.top, bounds.left, bounds.bottom, bounds.right,
                 visible ? 1 : 0, behind, title.c_str());
  if (behind == 0xFFFFFFFFu) g_windows.insert(g_windows.begin(), w);
  else if (behind == 0) g_windows.push_back(w);
  else {
    auto it = std::find(g_windows.begin(), g_windows.end(), behind);
    g_windows.insert(it == g_windows.end() ? g_windows.end() : it + 1, w);
  }
  relinkList(m);
  recomputeVisRegions(tb);
  return w;
}

void disposeWindow(Toolbox& tb, GuestAddr w) {
  if (!w) return;
  auto it = std::find(g_windows.begin(), g_windows.end(), w);
  if (it != g_windows.end()) g_windows.erase(it);
  for (u32 f : {qd::kPortVisRgn, qd::kPortClipRgn}) {
    GuestAddr r = tb.mem().r32(w + f);
    if (r) Regions::get().destroy(tb, r);
  }
  for (u32 f : {kWinStrucRgn, kWinContRgn, kWinUpdateRgn}) {
    GuestAddr r = tb.mem().r32(w + f);
    if (r) Regions::get().destroy(tb, r);
  }
  GuestAddr th = tb.mem().r32(w + kWinTitleHandle);
  if (th) tb.heap().disposeHandle(th);
  if (graphics().currentPort == w)
    graphics().currentPort = graphics().wMgrPort;
  tb.heap().disposePtr(w);
  relinkList(tb.mem());
  recomputeVisRegions(tb);
}

void registerWindowManager(Toolbox& tb) {
  tb.add("NewCWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                              // storage; this port always allocates
    Rect bounds = readRect(tb.mem(), a.ptr());
    std::string title = tb.mem().pstr(a.ptr());
    bool visible = a.u8v() != 0;
    s16 procId = a.s16v();
    GuestAddr behind = a.ptr();
    bool goAway = a.u8v() != 0;
    s32 refCon = a.s32v();
    GuestAddr w = createWindow(tb, bounds, title, visible, procId, behind,
                              goAway, refCon);
    Toolbox::ret(c, w);
  });
  tb.add("NewWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Rect bounds = readRect(tb.mem(), a.ptr());
    std::string title = tb.mem().pstr(a.ptr());
    bool visible = a.u8v() != 0;
    s16 procId = a.s16v();
    GuestAddr behind = a.ptr();
    bool goAway = a.u8v() != 0;
    s32 refCon = a.s32v();
    Toolbox::ret(c, createWindow(tb, bounds, title, visible, procId, behind,
                                 goAway, refCon));
  });
  auto fromResource = [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    a.ptr(); a.ptr();
    std::vector<u8> d;
    // WIND: boundsRect, procID, visible, filler, goAwayFlag, filler, refCon,
    // then the title as a Pascal string.
    if (!peekResource(resType("WIND"), id, &d) || d.size() < 18) {
      Toolbox::ret(c, 0);
      return;
    }
    auto be16 = [&](u32 o) { return s16(u16(d[o]) << 8 | d[o + 1]); };
    auto be32 = [&](u32 o) {
      return s32(u32(d[o]) << 24 | u32(d[o+1]) << 16 | u32(d[o+2]) << 8 | d[o+3]);
    };
    Rect bounds{be16(0), be16(2), be16(4), be16(6)};
    s16 procId = be16(8);
    bool visible = d[10] != 0;
    bool goAway = d[12] != 0;
    s32 refCon = be32(14);
    std::string title;
    if (d.size() > 18) {
      u32 n = d[18];
      if (19 + n <= d.size())
        title.assign(reinterpret_cast<const char*>(&d[19]), n);
    }
    Toolbox::ret(c, createWindow(tb, bounds, title, visible, procId,
                                 0xFFFFFFFFu, goAway, refCon));
  };
  tb.add("GetNewCWindow", fromResource);
  tb.add("GetNewWindow", fromResource);

  tb.add("DisposeWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    disposeWindow(tb, a.ptr());
  });
  tb.add("CloseWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    auto it = std::find(g_windows.begin(), g_windows.end(), w);
    if (it != g_windows.end()) g_windows.erase(it);
    relinkList(tb.mem());
    recomputeVisRegions(tb);
  });

  tb.add("FrontWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    for (GuestAddr w : g_windows)
      if (tb.mem().r8(w + kWinVisible)) { Toolbox::ret(c, w); return; }
    Toolbox::ret(c, 0);
  });
  tb.add("LMGetWindowList", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, g_windows.empty() ? 0 : g_windows.front());
  });
  tb.add("SelectWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    auto it = std::find(g_windows.begin(), g_windows.end(), w);
    if (it == g_windows.end()) return;
    g_windows.erase(it);
    g_windows.insert(g_windows.begin(), w);
    tb.mem().w8(w + kWinHilited, 1);
    relinkList(tb.mem());
    recomputeVisRegions(tb);
  });
  tb.add("BringToFront", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    auto it = std::find(g_windows.begin(), g_windows.end(), w);
    if (it == g_windows.end()) return;
    g_windows.erase(it);
    g_windows.insert(g_windows.begin(), w);
    relinkList(tb.mem());
    recomputeVisRegions(tb);
  });
  tb.add("SendBehind", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr(), behind = a.ptr();
    auto it = std::find(g_windows.begin(), g_windows.end(), w);
    if (it == g_windows.end()) return;
    g_windows.erase(it);
    auto b = std::find(g_windows.begin(), g_windows.end(), behind);
    g_windows.insert(b == g_windows.end() ? g_windows.end() : b + 1, w);
    relinkList(tb.mem());
    recomputeVisRegions(tb);
  });

  auto setVisible = [](bool visible) {
    return [visible](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr w = a.ptr();
      if (!w) return;
      tb.mem().w8(w + kWinVisible, visible ? 1 : 0);
      if (visible) {
        GuestAddr upd = tb.mem().r32(w + kWinUpdateRgn);
        if (upd)
          Regions::get().setShape(tb.mem(), upd,
                                  RectList{readRect(tb.mem(), w + qd::kPortRect)});
      }
      recomputeVisRegions(tb);
    };
  };
  tb.add("ShowWindow", setVisible(true));
  tb.add("HideWindow", setVisible(false));
  tb.add("ShowHide", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    bool show = a.u8v() != 0;
    if (w) tb.mem().w8(w + kWinVisible, show ? 1 : 0);
    recomputeVisRegions(tb);
  });
  tb.add("HiliteWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    bool on = a.u8v() != 0;
    if (w) tb.mem().w8(w + kWinHilited, on ? 1 : 0);
  });

  auto moveOrSize = [](bool sizing) {
    return [sizing](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr w = a.ptr();
      s16 h = a.s16v(), v = a.s16v();
      a.u8v();
      if (!w) return;
      Rect r = readRect(tb.mem(), w + qd::kPortRect);
      if (sizing) { r.right = s16(r.left + h); r.bottom = s16(r.top + v); }
      else { r.right = s16(r.right + (h - r.left)); r.bottom = s16(r.bottom + (v - r.top));
             r.left = h; r.top = v; }
      writeRect(tb.mem(), w + qd::kPortRect, r);
      for (u32 f : {kWinStrucRgn, kWinContRgn}) {
        GuestAddr rgn = tb.mem().r32(w + f);
        if (rgn) Regions::get().setShape(tb.mem(), rgn, RectList{r});
      }
      recomputeVisRegions(tb);
    };
  };
  tb.add("MoveWindow", moveOrSize(false));
  tb.add("SizeWindow", moveOrSize(true));

  // Update accounting: BeginUpdate narrows the visible region to the part that
  // needs redrawing, EndUpdate restores it and clears the update region.
  tb.add("BeginUpdate", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    if (!w) return;
    GuestAddr vis = tb.mem().r32(w + qd::kPortVisRgn);
    GuestAddr upd = tb.mem().r32(w + kWinUpdateRgn);
    if (vis && upd)
      Regions::get().setShape(tb.mem(), vis,
                              rectIntersect(Regions::get().shape(vis),
                                            Regions::get().shape(upd)));
  });
  tb.add("EndUpdate", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    if (!w) return;
    GuestAddr upd = tb.mem().r32(w + kWinUpdateRgn);
    if (upd) Regions::get().setShape(tb.mem(), upd, RectList{});
    recomputeVisRegions(tb);
  });
  auto invalidate = [](bool valid) {
    return [valid](Toolbox& tb, PpcCpu& c, Args& a) {
      Rect r = readRect(tb.mem(), a.ptr());
      GuestAddr w = graphics().currentPort;
      if (!w) return;
      GuestAddr upd = tb.mem().r32(w + kWinUpdateRgn);
      if (!upd || !Regions::get().known(upd)) return;
      Regions::get().setShape(
          tb.mem(), upd,
          valid ? rectDifference(Regions::get().shape(upd), RectList{r})
                : rectUnion(Regions::get().shape(upd), RectList{r}));
    };
  };
  tb.add("InvalRect", invalidate(false));
  tb.add("ValidRect", invalidate(true));
  auto invalidateRgn = [](bool valid) {
    return [valid](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr rgn = a.ptr();
      GuestAddr w = graphics().currentPort;
      if (!w) return;
      GuestAddr upd = tb.mem().r32(w + kWinUpdateRgn);
      if (!upd || !Regions::get().known(upd)) return;
      Regions::get().setShape(
          tb.mem(), upd,
          valid ? rectDifference(Regions::get().shape(upd),
                                 Regions::get().shape(rgn))
                : rectUnion(Regions::get().shape(upd),
                            Regions::get().shape(rgn)));
    };
  };
  tb.add("InvalRgn", invalidateRgn(false));
  tb.add("ValidRgn", invalidateRgn(true));

  tb.add("FindWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 pt = a.u32v();
    GuestAddr out = a.ptr();
    const s16 v = s16(pt >> 16), h = s16(pt & 0xFFFF);
    for (GuestAddr w : g_windows) {
      if (!tb.mem().r8(w + kWinVisible)) continue;
      Rect r = readRect(tb.mem(), w + qd::kPortRect);
      if (h >= r.left && h < r.right && v >= r.top && v < r.bottom) {
        if (out) tb.mem().w32(out, w);
        Toolbox::ret(c, 3);   // inContent
        return;
      }
    }
    if (out) tb.mem().w32(out, 0);
    Toolbox::ret(c, v < 20 ? 1u : 0u);   // inMenuBar, else inDesk
  });

  tb.add("GetWRefCon", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    Toolbox::ret(c, w ? tb.mem().r32(w + kWinRefCon) : 0);
  });
  tb.add("SetWRefCon", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    u32 v = a.u32v();
    if (w) tb.mem().w32(w + kWinRefCon, v);
  });
  tb.add("GetWTitle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr(), out = a.ptr();
    GuestAddr th = w ? tb.mem().r32(w + kWinTitleHandle) : 0;
    std::string t = th ? tb.mem().pstr(tb.mem().r32(th)) : std::string();
    if (out) tb.mem().writePstr(out, t, 256);
  });
  tb.add("SetWTitle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr w = a.ptr();
    std::string t = tb.mem().pstr(a.ptr());
    if (!w) return;
    GuestAddr th = tb.mem().r32(w + kWinTitleHandle);
    if (!th) {
      th = tb.heap().newHandle(u32(t.size()) + 1, true);
      tb.mem().w32(w + kWinTitleHandle, th);
    } else {
      tb.heap().setHandleSize(th, u32(t.size()) + 1);
    }
    if (th) tb.mem().writePstr(tb.mem().r32(th), t, u32(t.size()) + 1);
  });

  // Desktop painting and visibility recalculation are driven by the window list,
  // which is already maintained above.
  tb.add("PaintBehind", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr();
    recomputeVisRegions(tb);
  });
  tb.add("CalcVisBehind", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr();
    recomputeVisRegions(tb);
  });
  tb.add("LMSetMBarHeight", [](Toolbox& tb, PpcCpu& c, Args& a) { a.s16v(); });
  tb.add("LMSetPaintWhite", [](Toolbox& tb, PpcCpu& c, Args& a) { a.u8v(); });
  tb.add("CheckUpdate", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Toolbox::ret(c, 0);
  });
  // Window dragging and zooming need live mouse tracking, which arrives with the
  // front end; Cythera's own window is full-screen and immovable regardless.
  for (const char* track : {"DragWindow", "TrackBox", "TrackGoAway",
                            "GrowWindow", "ZoomWindow", "DragGrayRgn"})
    tb.add(track, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });
}

}  // namespace cyt
