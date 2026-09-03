#include "toolbox.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

#include "mac/heap.h"
#include "pef.h"

namespace cyt {

// Each manager registers its own handlers; declared here rather than in the
// header so that adding a manager does not force a rebuild of everything.
void registerMemoryManager(Toolbox&);
void registerResourceManager(Toolbox&);
void registerToolboxCore(Toolbox&);
void registerMenuManager(Toolbox&);
void registerFileManager(Toolbox&);
void registerStandardFile(Toolbox&);
void registerQuickDrawDevices(Toolbox&);
void registerThreadManager(Toolbox&);
void registerEventManager(Toolbox&);
void registerSoundManager(Toolbox&);
void registerQuickTime(Toolbox&);
void registerQuickDrawDrawing(Toolbox&);
void registerWindowManager(Toolbox&);
void registerDialogManager(Toolbox&);
void registerControlManager(Toolbox&);
void registerListManager(Toolbox&);
void registerVideoDriver(Toolbox&);
void registerFontManager(Toolbox&);
void registerAppleEvents(Toolbox&);
void threadDidExit(Toolbox&, PpcCpu&);
GuestAddr threadExitAddress();

Toolbox::Toolbox(Mem& mem, Interp& interp)
    : mem_(mem), interp_(interp), heap_(std::make_unique<Heap>(mem)) {}
Toolbox::~Toolbox() = default;

void Toolbox::add(const char* name, Handler h) {
  if (!handlers_.emplace(name, std::move(h)).second)
    std::fprintf(stderr,
                 "  [tb] two managers both claim \"%s\"; the first one wins\n",
                 name);
}

bool Toolbox::bind(const PefImage& img, std::string* err) {
  registerAll();

  const auto& syms = img.importSyms();
  names_.reserve(syms.size());
  libs_.reserve(syms.size());
  for (const auto& s : syms) {
    names_.push_back(s.name);
    libs_.push_back(s.library);
  }

  bound_.resize(syms.size());
  for (size_t i = 0; i < syms.size(); ++i) {
    bound_[i].name = &names_[i];
    bound_[i].weak = syms[i].weak;
    auto it = handlers_.find(syms[i].name);
    bound_[i].fn = (it == handlers_.end()) ? nullptr : &it->second;
  }
  return true;
}

void Toolbox::call(u32 index, PpcCpu& cpu) {
  // Indices past the import table are this port's own trampolines rather than
  // imported symbols. 2000 is the address a thread's entry procedure returns to.
  if (index == 2000) {
    threadDidExit(*this, cpu);
    return;
  }
  if (index >= bound_.size()) {
    interp_.halt("native call with an out-of-range import index");
    return;
  }
  Entry& e = bound_[index];
  ++e.calls;

  Args args(cpu, mem_);
  if (traceCalls_) {
    std::fprintf(stderr, "  [tb] %-28s (r3=%08x r4=%08x r5=%08x r6=%08x)\n",
                 e.name->c_str(), cpu.gpr[3], cpu.gpr[4], cpu.gpr[5],
                 cpu.gpr[6]);
  }

  if (e.fn) {
    (*e.fn)(*this, cpu, args);
    return;
  }

  // No handler yet: record it, return a benign zero, and keep going so that one
  // trace reveals as many distinct missing calls as possible rather than
  // stopping at the first.
  ++unimplemented_;
  ++missing_[*e.name];
  if (!traceCalls_)
    std::fprintf(stderr, "  [tb] UNIMPLEMENTED %-26s (r3=%08x r4=%08x r5=%08x)\n",
                 e.name->c_str(), cpu.gpr[3], cpu.gpr[4], cpu.gpr[5]);
  ret(cpu, 0);
  if (stopAfter_ && unimplemented_ >= stopAfter_)
    interp_.halt("stopping after " + std::to_string(unimplemented_) +
                 " unimplemented Toolbox calls");
}

void Toolbox::dumpCallStats(std::FILE* out) const {
  std::vector<std::pair<u64, std::string>> hit, miss;
  for (const auto& e : bound_)
    if (e.calls) (e.fn ? hit : miss).emplace_back(e.calls, *e.name);
  std::sort(hit.rbegin(), hit.rend());
  std::sort(miss.rbegin(), miss.rend());

  std::fprintf(out, "\nToolbox calls served (%zu distinct):\n", hit.size());
  for (const auto& [n, name] : hit)
    std::fprintf(out, "  %8llu  %s\n", (unsigned long long)n, name.c_str());
  std::fprintf(out, "\nToolbox calls still missing (%zu distinct, %llu total):\n",
               miss.size(), (unsigned long long)unimplemented_);
  for (const auto& [n, name] : miss)
    std::fprintf(out, "  %8llu  %s\n", (unsigned long long)n, name.c_str());
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void Toolbox::registerAll() {
  registerMemoryManager(*this);
  registerResourceManager(*this);
  registerToolboxCore(*this);
  registerMenuManager(*this);
  registerFileManager(*this);
  registerStandardFile(*this);
  registerQuickDrawDevices(*this);
  registerThreadManager(*this);
  registerEventManager(*this);
  registerSoundManager(*this);
  registerQuickTime(*this);
  registerQuickDrawDrawing(*this);
  registerWindowManager(*this);
  registerDialogManager(*this);
  registerControlManager(*this);
  registerListManager(*this);
  registerVideoDriver(*this);
  // After QuickDraw: the Font Manager owns the text calls and InitFonts.
  registerFontManager(*this);
  registerAppleEvents(*this);

  // ExitToShell must actually stop the interpreter; returning from it would let
  // the application run on past its own termination path.
  add("ExitToShell", [](Toolbox& tb, PpcCpu& c, Args& a) {
    tb.interp().halt("the application called ExitToShell");
  });
  add("DebugStr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    std::fprintf(stderr, "  [DebugStr] %s\n",
                 tb.mem().pstr(a.ptr()).c_str());
  });

  // Gestalt is how a 1999 application decides which of its own code paths to
  // take, so these answers shape the whole run. The rule applied here: report
  // present only what this port actually implements, and report everything else
  // with gestaltUndefSelectorErr so the game falls back to the code it already
  // carries for older systems. Answering "present" for something unimplemented
  // is far worse than answering "absent" -- the application would then call it.
  add("Gestalt", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u32 selector = a.u32v();
    const GuestAddr responsePtr = a.ptr();
    auto reply = [&](u32 v) {
      if (responsePtr) tb.mem().w32(responsePtr, v);
      ret(c, 0);
    };
    // gestaltUndefSelectorErr: the feature does not exist on this machine.
    auto absent = [&] { ret(c, u32(s32(-5551))); };

    switch (selector) {
      case 0x73797376:  // 'sysv' -- system version in BCD; report Mac OS 9.0.4
        reply(0x0904);
        break;
      case 0x71642020:  // 'qd  ' -- 32-bit Color QuickDraw 1.3
        reply(0x0230);
        break;
      case 0x74686473:  // 'thds' -- Thread Manager. Cythera imports ThreadsLib
        // weakly but refuses to start without it, and checks here rather than by
        // testing the imported symbol's address. Bit 0 is the manager itself and
        // bit 2 the shared library; both are implemented.
        reply(0x5);
        break;
      case 0x65766E74:  // 'evnt' -- bit 0 says WaitNextEvent exists
        reply(0x1);
        break;
      case 0x736E6420:  // 'snd ' -- stereo, stereo mixing, and 16-bit output
        reply(0x2B);
        break;
      case 0x63707574:  // 'cput' -- native CPU type; 0x108 is the PowerPC 750
        reply(0x108);
        break;
      case 0x70726F63:  // 'proc' -- the 68k processor type this would emulate
        reply(5);       // gestaltCPU68040, the value a PowerPC Mac reported
        break;
      case 0x7174696D:  // 'qtim' -- QuickTime version, needed for the music
        reply(0x03008000);   // 3.0 final, in QuickTime's version format
        break;
      case 0x71747273:  // 'qtrs' -- QuickTime features. Bit 0 says the PowerPC
        reply(0x1);     // QuickTime library exists, which a native app requires
        break;
      // Deliberately absent. Appearance and Contextual Menus make the game use
      // its own T7Widget drawing; QuickTime, the Component Manager and audio CD
      // support gate features this port does not provide; 'bugx' is Ambrosia's
      // crash reporter.
      case 0x61707072:  // 'appr'
      case 0x636D6E75:  // 'cmnu'
      case 0x6275677A:  // 'bugz'
      case 0x62756778:  // 'bugx'
      case 0x61756364:  // 'aucd'
      case 0x63706E74:  // 'cpnt'
      default:
        absent();
        break;
    }
  });
}

}  // namespace cyt
