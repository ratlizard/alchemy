// The screen's gamma ramp, and the sliver of video driver that sets it.
//
// Cythera fades between its Ambrosia logo and its Delver splash by ramping the
// display's gamma, not by redrawing anything. Everything the host needs from
// that is here: three 256-entry tables that the framebuffer's colours are
// passed through on the way to the window.
#pragma once

#include "mem.h"

namespace cyt {

// One entry per input level, per channel, in host 0..255 terms. Identity until
// the application sets something else.
struct GammaRamp {
  u8 red[256];
  u8 green[256];
  u8 blue[256];
  bool identity = true;      // lets the presenter skip the lookup entirely
};

const GammaRamp& gammaRamp();

}  // namespace cyt
