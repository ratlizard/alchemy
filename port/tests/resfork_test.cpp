// Proves that a resource fork this port writes is one it can read back.
//
// The Resource Manager gained a writer so that preferences and saved games can
// be kept, and a resource fork is an easy format to get subtly wrong: two of
// its counts are stored as one less than the real count, reference-list offsets
// are measured from the type list rather than the map, and an empty fork is the
// case where both conventions bite at once. A round trip through the reader is
// the cheapest complete check of all of it.
//
//   ./build/cyt_resfork_test
//
// Exits non-zero on the first mismatch, which makes it usable from smoke.sh.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "resfork.h"

using namespace cyt;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

// Writes `items`, reads the result back, and compares every field that survives
// the trip. Ordering within a type must survive too, because GetIndResource
// hands resources out by position.
void roundTrip(const std::string& what, const std::vector<ResWrite>& items) {
  std::string err;
  ResourceFork fork;
  if (!fork.parse(buildResourceFork(items), &err)) {
    check(false, what + ": will not parse back (" + err + ")");
    return;
  }
  for (const ResWrite& w : items) {
    const ResEntry* e = fork.find(w.type, w.id);
    if (!e) {
      check(false, what + ": lost " + resTypeName(w.type) + " " +
                       std::to_string(w.id));
      return;
    }
    const u8* d = fork.data(*e);
    const bool same = e->name == w.name && e->attrs == w.attrs &&
                      e->length == w.data.size() &&
                      (w.data.empty() ||
                       (d && std::equal(w.data.begin(), w.data.end(), d)));
    if (!same) {
      check(false, what + ": " + resTypeName(w.type) + " " +
                       std::to_string(w.id) + " came back changed");
      return;
    }
    if (!w.name.empty() && fork.findNamed(w.type, w.name) != e) {
      check(false, what + ": " + w.name + " is not findable by name");
      return;
    }
  }
  check(true, what);
}

std::vector<u8> bytes(std::initializer_list<int> v) {
  std::vector<u8> out;
  for (int b : v) out.push_back(u8(b));
  return out;
}

}  // namespace

int main() {
  std::printf("resource fork writer:\n");

  // The case CreateResFile produces, and the one that overflowed the reader's
  // "count minus one" arithmetic before the writer existed to expose it.
  {
    std::string err;
    ResourceFork empty;
    check(empty.parse(buildResourceFork({}), &err) && empty.typeCount() == 0,
          "an empty fork parses back as empty");
  }

  roundTrip("one named resource",
            {{resType("Pref"), 128, "UI Prefs", 0, bytes({0xDB, 0xC8, 0, 0})}});

  roundTrip("several types, named and unnamed, with attributes",
            {{resType("Pref"), 128, "UI Prefs", 0, bytes({1, 2, 3, 4})},
             {resType("Pref"), 129, "Audio", 0x40, bytes({5})},
             {resType("Pref"), 200, "", 0, bytes({6, 7})},
             {resType("STR "), 1, "greeting", 0x20, bytes({5, 'h', 'e', 'l', 'l', 'o'})},
             {resType("DelP"), -1, "", 0, {}}});

  // A resource long enough to need more than one byte of its 24-bit offset, so
  // that the packed attribute-and-offset word is exercised properly.
  {
    std::vector<ResWrite> big;
    big.push_back({resType("BULK"), 1, "first", 0, std::vector<u8>(70000, 0xA5)});
    big.push_back({resType("BULK"), 2, "second", 0, bytes({0xFF})});
    roundTrip("a resource past the 64k offset boundary", big);
  }

  // Order within a type is positional, and Get1IndResource depends on it.
  {
    ResourceFork fork;
    std::string err;
    fork.parse(buildResourceFork({{resType("Pref"), 30, "", 0, bytes({3})},
                                  {resType("Pref"), 10, "", 0, bytes({1})},
                                  {resType("Pref"), 20, "", 0, bytes({2})}}),
               &err);
    const ResEntry* second = fork.findIndex(resType("Pref"), 2);
    check(second && second->id == 10,
          "resources keep the order they were written in");
  }

  std::printf("\n%s\n", failures == 0 ? "resource fork: all checks passed"
                                      : "resource fork: FAILED");
  return failures == 0 ? 0 : 1;
}
