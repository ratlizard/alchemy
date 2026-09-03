// Memory Manager entry points, layered on Heap.
//
// Error reporting follows the Toolbox convention: the call itself returns the
// result (or NULL), and MemError() reports why. memFullErr is -108, nilHandleErr
// is -109, memWZErr is -111.
#include "mac/heap.h"
#include "toolbox.h"

namespace cyt {
namespace {
constexpr s16 kNoErr       = 0;
constexpr s16 kMemFullErr  = -108;
constexpr s16 kNilHandleErr= -109;
}  // namespace

void registerMemoryManager(Toolbox& tb) {
  // ---- non-relocatable blocks --------------------------------------------
  auto newPtr = [](bool clear) {
    return [clear](Toolbox& tb, PpcCpu& c, Args& a) {
      u32 size = a.u32v();
      GuestAddr p = tb.heap().newPtr(size, clear);
      tb.memErr = p ? kNoErr : kMemFullErr;
      Toolbox::ret(c, p);
    };
  };
  tb.add("NewPtr", newPtr(false));
  tb.add("NewPtrClear", newPtr(true));
  // The "Sys" variants allocate from the system zone; with one flat heap the
  // distinction has no observable effect.
  tb.add("NewPtrSys", newPtr(false));
  tb.add("NewPtrSysClear", newPtr(true));

  tb.add("DisposePtr", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    tb.memErr = tb.heap().disposePtr(p) ? kNoErr : kMemFullErr;
  });
  tb.add("GetPtrSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    u32 n = tb.heap().ptrSize(p);
    tb.memErr = n ? kNoErr : kMemFullErr;
    Toolbox::ret(c, n);
  });
  tb.add("SetPtrSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    u32 n = a.u32v();
    tb.memErr = tb.heap().setPtrSize(p, n) ? kNoErr : kMemFullErr;
  });

  // ---- relocatable blocks -------------------------------------------------
  auto newHandle = [](bool clear) {
    return [clear](Toolbox& tb, PpcCpu& c, Args& a) {
      u32 size = a.u32v();
      GuestAddr h = tb.heap().newHandle(size, clear);
      tb.memErr = h ? kNoErr : kMemFullErr;
      Toolbox::ret(c, h);
    };
  };
  tb.add("NewHandle", newHandle(false));
  tb.add("NewHandleClear", newHandle(true));
  tb.add("NewHandleSys", newHandle(false));
  tb.add("NewHandleSysClear", newHandle(true));
  // The Temporary Memory Manager handed out blocks from the system heap for
  // short-lived use; one flat heap serves the same purpose.
  tb.add("TempNewHandle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    u32 size = a.u32v();
    GuestAddr errPtr = a.ptr();
    GuestAddr h = tb.heap().newHandle(size, false);
    tb.memErr = h ? kNoErr : kMemFullErr;
    if (errPtr) tb.mem().w16(errPtr, u16(tb.memErr));
    Toolbox::ret(c, h);
  });

  tb.add("DisposeHandle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    tb.memErr = tb.heap().disposeHandle(h) ? kNoErr : kNilHandleErr;
  });
  auto getHandleSize = [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    if (!tb.heap().validHandle(h)) {
      tb.memErr = kNilHandleErr;
      Toolbox::ret(c, 0);
      return;
    }
    tb.memErr = kNoErr;
    Toolbox::ret(c, tb.heap().handleSize(h));
  };
  tb.add("GetHandleSize", getHandleSize);
  // InlineGetHandleSize is the same call; the compiler emitted it when it could
  // not prove the handle was non-null.
  tb.add("InlineGetHandleSize", getHandleSize);
  tb.add("SetHandleSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    u32 n = a.u32v();
    tb.memErr = tb.heap().setHandleSize(h, n) ? kNoErr : kMemFullErr;
  });
  tb.add("RecoverHandle", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr p = a.ptr();
    Toolbox::ret(c, tb.heap().recoverHandle(p));
    tb.memErr = kNoErr;
  });

  // ---- locking and purging ------------------------------------------------
  // Blocks are never relocated by this heap, so these only need to keep the
  // state byte that HGetState and HSetState round-trip.
  auto setFlag = [](u8 bit, bool on) {
    return [bit, on](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr h = a.ptr();
      if (!tb.heap().validHandle(h)) { tb.memErr = kNilHandleErr; return; }
      u8 s = tb.heap().handleState(h);
      tb.heap().setHandleState(h, on ? u8(s | bit) : u8(s & ~bit));
      tb.memErr = kNoErr;
    };
  };
  constexpr u8 kLockBit = 0x80, kPurgeBit = 0x40;
  tb.add("HLock", setFlag(kLockBit, true));
  tb.add("HLockHi", setFlag(kLockBit, true));
  tb.add("HUnlock", setFlag(kLockBit, false));
  tb.add("HPurge", setFlag(kPurgeBit, true));
  tb.add("HNoPurge", setFlag(kPurgeBit, false));
  tb.add("HGetState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    tb.memErr = tb.heap().validHandle(h) ? kNoErr : kNilHandleErr;
    Toolbox::ret(c, tb.heap().handleState(h));
  });
  tb.add("HSetState", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    u8 s = a.u8v();
    tb.heap().setHandleState(h, s);
    tb.memErr = kNoErr;
  });
  // Relocation requests are inherently advisory and this heap never moves a
  // block, so they succeed by doing nothing.
  for (const char* nop : {"MoveHHi", "EmptyHandle", "ReserveMem", "PurgeMem",
                          "CompactMem", "MoreMasters", "MaxApplZone",
                          "SetGrowZone", "InitZone", "PurgeSpace"})
    tb.add(nop, [](Toolbox& tb, PpcCpu& c, Args& a) { tb.memErr = kNoErr; });

  // ---- block copying ------------------------------------------------------
  auto blockMove = [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr src = a.ptr(), dst = a.ptr();
    u32 n = a.u32v();
    tb.mem().move(dst, src, n);
    tb.memErr = kNoErr;
  };
  tb.add("BlockMove", blockMove);
  tb.add("BlockMoveData", blockMove);
  tb.add("BlockMoveUncached", blockMove);
  tb.add("BlockMoveDataUncached", blockMove);

  tb.add("PtrToHand", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr src = a.ptr(), hOut = a.ptr();
    u32 n = a.u32v();
    GuestAddr h = tb.heap().newHandle(n, false);
    if (!h) { tb.memErr = kMemFullErr; Toolbox::ret(c, u32(kMemFullErr)); return; }
    tb.mem().move(tb.mem().r32(h), src, n);
    tb.mem().w32(hOut, h);
    tb.memErr = kNoErr;
    Toolbox::ret(c, 0);
  });
  tb.add("PtrToXHand", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr src = a.ptr(), h = a.ptr();
    u32 n = a.u32v();
    if (!tb.heap().setHandleSize(h, n)) {
      tb.memErr = kMemFullErr; Toolbox::ret(c, u32(kMemFullErr)); return;
    }
    tb.mem().move(tb.mem().r32(h), src, n);
    tb.memErr = kNoErr;
    Toolbox::ret(c, 0);
  });
  tb.add("HandToHand", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr hIO = a.ptr();
    GuestAddr src = tb.mem().r32(hIO);
    if (!tb.heap().validHandle(src)) {
      tb.memErr = kNilHandleErr; Toolbox::ret(c, u32(kNilHandleErr)); return;
    }
    u32 n = tb.heap().handleSize(src);
    GuestAddr dup = tb.heap().newHandle(n, false);
    if (!dup) { tb.memErr = kMemFullErr; Toolbox::ret(c, u32(kMemFullErr)); return; }
    tb.mem().move(tb.mem().r32(dup), tb.mem().r32(src), n);
    tb.mem().w32(hIO, dup);
    tb.memErr = kNoErr;
    Toolbox::ret(c, 0);
  });
  tb.add("PtrAndHand", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr src = a.ptr(), h = a.ptr();
    u32 n = a.u32v();
    u32 old = tb.heap().handleSize(h);
    if (!tb.heap().setHandleSize(h, old + n)) {
      tb.memErr = kMemFullErr; Toolbox::ret(c, u32(kMemFullErr)); return;
    }
    tb.mem().move(tb.mem().r32(h) + old, src, n);
    tb.memErr = kNoErr;
    Toolbox::ret(c, 0);
  });
  tb.add("HandAndHand", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h1 = a.ptr(), h2 = a.ptr();
    u32 n1 = tb.heap().handleSize(h1), n2 = tb.heap().handleSize(h2);
    if (!tb.heap().setHandleSize(h2, n1 + n2)) {
      tb.memErr = kMemFullErr; Toolbox::ret(c, u32(kMemFullErr)); return;
    }
    tb.mem().move(tb.mem().r32(h2) + n2, tb.mem().r32(h1), n1);
    tb.memErr = kNoErr;
    Toolbox::ret(c, 0);
  });

  // ---- zone and free-space queries ---------------------------------------
  tb.add("MemError", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(s32(tb.memErr)));
  });
  auto freeMem = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, tb.heap().freeBytes());
    tb.memErr = kNoErr;
  };
  tb.add("FreeMem", freeMem);
  tb.add("FreeMemSys", freeMem);
  auto maxBlock = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, tb.heap().largestFreeBlock());
    tb.memErr = kNoErr;
  };
  tb.add("MaxBlock", maxBlock);
  tb.add("MaxBlockSys", maxBlock);
  tb.add("MaxMem", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr growPtr = a.ptr();
    if (growPtr) tb.mem().w32(growPtr, 0);
    Toolbox::ret(c, tb.heap().largestFreeBlock());
    tb.memErr = kNoErr;
  });
  tb.add("StackSpace", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, layout::kStackTop - layout::kStackLimit);
  });
  // Zone pointers are opaque to applications; a stable non-null value is
  // sufficient for the identity comparisons they perform.
  auto zone = [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, layout::kHeapBase);
    tb.memErr = kNoErr;
  };
  tb.add("GetZone", zone);
  tb.add("ApplicationZone", zone);
  tb.add("SystemZone", zone);
  tb.add("HandleZone", zone);
  tb.add("PtrZone", zone);
  tb.add("SetZone", [](Toolbox& tb, PpcCpu& c, Args& a) { tb.memErr = kNoErr; });
}

}  // namespace cyt
