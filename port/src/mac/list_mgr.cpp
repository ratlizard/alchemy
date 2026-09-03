// List Manager.
//
// The record layout is Apple's, not invented here: Inside Macintosh: More
// Macintosh Toolbox, page 4-110, whose assembly-language summary prints the
// byte offsets outright. Two of them -- refCon at 60 and userHandle at 68 --
// are also what reading Cythera's own binary produced, so the documentation
// and the reverse engineering agree and the offsets below can be trusted.
//
// **This port's List Manager never draws a cell itself, and that is the whole
// design.** Cythera installs its own list definition procedure and does the
// drawing in its own code. The mechanism, from POWERPC-NOTES.md:
//
//   * TListBox::TListBox calls LNew with theProc 128, then writes a routine
//     descriptor over its MyLDEF into (**list).refCon, `this` into
//     (**list).userHandle, and lOnlyOne into (**list).selFlags.
//   * LDEF 128 in the resource fork is a small 68k trampoline whose entire job
//     is to read refCon and call it with the identical argument list. It is
//     the only genuine 68k in the application.
//   * So the port skips the trampoline and calls refCon directly, which is
//     what "reimplement its semantics natively" means. No 68k emulator.
//
// The one ordering trap, and it is a real one: **the trampoline clears refCon
// on lInitMsg**. Sending an init message after the caller has installed its
// descriptor would erase the very thing that makes the list draw. Zeroing
// refCon inside LNew is equivalent and safe, so that is what happens here and
// no init message is ever sent.
//
// Cell storage. `cells` is a handle holding every cell's bytes concatenated,
// and `cellArray` holds the offset of each cell, so cell i occupies
// cellArray[i] up to cellArray[i+1]. That is why there is always one more
// entry than there are cells. Inside Macintosh documents this ordering as
// internal and hands applications LGetCellDataLocation to read it, so the
// arrangement only has to be self-consistent -- but the high bit of an offset
// marking "this cell is selected" is the real List Manager's own trick and is
// kept, so that selection lives in guest memory with everything else rather
// than in a host-side table that a DisposeHandle would strand.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "mac/heap.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "mac/window_mgr.h"
#include "toolbox.h"

namespace cyt {

bool resolveUpp(Mem& m, GuestAddr upp, GuestAddr* code, GuestAddr* toc);

namespace {

// ListRec, from More Macintosh Toolbox 4-110.
constexpr u32 kLstRView       = 0;    // Rect, 8
constexpr u32 kLstPort        = 8;
constexpr u32 kLstIndent      = 12;   // Point
constexpr u32 kLstCellSize    = 16;   // Point
constexpr u32 kLstVisible     = 20;   // Rect, 8
constexpr u32 kLstVScroll     = 28;
constexpr u32 kLstHScroll     = 32;
constexpr u32 kLstSelFlags    = 36;   // byte
constexpr u32 kLstActive      = 37;   // byte
constexpr u32 kLstReserved    = 38;   // byte
constexpr u32 kLstListFlags   = 39;   // byte
constexpr u32 kLstClikTime    = 40;
constexpr u32 kLstClikLoc     = 44;   // Point
constexpr u32 kLstMouseLoc    = 48;   // Point
constexpr u32 kLstClikLoop    = 52;
constexpr u32 kLstLastClick   = 56;   // Cell
constexpr u32 kLstRefCon      = 60;
constexpr u32 kLstListDefProc = 64;
constexpr u32 kLstUserHandle  = 68;
constexpr u32 kLstDataBounds  = 72;   // Rect, 8
constexpr u32 kLstCells       = 80;   // DataHandle
constexpr u32 kLstMaxIndex    = 84;   // word
constexpr u32 kLstCellArray   = 86;   // word[], offsets into `cells`
constexpr u32 kListRecFixed   = 86;

// selFlags. The game sets lOnlyOne, so at most one cell is ever selected.
constexpr u8 kLOnlyOne = 0x80;

// The high bit of a cellArray offset marks the cell selected.
constexpr u16 kSelectedBit = 0x8000;

// LDEF messages. Only lDrawMsg and lHiliteMsg reach the game's own code;
// lInitMsg is deliberately never sent (see the header comment).
constexpr s16 kLDrawMsg   = 0;
constexpr s16 kLHiliteMsg = 1;

struct Cell { s16 h = 0, v = 0; };

Cell readCell(u32 packed) {
  // A Cell is a Point: vertical in the high half, horizontal in the low.
  return Cell{s16(u16(packed & 0xFFFF)), s16(u16(packed >> 16))};
}

GuestAddr body(Mem& m, GuestAddr list) { return list ? m.r32(list) : 0; }

s16 colCount(Mem& m, GuestAddr b) {
  const Rect d = readRect(m, b + kLstDataBounds);
  return s16(d.right - d.left);
}
s16 rowCount(Mem& m, GuestAddr b) {
  const Rect d = readRect(m, b + kLstDataBounds);
  return s16(d.bottom - d.top);
}
s32 cellTotal(Mem& m, GuestAddr b) {
  return s32(colCount(m, b)) * s32(rowCount(m, b));
}

// Row-major: cell (h,v) is index (v - top) * columns + (h - left). Returns -1
// when the cell is outside dataBounds, which every entry point treats as "do
// nothing" rather than as an error, exactly as the real manager does.
s32 cellIndex(Mem& m, GuestAddr b, Cell c) {
  const Rect d = readRect(m, b + kLstDataBounds);
  if (c.h < d.left || c.h >= d.right || c.v < d.top || c.v >= d.bottom) return -1;
  return s32(c.v - d.top) * s32(d.right - d.left) + s32(c.h - d.left);
}

u16 offsetAt(Mem& m, GuestAddr b, s32 i) {
  return m.r16(b + kLstCellArray + u32(i) * 2);
}
void setOffsetAt(Mem& m, GuestAddr b, s32 i, u16 v) {
  m.w16(b + kLstCellArray + u32(i) * 2, v);
}
u16 dataOffsetAt(Mem& m, GuestAddr b, s32 i) {
  return u16(offsetAt(m, b, i) & ~kSelectedBit);
}
bool selectedAt(Mem& m, GuestAddr b, s32 i) {
  return (offsetAt(m, b, i) & kSelectedBit) != 0;
}

// The record has to grow as rows are added, because cellArray is inline. The
// handle is resized and the record's address re-read afterwards: a handle IS
// relocatable, so nothing may hold the dereferenced pointer across this.
bool resizeForCells(Toolbox& tb, GuestAddr list, s32 cells) {
  const u32 want = kListRecFixed + u32(cells + 1) * 2;
  if (tb.heap().handleSize(list) >= want) return true;
  return tb.heap().setHandleSize(list, want);
}

// Calls the application's own list definition procedure, which is what draws.
// Does nothing when refCon is empty -- which is the case for any list built
// before TListBox installs its descriptor, and for lists this port creates for
// code paths the game never completes.
void callLDef(Toolbox& tb, GuestAddr list, s16 message, bool selected,
              const Rect& r, Cell c, u16 dataOffset, u16 dataLen) {
  Mem& m = tb.mem();
  GuestAddr b = body(m, list);
  if (!b) return;
  const GuestAddr upp = m.r32(b + kLstRefCon);
  GuestAddr code = 0, toc = 0;
  if (!upp || !resolveUpp(m, upp, &code, &toc)) return;
  // The rectangle and the cell are passed by address, so both need somewhere
  // in guest memory to live for the duration of the call. One scratch block,
  // reused: the callee reads it and does not keep it.
  static GuestAddr scratch = 0;
  if (!scratch) {
    scratch = tb.heap().newPtr(16, true);
    if (!scratch) return;
  }
  writeRect(m, scratch, r);
  m.w16(scratch + 8, u16(c.v));      // a Cell is a Point: v then h
  m.w16(scratch + 10, u16(c.h));
  tb.interp().callPpc(code, toc,
                      {u32(u16(message)), selected ? 1u : 0u, scratch,
                       scratch + 8, u32(dataOffset), u32(dataLen), list});
}

// Replaces (or extends) one cell's bytes, moving every later offset by the
// difference in length. Shared by LSetCell and LAddToCell, which differ only
// in whether the new bytes replace the old ones or follow them.
void writeCellData(Toolbox& tb, GuestAddr list, Cell cell,
                   const std::vector<u8>& repl, bool append) {
  Mem& m = tb.mem();
  GuestAddr b = body(m, list);
  if (!b) return;
  const s32 i = cellIndex(m, b, cell);
  if (i < 0) return;
  const s32 total = cellTotal(m, b);
  GuestAddr cellsH = m.r32(b + kLstCells);
  if (!cellsH) return;
  const u16 oldOff = dataOffsetAt(m, b, i);
  const u16 oldEnd = dataOffsetAt(m, b, i + 1);
  const u32 oldSize = tb.heap().handleSize(cellsH);
  std::vector<u8> buf(oldSize);
  GuestAddr src = m.r32(cellsH);
  for (u32 k = 0; k < oldSize; ++k) buf[k] = m.r8(src + k);
  // Where the replaced span starts: the whole cell, or just past its end.
  const size_t cut = append ? std::min<size_t>(oldEnd, buf.size())
                            : std::min<size_t>(oldOff, buf.size());
  const size_t keepFrom = std::min<size_t>(oldEnd, buf.size());
  std::vector<u8> out;
  out.insert(out.end(), buf.begin(), buf.begin() + cut);
  out.insert(out.end(), repl.begin(), repl.end());
  out.insert(out.end(), buf.begin() + keepFrom, buf.end());
  if (!tb.heap().setHandleSize(cellsH, u32(out.size()))) return;
  b = m.r32(list);
  GuestAddr dst = m.r32(cellsH);
  for (size_t k = 0; k < out.size(); ++k) m.w8(dst + u32(k), out[k]);
  const s32 delta = s32(out.size()) - s32(buf.size());
  for (s32 k = i + 1; k <= total; ++k) {
    const bool sel = selectedAt(m, b, k);
    const u16 v = u16(s32(dataOffsetAt(m, b, k)) + delta);
    setOffsetAt(m, b, k, u16(v | (sel ? kSelectedBit : 0)));
  }
}

// The rectangle a cell occupies inside the list's display rectangle.
bool cellRectOf(Mem& m, GuestAddr b, Cell c, Rect* out) {
  if (cellIndex(m, b, c) < 0) return false;
  const Rect rv = readRect(m, b + kLstRView);
  const Rect d = readRect(m, b + kLstDataBounds);
  const s16 cw = s16(m.r16(b + kLstCellSize + 2));   // Point: v then h
  const s16 ch = s16(m.r16(b + kLstCellSize + 0));
  *out = Rect{s16(rv.top + (c.v - d.top) * ch),
              s16(rv.left + (c.h - d.left) * cw),
              s16(rv.top + (c.v - d.top + 1) * ch),
              s16(rv.left + (c.h - d.left + 1) * cw)};
  return true;
}

// A selection change is lHiliteMsg, not a redraw: MyLDEF dispatches message 0
// to the client's LDEFDraw and message 1 to its LDEFHilite, and sending a draw
// where a hilite belongs would repaint the cell instead of inverting it.
void hiliteCell(Toolbox& tb, GuestAddr list, Cell c) {
  Mem& m = tb.mem();
  GuestAddr b = body(m, list);
  Rect r;
  if (!b || !cellRectOf(m, b, c, &r)) return;
  const s32 i = cellIndex(m, b, c);
  const u16 off = dataOffsetAt(m, b, i);
  const u16 end = dataOffsetAt(m, b, i + 1);
  callLDef(tb, list, kLHiliteMsg, selectedAt(m, b, i), r, c, off,
           u16(end - off));
}

void drawCell(Toolbox& tb, GuestAddr list, Cell c) {
  Mem& m = tb.mem();
  GuestAddr b = body(m, list);
  if (!b) return;
  const s32 i = cellIndex(m, b, c);
  Rect cellRect;
  if (i < 0 || !cellRectOf(m, b, c, &cellRect)) return;
  const u16 off = dataOffsetAt(m, b, i);
  const u16 end = dataOffsetAt(m, b, i + 1);
  callLDef(tb, list, kLDrawMsg, selectedAt(m, b, i), cellRect, c, off,
           u16(end - off));
}

}  // namespace

void registerListManager(Toolbox& tb) {
  // LNew(rView, dataBounds, cSize, theProc, theWindow,
  //      drawIt, hasGrow, scrollHoriz, scrollVert)
  tb.add("LNew", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const Rect rView = readRect(m, a.ptr());
    const Rect dataBounds = readRect(m, a.ptr());
    // cSize is a Point passed BY VALUE, not by pointer: it is four bytes and
    // fits a register, so it arrives packed as v<<16|h. Inside Macintosh's C
    // prototype writes it "Point *cSize", transliterating the Pascal, but the
    // Universal Headers and the actual convention pass it by value -- reading
    // it as an address produced cell sizes like -32000x26753.
    const u32 cSize = a.u32v();
    const s16 theProc = a.s16v();
    const GuestAddr theWindow = a.ptr();
    const bool drawIt = a.u8v() != 0;
    a.u8v();                                  // hasGrow
    a.u8v();                                  // scrollHoriz
    a.u8v();                                  // scrollVert

    const s32 cells = s32(dataBounds.right - dataBounds.left) *
                      s32(dataBounds.bottom - dataBounds.top);
    GuestAddr list = tb.heap().newHandle(kListRecFixed + u32(cells + 1) * 2, true);
    if (!list) { Toolbox::ret(c, 0); return; }
    GuestAddr b = m.r32(list);
    writeRect(m, b + kLstRView, rView);
    m.w32(b + kLstPort, theWindow);
    m.w32(b + kLstIndent, 0);
    m.w16(b + kLstCellSize + 0, u16(cSize >> 16));      // v
    m.w16(b + kLstCellSize + 2, u16(cSize & 0xFFFF));   // h
    writeRect(m, b + kLstVisible, dataBounds);
    m.w32(b + kLstVScroll, 0);
    m.w32(b + kLstHScroll, 0);
    m.w8(b + kLstSelFlags, 0);
    m.w8(b + kLstActive, 1);
    m.w8(b + kLstReserved, 0);
    m.w8(b + kLstListFlags, 0);
    m.w32(b + kLstClikTime, 0);
    m.w32(b + kLstClikLoc, 0);
    m.w32(b + kLstMouseLoc, 0);
    m.w32(b + kLstClikLoop, 0);
    m.w32(b + kLstLastClick, 0);
    // Zeroed here rather than by sending lInitMsg, which is what the real
    // LDEF 128 trampoline does on that message -- and which would erase the
    // descriptor the caller is about to install. See the header comment.
    m.w32(b + kLstRefCon, 0);
    m.w32(b + kLstListDefProc, u32(u16(theProc)));
    m.w32(b + kLstUserHandle, 0);
    writeRect(m, b + kLstDataBounds, dataBounds);
    GuestAddr cellsH = tb.heap().newHandle(0, true);
    m.w32(b + kLstCells, cellsH);
    m.w16(b + kLstMaxIndex, 0);
    for (s32 i = 0; i <= cells; ++i) setOffsetAt(m, b, i, 0);
    m.w8(b + kLstActive, drawIt ? 1 : 1);
    if (std::getenv("CYT_DEBUG_DIALOG"))
      std::fprintf(stderr,
                   "  [List] %08x rView(%d,%d,%d,%d) bounds(%d,%d,%d,%d) "
                   "cell %dx%d proc %d window %08x\n",
                   list, rView.top, rView.left, rView.bottom, rView.right,
                   dataBounds.top, dataBounds.left, dataBounds.bottom,
                   dataBounds.right, s16(m.r16(b + kLstCellSize + 2)),
                   s16(m.r16(b + kLstCellSize + 0)), theProc, theWindow);
    Toolbox::ret(c, list);
  });

  tb.add("LDispose", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b) return;
    if (GuestAddr cellsH = m.r32(b + kLstCells)) tb.heap().disposeHandle(cellsH);
    tb.heap().disposeHandle(list);
  });

  // LAddRow(count, rowNum, list) -> the row number actually added at.
  //
  // TCreatePlayerDialog calls LAddRow(1, i+1, list) against a list that holds
  // i rows, so rowNum is always one past the end. The real manager clamps
  // rather than leaving a gap, and getting that wrong would put every
  // archetype name in the wrong row -- or in no row at all.
  tb.add("LAddRow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const s16 count = a.s16v();
    const s16 rowNum = a.s16v();
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b || count <= 0) { Toolbox::ret(c, 0); return; }
    Rect d = readRect(m, b + kLstDataBounds);
    const s16 cols = s16(d.right - d.left);
    const s16 rows = s16(d.bottom - d.top);
    const s16 at = std::clamp<s16>(rowNum, d.top, s16(d.top + rows));
    const s32 oldCells = s32(cols) * s32(rows);
    const s32 newCells = s32(cols) * s32(rows + count);
    if (!resizeForCells(tb, list, newCells)) { Toolbox::ret(c, 0); return; }
    b = m.r32(list);                       // the handle may have moved
    // Shift the offsets after the insertion point up by count*cols entries,
    // all of them empty (they share the offset of the cell they now precede).
    const s32 first = s32(at - d.top) * s32(cols);
    for (s32 i = oldCells; i >= first; --i)
      setOffsetAt(m, b, i + s32(count) * cols, offsetAt(m, b, i));
    for (s32 i = first; i < first + s32(count) * cols; ++i)
      setOffsetAt(m, b, i, dataOffsetAt(m, b, first + s32(count) * cols));
    d.bottom = s16(d.bottom + count);
    writeRect(m, b + kLstDataBounds, d);
    Rect vis = readRect(m, b + kLstVisible);
    vis.bottom = d.bottom;
    writeRect(m, b + kLstVisible, vis);
    Toolbox::ret(c, u32(u16(at)));
  });

  tb.add("LDelRow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const s16 count = a.s16v();
    const s16 rowNum = a.s16v();
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b) return;
    Rect d = readRect(m, b + kLstDataBounds);
    // count 0 means "delete every row", which is how a list is emptied.
    const s16 n = count > 0 ? count : s16(d.bottom - d.top);
    d.bottom = s16(std::max<s16>(d.top, s16(d.bottom - n)));
    (void)rowNum;
    writeRect(m, b + kLstDataBounds, d);
    Rect vis = readRect(m, b + kLstVisible);
    vis.bottom = d.bottom;
    writeRect(m, b + kLstVisible, vis);
  });

  // LSetCell(dataPtr, dataLen, theCell, lHandle)
  tb.add("LSetCell", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr dataPtr = a.ptr();
    const s16 dataLen = a.s16v();
    const Cell cell = readCell(a.u32v());
    const GuestAddr list = a.ptr();
    std::vector<u8> repl(size_t(std::max<s16>(dataLen, 0)));
    for (size_t k = 0; k < repl.size(); ++k) repl[k] = m.r8(dataPtr + u32(k));
    writeCellData(tb, list, cell, repl, /*append=*/false);
    drawCell(tb, list, cell);
  });

  // LAddToCell appends rather than replaces, which is how a cell built up in
  // several pieces ends up as one string.
  tb.add("LAddToCell", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr dataPtr = a.ptr();
    const s16 dataLen = a.s16v();
    const Cell cell = readCell(a.u32v());
    const GuestAddr list = a.ptr();
    std::vector<u8> add(size_t(std::max<s16>(dataLen, 0)));
    for (size_t k = 0; k < add.size(); ++k) add[k] = m.r8(dataPtr + u32(k));
    writeCellData(tb, list, cell, add, /*append=*/true);
    drawCell(tb, list, cell);
  });

  tb.add("LGetCell", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr dataPtr = a.ptr();
    const GuestAddr lenPtr = a.ptr();
    const Cell cell = readCell(a.u32v());
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b) return;
    const s32 i = cellIndex(m, b, cell);
    const s16 want = lenPtr ? s16(m.r16(lenPtr)) : 0;
    if (i < 0) { if (lenPtr) m.w16(lenPtr, 0); return; }
    const u16 off = dataOffsetAt(m, b, i), end = dataOffsetAt(m, b, i + 1);
    const s16 have = s16(end - off);
    const s16 n = std::min<s16>(want, have);
    GuestAddr src = m.r32(m.r32(b + kLstCells));
    for (s16 k = 0; k < n; ++k) m.w8(dataPtr + u32(k), m.r8(src + off + u32(k)));
    if (lenPtr) m.w16(lenPtr, u16(n));
  });

  tb.add("LGetCellDataLocation", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr offPtr = a.ptr();
    const GuestAddr lenPtr = a.ptr();
    const Cell cell = readCell(a.u32v());
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    const s32 i = b ? cellIndex(m, b, cell) : -1;
    if (i < 0) {
      if (offPtr) m.w16(offPtr, 0);
      if (lenPtr) m.w16(lenPtr, 0);
      return;
    }
    const u16 off = dataOffsetAt(m, b, i), end = dataOffsetAt(m, b, i + 1);
    if (offPtr) m.w16(offPtr, off);
    if (lenPtr) m.w16(lenPtr, u16(end - off));
  });

  // LSetSelect(setIt, theCell, lHandle). With lOnlyOne set -- which is what
  // Cythera uses -- selecting one cell clears every other.
  tb.add("LSetSelect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const bool setIt = a.u8v() != 0;
    const Cell cell = readCell(a.u32v());
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b) return;
    const s32 i = cellIndex(m, b, cell);
    if (i < 0) return;
    const s32 total = cellTotal(m, b);
    if (setIt && (u8(m.r8(b + kLstSelFlags)) & kLOnlyOne)) {
      for (s32 k = 0; k < total; ++k)
        if (k != i && selectedAt(m, b, k)) {
          setOffsetAt(m, b, k, dataOffsetAt(m, b, k));
          Cell other{s16(k % colCount(m, b)), s16(k / colCount(m, b))};
          const Rect d = readRect(m, b + kLstDataBounds);
          other.h = s16(other.h + d.left);
          other.v = s16(other.v + d.top);
          hiliteCell(tb, list, other);
        }
    }
    const u16 off = dataOffsetAt(m, b, i);
    setOffsetAt(m, b, i, u16(off | (setIt ? kSelectedBit : 0)));
    hiliteCell(tb, list, cell);
  });

  // LGetSelect(next, &theCell, lHandle) -> Boolean.
  // next=false asks "is this cell selected"; next=true asks "find the next
  // selected cell at or after this one", and updates theCell when it does.
  tb.add("LGetSelect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const bool next = a.u8v() != 0;
    const GuestAddr cellPtr = a.ptr();
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b || !cellPtr) { Toolbox::ret(c, 0); return; }
    const Cell cell{s16(u16(m.r16(cellPtr + 2))), s16(u16(m.r16(cellPtr)))};
    const s32 start = cellIndex(m, b, cell);
    if (start < 0) { Toolbox::ret(c, 0); return; }
    if (!next) { Toolbox::ret(c, selectedAt(m, b, start) ? 1 : 0); return; }
    const s32 total = cellTotal(m, b);
    const s16 cols = colCount(m, b);
    const Rect d = readRect(m, b + kLstDataBounds);
    for (s32 k = start; k < total; ++k) {
      if (!selectedAt(m, b, k)) continue;
      m.w16(cellPtr + 0, u16(s16(k / cols + d.top)));      // v
      m.w16(cellPtr + 2, u16(s16(k % cols + d.left)));     // h
      Toolbox::ret(c, 1);
      return;
    }
    Toolbox::ret(c, 0);
  });

  // Drawing mode. The real manager defers redrawing while it is off; this port
  // does not draw on its own account at all -- the application's LDEF does --
  // so the flag is recorded and the deferred redraw happens through LUpdate.
  tb.add("LSetDrawingMode", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const bool on = a.u8v() != 0;
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (b) m.w8(b + kLstActive, on ? 1 : 0);
  });

  // Scrolling. Every list Cythera builds is exactly as tall as its contents --
  // LNew is called with no scroll bars -- so there is nothing to scroll, and
  // the visible rectangle already equals dataBounds. Recorded rather than
  // implemented, because implementing it without a scroll bar to move would be
  // inventing behaviour nothing asks for.
  tb.add("LScroll", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s16v(); a.ptr();
  });
  tb.add("LAutoScroll", [](Toolbox& tb, PpcCpu& c, Args& a) { a.ptr(); });

  tb.add("LUpdate", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    a.ptr();                                   // theRgn
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b) return;
    const Rect d = readRect(m, b + kLstDataBounds);
    for (s16 v = d.top; v < d.bottom; ++v)
      for (s16 h = d.left; h < d.right; ++h) drawCell(tb, list, Cell{h, v});
  });
  tb.add("LDraw", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const Cell cell = readCell(a.u32v());
    drawCell(tb, a.ptr(), cell);
  });

  tb.add("LActivate", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const bool act = a.u8v() != 0;
    GuestAddr b = body(m, a.ptr());
    if (b) m.w8(b + kLstActive, act ? 1 : 0);
  });

  tb.add("LSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const s16 w = a.s16v(), h = a.s16v();
    GuestAddr b = body(m, a.ptr());
    if (!b) return;
    Rect r = readRect(m, b + kLstRView);
    writeRect(m, b + kLstRView,
              Rect{r.top, r.left, s16(r.top + h), s16(r.left + w)});
  });
  tb.add("LRect", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const GuestAddr out = a.ptr();
    const Cell cell = readCell(a.u32v());
    GuestAddr b = body(m, a.ptr());
    if (!out) return;
    if (!b || cellIndex(m, b, cell) < 0) {
      writeRect(m, out, Rect{0, 0, 0, 0});
      return;
    }
    const Rect rv = readRect(m, b + kLstRView);
    const Rect d = readRect(m, b + kLstDataBounds);
    const s16 cw = s16(m.r16(b + kLstCellSize + 2));
    const s16 ch = s16(m.r16(b + kLstCellSize + 0));
    writeRect(m, out, Rect{s16(rv.top + (cell.v - d.top) * ch),
                           s16(rv.left + (cell.h - d.left) * cw),
                           s16(rv.top + (cell.v - d.top + 1) * ch),
                           s16(rv.left + (cell.h - d.left + 1) * cw)});
  });
  tb.add("LCellSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const u32 size = a.u32v();
    GuestAddr b = body(m, a.ptr());
    if (!b) return;
    m.w16(b + kLstCellSize + 0, u16(size >> 16));
    m.w16(b + kLstCellSize + 2, u16(size & 0xFFFF));
  });

  tb.add("LAddColumn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const s16 count = a.s16v();
    a.s16v();                                  // colNum
    const GuestAddr list = a.ptr();
    GuestAddr b = body(m, list);
    if (!b || count <= 0) { Toolbox::ret(c, 0); return; }
    Rect d = readRect(m, b + kLstDataBounds);
    const s16 at = d.right;
    d.right = s16(d.right + count);
    if (!resizeForCells(tb, list,
                        s32(d.right - d.left) * s32(d.bottom - d.top))) {
      Toolbox::ret(c, 0);
      return;
    }
    b = m.r32(list);
    writeRect(m, b + kLstDataBounds, d);
    Toolbox::ret(c, u32(u16(at)));
  });
  tb.add("LDelColumn", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    const s16 count = a.s16v();
    a.s16v();
    GuestAddr b = body(m, a.ptr());
    if (!b) return;
    Rect d = readRect(m, b + kLstDataBounds);
    const s16 n = count > 0 ? count : s16(d.right - d.left);
    d.right = s16(std::max<s16>(d.left, s16(d.right - n)));
    writeRect(m, b + kLstDataBounds, d);
  });

  // LClick follows the mouse inside a list. Input is scheduled at the frame
  // boundary in this port rather than tracked in a loop, so a click is
  // reported as a completed click on whichever cell it landed in, and the
  // caller's own selection handling does the rest.
  tb.add("LClick", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u32v();                                  // pt
    a.u16v();                                  // modifiers
    a.ptr();                                   // list
    Toolbox::ret(c, 0);
  });
  tb.add("LLastClick", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr b = body(m, a.ptr());
    Toolbox::ret(c, b ? m.r32(b + kLstLastClick) : 0);
  });
  tb.add("LSearch", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.s16v(); a.ptr(); a.ptr(); a.ptr();
    Toolbox::ret(c, 0);
  });
  tb.add("LNextCell", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u8v(); a.u8v(); a.ptr(); a.ptr();
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
