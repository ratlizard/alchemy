// A user-mode 32-bit PowerPC interpreter, sized for what a CFM application
// actually executes: integer and double-precision floating point, no MMU, no
// supervisor state, no AltiVec. Cythera's 143k symbolized instructions use only
// 76 distinct opcode forms, but the whole user-mode set is implemented so that
// unsymbolized code and the game's own defprocs cannot walk off the edge.
#pragma once

#include <functional>
#include <vector>
#include <string>

#include "mem.h"

namespace cyt {

class PefImage;

// Condition Register: eight 4-bit fields. Field i sits at shift (28 - 4i), so
// CR0 occupies the most significant nibble, matching the architecture's
// MSB-first bit numbering. Within a field: 8=LT, 4=GT, 2=EQ, 1=SO.
constexpr u32 kXerSo = 0x80000000;
constexpr u32 kXerOv = 0x40000000;
constexpr u32 kXerCa = 0x20000000;

struct PpcCpu {
  u32 gpr[32] = {};
  double fpr[32] = {};
  u32 pc = 0;
  u32 lr = 0;
  u32 ctr = 0;
  u32 cr = 0;
  u32 xer = 0;
  u32 fpscr = 0;

  u64 instructions = 0;   // retired instruction count, for budgeting and traces
  // Set by any instruction that writes pc itself, so the step loop knows not to
  // advance past the branch it just took.
  bool branched_ = false;
  bool halted = false;
  std::string haltReason;

  u32 crField(u32 i) const { return (cr >> (28 - 4 * i)) & 0xF; }
  void setCrField(u32 i, u32 v) {
    u32 sh = 28 - 4 * i;
    cr = (cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
  }
  // MSB-indexed CR bit, as the BI field of a conditional branch names it.
  u32 crBit(u32 n) const { return (cr >> (31 - n)) & 1; }
  void setCrBit(u32 n, u32 v) {
    u32 m = 1u << (31 - n);
    cr = v ? (cr | m) : (cr & ~m);
  }
  bool ca() const { return xer & kXerCa; }
  void setCa(bool v) { xer = v ? (xer | kXerCa) : (xer & ~kXerCa); }
};

// How a native (host-implemented) call reaches the interpreter. Every PEF
// import is bound to a unique fake code address inside the shim region; when
// the interpreter's program counter lands there it calls this instead of
// decoding an instruction, then returns to the link register.
using HostCallFn = std::function<void(u32 importIndex, PpcCpu&)>;

class Interp {
 public:
  Interp(PpcCpu* cpu, Mem* mem) : cpu_(cpu), mem_(mem) {}

  void setHostCall(HostCallFn fn) { host_ = std::move(fn); }
  void setImage(const PefImage* img) { img_ = img; }

  // Base of the synthetic code addresses that stand in for imported symbols.
  static constexpr GuestAddr kShimCodeBase = layout::kShimBase + 0x80000;
  static bool isShimCode(GuestAddr pc) {
    return pc >= kShimCodeBase && pc < layout::kShimEnd;
  }

  // Executes up to `budget` instructions, or until the CPU halts. Returns the
  // number retired. A budget of 0 means "until halted".
  u64 run(u64 budget = 0);

  // A ceiling on the total instruction count, honoured by every run -- nested
  // guest calls included. The harness's --budget has to be enforced here rather
  // than by the caller's own slicing, because a nested call can legitimately
  // run for the rest of the program: Cythera loads its whole world inside the
  // Apple event handler that starts it up.
  void setInstructionLimit(u64 n) { limit_ = n; }

  // Executes exactly one instruction (or one native call).
  void step();

  // Calls a guest routine described by a transition vector, PowerPC CFM style:
  // r2 comes from the vector, arguments must already be in r3..r10, and the
  // call returns when the synthetic return address is reached. Returns r3.
  u32 callTVector(GuestAddr tvector, u64 budget = 200'000'000);

  // Calls guest PowerPC code from inside a native handler: saves the whole CPU,
  // builds a fresh stack frame below the current one, runs to the return
  // sentinel, then restores. This is what lets Mixed Mode call the application's
  // own window and control definition procedures, which live in the PEF.
  u32 callPpc(GuestAddr code, GuestAddr toc, const std::vector<u32>& args,
              u64 budget = 50'000'000);

  // Called the first time the interpreter reaches an address named by
  // CYT_TRACE_CALLS_FROM_PC. The harness arms Toolbox call tracing from it.
  // This has to happen here rather than in the outer run loop, because the
  // outer loop does not come round while a nested guest call is running -- and
  // the game spends most of its life inside one.
  void setTraceArmHook(std::function<void()> fn) { armTrace_ = std::move(fn); }

  // Instruction-level tracing. Expensive; used to drive shim development.
  void setTrace(bool on) { trace_ = on; }
  bool trace() const { return trace_; }
  std::string disasmOne(GuestAddr pc) const;

  void halt(std::string reason) {
    cpu_->halted = true;
    cpu_->haltReason = std::move(reason);
  }

 private:
  void execute(u32 op);
  void unimplemented(u32 op, const char* what);

  // Shared helpers for the arithmetic forms.
  void setCr0(u32 result) {
    u32 f = (s32(result) < 0) ? 8u : (result == 0 ? 2u : 4u);
    if (cpu_->xer & kXerSo) f |= 1u;
    cpu_->setCrField(0, f);
  }
  void setOv(bool ov) {
    if (ov) cpu_->xer |= (kXerOv | kXerSo);
    else cpu_->xer &= ~kXerOv;
  }
  // Common add-with-carry core: computes a+b+carryIn, updating CA/OV/CR0.
  u32 addCore(u32 a, u32 b, u32 carryIn, bool wantCa, bool oe, bool rc);

  PpcCpu* cpu_;
  Mem* mem_;
  HostCallFn host_;
  const PefImage* img_ = nullptr;
  std::function<void()> armTrace_;
  bool trace_ = false;
  u64 limit_ = 0;
};

// The synthetic return address pushed by callTVector. Reaching it stops the
// interpreter cleanly rather than executing whatever happens to be there.
constexpr GuestAddr kReturnSentinel = layout::kShimEnd - 4;

}  // namespace cyt
