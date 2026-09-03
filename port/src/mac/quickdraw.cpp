#include "mac/quickdraw.h"

#include <cstdio>
#include <string>
#include <vector>

#include "mac/heap.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {
GraphicsWorld g_gw;

void writeRect(Mem& m, GuestAddr p, s16 top, s16 left, s16 bottom, s16 right) {
  m.w16(p + 0, u16(top));
  m.w16(p + 2, u16(left));
  m.w16(p + 4, u16(bottom));
  m.w16(p + 6, u16(right));
}

// Builds the screen's colour table. Cythera ships its own 256-entry palette as
// 'clut' 256, and using it verbatim means indexed pixel values the game writes
// mean exactly what the game intends.
GuestAddr buildColorTable(Toolbox& tb, std::string* err) {
  std::vector<u8> clut;
  const bool haveClut = peekResource(resType("clut"), 256, &clut) &&
                        clut.size() >= qd::kCtTable;

  u32 entries = 256;
  if (haveClut) {
    u16 ctSize = u16(u16(clut[qd::kCtSize]) << 8 | clut[qd::kCtSize + 1]);
    entries = u32(ctSize) + 1;
    if (entries == 0 || entries > 256) entries = 256;
  }

  const u32 size = qd::kCtTable + entries * qd::kColorSpecSize;
  GuestAddr h = tb.heap().newHandle(size, true);
  if (!h) {
    if (err) *err = "no room for the screen colour table";
    return 0;
  }
  GuestAddr t = tb.mem().r32(h);

  if (haveClut && clut.size() >= size) {
    tb.mem().copyIn(t, clut.data(), size);
  } else {
    // Fall back to a grey ramp so the screen is at least coherent.
    tb.mem().w32(t + qd::kCtSeed, 0);
    tb.mem().w16(t + qd::kCtFlags, 0);
    tb.mem().w16(t + qd::kCtSize, u16(entries - 1));
    for (u32 i = 0; i < entries; ++i) {
      GuestAddr e = t + qd::kCtTable + i * qd::kColorSpecSize;
      u16 v = u16((i * 65535) / (entries - 1));
      tb.mem().w16(e + 0, u16(i));
      tb.mem().w16(e + 2, v);
      tb.mem().w16(e + 4, v);
      tb.mem().w16(e + 6, v);
    }
    if (err) *err = "clut 256 not found; using a grey ramp";
  }
  // ctFlags bit 15 set marks a device colour table, which is what a screen has.
  tb.mem().w16(t + qd::kCtFlags, 0x8000);
  return h;
}

GuestAddr buildScreenPixMap(Toolbox& tb, GuestAddr ctabH, std::string* err) {
  // rowBytes is padded to a multiple of four, as QuickDraw required.
  const u32 rowBytes = u32((kScreenWidth * kScreenDepth / 8 + 3) & ~3);
  GuestAddr bits = tb.heap().newPtr(rowBytes * u32(kScreenHeight), true);
  if (!bits) {
    if (err) *err = "no room for the screen framebuffer";
    return 0;
  }
  g_gw.screenBits = bits;
  g_gw.rowBytes = rowBytes;

  GuestAddr h = tb.heap().newHandle(qd::kPixMapSize, true);
  if (!h) {
    if (err) *err = "no room for the screen PixMap";
    return 0;
  }
  GuestAddr p = tb.mem().r32(h);
  Mem& m = tb.mem();
  m.w32(p + qd::kPmBaseAddr, bits);
  // The high bit of rowBytes is what distinguishes a PixMap from a BitMap.
  m.w16(p + qd::kPmRowBytes, u16(0x8000 | rowBytes));
  writeRect(m, p + qd::kPmBounds, 0, 0, kScreenHeight, kScreenWidth);
  m.w16(p + qd::kPmVersion, 0);
  m.w16(p + qd::kPmPackType, 0);
  m.w32(p + qd::kPmPackSize, 0);
  m.w32(p + qd::kPmHRes, 72u << 16);
  m.w32(p + qd::kPmVRes, 72u << 16);
  m.w16(p + qd::kPmPixelType, 0);        // chunky indexed
  m.w16(p + qd::kPmPixelSize, u16(kScreenDepth));
  m.w16(p + qd::kPmCmpCount, 1);
  m.w16(p + qd::kPmCmpSize, u16(kScreenDepth));
  m.w32(p + qd::kPmPlaneBytes, 0);
  m.w32(p + qd::kPmTable, ctabH);
  m.w32(p + qd::kPmReserved, 0);
  return h;
}

// A region: { short rgnSize; Rect rgnBBox; } for the rectangular case.
GuestAddr newRectRegion(Toolbox& tb, s16 t, s16 l, s16 b, s16 r) {
  GuestAddr h = tb.heap().newHandle(10, true);
  if (!h) return 0;
  GuestAddr p = tb.mem().r32(h);
  tb.mem().w16(p, 10);
  writeRect(tb.mem(), p + 2, t, l, b, r);
  return h;
}

GuestAddr buildPort(Toolbox& tb, GuestAddr pixMapH, GuestAddr device) {
  GuestAddr port = tb.heap().newPtr(qd::kCGrafPortSize, true);
  if (!port) return 0;
  Mem& m = tb.mem();
  m.w16(port + qd::kPortDevice, 0);
  m.w32(port + qd::kPortPixMap, pixMapH);
  // portVersion's top two bits are set to mark this as a CGrafPort rather than
  // the older monochrome GrafPort; code branches on exactly that.
  m.w16(port + qd::kPortVersion, 0xC000);
  writeRect(m, port + qd::kPortRect, 0, 0, kScreenHeight, kScreenWidth);
  m.w32(port + qd::kPortVisRgn, newRectRegion(tb, 0, 0, kScreenHeight, kScreenWidth));
  m.w32(port + qd::kPortClipRgn, newRectRegion(tb, -32767, -32767, 32767, 32767));
  m.w16(port + qd::kPortPnSize + 0, 1);
  m.w16(port + qd::kPortPnSize + 2, 1);
  m.w16(port + qd::kPortPnMode, 8);      // patCopy
  m.w16(port + qd::kPortPnVis, 0);
  m.w16(port + qd::kPortTxFont, 0);      // the system font
  m.w16(port + qd::kPortTxSize, 12);
  m.w16(port + qd::kPortTxMode, 1);      // srcOr
  m.w32(port + qd::kPortFgColor, 33);    // blackColor
  m.w32(port + qd::kPortBkColor, 30);    // whiteColor
  // rgbFgColor black, rgbBkColor white.
  for (int i = 0; i < 3; ++i) {
    m.w16(port + qd::kPortRgbFgColor + u32(i) * 2, 0);
    m.w16(port + qd::kPortRgbBkColor + u32(i) * 2, 0xFFFF);
  }
  (void)device;
  return port;
}

}  // namespace

GraphicsWorld& graphics() { return g_gw; }

bool initGraphicsWorld(Toolbox& tb, std::string* err) {
  std::string note;
  g_gw.colorTableH = buildColorTable(tb, &note);
  if (!g_gw.colorTableH) { if (err) *err = note; return false; }
  if (!note.empty()) std::fprintf(stderr, "  [QuickDraw] %s\n", note.c_str());

  g_gw.screenPixMapH = buildScreenPixMap(tb, g_gw.colorTableH, err);
  if (!g_gw.screenPixMapH) return false;

  // The main GDevice.
  GuestAddr gdH = tb.heap().newHandle(qd::kGDeviceSize, true);
  if (!gdH) { if (err) *err = "no room for the main GDevice"; return false; }
  GuestAddr gd = tb.mem().r32(gdH);
  Mem& m = tb.mem();
  m.w16(gd + qd::kGdRefNum, u16(s16(-1)));
  m.w16(gd + qd::kGdID, 0);
  m.w16(gd + qd::kGdType, 0);            // an indexed-colour screen
  m.w16(gd + qd::kGdResPref, 4);
  m.w16(gd + qd::kGdFlags,
        u16(qd::kGdDevTypeBit | qd::kRamInitBit | qd::kMainScreenBit |
            qd::kAllInitBit | qd::kScreenDeviceBit | qd::kScreenActiveBit));
  m.w32(gd + qd::kGdPMap, g_gw.screenPixMapH);
  m.w32(gd + qd::kGdNextGD, 0);          // a single screen
  writeRect(m, gd + qd::kGdRect, 0, 0, kScreenHeight, kScreenWidth);
  m.w32(gd + qd::kGdMode, 0x80);
  g_gw.mainDevice = gdH;
  g_gw.currentDevice = gdH;

  g_gw.wMgrPort = buildPort(tb, g_gw.screenPixMapH, gdH);
  if (!g_gw.wMgrPort) { if (err) *err = "no room for the Window Manager port"; return false; }
  g_gw.currentPort = g_gw.wMgrPort;

  // The desktop region: the screen minus the menu bar.
  g_gw.grayRgn = newRectRegion(tb, 20, 0, kScreenHeight, kScreenWidth);
  return true;
}

// ---------------------------------------------------------------------------
// Device and port queries
// ---------------------------------------------------------------------------

void registerQuickDrawDevices(Toolbox& tb) {
  auto mainDevice = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, g_gw.mainDevice);
  };
  tb.add("GetMainDevice", mainDevice);
  tb.add("GetDeviceList", mainDevice);   // one screen, so the list is just it
  tb.add("GetMaxDevice", mainDevice);
  tb.add("GetNextDevice", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Toolbox::ret(c, 0);                  // no second screen
  });
  tb.add("GetGDevice", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, g_gw.currentDevice);
  });
  tb.add("SetGDevice", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr d = a.ptr();
    if (d) g_gw.currentDevice = d;
  });
  tb.add("TestDeviceAttribute", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr gdH = a.ptr();
    s16 attribute = a.s16v();
    if (!gdH || attribute < 0 || attribute > 15) { Toolbox::ret(c, 0); return; }
    GuestAddr gd = tb.mem().r32(gdH);
    u16 flags = tb.mem().r16(gd + qd::kGdFlags);
    Toolbox::ret(c, (flags >> u32(attribute)) & 1);
  });
  tb.add("HasDepth", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    s16 depth = a.s16v();
    a.s16v(); a.s16v();
    // The screen is eight bits deep and cannot be switched, which is exactly
    // what the game needs; report only that depth as available.
    Toolbox::ret(c, depth == kScreenDepth ? 1u : 0u);
  });
  tb.add("SetDepth", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    s16 depth = a.s16v();
    a.s16v(); a.s16v();
    Toolbox::ret(c, depth == kScreenDepth ? 0u : u32(s32(-11)));
  });
  tb.add("GetCWMgrPort", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, g_gw.wMgrPort);
  });
  tb.add("GetWMgrPort", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, g_gw.wMgrPort);
  });
  tb.add("GetGrayRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, g_gw.grayRgn);
  });
  tb.add("LMGetGrayRgn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, g_gw.grayRgn);
  });
  tb.add("GetGray", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr(); a.ptr();
    Toolbox::ret(c, 0);
  });
  tb.add("GetPixBaseAddr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr pmH = a.ptr();
    if (!pmH) { Toolbox::ret(c, 0); return; }
    Toolbox::ret(c, tb.mem().r32(tb.mem().r32(pmH) + qd::kPmBaseAddr));
  });
  tb.add("GetGWorldPixMap", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr port = a.ptr();
    if (!port) { Toolbox::ret(c, 0); return; }
    Toolbox::ret(c, tb.mem().r32(port + qd::kPortPixMap));
  });
  // Pixel locking is meaningless while nothing relocates image data.
  // PixPatChanged is the Colour QuickDraw drawing layer's; see qd_draw.cpp.
  for (const char* nop : {"LockPixels", "UnlockPixels", "SetPixelsState",
                          "AllowPurgePixels", "NoPurgePixels", "PortChanged",
                          "CTabChanged"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 1); });
  tb.add("GetPixelsState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
