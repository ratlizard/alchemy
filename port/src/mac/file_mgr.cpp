#include "mac/file_mgr.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cyt {
namespace {
// Data-fork refNums start well above the resource-file refNums so a mix-up is
// obvious rather than silently addressing the wrong table.
constexpr s16 kFirstRefNum = 100;
constexpr s16 kNoErr   = 0;
constexpr s16 kEofErr  = -39;
constexpr s16 kFnfErr  = -43;
constexpr s16 kIoErr   = -36;
constexpr s16 kRfNumErr= -51;
}  // namespace

FileMgr& FileMgr::get() {
  static FileMgr instance;
  return instance;
}

std::string FileMgr::resolve(const std::string& macName, bool resourceFork,
                             bool forWriting) const {
  namespace fs = std::filesystem;
  const std::string suffix = resourceFork ? ".rsrc" : ".data";

  // Original game files live in the game directory; saves in the support one.
  for (const std::string* dir : {&gameDir_, &supportDir_}) {
    if (dir->empty()) continue;
    // Prefer the explicit fork file, then the bare name for the data fork.
    fs::path forked = fs::path(*dir) / (macName + suffix);
    if (fs::exists(forked)) return forked.string();
    if (!resourceFork) {
      fs::path bare = fs::path(*dir) / macName;
      if (fs::exists(bare) && fs::is_regular_file(bare)) return bare.string();
    }
  }
  if (!forWriting) return {};
  // A new file is created in the support directory, never beside the originals.
  if (supportDir_.empty()) return {};
  std::error_code ec;
  fs::create_directories(supportDir_, ec);
  return (fs::path(supportDir_) / (macName + suffix)).string();
}

bool FileMgr::exists(const std::string& macName) const {
  return !resolve(macName, false, false).empty() ||
         !resolve(macName, true, false).empty();
}

std::vector<std::string> FileMgr::listSupportFiles() const {
  namespace fs = std::filesystem;
  std::vector<std::string> names;
  if (supportDir_.empty()) return names;
  auto endsWith = [](const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
  };
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(supportDir_, ec)) {
    if (!entry.is_regular_file()) continue;
    std::string name = entry.path().filename().string();
    // Neither the resource fork nor the Finder-info sidecar is a file in its
    // own right: each belongs to the Mac file the data fork already names.
    if (endsWith(name, ".rsrc") || endsWith(name, ".finf")) continue;
    if (endsWith(name, ".data")) name.resize(name.size() - 5);
    if (!name.empty()) names.push_back(std::move(name));
  }
  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  return names;
}

// The sidecar holding one file's Finder metadata: ten bytes, big-endian, in
// the same order as the FInfo record the Toolbox passes around.
static std::string finderInfoPath(const std::string& dir,
                                  const std::string& macName) {
  if (dir.empty()) return {};
  return (std::filesystem::path(dir) / (macName + ".finf")).string();
}

bool FileMgr::getFinderInfo(const std::string& macName, FinderInfo* out) const {
  for (const std::string* dir : {&gameDir_, &supportDir_}) {
    std::string path = finderInfoPath(*dir, macName);
    if (path.empty()) continue;
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    u8 b[10] = {};
    in.read(reinterpret_cast<char*>(b), 10);
    if (in.gcount() != 10) continue;
    if (out) {
      out->type    = u32(b[0]) << 24 | u32(b[1]) << 16 | u32(b[2]) << 8 | b[3];
      out->creator = u32(b[4]) << 24 | u32(b[5]) << 16 | u32(b[6]) << 8 | b[7];
      out->flags   = u16(u16(b[8]) << 8 | b[9]);
    }
    return true;
  }
  return false;
}

bool FileMgr::setFinderInfo(const std::string& macName, const FinderInfo& fi) {
  // Metadata is only ever recorded for files the port owns; the originals are
  // left exactly as they were extracted.
  if (supportDir_.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(supportDir_, ec);
  std::ofstream out(finderInfoPath(supportDir_, macName), std::ios::binary);
  if (!out) return false;
  const u8 b[10] = {
      u8(fi.type >> 24),    u8(fi.type >> 16),    u8(fi.type >> 8),
      u8(fi.type),          u8(fi.creator >> 24), u8(fi.creator >> 16),
      u8(fi.creator >> 8),  u8(fi.creator),       u8(fi.flags >> 8),
      u8(fi.flags)};
  out.write(reinterpret_cast<const char*>(b), 10);
  return bool(out);
}

s16 FileMgr::openFork(const std::string& macName, bool resourceFork,
                      bool writable, s16* refOut) {
  std::string path = resolve(macName, resourceFork, writable);
  if (path.empty()) return kFnfErr;

  Open o;
  o.path = path;
  o.writable = writable;
  std::ifstream in(path, std::ios::binary);
  if (in) {
    in.seekg(0, std::ios::end);
    o.buf.resize(size_t(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(o.buf.data()),
            std::streamsize(o.buf.size()));
  } else if (!writable) {
    return kFnfErr;
  }
  open_.push_back(std::move(o));
  if (refOut) *refOut = s16(kFirstRefNum + s16(open_.size()) - 1);
  return kNoErr;
}

bool FileMgr::valid(s16 ref) const {
  size_t i = size_t(ref - kFirstRefNum);
  return ref >= kFirstRefNum && i < open_.size() && !open_[i].path.empty();
}

bool FileMgr::close(s16 ref) {
  if (!valid(ref)) return false;
  Open& o = open_[size_t(ref - kFirstRefNum)];
  if (o.dirty && o.writable) {
    std::ofstream out(o.path, std::ios::binary | std::ios::trunc);
    if (out) out.write(reinterpret_cast<const char*>(o.buf.data()),
                       std::streamsize(o.buf.size()));
  }
  o.path.clear();
  o.buf.clear();
  o.buf.shrink_to_fit();
  return true;
}

s16 FileMgr::read(s16 ref, GuestAddr buf, u32 want, u32* got) {
  if (!valid(ref)) return kRfNumErr;
  Open& o = open_[size_t(ref - kFirstRefNum)];
  u32 avail = o.pos < o.buf.size() ? u32(o.buf.size()) - o.pos : 0;
  u32 n = want < avail ? want : avail;
  if (n) Mem::get().copyIn(buf, o.buf.data() + o.pos, n);
  o.pos += n;
  if (got) *got = n;
  // A short read is reported as eofErr with the partial count, which is what
  // callers looping to the end of a file expect.
  return n == want ? kNoErr : kEofErr;
}

s16 FileMgr::write(s16 ref, GuestAddr buf, u32 want, u32* wrote) {
  if (!valid(ref)) return kRfNumErr;
  Open& o = open_[size_t(ref - kFirstRefNum)];
  if (!o.writable) return -61;   // wrPermErr
  if (o.pos + want > o.buf.size()) o.buf.resize(o.pos + want);
  std::vector<u8> tmp(want);
  Mem::get().copyOut(tmp.data(), buf, want);
  std::copy(tmp.begin(), tmp.end(), o.buf.begin() + o.pos);
  o.pos += want;
  o.dirty = true;
  if (wrote) *wrote = want;
  return kNoErr;
}

s16 FileMgr::eof(s16 ref, u32* len) {
  if (!valid(ref)) return kRfNumErr;
  if (len) *len = u32(open_[size_t(ref - kFirstRefNum)].buf.size());
  return kNoErr;
}

s16 FileMgr::setEof(s16 ref, u32 len) {
  if (!valid(ref)) return kRfNumErr;
  Open& o = open_[size_t(ref - kFirstRefNum)];
  if (!o.writable) return -61;
  o.buf.resize(len);
  if (o.pos > len) o.pos = len;
  o.dirty = true;
  return kNoErr;
}

s16 FileMgr::setPos(s16 ref, s16 mode, s32 offset) {
  if (!valid(ref)) return kRfNumErr;
  Open& o = open_[size_t(ref - kFirstRefNum)];
  s64 target;
  switch (mode & 3) {
    case 1: target = offset; break;                      // fsFromStart
    case 2: target = s64(o.buf.size()) + offset; break;   // fsFromLEOF
    case 3: target = s64(o.pos) + offset; break;          // fsFromMark
    default: target = o.pos; break;                       // fsAtMark
  }
  if (target < 0) return kIoErr;
  if (target > s64(o.buf.size())) { o.pos = u32(o.buf.size()); return kEofErr; }
  o.pos = u32(target);
  return kNoErr;
}

s16 FileMgr::getPos(s16 ref, u32* pos) {
  if (!valid(ref)) return kRfNumErr;
  if (pos) *pos = open_[size_t(ref - kFirstRefNum)].pos;
  return kNoErr;
}

}  // namespace cyt
