// Loader for PEF (Preferred Executable Format) containers -- the format CFM
// used for PowerPC Mac executables and shared libraries.
//
// Loading a PEF means four things: copy or expand each instantiated section
// into the guest, run the loader section's relocation opcodes over the data
// sections, resolve every imported symbol to something callable, and hand back
// the entry point's transition vector. Cythera's own code needs no patching
// beyond that -- it is position-independent and reaches its data through r2.
#pragma once

#include <string>
#include <vector>

#include "mem.h"

namespace cyt {

enum class SectionKind : u8 {
  kCode = 0,           // executable, read-only, shared
  kUnpackedData = 1,   // read/write, verbatim in the container
  kPatternData = 2,    // read/write, run-length expanded at load time
  kConstant = 3,       // read-only data
  kLoader = 4,         // relocations, imports, exports (not instantiated)
  kDebug = 5,
  kExecData = 6,
  kException = 7,
  kTraceback = 8,
};

struct PefSection {
  int      nameOffset = -1;
  u32      defaultAddress = 0;
  u32      totalSize = 0;     // bytes in memory, including zero-filled tail
  u32      unpackedSize = 0;  // bytes of initialised data
  u32      packedSize = 0;    // bytes occupying the container
  u32      containerOffset = 0;
  SectionKind kind = SectionKind::kCode;
  u8       shareKind = 0;
  u8       alignment = 0;

  GuestAddr loadedAt = 0;     // filled in by PefImage::load
  bool      instantiated = false;
};

struct PefImportLib {
  std::string name;
  u32 currentVersion = 0;
  u32 symbolCount = 0;
  u32 firstSymbol = 0;
  bool weak = false;          // kPEFWeakImportLibMask: absence is not an error
};

struct PefImportSym {
  std::string name;
  u8 symClass = 0;            // 0 code, 1 data, 2 tvect, 3 toc, 4 glue
  bool weak = false;
  std::string library;
  // Where the loader wrote this symbol's resolved value. Relocation stores the
  // address of every import into the data section, so recording the slot lets
  // the shim layer report which import a guest call came through.
  GuestAddr slot = 0;
};

// A loaded PEF image plus everything needed to start and to symbolize it.
class PefImage {
 public:
  // Parses and loads `blob` (an entire data fork) into the guest.
  bool load(const std::vector<u8>& blob, std::string* err);

  // Reads the traceback table that Metrowerks emits after each function so
  // that addresses in traces carry names. Optional: a plain-text symbol file
  // may be supplied instead, which is what tooling has already produced.
  void symbolizeFromTracebacks();
  bool loadSymbolFile(const std::string& path, std::string* err);

  // Nearest preceding symbol for a guest code address, or "" when unknown.
  std::string symbolFor(GuestAddr addr) const;

  const std::vector<PefSection>&   sections() const { return sections_; }
  const std::vector<PefImportLib>& importLibs() const { return importLibs_; }
  const std::vector<PefImportSym>& importSyms() const { return importSyms_; }

  GuestAddr codeBase() const { return codeBase_; }
  u32       codeSize() const { return codeSize_; }
  GuestAddr dataBase() const { return dataBase_; }

  // The entry point, as CFM sees it: a transition vector holding the code
  // address and the TOC pointer to install in r2.
  GuestAddr entryTVector() const { return entryTVector_; }
  GuestAddr entryCode() const { return entryCode_; }
  GuestAddr entryToc() const { return entryToc_; }

  u32 architecture() const { return arch_; }
  u32 dateStamp() const { return stamp_; }

 private:
  bool parseHeader(const std::vector<u8>& b, std::string* err);
  bool instantiate(const std::vector<u8>& b, std::string* err);
  bool expandPattern(const std::vector<u8>& b, const PefSection& s,
                     GuestAddr dst, std::string* err);
  bool parseLoader(const std::vector<u8>& b, std::string* err);
  bool relocate(const std::vector<u8>& b, std::string* err);

  std::vector<PefSection>   sections_;
  std::vector<PefImportLib> importLibs_;
  std::vector<PefImportSym> importSyms_;

  u32 arch_ = 0, stamp_ = 0;
  u32 loaderIndex_ = 0;
  GuestAddr loaderBase_ = 0;   // container offset of the loader section

  // Loader section header fields that outlive parsing.
  s32 mainSection_ = -1;  u32 mainOffset_ = 0;
  u32 relocSectionCount_ = 0, relocInstrOffset_ = 0;
  u32 loaderStringsOffset_ = 0;
  u32 importedLibraryCount_ = 0, totalImportedSymbolCount_ = 0;

  GuestAddr codeBase_ = 0, dataBase_ = 0;
  u32 codeSize_ = 0;
  GuestAddr entryTVector_ = 0, entryCode_ = 0, entryToc_ = 0;

  struct Sym { GuestAddr addr; u32 size; std::string name; };
  std::vector<Sym> syms_;      // sorted by addr, for symbolFor()
};

}  // namespace cyt
