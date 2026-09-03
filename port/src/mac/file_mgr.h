// File Manager: maps the guest's HFS world onto a host directory.
//
// Classic files have two forks, so each Mac file name resolves to a host data
// file and, when present, a sibling resource fork. The extracted originals are
// named "<name>.data" and "<name>.rsrc"; a plain host file with the exact Mac
// name is also accepted, which is what saved games become.
#pragma once

#include <string>
#include <vector>

#include "mem.h"

namespace cyt {

// FSSpec: { short vRefNum; long parID; Str63 name; } -- 70 bytes.
constexpr u32 kFSSpecSize = 70;

// This port has one place for the original game files and one for saved games,
// so every specification it hands out names the same synthetic volume and
// directory; only the file name carries information.
void writeFSSpec(Mem& m, GuestAddr spec, const std::string& name);
std::string fsSpecName(Mem& m, GuestAddr spec);

class FileMgr {
 public:
  static FileMgr& get();

  // The directory holding "Cythera Data" and the extracted forks.
  void setGameDir(std::string dir) { gameDir_ = std::move(dir); }
  // Where saved games and preferences are written.
  void setSupportDir(std::string dir) { supportDir_ = std::move(dir); }

  const std::string& gameDir() const { return gameDir_; }
  const std::string& supportDir() const { return supportDir_; }

  // Host path for a Mac file name, for either fork. Returns "" when the fork
  // does not exist and `forWriting` is false.
  std::string resolve(const std::string& macName, bool resourceFork,
                      bool forWriting) const;

  // Does either fork of this Mac file exist anywhere the port can see?
  bool exists(const std::string& macName) const;

  // Finder metadata: a file's four-character type and creator, and its Finder
  // flags. The classic system kept these in the volume catalogue. A plain host
  // file has nowhere to put them, so the port writes a small sidecar beside the
  // data fork -- which matters because Standard File filters by type, and
  // without it "Open Game" could not tell a saved game from a log file.
  struct FinderInfo {
    u32 type = 0;
    u32 creator = 0;
    u16 flags = 0;
  };
  bool getFinderInfo(const std::string& macName, FinderInfo* out) const;
  bool setFinderInfo(const std::string& macName, const FinderInfo& info);

  // Mac file names in the support directory, in alphabetical order and with the
  // fork suffixes folded away, so a file written as both ".data" and ".rsrc"
  // appears once. This is what the port's Standard File replacement offers.
  std::vector<std::string> listSupportFiles() const;

  // Open data-fork streams, addressed by the refNums the Toolbox hands out.
  s16 openFork(const std::string& macName, bool resourceFork, bool writable,
               s16* refOut);
  bool close(s16 ref);
  s16 read(s16 ref, GuestAddr buf, u32 want, u32* got);
  s16 write(s16 ref, GuestAddr buf, u32 want, u32* wrote);
  s16 eof(s16 ref, u32* len);
  s16 setEof(s16 ref, u32 len);
  s16 setPos(s16 ref, s16 mode, s32 offset);
  s16 getPos(s16 ref, u32* pos);
  bool valid(s16 ref) const;

 private:
  struct Open {
    std::string path;
    std::vector<u8> buf;   // whole-file buffer; these files are a few megabytes
    u32 pos = 0;
    bool writable = false;
    bool dirty = false;
  };

  std::string gameDir_, supportDir_;
  std::vector<Open> open_;   // index + kFirstRefNum == refNum
};

}  // namespace cyt
