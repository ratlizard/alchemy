#include "pef.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cyt {
namespace {

// Big-endian readers over the container blob. The blob is a whole data fork, so
// every offset is bounds-checked against it rather than trusted.
struct Reader {
  const std::vector<u8>& b;
  bool ok = true;

  bool have(u64 off, u64 n) const { return off + n <= b.size(); }
  u8  u8At (u64 o) { if (!have(o,1)) { ok = false; return 0; } return b[o]; }
  u16 u16At(u64 o) {
    if (!have(o,2)) { ok = false; return 0; }
    return u16(b[o]) << 8 | b[o+1];
  }
  u32 u32At(u64 o) {
    if (!have(o,4)) { ok = false; return 0; }
    return u32(b[o]) << 24 | u32(b[o+1]) << 16 | u32(b[o+2]) << 8 | b[o+3];
  }
  std::string cstrAt(u64 o) {
    std::string s;
    while (have(o, 1) && b[o]) s.push_back(char(b[o++]));
    return s;
  }
};

constexpr u32 kContainerHeaderSize = 40;
constexpr u32 kSectionHeaderSize   = 28;
constexpr u32 kLoaderHeaderSize    = 56;
constexpr u32 kImportLibSize       = 24;
constexpr u32 kRelocHeaderSize     = 12;

// kPEFWeakImportLibMask in the imported library's options byte.
constexpr u8 kWeakLibMask = 0x40;
// Bit 7 of an imported symbol's class byte marks the symbol itself weak.
constexpr u8 kWeakSymMask = 0x80;

}  // namespace

bool PefImage::parseHeader(const std::vector<u8>& b, std::string* err) {
  Reader r{b};
  if (b.size() < kContainerHeaderSize ||
      r.u32At(0) != 0x4A6F7921 /* 'Joy!' */ ||
      r.u32At(4) != 0x70656666 /* 'peff' */) {
    if (err) *err = "not a PEF container (missing 'Joy!peff' tag)";
    return false;
  }
  arch_ = r.u32At(8);
  if (arch_ != 0x70777063 /* 'pwpc' */) {
    if (err) *err = "PEF container is not PowerPC ('pwpc')";
    return false;
  }
  if (r.u32At(12) != 1) {
    if (err) *err = "unsupported PEF format version";
    return false;
  }
  stamp_ = r.u32At(16);
  u32 nsec = r.u16At(32);

  sections_.clear();
  for (u32 i = 0; i < nsec; ++i) {
    u64 o = kContainerHeaderSize + u64(i) * kSectionHeaderSize;
    PefSection s;
    s.nameOffset      = s32(r.u32At(o + 0));
    s.defaultAddress  = r.u32At(o + 4);
    s.totalSize       = r.u32At(o + 8);
    s.unpackedSize    = r.u32At(o + 12);
    s.packedSize      = r.u32At(o + 16);
    s.containerOffset = r.u32At(o + 20);
    s.kind            = SectionKind(r.u8At(o + 24));
    s.shareKind       = r.u8At(o + 25);
    s.alignment       = r.u8At(o + 26);
    sections_.push_back(s);
  }
  if (!r.ok) {
    if (err) *err = "PEF section headers run past the end of the container";
    return false;
  }
  return true;
}

// Expands a pattern-initialised data section. The opcode byte carries a 3-bit
// kind and a 5-bit first argument; a zero first argument means the real value
// follows as a variable-length integer (7 bits per byte, high bit continues).
//
// The two interleaving opcodes are easy to get backwards: for RepeatBlock the
// common block is read from the container once, while for RepeatZero the common
// block is a run of zeroes that is *not* present in the container and it is the
// custom blocks that are read. Both emit repeatCount+1 common blocks separated
// by repeatCount custom blocks.
bool PefImage::expandPattern(const std::vector<u8>& b, const PefSection& s,
                             GuestAddr dst, std::string* err) {
  Mem& m = Mem::get();
  u64 p = s.containerOffset;
  const u64 end = u64(s.containerOffset) + s.packedSize;
  u64 out = 0;

  auto fail = [&](const char* what) {
    if (err) {
      std::ostringstream o;
      o << "pattern-initialised data: " << what << " at container offset " << p;
      *err = o.str();
    }
    return false;
  };
  auto varint = [&](u64* v) {
    *v = 0;
    for (int i = 0; i < 8; ++i) {
      if (p >= b.size()) return false;
      u8 byte = b[p++];
      *v = (*v << 7) | (byte & 0x7F);
      if (!(byte & 0x80)) return true;
    }
    return false;  // absurdly long encoding
  };
  auto zeros = [&](u64 n) {
    if (out + n > s.totalSize) return false;
    m.fill(dst + out, 0, n);
    out += n;
    return true;
  };
  auto copy = [&](u64 n) {
    if (p + n > b.size() || out + n > s.totalSize) return false;
    m.copyIn(dst + out, b.data() + p, n);
    p += n;
    out += n;
    return true;
  };

  while (p < end) {
    u8 op = b[p++];
    u64 arg = op & 0x1F;
    if (arg == 0 && !varint(&arg)) return fail("truncated argument");

    switch (op >> 5) {
      case 0:  // kPEFPkDataZero
        if (!zeros(arg)) return fail("zero run overflows the section");
        break;
      case 1:  // kPEFPkDataBlock
        if (!copy(arg)) return fail("block copy overflows the section");
        break;
      case 2: {  // kPEFPkDataRepeat -- one block, repeatCount+1 times
        u64 rpt = 0;
        if (!varint(&rpt)) return fail("truncated repeat count");
        if (p + arg > b.size()) return fail("repeat block past end");
        const u8* blk = b.data() + p;
        p += arg;
        for (u64 i = 0; i <= rpt; ++i) {
          if (out + arg > s.totalSize) return fail("repeat overflows section");
          m.copyIn(dst + out, blk, arg);
          out += arg;
        }
        break;
      }
      case 3: {  // kPEFPkDataRepeatBlock -- common from container, customs too
        u64 customSize = 0, rpt = 0;
        if (!varint(&customSize) || !varint(&rpt))
          return fail("truncated repeat-block header");
        if (p + arg > b.size()) return fail("common block past end");
        std::vector<u8> common(b.begin() + p, b.begin() + p + arg);
        p += arg;
        for (u64 i = 0; i < rpt; ++i) {
          if (out + arg > s.totalSize) return fail("overflow (common)");
          m.copyIn(dst + out, common.data(), arg);
          out += arg;
          if (!copy(customSize)) return fail("overflow (custom)");
        }
        if (out + arg > s.totalSize) return fail("overflow (final common)");
        m.copyIn(dst + out, common.data(), arg);
        out += arg;
        break;
      }
      case 4: {  // kPEFPkDataRepeatZero -- common is zeroes, customs read
        u64 customSize = 0, rpt = 0;
        if (!varint(&customSize) || !varint(&rpt))
          return fail("truncated repeat-zero header");
        for (u64 i = 0; i < rpt; ++i) {
          if (!zeros(arg)) return fail("overflow (zero common)");
          if (!copy(customSize)) return fail("overflow (custom)");
        }
        if (!zeros(arg)) return fail("overflow (final zero common)");
        break;
      }
      default:
        return fail("unknown pattern opcode");
    }
  }

  if (out != s.unpackedSize) {
    if (err) {
      std::ostringstream o;
      o << "pattern-initialised data expanded to " << out << " bytes but the "
        << "section header declares " << s.unpackedSize;
      *err = o.str();
    }
    return false;
  }
  // Everything past the initialised part is zero-filled by the loader.
  m.fill(dst + out, 0, s.totalSize - out);
  return true;
}

bool PefImage::instantiate(const std::vector<u8>& b, std::string* err) {
  Mem& m = Mem::get();
  GuestAddr nextData = layout::kDataBase;

  for (auto& s : sections_) {
    switch (s.kind) {
      case SectionKind::kCode:
        s.loadedAt = layout::kCodeBase;
        s.instantiated = true;
        if (s.packedSize > s.totalSize ||
            u64(s.containerOffset) + s.packedSize > b.size()) {
          if (err) *err = "code section extends past the end of the container";
          return false;
        }
        m.copyIn(s.loadedAt, b.data() + s.containerOffset, s.packedSize);
        m.fill(s.loadedAt + s.packedSize, 0, s.totalSize - s.packedSize);
        codeBase_ = s.loadedAt;
        codeSize_ = s.totalSize;
        break;

      case SectionKind::kUnpackedData:
      case SectionKind::kConstant:
      case SectionKind::kExecData:
        s.loadedAt = nextData;
        s.instantiated = true;
        if (u64(s.containerOffset) + s.packedSize > b.size()) {
          if (err) *err = "data section extends past the end of the container";
          return false;
        }
        m.copyIn(s.loadedAt, b.data() + s.containerOffset, s.packedSize);
        m.fill(s.loadedAt + s.packedSize, 0, s.totalSize - s.packedSize);
        if (!dataBase_) dataBase_ = s.loadedAt;
        nextData += (s.totalSize + 0xFFFF) & ~u32(0xFFFF);
        break;

      case SectionKind::kPatternData:
        s.loadedAt = nextData;
        s.instantiated = true;
        if (!expandPattern(b, s, s.loadedAt, err)) return false;
        if (!dataBase_) dataBase_ = s.loadedAt;
        nextData += (s.totalSize + 0xFFFF) & ~u32(0xFFFF);
        break;

      default:  // loader, debug, exception, traceback: not instantiated
        break;
    }
    if (nextData >= layout::kHeapBase) {
      if (err) *err = "PEF data sections do not fit below the guest heap";
      return false;
    }
  }
  if (!codeBase_) {
    if (err) *err = "PEF container has no code section";
    return false;
  }
  return true;
}

bool PefImage::parseLoader(const std::vector<u8>& b, std::string* err) {
  const PefSection* ld = nullptr;
  for (u32 i = 0; i < sections_.size(); ++i) {
    if (sections_[i].kind == SectionKind::kLoader) {
      ld = &sections_[i];
      loaderIndex_ = i;
    }
  }
  if (!ld) {
    if (err) *err = "PEF container has no loader section";
    return false;
  }
  loaderBase_ = ld->containerOffset;

  Reader r{b};
  const u64 base = loaderBase_;
  mainSection_             = s32(r.u32At(base + 0));
  mainOffset_              = r.u32At(base + 4);
  importedLibraryCount_    = r.u32At(base + 24);
  totalImportedSymbolCount_= r.u32At(base + 28);
  relocSectionCount_       = r.u32At(base + 32);
  relocInstrOffset_        = r.u32At(base + 36);
  loaderStringsOffset_     = r.u32At(base + 40);
  if (!r.ok) {
    if (err) *err = "PEF loader header is truncated";
    return false;
  }

  const u64 strs = base + loaderStringsOffset_;

  importLibs_.clear();
  for (u32 i = 0; i < importedLibraryCount_; ++i) {
    u64 o = base + kLoaderHeaderSize + u64(i) * kImportLibSize;
    PefImportLib L;
    L.name           = r.cstrAt(strs + r.u32At(o + 0));
    L.currentVersion = r.u32At(o + 8);
    L.symbolCount    = r.u32At(o + 12);
    L.firstSymbol    = r.u32At(o + 16);
    L.weak           = (r.u8At(o + 20) & kWeakLibMask) != 0;
    importLibs_.push_back(std::move(L));
  }

  const u64 symBase =
      base + kLoaderHeaderSize + u64(importedLibraryCount_) * kImportLibSize;
  importSyms_.assign(totalImportedSymbolCount_, PefImportSym{});
  for (u32 i = 0; i < totalImportedSymbolCount_; ++i) {
    u32 v = r.u32At(symBase + u64(i) * 4);
    u8 cls = u8(v >> 24);
    importSyms_[i].symClass = cls & 0x0F;
    importSyms_[i].weak     = (cls & kWeakSymMask) != 0;
    importSyms_[i].name     = r.cstrAt(strs + (v & 0x00FFFFFF));
  }
  // Attribute each symbol to its library so diagnostics can name both.
  for (const auto& L : importLibs_) {
    for (u32 i = 0; i < L.symbolCount; ++i) {
      u32 k = L.firstSymbol + i;
      if (k < importSyms_.size()) {
        importSyms_[k].library = L.name;
        if (L.weak) importSyms_[k].weak = true;
      }
    }
  }
  if (!r.ok) {
    if (err) *err = "PEF import tables are truncated";
    return false;
  }
  return true;
}

// Runs the loader section's relocation opcodes. Field widths here were verified
// against Cythera's own container: with skipCount as 8 bits and relocCount as
// 6 bits, every one of the 8466 relocations resolves into the code section, the
// data section, or the import area, and the entry transition vector's TOC lands
// exactly on the data section base. Note that relocation targets are not always
// four-byte aligned -- Metrowerks used 68k-compatible two-byte struct alignment,
// which PowerPC tolerates in hardware -- so the accessors must not assume it.
bool PefImage::relocate(const std::vector<u8>& b, std::string* err) {
  Mem& m = Mem::get();
  Reader r{b};

  auto sectionAddr = [&](u32 idx) -> GuestAddr {
    return idx < sections_.size() ? sections_[idx].loadedAt : 0;
  };

  // Each import resolves to a synthetic 8-byte transition vector in the shim
  // area: word 0 is a unique fake code address the interpreter recognises as a
  // native call, word 1 is the TOC to install (unused by native handlers).
  auto importAddr = [&](u32 idx) -> GuestAddr {
    return layout::kShimBase + idx * 8;
  };
  for (u32 i = 0; i < importSyms_.size(); ++i) {
    GuestAddr tv = importAddr(i);
    m.w32(tv + 0, layout::kShimBase + 0x80000 + i * 4);  // fake code address
    m.w32(tv + 4, 0);
    importSyms_[i].slot = tv;
  }

  const u64 relHdrs = loaderBase_ + kLoaderHeaderSize +
                      u64(importedLibraryCount_) * kImportLibSize +
                      u64(totalImportedSymbolCount_) * 4;

  for (u32 h = 0; h < relocSectionCount_; ++h) {
    u64 o = relHdrs + u64(h) * kRelocHeaderSize;
    u32 sidx  = r.u16At(o + 0);
    u32 count = r.u32At(o + 4);
    u32 first = r.u32At(o + 8);
    if (!r.ok || sidx >= sections_.size()) {
      if (err) *err = "PEF relocation header is invalid";
      return false;
    }

    const u64 streamBase = loaderBase_ + relocInstrOffset_ + first;
    std::vector<u16> words(count);
    for (u32 i = 0; i < count; ++i) words[i] = r.u16At(streamBase + u64(i) * 2);
    if (!r.ok) {
      if (err) *err = "PEF relocation stream is truncated";
      return false;
    }

    const GuestAddr secStart = sectionAddr(sidx);
    GuestAddr relocAddress = secStart;
    u32 importIndex = 0;
    GuestAddr sectionC = sectionAddr(0);
    GuestAddr sectionD = sections_.size() > 1 ? sectionAddr(1) : sectionAddr(0);

    auto add = [&](GuestAddr delta) {
      m.w32(relocAddress, m.r32(relocAddress) + delta);
      relocAddress += 4;
    };
    auto tvector = [&](u32 stride, u32 run) {
      for (u32 i = 0; i < run; ++i) {
        m.w32(relocAddress + 0, m.r32(relocAddress + 0) + sectionC);
        m.w32(relocAddress + 4, m.r32(relocAddress + 4) + sectionD);
        relocAddress += stride;
      }
    };

    for (u32 i = 0; i < words.size(); ++i) {
      const u16 w = words[i];
      const u32 op = w >> 9;
      const u32 run = (w & 0x1FF) + 1;

      if (op <= 0x1F) {                       // kPEFRelocBySectDWithSkip
        relocAddress += ((w >> 6) & 0xFF) * 4;
        for (u32 k = 0; k < (w & 0x3Fu); ++k) add(sectionD);
      } else if (op == 0x20) {                // kPEFRelocBySectC
        for (u32 k = 0; k < run; ++k) add(sectionC);
      } else if (op == 0x21) {                // kPEFRelocBySectD
        for (u32 k = 0; k < run; ++k) add(sectionD);
      } else if (op == 0x22) {                // kPEFRelocTVector12
        tvector(12, run);
      } else if (op == 0x23) {                // kPEFRelocTVector8
        tvector(8, run);
      } else if (op == 0x24) {                // kPEFRelocVTable8
        for (u32 k = 0; k < run; ++k) { add(sectionD); relocAddress += 4; }
      } else if (op == 0x25) {                // kPEFRelocImportRun
        for (u32 k = 0; k < run; ++k) add(importAddr(importIndex++));
      } else if (op == 0x30) {                // kPEFRelocSmByImport
        u32 idx = w & 0x1FF;
        add(importAddr(idx));
        importIndex = idx + 1;
      } else if (op == 0x31) {                // kPEFRelocSmSetSectC
        sectionC = sectionAddr(w & 0x1FF);
      } else if (op == 0x32) {                // kPEFRelocSmSetSectD
        sectionD = sectionAddr(w & 0x1FF);
      } else if (op == 0x33) {                // kPEFRelocSmBySection
        add(sectionAddr(w & 0x1FF));
      } else if (op >= 0x40 && op <= 0x47) {  // kPEFRelocIncrPosition
        relocAddress += (w & 0x0FFF) + 1;
      } else if (op >= 0x48 && op <= 0x4F) {  // kPEFRelocSmRepeat
        u32 blk = ((w >> 8) & 0xF) + 1;
        u32 rpt = (w & 0xFF) + 1;
        if (blk > i) {
          if (err) *err = "PEF SmRepeat refers before the start of the stream";
          return false;
        }
        std::vector<u16> seg(words.begin() + (i - blk), words.begin() + i);
        std::vector<u16> expanded;
        expanded.reserve(seg.size() * rpt);
        for (u32 k = 0; k < rpt; ++k)
          expanded.insert(expanded.end(), seg.begin(), seg.end());
        words.insert(words.begin() + i + 1, expanded.begin(), expanded.end());
      } else if (op == 0x50 || op == 0x51) {  // kPEFRelocSetPosition (2 words)
        if (i + 1 >= words.size()) break;
        u32 off = ((w & 0x3FFu) << 16) | words[++i];
        relocAddress = secStart + off;
      } else if (op == 0x52 || op == 0x53) {  // kPEFRelocLgByImport (2 words)
        if (i + 1 >= words.size()) break;
        u32 idx = ((w & 0x3FFu) << 16) | words[++i];
        add(importAddr(idx));
        importIndex = idx + 1;
      } else if (op >= 0x58 && op <= 0x5B) {  // kPEFRelocLgRepeat (2 words)
        if (i + 1 >= words.size()) break;
        u32 blk = ((w >> 6) & 0xF) + 1;
        u32 rpt = (((w & 0x3Fu) << 16) | words[++i]) + 1;
        if (blk > i) break;
        std::vector<u16> seg(words.begin() + (i - blk), words.begin() + i);
        std::vector<u16> expanded;
        for (u32 k = 0; k < rpt; ++k)
          expanded.insert(expanded.end(), seg.begin(), seg.end());
        words.insert(words.begin() + i + 1, expanded.begin(), expanded.end());
      } else if (op >= 0x5C && op <= 0x5F) {  // kPEFRelocLgSetOrBySection
        if (i + 1 >= words.size()) break;
        u32 sub = (w >> 6) & 0xF;
        u32 idx = ((w & 0x3Fu) << 16) | words[++i];
        if (sub == 0)      add(sectionAddr(idx));
        else if (sub == 1) sectionC = sectionAddr(idx);
        else if (sub == 2) sectionD = sectionAddr(idx);
      } else {
        if (err) {
          std::ostringstream s;
          s << "unhandled PEF relocation opcode 0x" << std::hex << op;
          *err = s.str();
        }
        return false;
      }
    }
  }

  // The entry point is a transition vector in a data section: {code, TOC}.
  if (mainSection_ >= 0 && u32(mainSection_) < sections_.size()) {
    entryTVector_ = sectionAddr(u32(mainSection_)) + mainOffset_;
    entryCode_ = m.r32(entryTVector_ + 0);
    entryToc_  = m.r32(entryTVector_ + 4);
    if (entryCode_ < codeBase_ || entryCode_ >= codeBase_ + codeSize_) {
      if (err) *err = "relocated entry point does not land in the code section";
      return false;
    }
  }
  return true;
}

bool PefImage::load(const std::vector<u8>& blob, std::string* err) {
  if (!parseHeader(blob, err)) return false;
  if (!parseLoader(blob, err)) return false;   // needs headers, not memory
  if (!instantiate(blob, err)) return false;
  if (!relocate(blob, err)) return false;
  return true;
}

// Metrowerks emits an XCOFF-style traceback table after each function: a zero
// word, then flags, then a length-prefixed name. Walking them recovers symbols
// straight from the binary, which is what produced cythera_symbols.txt.
void PefImage::symbolizeFromTracebacks() {
  Mem& m = Mem::get();
  syms_.clear();
  for (u32 off = 0; off + 8 <= codeSize_; off += 4) {
    if (m.r32(codeBase_ + off) != 0) continue;
    u32 flags = m.r32(codeBase_ + off + 4);
    // Byte 0 of the flags word is version 0; bit 7 of byte 1 marks "has name".
    if ((flags >> 24) != 0) continue;
    u32 tb = off + 8;
    if ((flags & 0x00800000u) == 0) continue;   // no name present
    // Skip the optional fixed fields the flags select.
    if (flags & 0x00000800u) tb += 4;           // has_tboff
    if (flags & 0x00000400u) tb += 4;           // int_hndl
    if (flags & 0x00000200u) tb += 4;           // has_ctl
    u32 nameLen = m.r16(codeBase_ + tb);
    if (nameLen == 0 || nameLen > 512) continue;
    std::string name;
    for (u32 i = 0; i < nameLen; ++i)
      name.push_back(char(m.r8(codeBase_ + tb + 2 + i)));
    syms_.push_back({codeBase_, 0, std::move(name)});
  }
  std::sort(syms_.begin(), syms_.end(),
            [](const Sym& a, const Sym& b) { return a.addr < b.addr; });
}

bool PefImage::loadSymbolFile(const std::string& path, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    if (err) *err = "cannot open symbol file: " + path;
    return false;
  }
  syms_.clear();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string addrTok;
    u32 bytes = 0, instrs = 0;
    std::string mangled;
    if (!(s >> addrTok >> bytes >> instrs >> mangled)) continue;
    if (addrTok.size() < 3 || addrTok[0] != '0' || addrTok[1] != 'x') continue;
    u32 off = u32(std::stoul(addrTok.substr(2), nullptr, 16));
    std::string demangled;
    std::getline(s, demangled);
    size_t b = demangled.find_first_not_of(" \t");
    syms_.push_back({codeBase_ + off, bytes,
                     b == std::string::npos ? mangled : demangled.substr(b)});
  }
  std::sort(syms_.begin(), syms_.end(),
            [](const Sym& a, const Sym& b) { return a.addr < b.addr; });
  return true;
}

std::string PefImage::symbolFor(GuestAddr addr) const {
  if (syms_.empty()) return {};
  // Last symbol whose address is <= addr.
  auto it = std::upper_bound(syms_.begin(), syms_.end(), addr,
                             [](GuestAddr a, const Sym& s) { return a < s.addr; });
  if (it == syms_.begin()) return {};
  --it;
  if (it->size && addr >= it->addr + it->size) return {};
  if (addr == it->addr) return it->name;
  std::ostringstream o;
  o << it->name << "+0x" << std::hex << (addr - it->addr);
  return o.str();
}

}  // namespace cyt
