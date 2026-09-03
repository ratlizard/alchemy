// Dialog Manager.
//
// A DialogRecord is a WindowRecord with five fields on the end, and a dialogue
// is therefore an ordinary window that happens to own an item list. That is not
// a detail worth hiding: the window has to be built by the Window Manager's own
// createWindow so that it lands in the window list and gets a visible region,
// and every drawing call a dialogue makes is the same call any other window
// makes.
//
// The item list is the interesting part. A DITL resource and an in-memory item
// list have the *same* layout, except that the four bytes the resource wastes
// on a placeholder are, in memory, the item's handle -- a ControlHandle for a
// control, a TEHandle for text, and for a userItem the routine descriptor its
// owner installs to draw it. So GetNewDialog copies the resource into a handle
// and everything afterwards reads and writes that copy in place. Nothing is
// parsed into a host-side structure, because the application both reads the
// list (GetDialogItem) and writes it (SetDialogItem), and a host-side copy
// would have to be kept in step with a guest-side one for no benefit.
//
// Items are variable length and word-aligned, so item N can only be found by
// walking from item 1. Every entry point that takes an item number therefore
// walks; the lists are thirteen items long and this is not worth indexing.
//
// What is NOT here yet: DrawDialog, ModalDialog and DialogSelect. Those need
// the Control Manager to draw a push button and the List Manager to draw a
// cell, and both are separate pieces of work. GetNewDialog through
// SetDialogItem is what TDialog::TDialog and TDialog::InstallUserItems need to
// get through their constructor, which is the point this port stops at today.
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

// DialogRecord fields, following the WindowRecord.
constexpr u32 kDlgItems     = kWindowRecordSize + 0;   // Handle to the item list
constexpr u32 kDlgTextH     = kWindowRecordSize + 4;   // TEHandle, editing
constexpr u32 kDlgEditField = kWindowRecordSize + 8;   // short, -1 = none
constexpr u32 kDlgEditOpen  = kWindowRecordSize + 10;  // short
constexpr u32 kDlgDefItem   = kWindowRecordSize + 12;  // short, the default
constexpr u32 kDialogRecordSize = kWindowRecordSize + 14;

// One item, located within the guest-side item list.
struct Item {
  GuestAddr entry = 0;   // start of the entry: the 4-byte handle field
  GuestAddr box = 0;     // the display rectangle, entry + 4
  GuestAddr typeByte = 0;
  GuestAddr textLen = 0; // the Pascal length byte, typeByte + 1
  u32 length = 0;        // whole entry length, including any alignment pad
};

// How many items the list holds. The stored count is one less than the real
// one, which is the single most reliable way to be off by one in this format.
s16 itemCount(Mem& m, GuestAddr items) {
  if (!items) return 0;
  GuestAddr p = m.r32(items);
  if (!p) return 0;
  return s16(m.r16(p) + 1);
}

// Walks to item `want` (1-based). Returns false when the list is shorter than
// that, or when the walk would run past the end of the handle's block.
bool findItem(Toolbox& tb, GuestAddr items, s16 want, Item* out) {
  Mem& m = tb.mem();
  if (!items || want < 1) return false;
  GuestAddr base = m.r32(items);
  if (!base) return false;
  const s16 n = s16(m.r16(base) + 1);
  if (want > n) return false;
  // The handle's logical size bounds the walk. A DITL with a truncated last
  // item would otherwise walk into whatever follows it in the heap.
  const u32 limit = tb.heap().handleSize(items);
  u32 off = 2;
  for (s16 i = 1; i <= want; ++i) {
    if (off + 14 > limit) return false;
    Item it;
    it.entry = base + off;
    it.box = it.entry + 4;
    it.typeByte = it.entry + 12;
    it.textLen = it.entry + 13;
    const u8 len = m.r8(it.textLen);
    u32 size = 14 + len;
    if (size & 1) ++size;                    // entries are word-aligned
    it.length = size;
    if (off + size > limit) return false;
    if (i == want) { *out = it; return true; }
    off += size;
  }
  return false;
}

// The DLOG resource, in the order the fields appear.
struct Dlog {
  Rect bounds;
  s16 procId = 0;
  bool visible = false;
  bool goAway = false;
  s32 refCon = 0;
  s16 itemsId = 0;
  std::string title;
};

bool readDlog(s16 id, Dlog* out) {
  std::vector<u8> d;
  if (!peekResource(resType("DLOG"), id, &d) || d.size() < 20) return false;
  auto be16 = [&](u32 o) { return s16(u16(d[o]) << 8 | d[o + 1]); };
  auto be32 = [&](u32 o) {
    return s32(u32(d[o]) << 24 | u32(d[o + 1]) << 16 | u32(d[o + 2]) << 8 | d[o + 3]);
  };
  out->bounds = Rect{be16(0), be16(2), be16(4), be16(6)};
  out->procId = be16(8);
  out->visible = d[10] != 0;
  out->goAway = d[12] != 0;
  out->refCon = be32(14);
  out->itemsId = be16(18);
  // The title is a Pascal string, and in this game it is NOT a title: every
  // dialogue stores semicolon-separated keyboard equivalents there instead
  // (DLOG 133's is ";;Mm;Fm"). It is read so that nothing is lost, and
  // deliberately not passed to the window, which would draw it as a caption.
  if (d.size() > 20) {
    const u32 n = d[20];
    if (21 + n <= d.size())
      out->title.assign(reinterpret_cast<const char*>(&d[21]), n);
  }
  return true;
}

// Copies a DITL resource into a fresh handle. The copy is what the dialogue
// then owns and mutates: SetDialogItem writes into it, and the resource must
// not be touched, because the Resource Manager hands out one shared copy and a
// second dialogue from the same DITL would inherit the first one's handles.
GuestAddr copyItemList(Toolbox& tb, s16 ditlId) {
  std::vector<u8> d;
  if (!peekResource(resType("DITL"), ditlId, &d) || d.size() < 2) return 0;
  GuestAddr h = tb.heap().newHandle(u32(d.size()), false);
  if (!h) return 0;
  Mem& m = tb.mem();
  GuestAddr p = m.r32(h);
  for (u32 i = 0; i < d.size(); ++i) m.w8(p + i, d[i]);
  // Every item's handle field is a placeholder in the resource and must start
  // life empty in the copy, or InstallUserItems would read resource bytes as a
  // routine descriptor.
  const s16 n = s16(m.r16(p) + 1);
  u32 off = 2;
  for (s16 i = 1; i <= n && off + 14 <= d.size(); ++i) {
    m.w32(p + off, 0);
    u32 size = 14 + u32(d[off + 13]);
    if (size & 1) ++size;
    off += size;
  }
  return h;
}

GuestAddr newDialog(Toolbox& tb, const Dlog& dl, GuestAddr behind) {
  // The window is created invisible whatever the resource says, and shown
  // later by ShowWindow. Cythera's own dialogues are all authored invisible so
  // that they can be populated before they appear -- TCreatePlayerDialog fills
  // two lists and sets a radio button before its ShowWindow -- and honouring a
  // "visible" flag here would only paint an empty frame first.
  GuestAddr w = createWindow(tb, dl.bounds, /*title=*/std::string(),
                             /*visible=*/false, dl.procId, behind,
                             dl.goAway, dl.refCon, kDialogRecordSize);
  if (!w) return 0;
  Mem& m = tb.mem();
  m.w16(w + kWinKind, 2);            // dialogKind
  GuestAddr items = copyItemList(tb, dl.itemsId);
  if (!items) {
    std::fprintf(stderr, "  [Dialog] DITL %d is missing or unreadable\n",
                 dl.itemsId);
    return 0;
  }
  m.w32(w + kDlgItems, items);
  m.w32(w + kDlgTextH, 0);
  m.w16(w + kDlgEditField, u16(0xFFFF));   // -1: no edit field open
  m.w16(w + kDlgEditOpen, 0);
  m.w16(w + kDlgDefItem, 1);               // item 1 is OK, by convention
  if (std::getenv("CYT_DEBUG_DIALOG"))
    std::fprintf(stderr,
                 "  [Dialog] %08x from DLOG bounds(%d,%d,%d,%d) procID %d, "
                 "DITL %d with %d items, keys \"%s\"\n",
                 w, dl.bounds.top, dl.bounds.left, dl.bounds.bottom,
                 dl.bounds.right, dl.procId, dl.itemsId,
                 itemCount(m, items), dl.title.c_str());
  return w;
}

}  // namespace

void registerDialogManager(Toolbox& tb) {
  // GetNewDialog(dialogID, dStorage, behind). dStorage is ignored: this port
  // always allocates, as it does for windows.
  tb.add("GetNewDialog", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 id = a.s16v();
    a.ptr();                                  // dStorage
    const GuestAddr behind = a.ptr();
    Dlog dl;
    if (!readDlog(id, &dl)) {
      std::fprintf(stderr, "  [Dialog] no DLOG %d\n", id);
      Toolbox::ret(c, 0);
      return;
    }
    Toolbox::ret(c, newDialog(tb, dl, behind));
  });

  // NewDialog / NewColorDialog build the same record from arguments rather
  // than from a resource. The item list arrives as a handle the caller owns;
  // it is copied for the same reason GetNewDialog copies a resource.
  auto fromArgs = [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                                  // dStorage
    Rect bounds = readRect(tb.mem(), a.ptr());
    a.ptr();                                  // title, deliberately unused
    const bool visible = a.u8v() != 0;
    const s16 procId = a.s16v();
    const GuestAddr behind = a.ptr();
    const bool goAway = a.u8v() != 0;
    const s32 refCon = a.s32v();
    const GuestAddr itemsIn = a.ptr();
    GuestAddr w = createWindow(tb, bounds, std::string(), false, procId,
                               behind, goAway, refCon, kDialogRecordSize);
    if (!w) { Toolbox::ret(c, 0); return; }
    Mem& m = tb.mem();
    m.w16(w + kWinKind, 2);
    m.w32(w + kDlgItems, itemsIn);
    m.w32(w + kDlgTextH, 0);
    m.w16(w + kDlgEditField, u16(0xFFFF));
    m.w16(w + kDlgEditOpen, 0);
    m.w16(w + kDlgDefItem, 1);
    if (visible) {
      // Deferred to ShowWindow rather than done here, so that there is exactly
      // one path that makes a dialogue visible.
      m.w8(w + kWinVisible, 0);
    }
    Toolbox::ret(c, w);
  };
  tb.add("NewDialog", fromArgs);
  tb.add("NewColorDialog", fromArgs);

  tb.add("CountDITL", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr d = a.ptr();
    Mem& m = tb.mem();
    Toolbox::ret(c, d ? u32(u16(itemCount(m, m.r32(d + kDlgItems)))) : 0);
  });

  // GetDialogItem(d, item, &type, &handle, &box). All three outputs are
  // optional in practice, and the game passes all three.
  tb.add("GetDialogItem", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr d = a.ptr();
    const s16 item = a.s16v();
    const GuestAddr typeOut = a.ptr();
    const GuestAddr handleOut = a.ptr();
    const GuestAddr boxOut = a.ptr();
    Mem& m = tb.mem();
    Item it;
    if (!d || !findItem(tb, m.r32(d + kDlgItems), item, &it)) {
      // Reporting an item that does not exist as type 0 with an empty box is
      // what the real manager does, and InstallUserItems walks 1..CountDITL,
      // so this should never be reached from a well-formed list.
      if (typeOut) m.w16(typeOut, 0);
      if (handleOut) m.w32(handleOut, 0);
      if (boxOut) writeRect(m, boxOut, Rect{0, 0, 0, 0});
      Toolbox::ret(c, 0);
      return;
    }
    if (typeOut) m.w16(typeOut, m.r8(it.typeByte));
    if (handleOut) m.w32(handleOut, m.r32(it.entry));
    if (boxOut) writeRect(m, boxOut, readRect(m, it.box));
    Toolbox::ret(c, 0);
  });

  // SetDialogItem(d, item, type, handle, &box). This is how the application
  // installs its own drawing routine for every userItem, so the handle it
  // stores is a routine descriptor rather than anything the port allocated.
  tb.add("SetDialogItem", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr d = a.ptr();
    const s16 item = a.s16v();
    const s16 type = a.s16v();
    const GuestAddr handle = a.ptr();
    const GuestAddr boxIn = a.ptr();
    Mem& m = tb.mem();
    Item it;
    if (!d || !findItem(tb, m.r32(d + kDlgItems), item, &it)) {
      Toolbox::ret(c, 0);
      return;
    }
    m.w32(it.entry, handle);
    m.w8(it.typeByte, u8(type));
    if (boxIn) writeRect(m, it.box, readRect(m, boxIn));
    Toolbox::ret(c, 0);
  });

  // GetDialogItemText / SetDialogItemText work on the item's *text*, which for
  // a static or edit text item is stored inline in the item list.
  tb.add("GetDialogItemText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr h = a.ptr();
    const GuestAddr out = a.ptr();
    Mem& m = tb.mem();
    if (!out) { Toolbox::ret(c, 0); return; }
    // The handle here is the item's handle as GetDialogItem reported it. For a
    // text item that is a handle to the text itself.
    if (!h || !tb.heap().validHandle(h)) {
      m.w8(out, 0);
      Toolbox::ret(c, 0);
      return;
    }
    const GuestAddr p = m.r32(h);
    const u32 n = tb.heap().handleSize(h);
    std::string s;
    for (u32 i = 0; i < n && i < 255; ++i) s.push_back(char(m.r8(p + i)));
    m.writePstr(out, s, 256);
    Toolbox::ret(c, 0);
  });
  tb.add("SetDialogItemText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr h = a.ptr();
    const GuestAddr in = a.ptr();
    Mem& m = tb.mem();
    if (!h || !tb.heap().validHandle(h) || !in) { Toolbox::ret(c, 0); return; }
    const std::string s = m.pstr(in);
    if (tb.heap().setHandleSize(h, u32(s.size()))) {
      const GuestAddr p = m.r32(h);
      for (u32 i = 0; i < s.size(); ++i) m.w8(p + i, u8(s[i]));
    }
    Toolbox::ret(c, 0);
  });

  tb.add("DisposeDialog", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr d = a.ptr();
    if (!d) { Toolbox::ret(c, 0); return; }
    Mem& m = tb.mem();
    if (GuestAddr items = m.r32(d + kDlgItems)) tb.heap().disposeHandle(items);
    m.w32(d + kDlgItems, 0);
    disposeWindow(tb, d);
    Toolbox::ret(c, 0);
  });

  tb.add("GetDialogWindow", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, a.ptr());     // a DialogPtr already IS a WindowPtr
  });
  tb.add("SetDialogDefaultItem", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const GuestAddr d = a.ptr();
    const s16 item = a.s16v();
    if (d) tb.mem().w16(d + kDlgDefItem, u16(item));
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
