// File Manager Toolbox entry points.
//
// Only the FSSpec-based calls and the classic HFS ones the game actually uses
// are here. An FSSpec is treated as a name plus a synthetic volume and
// directory: this port has one place for the original game files and one for
// saved games, so a full HFS directory hierarchy would add nothing the game can
// observe.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mac/file_mgr.h"
#include "mac/heap.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

// Provided by resource_mgr.cpp.
s16 openResourceFile(const std::string& path, const std::string& name,
                     bool writable, std::string* err);
void setResError(s16 err);

namespace {
constexpr s16 kNoErr  = 0;
constexpr s16 kFnfErr = -43;
constexpr s16 kDupFNErr = -48;
constexpr s16 kIoErr    = -36;

// Permissions: 0 whatever the file allows, 1 read, 2 write, 3 read/write.
// A fork may only be written back if it is one the port owns; the extracted
// originals stay exactly as they were, whatever permission is asked for.
inline bool writableFork(const std::string& path, u8 perm) {
  const std::string& support = FileMgr::get().supportDir();
  return perm >= 2 && !support.empty() && path.rfind(support, 0) == 0;
}

constexpr s16 kOurVRefNum = -1;
constexpr s32 kOurDirID   = 2;   // the classic root directory id

// FSSpec field offsets.
constexpr u32 kSpecVRefNum = 0;
constexpr u32 kSpecParID   = 2;
constexpr u32 kSpecName    = 6;

}  // namespace

void writeFSSpec(Mem& m, GuestAddr spec, const std::string& name) {
  m.w16(spec + kSpecVRefNum, u16(kOurVRefNum));
  m.w32(spec + kSpecParID, u32(kOurDirID));
  m.writePstr(spec + kSpecName, name, 64);
}

std::string fsSpecName(Mem& m, GuestAddr spec) {
  return m.pstr(spec + kSpecName);
}

void registerFileManager(Toolbox& tb) {
  // ---- specifications -----------------------------------------------------
  tb.add("FSMakeFSSpec", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v();                       // vRefNum
    a.s32v();                       // dirID
    std::string name = tb.mem().pstr(a.ptr());
    GuestAddr spec = a.ptr();
    writeFSSpec(tb.mem(), spec, name);
    // The spec is filled in either way; the result only says whether the file
    // exists, and callers creating a file rely on getting fnfErr plus a usable
    // specification.
    bool exists = !FileMgr::get().resolve(name, false, false).empty() ||
                  !FileMgr::get().resolve(name, true, false).empty();
    Toolbox::ret(c, u32(s32(exists ? kNoErr : kFnfErr)));
  });

  // ---- resource forks -----------------------------------------------------
  auto openResFork = [](Toolbox& tb, PpcCpu& c, const std::string& name,
                        u8 perm, bool complain) {
    std::string path = FileMgr::get().resolve(name, true, false);
    if (path.empty()) {
      if (complain)
        std::fprintf(stderr, "  [FileMgr] no resource fork for \"%s\"\n",
                     name.c_str());
      Toolbox::ret(c, u32(s32(-1)));
      return;
    }
    std::string err;
    s16 ref = openResourceFile(path, name, writableFork(path, perm), &err);
    if (ref < 0)
      std::fprintf(stderr, "  [FileMgr] %s\n", err.c_str());
    Toolbox::ret(c, u32(s32(ref)));
  };
  tb.add("FSpOpenResFile", [openResFork](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr spec = a.ptr();
    u8 perm = a.u8v();
    openResFork(tb, c, fsSpecName(tb.mem(), spec), perm, true);
  });
  tb.add("HOpenResFile", [openResFork](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    std::string name = tb.mem().pstr(a.ptr());
    openResFork(tb, c, name, a.u8v(), false);
  });
  tb.add("OpenResFile", [openResFork](Toolbox& tb, PpcCpu& c, Args& a) {
    openResFork(tb, c, tb.mem().pstr(a.ptr()), 1, false);
  });
  // Creating a resource file writes an empty fork the Resource Manager can
  // then open and add to. A file that already has one is left alone and
  // reported as a duplicate, which is what CreateResFile does and what callers
  // that create-then-open rely on.
  tb.add("FSpCreateResFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr spec = a.ptr();
    u32 creator = a.u32v(), type = a.u32v();
    a.s16v();                       // scriptTag
    std::string name = fsSpecName(tb.mem(), spec);
    if (!FileMgr::get().resolve(name, true, false).empty()) {
      setResError(kDupFNErr);
      Toolbox::ret(c, u32(s32(kDupFNErr)));
      return;
    }
    std::string path = FileMgr::get().resolve(name, true, true);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const std::vector<u8> empty = buildResourceFork({});
    if (!out || !out.write(reinterpret_cast<const char*>(empty.data()),
                           std::streamsize(empty.size()))) {
      setResError(kIoErr);
      Toolbox::ret(c, u32(s32(kIoErr)));
      return;
    }
    FileMgr::get().setFinderInfo(name, {type, creator, 0});
    setResError(kNoErr);
    Toolbox::ret(c, 0);
  });

  // ---- data forks ---------------------------------------------------------
  auto openFork = [](bool resourceFork) {
    return [resourceFork](Toolbox& tb, PpcCpu& c, Args& a) {
      GuestAddr spec = a.ptr();
      u8 perm = a.u8v();
      GuestAddr refOut = a.ptr();
      std::string name = fsSpecName(tb.mem(), spec);
      s16 ref = 0;
      // Permissions 2 and 3 are write and read/write. Opening a resource fork
      // as a byte stream is distinct from opening it through the Resource
      // Manager, and reads the ".rsrc" sibling rather than the data file.
      s16 result = FileMgr::get().openFork(name, resourceFork, perm >= 2, &ref);
      if (result == kNoErr && refOut) tb.mem().w16(refOut, u16(ref));
      Toolbox::ret(c, u32(s32(result)));
    };
  };
  tb.add("FSpOpenDF", openFork(false));
  tb.add("FSpOpenRF", openFork(true));
  tb.add("HOpen", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    std::string name = tb.mem().pstr(a.ptr());
    u8 perm = a.u8v();
    GuestAddr refOut = a.ptr();
    s16 ref = 0;
    s16 r = FileMgr::get().openFork(name, false, perm >= 2, &ref);
    if (r == kNoErr && refOut) tb.mem().w16(refOut, u16(ref));
    Toolbox::ret(c, u32(s32(r)));
  });

  tb.add("FSRead", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    GuestAddr countPtr = a.ptr();
    GuestAddr buf = a.ptr();
    u32 want = tb.mem().r32(countPtr), got = 0;
    s16 r = FileMgr::get().read(ref, buf, want, &got);
    tb.mem().w32(countPtr, got);
    Toolbox::ret(c, u32(s32(r)));
  });
  tb.add("FSWrite", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    GuestAddr countPtr = a.ptr();
    GuestAddr buf = a.ptr();
    u32 want = tb.mem().r32(countPtr), wrote = 0;
    s16 r = FileMgr::get().write(ref, buf, want, &wrote);
    tb.mem().w32(countPtr, wrote);
    Toolbox::ret(c, u32(s32(r)));
  });
  // PBFlushFileSync(paramBlock) commits a file's buffers to disk. This port
  // has none to commit: every write goes straight to the host file through
  // FileMgr, so there is nothing held back and the honest answer is noErr.
  // Reporting an error instead would make the game believe a save had failed.
  tb.add("PBFlushFileSync", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                                   // the parameter block
    Toolbox::ret(c, 0);                        // noErr
  });

  tb.add("FSClose", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, FileMgr::get().close(a.s16v()) ? 0u : u32(s32(-51)));
  });
  tb.add("GetEOF", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    GuestAddr out = a.ptr();
    u32 len = 0;
    s16 r = FileMgr::get().eof(ref, &len);
    if (out) tb.mem().w32(out, len);
    Toolbox::ret(c, u32(s32(r)));
  });
  tb.add("SetEOF", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    u32 len = a.u32v();
    Toolbox::ret(c, u32(s32(FileMgr::get().setEof(ref, len))));
  });
  tb.add("SetFPos", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    s16 mode = a.s16v();
    s32 off = a.s32v();
    Toolbox::ret(c, u32(s32(FileMgr::get().setPos(ref, mode, off))));
  });
  tb.add("GetFPos", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 ref = a.s16v();
    GuestAddr out = a.ptr();
    u32 pos = 0;
    s16 r = FileMgr::get().getPos(ref, &pos);
    if (out) tb.mem().w32(out, pos);
    Toolbox::ret(c, u32(s32(r)));
  });

  // ---- catalogue operations ----------------------------------------------
  // Type and creator come from the sidecar the port writes when a file is
  // created. Files that predate the sidecar -- the extracted originals -- are
  // reported as Cythera's own documents, which is what they are.
  auto getInfo = [](Toolbox& tb, PpcCpu& c, const std::string& name,
                    GuestAddr info) {
    if (!FileMgr::get().exists(name)) {
      Toolbox::ret(c, u32(s32(kFnfErr)));
      return;
    }
    FileMgr::FinderInfo fi{resType("DelS"), resType("Delv"), 0};
    FileMgr::get().getFinderInfo(name, &fi);
    // FInfo: fdType, fdCreator, fdFlags, fdLocation, fdFldr.
    tb.mem().w32(info + 0, fi.type);
    tb.mem().w32(info + 4, fi.creator);
    tb.mem().w16(info + 8, fi.flags);
    tb.mem().w32(info + 10, 0);
    tb.mem().w16(info + 14, 0);
    Toolbox::ret(c, 0);
  };
  tb.add("FSpGetFInfo", [getInfo](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr spec = a.ptr();
    getInfo(tb, c, fsSpecName(tb.mem(), spec), a.ptr());
  });
  tb.add("HGetFInfo", [getInfo](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    std::string name = tb.mem().pstr(a.ptr());
    getInfo(tb, c, name, a.ptr());
  });

  auto setInfo = [](Toolbox& tb, PpcCpu& c, const char* who,
                    const std::string& name, GuestAddr info) {
    FileMgr::FinderInfo fi;
    fi.type = tb.mem().r32(info + 0);
    fi.creator = tb.mem().r32(info + 4);
    fi.flags = tb.mem().r16(info + 8);
    FileMgr::get().setFinderInfo(name, fi);
    if (std::getenv("CYT_DEBUG_FILE"))
      std::fprintf(stderr,
                   "  [FileMgr] %s \"%s\" type '%s' creator '%s' "
                   "flags %04x\n",
                   who, name.c_str(), resTypeName(fi.type).c_str(),
                   resTypeName(fi.creator).c_str(), fi.flags);
    Toolbox::ret(c, 0);
  };
  tb.add("FSpSetFInfo", [setInfo](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr spec = a.ptr();
    setInfo(tb, c, "FSpSetFInfo", fsSpecName(tb.mem(), spec), a.ptr());
  });
  tb.add("HSetFInfo", [setInfo](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    std::string name = tb.mem().pstr(a.ptr());
    setInfo(tb, c, "HSetFInfo", name, a.ptr());
  });
  tb.add("SetFInfo", [setInfo](Toolbox& tb, PpcCpu& c, Args& a) {
    std::string name = tb.mem().pstr(a.ptr());
    a.s16v();
    setInfo(tb, c, "SetFInfo", name, a.ptr());
  });

  // Creating a file records the type and creator it was given, so that the
  // Standard File replacement can later tell a saved game from anything else
  // the game has left in the support directory.
  auto create = [](Toolbox& tb, PpcCpu& c, const char* who,
                   const std::string& name,
                   u32 creator, u32 type, bool failIfPresent) {
    std::string path = FileMgr::get().resolve(name, false, true);
    if (path.empty()) { Toolbox::ret(c, u32(s32(-35))); return; }
    if (failIfPresent && std::filesystem::exists(path)) {
      Toolbox::ret(c, u32(s32(kDupFNErr)));
      return;
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { Toolbox::ret(c, u32(s32(-36))); return; }
    std::fclose(f);
    FileMgr::get().setFinderInfo(name, {type, creator, 0});
    if (std::getenv("CYT_DEBUG_FILE"))
      std::fprintf(stderr, "  [FileMgr] %s \"%s\" type '%s' creator '%s'\n",
                   who, name.c_str(), resTypeName(type).c_str(),
                   resTypeName(creator).c_str());
    Toolbox::ret(c, 0);
  };
  tb.add("FSpCreate", [create](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr spec = a.ptr();
    u32 creator = a.u32v(), type = a.u32v();
    a.s16v();                     // scriptTag
    create(tb, c, "FSpCreate", fsSpecName(tb.mem(), spec), creator, type, true);
  });
  tb.add("HCreate", [create](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    std::string name = tb.mem().pstr(a.ptr());
    u32 creator = a.u32v(), type = a.u32v();
    create(tb, c, "HCreate", name, creator, type, false);
  });
  auto deleteFile = [](Toolbox& tb, PpcCpu& c, const std::string& name) {
    namespace fs = std::filesystem;
    std::error_code ec;
    bool any = false;
    for (bool rsrc : {false, true}) {
      std::string p = FileMgr::get().resolve(name, rsrc, false);
      // Only files under the support directory may be removed; the originals
      // must survive whatever the game asks for.
      if (!p.empty() &&
          p.rfind(FileMgr::get().supportDir(), 0) == 0 &&
          !FileMgr::get().supportDir().empty()) {
        any = fs::remove(p, ec) || any;
      }
    }
    // The Finder-info sidecar goes with the file it described.
    if (any && !FileMgr::get().supportDir().empty())
      fs::remove(fs::path(FileMgr::get().supportDir()) / (name + ".finf"), ec);
    Toolbox::ret(c, any ? 0u : u32(s32(kFnfErr)));
  };
  tb.add("FSpDelete", [deleteFile](Toolbox& tb, PpcCpu& c, Args& a) {
    deleteFile(tb, c, fsSpecName(tb.mem(), a.ptr()));
  });
  tb.add("HDelete", [deleteFile](Toolbox& tb, PpcCpu& c, Args& a) {
    a.s16v(); a.s32v();
    deleteFile(tb, c, tb.mem().pstr(a.ptr()));
  });
  tb.add("FSpExchangeFiles", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // Used by safe-save: write a temporary file then swap it with the original.
    namespace fs = std::filesystem;
    std::string src = fsSpecName(tb.mem(), a.ptr());
    std::string dst = fsSpecName(tb.mem(), a.ptr());
    std::string a1 = FileMgr::get().resolve(src, false, false);
    std::string b1 = FileMgr::get().resolve(dst, false, false);
    if (a1.empty() || b1.empty()) { Toolbox::ret(c, u32(s32(kFnfErr))); return; }
    std::error_code ec;
    std::string tmp = a1 + ".swap";
    fs::rename(a1, tmp, ec);
    fs::rename(b1, a1, ec);
    fs::rename(tmp, b1, ec);
    Toolbox::ret(c, ec ? u32(s32(-36)) : 0u);
  });

  // ---- volumes -----------------------------------------------------------
  tb.add("HGetVol", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr namePtr = a.ptr();
    GuestAddr vRefOut = a.ptr(), dirOut = a.ptr();
    if (namePtr) tb.mem().writePstr(namePtr, "Cythera", 32);
    if (vRefOut) tb.mem().w16(vRefOut, u16(kOurVRefNum));
    if (dirOut) tb.mem().w32(dirOut, u32(kOurDirID));
    Toolbox::ret(c, 0);
  });
  for (const char* ok : {"FlushVol", "HSetVol", "SetVol", "UnmountVol", "Eject",
                         "FlushFile"})
    tb.add(ok, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });

  // Alias resolution: this port has no aliases, so a spec resolves to itself.
  tb.add("ResolveAlias", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr();
    GuestAddr target = a.ptr();
    GuestAddr changed = a.ptr();
    if (changed) tb.mem().w8(changed, 0);
    (void)target;
    Toolbox::ret(c, u32(s32(kFnfErr)));
  });
  tb.add("NewAlias", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr();
    GuestAddr out = a.ptr();
    if (out) tb.mem().w32(out, 0);
    Toolbox::ret(c, u32(s32(kFnfErr)));
  });

  // Standard File lives in standard_file.cpp.
}

}  // namespace cyt
