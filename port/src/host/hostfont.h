// Rasterising a bitmap strike from an outline font on the host.
//
// Two of the three faces Cythera asks for by name cannot come from its own
// resources: Geneva lived in the System file, and the game's display face is a
// scalable sfnt with no bitmap strikes at all. Both are rasterised here, with
// antialiasing off, so the result is the one-bit-per-pixel strike the rest of
// the Font Manager works in and looks like what the game was drawn against.
#pragma once

#include <string>
#include <vector>

#include "mac/font_mgr.h"

namespace cyt {

// A family installed on the host, by its family name.
bool rasterizeHostFamily(const std::string& family, s16 size, Strike* out);

// A complete sfnt held in memory -- the game carries its display face as one.
bool rasterizeHostSfnt(const std::vector<u8>& sfnt, s16 size, Strike* out);

// True when the host can supply fonts at all; false makes the Font Manager
// fall back to scaling whatever bitmap strikes the game itself ships.
bool hostFontsAvailable();

}  // namespace cyt
