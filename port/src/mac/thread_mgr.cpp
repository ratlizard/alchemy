// Thread Manager.
//
// Cythera imports ThreadsLib weakly but refuses to run without it, so this is a
// real cooperative scheduler rather than a stub. Cooperative threading is a
// natural fit for an interpreter: a thread *is* a saved processor state plus a
// stack, and a yield is just swapping which state the interpreter is stepping.
// Nothing preempts, so no locking is needed anywhere -- which is exactly the
// guarantee the application was written against.
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include "mac/heap.h"
#include "toolbox.h"

namespace cyt {

namespace {

// Thread states, as the Thread Manager defines them.
constexpr u16 kReadyThreadState   = 0;
constexpr u16 kStoppedThreadState = 1;
constexpr u16 kRunningThreadState = 2;

// Option bits for NewThread.
constexpr u32 kNewSuspend = 1u << 0;

[[maybe_unused]] constexpr s16 kNoErr            = 0;
constexpr s16 kThreadNotFound   = -617;
constexpr s16 kThreadProtocolErr= -619;
constexpr s16 kMemFullErr       = -108;

constexpr u32 kDefaultStackSize = 96 * 1024;

struct Thread {
  u32 id = 0;
  PpcCpu state;              // valid whenever the thread is not running
  GuestAddr stack = 0;       // base of its allocated stack block
  u32 stackSize = 0;
  u16 threadState = kReadyThreadState;
  bool terminated = false;
  u64 switches = 0;          // how many times the scheduler resumed this thread
  GuestAddr resultPtr = 0;   // where NewThread's caller wants the result
};

std::vector<Thread> g_threads;
u64 g_yields = 0, g_pickedNone = 0, g_switches = 0, g_newThreadAt = 0;
u32 g_current = 0;           // id of the running thread; 1 is the application
u32 g_nextId = 1;
GuestAddr g_scheduler = 0;   // a custom scheduler UPP, if one was installed

Thread* find(u32 id) {
  for (auto& t : g_threads)
    if (t.id == id && !t.terminated) return &t;
  return nullptr;
}

// Ensures the application's own execution counts as thread 1, so that
// GetCurrentThread is meaningful before any thread is created.
Thread& mainThread() {
  if (g_threads.empty()) {
    Thread t;
    t.id = g_nextId++;
    t.threadState = kRunningThreadState;
    g_threads.push_back(t);
    g_current = t.id;
  }
  return g_threads.front();
}

// Round-robin over the ready threads, starting after the current one. Returns 0
// when nothing else can run.
u32 pickNext(u32 preferred) {
  mainThread();
  ++g_yields;
  if (preferred) {
    Thread* t = find(preferred);
    if (t && t->threadState != kStoppedThreadState) return preferred;
    return 0;
  }
  size_t start = 0;
  for (size_t i = 0; i < g_threads.size(); ++i)
    if (g_threads[i].id == g_current) { start = i; break; }
  for (size_t k = 1; k <= g_threads.size(); ++k) {
    Thread& t = g_threads[(start + k) % g_threads.size()];
    if (!t.terminated && t.threadState != kStoppedThreadState &&
        t.id != g_current)
      return t.id;
  }
  ++g_pickedNone;
  return 0;
}

// Swaps the interpreter onto another thread. The caller has already decided
// that `to` is runnable.
void switchTo(Toolbox& tb, PpcCpu& c, u32 to) {
  Thread* from = find(g_current);
  Thread* dest = find(to);
  if (!dest || to == g_current) return;
  if (from) {
    from->state = c;
    if (from->threadState == kRunningThreadState)
      from->threadState = kReadyThreadState;
  }
  // The yield call itself must appear to return, so resume the outgoing thread
  // after its trampoline rather than back inside it.
  if (from) from->state.pc = c.lr;
  // The retired-instruction count belongs to the machine, not to a thread:
  // carrying a thread's own counter across a switch would make it jump around
  // and break every budget and progress check built on it.
  const u64 retired = c.instructions;
  c = dest->state;
  c.instructions = retired;
  dest->threadState = kRunningThreadState;
  ++dest->switches;
  ++g_switches;
  g_current = to;
  c.branched_ = true;    // the new state's pc is authoritative
}


}  // namespace

// Reports how the scheduler actually behaved, which is the only way to tell a
// starved thread from a thread that is running but idle.
void dumpThreadStats() {
  if (!std::getenv("CYT_DEBUG_THREADS")) return;
  std::fprintf(stderr,
               "  [threads] %zu thread(s), current=%u; scheduler queries=%llu "
               "(no candidate %llu), switches=%llu, NewThread at instruction "
               "%llu\n",
               g_threads.size(), g_current, (unsigned long long)g_yields,
               (unsigned long long)g_pickedNone,
               (unsigned long long)g_switches,
               (unsigned long long)g_newThreadAt);
  for (const auto& t : g_threads)
    std::fprintf(stderr,
                 "  [threads]   id=%u state=%u terminated=%d switches=%llu "
                 "pc=%08x stack=%08x\n",
                 t.id, t.threadState, t.terminated ? 1 : 0,
                 (unsigned long long)t.switches, t.state.pc, t.stack);
}

// The address a thread returns to when its entry procedure finishes. It lives in
// the shim range so the interpreter routes it here as a native call.
constexpr u32 kThreadExitIndex = 2000;
GuestAddr threadExitAddress() {
  return Interp::kShimCodeBase + kThreadExitIndex * 4;
}

// Called by Toolbox::call when a thread's entry procedure returns.
void threadDidExit(Toolbox& tb, PpcCpu& c) {
  Thread* t = find(g_current);
  if (t) {
    t->terminated = true;
    t->threadState = kStoppedThreadState;
    if (t->stack) tb.heap().disposePtr(t->stack);
    t->stack = 0;
    if (t->resultPtr) tb.mem().w32(t->resultPtr, c.gpr[3]);
  }
  u32 next = pickNext(0);
  if (!next) {
    // Nothing left to run. If the application's own thread is gone the program
    // is over; say so rather than stepping into freed memory.
    tb.interp().halt("every thread has terminated");
    return;
  }
  Thread* dest = find(next);
  const u64 retired = c.instructions;
  c = dest->state;
  c.instructions = retired;
  dest->threadState = kRunningThreadState;
  g_current = next;
  c.branched_ = true;
}

void registerThreadManager(Toolbox& tb) {
  tb.add("NewThread", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 style = a.u32v();            // 1 = cooperative, 2 = preemptive
    GuestAddr entry = a.ptr();       // ThreadEntryProcPtr (a transition vector)
    GuestAddr param = a.ptr();
    u32 stackSize = a.u32v();
    u32 options = a.u32v();
    GuestAddr resultPtr = a.ptr();
    GuestAddr idOut = a.ptr();

    mainThread();
    // Only cooperative threads exist here; a preemptive request is refused
    // rather than silently downgraded, since the caller's locking would differ.
    if (style != 1) {
      Toolbox::ret(c, u32(s32(kThreadProtocolErr)));
      return;
    }
    if (!stackSize) stackSize = kDefaultStackSize;
    GuestAddr stack = tb.heap().newPtr(stackSize, false);
    if (!stack) {
      Toolbox::ret(c, u32(s32(kMemFullErr)));
      return;
    }

    Thread t;
    t.id = g_nextId++;
    t.stack = stack;
    t.stackSize = stackSize;
    t.resultPtr = resultPtr;
    t.threadState = (options & kNewSuspend) ? kStoppedThreadState
                                           : kReadyThreadState;

    // Build the thread's initial processor state: a fresh stack frame at the top
    // of its block, the entry procedure's transition vector loaded, its
    // parameter in r3, and a return address that routes back into this file.
    GuestAddr sp = (stack + stackSize - 64) & ~15u;
    tb.mem().w32(sp, 0);            // null back chain terminates the frame list
    t.state = c;                    // inherit nothing meaningful but be defined
    for (auto& r : t.state.gpr) r = 0;
    t.state.gpr[1] = sp;
    t.state.gpr[2] = tb.mem().r32(entry + 4);   // the entry's TOC
    t.state.gpr[3] = param;
    t.state.pc = tb.mem().r32(entry + 0);
    t.state.lr = threadExitAddress();
    t.state.cr = 0;
    t.state.xer = 0;
    t.state.instructions = 0;

    g_threads.push_back(t);
    g_newThreadAt = c.instructions;
    if (idOut) tb.mem().w32(idOut, t.id);
    Toolbox::ret(c, 0);
  });

  tb.add("GetCurrentThread", [](Toolbox& tb, PpcCpu& c, Args& a) {
    mainThread();
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, g_current);
    Toolbox::ret(c, 0);
  });

  tb.add("GetThreadState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 id = a.u32v();
    GuestAddr out = a.ptr();
    mainThread();
    if (id == 0) id = g_current;
    Thread* t = find(id);
    if (!t) { Toolbox::ret(c, u32(s32(kThreadNotFound))); return; }
    if (out) tb.mem().w16(out, t->threadState);
    Toolbox::ret(c, 0);
  });

  tb.add("SetThreadState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 id = a.u32v();
    u16 newState = a.u16v();
    u32 suspendId = a.u32v();
    mainThread();
    if (id == 0) id = g_current;
    Thread* t = find(id);
    if (!t) { Toolbox::ret(c, u32(s32(kThreadNotFound))); return; }
    t->threadState = newState;
    Toolbox::ret(c, 0);
    // Stopping the running thread has to take effect immediately.
    if (newState == kStoppedThreadState && id == g_current) {
      u32 next = pickNext(suspendId);
      if (next) switchTo(tb, c, next);
      else tb.interp().halt("the only runnable thread stopped itself");
    }
  });

  tb.add("YieldToAnyThread", [](Toolbox& tb, PpcCpu& c, Args& a) {
    mainThread();
    Toolbox::ret(c, 0);
    u32 next = pickNext(0);
    if (next) switchTo(tb, c, next);
    // With nothing else ready, yielding correctly returns to the caller.
  });

  tb.add("YieldToThread", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 id = a.u32v();
    mainThread();
    Toolbox::ret(c, 0);
    u32 next = pickNext(id);
    if (next) switchTo(tb, c, next);
  });

  tb.add("SetThreadScheduler", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // A custom scheduler picks the next thread on every yield. Round-robin over
    // the ready threads is a valid schedule for any cooperative program, so the
    // procedure is recorded but the built-in order is kept: calling back into
    // the guest from inside a context switch would need the switch to be
    // re-entrant, and nothing the game does depends on the choice.
    g_scheduler = a.ptr();
    Toolbox::ret(c, 0);
  });

  // Critical sections cannot be interrupted when nothing preempts.
  for (const char* nop : {"ThreadBeginCritical", "ThreadEndCritical"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });
  tb.add("DisposeThread", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 id = a.u32v();
    a.ptr(); a.u8v();
    Thread* t = find(id);
    if (!t) { Toolbox::ret(c, u32(s32(kThreadNotFound))); return; }
    t->terminated = true;
    if (t->stack) tb.heap().disposePtr(t->stack);
    t->stack = 0;
    Toolbox::ret(c, 0);
  });
}

}  // namespace cyt
