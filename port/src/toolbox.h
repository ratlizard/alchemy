// The host side of the Mac OS Toolbox: one native handler per PEF import.
//
// Cythera imports 563 symbols, but only InterfaceLib's 498 are non-weak. The
// nine weak libraries (Appearance, QuickTime, InputSprocket, Navigation,
// Threads, ControlStrip, ContextualMenu, Sound, Math) are deliberately reported
// absent, which makes the game fall back to the drawing and input code it
// already carries for pre-8.5 systems -- the T7Widget class family. That cuts
// the surface that has to be emulated to one library.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mem.h"
#include "ppc.h"

namespace cyt {

class PefImage;

// Reads a PowerPC CFM argument list: r3..r10 first, then the caller's stack
// frame at r1+56. Widened types are read as the callee would see them.
class Args {
 public:
  Args(PpcCpu& cpu, Mem& mem) : cpu_(cpu), mem_(mem) {}

  u32 u32v() {
    if (next_ <= 10) return cpu_.gpr[next_++];
    u32 v = mem_.r32(cpu_.gpr[1] + 56 + stack_);
    stack_ += 4;
    return v;
  }
  // Narrow integers still occupy a whole parameter word, right-justified.
  u16 u16v() { return u16(u32v()); }
  s16 s16v() { return s16(u32v()); }
  u8  u8v()  { return u8(u32v()); }
  s32 s32v() { return s32(u32v()); }
  GuestAddr ptr() { return u32v(); }
  double dblv() { return cpu_.fpr[fpr_++]; }

 private:
  PpcCpu& cpu_;
  Mem& mem_;
  u32 next_ = 3;      // next GPR to consume
  u32 fpr_ = 1;       // next FPR to consume
  u32 stack_ = 0;
};

class Toolbox {
 public:
  using Handler = std::function<void(Toolbox&, PpcCpu&, Args&)>;

  Toolbox(Mem& mem, Interp& interp);
  ~Toolbox();

  // Associates every import with a handler, or with the tracing fallback.
  bool bind(const PefImage& img, std::string* err);

  // Invoked by the interpreter when the program counter enters the shim area.
  void call(u32 importIndex, PpcCpu& cpu);

  // Return-value helpers, matching the ABI's single integer return register.
  static void ret(PpcCpu& c, u32 v) { c.gpr[3] = v; }

  Mem& mem() { return mem_; }
  Interp& interp() { return interp_; }

  // Diagnostics: what got called, how often, and what is still missing.
  void dumpCallStats(std::FILE* out) const;
  u64 unimplementedCalls() const { return unimplemented_; }
  void setStopOnUnimplemented(u32 n) { stopAfter_ = n; }
  void setTraceCalls(bool on) { traceCalls_ = on; }

  // Registers one handler under its InterfaceLib name. Public so that each
  // manager can live in its own translation unit.
  //
  // Two managers claiming the same name is always a mistake -- the later
  // registration silently replaces the earlier one, and the symptom is a call
  // that reaches a handler nobody expected. It cost an hour once; it now says
  // so instead.
  void add(const char* name, Handler h);

  // ---- Toolbox state shared between managers -----------------------------
  s16 memErr = 0;                 // what MemError() will report

  class Heap& heap() { return *heap_; }

 private:
  void registerAll();

  struct Entry {
    const std::string* name = nullptr;
    bool weak = false;
    Handler* fn = nullptr;
    u64 calls = 0;
  };

  Mem& mem_;
  Interp& interp_;
  std::unordered_map<std::string, Handler> handlers_;
  std::vector<Entry> bound_;
  std::vector<std::string> names_;
  std::vector<std::string> libs_;
  std::unique_ptr<class Heap> heap_;
  u64 unimplemented_ = 0;
  u32 stopAfter_ = 0;
  bool traceCalls_ = false;
  std::unordered_map<std::string, u64> missing_;
};

}  // namespace cyt
