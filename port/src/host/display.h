// The host window: presents the guest framebuffer and turns host input into
// classic Macintosh events.
//
// The integration point is WaitNextEvent. A cooperative Mac application calls it
// every time round its main loop, which makes it exactly the right place to
// present a frame and collect input -- no threads, no separate render loop, and
// the game's own pacing drives the display.
#pragma once

#include <deque>
#include <string>

#include "mem.h"

namespace cyt {

// One classic event, in the form an EventRecord stores.
struct MacEvent {
  u16 what = 0;
  u32 message = 0;
  u32 when = 0;
  s16 whereH = 0, whereV = 0;
  u16 modifiers = 0;
};

// Event codes.
constexpr u16 kNullEvent   = 0;
constexpr u16 kMouseDown   = 1;
constexpr u16 kMouseUp     = 2;
constexpr u16 kKeyDown     = 3;
constexpr u16 kKeyUp       = 4;
constexpr u16 kAutoKey     = 5;
constexpr u16 kUpdateEvt   = 6;
constexpr u16 kActivateEvt = 8;
// A high-level event -- an Apple event, in practice. The launch event the
// Finder sends arrives this way.
constexpr u16 kHighLevelEvent = 23;

// Modifier bits.
constexpr u16 kBtnState  = 0x0080;   // set when the button is *up*
constexpr u16 kCmdKey    = 0x0100;
constexpr u16 kShiftKey  = 0x0200;
constexpr u16 kAlphaLock = 0x0400;
constexpr u16 kOptionKey = 0x0800;
constexpr u16 kControlKey= 0x1000;

class Display {
 public:
  static Display& get();

  // Opens the window. `scale` multiplies the 640x480 logical size.
  bool open(int scale, std::string* err);
  // Runs without a window: input can still be injected and the framebuffer
  // dumped, which is how the port is tested without a window server.
  void openHeadless() { headless_ = true; }
  void close();
  bool isOpen() const { return window_ != nullptr || headless_; }
  bool windowed() const { return window_ != nullptr; }

  // Synthesises input, for tests and for scripted start-up.
  void injectClick(s16 h, s16 v);
  // Pushes a genuine SDL button event, so a test can drive the real input path
  // -- window coordinates, SDL_RenderWindowToLogical, handleInput -- rather
  // than only the event queue behind it. Does nothing when headless.
  void pushHostClick(s16 h, s16 v, bool down);
  void injectRelease();
  void injectKey(u8 keyCode, u8 charCode, u16 modifiers = 0);

  // Idles for up to `ticks` sixtieths of a second while no event is waiting, so
  // an application polling its event loop does not spin a core needlessly.
  void idle(u32 ticks, u16 mask);

  // Presents the framebuffer, then drains host input into the event queue.
  // Returns false when the user has asked to quit.
  bool pump();

  // Removes the next event matching `mask`, or reports that there is none.
  bool nextEvent(u16 mask, MacEvent* out);
  bool peekEvent(u16 mask, MacEvent* out) const;

  bool mouseDown() const { return mouseDown_; }
  s16 mouseH() const { return mouseH_; }
  s16 mouseV() const { return mouseV_; }
  u16 modifiers() const;
  bool quitRequested() const { return quit_; }

  // The 128-bit KeyMap that GetKeys reports.
  void keyMap(u8 out[16]) const;

 private:
  void present();
  void handleInput();
  void eventToLogical(int px, int py, s16* outH, s16* outV) const;
  void push(const MacEvent& e);

  void* window_ = nullptr;      // SDL_Window
  void* renderer_ = nullptr;    // SDL_Renderer
  void* texture_ = nullptr;     // SDL_Texture, 640x480 32-bit
  std::deque<MacEvent> queue_;

  bool mouseDown_ = false;
  s16 mouseH_ = 320, mouseV_ = 240;
  bool quit_ = false;
  bool headless_ = false;
  u32 lastPresent_ = 0;
  u8 keys_[16] = {};
};

}  // namespace cyt
