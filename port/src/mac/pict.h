// QuickDraw picture playback.
//
// A PICT is a recorded stream of QuickDraw operations. Cythera uses it only for
// bitmap artwork -- title screen, portraits, banners -- so this plays the
// CopyBits family and skips every other opcode by its documented length. The
// opcode length table is what makes skipping safe, and it is the part that is
// tedious to get right; the decoding logic mirrors a Python reference that was
// verified against all 21 pictures in the game's two resource forks.
#pragma once

#include <vector>

#include "mac/qd_surface.h"
#include "mem.h"

namespace cyt {

// A decoded picture. Cythera's artwork is mostly indexed -- a palette plus one
// byte per pixel -- but its animations are not: the start screen's torch is six
// 32-bit direct-colour pictures. A direct picture has no palette to speak of,
// so its pixels are kept as true colour in `rgb` and matched to whatever colour
// table the screen has when it is finally drawn. Reducing them at decode time
// is what this port used to do, and it produced a green flame: the reduction
// was luminance, and a luminance used as a palette index lands on an unrelated
// colour.
struct DecodedPict {
  s16 width = 0, height = 0;
  bool direct = false;         // true: read `rgb`; false: read `pixels`
  std::vector<u8> pixels;      // width * height, row-major, indexed
  std::vector<Rgb> rgb;        // width * height, row-major, direct colour
  Palette palette;             // the picture's own colour table, if indexed
  bool ok = false;
  std::string error;
};

// Decodes the first bitmap in a PICT held in guest memory.
DecodedPict decodePict(Mem& m, GuestAddr data, u32 length);

}  // namespace cyt
