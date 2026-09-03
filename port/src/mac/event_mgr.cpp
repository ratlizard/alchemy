// Event Manager and Sound Manager.
//
// Events come from two places: the host window, through Display, and the Apple
// event layer, which delivers the launch event the Finder would have sent. When
// neither has anything, the call reports a null event with a truthful mouse
// position -- the quiet pass a cooperative application must already handle.
// Sound reports that no channel could be opened, which the game treats as "run
// silently".
#include <cstdio>
#include <functional>

#include "host/display.h"
#include "mac/heap.h"
#include "mac/quickdraw.h"
#include "toolbox.h"

namespace cyt {

// Provided by apple_event.cpp: the launch event the Finder would have sent.
// It is offered to the application through the same queue as host input, so
// the application's own event loop is what dispatches it.
bool nextAppleEvent(u16 mask, MacEvent* out);

namespace {

// EventRecord: { short what; long message; long when; Point where;
//                short modifiers; } -- 16 bytes.
[[maybe_unused]] constexpr u32 kEventRecordSize = 16;
constexpr u32 kEvtWhat      = 0;
constexpr u32 kEvtMessage   = 2;
constexpr u32 kEvtWhen      = 6;
constexpr u32 kEvtWhere     = 10;
constexpr u32 kEvtModifiers = 14;

void writeEvent(Toolbox& tb, GuestAddr evt, const MacEvent& e) {
  if (!evt) return;
  tb.mem().w16(evt + kEvtWhat, e.what);
  tb.mem().w32(evt + kEvtMessage, e.message);
  tb.mem().w32(evt + kEvtWhen, e.when);
  // A Point is stored vertical-first.
  tb.mem().w16(evt + kEvtWhere + 0, u16(e.whereV));
  tb.mem().w16(evt + kEvtWhere + 2, u16(e.whereH));
  tb.mem().w16(evt + kEvtModifiers, e.modifiers);
}

// Builds the null event a quiet pass through the loop should see: no event, but
// a truthful mouse position and modifier state, which idle handlers read.
MacEvent idleEvent() {
  Display& d = Display::get();
  MacEvent e;
  e.what = kNullEvent;
  e.whereH = d.mouseH();
  e.whereV = d.mouseV();
  e.modifiers = d.modifiers();
  return e;
}

// How many times the application has passed through its event loop. Wall-clock
// delay loops make instruction counts unreproducible between runs, so this is
// the stable way to say "once the game has settled, do something".
u64 g_waitNextEventCalls = 0;

// What the harness asked to be done at the port's frame boundary.
std::function<void()> g_pumpHook;

// Presents a frame and collects input. This is the port's frame boundary: a
// cooperative Mac application passes through here every time round its loop,
// and also from any loop that polls the mouse -- which is why scheduled input
// is delivered from here rather than from the interpreter's own slicing. A
// mouse-tracking loop never returns to the event loop while the button is
// down, so a release scheduled anywhere else would never arrive.
void pumpHost(Toolbox& tb) {
  if (g_pumpHook) g_pumpHook();
  Display& d = Display::get();
  if (!d.isOpen()) return;
  if (!d.pump())
    tb.interp().halt("the window was closed");
}

}  // namespace

// Exposed so the harness can act once the application has settled into its loop.
u64 eventLoopPasses() { return g_waitNextEventCalls; }
void setHostPumpHook(std::function<void()> fn) { g_pumpHook = std::move(fn); }

void registerEventManager(Toolbox& tb) {
  tb.add("WaitNextEvent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u16 mask = a.u16v();
    GuestAddr evt = a.ptr();
    const u32 sleepTicks = a.u32v();
    a.ptr();                        // mouse region
    ++g_waitNextEventCalls;
    pumpHost(tb);
    Display::get().idle(sleepTicks, mask);
    MacEvent e;
    if (nextAppleEvent(mask, &e)) {
      writeEvent(tb, evt, e);
      Toolbox::ret(c, 1);
      return;
    }
    if (Display::get().nextEvent(mask, &e)) {
      writeEvent(tb, evt, e);
      Toolbox::ret(c, 1);
      return;
    }
    writeEvent(tb, evt, idleEvent());
    Toolbox::ret(c, 0);
  });
  auto getEvent = [](bool remove) {
    return [remove](Toolbox& tb, PpcCpu& c, Args& a) {
      const u16 mask = a.u16v();
      GuestAddr evt = a.ptr();
      pumpHost(tb);
      MacEvent e;
      bool got = remove && nextAppleEvent(mask, &e);
      if (!got)
        got = remove ? Display::get().nextEvent(mask, &e)
                     : Display::get().peekEvent(mask, &e);
      writeEvent(tb, evt, got ? e : idleEvent());
      Toolbox::ret(c, got ? 1u : 0u);
    };
  };
  tb.add("GetNextEvent", getEvent(true));
  tb.add("EventAvail", getEvent(false));
  tb.add("GetOSEvent", getEvent(true));
  tb.add("OSEventAvail", getEvent(false));

  tb.add("GetKeys", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr keyMap = a.ptr();
    if (!keyMap) return;
    u8 keys[16];
    Display::get().keyMap(keys);
    for (u32 i = 0; i < 16; ++i) tb.mem().w8(keyMap + i, keys[i]);
  });
  tb.add("GetMouse", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr pt = a.ptr();
    if (!pt) return;
    tb.mem().w16(pt + 0, u16(Display::get().mouseV()));
    tb.mem().w16(pt + 2, u16(Display::get().mouseH()));
  });
  tb.add("Button", [](Toolbox& tb, PpcCpu& c, Args& a) {
    pumpHost(tb);
    Toolbox::ret(c, Display::get().mouseDown() ? 1u : 0u);
  });
  tb.add("StillDown", [](Toolbox& tb, PpcCpu& c, Args& a) {
    pumpHost(tb);
    Toolbox::ret(c, Display::get().mouseDown() ? 1u : 0u);
  });
  tb.add("WaitMouseUp", [](Toolbox& tb, PpcCpu& c, Args& a) {
    pumpHost(tb);
    // Reports whether the button is still down, having consumed the mouse-up
    // event if one had already arrived.
    MacEvent e;
    if (Display::get().nextEvent(u16(1u << kMouseUp), &e)) {
      Toolbox::ret(c, 0);
      return;
    }
    Toolbox::ret(c, Display::get().mouseDown() ? 1u : 0u);
  });
  // SystemTask is the other place a classic application yields, so the host is
  // pumped there too for applications that spin without WaitNextEvent.
  tb.add("SystemTask", [](Toolbox& tb, PpcCpu& c, Args& a) { pumpHost(tb); });
  tb.add("LMGetDoubleTime", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 30);            // half a second, the classic default
  });
  tb.add("LMGetKeyThresh", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 8);
  });
  for (const char* nop : {"SystemClick", "SystemEdit", "SystemBeep",
                          "OpenDeskAcc", "Enqueue", "Dequeue", "SetEventMask",
                          "FlushEvents"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });

}

void registerSoundManager(Toolbox& tb) {
  // Reporting that no channel is available is the documented way for a machine
  // without usable audio to behave, and Cythera continues without sound rather
  // than refusing to start. Real output arrives with the audio back end.
  constexpr s16 kNotEnoughHardwareErr = -201;
  tb.add("SndNewChannel", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr chanOut = a.ptr();
    if (chanOut) tb.mem().w32(chanOut, 0);
    Toolbox::ret(c, u32(s32(kNotEnoughHardwareErr)));
  });
  tb.add("SndDisposeChannel", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0);
  });
  for (const char* quiet : {"SndPlayDoubleBuffer", "SndPlay", "SndDoCommand",
                            "SndDoImmediate", "SndControl", "SndGetInfo",
                            "SndSetInfo"})
    tb.add(quiet, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, u32(s32(kNotEnoughHardwareErr)));
    });
  tb.add("SndSoundManagerVersion", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0x03060000);    // Sound Manager 3.6
  });
  tb.add("GetDefaultOutputVolume", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, 0x01000100);   // full volume, both channels
    Toolbox::ret(c, 0);
  });
  tb.add("SetDefaultOutputVolume", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0);
  });

  // InputSprocket is absent; the game falls back to the Event Manager.
  for (const char* isp : {"ISpInit", "ISpStop", "ISpElementList_New",
                          "ISpElement_NewVirtualFromNeeds", "ISpStartup",
                          "ISpShutdown", "ISpDevices_Extract",
                          "ISpElement_GetSimpleState", "ISpSuspend",
                          "ISpResume", "ISpGetVersion"})
    tb.add(isp, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, u32(s32(-1))); // any failure makes the game skip it
    });
}

}  // namespace cyt
