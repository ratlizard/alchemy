// Reader for a classic Macintosh resource fork.
//
// The layout is a 16-byte header pointing at a data area and a map; the map
// holds a type list, and each type has a reference list of (id, name, data
// offset) entries. Nothing here allocates guest memory -- the Resource Manager
// layer decides when a resource becomes a handle.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "mem.h"

namespace cyt {

// A four-character resource type, kept as the big-endian u32 the Toolbox uses,
// so that comparisons are integer comparisons.
using ResType = u32;

inline ResType resType(const char s[5]) {
  return u32(u8(s[0])) << 24 | u32(u8(s[1])) << 16 | u32(u8(s[2])) << 8 |
         u32(u8(s[3]));
}
std::string resTypeName(ResType t);

struct ResEntry {
  s16 id = 0;
  std::string name;
  u8 attrs = 0;
  u32 dataOffset = 0;   // absolute offset of the length-prefixed data
  u32 length = 0;
};

// One resource found while scanning every open resource file. The Font Manager
// needs names as well as data, and needs *every* occurrence of a type rather
// than just the first: FOND 128 exists in both of Cythera's forks under
// different names ("Rogue Font" in the application, "Seldane" in the data
// file), and both families are real.
struct ResItem {
  s16 id = 0;
  std::string name;
  std::vector<u8> data;
};

// Every resource of a type across all open resource files, in search order.
// Defined by the Resource Manager layer, alongside peekResource.
bool listResources(ResType type, std::vector<ResItem>* out);

// One resource on its way to disk. Writing a fork is kept separate from
// reading one because the two share almost nothing: a reader indexes bytes that
// are already laid out, while a writer has to choose the layout.
struct ResWrite {
  ResType type = 0;
  s16 id = 0;
  std::string name;
  u8 attrs = 0;
  std::vector<u8> data;
};

// Serialises a complete resource fork. Resources are grouped by type in the
// order the types first appear, which is the order a reader will report them
// through GetIndType and Get1IndResource.
std::vector<u8> buildResourceFork(const std::vector<ResWrite>& items);

class ResourceFork {
 public:
  // Parses `blob`, which must be an entire resource fork.
  bool parse(std::vector<u8> blob, std::string* err);

  const std::vector<ResEntry>* entriesFor(ResType t) const;
  const ResEntry* find(ResType t, s16 id) const;
  const ResEntry* findNamed(ResType t, const std::string& name) const;
  const ResEntry* findIndex(ResType t, u16 oneBasedIndex) const;
  ResType typeAtIndex(u16 oneBasedIndex) const;
  u16 typeCount() const { return u16(order_.size()); }

  // Raw bytes of a resource, or nullptr when the entry is out of range.
  const u8* data(const ResEntry& e) const;

  bool loaded() const { return loaded_; }

 private:
  std::vector<u8> blob_;
  std::map<ResType, std::vector<ResEntry>> byType_;
  std::vector<ResType> order_;   // type-list order, for GetIndType
  bool loaded_ = false;
};

}  // namespace cyt
