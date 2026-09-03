#include "host/display.h"

#include <SDL.h>

#include <cstdio>
#include <vector>

#include "mac/qd_surface.h"
#include "mac/quickdraw.h"
#include "mac/video_driver.h"

namespace cyt {
namespace {

// Mac virtual key codes for the keys a game needs. Cythera is driven by the
// arrow keys, Return, Escape and letter shortcuts.
u8 macKeyCode(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_A: return 0x00;
    case SDL_SCANCODE_S: return 0x01;
    case SDL_SCANCODE_D: return 0x02;
    case SDL_SCANCODE_F: return 0x03;
    case SDL_SCANCODE_H: return 0x04;
    case SDL_SCANCODE_G: return 0x05;
    case SDL_SCANCODE_Z: return 0x06;
    case SDL_SCANCODE_X: return 0x07;
    case SDL_SCANCODE_C: return 0x08;
    case SDL_SCANCODE_V: return 0x09;
    case SDL_SCANCODE_B: return 0x0B;
    case SDL_SCANCODE_Q: return 0x0C;
    case SDL_SCANCODE_W: return 0x0D;
    case SDL_SCANCODE_E: return 0x0E;
    case SDL_SCANCODE_R: return 0x0F;
    case SDL_SCANCODE_Y: return 0x10;
    case SDL_SCANCODE_T: return 0x11;
    case SDL_SCANCODE_1: return 0x12;
    case SDL_SCANCODE_2: return 0x13;
    case SDL_SCANCODE_3: return 0x14;
    case SDL_SCANCODE_4: return 0x15;
    case SDL_SCANCODE_6: return 0x16;
    case SDL_SCANCODE_5: return 0x17;
    case SDL_SCANCODE_9: return 0x19;
    case SDL_SCANCODE_7: return 0x1A;
    case SDL_SCANCODE_8: return 0x1C;
    case SDL_SCANCODE_0: return 0x1D;
    case SDL_SCANCODE_O: return 0x1F;
    case SDL_SCANCODE_U: return 0x20;
    case SDL_SCANCODE_I: return 0x22;
    case SDL_SCANCODE_P: return 0x23;
    case SDL_SCANCODE_RETURN: return 0x24;
    case SDL_SCANCODE_L: return 0x25;
    case SDL_SCANCODE_J: return 0x26;
    case SDL_SCANCODE_K: return 0x28;
    case SDL_SCANCODE_N: return 0x2D;
    case SDL_SCANCODE_M: return 0x2E;
    case SDL_SCANCODE_TAB: return 0x30;
    case SDL_SCANCODE_SPACE: return 0x31;
    case SDL_SCANCODE_BACKSPACE: return 0x33;
    case SDL_SCANCODE_ESCAPE: return 0x35;
    case SDL_SCANCODE_LEFT: return 0x7B;
    case SDL_SCANCODE_RIGHT: return 0x7C;
    case SDL_SCANCODE_DOWN: return 0x7D;
    case SDL_SCANCODE_UP: return 0x7E;
    default: return 0xFF;
  }
}

// Mac Roman character for a key press, which is what the low byte of a keyDown
// message carries.
u8 macChar(SDL_Keycode key, bool shift) {
  if (key >= SDLK_a && key <= SDLK_z)
    return u8((shift ? 'A' : 'a') + (key - SDLK_a));
  if (key >= SDLK_0 && key <= SDLK_9) {
    static const char* shifted = ")!@#$%^&*(";
    return u8(shift ? shifted[key - SDLK_0] : char('0' + (key - SDLK_0)));
  }
  switch (key) {
    case SDLK_RETURN: return 13;
    case SDLK_TAB: return 9;
    case SDLK_SPACE: return 32;
    case SDLK_BACKSPACE: return 8;
    case SDLK_ESCAPE: return 27;
    case SDLK_LEFT: return 28;
    case SDLK_RIGHT: return 29;
    case SDLK_UP: return 30;
    case SDLK_DOWN: return 31;
    case SDLK_PERIOD: return shift ? '>' : '.';
    case SDLK_COMMA: return shift ? '<' : ',';
    case SDLK_SLASH: return shift ? '?' : '/';
    case SDLK_MINUS: return shift ? '_' : '-';
    case SDLK_EQUALS: return shift ? '+' : '=';
    default: return 0;
  }
}

}  // namespace

Display& Display::get() {
  static Display instance;
  return instance;
}

bool Display::open(int scale, std::string* err) {
  if (window_) return true;
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    if (err) *err = std::string("SDL_Init failed: ") + SDL_GetError();
    return false;
  }
  if (scale < 1) scale = 1;
  SDL_Window* w = SDL_CreateWindow(
      "Cythera", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      kScreenWidth * scale, kScreenHeight * scale,
      SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
  if (!w) {
    if (err) *err = std::string("could not create a window: ") + SDL_GetError();
    return false;
  }
  SDL_Renderer* r = SDL_CreateRenderer(w, -1, SDL_RENDERER_ACCELERATED |
                                                  SDL_RENDERER_PRESENTVSYNC);
  if (!r) r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
  if (!r) {
    if (err) *err = std::string("could not create a renderer: ") + SDL_GetError();
    return false;
  }
  // Nearest-neighbour keeps the 1999 pixel art crisp, and a logical size lets
  // the window be resized without the game knowing.
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_RenderSetLogicalSize(r, kScreenWidth, kScreenHeight);
  SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                     SDL_TEXTUREACCESS_STREAMING,
                                     kScreenWidth, kScreenHeight);
  if (!t) {
    if (err) *err = std::string("could not create a texture: ") + SDL_GetError();
    return false;
  }
  window_ = w;
  renderer_ = r;
  texture_ = t;
  return true;
}

void Display::close() {
  if (texture_) SDL_DestroyTexture(static_cast<SDL_Texture*>(texture_));
  if (renderer_) SDL_DestroyRenderer(static_cast<SDL_Renderer*>(renderer_));
  if (window_) SDL_DestroyWindow(static_cast<SDL_Window*>(window_));
  texture_ = renderer_ = window_ = nullptr;
  SDL_Quit();
}

void Display::present() {
  const GraphicsWorld& gw = graphics();
  if (!gw.screenBits || !texture_) return;

  // Cap presentation at about sixty frames a second; the game calls
  // WaitNextEvent far more often than that.
  const u32 now = SDL_GetTicks();
  if (now - lastPresent_ < 16) return;
  lastPresent_ = now;

  Mem& m = Mem::get();
  Palette pal = readPalette(m, gw.colorTableH);
  // The screen's gamma ramp sits between the colour table and the window. It is
  // the identity until the game fades, and skipped entirely while it is.
  const GammaRamp& g = gammaRamp();
  u32 lut[256];
  for (u32 i = 0; i < 256; ++i) {
    Rgb e = i < pal.entries.size() ? pal.entries[i] : Rgb{};
    u32 r8 = e.r >> 8, g8 = e.g >> 8, b8 = e.b >> 8;
    if (!g.identity) {
      r8 = g.red[r8];
      g8 = g.green[g8];
      b8 = g.blue[b8];
    }
    lut[i] = 0xFF000000u | (r8 << 16) | (g8 << 8) | b8;
  }

  void* pixels = nullptr;
  int pitch = 0;
  SDL_Texture* tex = static_cast<SDL_Texture*>(texture_);
  if (SDL_LockTexture(tex, nullptr, &pixels, &pitch) != 0) return;
  const u8* src = static_cast<const u8*>(m.host(gw.screenBits,
                                                u64(gw.rowBytes) * kScreenHeight));
  if (src) {
    for (int y = 0; y < kScreenHeight; ++y) {
      u32* dst = reinterpret_cast<u32*>(static_cast<u8*>(pixels) + y * pitch);
      const u8* row = src + size_t(y) * gw.rowBytes;
      for (int x = 0; x < kScreenWidth; ++x) dst[x] = lut[row[x]];
    }
  }
  SDL_UnlockTexture(tex);

  SDL_Renderer* r = static_cast<SDL_Renderer*>(renderer_);
  SDL_RenderClear(r);
  SDL_RenderCopy(r, tex, nullptr, nullptr);
  SDL_RenderPresent(r);
}

u16 Display::modifiers() const {
  u16 mods = mouseDown_ ? 0 : kBtnState;
  const SDL_Keymod k = SDL_GetModState();
  if (k & KMOD_SHIFT) mods |= kShiftKey;
  if (k & KMOD_ALT) mods |= kOptionKey;
  if (k & KMOD_CTRL) mods |= kControlKey;
  // The command key is what a Mac application checks for menu shortcuts; map
  // both the host's command and control keys onto it.
  if (k & KMOD_GUI) mods |= kCmdKey;
  if (k & KMOD_CAPS) mods |= kAlphaLock;
  return mods;
}

void Display::keyMap(u8 out[16]) const {
  for (int i = 0; i < 16; ++i) out[i] = keys_[i];
}

void Display::push(const MacEvent& e) {
  // A queue this small cannot meaningfully overflow, but a runaway producer
  // should not grow without bound either.
  if (queue_.size() > 128) queue_.pop_front();
  queue_.push_back(e);
}

// Turns a mouse position from an SDL event into the game's 640x480 space.
//
// This is where a Retina display bites. The window is created with
// SDL_WINDOW_ALLOW_HIGHDPI, so the renderer's output is measured in backing
// *pixels* -- 2560x1920 for a 1280x960 window on a 2x display -- while SDL
// reports mouse positions in window *points*. SDL_RenderWindowToLogical scales
// by the output size, so handing it point coordinates divides by twice what it
// should: a click meant for the Quit sign at (510,268) arrives as (255,134),
// which is over the signboard and over nothing in particular. The cursor
// position is wrong by the same factor, which is why nothing highlighted under
// the pointer either.
//
// Scaling points up to backing pixels first is the standard correction, and it
// is the identity on a non-Retina display.
void Display::eventToLogical(int px, int py, s16* outH, s16* outV) const {
  SDL_Window* w = static_cast<SDL_Window*>(window_);
  SDL_Renderer* r = static_cast<SDL_Renderer*>(renderer_);
  int winW = 0, winH = 0, outW = 0, outH2 = 0;
  SDL_GetWindowSize(w, &winW, &winH);
  SDL_GetRendererOutputSize(r, &outW, &outH2);
  const float sx = winW > 0 ? float(outW) / float(winW) : 1.0f;
  const float sy = winH > 0 ? float(outH2) / float(winH) : 1.0f;
  float lx = 0, ly = 0;
  SDL_RenderWindowToLogical(r, float(px) * sx, float(py) * sy, &lx, &ly);
  *outH = s16(lx);
  *outV = s16(ly);
}

void Display::handleInput() {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    MacEvent e;
    e.when = SDL_GetTicks() * 60 / 1000;
    e.whereH = mouseH_;
    e.whereV = mouseV_;
    e.modifiers = modifiers();

    switch (ev.type) {
      case SDL_QUIT:
        quit_ = true;
        break;
      case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
          e.what = kUpdateEvt;
          e.message = 0;
          push(e);
        }
        break;
      case SDL_MOUSEMOTION:
        eventToLogical(ev.motion.x, ev.motion.y, &mouseH_, &mouseV_);
        break;
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP: {
        eventToLogical(ev.button.x, ev.button.y, &mouseH_, &mouseV_);
        if (ev.button.button != SDL_BUTTON_LEFT) break;
        mouseDown_ = (ev.type == SDL_MOUSEBUTTONDOWN);
        e.what = mouseDown_ ? kMouseDown : kMouseUp;
        e.whereH = mouseH_;
        e.whereV = mouseV_;
        e.modifiers = modifiers();
        push(e);
        break;
      }
      case SDL_KEYDOWN:
      case SDL_KEYUP: {
        const u8 code = macKeyCode(ev.key.keysym.scancode);
        if (code != 0xFF) {
          const u32 bit = code & 7, byte = code >> 3;
          if (byte < 16) {
            if (ev.type == SDL_KEYDOWN) keys_[byte] |= u8(1u << bit);
            else keys_[byte] &= u8(~(1u << bit));
          }
        }
        const bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const u8 ch = macChar(ev.key.keysym.sym, shift);
        e.what = (ev.type == SDL_KEYDOWN)
                     ? (ev.key.repeat ? kAutoKey : kKeyDown)
                     : kKeyUp;
        e.message = (u32(code == 0xFF ? 0 : code) << 8) | ch;
        push(e);
        break;
      }
      default:
        break;
    }
  }
}

bool Display::pump() {
  if (headless_) return !quit_;
  present();
  handleInput();
  return !quit_;
}

void Display::pushHostClick(s16 h, s16 v, bool down) {
  if (headless_ || !window_) return;
  int wx = 0, wy = 0;
  SDL_RenderLogicalToWindow(static_cast<SDL_Renderer*>(renderer_), float(h),
                            float(v), &wx, &wy);
  // ...and back down from backing pixels to the window points SDL reports.
  int winW = 0, winH = 0, outW = 0, outH = 0;
  SDL_GetWindowSize(static_cast<SDL_Window*>(window_), &winW, &winH);
  SDL_GetRendererOutputSize(static_cast<SDL_Renderer*>(renderer_), &outW, &outH);
  if (outW > 0) wx = int(float(wx) * float(winW) / float(outW));
  if (outH > 0) wy = int(float(wy) * float(winH) / float(outH));
  SDL_Event ev;
  SDL_zero(ev);
  ev.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
  ev.button.button = SDL_BUTTON_LEFT;
  ev.button.state = down ? SDL_PRESSED : SDL_RELEASED;
  ev.button.clicks = 1;
  ev.button.x = wx;
  ev.button.y = wy;
  SDL_PushEvent(&ev);
}

void Display::injectClick(s16 h, s16 v) {
  mouseH_ = h;
  mouseV_ = v;
  // Hold the button down as well as queueing the event. Classic code polls
  // Button() as often as it reads the event queue -- delay loops of the form
  // "wait for time to pass or for the user to click" test Button() and never
  // look at the queue at all -- so a press that only queues an event would be
  // invisible to half the application.
  mouseDown_ = true;
  MacEvent down;
  down.what = kMouseDown;
  down.whereH = h;
  down.whereV = v;
  down.modifiers = 0;               // the button is down, so btnState is clear
  push(down);
}

void Display::injectRelease() {
  mouseDown_ = false;
  MacEvent up;
  up.what = kMouseUp;
  up.whereH = mouseH_;
  up.whereV = mouseV_;
  up.modifiers = kBtnState;
  push(up);
}

void Display::injectKey(u8 keyCode, u8 charCode, u16 modifiers) {
  MacEvent e;
  e.what = kKeyDown;
  e.message = (u32(keyCode) << 8) | charCode;
  e.whereH = mouseH_;
  e.whereV = mouseV_;
  e.modifiers = u16(kBtnState | modifiers);
  push(e);
  e.what = kKeyUp;
  push(e);
}

void Display::idle(u32 ticks, u16 mask) {
  if (headless_ || ticks == 0) return;
  if (peekEvent(mask, nullptr)) return;
  // One tick is a sixtieth of a second. Waiting the whole requested period would
  // make the game feel sluggish, so this caps the nap at a couple of
  // milliseconds: enough to stop the idle loop saturating a core, short enough
  // that input still feels immediate.
  const u32 ms = ticks * 1000 / 60;
  SDL_Delay(ms < 2 ? ms : 2);
}

bool Display::nextEvent(u16 mask, MacEvent* out) {
  for (auto it = queue_.begin(); it != queue_.end(); ++it) {
    if (mask & u16(1u << it->what)) {
      if (out) *out = *it;
      queue_.erase(it);
      return true;
    }
  }
  return false;
}

bool Display::peekEvent(u16 mask, MacEvent* out) const {
  for (const MacEvent& e : queue_) {
    if (mask & u16(1u << e.what)) {
      if (out) *out = e;
      return true;
    }
  }
  return false;
}

}  // namespace cyt
