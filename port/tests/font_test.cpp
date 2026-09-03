// Proves the Font Manager produces real glyphs for every font Cythera asks for.
//
// The game itself cannot demonstrate this yet: it sits at its splash screen and
// will not draw a character until the menu bar and dialogs exist. So this walks
// the demand directly instead. Cythera's twenty 'TxSt' resources are its
// complete text-style vocabulary -- each is { size, face, font name } -- so
// resolving every one of them and rasterising the strike behind it exercises
// exactly the set of faces the game will ever use, and nothing speculative.
//
//   ./build/cyt_font_test <Cythera.rsrc> <Cythera Data.rsrc> [--art]
//
// Exits non-zero if any style resource fails to produce a usable strike, which
// makes it usable as a regression check from smoke.sh.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mac/font_mgr.h"
#include "mem.h"
#include "resfork.h"

namespace cyt {
bool openApplicationResources(const std::string& path, std::string* err);
s16 openResourceFile(const std::string& path, const std::string& name,
                     bool writable,
                     std::string* err);
}  // namespace cyt

using namespace cyt;

namespace {

std::string faceName(s16 f) {
  if (f == 0) return "plain";
  std::string s;
  const struct { s16 bit; const char* name; } kBits[] = {
      {face::kBold, "bold"},         {face::kItalic, "italic"},
      {face::kUnderline, "underline"}, {face::kOutline, "outline"},
      {face::kShadow, "shadow"},     {face::kCondense, "condense"},
      {face::kExtend, "extend"}};
  for (const auto& b : kBits)
    if (f & b.bit) { if (!s.empty()) s += "+"; s += b.name; }
  return s.empty() ? "plain" : s;
}

// Renders a run into a character grid, using the same glyph geometry the real
// blitter uses, so what prints here is what would reach the framebuffer.
void showRun(const Strike& s, const std::string& text) {
  const int top = -s.ascent, height = s.ascent + s.descent;
  int width = 2;
  for (unsigned char ch : text) width += s[ch].advance;
  if (width <= 0 || height <= 0 || width > 400) return;
  std::vector<std::string> rows(size_t(height), std::string(size_t(width), '.'));
  int pen = 1;
  for (unsigned char ch : text) {
    const Glyph& g = s[ch];
    for (s16 gy = 0; gy < g.h; ++gy)
      for (s16 gx = 0; gx < g.w; ++gx) {
        if (!g.on(gx, gy)) continue;
        const int x = pen + g.left + gx, y = (g.top + gy) - top;
        if (x >= 0 && x < width && y >= 0 && y < height) rows[size_t(y)][size_t(x)] = '#';
      }
    pen += g.advance;
  }
  for (int y = 0; y < height; ++y)
    std::printf("        |%s|%s\n", rows[size_t(y)].c_str(),
                y == s.ascent - 1 ? "  <- baseline" : "");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <Cythera.rsrc> <Cythera Data.rsrc> [--art]\n",
                 argv[0]);
    return 2;
  }
  const bool art = argc > 3 && std::strcmp(argv[3], "--art") == 0;

  std::string err;
  if (!Mem::get().init(&err)) {
    std::fprintf(stderr, "cannot create the guest address space: %s\n",
                 err.c_str());
    return 2;
  }
  if (!openApplicationResources(argv[1], &err)) {
    std::fprintf(stderr, "cannot open %s: %s\n", argv[1], err.c_str());
    return 2;
  }
  if (openResourceFile(argv[2], "Cythera Data", false, &err) < 0) {
    std::fprintf(stderr, "cannot open %s: %s\n", argv[2], err.c_str());
    return 2;
  }
  initFontRegistry();

  // Every text style the game carries, from both forks.
  std::vector<ResItem> styles;
  listResources(resType("TxSt"), &styles);
  if (styles.empty()) {
    std::fprintf(stderr, "no TxSt resources found\n");
    return 1;
  }

  int checked = 0, failed = 0;
  std::printf("%-16s %-5s %-22s %-7s %s\n", "family", "size", "face", "strike",
              "metrics (ascent/descent/leading/widMax), sample width");
  for (const ResItem& r : styles) {
    if (r.data.size() < 3) continue;
    const s16 size = s16(r.data[0]);
    const s16 fc = s16(r.data[1]);
    const size_t n = r.data[2];
    if (r.data.size() < 3 + n) continue;
    const std::string name(reinterpret_cast<const char*>(r.data.data() + 3), n);

    const s16 family = fontNumberForName(name);
    const Strike* s = strikeFor(family, size, fc);
    ++checked;
    if (!s) {
      std::printf("%-16s %-5d %-22s %-7s FAILED: no strike\n", name.c_str(),
                  size, faceName(fc).c_str(), "-");
      ++failed;
      continue;
    }
    // A strike that produced no drawable glyph at all is a failure too, and a
    // quieter one than no strike: it would silently draw nothing.
    int drawable = 0;
    for (int ch = 0x20; ch < 0x100; ++ch)
      if (s->glyphs[size_t(ch)].present) ++drawable;

    const std::string sample = "The quick brown fox";
    s32 w = 0;
    for (unsigned char ch : sample) w += (*s)[ch].advance;
    std::printf("%-16s %-5d %-22s fam=%-3d %d/%d/%d/%d, %d glyphs, "
                "sample=%dpx%s\n",
                name.c_str(), size, faceName(fc).c_str(), family, s->ascent,
                s->descent, s->leading, s->widMax, drawable, w,
                drawable == 0 ? "  FAILED: no glyphs" : "");
    if (drawable == 0) ++failed;

    if (art) {
      // Seldane defines only the twenty-six uppercase runes, so it is shown
      // with text it can actually set.
      const bool runic = drawable < 40;
      showRun(*s, runic ? "ABCDEFG" : "Hamburgefonstiv 123");
    }
  }

  std::printf("\n%d style resources checked, %d failed\n", checked, failed);
  return failed == 0 ? 0 : 1;
}
