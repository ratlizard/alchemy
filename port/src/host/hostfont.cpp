// Rasterising a bitmap strike on the host, through CoreText.
//
// Two of the five families Cythera asks for by name cannot come from its own
// resources, and one can only come from them by being rasterised:
//
//   * ArgosANouveau, the display face, ships as 'sfnt' 7289 with no bitmap
//     strikes at all -- its FOND lists a single association at size 0, which is
//     the classic way of saying "scalable, ask the outline". It is rasterised
//     here from the game's own bytes, so it is exact.
//   * Geneva lived in the System file. It is still installed on macOS, so it is
//     also exact, from the same outlines Apple has always shipped.
//   * Chicago and Espy Sans are gone: Chicago was retired after Mac OS 8 and
//     Espy Sans was never a shipping system font. Those two are substituted,
//     and the substitution is the port's one real text-fidelity compromise.
//
// Antialiasing and font smoothing are switched off, so what comes back is the
// one-bit-per-pixel strike the rest of the Font Manager works in and looks like
// what the game was drawn against, rather than a grey modern rendering.
#include "host/hostfont.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace cyt {
namespace {

// MacRoman to Unicode for the upper half. The lower half is ASCII. Cythera's
// text, its resource names and its TxSt style resources are all MacRoman -- the
// 'Omega' that names TxSt 999 is byte 0xBD -- so this is the mapping that makes
// a byte the game hands to DrawText select the right outline.
constexpr u16 kMacRomanHigh[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

u16 toUnicode(u8 ch) {
  return ch < 0x80 ? u16(ch) : kMacRomanHigh[ch - 0x80];
}

// A CFTypeRef that releases itself, so the many early exits below cannot leak.
template <typename T>
class Cf {
 public:
  Cf() = default;
  explicit Cf(T v) : v_(v) {}
  ~Cf() { if (v_) CFRelease(v_); }
  Cf(const Cf&) = delete;
  Cf& operator=(const Cf&) = delete;
  Cf(Cf&& o) : v_(o.v_) { o.v_ = nullptr; }
  T get() const { return v_; }
  explicit operator bool() const { return v_ != nullptr; }

 private:
  T v_ = nullptr;
};

// True only when the host really has this family. CTFontCreateWithName never
// fails -- asked for "Chicago" it quietly hands back Helvetica -- so asking it
// is useless as an availability test. Matching a descriptor with the family
// name marked *mandatory* is the test that actually answers the question.
bool familyInstalled(CFStringRef family) {
  const void* keys[1] = {kCTFontFamilyNameAttribute};
  const void* vals[1] = {family};
  Cf<CFDictionaryRef> attrs(CFDictionaryCreate(
      nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks));
  if (!attrs) return false;
  Cf<CTFontDescriptorRef> want(
      CTFontDescriptorCreateWithAttributes(attrs.get()));
  if (!want) return false;
  Cf<CFSetRef> mandatory(
      CFSetCreate(nullptr, keys, 1, &kCFTypeSetCallBacks));
  if (!mandatory) return false;
  Cf<CTFontDescriptorRef> got(CTFontDescriptorCreateMatchingFontDescriptor(
      want.get(), mandatory.get()));
  return bool(got);
}

// The hollow rectangle the Font Manager drew for a character the strike does
// not define. Built rather than looked up, because whether an outline has a
// .notdef glyph varies and this is what the classic one looked like.
Glyph buildMissingGlyph(int ascent) {
  Glyph g;
  const int h = std::max(4, ascent - ascent / 4);
  const int w = std::max(3, h / 2 + 1);
  g.allocate(s16(w), s16(h));
  for (int x = 0; x < w; ++x) { g.set(s16(x), 0); g.set(s16(x), s16(h - 1)); }
  for (int y = 0; y < h; ++y) { g.set(0, s16(y)); g.set(s16(w - 1), s16(y)); }
  g.left = 0;
  g.top = s16(-h);
  g.advance = s16(w + 1);
  g.present = true;
  return g;
}

// Draws one glyph with antialiasing off and trims the result to its ink, which
// is what gives `left`, `top` and the image size their values.
bool renderGlyph(CTFontRef font, CGGlyph glyph, int ascent, int descent,
                 Glyph* out) {
  CGSize advance{};
  CTFontGetAdvancesForGlyphs(font, kCTFontOrientationHorizontal, &glyph,
                             &advance, 1);
  CGRect ink{};
  CTFontGetBoundingRectsForGlyphs(font, kCTFontOrientationHorizontal, &glyph,
                                  &ink, 1);

  const int adv = int(std::lround(advance.width));
  // Padding absorbs negative left bearings, right overhang and the overshoot
  // hinting can add above the ascent line.
  const int pad = std::max(4, (ascent + descent) / 2);
  const int loX = int(std::floor(std::min(CGFloat(0), ink.origin.x)));
  const int hiX = int(std::ceil(std::max(CGFloat(adv),
                                         ink.origin.x + ink.size.width)));
  const int boxW = (hiX - loX) + 2 * pad;
  const int boxH = ascent + descent + 2 * pad;
  if (boxW <= 0 || boxH <= 0 || boxW > 4096 || boxH > 4096) return false;

  std::vector<u8> gray(size_t(boxW) * size_t(boxH), 0);
  Cf<CGColorSpaceRef> cs(CGColorSpaceCreateDeviceGray());
  if (!cs) return false;
  Cf<CGContextRef> ctx(CGBitmapContextCreate(
      gray.data(), size_t(boxW), size_t(boxH), 8, size_t(boxW), cs.get(),
      kCGImageAlphaNone));
  if (!ctx) return false;
  CGContextSetShouldAntialias(ctx.get(), false);
  CGContextSetAllowsAntialiasing(ctx.get(), false);
  CGContextSetShouldSmoothFonts(ctx.get(), false);
  CGContextSetAllowsFontSmoothing(ctx.get(), false);
  CGContextSetShouldSubpixelPositionFonts(ctx.get(), false);
  CGContextSetShouldSubpixelQuantizeFonts(ctx.get(), false);
  CGContextSetGrayFillColor(ctx.get(), 1.0, 1.0);

  // CoreGraphics puts the origin at the bottom left, so the pen sits `descent +
  // pad` up from the bottom of the box and buffer row 0 is the *top*.
  const int originX = pad - loX;
  CGPoint pen = CGPointMake(CGFloat(originX), CGFloat(descent + pad));
  CTFontDrawGlyphs(font, &glyph, &pen, 1, ctx.get());

  int minX = boxW, maxX = -1, minY = boxH, maxY = -1;
  for (int y = 0; y < boxH; ++y)
    for (int x = 0; x < boxW; ++x)
      if (gray[size_t(y) * size_t(boxW) + size_t(x)] >= 128) {
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
      }

  out->advance = s16(adv);
  out->present = true;
  if (maxX < 0) {                 // no ink: a space, which is a real glyph
    out->allocate(0, 0);
    out->left = 0;
    out->top = 0;
    return true;
  }
  const int w = maxX - minX + 1, h = maxY - minY + 1;
  out->allocate(s16(w), s16(h));
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x)
      if (gray[size_t(minY + y) * size_t(boxW) + size_t(minX + x)] >= 128)
        out->set(s16(x), s16(y));
  out->left = s16(minX - originX);
  // Rows above the baseline number `ascent + pad`, so ink starting at row minY
  // begins that many rows above it -- negative, as Glyph documents.
  out->top = s16(minY - (ascent + pad));
  return true;
}

bool buildStrike(CTFontRef font, Strike* out) {
  const int ascent = int(std::ceil(CTFontGetAscent(font)));
  const int descent = int(std::ceil(CTFontGetDescent(font)));
  if (ascent <= 0 || ascent > 512 || descent < 0 || descent > 512) return false;

  *out = Strike{};
  out->ascent = s16(ascent);
  out->descent = s16(descent);
  out->leading = s16(std::lround(CTFontGetLeading(font)));
  out->missing = buildMissingGlyph(ascent);

  int widMax = 0, defined = 0;
  for (int code = 0; code < 256; ++code) {
    // Control codes have no image; the game never asks a strike to draw them.
    if (code < 0x20 || code == 0x7F) continue;
    UniChar uc = toUnicode(u8(code));
    CGGlyph glyph = 0;
    if (!CTFontGetGlyphsForCharacters(font, &uc, &glyph, 1) || glyph == 0)
      continue;
    if (!renderGlyph(font, glyph, ascent, descent, &out->glyphs[size_t(code)]))
      continue;
    widMax = std::max(widMax, int(out->glyphs[size_t(code)].advance));
    ++defined;
  }
  out->widMax = s16(widMax);
  // A face that produced nothing usable is worse than no face at all: the
  // caller can then try the next source instead of drawing 200 empty boxes.
  return defined > 16;
}

}  // namespace

bool hostFontsAvailable() {
  // Geneva is the substitution target for every family the game names that no
  // longer exists, so its absence is what would leave text unrenderable.
  static const bool ok = familyInstalled(CFSTR("Geneva"));
  return ok;
}

bool rasterizeHostFamily(const std::string& family, s16 size, Strike* out) {
  if (size <= 0 || size > 256) return false;
  Cf<CFStringRef> name(CFStringCreateWithCString(nullptr, family.c_str(),
                                                 kCFStringEncodingUTF8));
  if (!name || !familyInstalled(name.get())) return false;
  Cf<CTFontRef> font(CTFontCreateWithName(name.get(), CGFloat(size), nullptr));
  if (!font) return false;
  return buildStrike(font.get(), out);
}

bool rasterizeHostSfnt(const std::vector<u8>& sfnt, s16 size, Strike* out) {
  if (sfnt.size() < 12 || size <= 0 || size > 256) return false;
  // CFData copies the bytes, so the strike does not depend on the caller
  // keeping the resource alive.
  Cf<CFDataRef> data(CFDataCreate(nullptr, sfnt.data(),
                                  CFIndex(sfnt.size())));
  if (!data) return false;
  Cf<CGDataProviderRef> provider(CGDataProviderCreateWithCFData(data.get()));
  if (!provider) return false;
  Cf<CGFontRef> cg(CGFontCreateWithDataProvider(provider.get()));
  if (!cg) return false;
  Cf<CTFontRef> font(
      CTFontCreateWithGraphicsFont(cg.get(), CGFloat(size), nullptr, nullptr));
  if (!font) return false;
  return buildStrike(font.get(), out);
}

}  // namespace cyt
