// Standard File package: the dialogue that asks the user for a file.
//
// The original opens a modal chooser and spins its own event loop inside it.
// This port answers from the support directory instead, without any UI: a save
// is accepted under the name the caller suggested, and an open returns the
// first file there whose Finder type the caller said it would accept. That
// keeps the whole new-game and load-game path runnable headlessly, which a
// native chooser could not do -- see README.md for why that trade was made and
// POWERPC-NOTES.md for what it costs.
//
// Every entry point fills in a StandardFileReply, whose layout is fixed:
//
//   0  Boolean sfGood        the user did not cancel
//   1  Boolean sfReplacing   the chosen name already exists
//   2  OSType  sfType        the file's type, for an open
//   6  FSSpec  sfFile        70 bytes: vRefNum, parID, Str63 name
//  76  short   sfScript
//  78  short   sfFlags       Finder flags
//  80  Boolean sfIsFolder
//  81  Boolean sfIsVolume
//  82  long    sfReserved1
//  86  short   sfReserved2   -- 88 bytes in all
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "mac/file_mgr.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {
namespace {

// StandardFileReply field offsets.
constexpr u32 kReplyGood      = 0;
constexpr u32 kReplyReplacing = 1;
constexpr u32 kReplyType      = 2;
constexpr u32 kReplyFile      = 6;
constexpr u32 kReplyScript    = 76;
constexpr u32 kReplyFlags     = 78;
constexpr u32 kReplyIsFolder  = 80;
constexpr u32 kReplyIsVolume  = 81;
constexpr u32 kReplySize      = 88;

// The name a save falls back to when the caller suggests nothing. Cythera
// always passes a default, so this is a backstop rather than a policy.
constexpr const char* kUntitled = "Untitled";

constexpr s16 kSmRoman = 0;

bool debugOn() {
  static const bool on = std::getenv("CYT_DEBUG_FILE") != nullptr;
  return on;
}

void clearReply(Mem& m, GuestAddr reply) {
  if (!reply) return;
  for (u32 i = 0; i < kReplySize; ++i) m.w8(reply + i, 0);
}

// Fills in the "user chose this file" answer. `type` is reported as the file's
// type for an open and is meaningless for a save, where the caller has not yet
// decided what it is writing.
void answer(Mem& m, GuestAddr reply, const std::string& name, ResType type) {
  clearReply(m, reply);
  if (!reply) return;
  m.w8(reply + kReplyGood, 1);
  m.w8(reply + kReplyReplacing, FileMgr::get().exists(name) ? 1 : 0);
  m.w32(reply + kReplyType, type);
  writeFSSpec(m, reply + kReplyFile, name);
  m.w16(reply + kReplyScript, u16(kSmRoman));
  m.w16(reply + kReplyFlags, 0);
  m.w8(reply + kReplyIsFolder, 0);
  m.w8(reply + kReplyIsVolume, 0);
}

// The reply for "the user pressed Cancel". Every caller checks sfGood first,
// so nothing else in the record has to be meaningful -- but it is zeroed
// anyway, because a caller that reads sfFile regardless should see an empty
// name rather than whatever was on the stack.
void cancel(Mem& m, GuestAddr reply) { clearReply(m, reply); }

// Shared by StandardPutFile and CustomPutFile, which differ only in the eight
// arguments after the reply that describe a dialogue this port does not open.
void putFile(Toolbox& tb, Args& a) {
  Mem& m = tb.mem();
  const std::string prompt = m.pstr(a.ptr());
  std::string name = m.pstr(a.ptr());
  const GuestAddr reply = a.ptr();
  if (name.empty()) name = kUntitled;
  answer(m, reply, name, 0);
  if (debugOn())
    std::fprintf(stderr, "  [StdFile] put \"%s\" -> \"%s\"%s\n", prompt.c_str(),
                 name.c_str(),
                 FileMgr::get().exists(name) ? " (replacing)" : "");
}

// Shared by StandardGetFile, StandardGetFilePreview and CustomGetFile.
//
// The caller names the file types it will accept, and the port honours that
// list against the type recorded when each file was created -- which matters,
// because the support directory also holds the licence text and the log the
// game writes at start-up, and neither is a saved game. A file filter
// procedure is a different matter and is not called: it would have to be run
// through Mixed Mode against a catalogue entry this port does not build.
void getFile(Toolbox& tb, Args& a) {
  Mem& m = tb.mem();
  a.ptr();                            // fileFilter
  const s16 numTypes = a.s16v();
  const GuestAddr typeList = a.ptr();
  const GuestAddr reply = a.ptr();

  // numTypes of -1 means "every type is acceptable".
  auto acceptable = [&](const std::string& name, ResType* typeOut) {
    FileMgr::FinderInfo fi{};
    const bool known = FileMgr::get().getFinderInfo(name, &fi);
    if (typeOut) *typeOut = fi.type;
    if (numTypes < 0 || !typeList) return true;
    if (!known) return false;
    for (s16 i = 0; i < numTypes; ++i)
      if (m.r32(typeList + u32(i) * 4) == fi.type) return true;
    return false;
  };

  // With no chooser there is no way to pick among several, so the port takes
  // the first acceptable file in alphabetical order.
  for (const std::string& name : FileMgr::get().listSupportFiles()) {
    ResType type = 0;
    if (!acceptable(name, &type)) continue;
    answer(m, reply, name, type);
    if (debugOn())
      std::fprintf(stderr, "  [StdFile] get: choosing \"%s\" ('%s')\n",
                   name.c_str(), resTypeName(type).c_str());
    return;
  }

  cancel(m, reply);
  if (debugOn()) {
    std::fprintf(stderr, "  [StdFile] get: no file of an acceptable type, "
                         "reporting cancelled\n");
    for (s16 i = 0; i < numTypes && typeList; ++i)
      std::fprintf(stderr, "  [StdFile]   asked for type '%s'\n",
                   resTypeName(m.r32(typeList + u32(i) * 4)).c_str());
  }
}

}  // namespace

void registerStandardFile(Toolbox& tb) {
  tb.add("StandardPutFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    putFile(tb, a);
  });
  tb.add("StandardGetFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    getFile(tb, a);
  });
  // The preview variant differs only in the dialogue it opens.
  tb.add("StandardGetFilePreview", [](Toolbox& tb, PpcCpu& c, Args& a) {
    getFile(tb, a);
  });
  // The Custom entry points take the same leading arguments and then a
  // dialogue id, position and four hook procedures. Reading only the leading
  // arguments is enough, because the rest describe UI that never opens.
  tb.add("CustomPutFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    putFile(tb, a);
  });
  tb.add("CustomGetFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    getFile(tb, a);
  });

  // Navigation Services is the System 8.5 replacement for all of the above.
  // It is reported absent so that the game uses Standard File, which is the
  // path this port implements; see the note about Gestalt in the README.
  for (const char* absent : {"NavServicesCanRun", "NavServicesAvailable"})
    tb.add(absent, [](Toolbox& tb, PpcCpu& c, Args& a) { Toolbox::ret(c, 0); });
}

}  // namespace cyt
