#include "resfork.h"

#include <map>
#include <sstream>

namespace cyt {
namespace {
inline u16 be16(const std::vector<u8>& b, u64 o) {
  return u16(u16(b[o]) << 8 | b[o + 1]);
}
inline u32 be32(const std::vector<u8>& b, u64 o) {
  return u32(b[o]) << 24 | u32(b[o + 1]) << 16 | u32(b[o + 2]) << 8 | b[o + 3];
}
}  // namespace

std::string resTypeName(ResType t) {
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) {
    char c = char((t >> (24 - 8 * i)) & 0xFF);
    s[size_t(i)] = (c >= 32 && c < 127) ? c : '.';
  }
  return s;
}

// The fork's fixed layout, from Inside Macintosh: More Macintosh Toolbox.
//
//   header  16 bytes at offset 0: data offset, map offset, data length,
//           map length, followed by 240 reserved bytes, so the data area
//           conventionally starts at 256
//   data    each resource as a 32-bit length followed by its bytes
//   map     28 bytes of bookkeeping, then the type list, the reference lists
//           and the name list, all addressed relative to the map or the list
//
// Both counts in the map are stored as one less than the real count, which is
// why an empty fork writes 0xFFFF rather than zero.
std::vector<u8> buildResourceFork(const std::vector<ResWrite>& items) {
  constexpr u32 kDataStart = 256;

  auto put16 = [](std::vector<u8>& b, u32 v) {
    b.push_back(u8(v >> 8)); b.push_back(u8(v));
  };
  auto put32 = [](std::vector<u8>& b, u32 v) {
    b.push_back(u8(v >> 24)); b.push_back(u8(v >> 16));
    b.push_back(u8(v >> 8));  b.push_back(u8(v));
  };

  // Group by type, keeping the order in which each type was first seen.
  std::vector<ResType> types;
  std::map<ResType, std::vector<const ResWrite*>> byType;
  for (const ResWrite& r : items) {
    if (!byType.count(r.type)) types.push_back(r.type);
    byType[r.type].push_back(&r);
  }

  // The data area, and each resource's offset within it.
  std::vector<u8> data;
  std::map<const ResWrite*, u32> dataOffset;
  for (ResType t : types)
    for (const ResWrite* r : byType[t]) {
      dataOffset[r] = u32(data.size());
      put32(data, u32(r->data.size()));
      data.insert(data.end(), r->data.begin(), r->data.end());
    }

  // The name list, and each named resource's offset within it. Unnamed
  // resources are marked with -1 rather than pointing at an empty string.
  std::vector<u8> names;
  std::map<const ResWrite*, s16> nameOffset;
  for (ResType t : types)
    for (const ResWrite* r : byType[t]) {
      if (r->name.empty()) { nameOffset[r] = -1; continue; }
      nameOffset[r] = s16(names.size());
      const u8 n = u8(r->name.size() > 255 ? 255 : r->name.size());
      names.push_back(n);
      names.insert(names.end(), r->name.begin(), r->name.begin() + n);
    }

  // Type list at 28, then the reference lists, then the names. Reference-list
  // offsets are measured from the start of the type list, not the map.
  const u32 typeListOff = 28;
  const u32 refListsOff = typeListOff + 2 + 8 * u32(types.size());
  u32 totalRefs = 0;
  for (ResType t : types) totalRefs += u32(byType[t].size());
  const u32 nameListOff = refListsOff + 12 * totalRefs;

  std::vector<u8> map;
  map.resize(16, 0);            // a copy of the header, which readers ignore
  put32(map, 0);                // handle to the next map
  put16(map, 0);                // file reference number
  put16(map, 0);                // fork attributes
  put16(map, typeListOff);
  put16(map, nameListOff);
  put16(map, u16(types.size() - 1));   // 0xFFFF when there are none

  u32 refCursor = refListsOff - typeListOff;
  for (ResType t : types) {
    put32(map, t);
    put16(map, u16(byType[t].size() - 1));
    put16(map, u16(refCursor));
    refCursor += 12 * u32(byType[t].size());
  }
  for (ResType t : types)
    for (const ResWrite* r : byType[t]) {
      put16(map, u16(r->id));
      put16(map, u16(nameOffset[r]));
      put32(map, (u32(r->attrs) << 24) | (dataOffset[r] & 0x00FFFFFF));
      put32(map, 0);            // the in-memory handle, always zero on disk
    }
  map.insert(map.end(), names.begin(), names.end());

  std::vector<u8> out;
  out.reserve(kDataStart + data.size() + map.size());
  put32(out, kDataStart);
  put32(out, kDataStart + u32(data.size()));
  put32(out, u32(data.size()));
  put32(out, u32(map.size()));
  out.resize(kDataStart, 0);
  out.insert(out.end(), data.begin(), data.end());
  out.insert(out.end(), map.begin(), map.end());
  return out;
}

bool ResourceFork::parse(std::vector<u8> blob, std::string* err) {
  blob_ = std::move(blob);
  const auto& b = blob_;
  auto bad = [&](const char* what) {
    if (err) *err = std::string("resource fork: ") + what;
    return false;
  };
  if (b.size() < 16) return bad("shorter than a header");

  const u32 dataOff = be32(b, 0);
  const u32 mapOff  = be32(b, 4);
  const u32 dataLen = be32(b, 8);
  const u32 mapLen  = be32(b, 12);
  if (u64(mapOff) + 30 > b.size() || u64(dataOff) + dataLen > b.size() ||
      u64(mapOff) + mapLen > b.size())
    return bad("header offsets fall outside the fork");

  const u32 typeListOff = mapOff + be16(b, mapOff + 24);
  const u32 nameListOff = mapOff + be16(b, mapOff + 26);
  if (u64(typeListOff) + 2 > b.size()) return bad("type list is out of range");

  // Both counts are stored as "one less than the real count", so a fork with no
  // resources at all -- which is exactly what CreateResFile produces -- holds
  // 0xFFFF here and must not be read as 65536 types.
  const u16 rawTypes = be16(b, typeListOff);
  const u32 nTypes = rawTypes == 0xFFFF ? 0 : u32(rawTypes) + 1;
  for (u32 i = 0; i < nTypes; ++i) {
    const u64 to = u64(typeListOff) + 2 + u64(i) * 8;
    if (to + 8 > b.size()) return bad("truncated type list");
    const ResType type = be32(b, to);
    const u32 nRefs = u32(be16(b, to + 4)) + 1;
    const u32 refOff = be16(b, to + 6);

    std::vector<ResEntry> entries;
    entries.reserve(nRefs);
    for (u32 j = 0; j < nRefs; ++j) {
      const u64 ro = u64(typeListOff) + refOff + u64(j) * 12;
      if (ro + 12 > b.size()) return bad("truncated reference list");
      ResEntry e;
      e.id = s16(be16(b, ro));
      const s16 nameOff = s16(be16(b, ro + 2));
      const u32 packed = be32(b, ro + 4);
      e.attrs = u8(packed >> 24);
      const u32 rel = packed & 0x00FFFFFF;
      const u64 abs = u64(dataOff) + rel;
      if (abs + 4 > b.size()) return bad("resource data offset is out of range");
      e.length = be32(b, abs);
      if (abs + 4 + e.length > b.size()) return bad("resource data overruns");
      e.dataOffset = u32(abs + 4);
      if (nameOff >= 0) {
        const u64 no = u64(nameListOff) + u32(nameOff);
        if (no < b.size()) {
          const u32 n = b[no];
          if (no + 1 + n <= b.size())
            e.name.assign(reinterpret_cast<const char*>(&b[no + 1]), n);
        }
      }
      entries.push_back(std::move(e));
    }
    byType_[type] = std::move(entries);
    order_.push_back(type);
  }
  loaded_ = true;
  return true;
}

const std::vector<ResEntry>* ResourceFork::entriesFor(ResType t) const {
  auto it = byType_.find(t);
  return it == byType_.end() ? nullptr : &it->second;
}

const ResEntry* ResourceFork::find(ResType t, s16 id) const {
  const auto* v = entriesFor(t);
  if (!v) return nullptr;
  for (const auto& e : *v)
    if (e.id == id) return &e;
  return nullptr;
}

const ResEntry* ResourceFork::findNamed(ResType t, const std::string& n) const {
  const auto* v = entriesFor(t);
  if (!v) return nullptr;
  for (const auto& e : *v)
    if (e.name == n) return &e;
  return nullptr;
}

const ResEntry* ResourceFork::findIndex(ResType t, u16 idx) const {
  const auto* v = entriesFor(t);
  if (!v || idx == 0 || idx > v->size()) return nullptr;
  return &(*v)[idx - 1];
}

ResType ResourceFork::typeAtIndex(u16 idx) const {
  if (idx == 0 || idx > order_.size()) return 0;
  return order_[idx - 1];
}

const u8* ResourceFork::data(const ResEntry& e) const {
  if (u64(e.dataOffset) + e.length > blob_.size()) return nullptr;
  return blob_.data() + e.dataOffset;
}

}  // namespace cyt
