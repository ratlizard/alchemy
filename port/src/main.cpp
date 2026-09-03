// Entry point for the Cythera port.
//
// Loads the PEF container, builds the Toolbox, and either reports what the
// loader produced or starts interpreting the game's own code.
//
// A note on the diagnostic options, because it is easy to reach for the wrong
// one: input is scheduled against the application's *event-loop pass count*, not
// against an instruction count. Cythera's start-up delay loops poll TickCount,
// so they consume however many instructions the host burns while real time
// passes, and an instruction count therefore lands somewhere different on every
// run. Pass counts are stable.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "host/display.h"
#include "mac/file_mgr.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "mac/video_driver.h"
#include "mem.h"
#include "pef.h"
#include "ppc.h"
#include "toolbox.h"

namespace cyt {
bool openApplicationResources(const std::string& path, std::string* err);
void dumpThreadStats();
u64 eventLoopPasses();
void setHostPumpHook(std::function<void()> fn);
}  // namespace cyt

using namespace cyt;

namespace {

bool readFile(const std::string& path, std::vector<u8>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  out->resize(size_t(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size()));
  return true;
}

void reportImage(const PefImage& img) {
  static const char* kKindName[] = {"code", "unpackedData", "patternData",
                                    "constant", "loader", "debug", "execData",
                                    "exception", "traceback"};
  std::printf("loaded PEF (built %08x)\n", img.dateStamp());
  for (const auto& s : img.sections()) {
    unsigned k = unsigned(s.kind);
    std::printf("  %-14s guest 0x%08x  total %-8u container %u%s\n",
                k < 9 ? kKindName[k] : "?", s.loadedAt, s.totalSize,
                s.containerOffset, s.instantiated ? "" : "  (not loaded)");
  }
  std::printf("code:  0x%08x .. 0x%08x\n", img.codeBase(),
              img.codeBase() + img.codeSize());
  std::printf("entry: tvector 0x%08x -> code 0x%08x, toc 0x%08x\n",
              img.entryTVector(), img.entryCode(), img.entryToc());
  std::printf("imports: %zu symbols in %zu libraries\n",
              img.importSyms().size(), img.importLibs().size());
}

// Writes the framebuffer as indexed pixels followed by a 256-entry palette, for
// tools/screen_to_png.py. Lets the port be checked without a window server.
void writeFramebuffer(const char* path) {
  const GraphicsWorld& gw = graphics();
  if (!gw.screenBits) {
    std::fprintf(stderr, "cythera: no framebuffer to dump\n");
    return;
  }
  std::vector<u8> buf(size_t(gw.rowBytes) * size_t(kScreenHeight));
  Mem::get().copyOut(buf.data(), gw.screenBits, buf.size());
  Palette pal = readPalette(Mem::get(), gw.colorTableH);
  std::ofstream out(path, std::ios::binary);
  auto be16 = [&out](u16 v) {
    out.put(char(v >> 8)).put(char(v & 0xFF));
  };
  be16(u16(kScreenWidth));
  be16(u16(kScreenHeight));
  be16(u16(gw.rowBytes));
  out.write(reinterpret_cast<const char*>(buf.data()),
            std::streamsize(buf.size()));
  // The gamma ramp is applied here as well as in the window, so that a dump
  // taken mid-fade looks like what a person would have been seeing. Without
  // this a headless run cannot show the fade at all, because nothing headless
  // ever reaches Display::present.
  const GammaRamp& g = gammaRamp();
  for (u32 i = 0; i < 256; ++i) {
    Rgb e = i < pal.entries.size() ? pal.entries[i] : Rgb{};
    u32 r8 = e.r >> 8, g8 = e.g >> 8, b8 = e.b >> 8;
    if (!g.identity) { r8 = g.red[r8]; g8 = g.green[g8]; b8 = g.blue[b8]; }
    out.put(char(r8)).put(char(g8)).put(char(b8));
  }
  std::printf("wrote framebuffer to %s\n", path);
}

// Every call the application makes to an imported routine goes through a
// cross-TOC glue stub: six instructions that load a transition vector out of
// the TOC and jump through it. A static disassembly therefore shows a branch to
// an unnamed address rather than to "GetNewDialog", which makes reading any
// function's Toolbox usage far harder than it should be.
//
// The stubs are recognisable, and after relocation their TOC slot holds the
// import's synthetic transition vector -- so the address of every stub can be
// paired with the name of the import it reaches. Writing that out lets
// tools/pefdisasm.py annotate branches, which is how a function's behaviour can
// be read before any of it has run.
void writeGlueMap(const PefImage& img, const char* path) {
  Mem& m = Mem::get();
  std::map<GuestAddr, const std::string*> byTVector;
  for (const auto& s : img.importSyms())
    if (s.slot) byTVector[s.slot] = &s.name;

  std::ofstream out(path);
  if (!out) {
    std::fprintf(stderr, "cythera: cannot write %s\n", path);
    return;
  }
  const GuestAddr toc = img.entryToc();
  const GuestAddr base = img.codeBase(), end = base + img.codeSize();
  u32 found = 0;
  for (GuestAddr a = base; a + 24 <= end; a += 4) {
    // lwz r12,d(r2) ; stw r2,20(r1) ; lwz r0,0(r12) ; lwz r2,4(r12) ;
    // mtctr r0 ; bctr
    if ((m.r32(a) & 0xFFFF0000u) != 0x81820000u) continue;
    if (m.r32(a + 4) != 0x90410014u || m.r32(a + 8) != 0x800C0000u ||
        m.r32(a + 12) != 0x804C0004u || m.r32(a + 16) != 0x7C0903A6u ||
        m.r32(a + 20) != 0x4E800420u)
      continue;
    const GuestAddr slot = toc + u32(s32(s16(m.r32(a) & 0xFFFF)));
    auto it = byTVector.find(m.r32(slot));
    if (it == byTVector.end()) continue;
    out << std::hex << (a - base) << " " << *it->second << "\n";
    ++found;
  }
  std::printf("wrote %u glue stubs to %s\n", found, path);
}

struct Options {
  const char* dataFork = nullptr;
  const char* symbols = nullptr;
  const char* rsrcFork = nullptr;
  const char* gameDir = nullptr;
  const char* supportDir = nullptr;
  const char* dumpScreen = nullptr;
  const char* dumpGlue = nullptr;
  bool run = false;
  bool traceInstr = false;
  bool traceCalls = false;
  bool headless = false;
  int scale = 2;
  u64 budget = 2'000'000;
  u32 stopAfter = 40;
  u64 profileEvery = 0;
  u64 traceFrom = 0;
  // Input delivered once the application has looped this many times.
  u64 atPass = 0;
  bool passClick = false;
  char passKey = 0;
  s16 passH = 320, passV = 240;
};

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s <Cythera.data> [options]\n"
      "\n"
      "  <Cythera.data> is the application's data fork, as produced by\n"
      "  binhex_decode.py from Cythera.hqx.\n"
      "\n"
      "  --symbols FILE        cythera_symbols.txt, for named traces\n"
      "  --rsrc FILE           Cythera.rsrc, the application's resource fork\n"
      "  --game-dir DIR        where \"Cythera Data\" and the forks live\n"
      "  --support-dir DIR     where saved games and preferences are written\n"
      "  --run                 interpret, rather than only reporting the image\n"
      "  --headless            do not open a window\n"
      "  --scale N             window scale over 640x480 (default 2)\n"
      "  --budget N            stop after N instructions (0 = unlimited)\n"
      "  --dump-screen FILE    write the framebuffer at exit\n"
      "  --dump-glue FILE      write 'stub-address import-name' for every\n"
      "                        cross-TOC glue stub, for pefdisasm.py\n"
      "\n"
      "  diagnostics:\n"
      "  --trace               log every interpreted instruction (very verbose)\n"
      "  --trace-calls         log every Toolbox call with its arguments\n"
      "  --trace-from N        run quietly to instruction N, then trace calls\n"
      "  --profile N           sample the program counter every N instructions\n"
      "  --stop-after N        halt after N unimplemented Toolbox calls\n"
      "  --click-at-pass N[:H,V]  click once the game has looped N times\n"
      "  --cmd-key-at-pass N:C    press command-C once it has looped N times\n",
      argv0);
}

bool parse(int argc, char** argv, Options* o) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (a == "--run") o->run = true;
    else if (a == "--trace") { o->traceInstr = true; o->run = true; }
    else if (a == "--trace-calls") { o->traceCalls = true; o->run = true; }
    else if (a == "--headless") o->headless = true;
    else if (a == "--symbols") o->symbols = next();
    else if (a == "--rsrc") o->rsrcFork = next();
    else if (a == "--game-dir") o->gameDir = next();
    else if (a == "--support-dir") o->supportDir = next();
    else if (a == "--dump-screen") o->dumpScreen = next();
    else if (a == "--dump-glue") o->dumpGlue = next();
    else if (a == "--scale") { if (const char* v = next()) o->scale = std::atoi(v); }
    else if (a == "--budget") { if (const char* v = next()) o->budget = std::strtoull(v, nullptr, 0); }
    else if (a == "--stop-after") { if (const char* v = next()) o->stopAfter = u32(std::strtoul(v, nullptr, 0)); }
    else if (a == "--profile") { if (const char* v = next()) o->profileEvery = std::strtoull(v, nullptr, 0); }
    else if (a == "--trace-from") { if (const char* v = next()) o->traceFrom = std::strtoull(v, nullptr, 0); }
    else if (a == "--click-at-pass" || a == "--cmd-key-at-pass") {
      const char* v = next();
      if (!v) return false;
      const std::string spec = v;
      const size_t colon = spec.find(':');
      o->atPass = std::strtoull(spec.substr(0, colon).c_str(), nullptr, 0);
      if (a == "--click-at-pass") {
        o->passClick = true;
        if (colon != std::string::npos) {
          const size_t comma = spec.find(',', colon);
          o->passH = s16(std::atoi(spec.substr(colon + 1).c_str()));
          if (comma != std::string::npos)
            o->passV = s16(std::atoi(spec.substr(comma + 1).c_str()));
        }
      } else if (colon != std::string::npos && colon + 1 < spec.size()) {
        o->passKey = spec[colon + 1];
      }
    }
    else if (a == "--help" || a == "-h") return false;
    else if (!o->dataFork) o->dataFork = argv[i];
    else o->symbols = argv[i];
  }
  return o->dataFork != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse(argc, argv, &opt)) {
    usage(argv[0]);
    return 2;
  }

  std::string err;
  if (!Mem::get().init(&err)) {
    std::fprintf(stderr, "cythera: %s\n", err.c_str());
    return 1;
  }
  std::vector<u8> blob;
  if (!readFile(opt.dataFork, &blob)) {
    std::fprintf(stderr, "cythera: cannot read %s\n", opt.dataFork);
    return 1;
  }
  PefImage img;
  if (!img.load(blob, &err)) {
    std::fprintf(stderr, "cythera: failed to load PEF: %s\n", err.c_str());
    return 1;
  }
  if (opt.symbols && !img.loadSymbolFile(opt.symbols, &err))
    std::fprintf(stderr, "cythera: %s (continuing without symbols)\n",
                 err.c_str());

  reportImage(img);
  if (opt.dumpGlue) writeGlueMap(img, opt.dumpGlue);
  if (!opt.run) return 0;

  // ---- build the machine --------------------------------------------------
  PpcCpu cpu;
  Interp interp(&cpu, &Mem::get());
  interp.setImage(&img);
  interp.setTrace(opt.traceInstr);

  Toolbox tb(Mem::get(), interp);
  if (!tb.bind(img, &err)) {
    std::fprintf(stderr, "cythera: %s\n", err.c_str());
    return 1;
  }
  if (opt.gameDir) FileMgr::get().setGameDir(opt.gameDir);
  FileMgr::get().setSupportDir(opt.supportDir ? opt.supportDir
                                             : "./cythera-saves");
  // The launcher opens the application's own resource fork before main runs, so
  // menus, dialogs and strings are already reachable by the time it starts.
  if (opt.rsrcFork && !openApplicationResources(opt.rsrcFork, &err))
    std::fprintf(stderr, "cythera: %s (continuing without resources)\n",
                 err.c_str());

  // Tracing that is scheduled starts off, and is switched on at its trigger.
  tb.setTraceCalls(opt.traceCalls && !opt.traceFrom && !opt.atPass);
  // CYT_TRACE_CALLS_FROM_PC turns tracing on when the interpreter reaches a
  // named function, which works inside a nested guest call where --trace-from
  // cannot reach.
  interp.setTraceArmHook([&tb] { tb.setTraceCalls(true); });
  tb.setStopOnUnimplemented(opt.stopAfter);
  interp.setHostCall([&tb](u32 i, PpcCpu& c) { tb.call(i, c); });

  if (opt.headless) {
    Display::get().openHeadless();
  } else {
    std::string derr;
    if (!Display::get().open(opt.scale, &derr)) {
      std::fprintf(stderr, "cythera: %s\n  continuing without a window\n",
                   derr.c_str());
    } else if (opt.budget == 2'000'000) {
      // With a window, run until it is closed rather than to a token budget.
      opt.budget = 0;
    }
  }

  // A PowerPC stack frame: r1 points at the frame, word 0 is the back chain
  // (null terminates it) and word 2 is where a callee spills its return
  // address. Sixteen-byte aligned, as the ABI requires.
  const GuestAddr sp = (layout::kStackTop - 256) & ~15u;
  Mem::get().w32(sp, 0);
  cpu.gpr[1] = sp;
  cpu.gpr[3] = 0;
  cpu.gpr[2] = Mem::get().r32(img.entryTVector() + 4);
  cpu.pc = Mem::get().r32(img.entryTVector());
  cpu.lr = kReturnSentinel;

  std::printf("\nrunning from 0x%08x (budget %llu instructions)\n",
              img.entryCode(), (unsigned long long)opt.budget);
  interp.setInstructionLimit(opt.budget);

  // Scheduled input, delivered at the port's frame boundary so that it reaches
  // the application wherever it happens to be -- including inside the Apple
  // event handler that runs its whole start-up, and inside a mouse-tracking
  // loop that will not return to the event loop until the button comes up.
  bool pressed = false, released = false;
  u64 pressedAt = 0;
  constexpr u64 kHoldInstructions = 2'000'000;
  setHostPumpHook([&] {
    if (!opt.atPass) return;
    if (!pressed && eventLoopPasses() >= opt.atPass) {
      pressed = true;
      pressedAt = cpu.instructions;
      if (opt.traceCalls) tb.setTraceCalls(true);
      if (opt.passClick) {
        std::printf("clicking at (%d, %d) on event-loop pass %llu\n", opt.passH,
                    opt.passV, (unsigned long long)eventLoopPasses());
        // CYT_SDL_CLICK drives the real SDL path instead of the queue, so
        // that a windowed run can be tested the way a person uses it.
        if (std::getenv("CYT_SDL_CLICK"))
          Display::get().pushHostClick(opt.passH, opt.passV, true);
        else
          Display::get().injectClick(opt.passH, opt.passV);
      } else {
        std::printf("pressing command-%c on event-loop pass %llu\n", opt.passKey,
                    (unsigned long long)eventLoopPasses());
        Display::get().injectKey(0, u8(opt.passKey), kCmdKey);
        released = true;              // a key press needs no release
      }
      return;
    }
    // A real click holds the button down for a while: code that polls Button()
    // must observe the press, and code waiting for mouse-up needs the release.
    if (pressed && !released && cpu.instructions - pressedAt > kHoldInstructions) {
      released = true;
      if (std::getenv("CYT_SDL_CLICK"))
        Display::get().pushHostClick(opt.passH, opt.passV, false);
      else
        Display::get().injectRelease();
    }
  });

  // ---- run ----------------------------------------------------------------
  // Everything runs in slices so that the scheduled diagnostics can act between
  // them. The slice size is the profiler's sampling interval when profiling.
  std::map<std::string, u64> samples;
  const u64 slice = opt.profileEvery ? opt.profileEvery : 2'000'000;
  u64 done = 0;
  bool tracingStarted = false;

  while (!cpu.halted && (opt.budget == 0 || done < opt.budget)) {
    const u64 want = (opt.budget && opt.budget - done < slice)
                         ? opt.budget - done : slice;
    const u64 ran = interp.run(want);
    if (ran == 0) break;
    done += ran;

    if (opt.profileEvery) {
      std::string sym = img.symbolFor(cpu.pc);
      if (sym.empty())
        sym = Interp::isShimCode(cpu.pc) ? "<toolbox>" : "<unknown>";
      // Collapse the "+0x..." suffix so samples group by function.
      const size_t plus = sym.find("+0x");
      if (plus != std::string::npos) sym.resize(plus);
      ++samples[sym];
    }

    if (opt.traceFrom && !tracingStarted && done >= opt.traceFrom) {
      tracingStarted = true;
      std::printf("call tracing from instruction %llu\n",
                  (unsigned long long)done);
      tb.setTraceCalls(true);
    }

  }

  // ---- report -------------------------------------------------------------
  std::printf("\nstopped after %llu instructions at 0x%08x: %s\n",
              (unsigned long long)cpu.instructions, cpu.pc,
              cpu.haltReason.empty() ? "(budget exhausted)"
                                     : cpu.haltReason.c_str());
  const std::string sym = img.symbolFor(cpu.pc);
  if (!sym.empty()) std::printf("  in %s\n", sym.c_str());
  if (Mem::get().faultCount())
    std::printf("  %llu out-of-range guest accesses (last 0x%08x)\n",
                (unsigned long long)Mem::get().faultCount(),
                Mem::get().lastFault());
  std::printf("  %llu event-loop passes\n",
              (unsigned long long)eventLoopPasses());

  if (!samples.empty()) {
    std::vector<std::pair<u64, std::string>> top;
    for (const auto& [name, n] : samples) top.emplace_back(n, name);
    std::sort(top.rbegin(), top.rend());
    u64 total = 0;
    for (const auto& [n, name] : top) total += n;
    std::printf("\nwhere the time went (%llu samples):\n",
                (unsigned long long)total);
    for (size_t i = 0; i < top.size() && i < 25; ++i)
      std::printf("  %5.1f%%  %s\n",
                  100.0 * double(top[i].first) / double(total),
                  top[i].second.c_str());
  }

  if (opt.dumpScreen) writeFramebuffer(opt.dumpScreen);
  dumpThreadStats();
  tb.dumpCallStats(stdout);
  Display::get().close();
  return 0;
}
