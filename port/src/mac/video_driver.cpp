// The screen's video driver, as far as Cythera uses it: gamma.
//
// The start screen's fade is a gamma fade, and finding that out was worth the
// trouble because nothing in a Toolbox call trace shows it. The game asks a
// question first, and if the answer is no it fades by doing nothing at all --
// no palette call, no colour table call, nothing to see. Read statically with
// tools/pefdisasm.py, IsOneGammaAvailable (0x074F28) is:
//
//   1. NGetTrapAddress(0xAA29) vs NGetTrapAddress(0xA89F, _Unimplemented);
//      equal means "no gamma support", and it returns false immediately.
//   2. TestDeviceAttribute(gd, 13) screenDevice, or (gd, 14) noDriver.
//   3. (**gd).gdType must not be 1.
//
// This port passed 2 and 3 and failed 1, because it deliberately answers every
// unimplemented trap with one shared address -- which is right, and is what
// makes the game take its own fallback paths elsewhere. Trap 0xAA29 is now
// answered as available, and that is safe here for a specific reason: the game
// only ever *compares* the address, it never calls through it. What it calls is
// the driver, and that is what this file provides.
//
//   GetDevGammaTable (0x075544)  PBStatusSync,  csCode 8  (cscGetGamma)
//   SetDevGammaTable (0x075624)  PBControlSync, csCode 4  (cscSetGamma)
//
// Both build a 50-byte parameter block by hand, put the driver reference from
// (**gd).gdRefNum at offset 24 and the csCode at offset 26, and pass the gamma
// table pointer in csParam at offset 28.
#include <cstdio>
#include <cstdlib>

#include "mac/heap.h"
#include "mac/quickdraw.h"
#include "mac/video_driver.h"
#include "toolbox.h"

namespace cyt {

namespace {

// CntrlParam / ParamBlockRec offsets, the few that matter.
// Which driver the call is for. Unread: this port has exactly one screen, so
// every gamma call can only be for it, and refusing calls whose refNum did not
// match would mean inventing a refNum the game must agree with.
[[maybe_unused]] constexpr u32 kPbIoCRefNum = 24;
constexpr u32 kPbCsCode    = 26;   // short: what to do
constexpr u32 kPbCsParam   = 28;   // the operation's own arguments

constexpr s16 kCscSetGamma = 4;    // Control
constexpr s16 kCscGetGamma = 8;    // Status

// GammaTbl, as Inside Macintosh: Devices defines it. The data follows the
// formula data, which is empty for a plain table.
constexpr u32 kGtVersion     = 0;
constexpr u32 kGtType        = 2;
constexpr u32 kGtFormulaSize = 4;
constexpr u32 kGtChanCnt     = 6;
constexpr u32 kGtDataCnt     = 8;
constexpr u32 kGtDataWidth   = 10;
constexpr u32 kGtFormulaData = 12;

GammaRamp g_ramp;
GuestAddr g_tableOut = 0;          // the table handed back by cscGetGamma

void makeIdentity() {
  for (u32 i = 0; i < 256; ++i) {
    g_ramp.red[i] = u8(i);
    g_ramp.green[i] = u8(i);
    g_ramp.blue[i] = u8(i);
  }
  g_ramp.identity = true;
}

// Reads a GammaTbl out of guest memory into the host ramp. Channel counts of 1
// and 3 both occur -- one means "the same curve for all three" -- and the data
// width is eight bits for every table this game builds.
// Does this address look like a GammaTbl header rather than something else?
bool plausibleTable(Mem& m, GuestAddr t) {
  if (!t) return false;
  const u16 chanCnt = m.r16(t + kGtChanCnt);
  const u16 dataCnt = m.r16(t + kGtDataCnt);
  const u16 dataWidth = m.r16(t + kGtDataWidth);
  return (chanCnt == 1 || chanCnt == 3) && dataCnt > 0 && dataCnt <= 1024 &&
         dataWidth > 0 && dataWidth <= 8;
}

// Reads a GammaTbl out of guest memory into the host ramp. Channel counts of 1
// and 3 both occur -- one means "the same curve for all three" -- and the data
// width is eight bits for every table this game builds.
//
// What arrives in csParam is a *handle*, not a pointer. Inside Macintosh types
// the field as GammaTblPtr, and Cythera passes what its own caller gave it, so
// both forms are accepted and told apart by whether the header reads as
// plausible. Guessing wrong here is how the fade came out as noise rather than
// as nothing.
bool readGammaTable(Mem& m, GuestAddr t) {
  if (!plausibleTable(m, t)) {
    const GuestAddr deref = t ? m.r32(t) : 0;
    if (!plausibleTable(m, deref)) return false;
    t = deref;
  }
  const u16 formulaSize = m.r16(t + kGtFormulaSize);
  const u16 chanCnt = m.r16(t + kGtChanCnt);
  const u16 dataCnt = m.r16(t + kGtDataCnt);
  const u16 dataWidth = m.r16(t + kGtDataWidth);
  const GuestAddr data = t + kGtFormulaData + formulaSize;
  // A table may be shorter than 256 entries, in which case the input index is
  // scaled onto it; and narrower than eight bits, in which case its values are
  // shifted up. Both are in the format and both are cheap to honour.
  const u32 shift = 8u - dataWidth;
  u8* dst[3] = {g_ramp.red, g_ramp.green, g_ramp.blue};
  for (u32 ch = 0; ch < 3; ++ch) {
    const u32 srcCh = (chanCnt == 1) ? 0 : ch;
    for (u32 i = 0; i < 256; ++i) {
      const u32 idx = (u32(dataCnt) == 256) ? i : (i * dataCnt) / 256;
      const u8 v = m.r8(data + srcCh * dataCnt + idx);
      dst[ch][i] = u8(v << shift);
    }
  }
  // Noting an identity ramp lets the presenter skip the lookup entirely, which
  // matters because it runs over every pixel of every frame.
  bool same = true;
  for (u32 i = 0; i < 256 && same; ++i)
    same = g_ramp.red[i] == i && g_ramp.green[i] == i && g_ramp.blue[i] == i;
  g_ramp.identity = same;
  return true;
}

// Builds the table cscGetGamma hands back, from the ramp currently in force.
GuestAddr writeGammaTable(Toolbox& tb) {
  Mem& m = tb.mem();
  if (!g_tableOut) {
    g_tableOut = tb.heap().newPtr(kGtFormulaData + 3 * 256, true);
    if (!g_tableOut) return 0;
  }
  m.w16(g_tableOut + kGtVersion, 0);
  m.w16(g_tableOut + kGtType, 0);
  m.w16(g_tableOut + kGtFormulaSize, 0);
  m.w16(g_tableOut + kGtChanCnt, 3);
  m.w16(g_tableOut + kGtDataCnt, 256);
  m.w16(g_tableOut + kGtDataWidth, 8);
  const GuestAddr data = g_tableOut + kGtFormulaData;
  for (u32 i = 0; i < 256; ++i) {
    m.w8(data + i, g_ramp.red[i]);
    m.w8(data + 256 + i, g_ramp.green[i]);
    m.w8(data + 512 + i, g_ramp.blue[i]);
  }
  return g_tableOut;
}

}  // namespace

const GammaRamp& gammaRamp() {
  static bool init = (makeIdentity(), true);
  (void)init;
  return g_ramp;
}

void registerVideoDriver(Toolbox& tb) {
  gammaRamp();                                  // force the identity ramp

  // Control: the application sets a gamma table. Anything else on any driver
  // succeeds silently -- a driver call this port does not model is not a
  // failure the game should be told about, because the classic answer to an
  // unsupported control code is controlErr and the game would then stop
  // fading altogether.
  auto control = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr pb = a.ptr();
    if (!pb) { Toolbox::ret(c, 0); return; }
    const s16 csCode = s16(m.r16(pb + kPbCsCode));
    if (csCode == kCscSetGamma) {
      const GuestAddr table = m.r32(pb + kPbCsParam);
      if (readGammaTable(m, table) && std::getenv("CYT_DEBUG_GAMMA"))
        std::fprintf(stderr, "  [Gamma] r[64]=%3u r[128]=%3u r[192]=%3u%s\n",
                     unsigned(g_ramp.red[64]), unsigned(g_ramp.red[128]),
                     unsigned(g_ramp.red[192]),
                     g_ramp.identity ? "  (identity)" : "");
    }
    Toolbox::ret(c, 0);                         // noErr
  };
  tb.add("PBControlSync", control);
  tb.add("PBControlAsync", control);

  // Status: the application reads the current gamma table back.
  auto status = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr pb = a.ptr();
    if (!pb) { Toolbox::ret(c, 0); return; }
    if (s16(m.r16(pb + kPbCsCode)) == kCscGetGamma) {
      const GuestAddr table = writeGammaTable(tb);
      // GetDevGammaTable (0x075544) does `stw 29, 28(31)`: it puts its own
      // output pointer in csParam and expects the driver to write the table
      // pointer *through* it, rather than into csParam the way a plain
      // VDGammaRecord would be filled. Both are written, because the second
      // costs nothing and a caller that reads csParam back still works.
      const GuestAddr out = m.r32(pb + kPbCsParam);
      if (out) m.w32(out, table);
      m.w32(pb + kPbCsParam, table);
      if (std::getenv("CYT_DEBUG_GAMMA"))
        std::fprintf(stderr, "  [Gamma] get -> table %08x (via out %08x)\n",
                     table, out);
    }
    Toolbox::ret(c, 0);
  };
  tb.add("PBStatusSync", status);
  tb.add("PBStatusAsync", status);
}

}  // namespace cyt
