// Apple events.
//
// A classic Macintosh application is not started by its main function alone.
// The Finder launches it and then *sends* it the 'oapp' ("open application")
// Apple event, and an application built on a framework does its real start-up
// work in that handler rather than in main. Cythera is one of those: main sets
// the Toolbox up, shows the splash, and enters the event loop, while the code
// that opens the game's start dialog hangs off the 'oapp' handler it installed
// during initialisation. With no Finder to send the event, the game reaches its
// event loop and idles there forever, looking for all the world like a hang.
//
// So the port delivers the launch event itself. The queue is synthetic -- there
// is no interapplication messaging here -- but the path the event takes through
// the application is the real one: WaitNextEvent returns a high-level event,
// the application calls AEProcessAppleEvent, and that dispatches through Mixed
// Mode to the handler the application installed.
#include <cstdio>
#include <map>
#include <utility>

#include "host/display.h"
#include "mac/heap.h"
#include "toolbox.h"

namespace cyt {

u32 macTicks();

namespace {

constexpr u32 kCoreEventClass    = 0x61657674;   // 'aevt'
constexpr u32 kAEOpenApplication = 0x6F617070;   // 'oapp'
constexpr u32 kTypeAppleEvent    = 0x61657674;   // 'aevt'
constexpr u32 kTypeNull          = 0x6E756C6C;   // 'null'

constexpr s16 kErrAEEventNotHandled = -1708;

// A high-level event is reported through the event mask bit the Communications
// Toolbox also used for network events, which is why it is not simply 1 << 23.
constexpr u16 kHighLevelEventMask = 1024;

// An AEDesc is a descriptor type followed by the handle holding the data. Two
// of them are all a handler is passed: the event itself and the reply it may
// fill in.
constexpr u32 kAEDescSize = 8;

struct AeHandler {
  GuestAddr upp = 0;
  u32 refcon = 0;
};
std::map<std::pair<u32, u32>, AeHandler> g_handlers;
bool g_launchDelivered = false;

const AeHandler* findHandler(u32 cls, u32 id) {
  auto it = g_handlers.find({cls, id});
  if (it != g_handlers.end()) return &it->second;
  // A handler installed for typeWildCard answers for anything it covers.
  constexpr u32 kAny = 0x2A2A2A2Au;             // '****'
  for (const auto& [key, h] : g_handlers)
    if ((key.first == cls || key.first == kAny) &&
        (key.second == id || key.second == kAny))
      return &h;
  return nullptr;
}

// Resolves a routine descriptor, or a bare transition vector, to the code and
// TOC a nested guest call needs. This is the same unwrapping CallUniversalProc
// does; the handler is native PowerPC, so no 68k emulation is involved.
}  // namespace

// Shared with the List Manager, which calls the application's own list
// definition procedure through a descriptor stored in the list record.
bool resolveUpp(Mem& m, GuestAddr upp, GuestAddr* code, GuestAddr* toc) {
  if (!upp) return false;
  GuestAddr tvector = upp;
  if (m.r32(upp) == 0xAAFE0000u) tvector = m.r32(upp + 4);
  if (!tvector) return false;
  *code = m.r32(tvector);
  *toc = m.r32(tvector + 4);
  return *code >= layout::kCodeBase && *code < layout::kHeapBase;
}

// Reports the launch event to the Event Manager, once, as soon as the
// application both asks for high-level events and has a handler able to
// receive one. Waiting for the handler matters: an event delivered before
// AEInstallEventHandler ran would be answered with errAEEventNotHandled and
// the application would never start up.
bool nextAppleEvent(u16 mask, MacEvent* out) {
  if (g_launchDelivered || !(mask & kHighLevelEventMask)) return false;
  if (!findHandler(kCoreEventClass, kAEOpenApplication)) return false;
  g_launchDelivered = true;
  out->what = kHighLevelEvent;
  out->message = kCoreEventClass;
  // For a high-level event the where field carries the event ID rather than a
  // mouse position.
  out->whereV = s16(kAEOpenApplication >> 16);
  out->whereH = s16(kAEOpenApplication & 0xFFFF);
  out->when = macTicks();
  out->modifiers = Display::get().modifiers();
  return true;
}

void registerAppleEvents(Toolbox& tb) {
  tb.add("AEInstallEventHandler", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u32 cls = a.u32v();
    const u32 id = a.u32v();
    AeHandler h;
    h.upp = a.ptr();
    h.refcon = a.u32v();
    a.u8v();                            // isSysHandler: there is no system table
    g_handlers[{cls, id}] = h;
    Toolbox::ret(c, 0);
  });
  tb.add("AERemoveEventHandler", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u32 cls = a.u32v();
    const u32 id = a.u32v();
    g_handlers.erase({cls, id});
    Toolbox::ret(c, 0);
  });

  tb.add("AEProcessAppleEvent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Mem& m = tb.mem();
    GuestAddr evt = a.ptr();
    if (!evt) { Toolbox::ret(c, u32(s32(kErrAEEventNotHandled))); return; }
    const u32 cls = m.r32(evt + 2);                  // message
    const u32 id = m.r32(evt + 10);                  // where, as a long
    const AeHandler* h = findHandler(cls, id);
    GuestAddr code = 0, toc = 0;
    if (!h || !resolveUpp(m, h->upp, &code, &toc)) {
      Toolbox::ret(c, u32(s32(kErrAEEventNotHandled)));
      return;
    }
    // The two descriptors the handler is passed. There is no real event data
    // behind them -- the launch event carries none -- so the event describes
    // itself as an Apple event with an empty data handle, and the reply is the
    // null descriptor a handler that answers nothing leaves alone.
    static GuestAddr descs = 0;
    if (!descs) {
      descs = tb.heap().newPtr(kAEDescSize * 2, true);
      if (!descs) { Toolbox::ret(c, u32(s32(kErrAEEventNotHandled))); return; }
    }
    const GuestAddr theEvent = descs, reply = descs + kAEDescSize;
    m.w32(theEvent + 0, kTypeAppleEvent);
    m.w32(theEvent + 4, 0);
    m.w32(reply + 0, kTypeNull);
    m.w32(reply + 4, 0);
    // Worth saying out loud once: this is the call that starts the game, and if
    // it ever stops happening the port will sit at its splash again.
    std::fprintf(stderr,
                 "  [AppleEvent] dispatching '%c%c%c%c' to the application's "
                 "own handler\n",
                 char(id >> 24), char(id >> 16), char(id >> 8), char(id));
    // No instruction budget: this handler is where Cythera does its start-up,
    // world loading and all, so it legitimately runs for as long as the rest of
    // the program would. The interpreter's own overall limit still applies.
    const u32 err =
        tb.interp().callPpc(code, toc, {theEvent, reply, h->refcon}, 0);
    // The handler returns an OSErr, which occupies the low half of r3.
    Toolbox::ret(c, u32(s32(s16(err & 0xFFFF))));
  });

  // The rest of the suite. Every accessor reports "no such descriptor", which
  // is not a shortcut: the launch event genuinely has no parameters, and
  // reporting their absence is how CheckAppleEventForMissingParams concludes
  // that nothing is missing. Opening a document from the Finder would need
  // these to be real; the port is launched directly instead.
  constexpr s32 kErrAEDescNotFound = -1701;
  for (const char* absent : {"AEGetParamDesc", "AEGetParamPtr", "AECountItems",
                             "AEGetNthDesc", "AEGetNthPtr", "AEGetAttributePtr",
                             "AESizeOfNthItem", "AECreateDesc"})
    tb.add(absent, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, u32(kErrAEDescNotFound));
    });
  tb.add("AEDisposeDesc", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0);
  });
  tb.add("AESetInteractionAllowed", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
