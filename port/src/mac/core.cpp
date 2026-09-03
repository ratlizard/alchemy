// Core Toolbox services: the initialisation sequence, Mixed Mode, the string
// and arithmetic utilities, time, and alerts.
//
// Alerts are resolved rather than merely counted: an application that fails
// during startup reports why through StopAlert, so walking the ALRT's item list
// and substituting ParamText turns an opaque exit into a readable message. That
// diagnostic is worth more during a port than the dialog itself.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <ctime>
#include <string>

#include "mac/heap.h"
#include "mac/quickdraw.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

// Provided by resource_mgr.cpp: reads a resource's bytes without creating a
// guest handle, which is what the alert machinery needs.
bool peekResource(ResType type, s16 id, std::vector<u8>* out);

// Sixtieths of a second since start-up, as every classic timing loop expects.
// Wall time rather than processor time, so animation and timeouts run at the
// speed a player experiences. Shared with the Apple event layer, which needs a
// truthful timestamp on the event it delivers.
//
// The clock is read on every call. An idling Cythera asks better than a million
// times a second, so caching is tempting, but a cache can only be keyed on
// something that says how much time may have passed -- and the guest making no
// progress does not mean the clock has not moved. A stale answer to a lone poll
// several seconds later is a far worse bug than the two percent this costs.
u32 macTicks() {
  static const std::chrono::steady_clock::time_point start =
      std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  return u32(std::chrono::duration_cast<std::chrono::milliseconds>(now - start)
                 .count() * 60 / 1000);
}

namespace {

// ParamText's four substitution strings, referenced as ^0 through ^3.
std::string g_param[4];

std::string substituteParams(const std::string& in) {
  std::string out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '^' && i + 1 < in.size() && in[i + 1] >= '0' &&
        in[i + 1] <= '3') {
      out += g_param[size_t(in[i + 1] - '0')];
      ++i;
    } else {
      out.push_back(in[i]);
    }
  }
  return out;
}

// Pulls the static-text items out of a DITL so an alert can be printed.
std::string ditlText(s16 ditlId) {
  std::vector<u8> d;
  if (!peekResource(resType("DITL"), ditlId, &d) || d.size() < 2) return {};
  const u32 count = u32(u16(d[0]) << 8 | d[1]) + 1;
  u32 p = 2;
  std::string text;
  for (u32 i = 0; i < count && p + 14 <= d.size(); ++i) {
    p += 4;                       // placeholder for the item's handle
    p += 8;                       // display rectangle
    const u8 type = d[p++] & 0x7F;
    const u8 len = d[p++];
    if (p + len > d.size()) break;
    // 8 is a static text item; 4 and 5 are push buttons and check boxes.
    if (type == 8) {
      if (!text.empty()) text += " / ";
      text.append(reinterpret_cast<const char*>(&d[p]), len);
    }
    p += len;
    if (p & 1) ++p;               // items are word-aligned
  }
  return substituteParams(text);
}

void reportAlert(const char* kind, s16 alertId) {
  std::vector<u8> a;
  s16 ditlId = -1;
  if (peekResource(resType("ALRT"), alertId, &a) && a.size() >= 10)
    ditlId = s16(u16(a[8]) << 8 | a[9]);
  std::string msg = ditlId >= 0 ? ditlText(ditlId) : std::string();
  std::fprintf(stderr, "  [%s %d] %s\n", kind, alertId,
               msg.empty() ? "(no text found)" : msg.c_str());
}

}  // namespace

void registerToolboxCore(Toolbox& tb) {
  // ---- initialisation -----------------------------------------------------
  // These set up manager state that this port creates eagerly, so they only
  // need to consume their arguments and succeed.
  // InitGraf is where the screen actually comes into existence: the framebuffer,
  // its colour table, the main GDevice and the Window Manager port.
  tb.add("InitGraf", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                     // &qd.thePort, which this port does not use
    std::string err;
    if (!initGraphicsWorld(tb, &err))
      std::fprintf(stderr, "  [QuickDraw] cannot build the screen: %s\n",
                   err.c_str());
  });
  // InitFonts is not here: it builds the font family registry, so the Font
  // Manager registers it.
  // FlushEvents, SystemTask and SetEventMask belong to the Event Manager --
  // SystemTask in particular is where the host gets pumped, so a no-op here
  // would stop input reaching a loop that yields that way. RestoreDeviceClut
  // and PortChanged belong to QuickDraw.
  for (const char* init : {"InitWindows", "InitMenus",
                           "TEInit", "InitDialogs", "InitCursor", "InitPalettes",
                           "InitContextualMenus",
                           "ObscureCursor", "HideCursor", "ShowCursor",
                           "SysBeep", "InitProcMenu"})
    tb.add(init, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  // ---- Process Manager ----------------------------------------------------
  tb.add("GetCurrentProcess", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr psn = a.ptr();
    // A ProcessSerialNumber is two longs; 0/1 is kCurrentProcess.
    tb.mem().w32(psn + 0, 0);
    tb.mem().w32(psn + 4, 1);
    Toolbox::ret(c, 0);
  });
  tb.add("GetProcessInformation", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    GuestAddr info = a.ptr();
    // ProcessInfoRec: the fields the application reads are the size it was
    // given, its type and creator, and its free memory. processInfoLength is at
    // offset 0 and must be left as the caller set it.
    tb.mem().w32(info + 8, resType("APPL"));   // processType
    tb.mem().w32(info + 12, resType("Delv"));  // processSignature
    tb.mem().w32(info + 20, layout::kHeapEnd - layout::kHeapBase);  // size
    tb.mem().w32(info + 24, tb.heap().freeBytes());
    Toolbox::ret(c, 0);
  });
  tb.add("LaunchApplication", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    Toolbox::ret(c, u32(-43));   // fnfErr: nothing else to launch here
  });

  // ---- trap availability --------------------------------------------------
  // The standard idiom is
  //     NGetTrapAddress(trap) != NGetTrapAddress(_Unimplemented)
  // so returning one shared address for every trap reports every optional
  // manager as absent, and the application takes the fallback path it already
  // carries for older systems. Returning a distinct fake address per trap would
  // be worse than useless: the application would believe the trap exists and
  // then call straight into unmapped memory, which is exactly what Cythera does
  // when it probes for the Cursor Device Manager at trap 0xAADB.
  //
  // One trap is answered as *available*, and the exception is deliberate.
  // Cythera's IsOneGammaAvailable compares NGetTrapAddress(0xAA29) against
  // _Unimplemented and, finding them equal, skips its start-screen fade
  // entirely -- with no other call to show for it. It never calls through the
  // address; it only compares. What it calls when the answer is yes is the
  // video driver, which mac/video_driver.cpp implements. So this reports the
  // trap present and nothing calls into nothing.
  auto trapAddress = [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u32 trap = a.u32v() & 0xFFFF;
    if (trap == 0xAA29) {
      Toolbox::ret(c, layout::kShimBase + 0x70004);
      return;
    }
    Toolbox::ret(c, layout::kShimBase + 0x70000);
  };
  tb.add("NGetTrapAddress", trapAddress);
  tb.add("GetToolboxTrapAddress", trapAddress);
  tb.add("GetOSTrapAddress", trapAddress);
  tb.add("FindSymbol", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr(); a.ptr(); a.ptr();
    Toolbox::ret(c, u32(-2));    // cfragNoSymbolErr
  });
  tb.add("GetSharedLibrary", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.u32v(); a.u32v(); a.ptr(); a.ptr(); a.ptr();
    Toolbox::ret(c, u32(-2804)); // cfragNoLibraryErr
  });

  // ---- Mixed Mode ---------------------------------------------------------
  // On PowerPC a routine descriptor wraps a transition vector so that 68k
  // callers can reach native code. Cythera builds descriptors for its own
  // window, control and list definition procedures -- which is why the WDEF and
  // CDEF resources in the fork are six-byte placeholders rather than real code.
  tb.add("NewRoutineDescriptor", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr proc = a.ptr();
    a.u32v();                    // procInfo: the calling convention
    a.u32v();                    // ISA
    // Store the transition vector address behind a recognisable tag so that
    // CallUniversalProc can tell a descriptor from a bare procedure pointer.
    GuestAddr rd = tb.heap().newPtr(8, true);
    if (!rd) { Toolbox::ret(c, 0); return; }
    tb.mem().w32(rd + 0, 0xAAFE0000u);   // kRoutineDescriptorSignature-ish tag
    tb.mem().w32(rd + 4, proc);
    Toolbox::ret(c, rd);
  });
  tb.add("DisposeRoutineDescriptor", [](Toolbox& tb, PpcCpu& c, Args& a) {
    tb.heap().disposePtr(a.ptr());
  });
  tb.add("CallUniversalProc", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr rd = a.ptr();
    a.u32v();                    // procInfo
    // Remaining arguments belong to the callee; pass the registers straight
    // through, which is correct for every convention the game uses.
    GuestAddr tvector = rd;
    if (tb.mem().r32(rd) == 0xAAFE0000u) tvector = tb.mem().r32(rd + 4);
    std::vector<u32> args;
    for (u32 r = 5; r <= 10; ++r) args.push_back(c.gpr[r]);
    u32 code = tvector ? tb.mem().r32(tvector) : 0;
    u32 toc = tvector ? tb.mem().r32(tvector + 4) : 0;
    // Refuse to dispatch to something that cannot be code. This happens when
    // the application calls a trap it believed was available, and reporting it
    // is far more useful than executing whatever the address happens to hold.
    if (code < layout::kCodeBase || code >= layout::kHeapBase) {
      std::fprintf(stderr,
                   "  [MixedMode] refusing CallUniversalProc to 0x%08x "
                   "(descriptor 0x%08x)\n", code, rd);
      Toolbox::ret(c, 0);
      return;
    }
    Toolbox::ret(c, tb.interp().callPpc(code, toc, args));
  });

  // ---- string and bit utilities ------------------------------------------
  // c2pstr and p2cstr convert in place and return their argument.
  tb.add("c2pstr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    std::string s = tb.mem().cstr(p, 255);
    tb.mem().writePstr(p, s, 256);
    Toolbox::ret(c, p);
  });
  tb.add("p2cstr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    std::string s = tb.mem().pstr(p);
    for (size_t i = 0; i < s.size(); ++i) tb.mem().w8(p + u32(i), u8(s[i]));
    tb.mem().w8(p + u32(s.size()), 0);
    Toolbox::ret(c, p);
  });
  tb.add("NumToString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s32 v = a.s32v();
    GuestAddr out = a.ptr();
    tb.mem().writePstr(out, std::to_string(v), 256);
  });
  tb.add("StringToNum", [](Toolbox& tb, PpcCpu& c, Args& a) {
    std::string s = tb.mem().pstr(a.ptr());
    GuestAddr out = a.ptr();
    tb.mem().w32(out, u32(std::strtol(s.c_str(), nullptr, 10)));
  });
  tb.add("UpperString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    a.u8v();                     // diacritics flag
    u8 n = tb.mem().r8(p);
    for (u8 i = 0; i < n; ++i) {
      u8 ch = tb.mem().r8(p + 1 + i);
      if (ch >= 'a' && ch <= 'z') tb.mem().w8(p + 1 + i, u8(ch - 32));
    }
  });
  tb.add("EqualString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    std::string x = tb.mem().pstr(a.ptr()), y = tb.mem().pstr(a.ptr());
    bool caseSens = a.u8v() != 0;
    bool diacSens = a.u8v() != 0;
    (void)diacSens;
    if (!caseSens) {
      for (auto& ch : x) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
      for (auto& ch : y) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
    }
    Toolbox::ret(c, x == y ? 1 : 0);
  });
  // TruncString measures text, so it belongs to the Font Manager and is
  // registered there.
  tb.add("BitAnd", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 x = a.u32v(), y = a.u32v();
    Toolbox::ret(c, x & y);
  });
  tb.add("BitOr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 x = a.u32v(), y = a.u32v();
    Toolbox::ret(c, x | y);
  });
  tb.add("BitXor", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 x = a.u32v(), y = a.u32v();
    Toolbox::ret(c, x ^ y);
  });
  tb.add("BitNot", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, ~a.u32v());
  });
  tb.add("BitShift", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 v = a.u32v();
    s16 n = a.s16v();
    Toolbox::ret(c, n >= 0 ? (v << (n & 31)) : (v >> ((-n) & 31)));
  });
  // Fixed-point helpers: Fixed is 16.16.
  tb.add("FixMul", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s64 x = s32(a.u32v()), y = s32(a.u32v());
    Toolbox::ret(c, u32((x * y) >> 16));
  });
  tb.add("FixDiv", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s64 x = s32(a.u32v()), y = s32(a.u32v());
    Toolbox::ret(c, y ? u32((x << 16) / y) : 0x7FFFFFFFu);
  });
  tb.add("FixRatio", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s32 n = a.s32v(), d = a.s32v();
    Toolbox::ret(c, d ? u32((s64(n) << 16) / d) : 0x7FFFFFFFu);
  });
  tb.add("Random", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // The Toolbox generator is a documented 16-bit LCG seeded from randSeed.
    // Reproducing its exact sequence keeps the game's own randomness faithful.
    static u32 seed = 1;
    seed = u32((u64(seed) * 16807) % 0x7FFFFFFF);
    Toolbox::ret(c, u32(s32(s16(seed & 0xFFFF))));
  });

  // ---- time ---------------------------------------------------------------
  tb.add("TickCount", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u32 ticks = macTicks();
    if (std::getenv("CYT_DEBUG_TICKS")) {
      static u64 calls = 0;
      if ((++calls % 5'000'000) == 0)
        std::fprintf(stderr, "  [ticks] call %llu -> %u ticks\n",
                     (unsigned long long)calls, ticks);
    }
    Toolbox::ret(c, ticks);
  });
  tb.add("GetDateTime", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    // Mac epoch is 1904-01-01; Unix is 1970-01-01.
    constexpr u32 kMacEpochOffset = 2082844800u;
    tb.mem().w32(out, u32(std::time(nullptr)) + kMacEpochOffset);
  });
  tb.add("Delay", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u32v();
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, 0);
  });
  for (const char* t : {"InsTime", "RmvTime", "PrimeTime", "Microseconds"})
    tb.add(t, [](Toolbox& tb, PpcCpu& c, Args& a) {});

  // ---- alerts and dialog text -------------------------------------------
  tb.add("ParamText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    for (int i = 0; i < 4; ++i) {
      GuestAddr p = a.ptr();
      if (p) g_param[size_t(i)] = tb.mem().pstr(p);
    }
  });
  auto alert = [](const char* kind) {
    return [kind](Toolbox& tb, PpcCpu& c, Args& a) {
      s16 id = a.s16v();
      a.ptr();                   // modal filter procedure
      reportAlert(kind, id);
      // Item 1 is the default button, which is what dismissing an alert
      // reports. A startup alert therefore proceeds to the application's own
      // error handling rather than hanging.
      Toolbox::ret(c, 1);
    };
  };
  tb.add("Alert", alert("Alert"));
  tb.add("StopAlert", alert("StopAlert"));
  tb.add("NoteAlert", alert("NoteAlert"));
  tb.add("CautionAlert", alert("CautionAlert"));

  // ---- Balloon help and the Control Strip are absent ----------------------
  for (const char* absent : {"HMShowBalloon", "HMRemoveBalloon",
                             "SBIsControlStripVisible", "SBShowHideControlStrip"})
    tb.add(absent, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, 0);
    });

  // ---- Folder and volume queries ------------------------------------------
  tb.add("FindFolder", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u16v(); a.u32v(); a.u8v();
    GuestAddr vRefOut = a.ptr(), dirOut = a.ptr();
    // A single synthetic volume and directory: the File Manager layer maps them
    // onto the port's own support directory.
    if (vRefOut) tb.mem().w16(vRefOut, u16(s16(-1)));
    if (dirOut) tb.mem().w32(dirOut, 2);
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
