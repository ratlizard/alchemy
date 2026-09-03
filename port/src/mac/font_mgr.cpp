// The Font Manager and QuickDraw's text calls.
//
// Cythera names exactly five families across its twenty 'TxSt' style resources,
// and each one reaches a strike by a different route, which is why this is a
// registry rather than a single parser:
//
//   Seldane        FOND 128 in the data file -> NFNT 25740 (12pt), 25746 (18pt)
//                  Twenty-six uppercase runes and nothing else; a display face
//                  for inscriptions, asked for at 10, 12 and 18 point.
//   ArgosANouveau  FOND 1046 -> a single association at size 0, meaning
//                  "scalable": 'sfnt' 7289, rasterised by the host.
//   Geneva         no FOND at all. It lived in the System file, and macOS still
//                  installs it, so it comes from the host by name.
//   Chicago        likewise had no FOND, and no longer exists anywhere.
//   Espy Sans      never shipped as a system font at all.
//
// The last two are the port's one genuine text-fidelity compromise: they are
// substituted by Geneva, which is at least a real Apple screen face of the same
// era and the same design lineage. Everything else is exact, either from the
// game's own bitmaps or from the game's own outlines.
//
// Styles are synthesised from a plain strike rather than stored per style,
// which is what the Font Manager itself did -- bold by smearing, italic by
// shearing, outline and shadow by dilation.
#include "mac/font_mgr.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "host/hostfont.h"
#include "mac/heap.h"
#include "mac/qd_region.h"
#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {

bool peekResource(ResType type, s16 id, std::vector<u8>* out);

namespace {

bool debugFonts() {
  static const bool on = std::getenv("CYT_DEBUG_FONT") != nullptr;
  return on;
}

// ---------------------------------------------------------------------------
// NFNT: a rasterised strike in the game's own resources
// ---------------------------------------------------------------------------
//
// The layout is a fixed header, a single wide bit image holding every glyph
// side by side, a location table giving each glyph's left edge within that
// image, and an offset/width table giving each glyph's bearing and advance.
// Verified against both of Cythera's strikes: for NFNT 25740, owTLoc 344 puts
// the offset/width table at byte 704, and 100 entries end exactly at the
// resource's 904 bytes.
// (fontType at 0 and fRectWidth at 12 are not read: the depth bits of fontType
// are always 1-bit for these strikes, and the font rectangle's width is implied
// by the location table.)
namespace nfnt {
constexpr u32 kFirstChar  = 2;
constexpr u32 kLastChar   = 4;
constexpr u32 kWidMax     = 6;
constexpr u32 kKernMax    = 8;
constexpr u32 kNDescent   = 10;
constexpr u32 kFRectHeight = 14;
constexpr u32 kOwTLoc     = 16;
constexpr u32 kAscent     = 18;
constexpr u32 kDescent    = 20;
constexpr u32 kLeading    = 22;
constexpr u32 kRowWords   = 24;
constexpr u32 kBitImage   = 26;
}  // namespace nfnt

u16 be16(const std::vector<u8>& d, size_t off) {
  return off + 2 <= d.size() ? u16(u16(d[off]) << 8 | d[off + 1]) : 0;
}
s16 sbe16(const std::vector<u8>& d, size_t off) { return s16(be16(d, off)); }

bool parseNfnt(const std::vector<u8>& d, Strike* out) {
  if (d.size() < nfnt::kBitImage) return false;
  const s16 first = sbe16(d, nfnt::kFirstChar);
  const s16 last = sbe16(d, nfnt::kLastChar);
  const s16 kernMax = sbe16(d, nfnt::kKernMax);
  const s16 fRectHeight = sbe16(d, nfnt::kFRectHeight);
  const s16 ascent = sbe16(d, nfnt::kAscent);
  const s16 descent = sbe16(d, nfnt::kDescent);
  const s16 leading = sbe16(d, nfnt::kLeading);
  const u16 rowWords = be16(d, nfnt::kRowWords);
  const s16 nDescent = sbe16(d, nfnt::kNDescent);

  if (first < 0 || last < first || last > 255) return false;
  if (fRectHeight <= 0 || fRectHeight > 512 || rowWords == 0) return false;
  if (ascent <= 0 || ascent > 512 || descent < 0 || descent > 512) return false;

  // owTLoc is a word offset measured from the owTLoc field itself. A positive
  // nDescent is the high word of that offset, for strikes larger than 64K --
  // both of Cythera's are small and store -2 there, which is not a high word.
  u32 owTLoc = be16(d, nfnt::kOwTLoc);
  if (nDescent > 0) owTLoc |= u32(u16(nDescent)) << 16;
  const size_t rowBytes = size_t(rowWords) * 2;
  const size_t imageEnd = nfnt::kBitImage + rowBytes * size_t(fRectHeight);
  const size_t locOff = imageEnd;
  const size_t owOff = nfnt::kOwTLoc + size_t(owTLoc) * 2;
  if (imageEnd > d.size() || owOff >= d.size()) return false;

  // Glyph count is the character range plus the missing-character image that
  // follows it. The location table carries one extra entry, bounding the last
  // glyph's right edge; the offset/width table does not.
  const int nGlyphs = last - first + 2;
  auto loc = [&](int k) -> int {
    const size_t o = locOff + size_t(k) * 2;
    return o + 2 <= d.size() ? int(be16(d, o)) : -1;
  };
  auto ow = [&](int k) -> int {
    const size_t o = owOff + size_t(k) * 2;
    return o + 2 <= d.size() ? int(be16(d, o)) : 0xFFFF;
  };

  *out = Strike{};
  out->ascent = ascent;
  out->descent = descent;
  out->leading = leading;

  const int imageWidthBits = int(rowBytes) * 8;
  int widMax = 0;
  auto buildGlyph = [&](int k, Glyph* g) -> bool {
    const int entry = ow(k);
    if (entry == 0xFFFF) return false;      // this character is not defined
    const int bearing = entry >> 8, advance = entry & 0xFF;
    const int x0 = loc(k), x1 = loc(k + 1);
    if (x0 < 0 || x1 < x0 || x1 > imageWidthBits) return false;
    const int w = x1 - x0;
    g->allocate(s16(w), fRectHeight);
    for (int y = 0; y < fRectHeight; ++y)
      for (int x = 0; x < w; ++x) {
        const size_t byte = nfnt::kBitImage + size_t(y) * rowBytes +
                            size_t((x0 + x) >> 3);
        if (byte < d.size() && ((d[byte] >> (7 - ((x0 + x) & 7))) & 1))
          g->set(s16(x), s16(y));
      }
    // The bearing in the offset/width table is measured from kernMax, which is
    // the furthest any glyph in the strike reaches left of the pen.
    g->left = s16(bearing + kernMax);
    g->top = s16(-ascent);
    g->advance = s16(advance);
    g->present = true;
    widMax = std::max(widMax, advance);
    return true;
  };

  int defined = 0;
  for (int code = first; code <= last; ++code) {
    if (buildGlyph(code - first, &out->glyphs[size_t(code)])) ++defined;
  }
  // The image one past lastChar is what the Font Manager drew for any
  // character the strike does not define.
  Glyph missing;
  if (buildGlyph(nGlyphs - 1, &missing)) out->missing = missing;
  out->widMax = s16(widMax ? widMax : sbe16(d, nfnt::kWidMax));
  return defined > 0;
}

// ---------------------------------------------------------------------------
// FOND: the family record
// ---------------------------------------------------------------------------
namespace fond {
constexpr u32 kFamID     = 2;
constexpr u32 kAscent    = 8;
constexpr u32 kDescent   = 10;
constexpr u32 kLeading   = 12;
constexpr u32 kNumAssoc  = 52;   // count minus one
constexpr u32 kAssoc     = 54;   // { size, style, resource id } each
}  // namespace fond

struct AssocEntry {
  s16 size = 0;      // 0 means scalable: the id refers to an outline
  s16 style = 0;
  s16 resId = 0;
};

struct Family {
  s16 number = 0;
  std::string name;
  std::vector<AssocEntry> assoc;
  s16 sfntId = 0;              // outline strike source, 0 if none
  std::string hostFamily;      // host face to rasterise from, empty if none
  bool substituted = false;    // true when hostFamily is not this family
  // Family metrics in 1/4096 em, as the FOND stores them; 0 when absent.
  s16 emAscent = 0, emDescent = 0, emLeading = 0;
};

// Family numbers for the classic faces Cythera names but ships no FOND for.
// Chicago is family 0 because it *was* the system font, which is what TextFont(0)
// has to mean. Geneva and Monaco keep the numbers the System file gave them.
constexpr s16 kFamChicago = 0;
constexpr s16 kFamGeneva  = 3;
constexpr s16 kFamMonaco  = 4;
// Numbers handed to families that have neither a FOND nor a classic number.
constexpr s16 kPrivateFamilyBase = 20000;

std::map<s16, Family> g_families;
s16 g_nextPrivate = kPrivateFamilyBase;
s16 g_sysFontFamily = kFamChicago;
s16 g_sysFontSize = 12;
bool g_scanned = false;

std::string lowered(const std::string& s) {
  std::string r = s;
  for (char& c : r)
    if (c >= 'A' && c <= 'Z') c = char(c + 32);
  return r;
}

// The host face to render a family from when its own resources cannot. Only
// Geneva and Monaco still exist; everything else Cythera names is substituted
// by Geneva, deliberately and in one place so the compromise is visible.
struct HostMapping { const char* name; const char* host; bool exact; };
constexpr HostMapping kHostMappings[] = {
    {"geneva",    "Geneva", true},
    {"monaco",    "Monaco", true},
    {"chicago",   "Geneva", false},
    {"espy sans", "Geneva", false},
};

void applyHostMapping(Family* f) {
  const std::string key = lowered(f->name);
  for (const HostMapping& m : kHostMappings) {
    if (key == m.name) {
      f->hostFamily = m.host;
      f->substituted = !m.exact;
      return;
    }
  }
  // A family with no strike of its own and no known mapping still has to draw
  // something; Geneva is the fallback of last resort.
  if (f->assoc.empty() && f->sfntId == 0) {
    f->hostFamily = "Geneva";
    f->substituted = true;
  }
}

void registerFamily(Family f) {
  if (f.name.empty()) return;
  // A family already known by this name keeps its number, so that font numbers
  // the game has already been told stay valid across a rescan.
  for (auto& [num, existing] : g_families)
    if (lowered(existing.name) == lowered(f.name)) {
      if (existing.assoc.empty() && !f.assoc.empty()) {
        existing.assoc = f.assoc;
        existing.sfntId = f.sfntId;
        applyHostMapping(&existing);
      }
      return;
    }
  applyHostMapping(&f);
  g_families[f.number] = std::move(f);
}

// Scans every open fork for FOND resources. Idempotent, and cheap enough to
// repeat: Cythera has three FONDs in total, and the data file is opened after
// InitFonts runs, so a later call is how Seldane and ArgosANouveau appear.
void scanFonds() {
  std::vector<ResItem> fonds;
  listResources(resType("FOND"), &fonds);
  for (const ResItem& r : fonds) {
    if (r.name.empty()) continue;          // an unnamed family cannot be asked for
    const std::vector<u8>& d = r.data;
    if (d.size() < fond::kAssoc) continue;
    Family f;
    f.name = r.name;
    const s16 famId = sbe16(d, fond::kFamID);
    f.number = famId > 0 ? famId : r.id;
    f.emAscent = sbe16(d, fond::kAscent);
    f.emDescent = sbe16(d, fond::kDescent);
    f.emLeading = sbe16(d, fond::kLeading);
    const int count = sbe16(d, fond::kNumAssoc) + 1;
    for (int i = 0; i < count; ++i) {
      const size_t o = fond::kAssoc + size_t(i) * 6;
      if (o + 6 > d.size()) break;
      AssocEntry e{sbe16(d, o), sbe16(d, o + 2), sbe16(d, o + 4)};
      // A size of zero is the classic marker for "scalable": the association
      // points at an outline rather than at a bitmap strike.
      if (e.size == 0) {
        std::vector<u8> probe;
        if (peekResource(resType("sfnt"), e.resId, &probe)) f.sfntId = e.resId;
      } else {
        f.assoc.push_back(e);
      }
    }
    registerFamily(std::move(f));
  }

  // The classic faces with no FOND anywhere. Registered unconditionally so
  // that GetFNum can answer for them and TextFont(0) means the system font.
  for (const auto& [num, name] :
       {std::pair<s16, const char*>{kFamChicago, "Chicago"},
        std::pair<s16, const char*>{kFamGeneva, "Geneva"},
        std::pair<s16, const char*>{kFamMonaco, "Monaco"}}) {
    Family f;
    f.number = num;
    f.name = name;
    registerFamily(std::move(f));
  }
  for (const char* name : {"Espy Sans"}) {
    Family f;
    f.number = g_nextPrivate;
    f.name = name;
    const size_t before = g_families.size();
    registerFamily(std::move(f));
    if (g_families.size() != before) ++g_nextPrivate;
  }
  g_scanned = true;

  // Only report the registry when a rescan actually found something, since
  // every GetFNum triggers one.
  static size_t reported = 0;
  if (debugFonts() && g_families.size() != reported) {
    reported = g_families.size();
    std::fprintf(stderr, "  [font] registry: %zu families\n",
                 g_families.size());
    for (const auto& [num, f] : g_families) {
      std::fprintf(stderr, "  [font]   %5d '%s'", num, f.name.c_str());
      if (f.sfntId) std::fprintf(stderr, "  sfnt %d", f.sfntId);
      for (const AssocEntry& a : f.assoc)
        std::fprintf(stderr, "  NFNT %d@%dpt", a.resId, a.size);
      if (!f.hostFamily.empty())
        std::fprintf(stderr, "  host '%s'%s", f.hostFamily.c_str(),
                     f.substituted ? " (substituted)" : "");
      std::fprintf(stderr, "\n");
    }
  }
}

void ensureRegistry() {
  if (!g_scanned) scanFonds();
}

const Family* familyByNumber(s16 num) {
  ensureRegistry();
  auto it = g_families.find(num);
  if (it != g_families.end()) return &it->second;
  // An unknown number is the system font, which is what the Font Manager did
  // with a font it could not find.
  it = g_families.find(g_sysFontFamily);
  return it == g_families.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Style synthesis
// ---------------------------------------------------------------------------

// Bold: the glyph smeared one pixel to the right, which thickens every stem
// without disturbing the baseline or the left bearing.
void embolden(Strike* s) {
  auto fatten = [](Glyph* g) {
    if (!g->present || g->w <= 0 || g->h <= 0) { g->advance = s16(g->advance + 1); return; }
    Glyph out;
    out.allocate(s16(g->w + 1), g->h);
    for (s16 y = 0; y < g->h; ++y)
      for (s16 x = 0; x < g->w; ++x)
        if (g->on(x, y)) { out.set(x, y); out.set(s16(x + 1), y); }
    out.left = g->left;
    out.top = g->top;
    out.advance = s16(g->advance + 1);
    out.present = true;
    *g = out;
  };
  for (Glyph& g : s->glyphs) fatten(&g);
  fatten(&s->missing);
  s->widMax = s16(s->widMax + 1);
}

// Italic: a shear of one pixel every two rows, leaning right above the
// baseline and left below it, as the Font Manager's italic did.
void italicize(Strike* s) {
  auto shear = [](Glyph* g) {
    if (!g->present || g->w <= 0 || g->h <= 0) return;
    const int baselineRow = -g->top;   // first row at or below the baseline
    auto shiftAt = [&](int y) { return (baselineRow - y) / 2; };
    int lo = shiftAt(0), hi = lo;
    for (int y = 0; y < g->h; ++y) {
      lo = std::min(lo, shiftAt(y));
      hi = std::max(hi, shiftAt(y));
    }
    Glyph out;
    out.allocate(s16(g->w + (hi - lo)), g->h);
    for (s16 y = 0; y < g->h; ++y) {
      const int dx = shiftAt(y) - lo;
      for (s16 x = 0; x < g->w; ++x)
        if (g->on(x, y)) out.set(s16(x + dx), y);
    }
    out.left = s16(g->left + lo);
    out.top = g->top;
    out.advance = g->advance;
    out.present = true;
    *g = out;
  };
  for (Glyph& g : s->glyphs) shear(&g);
  shear(&s->missing);
}

// The one-pixel dilation both outline and shadow are built from.
Glyph dilate(const Glyph& g) {
  Glyph out;
  if (!g.present || g.w <= 0 || g.h <= 0) return out;
  out.allocate(s16(g.w + 2), s16(g.h + 2));
  for (s16 y = 0; y < g.h; ++y)
    for (s16 x = 0; x < g.w; ++x)
      if (g.on(x, y))
        for (s16 dy = 0; dy < 3; ++dy)
          for (s16 dx = 0; dx < 3; ++dx)
            out.set(s16(x + dx), s16(y + dy));
  out.left = s16(g.left - 1);
  out.top = s16(g.top - 1);
  out.advance = g.advance;
  out.present = true;
  return out;
}

// Outline: the dilation with the original knocked back out of it, leaving a
// hollow letter.
void outlineFace(Strike* s, bool withShadow) {
  auto hollow = [withShadow](Glyph* g) {
    if (!g->present || g->w <= 0 || g->h <= 0) { g->advance = s16(g->advance + 1); return; }
    Glyph d = dilate(*g);
    Glyph out;
    out.allocate(s16(d.w + (withShadow ? 1 : 0)),
                 s16(d.h + (withShadow ? 1 : 0)));
    for (s16 y = 0; y < d.h; ++y)
      for (s16 x = 0; x < d.w; ++x) {
        // Coordinates in the original glyph's space, since the dilation grew
        // it by one pixel on every side.
        const bool inner = g->on(s16(x - 1), s16(y - 1));
        if (d.on(x, y) && !inner) {
          out.set(x, y);
          if (withShadow) out.set(s16(x + 1), s16(y + 1));
        }
      }
    out.left = d.left;
    out.top = d.top;
    out.advance = s16(g->advance + (withShadow ? 2 : 1));
    out.present = true;
    *g = out;
  };
  for (Glyph& g : s->glyphs) hollow(&g);
  hollow(&s->missing);
  s->widMax = s16(s->widMax + (withShadow ? 2 : 1));
}

void adjustAdvances(Strike* s, int delta) {
  auto bump = [delta](Glyph* g) {
    g->advance = s16(std::max(0, int(g->advance) + delta));
  };
  for (Glyph& g : s->glyphs) bump(&g);
  bump(&s->missing);
  s->widMax = s16(std::max(0, int(s->widMax) + delta));
}

// Nearest-neighbour resize, for a size the family has no strike at. Cythera
// asks for Seldane at 10 point and ships it at 12 and 18, so this is the only
// way to answer without abandoning the game's own artwork for a substitute.
std::unique_ptr<Strike> scaleStrike(const Strike& src, s16 from, s16 to) {
  if (from <= 0 || to <= 0) return nullptr;
  const double k = double(to) / double(from);
  auto out = std::make_unique<Strike>();
  out->ascent = s16(std::lround(src.ascent * k));
  out->descent = s16(std::lround(src.descent * k));
  out->leading = s16(std::lround(src.leading * k));
  out->widMax = s16(std::lround(src.widMax * k));
  if (out->ascent <= 0) out->ascent = 1;

  auto resize = [k](const Glyph& g, Glyph* o) {
    *o = Glyph{};
    if (!g.present) return;
    o->present = true;
    o->advance = s16(std::lround(g.advance * k));
    o->left = s16(std::lround(g.left * k));
    o->top = s16(std::lround(g.top * k));
    const int w = int(std::lround(g.w * k)), h = int(std::lround(g.h * k));
    if (w <= 0 || h <= 0) { o->allocate(0, 0); return; }
    o->allocate(s16(w), s16(h));
    for (int y = 0; y < h; ++y) {
      const int sy = std::min(int(g.h) - 1, int(double(y) / k));
      for (int x = 0; x < w; ++x) {
        const int sx = std::min(int(g.w) - 1, int(double(x) / k));
        if (g.on(s16(sx), s16(sy))) o->set(s16(x), s16(y));
      }
    }
  };
  for (size_t i = 0; i < src.glyphs.size(); ++i)
    resize(src.glyphs[i], &out->glyphs[i]);
  resize(src.missing, &out->missing);
  return out;
}

// ---------------------------------------------------------------------------
// Strike production and caching
// ---------------------------------------------------------------------------

// A cached strike. A null pointer records that this combination was tried and
// cannot be produced, so a failure costs one rasterisation rather than one per
// character drawn.
std::map<u64, std::unique_ptr<Strike>> g_strikes;

u64 strikeKey(s16 family, s16 size, s16 face) {
  return u64(u16(family)) << 32 | u64(u16(size)) << 16 | u64(u16(face));
}

// The plain strike for a family at a size, from the best source available.
// Order matters: the game's own bitmaps first, then its own outline, then a
// resize of its own bitmaps, and only then the host. Substituting a host face
// when the game ships the artwork would throw away the real thing.
std::unique_ptr<Strike> makePlainStrike(const Family& f, s16 size) {
  auto out = std::make_unique<Strike>();

  for (const AssocEntry& a : f.assoc) {
    if (a.size != size) continue;
    std::vector<u8> d;
    if (peekResource(resType("NFNT"), a.resId, &d) && parseNfnt(d, out.get())) {
      if (debugFonts())
        std::fprintf(stderr, "  [font] '%s' %dpt from NFNT %d\n",
                     f.name.c_str(), size, a.resId);
      return out;
    }
  }

  if (f.sfntId) {
    std::vector<u8> d;
    if (peekResource(resType("sfnt"), f.sfntId, &d) &&
        rasterizeHostSfnt(d, size, out.get())) {
      if (debugFonts())
        std::fprintf(stderr, "  [font] '%s' %dpt from sfnt %d\n",
                     f.name.c_str(), size, f.sfntId);
      return out;
    }
  }

  // Nearest bitmap strike, resized. Preferring to scale down keeps stems from
  // being doubled, which is what the Font Manager preferred too.
  const AssocEntry* best = nullptr;
  for (const AssocEntry& a : f.assoc) {
    std::vector<u8> probe;
    if (!peekResource(resType("NFNT"), a.resId, &probe)) continue;
    if (!best || std::abs(a.size - size) < std::abs(best->size - size) ||
        (std::abs(a.size - size) == std::abs(best->size - size) &&
         a.size > best->size))
      best = &a;
  }
  if (best) {
    std::vector<u8> d;
    Strike base;
    if (peekResource(resType("NFNT"), best->resId, &d) &&
        parseNfnt(d, &base)) {
      if (auto scaled = scaleStrike(base, best->size, size)) {
        if (debugFonts())
          std::fprintf(stderr, "  [font] '%s' %dpt scaled from NFNT %d (%dpt)\n",
                       f.name.c_str(), size, best->resId, best->size);
        return scaled;
      }
    }
  }

  if (!f.hostFamily.empty() &&
      rasterizeHostFamily(f.hostFamily, size, out.get())) {
    if (debugFonts())
      std::fprintf(stderr, "  [font] '%s' %dpt from host '%s'%s\n",
                   f.name.c_str(), size, f.hostFamily.c_str(),
                   f.substituted ? " (substituted)" : "");
    return out;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public Font Manager interface
// ---------------------------------------------------------------------------

void initFontRegistry() { scanFonds(); }

s16 sysFontFamily() { return g_sysFontFamily; }
s16 sysFontSize() { return g_sysFontSize; }
void setSysFontFamily(s16 v) { g_sysFontFamily = v; }
void setSysFontSize(s16 v) { if (v > 0) g_sysFontSize = v; }

s16 fontNumberForName(const std::string& name) {
  // Rescan first: the data file, which carries three of the five families, is
  // opened after InitFonts runs.
  scanFonds();
  const std::string key = lowered(name);
  for (const auto& [num, f] : g_families)
    if (lowered(f.name) == key) return num;
  return 0;                                  // the system font, as GetFNum does
}

std::string fontNameForNumber(s16 num) {
  ensureRegistry();
  auto it = g_families.find(num);
  return it == g_families.end() ? std::string() : it->second.name;
}

const Strike* strikeFor(s16 family, s16 size, s16 face) {
  if (size <= 0) size = g_sysFontSize;
  if (size > 255) size = 255;
  face = s16(face & 0x7F);
  const u64 key = strikeKey(family, size, face);
  auto it = g_strikes.find(key);
  if (it != g_strikes.end()) return it->second.get();

  const Family* f = familyByNumber(family);
  if (!f) {
    g_strikes[key] = nullptr;
    return nullptr;
  }

  std::unique_ptr<Strike> s = makePlainStrike(*f, size);
  if (!s && !g_scanned) {                    // a rescan may have added it
    scanFonds();
    f = familyByNumber(family);
    if (f) s = makePlainStrike(*f, size);
  }
  if (!s) {
    if (debugFonts())
      std::fprintf(stderr, "  [font] no strike for family %d ('%s') at %dpt\n",
                   family, f ? f->name.c_str() : "?", size);
    g_strikes[key] = nullptr;
    return nullptr;
  }

  // Synthesised styles, in the order the Font Manager applied them: weight,
  // then slant, then the decorative faces, then the width adjustment.
  if (face & face::kBold) embolden(s.get());
  if (face & face::kItalic) italicize(s.get());
  if (face & face::kShadow) outlineFace(s.get(), true);
  else if (face & face::kOutline) outlineFace(s.get(), false);
  if (face & face::kCondense) adjustAdvances(s.get(), -1);
  if (face & face::kExtend) adjustAdvances(s.get(), 1);

  Strike* raw = s.get();
  g_strikes[key] = std::move(s);
  return raw;
}

// ---------------------------------------------------------------------------
// Text state, measurement and drawing
// ---------------------------------------------------------------------------

TextStyle currentTextStyle(Toolbox& tb) {
  TextStyle st;
  GuestAddr p = graphics().currentPort;
  if (!p) return st;
  Mem& m = tb.mem();
  st.family = s16(m.r16(p + qd::kPortTxFont));
  st.size = s16(m.r16(p + qd::kPortTxSize));
  // txFace is a one-byte Style followed by a pad byte.
  st.faceBits = s16(m.r8(p + qd::kPortTxFace));
  st.mode = s16(m.r16(p + qd::kPortTxMode));
  st.chExtra = s16(m.r16(p + qd::kPortChExtra));
  st.spExtra = s32(m.r32(p + qd::kPortSpExtra));
  if (st.size <= 0) st.size = g_sysFontSize;
  return st;
}

s32 measureText(const TextStyle& st, const u8* text, size_t len) {
  const Strike* s = strikeFor(st.family, st.size, st.faceBits);
  if (!s) return 0;
  const s32 space = st.spExtra >> 16;   // SpaceExtra takes a Fixed
  s32 w = 0;
  for (size_t i = 0; i < len; ++i) {
    w += s32((*s)[text[i]].advance) + st.chExtra;
    if (text[i] == ' ') w += space;
  }
  return w;
}

namespace {

// Blits one glyph, clipped, in QuickDraw's text transfer modes. The mask is the
// glyph; `fore` and `back` are already resolved to the surface's palette.
void blitGlyph(Mem& m, PixSurface& surf, const RectList& clip, const Glyph& g,
               s16 penH, s16 penV, u16 mode, u8 fore, u8 back) {
  if (g.w <= 0 || g.h <= 0) return;
  const Rect dst{s16(penV + g.top), s16(penH + g.left),
                 s16(penV + g.top + g.h), s16(penH + g.left + g.w)};
  const bool invert = (mode & 4) != 0;      // the notSrc* variants
  const u16 op = u16(mode & 3);
  const bool grayish = mode == 49;          // grayishTextOr, for dimmed text
  for (const Rect& c : clip) {
    const Rect piece = intersect(c, dst);
    if (piece.empty()) continue;
    for (s16 y = piece.top; y < piece.bottom; ++y)
      for (s16 x = piece.left; x < piece.right; ++x) {
        bool on = g.on(s16(x - dst.left), s16(y - dst.top));
        if (invert) on = !on;
        if (grayish && ((x + y) & 1)) continue;
        const GuestAddr a = surf.addr(x, y);
        switch (op) {
          case 0: m.w8(a, on ? fore : back); break;       // srcCopy
          case 1: if (on) m.w8(a, fore); break;           // srcOr
          case 2: if (on) m.w8(a, u8(m.r8(a) ^ fore)); break;  // srcXor
          case 3: if (on) m.w8(a, back); break;           // srcBic
          default: if (on) m.w8(a, fore); break;
        }
      }
  }
}

// srcCopy has to lay down the background across the whole cell the run
// occupies, not just where the glyphs have ink.
void eraseRun(Mem& m, PixSurface& surf, const RectList& clip,
              const Strike& strike, s16 penH, s16 penV, s32 width, u8 back) {
  const Rect cell{s16(penV - strike.ascent), penH,
                  s16(penV + strike.descent), s16(penH + width)};
  for (const Rect& c : clip) {
    const Rect piece = intersect(c, cell);
    if (piece.empty()) continue;
    for (s16 y = piece.top; y < piece.bottom; ++y)
      for (s16 x = piece.left; x < piece.right; ++x)
        m.w8(surf.addr(x, y), back);
  }
}

void drawUnderline(Mem& m, PixSurface& surf, const RectList& clip, s16 penH,
                   s16 penV, s32 width, u8 fore) {
  // Two pixels below the baseline. The Font Manager also broke the line around
  // descenders; this does not, which is visible only on descending letters.
  const Rect line{s16(penV + 2), penH, s16(penV + 3), s16(penH + width)};
  for (const Rect& c : clip) {
    const Rect piece = intersect(c, line);
    if (piece.empty()) continue;
    for (s16 y = piece.top; y < piece.bottom; ++y)
      for (s16 x = piece.left; x < piece.right; ++x)
        m.w8(surf.addr(x, y), fore);
  }
}

}  // namespace

s32 drawTextRun(Toolbox& tb, const u8* text, size_t len) {
  Mem& m = tb.mem();
  GuestAddr port = graphics().currentPort;
  const TextStyle st = currentTextStyle(tb);
  const Strike* strike = strikeFor(st.family, st.size, st.faceBits);
  if (!strike) return 0;

  s16 penV = 0, penH = 0;
  if (port) {
    penV = s16(m.r16(port + qd::kPortPnLoc + 0));
    penH = s16(m.r16(port + qd::kPortPnLoc + 2));
  }
  const s32 width = measureText(st, text, len);

  PixSurface surf = PixSurface::fromPort(m, port);
  if (surf.valid() && len > 0) {
    const RectList clip = portClipArea(tb, port, surf);
    const u8 fore = portForeIndex(tb, port, surf);
    const u8 back = portBackIndex(tb, port, surf);
    const u16 mode = u16(st.mode);
    if ((mode & 3) == 0) eraseRun(m, surf, clip, *strike, penH, penV, width, back);
    const s32 space = st.spExtra >> 16;
    s16 x = penH;
    for (size_t i = 0; i < len; ++i) {
      const Glyph& g = (*strike)[text[i]];
      // srcCopy already painted the background across the run.
      blitGlyph(m, surf, clip, g, x, penV, (mode & 3) == 0 ? u16(1) : mode,
                fore, back);
      x = s16(x + g.advance + st.chExtra + (text[i] == ' ' ? space : 0));
    }
    if (st.faceBits & face::kUnderline)
      drawUnderline(m, surf, clip, penH, penV, width, fore);
  }

  if (port) m.w16(port + qd::kPortPnLoc + 2, u16(s16(penH + width)));
  return width;
}

// ---------------------------------------------------------------------------
// Toolbox entry points
// ---------------------------------------------------------------------------

namespace {

// Reads a run of guest text into host memory. Text runs here are single lines
// of a dialog or a menu, so a bound keeps a bad length from allocating wildly.
std::vector<u8> readText(Mem& m, GuestAddr p, s32 len) {
  std::vector<u8> out;
  if (!p || len <= 0) return out;
  if (len > 32767) len = 32767;
  out.resize(size_t(len));
  m.copyOut(out.data(), p, u64(len));
  return out;
}

std::vector<u8> readPascal(Mem& m, GuestAddr p) {
  if (!p) return {};
  return readText(m, p + 1, s32(m.r8(p)));
}

// Wraps `text` to `width`, breaking at spaces where possible. Used by TETextBox
// and, through StyledLineBreak, by the application's own layout.
size_t breakLine(const TextStyle& st, const std::vector<u8>& text, size_t from,
                 s32 width, bool* brokeAtWord) {
  *brokeAtWord = false;
  const Strike* s = strikeFor(st.family, st.size, st.faceBits);
  if (!s) return text.size();
  const s32 space = st.spExtra >> 16;
  s32 w = 0;
  size_t lastSpace = 0;
  for (size_t i = from; i < text.size(); ++i) {
    if (text[i] == '\r' || text[i] == '\n') return i;
    w += s32((*s)[text[i]].advance) + st.chExtra;
    if (text[i] == ' ') { w += space; lastSpace = i; }
    if (w > width && i > from) {
      if (lastSpace > from) { *brokeAtWord = true; return lastSpace; }
      return i;
    }
  }
  return text.size();
}

}  // namespace

void registerFontManager(Toolbox& tb) {
  // ---- Font Manager -------------------------------------------------------
  tb.add("InitFonts", [](Toolbox& tb, PpcCpu& c, Args& a) {
    initFontRegistry();
  });
  tb.add("GetFNum", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const std::string name = tb.mem().pstr(a.ptr());
    GuestAddr out = a.ptr();
    const s16 num = fontNumberForName(name);
    if (debugFonts())
      std::fprintf(stderr, "  [font] GetFNum('%s') -> %d\n", name.c_str(), num);
    if (out) tb.mem().w16(out, u16(num));
  });
  tb.add("GetFontName", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 num = a.s16v();
    GuestAddr out = a.ptr();
    const std::string name = fontNameForNumber(num);
    if (!out) return;
    tb.mem().w8(out, u8(std::min<size_t>(name.size(), 255)));
    for (size_t i = 0; i < name.size() && i < 255; ++i)
      tb.mem().w8(out + 1 + u32(i), u8(name[i]));
  });
  tb.add("RealFont", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 num = a.s16v();
    const s16 size = a.s16v();
    // "Real" means a strike exists at exactly this size without scaling.
    const Family* f = familyByNumber(num);
    bool real = false;
    if (f) {
      if (f->sfntId) real = true;            // an outline is real at any size
      for (const AssocEntry& e : f->assoc)
        if (e.size == size) real = true;
      if (!f->hostFamily.empty()) real = true;
    }
    Toolbox::ret(c, real ? 1u : 0u);
  });
  // The low-memory globals that carry the system font, which the Font Manager
  // read on every call and some applications set directly.
  tb.add("LMGetSysFontFam", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(u16(sysFontFamily())));
  });
  tb.add("LMSetSysFontFam", [](Toolbox& tb, PpcCpu& c, Args& a) {
    setSysFontFamily(a.s16v());
  });
  tb.add("LMGetSysFontSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, u32(u16(sysFontSize())));
  });
  tb.add("LMSetSysFontSize", [](Toolbox& tb, PpcCpu& c, Args& a) {
    setSysFontSize(a.s16v());
  });

  // ---- port text state ----------------------------------------------------
  auto setTextField = [](u32 field, bool byteField) {
    return [field, byteField](Toolbox& tb, PpcCpu& c, Args& a) {
      const s16 v = a.s16v();
      GuestAddr p = graphics().currentPort;
      if (!p) return;
      if (byteField) tb.mem().w8(p + field, u8(v));
      else tb.mem().w16(p + field, u16(v));
    };
  };
  tb.add("TextFont", setTextField(qd::kPortTxFont, false));
  tb.add("TextSize", setTextField(qd::kPortTxSize, false));
  tb.add("TextMode", setTextField(qd::kPortTxMode, false));
  tb.add("TextFace", setTextField(qd::kPortTxFace, true));
  tb.add("CharExtra", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s32 fixed = a.s32v();
    GuestAddr p = graphics().currentPort;
    if (p) tb.mem().w16(p + qd::kPortChExtra, u16(s16(fixed >> 16)));
  });
  tb.add("SpaceExtra", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s32 fixed = a.s32v();
    GuestAddr p = graphics().currentPort;
    if (p) tb.mem().w32(p + qd::kPortSpExtra, u32(fixed));
  });

  // ---- metrics ------------------------------------------------------------
  tb.add("GetFontInfo", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr info = a.ptr();
    if (!info) return;
    const TextStyle st = currentTextStyle(tb);
    const Strike* s = strikeFor(st.family, st.size, st.faceBits);
    // FontInfo: ascent, descent, widMax, leading.
    if (!s) {
      tb.mem().w16(info + 0, u16(st.size * 4 / 5));
      tb.mem().w16(info + 2, u16(st.size / 5 + 1));
      tb.mem().w16(info + 4, u16(st.size));
      tb.mem().w16(info + 6, 1);
      return;
    }
    tb.mem().w16(info + 0, u16(s->ascent));
    tb.mem().w16(info + 2, u16(s->descent));
    tb.mem().w16(info + 4, u16(s->widMax));
    tb.mem().w16(info + 6, u16(s->leading));
  });
  tb.add("CharWidth", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u8 ch = u8(a.s16v());
    const TextStyle st = currentTextStyle(tb);
    Toolbox::ret(c, u32(measureText(st, &ch, 1)));
  });
  tb.add("StringWidth", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const std::vector<u8> s = readPascal(tb.mem(), a.ptr());
    const TextStyle st = currentTextStyle(tb);
    Toolbox::ret(c, u32(measureText(st, s.data(), s.size())));
  });
  tb.add("TextWidth", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr buf = a.ptr();
    const s16 first = a.s16v(), count = a.s16v();
    const std::vector<u8> s = readText(tb.mem(), buf + u32(u16(first)), count);
    const TextStyle st = currentTextStyle(tb);
    Toolbox::ret(c, u32(measureText(st, s.data(), s.size())));
  });
  tb.add("MeasureText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 count = a.s16v();
    GuestAddr buf = a.ptr();
    GuestAddr locs = a.ptr();
    const std::vector<u8> s = readText(tb.mem(), buf, count);
    const TextStyle st = currentTextStyle(tb);
    if (!locs) return;
    // charLocs[i] is the width of the first i characters, so it has count + 1
    // entries and starts at zero.
    for (s16 i = 0; i <= count; ++i)
      tb.mem().w16(locs + u32(u16(i)) * 2,
                   u16(measureText(st, s.data(),
                                   std::min(size_t(i), s.size()))));
  });
  tb.add("VisibleLength", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr buf = a.ptr();
    const s32 len = a.s32v();
    const std::vector<u8> s = readText(tb.mem(), buf, len);
    size_t n = s.size();
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
      --n;
    Toolbox::ret(c, u32(s32(n)));
  });
  tb.add("StyledLineBreak", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr buf = a.ptr();
    const s32 textLen = a.s32v();
    const s32 textStart = a.s32v();
    const s32 textEnd = a.s32v();
    a.s32v();                                // flags
    GuestAddr widthP = a.ptr();
    GuestAddr offsetP = a.ptr();
    const std::vector<u8> all = readText(tb.mem(), buf, textLen);
    const TextStyle st = currentTextStyle(tb);
    const s32 avail = widthP ? s32(tb.mem().r32(widthP)) >> 16 : 0;

    const size_t from = size_t(std::max<s32>(0, textStart));
    const size_t upto = size_t(std::clamp<s32>(textEnd, 0, s32(all.size())));
    std::vector<u8> run(all.begin() + std::min(from, all.size()),
                        all.begin() + std::max(from, upto));
    bool word = false;
    const size_t brk = breakLine(st, run, 0, avail, &word);
    // smBreakOverflow (0) means the whole range fitted; otherwise the break is
    // at a word (1) or, when a single word is too long, mid-word (2).
    u32 code = 0;
    size_t offset = run.size();
    if (brk < run.size()) {
      offset = brk;
      code = word ? 1u : 2u;
    }
    if (offsetP) tb.mem().w32(offsetP, u32(s32(from + offset)));
    if (widthP)
      tb.mem().w32(widthP,
                   u32(measureText(st, run.data(), offset) << 16));
    Toolbox::ret(c, code);
  });
  tb.add("TruncString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const s16 width = a.s16v();
    GuestAddr p = a.ptr();
    const s16 truncWhere = a.s16v();
    if (!p) { Toolbox::ret(c, u32(s32(-1))); return; }
    std::vector<u8> s = readPascal(tb.mem(), p);
    const TextStyle st = currentTextStyle(tb);
    if (measureText(st, s.data(), s.size()) <= width) {
      Toolbox::ret(c, 0);                    // nothing to do
      return;
    }
    const u8 kEllipsis = 0xC9;               // MacRoman horizontal ellipsis
    std::vector<u8> out;
    if (truncWhere == 0) {                   // truncEnd
      out = s;
      while (!out.empty()) {
        out.pop_back();
        std::vector<u8> t = out;
        t.push_back(kEllipsis);
        if (measureText(st, t.data(), t.size()) <= width) { out = t; break; }
      }
    } else {                                 // truncMiddle
      size_t head = s.size() / 2, tail = s.size() - head;
      while (head + tail > 0) {
        if (head >= tail && head > 0) --head; else if (tail > 0) --tail;
        std::vector<u8> t(s.begin(), s.begin() + std::ptrdiff_t(head));
        t.push_back(kEllipsis);
        t.insert(t.end(), s.end() - std::ptrdiff_t(tail), s.end());
        if (measureText(st, t.data(), t.size()) <= width) { out = t; break; }
      }
    }
    tb.mem().w8(p, u8(std::min<size_t>(out.size(), 255)));
    for (size_t i = 0; i < out.size() && i < 255; ++i)
      tb.mem().w8(p + 1 + u32(i), out[i]);
    Toolbox::ret(c, 1);                      // truncated
  });

  // ---- drawing ------------------------------------------------------------
  tb.add("DrawChar", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const u8 ch = u8(a.s16v());
    drawTextRun(tb, &ch, 1);
  });
  tb.add("DrawString", [](Toolbox& tb, PpcCpu& c, Args& a) {
    const std::vector<u8> s = readPascal(tb.mem(), a.ptr());
    drawTextRun(tb, s.data(), s.size());
  });
  tb.add("DrawText", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr buf = a.ptr();
    const s16 first = a.s16v(), count = a.s16v();
    const std::vector<u8> s = readText(tb.mem(), buf + u32(u16(first)), count);
    drawTextRun(tb, s.data(), s.size());
  });
  // TextBox and TETextBox are the same call: wrap a run inside a rectangle,
  // erasing it first, which is how dialogs draw their static text.
  auto textBox = [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr buf = a.ptr();
    const s32 len = a.s32v();
    GuestAddr boxP = a.ptr();
    const s16 just = a.s16v();
    if (!boxP) return;
    const std::vector<u8> text = readText(tb.mem(), buf, len);
    const Rect box = readRect(tb.mem(), boxP);
    const TextStyle st = currentTextStyle(tb);
    const Strike* strike = strikeFor(st.family, st.size, st.faceBits);
    if (!strike) return;

    Mem& m = tb.mem();
    GuestAddr port = graphics().currentPort;
    PixSurface surf = PixSurface::fromPort(m, port);
    if (surf.valid()) {
      const RectList clip = portClipArea(tb, port, surf);
      const u8 back = portBackIndex(tb, port, surf);
      for (const Rect& cr : clip) {
        const Rect piece = intersect(cr, box);
        if (piece.empty()) continue;
        for (s16 y = piece.top; y < piece.bottom; ++y)
          for (s16 x = piece.left; x < piece.right; ++x)
            m.w8(surf.addr(x, y), back);
      }
    }

    const s16 lineHeight = strike->lineHeight() > 0 ? strike->lineHeight()
                                                    : s16(st.size + 2);
    s16 baseline = s16(box.top + strike->ascent);
    size_t at = 0;
    while (at <= text.size() && baseline - strike->ascent < box.bottom) {
      bool word = false;
      const size_t end = breakLine(st, text, at, box.width(), &word);
      const size_t n = end > at ? end - at : 0;
      const s32 w = measureText(st, text.data() + at, n);
      s16 x = box.left;
      // teFlushRight is -1 and teCenter is 1; anything else is flush left.
      if (just == 1) x = s16(box.left + (box.width() - w) / 2);
      else if (just == -1) x = s16(box.right - w);
      if (port) {
        m.w16(port + qd::kPortPnLoc + 0, u16(baseline));
        m.w16(port + qd::kPortPnLoc + 2, u16(x));
      }
      if (n) drawTextRun(tb, text.data() + at, n);
      // Step past the break, and past the line ending or space it fell on.
      at = end;
      while (at < text.size() &&
             (text[at] == ' ' || text[at] == '\r' || text[at] == '\n'))
        ++at;
      if (end >= text.size()) break;
      baseline = s16(baseline + lineHeight);
    }
  };
  tb.add("TextBox", textBox);
  tb.add("TETextBox", textBox);
}

}  // namespace cyt
