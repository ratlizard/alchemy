// Resource Manager: turns entries in a parsed resource fork into guest handles.
//
// Semantics worth preserving, because applications depend on them: a resource
// is loaded once and the same handle comes back on every subsequent request;
// SetResLoad(false) makes lookups return an empty handle without reading data;
// ResError() reports the last failure; and the search order runs from the
// current resource file outwards, which for a single application means the
// game's own fork and then the (empty) system fork.
#include <cstdio>
#include <map>
#include <vector>

#include "mac/heap.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

namespace {
constexpr s16 kNoErr        = 0;
constexpr s16 kResNotFound  = -192;
constexpr s16 kResFNotFound = -193;
constexpr s16 kAddResFailed = -194;
constexpr s16 kWrPermErr    = -61;
constexpr s16 kIoErr        = -36;
}  // namespace

// One open resource file: the parsed fork, the handles already handed out, and
// whatever the application has changed since it was opened.
//
// Edits are kept as an overlay rather than by loading the whole fork into a
// mutable structure, because the two forks the game reads are megabytes of
// artwork it never writes to, and only a preferences or saved-game file is ever
// dirty. An edit carries the guest handle rather than a copy of its bytes: the
// application writes through the handle after AddResource and expects the file
// to end up holding whatever it last wrote there.
struct OpenResFile {
  s16 refNum = 0;
  std::string name;
  std::string path;          // where a flush writes back
  bool writable = false;
  bool dirty = false;
  ResourceFork fork;
  std::map<std::pair<ResType, s16>, GuestAddr> loaded;

  struct Edit {
    std::string name;
    u8 attrs = 0;
    GuestAddr handle = 0;
    bool removed = false;
  };
  std::map<std::pair<ResType, s16>, Edit> edits;
};

class ResourceMgr {
 public:
  s16 lastError = kNoErr;
  bool resLoad = true;

  s16 open(const std::string& path, const std::string& name, bool writable,
           std::string* err) {
    std::vector<u8> blob;
    if (!readFile(path, &blob)) {
      if (err) *err = "cannot read " + path;
      lastError = kResFNotFound;
      return -1;
    }
    auto f = std::make_unique<OpenResFile>();
    f->refNum = nextRef_++;
    f->name = name;
    f->path = path;
    f->writable = writable;
    if (!f->fork.parse(std::move(blob), err)) {
      lastError = kResFNotFound;
      return -1;
    }
    s16 ref = f->refNum;
    files_[ref] = std::move(f);
    // Newly opened files go to the front of the search order and become current.
    search_.insert(search_.begin(), ref);
    current_ = ref;
    lastError = kNoErr;
    return ref;
  }

  OpenResFile* file(s16 ref) {
    auto it = files_.find(ref);
    return it == files_.end() ? nullptr : it->second.get();
  }
  s16 current() const { return current_; }
  void setCurrent(s16 ref) { if (files_.count(ref)) current_ = ref; }
  const std::vector<s16>& searchOrder() const { return search_; }

  // The file and key that own a handle the application is holding, or null if
  // the handle is not a resource. Every write call is addressed this way.
  OpenResFile* ownerOf(GuestAddr h, std::pair<ResType, s16>* key) {
    if (!h) return nullptr;
    for (s16 ref : search_) {
      OpenResFile* f = file(ref);
      if (!f) continue;
      for (const auto& [k, handle] : f->loaded)
        if (handle == h) { if (key) *key = k; return f; }
    }
    return nullptr;
  }

 private:
  static bool readFile(const std::string& path, std::vector<u8>* out);

  std::map<s16, std::unique_ptr<OpenResFile>> files_;
  std::vector<s16> search_;
  s16 current_ = 0;
  s16 nextRef_ = 2;   // 0 is the System file, 1 the (absent) ROM map
};

}  // namespace cyt

#include <fstream>

namespace cyt {

bool ResourceMgr::readFile(const std::string& path, std::vector<u8>* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  out->resize(size_t(in.tellg()));
  in.seekg(0);
  in.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size()));
  return true;
}

static ResourceMgr g_res;

ResourceMgr& resourceMgr() { return g_res; }

// Materialises a resource as a guest handle, reusing the handle if the resource
// has already been loaded from this file.
static GuestAddr loadResource(Toolbox& tb, OpenResFile& f, ResType type,
                              const ResEntry& e) {
  auto key = std::make_pair(type, e.id);
  auto it = f.loaded.find(key);
  if (it != f.loaded.end()) return it->second;

  // SetResLoad(false) asks for the handle without the data.
  const u32 want = g_res.resLoad ? e.length : 0;
  GuestAddr h = tb.heap().newHandle(want, false);
  if (!h) {
    g_res.lastError = -108;  // memFullErr
    return 0;
  }
  if (want) {
    const u8* src = f.fork.data(e);
    if (src) tb.mem().copyIn(tb.mem().r32(h), src, want);
  }
  f.loaded[key] = h;
  g_res.lastError = kNoErr;
  return h;
}

// The current bytes of a resource the application has edited. AddResource
// hands the Resource Manager a handle, not a copy, so what reaches the file is
// whatever the application last wrote through it.
static std::vector<u8> handleBytes(Toolbox& tb, GuestAddr h) {
  std::vector<u8> out(tb.heap().handleSize(h));
  const GuestAddr p = tb.mem().r32(h);
  if (!out.empty() && p) tb.mem().copyOut(out.data(), p, out.size());
  return out;
}

// Writes a file back to disk: everything its fork already held, minus what has
// been removed, with the edits laid over the top. The fork is then re-read so
// that what is in memory matches what is on disk -- otherwise a later flush
// would write the original bytes back over an edit that is no longer pending.
static bool flushResFile(Toolbox& tb, OpenResFile& f) {
  if (!f.dirty) return true;
  if (!f.writable || f.path.empty()) {
    g_res.lastError = kWrPermErr;
    return false;
  }

  std::vector<ResWrite> items;
  for (u16 i = 1; i <= f.fork.typeCount(); ++i) {
    const ResType type = f.fork.typeAtIndex(i);
    const std::vector<ResEntry>* entries = f.fork.entriesFor(type);
    if (!entries) continue;
    for (const ResEntry& e : *entries) {
      if (f.edits.count({type, e.id})) continue;   // superseded below
      ResWrite w;
      w.type = type;
      w.id = e.id;
      w.name = e.name;
      w.attrs = e.attrs;
      // A resource the application is holding may have been changed through
      // its handle without ChangedResource; taking the handle's bytes when one
      // exists is both correct and what the original did.
      auto it = f.loaded.find({type, e.id});
      if (it != f.loaded.end() && tb.heap().validHandle(it->second))
        w.data = handleBytes(tb, it->second);
      else if (const u8* d = f.fork.data(e))
        w.data.assign(d, d + e.length);
      items.push_back(std::move(w));
    }
  }
  for (const auto& [key, e] : f.edits) {
    if (e.removed) continue;
    ResWrite w;
    w.type = key.first;
    w.id = key.second;
    w.name = e.name;
    w.attrs = e.attrs;
    if (e.handle) w.data = handleBytes(tb, e.handle);
    items.push_back(std::move(w));
  }

  std::vector<u8> blob = buildResourceFork(items);
  std::ofstream out(f.path, std::ios::binary | std::ios::trunc);
  if (!out) {
    g_res.lastError = kIoErr;
    return false;
  }
  out.write(reinterpret_cast<const char*>(blob.data()),
            std::streamsize(blob.size()));
  out.close();

  std::string err;
  if (!f.fork.parse(std::move(blob), &err)) {
    std::fprintf(stderr, "  [ResMgr] wrote a fork it cannot read back: %s\n",
                 err.c_str());
    g_res.lastError = kIoErr;
    return false;
  }
  f.edits.clear();
  f.dirty = false;
  g_res.lastError = kNoErr;
  return true;
}

void registerResourceManager(Toolbox& tb) {
  // Searches the files in order, optionally restricted to the current one.
  auto lookup = [](Toolbox& tb, ResType type, s16 id, bool currentOnly,
                   OpenResFile** owner) -> const ResEntry* {
    auto tryFile = [&](s16 ref) -> const ResEntry* {
      OpenResFile* f = g_res.file(ref);
      if (!f) return nullptr;
      const ResEntry* e = f->fork.find(type, id);
      if (e) *owner = f;
      return e;
    };
    if (currentOnly) return tryFile(g_res.current());
    for (s16 ref : g_res.searchOrder())
      if (const ResEntry* e = tryFile(ref)) return e;
    return nullptr;
  };

  auto getResource = [lookup](bool currentOnly) {
    return [lookup, currentOnly](Toolbox& tb, PpcCpu& c, Args& a) {
      ResType type = a.u32v();
      s16 id = a.s16v();
      OpenResFile* owner = nullptr;
      const ResEntry* e = lookup(tb, type, id, currentOnly, &owner);
      if (!e) {
        g_res.lastError = kResNotFound;
        Toolbox::ret(c, 0);
        return;
      }
      Toolbox::ret(c, loadResource(tb, *owner, type, *e));
    };
  };
  tb.add("GetResource", getResource(false));
  tb.add("Get1Resource", getResource(true));

  auto getNamed = [](bool currentOnly) {
    return [currentOnly](Toolbox& tb, PpcCpu& c, Args& a) {
      ResType type = a.u32v();
      std::string name = tb.mem().pstr(a.ptr());
      for (s16 ref : g_res.searchOrder()) {
        if (currentOnly && ref != g_res.current()) continue;
        OpenResFile* f = g_res.file(ref);
        if (!f) continue;
        if (const ResEntry* e = f->fork.findNamed(type, name)) {
          Toolbox::ret(c, loadResource(tb, *f, type, *e));
          return;
        }
      }
      g_res.lastError = kResNotFound;
      Toolbox::ret(c, 0);
    };
  };
  tb.add("GetNamedResource", getNamed(false));
  tb.add("Get1NamedResource", getNamed(true));

  auto getInd = [](bool currentOnly) {
    return [currentOnly](Toolbox& tb, PpcCpu& c, Args& a) {
      ResType type = a.u32v();
      s16 index = a.s16v();
      for (s16 ref : g_res.searchOrder()) {
        if (currentOnly && ref != g_res.current()) continue;
        OpenResFile* f = g_res.file(ref);
        if (!f) continue;
        if (const ResEntry* e = f->fork.findIndex(type, u16(index))) {
          Toolbox::ret(c, loadResource(tb, *f, type, *e));
          return;
        }
      }
      g_res.lastError = kResNotFound;
      Toolbox::ret(c, 0);
    };
  };
  tb.add("GetIndResource", getInd(false));
  tb.add("Get1IndResource", getInd(true));

  auto countRes = [](bool currentOnly) {
    return [currentOnly](Toolbox& tb, PpcCpu& c, Args& a) {
      ResType type = a.u32v();
      u32 n = 0;
      for (s16 ref : g_res.searchOrder()) {
        if (currentOnly && ref != g_res.current()) continue;
        if (OpenResFile* f = g_res.file(ref))
          if (const auto* v = f->fork.entriesFor(type)) n += u32(v->size());
      }
      g_res.lastError = kNoErr;
      Toolbox::ret(c, n);
    };
  };
  tb.add("CountResources", countRes(false));
  tb.add("Count1Resources", countRes(true));

  tb.add("ResError", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(s32(g_res.lastError)));
  });
  tb.add("SetResLoad", [](Toolbox& tb, PpcCpu& c, Args& a) {
    g_res.resLoad = a.u8v() != 0;
    g_res.lastError = kNoErr;
  });
  tb.add("CurResFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(s32(g_res.current())));
  });
  tb.add("UseResFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    g_res.setCurrent(a.s16v());
    g_res.lastError = kNoErr;
  });
  tb.add("HomeResFile", [](Toolbox& tb, PpcCpu& c, Args& a) {
    // Every resource this port hands out comes from the application's own fork.
    a.ptr();
    Toolbox::ret(c, u32(s32(g_res.current())));
  });

  // Releasing a resource drops the handle; the next request reloads it.
  tb.add("ReleaseResource", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    for (s16 ref : g_res.searchOrder()) {
      OpenResFile* f = g_res.file(ref);
      if (!f) continue;
      for (auto it = f->loaded.begin(); it != f->loaded.end(); ++it) {
        if (it->second == h) {
          tb.heap().disposeHandle(h);
          f->loaded.erase(it);
          g_res.lastError = kNoErr;
          return;
        }
      }
    }
    g_res.lastError = kResNotFound;
  });
  // Detaching hands ownership to the application: forget the handle but keep it.
  tb.add("DetachResource", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    for (s16 ref : g_res.searchOrder()) {
      OpenResFile* f = g_res.file(ref);
      if (!f) continue;
      for (auto it = f->loaded.begin(); it != f->loaded.end(); ++it) {
        if (it->second == h) {
          f->loaded.erase(it);
          g_res.lastError = kNoErr;
          return;
        }
      }
    }
    g_res.lastError = kResNotFound;
  });
  tb.add("LoadResource", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    g_res.lastError = kNoErr;
  });

  tb.add("GetResInfo", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    GuestAddr idPtr = a.ptr(), typePtr = a.ptr(), namePtr = a.ptr();
    for (s16 ref : g_res.searchOrder()) {
      OpenResFile* f = g_res.file(ref);
      if (!f) continue;
      for (const auto& [key, handle] : f->loaded) {
        if (handle != h) continue;
        if (idPtr) tb.mem().w16(idPtr, u16(key.second));
        if (typePtr) tb.mem().w32(typePtr, key.first);
        if (namePtr) {
          const ResEntry* e = f->fork.find(key.first, key.second);
          tb.mem().writePstr(namePtr, e ? e->name : std::string(), 256);
        }
        g_res.lastError = kNoErr;
        return;
      }
    }
    g_res.lastError = kResNotFound;
  });

  // ---- string resources ---------------------------------------------------
  // 'STR ' is a single Pascal string; 'STR#' is a count followed by that many
  // Pascal strings packed end to end.
  tb.add("GetString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    s16 id = a.s16v();
    OpenResFile* owner = nullptr;
    for (s16 ref : g_res.searchOrder()) {
      OpenResFile* f = g_res.file(ref);
      if (f && f->fork.find(resType("STR "), id)) { owner = f; break; }
    }
    if (!owner) { g_res.lastError = kResNotFound; Toolbox::ret(c, 0); return; }
    const ResEntry* e = owner->fork.find(resType("STR "), id);
    Toolbox::ret(c, loadResource(tb, *owner, resType("STR "), *e));
  });
  tb.add("GetIndString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr out = a.ptr();
    s16 listId = a.s16v();
    s16 index = a.s16v();
    tb.mem().w8(out, 0);
    for (s16 ref : g_res.searchOrder()) {
      OpenResFile* f = g_res.file(ref);
      if (!f) continue;
      const ResEntry* e = f->fork.find(resType("STR#"), listId);
      if (!e) continue;
      const u8* d = f->fork.data(*e);
      if (!d || e->length < 2) break;
      u16 count = u16(u16(d[0]) << 8 | d[1]);
      if (index < 1 || index > s16(count)) break;
      u32 off = 2;
      for (s16 i = 1; i <= index; ++i) {
        if (off >= e->length) return;
        u8 n = d[off];
        if (i == index) {
          std::string s(reinterpret_cast<const char*>(d + off + 1),
                        (off + 1 + n <= e->length) ? n : 0);
          tb.mem().writePstr(out, s, 256);
          g_res.lastError = kNoErr;
          return;
        }
        off += 1u + n;
      }
      break;
    }
    g_res.lastError = kResNotFound;
  });

  // ---- opening and closing resource files ---------------------------------
  // The application's own fork is opened by the launcher before main runs, so
  // these mostly serve "Cythera Data". The file specification is resolved by
  // the File Manager layer, which maps guest paths onto the extracted forks.
  // Both of these write a dirty file back. CloseResFile should also drop the
  // file from the search order, but this port never reopens one within a run
  // and the game reads its preferences back through the same handles, so the
  // file is left open and only the bytes are committed.
  auto flushFile = [](Toolbox& tb, PpcCpu& c, Args& a) {
    OpenResFile* f = g_res.file(a.s16v());
    if (!f) { g_res.lastError = kResFNotFound; return; }
    flushResFile(tb, *f);
  };
  tb.add("CloseResFile", flushFile);
  tb.add("UpdateResFile", flushFile);

  // ---- writing ------------------------------------------------------------
  // A resource is added by handing the Resource Manager a handle, which it then
  // owns as far as the file is concerned; the application keeps writing through
  // it, and says so with ChangedResource. Nothing reaches the disk until
  // UpdateResFile or CloseResFile.
  tb.add("AddResource", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    ResType type = a.u32v();
    s16 id = a.s16v();
    std::string name = tb.mem().pstr(a.ptr());
    OpenResFile* f = g_res.file(g_res.current());
    if (!f) { g_res.lastError = kResFNotFound; return; }
    if (!h || g_res.ownerOf(h, nullptr)) {
      g_res.lastError = kAddResFailed;
      return;
    }
    const auto key = std::make_pair(type, id);
    f->edits[key] = {std::move(name), 0, h, false};
    f->loaded[key] = h;
    f->dirty = true;
    g_res.lastError = kNoErr;
  });

  // Turns a resource that came from the fork into an edit, so that a flush
  // takes its bytes from the handle rather than from the file it was read out
  // of. Resources added during this run are already edits.
  auto asEdit = [](OpenResFile& f, const std::pair<ResType, s16>& key,
                   GuestAddr h) -> OpenResFile::Edit& {
    auto it = f.edits.find(key);
    if (it != f.edits.end()) return it->second;
    OpenResFile::Edit e;
    e.handle = h;
    if (const ResEntry* orig = f.fork.find(key.first, key.second)) {
      e.name = orig->name;
      e.attrs = orig->attrs;
    }
    return f.edits.emplace(key, std::move(e)).first->second;
  };

  auto markChanged = [asEdit](Toolbox& tb, PpcCpu& c, Args& a, bool flushNow) {
    GuestAddr h = a.ptr();
    std::pair<ResType, s16> key;
    OpenResFile* f = g_res.ownerOf(h, &key);
    if (!f) { g_res.lastError = kResNotFound; return; }
    asEdit(*f, key, h);
    f->dirty = true;
    g_res.lastError = kNoErr;
    if (flushNow) flushResFile(tb, *f);
  };
  tb.add("ChangedResource", [markChanged](Toolbox& tb, PpcCpu& c, Args& a) {
    markChanged(tb, c, a, false);
  });
  // WriteResource commits one resource immediately. This port has the file to
  // itself, so committing the whole file is the same thing to any observer and
  // avoids keeping a second, partial layout of the fork.
  tb.add("WriteResource", [markChanged](Toolbox& tb, PpcCpu& c, Args& a) {
    markChanged(tb, c, a, true);
  });

  // Removing a resource detaches it from the file but does not dispose the
  // handle: the application owns it afterwards and is expected to.
  tb.add("RemoveResource", [asEdit](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    std::pair<ResType, s16> key;
    OpenResFile* f = g_res.ownerOf(h, &key);
    if (!f) { g_res.lastError = kResNotFound; return; }
    asEdit(*f, key, h).removed = true;
    f->loaded.erase(key);
    f->dirty = true;
    g_res.lastError = kNoErr;
  });

  tb.add("SetResInfo", [asEdit](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 id = a.s16v();
    GuestAddr namePtr = a.ptr();
    std::pair<ResType, s16> key;
    OpenResFile* f = g_res.ownerOf(h, &key);
    if (!f) { g_res.lastError = kResNotFound; return; }
    OpenResFile::Edit e = asEdit(*f, key, h);
    if (namePtr) e.name = tb.mem().pstr(namePtr);
    const auto fresh = std::make_pair(key.first, id);
    if (fresh != key) {
      f->edits.erase(key);
      f->loaded.erase(key);
      // The old identity has to be written out as removed as well, or the
      // entry the fork still carries under it would survive the flush.
      OpenResFile::Edit gone;
      gone.removed = true;
      f->edits[key] = gone;
      f->loaded[fresh] = h;
    }
    f->edits[fresh] = std::move(e);
    f->dirty = true;
    g_res.lastError = kNoErr;
  });
  tb.add("SetResAttrs", [asEdit](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    s16 attrs = a.s16v();
    std::pair<ResType, s16> key;
    OpenResFile* f = g_res.ownerOf(h, &key);
    if (!f) { g_res.lastError = kResNotFound; return; }
    asEdit(*f, key, h).attrs = u8(attrs);
    f->dirty = true;
    g_res.lastError = kNoErr;
  });
  tb.add("GetResAttrs", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr h = a.ptr();
    std::pair<ResType, s16> key;
    OpenResFile* f = g_res.ownerOf(h, &key);
    if (!f) { g_res.lastError = kResNotFound; Toolbox::ret(c, 0); return; }
    auto it = f->edits.find(key);
    u8 attrs = 0;
    if (it != f->edits.end()) attrs = it->second.attrs;
    else if (const ResEntry* e = f->fork.find(key.first, key.second))
      attrs = e->attrs;
    g_res.lastError = kNoErr;
    Toolbox::ret(c, attrs);
  });

  // The lowest identifier of this type that the current file does not already
  // use. The original returned an arbitrary unused one; what callers rely on is
  // only that AddResource with it will not collide.
  tb.add("UniqueID", [](Toolbox& tb, PpcCpu& c, Args& a) {
    ResType type = a.u32v();
    OpenResFile* f = g_res.file(g_res.current());
    s16 id = 128;
    while (f && (f->fork.find(type, id) || f->edits.count({type, id}))) ++id;
    g_res.lastError = kNoErr;
    Toolbox::ret(c, u32(s32(id)));
  });
}

// What ResError() will report. The File Manager layer sets it from the calls
// that create a resource file, which fail or succeed before any of this file's
// own machinery is involved.
void setResError(s16 err) { g_res.lastError = err; }

// Opens an additional resource file, as FSpOpenResFile does. The new file goes
// to the front of the search order and becomes current, matching the Resource
// Manager's own behaviour.
s16 openResourceFile(const std::string& path, const std::string& name,
                     bool writable, std::string* err) {
  return g_res.open(path, name, writable, err);
}

// Reads a resource's raw bytes without creating a guest handle. Used by the
// alert machinery, which needs to inspect ALRT and DITL data to report a
// message rather than to hand it to the application.
bool peekResource(ResType type, s16 id, std::vector<u8>* out) {
  for (s16 ref : g_res.searchOrder()) {
    OpenResFile* f = g_res.file(ref);
    if (!f) continue;
    if (const ResEntry* e = f->fork.find(type, id)) {
      const u8* d = f->fork.data(*e);
      if (!d) return false;
      out->assign(d, d + e->length);
      return true;
    }
  }
  return false;
}

// Every resource of a type, across every open fork, in search order. Unlike
// peekResource this does not stop at the first match, because two forks may
// each define a resource with the same id and both be meaningful -- which is
// exactly the case for Cythera's two FOND 128s.
bool listResources(ResType type, std::vector<ResItem>* out) {
  const size_t before = out->size();
  for (s16 ref : g_res.searchOrder()) {
    OpenResFile* f = g_res.file(ref);
    if (!f) continue;
    const std::vector<ResEntry>* entries = f->fork.entriesFor(type);
    if (!entries) continue;
    for (const ResEntry& e : *entries) {
      const u8* d = f->fork.data(e);
      if (!d) continue;
      ResItem item;
      item.id = e.id;
      item.name = e.name;
      item.data.assign(d, d + e.length);
      out->push_back(std::move(item));
    }
  }
  return out->size() != before;
}

// Opens the application's own resource fork, as the launcher would have done.
bool openApplicationResources(const std::string& path, std::string* err) {
  return g_res.open(path, "Cythera", false, err) >= 0;
}

}  // namespace cyt
