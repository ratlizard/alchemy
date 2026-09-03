// Menu Manager.
//
// A MenuHandle points at a MenuInfo whose variable-length tail holds the title
// followed by the items, each a Pascal string plus four bytes of icon, command
// key, mark character and style. A MENU resource uses that identical layout,
// except that the four bytes MenuInfo spends on a menuProc handle are a
// definition-procedure id and a filler word. GetMenu therefore copies the
// resource almost verbatim, which keeps the item encoding exactly as the game
// authored it -- including the metacharacters it relies on.
#include <cstdio>
#include <string>
#include <vector>

#include "mac/heap.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {

// MenuInfo field offsets.
constexpr u32 kMenuID       = 0;
constexpr u32 kMenuWidth    = 2;
constexpr u32 kMenuHeight   = 4;
constexpr u32 kMenuProc     = 6;
constexpr u32 kEnableFlags  = 10;
constexpr u32 kMenuData     = 14;   // title Pascal string starts here

// The menu bar, in left-to-right order. Menus are held by handle.
std::vector<GuestAddr> g_menuBar;
s16 g_hilited = 0;

// Walks the item list, returning the guest address of item `want` (1-based), or
// 0 when it does not exist. Item 0 addresses the title.
GuestAddr itemAddr(Mem& m, GuestAddr menu, s16 want) {
  GuestAddr blk = m.r32(menu);
  if (!blk) return 0;
  GuestAddr p = blk + kMenuData;
  p += 1u + m.r8(p);                    // skip the title
  for (s16 i = 1;; ++i) {
    u8 len = m.r8(p);
    if (len == 0 && i > 0) {
      // A zero-length item name terminates the list.
      return 0;
    }
    if (i == want) return p;
    p += 1u + len + 4u;                 // name, then icon/key/mark/style
    if (i > 256) return 0;              // malformed data: refuse to spin
  }
}

s16 countItems(Mem& m, GuestAddr menu) {
  GuestAddr blk = m.r32(menu);
  if (!blk) return 0;
  GuestAddr p = blk + kMenuData;
  p += 1u + m.r8(p);
  s16 n = 0;
  while (n < 256) {
    u8 len = m.r8(p);
    if (len == 0) break;
    ++n;
    p += 1u + len + 4u;
  }
  return n;
}

GuestAddr findMenu(Mem& m, s16 menuID) {
  for (GuestAddr h : g_menuBar) {
    GuestAddr blk = m.r32(h);
    if (blk && s16(m.r16(blk + kMenuID)) == menuID) return h;
  }
  return 0;
}

}  // namespace

void registerMenuManager(Toolbox& tb) {
  tb.add("GetMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    std::vector<u8> r;
    if (!peekResource(resType("MENU"), id, &r) || r.size() < kMenuData) {
      Toolbox::ret(c, 0);
      return;
    }
    GuestAddr h = tb.heap().newHandle(u32(r.size()), false);
    if (!h) { Toolbox::ret(c, 0); return; }
    GuestAddr blk = tb.mem().r32(h);
    tb.mem().copyIn(blk, r.data(), u32(r.size()));
    // Replace the resource's procID and filler with a menuProc handle. The
    // standard definition procedure is this port's own drawing code, so a null
    // handle is correct and is what the Menu Manager itself stored for MDEF 0.
    tb.mem().w32(blk + kMenuProc, 0);
    Toolbox::ret(c, h);
  });

  tb.add("NewMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    std::string title = tb.mem().pstr(a.ptr());
    u32 size = kMenuData + 1 + u32(title.size()) + 1;
    GuestAddr h = tb.heap().newHandle(size, true);
    if (!h) { Toolbox::ret(c, 0); return; }
    GuestAddr blk = tb.mem().r32(h);
    tb.mem().w16(blk + kMenuID, u16(id));
    tb.mem().w32(blk + kEnableFlags, 0xFFFFFFFFu);
    tb.mem().writePstr(blk + kMenuData, title, 256);
    tb.mem().w8(blk + kMenuData + 1 + u32(title.size()), 0);   // terminator
    Toolbox::ret(c, h);
  });

  tb.add("DisposeMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    tb.heap().disposeHandle(a.ptr());
  });

  tb.add("ClearMenuBar", [](Toolbox& tb, PpcCpu& c, Args& a) {
    g_menuBar.clear();
  });
  tb.add("InsertMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 before = a.s16v();
    if (!h) return;
    // beforeID 0 appends; otherwise insert ahead of that menu.
    if (before == 0) { g_menuBar.push_back(h); return; }
    for (size_t i = 0; i < g_menuBar.size(); ++i) {
      GuestAddr blk = tb.mem().r32(g_menuBar[i]);
      if (blk && s16(tb.mem().r16(blk + kMenuID)) == before) {
        g_menuBar.insert(g_menuBar.begin() + long(i), h);
        return;
      }
    }
    g_menuBar.push_back(h);
  });
  tb.add("DeleteMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    for (size_t i = 0; i < g_menuBar.size(); ++i) {
      GuestAddr blk = tb.mem().r32(g_menuBar[i]);
      if (blk && s16(tb.mem().r16(blk + kMenuID)) == id) {
        g_menuBar.erase(g_menuBar.begin() + long(i));
        return;
      }
    }
  });
  tb.add("GetMenuHandle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, findMenu(tb.mem(), a.s16v()));
  });
  tb.add("GetMenuBar", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // A saved menu bar is opaque; hand back a handle holding the current list.
    u32 n = u32(g_menuBar.size());
    GuestAddr h = tb.heap().newHandle(4 + n * 4, true);
    if (h) {
      GuestAddr blk = tb.mem().r32(h);
      tb.mem().w32(blk, n);
      for (u32 i = 0; i < n; ++i) tb.mem().w32(blk + 4 + i * 4, g_menuBar[i]);
    }
    Toolbox::ret(c, h);
  });
  tb.add("SetMenuBar", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    if (!h) return;
    GuestAddr blk = tb.mem().r32(h);
    u32 n = tb.mem().r32(blk);
    g_menuBar.clear();
    for (u32 i = 0; i < n && i < 64; ++i)
      g_menuBar.push_back(tb.mem().r32(blk + 4 + i * 4));
  });

  // Drawing is the Window Manager layer's job; the menu bar's contents are
  // already correct by the time it runs.
  for (const char* nop : {"DrawMenuBar", "InvalMenuBar", "FlashMenuBar",
                          "SetMenuFlash", "InsertFontResMenu",
                          "InsertIntlResMenu", "DeleteMenuItem"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  tb.add("GetMBarHeight", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 20);   // the classic menu bar height
  });
  tb.add("HiliteMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    g_hilited = a.s16v();
  });

  tb.add("CalcMenuSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    if (!h) return;
    GuestAddr blk = tb.mem().r32(h);
    if (!blk) return;
    // Without text measurement yet, size from the longest item name. The Font
    // Manager layer replaces this with real widths; the values only have to be
    // self-consistent for the application's own layout arithmetic.
    s16 n = countItems(tb.mem(), h);
    u32 widest = 0;
    for (s16 i = 1; i <= n; ++i) {
      GuestAddr p = itemAddr(tb.mem(), h, i);
      if (p) widest = std::max<u32>(widest, tb.mem().r8(p));
    }
    tb.mem().w16(blk + kMenuWidth, u16(16 + widest * 7));
    tb.mem().w16(blk + kMenuHeight, u16(n * 16));
  });

  tb.add("CountMItems", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(countItems(tb.mem(), a.ptr())));
  });

  tb.add("GetMenuItemText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 item = a.s16v();
    GuestAddr out = a.ptr();
    GuestAddr p = itemAddr(tb.mem(), h, item);
    tb.mem().writePstr(out, p ? tb.mem().pstr(p) : std::string(), 256);
  });
  tb.add("SetMenuItemText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 item = a.s16v();
    std::string text = tb.mem().pstr(a.ptr());
    GuestAddr p = itemAddr(tb.mem(), h, item);
    if (!p) return;
    // Only an in-place rewrite is safe here: growing an item would shift every
    // following item, and callers hold no expectation that it can grow.
    u8 cap = tb.mem().r8(p);
    tb.mem().writePstr(p, text.substr(0, cap), cap + 1u);
    tb.mem().w8(p, u8(std::min<size_t>(text.size(), cap)));
  });

  // Enable flags are a bitmask: bit 0 is the whole menu, bits 1..31 the items.
  auto setEnable = [](bool on) {
    return [on](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr h = a.ptr();
      s16 item = a.s16v();
      if (!h) return;
      GuestAddr blk = tb.mem().r32(h);
      if (!blk) return;
      if (item > 31) return;
      u32 flags = tb.mem().r32(blk + kEnableFlags);
      u32 bit = 1u << u32(item);
      tb.mem().w32(blk + kEnableFlags, on ? (flags | bit) : (flags & ~bit));
    };
  };
  tb.add("EnableItem", setEnable(true));
  tb.add("DisableItem", setEnable(false));

  // The mark character sits two bytes past the end of the item's name.
  auto markAddr = [](Mem& m, GuestAddr h, s16 item) -> GuestAddr {
    GuestAddr p = itemAddr(m, h, item);
    return p ? p + 1u + m.r8(p) + 2u : 0;
  };
  tb.add("SetItemMark", [markAddr](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 item = a.s16v();
    u8 mark = a.u8v();
    if (GuestAddr p = markAddr(tb.mem(), h, item)) tb.mem().w8(p, mark);
  });
  tb.add("GetItemMark", [markAddr](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 item = a.s16v();
    GuestAddr out = a.ptr();
    GuestAddr p = markAddr(tb.mem(), h, item);
    if (out) tb.mem().w16(out, p ? tb.mem().r8(p) : 0);
  });
  tb.add("CheckItem", [markAddr](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 item = a.s16v();
    bool on = a.u8v() != 0;
    if (GuestAddr p = markAddr(tb.mem(), h, item))
      tb.mem().w8(p, on ? 0x12 : 0);   // 0x12 is the check mark in Mac Roman
  });

  tb.add("AppendMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    std::string spec = tb.mem().pstr(a.ptr());
    if (!h) return;
    // Items are separated by ';' or ','. Metacharacters after the name set the
    // command key ('/'), mark ('!'), style ('<') and icon ('^'); '(' disables.
    u32 oldSize = tb.heap().handleSize(h);
    std::vector<std::pair<std::string, std::array<u8, 4>>> items;
    size_t start = 0;
    while (start <= spec.size()) {
      size_t end = spec.find_first_of(";,", start);
      std::string one = spec.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start);
      std::array<u8, 4> meta{0, 0, 0, 0};   // icon, key, mark, style
      std::string name;
      for (size_t i = 0; i < one.size(); ++i) {
        char ch = one[i];
        if ((ch == '/' || ch == '!' || ch == '<' || ch == '^') &&
            i + 1 < one.size()) {
          u8 v = u8(one[++i]);
          if (ch == '/') meta[1] = v;
          else if (ch == '!') meta[2] = v;
          else if (ch == '^') meta[0] = v;
          else meta[3] = u8(v - 'B' <= 4 ? (1u << (v - 'B')) : 0);
        } else if (ch == '(') {
          meta[3] |= 0x80;                  // remembered as "disabled"
        } else {
          name.push_back(ch);
        }
      }
      items.emplace_back(name, meta);
      if (end == std::string::npos) break;
      start = end + 1;
    }
    u32 extra = 0;
    for (const auto& [name, meta] : items) extra += 1u + u32(name.size()) + 4u;
    if (!tb.heap().setHandleSize(h, oldSize + extra)) return;
    GuestAddr blk = tb.mem().r32(h);
    // Overwrite the old terminator and re-terminate after the new items.
    GuestAddr p = blk + oldSize - 1;
    s16 nextIndex = countItems(tb.mem(), h) + 1;
    for (const auto& [name, meta] : items) {
      tb.mem().writePstr(p, name, 256);
      p += 1u + u32(name.size());
      for (int k = 0; k < 4; ++k) tb.mem().w8(p + u32(k), meta[size_t(k)]);
      // A leading '(' disables the item through the enable mask, not the style.
      if (meta[3] & 0x80) {
        tb.mem().w8(p + 3, u8(meta[3] & 0x7F));
        if (nextIndex <= 31) {
          u32 flags = tb.mem().r32(blk + kEnableFlags);
          tb.mem().w32(blk + kEnableFlags, flags & ~(1u << u32(nextIndex)));
        }
      }
      p += 4;
      ++nextIndex;
    }
    tb.mem().w8(p, 0);
  });

  tb.add("AppendResMenu", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // Populates a menu with every resource of a type -- used for the Apple
    // menu's desk accessories and for font lists. Neither exists here, so the
    // menu is correctly left as it is.
    a.ptr(); a.u32v();
  });

  // MenuKey resolves a command-key press against the menu bar. It needs no
  // drawing at all, which makes keyboard equivalents usable well before the
  // menu bar is rendered -- and they are how a classic application is driven.
  tb.add("MenuKey", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 ch = a.s16v();
    const u8 want = u8(ch & 0xFF);
    // Command keys match without regard to case.
    const u8 wantUpper = (want >= 'a' && want <= 'z') ? u8(want - 32) : want;
    Mem& m = tb.mem();
    for (GuestAddr h : g_menuBar) {
      GuestAddr blk = m.r32(h);
      if (!blk) continue;
      const u32 flags = m.r32(blk + kEnableFlags);
      if (!(flags & 1)) continue;            // the whole menu is disabled
      const s16 menuID = s16(m.r16(blk + kMenuID));
      const s16 n = countItems(m, h);
      for (s16 i = 1; i <= n; ++i) {
        GuestAddr p = itemAddr(m, h, i);
        if (!p) break;
        const u8 len = m.r8(p);
        const u8 key = m.r8(p + 1 + len + 1);
        if (!key) continue;
        const u8 keyUpper = (key >= 'a' && key <= 'z') ? u8(key - 32) : key;
        if (keyUpper != wantUpper) continue;
        if (i <= 31 && !(flags & (1u << u32(i)))) continue;   // item disabled
        Toolbox::ret(c, (u32(u16(menuID)) << 16) | u32(u16(i)));
        return;
      }
    }
    Toolbox::ret(c, 0);
  });
  tb.add("MenuChoice", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0);
  });
  // Tracking a pulled-down menu needs the menu bar drawn and the mouse followed
  // through it, which belongs with the rest of the drawing work.
  for (const char* none : {"MenuSelect", "PopUpMenuSelect", "MenuEvent"})
    tb.add(none, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, 0);
    });
}

}  // namespace cyt
