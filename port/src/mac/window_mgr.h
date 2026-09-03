// What the Window Manager exposes to the Dialog Manager.
//
// A DialogRecord *is* a WindowRecord with five fields bolted on the end, so the
// Dialog Manager cannot invent its own window: it has to build the same record,
// in the same list, or the window falls out of the visible-region computation
// in recomputeVisRegions and draws over things it is behind. Rather than
// duplicate the record size in two files -- which is precisely how two copies
// of one constant start to drift -- both come from here.
#pragma once

#include <string>

#include "mac/qd_surface.h"
#include "mem.h"

namespace cyt {

class Toolbox;

// WindowRecord: a CGrafPort followed by 60 bytes of window fields. The Dialog
// Manager's own fields begin at this offset within a DialogRecord.
constexpr u32 kWindowRecordSize = qd::kCGrafPortSize + 60;

// The two WindowRecord fields the Dialog Manager has to set for itself:
// windowKind distinguishes a dialogue from a document window, and visibility
// is deferred so that exactly one path (ShowWindow) makes a window appear.
constexpr u32 kWinKind    = qd::kCGrafPortSize + 0;
constexpr u32 kWinVisible = qd::kCGrafPortSize + 2;
// Head of the window's control list, which the Control Manager owns.
constexpr u32 kWinControlList = qd::kCGrafPortSize + 32;

// Allocates a window record, links it into the window list at `behind`
// (0xFFFFFFFF = front, 0 = back) and recomputes every visible region.
// Returns the WindowPtr, or 0 if the heap is exhausted.
//
// `recordSize` exists so that the Dialog Manager gets a DialogRecord-sized
// block in one allocation. Growing it afterwards is not an option: a Ptr is
// non-relocatable, SetPtrSize only extends into free space immediately after
// the block, and createWindow allocates five regions straight after the record
// -- so the grow would fail exactly when the heap is busy, which is to say
// unpredictably.
GuestAddr createWindow(Toolbox& tb, const Rect& bounds, const std::string& title,
                       bool visible, s16 procId, GuestAddr behind, bool goAway,
                       s32 refCon, u32 recordSize = kWindowRecordSize);

// Unlinks a window, frees its five regions and its title, and disposes the
// record. Shared so that DisposeDialog is the same teardown as DisposeWindow
// plus the item list -- two teardowns that drift apart leak differently.
void disposeWindow(Toolbox& tb, GuestAddr w);

}  // namespace cyt
