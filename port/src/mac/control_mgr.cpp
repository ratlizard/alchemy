// Control Manager.
//
// Cythera draws almost all of its own widgets through the T7Widget class
// family, so this manager is thinner than it looks: what actually reaches it
// are the four inline controls in the character-creation dialogue's item list
// -- two push buttons and two radio buttons -- plus the accessors the
// application uses to read and write their state.
//
// Two things about that are worth writing down, because they decide how much
// drawing belongs here.
//
// The game's own CNTL resources use procID 16000, which is CDEF 1000 patched at
// run time to point into the PEF (see TCDEFRegister in POWERPC-NOTES.md). Those could
// be drawn by calling the game's own code. But the controls in DITL 133 are
// *not* CNTL references -- they are inline dialogue items, so the Dialog
// Manager creates them with the standard procIDs 0, 1 and 2, and there is
// nothing in the game's fork that draws those. They have to be drawn here.
//
// So this file keeps a control's state exactly, and does not draw yet: nothing
// reaches the Control Manager today except while the dialogue is still
// invisible. See the drawing section below for what remains and why it is
// deferred rather than half-written.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mac/heap.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "mac/window_mgr.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {

// ControlRecord, in the classic layout. The title is a Str255 at the end, so
// the record is a fixed 296 bytes.
constexpr u32 kCtlNext    = 0;
constexpr u32 kCtlOwner   = 4;
constexpr u32 kCtlRect    = 8;
constexpr u32 kCtlVis     = 16;
// contrlVis is 255 when the control is visible, not 1. Inside Macintosh:
// Macintosh Toolbox Essentials, page 5-126: "contrlVis: Byte; {255 if
// visible}". This port wrote 1 at first, which every "is it visible" test that
// compares against 0 would accept and every test that compares against 255
// would silently reject.
constexpr u8 kCtlVisible = 255;
constexpr u32 kCtlHilite  = 17;
constexpr u32 kCtlValue   = 18;
constexpr u32 kCtlMin     = 20;
constexpr u32 kCtlMax     = 22;
constexpr u32 kCtlDefProc = 24;
constexpr u32 kCtlData    = 28;
constexpr u32 kCtlAction  = 32;
constexpr u32 kCtlRefCon  = 36;
constexpr u32 kCtlTitle   = 40;
constexpr u32 kControlRecordSize = kCtlTitle + 256;

// The three standard definition procedures, as procID variation codes. Unused
// until drawing lands, and kept here because working out which is which is the
// part that takes the time.
[[maybe_unused]] constexpr s16 kPushButProc = 0;
[[maybe_unused]] constexpr s16 kCheckBoxProc = 1;
[[maybe_unused]] constexpr s16 kRadioButProc = 2;

// A ControlHandle is a real handle: the application dereferences it, and
// TCreatePlayerDialog stores one straight from GetDialogItem.
GuestAddr ctlBody(Mem& m, GuestAddr h) {
  if (!h) return 0;
  return m.r32(h);
}

[[maybe_unused]] s16 procVariation(Mem& m, GuestAddr body) {
  // contrlDefProc holds the procID for controls this port created, rather than
  // a real definition-procedure handle. Only the low nibble varies among the
  // standard three.
  return s16(m.r32(body + kCtlDefProc) & 0x0F);
}

void linkControl(Toolbox& tb, GuestAddr owner, GuestAddr h) {
  Mem& m = tb.mem();
  if (!owner) return;
  // Controls hang off the window in creation order, which is the order
  // DrawControls has to paint them in.
  GuestAddr prev = 0, cur = m.r32(owner + kWinControlList);
  while (cur) { prev = cur; cur = m.r32(ctlBody(m, cur) + kCtlNext); }
  if (prev) m.w32(ctlBody(m, prev) + kCtlNext, h);
  else m.w32(owner + kWinControlList, h);
  m.w32(ctlBody(m, h) + kCtlNext, 0);
}

void unlinkControl(Toolbox& tb, GuestAddr h) {
  Mem& m = tb.mem();
  GuestAddr body = ctlBody(m, h);
  if (!body) return;
  GuestAddr owner = m.r32(body + kCtlOwner);
  if (!owner) return;
  GuestAddr prev = 0, cur = m.r32(owner + kWinControlList);
  while (cur && cur != h) { prev = cur; cur = m.r32(ctlBody(m, cur) + kCtlNext); }
  if (!cur) return;
  const GuestAddr next = m.r32(body + kCtlNext);
  if (prev) m.w32(ctlBody(m, prev) + kCtlNext, next);
  else m.w32(owner + kWinControlList, next);
}

GuestAddr newControl(Toolbox& tb, GuestAddr owner, const Rect& bounds,
                     const std::string& title, bool visible, s16 value,
                     s16 min, s16 max, s16 procId, s32 refCon) {
  Mem& m = tb.mem();
  GuestAddr h = tb.heap().newHandle(kControlRecordSize, true);
  if (!h) return 0;
  GuestAddr body = m.r32(h);
  m.w32(body + kCtlNext, 0);
  m.w32(body + kCtlOwner, owner);
  writeRect(m, body + kCtlRect, bounds);
  m.w8(body + kCtlVis, visible ? kCtlVisible : 0);
  m.w8(body + kCtlHilite, 0);
  m.w16(body + kCtlValue, u16(value));
  m.w16(body + kCtlMin, u16(min));
  m.w16(body + kCtlMax, u16(max));
  m.w32(body + kCtlDefProc, u32(u16(procId)));
  m.w32(body + kCtlData, 0);
  m.w32(body + kCtlAction, 0);
  m.w32(body + kCtlRefCon, u32(refCon));
  m.writePstr(body + kCtlTitle, title, 256);
  linkControl(tb, owner, h);
  if (std::getenv("CYT_DEBUG_DIALOG"))
    std::fprintf(stderr,
                 "  [Control] %08x on %08x (%d,%d,%d,%d) procID %d value %d "
                 "\"%s\"\n", h, owner, bounds.top, bounds.left, bounds.bottom,
                 bounds.right, procId, value, title.c_str());
  return h;
}

// ---- drawing ---------------------------------------------------------------
//
// Not yet. A control is drawn by DrawDialog, and DrawDialog is not implemented
// -- see the header comment in dialog_mgr.cpp. Everything that reaches the
// Control Manager today happens while the dialogue is still invisible:
// TCreatePlayerDialog builds its items, sets "Male" with SetControlValue and
// only then calls ShowWindow.
//
// When it is time, the three shapes are a framed rectangle with centred text
// (push button) and a 12x12 mark box with the title beside it (check box and
// radio button). The blocker is not the shapes: fillRect and frameRect are
// file-local to qd_draw.cpp and the text calls take a TextStyle plus a byte
// range rather than a std::string, so drawing from here means exporting a
// small drawing interface from QuickDraw first. That is worth doing once, for
// the Dialog, Control, List and Menu Managers together, rather than four
// times. Note also that ovals are still approximated by their bounding
// rectangle, so a radio button will be square until that is fixed.
void drawControl(Toolbox& tb, GuestAddr h) {
  (void)tb;
  (void)h;
}

}  // namespace

void drawWindowControls(Toolbox& tb, GuestAddr owner) {
  Mem& m = tb.mem();
  if (!owner) return;
  for (GuestAddr h = m.r32(owner + kWinControlList); h;
       h = m.r32(ctlBody(m, h) + kCtlNext))
    drawControl(tb, h);
}

void registerControlManager(Toolbox& tb) {
  tb.add("NewControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr owner = a.ptr();
    const Rect bounds = readRect(tb.mem(), a.ptr());
    const std::string title = tb.mem().pstr(a.ptr());
    const bool visible = a.u8v() != 0;
    const s16 value = a.s16v();
    const s16 min = a.s16v();
    const s16 max = a.s16v();
    const s16 procId = a.s16v();
    const s32 refCon = a.s32v();
    Toolbox::ret(c, newControl(tb, owner, bounds, title, visible, value, min,
                               max, procId, refCon));
  });

  // GetNewControl reads a CNTL: rect, value, visible, filler, max, min,
  // procID, refCon, title.
  tb.add("GetNewControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 id = a.s16v();
    const GuestAddr owner = a.ptr();
    std::vector<u8> d;
    if (!peekResource(resType("CNTL"), id, &d) || d.size() < 22) {
      Toolbox::ret(c, 0);
      return;
    }
    auto be16 = [&](u32 o) { return s16(u16(d[o]) << 8 | d[o + 1]); };
    auto be32 = [&](u32 o) {
      return s32(u32(d[o]) << 24 | u32(d[o+1]) << 16 | u32(d[o+2]) << 8 | d[o+3]);
    };
    const Rect bounds{be16(0), be16(2), be16(4), be16(6)};
    const s16 value = be16(8);
    const bool visible = d[10] != 0;
    const s16 max = be16(12), min = be16(14), procId = be16(16);
    const s32 refCon = be32(18);
    std::string title;
    if (d.size() > 22) {
      const u32 n = d[22];
      if (23 + n <= d.size())
        title.assign(reinterpret_cast<const char*>(&d[23]), n);
    }
    Toolbox::ret(c, newControl(tb, owner, bounds, title, visible, value, min,
                               max, procId, refCon));
  });

  tb.add("DisposeControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr h = a.ptr();
    if (!h) return;
    unlinkControl(tb, h);
    tb.heap().disposeHandle(h);
  });
  tb.add("KillControls", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr owner = a.ptr();
    if (!owner) return;
    GuestAddr h = m.r32(owner + kWinControlList);
    while (h) {
      const GuestAddr next = m.r32(ctlBody(m, h) + kCtlNext);
      tb.heap().disposeHandle(h);
      h = next;
    }
    m.w32(owner + kWinControlList, 0);
  });

  // ---- accessors ----------------------------------------------------------
  // Each is a field read or write, and each has to tolerate a null handle:
  // the application asks about dialogue items generically, and a userItem's
  // "handle" is a drawing routine, not a control.
  auto getShort = [](u32 field) {
    return [field](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr body = ctlBody(tb.mem(), a.ptr());
      Toolbox::ret(c, body ? u32(u16(tb.mem().r16(body + field))) : 0);
    };
  };
  auto setShort = [](u32 field) {
    return [field](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr body = ctlBody(tb.mem(), a.ptr());
      const s16 v = a.s16v();
      if (body) tb.mem().w16(body + field, u16(v));
    };
  };
  tb.add("GetControlValue", getShort(kCtlValue));
  tb.add("GetControlMinimum", getShort(kCtlMin));
  tb.add("GetControlMaximum", getShort(kCtlMax));
  tb.add("SetControlMinimum", setShort(kCtlMin));
  tb.add("SetControlMaximum", setShort(kCtlMax));

  // SetControlValue clamps to the control's range, as the real one does. For a
  // radio button that is min 0, max 1, so the game's SetControlValue(h, 1) to
  // turn "Male" on lands exactly where it should.
  tb.add("SetControlValue", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr h = a.ptr();
    const s16 want = a.s16v();
    GuestAddr body = ctlBody(m, h);
    if (!body) return;
    const s16 lo = s16(m.r16(body + kCtlMin)), hi = s16(m.r16(body + kCtlMax));
    m.w16(body + kCtlValue, u16(std::clamp(want, lo, hi)));
    if (m.r8(body + kCtlVis)) drawControl(tb, h);
  });

  tb.add("GetControlReference", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr body = ctlBody(tb.mem(), a.ptr());
    Toolbox::ret(c, body ? tb.mem().r32(body + kCtlRefCon) : 0);
  });
  tb.add("SetControlReference", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr body = ctlBody(tb.mem(), a.ptr());
    const s32 v = a.s32v();
    if (body) tb.mem().w32(body + kCtlRefCon, u32(v));
  });
  tb.add("GetControlAction", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr body = ctlBody(tb.mem(), a.ptr());
    Toolbox::ret(c, body ? tb.mem().r32(body + kCtlAction) : 0);
  });
  tb.add("SetControlAction", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr body = ctlBody(tb.mem(), a.ptr());
    const GuestAddr v = a.ptr();
    if (body) tb.mem().w32(body + kCtlAction, v);
  });
  tb.add("GetControlTitle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr body = ctlBody(m, a.ptr());
    const GuestAddr out = a.ptr();
    if (!out) return;
    m.writePstr(out, body ? m.pstr(body + kCtlTitle) : std::string(), 256);
  });
  tb.add("SetControlTitle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr h = a.ptr();
    const GuestAddr in = a.ptr();
    GuestAddr body = ctlBody(m, h);
    if (!body || !in) return;
    m.writePstr(body + kCtlTitle, m.pstr(in), 256);
    if (m.r8(body + kCtlVis)) drawControl(tb, h);
  });

  tb.add("GetControlHilite", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr body = ctlBody(tb.mem(), a.ptr());
    Toolbox::ret(c, body ? u32(tb.mem().r8(body + kCtlHilite)) : 0);
  });
  tb.add("HiliteControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr h = a.ptr();
    const s16 state = a.s16v();
    GuestAddr body = ctlBody(m, h);
    if (!body) return;
    m.w8(body + kCtlHilite, u8(state));
    if (m.r8(body + kCtlVis)) drawControl(tb, h);
  });

  auto setVisible = [](bool show) {
    return [show](Toolbox& tb, PpcCpu& c, Args& a) {
      Mem& m = tb.mem();
      const GuestAddr h = a.ptr();
      GuestAddr body = ctlBody(m, h);
      if (!body) return;
      m.w8(body + kCtlVis, show ? kCtlVisible : 0);
      if (show) drawControl(tb, h);
    };
  };
  tb.add("ShowControl", setVisible(true));
  tb.add("HideControl", setVisible(false));

  tb.add("MoveControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr body = ctlBody(m, a.ptr());
    const s16 h = a.s16v(), v = a.s16v();
    if (!body) return;
    Rect r = readRect(m, body + kCtlRect);
    const s16 w = s16(r.right - r.left), ht = s16(r.bottom - r.top);
    writeRect(m, body + kCtlRect, Rect{v, h, s16(v + ht), s16(h + w)});
  });
  tb.add("SizeControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr body = ctlBody(m, a.ptr());
    const s16 w = a.s16v(), h = a.s16v();
    if (!body) return;
    Rect r = readRect(m, body + kCtlRect);
    writeRect(m, body + kCtlRect,
              Rect{r.top, r.left, s16(r.top + h), s16(r.left + w)});
  });

  tb.add("Draw1Control", [](Toolbox& tb, PpcCpu& c, Args& a) {
    drawControl(tb, a.ptr());
  });
  auto drawAll = [](Toolbox& tb, PpcCpu& c, Args& a) {
    drawWindowControls(tb, a.ptr());
  };
  tb.add("DrawControls", drawAll);
  tb.add("UpdateControls", drawAll);

  // FindControl / TestControl report which control a point is in, and which
  // part of it. With only buttons and radio buttons in play there is one part
  // code that matters: 10, inButton, which is also inCheckBox.
  tb.add("FindControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr ptAddr = a.ptr();     // a Point, passed by value in a reg
    const GuestAddr owner = a.ptr();
    const GuestAddr out = a.ptr();
    const s16 v = s16(u16(ptAddr >> 16)), hh = s16(u16(ptAddr & 0xFFFF));
    for (GuestAddr h = owner ? m.r32(owner + kWinControlList) : 0; h;
         h = m.r32(ctlBody(m, h) + kCtlNext)) {
      GuestAddr body = ctlBody(m, h);
      if (!m.r8(body + kCtlVis)) continue;
      if (m.r8(body + kCtlHilite) == 255) continue;      // inactive
      const Rect r = readRect(m, body + kCtlRect);
      if (v < r.top || v >= r.bottom || hh < r.left || hh >= r.right) continue;
      if (out) m.w32(out, h);
      Toolbox::ret(c, 10);
      return;
    }
    if (out) m.w32(out, 0);
    Toolbox::ret(c, 0);
  });
  tb.add("TestControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr body = ctlBody(m, a.ptr());
    const GuestAddr pt = a.ptr();
    if (!body || !m.r8(body + kCtlVis)) { Toolbox::ret(c, 0); return; }
    const s16 v = s16(u16(pt >> 16)), hh = s16(u16(pt & 0xFFFF));
    const Rect r = readRect(m, body + kCtlRect);
    const bool in = v >= r.top && v < r.bottom && hh >= r.left && hh < r.right;
    Toolbox::ret(c, in ? 10 : 0);
  });

  // TrackControl follows the mouse until release. The port has no mouse-down
  // loop to run here yet -- input is scheduled at the frame boundary -- so a
  // click that reached a control is reported as a completed click on it, which
  // is what a dialogue's item handling expects.
  tb.add("TrackControl", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr h = a.ptr();
    a.ptr();                                   // startPoint
    a.ptr();                                   // actionProc
    GuestAddr body = ctlBody(m, h);
    Toolbox::ret(c, body ? 10 : 0);
  });
}

}  // namespace cyt
