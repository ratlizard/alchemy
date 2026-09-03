// QuickDraw's data model, laid out in guest memory exactly as Color QuickDraw
// defined it, because the application walks these structures directly rather
// than only through accessors.
#pragma once

#include "mem.h"

namespace cyt {

class Toolbox;

// ---- structure sizes and field offsets ------------------------------------
namespace qd {

// PixMap: the description of a pixel image.
constexpr u32 kPixMapSize      = 50;
constexpr u32 kPmBaseAddr      = 0;
constexpr u32 kPmRowBytes      = 4;   // high bit set marks a PixMap, not BitMap
constexpr u32 kPmBounds        = 6;   // Rect: top, left, bottom, right
constexpr u32 kPmVersion       = 14;
constexpr u32 kPmPackType      = 16;
constexpr u32 kPmPackSize      = 18;
constexpr u32 kPmHRes          = 22;  // Fixed, 72 dpi
constexpr u32 kPmVRes          = 26;
constexpr u32 kPmPixelType     = 30;  // 0 = indexed, 16 = direct
constexpr u32 kPmPixelSize     = 32;
constexpr u32 kPmCmpCount      = 34;
constexpr u32 kPmCmpSize       = 36;
constexpr u32 kPmPlaneBytes    = 38;
constexpr u32 kPmTable         = 42;  // CTabHandle
constexpr u32 kPmReserved      = 46;

// ColorTable, followed by (ctSize + 1) eight-byte ColorSpec entries.
constexpr u32 kCtSeed          = 0;
constexpr u32 kCtFlags         = 4;
constexpr u32 kCtSize          = 6;   // one less than the entry count
constexpr u32 kCtTable         = 8;
constexpr u32 kColorSpecSize   = 8;   // short value, then RGBColor

// GDevice: one screen.
constexpr u32 kGDeviceSize     = 62;
constexpr u32 kGdRefNum        = 0;
constexpr u32 kGdID            = 2;
constexpr u32 kGdType          = 4;   // 0 = indexed ("clut") device
constexpr u32 kGdITable        = 6;
constexpr u32 kGdResPref       = 10;
constexpr u32 kGdSearchProc    = 12;
constexpr u32 kGdCompProc      = 16;
constexpr u32 kGdFlags         = 20;
constexpr u32 kGdPMap          = 22;  // PixMapHandle
constexpr u32 kGdRefCon        = 26;
constexpr u32 kGdNextGD        = 30;
constexpr u32 kGdRect          = 34;
constexpr u32 kGdMode          = 42;
constexpr u32 kGdCCBytes       = 46;
constexpr u32 kGdCCDepth       = 48;
constexpr u32 kGdCCXData       = 50;
constexpr u32 kGdCCXMask       = 54;
constexpr u32 kGdExt           = 58;

// gdFlags bits, as Inside Macintosh numbers them from the least significant.
constexpr u16 kGdDevTypeBit    = 1u << 0;   // set for a colour device
constexpr u16 kRamInitBit      = 1u << 10;
constexpr u16 kMainScreenBit   = 1u << 11;
constexpr u16 kAllInitBit      = 1u << 12;
constexpr u16 kScreenDeviceBit = 1u << 13;
constexpr u16 kScreenActiveBit = 1u << 15;

// CGrafPort, the colour drawing environment.
constexpr u32 kCGrafPortSize   = 108;
constexpr u32 kPortDevice      = 0;
constexpr u32 kPortPixMap      = 2;
constexpr u32 kPortVersion     = 6;
constexpr u32 kPortGrafVars    = 8;
constexpr u32 kPortChExtra     = 12;
constexpr u32 kPortPnLocHFrac  = 14;
constexpr u32 kPortRect        = 16;
constexpr u32 kPortVisRgn      = 24;
constexpr u32 kPortClipRgn     = 28;
constexpr u32 kPortBkPixPat    = 32;
constexpr u32 kPortRgbFgColor  = 36;
constexpr u32 kPortRgbBkColor  = 42;
constexpr u32 kPortPnLoc       = 48;
constexpr u32 kPortPnSize      = 52;
constexpr u32 kPortPnMode      = 56;
constexpr u32 kPortPnPixPat    = 58;
constexpr u32 kPortFillPixPat  = 62;
constexpr u32 kPortPnVis       = 66;
constexpr u32 kPortTxFont      = 68;
constexpr u32 kPortTxFace      = 70;
constexpr u32 kPortTxMode      = 72;
constexpr u32 kPortTxSize      = 74;
constexpr u32 kPortSpExtra     = 76;
constexpr u32 kPortFgColor     = 80;
constexpr u32 kPortBkColor     = 84;
constexpr u32 kPortColrBit     = 88;
constexpr u32 kPortPatStretch  = 90;
constexpr u32 kPortPicSave     = 92;
constexpr u32 kPortRgnSave     = 96;
constexpr u32 kPortPolySave     = 100;
constexpr u32 kPortGrafProcs   = 104;

}  // namespace qd

// The screen. Cythera requires 640x480 at eight bits or deeper, and drove the
// monitor to exactly that; the port gives it precisely that surface and scales
// on output instead of asking the game to change resolution.
constexpr s16 kScreenWidth  = 640;
constexpr s16 kScreenHeight = 480;
constexpr s16 kScreenDepth  = 8;

// Builds the graphics world -- screen framebuffer, colour table, PixMap,
// GDevice and the Window Manager port -- and returns false with a reason when
// the guest heap cannot hold it.
bool initGraphicsWorld(Toolbox& tb, std::string* err);

// Guest addresses of the pieces other managers need.
struct GraphicsWorld {
  GuestAddr screenBits = 0;     // the 640x480 eight-bit framebuffer
  u32 rowBytes = 0;
  GuestAddr screenPixMapH = 0;  // PixMapHandle
  GuestAddr colorTableH = 0;    // CTabHandle
  GuestAddr mainDevice = 0;     // GDHandle
  GuestAddr wMgrPort = 0;       // CGrafPtr covering the whole screen
  GuestAddr currentPort = 0;
  GuestAddr currentDevice = 0;
  GuestAddr grayRgn = 0;        // the desktop region
};
GraphicsWorld& graphics();

}  // namespace cyt
