// The Font Manager: font families, bitmap strikes, and the glyph images that
// QuickDraw's text calls draw through.
//
// A "strike" is the classic Font Manager's unit -- one family at one size and
// one style, already rasterised. Three things can produce one here:
//
//   * an NFNT resource in the game's own forks (the Seldane runic face),
//   * the sfnt the game carries for its display face (ArgosANouveau), or
//   * a host-installed family, for the names that used to come from the System
//     file and no longer exist anywhere else (Geneva, Monaco).
//
// Whatever the source, the result is the same structure, so style synthesis,
// measurement and drawing are written once. That mirrors what the Font Manager
// itself did: it synthesised bold, italic, outline and the rest from a plain
// strike rather than storing a strike per style.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "mem.h"

namespace cyt {

class Toolbox;

// Style bits, as QuickDraw numbers them in a Style byte.
namespace face {
constexpr s16 kBold      = 0x01;
constexpr s16 kItalic    = 0x02;
constexpr s16 kUnderline = 0x04;
constexpr s16 kOutline   = 0x08;
constexpr s16 kShadow    = 0x10;
constexpr s16 kCondense  = 0x20;
constexpr s16 kExtend    = 0x40;
}  // namespace face

// One character's image, positioned relative to the pen and the baseline.
// `left` and `top` are offsets from the pen position, with `top` negative for
// the rows above the baseline, so a glyph is drawn at (pen.h + left,
// pen.v + top) and the pen then advances by `advance`.
struct Glyph {
  s16 left = 0, top = 0;
  s16 w = 0, h = 0;
  s16 advance = 0;
  bool present = false;
  std::vector<u8> bits;   // rowBytes() per row, most significant bit leftmost

  s32 rowBytes() const { return (w + 7) / 8; }
  bool on(s16 x, s16 y) const {
    if (x < 0 || x >= w || y < 0 || y >= h) return false;
    return (bits[size_t(y) * size_t(rowBytes()) + size_t(x >> 3)] >>
            (7 - (x & 7))) & 1;
  }
  void set(s16 x, s16 y) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    bits[size_t(y) * size_t(rowBytes()) + size_t(x >> 3)] |= u8(0x80 >> (x & 7));
  }
  void allocate(s16 width, s16 height) {
    w = width; h = height;
    bits.assign(size_t(rowBytes()) * size_t(height < 0 ? 0 : height), 0);
  }
};

// A font family at one size and style.
struct Strike {
  s16 ascent = 0, descent = 0, leading = 0, widMax = 0;
  std::array<Glyph, 256> glyphs;
  Glyph missing;          // drawn for codes the font does not define

  const Glyph& operator[](u8 ch) const {
    return glyphs[ch].present ? glyphs[ch] : missing;
  }
  s16 lineHeight() const { return s16(ascent + descent + leading); }
};

// Scans the open resource forks for FOND resources and builds the family
// registry. Safe to call more than once; later calls pick up forks opened
// since. Called for the application from InitFonts.
void initFontRegistry();

// The classic Font Manager's name/number mapping. Unknown names resolve to 0,
// the system font, which is what GetFNum reports.
s16 fontNumberForName(const std::string& name);
std::string fontNameForNumber(s16 num);

// The strike for a family, size and style, rasterising and caching on first
// use. Never null once any font at all is available; null only if no font
// could be produced, which the drawing code treats as "draw nothing".
const Strike* strikeFor(s16 family, s16 size, s16 face);

// Font number and size the system font resolves to, as the low-memory globals
// SysFontFam and SysFontSize report them.
s16 sysFontFamily();
s16 sysFontSize();
void setSysFontFamily(s16 v);
void setSysFontSize(s16 v);

void registerFontManager(Toolbox& tb);

// ---- text drawing, shared with the Dialog, Menu and List Managers ---------

// The text state a run is drawn with, read from the port QuickDraw is pointed
// at. Kept explicit so that managers which draw text on the application's
// behalf can measure with the same code that draws.
struct TextStyle {
  s16 family = 0, size = 12, faceBits = 0, mode = 1 /* srcOr */;
  s16 chExtra = 0;
  s32 spExtra = 0;       // Fixed, as SpaceExtra takes it
};
TextStyle currentTextStyle(Toolbox& tb);

// Width of a run in the given style, in pixels.
s32 measureText(const TextStyle& st, const u8* text, size_t len);

// Draws a run at the current pen position of the current port and advances the
// pen, exactly as DrawText does. Returns the width drawn.
s32 drawTextRun(Toolbox& tb, const u8* text, size_t len);

}  // namespace cyt
