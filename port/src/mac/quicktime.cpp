// QuickTime, as far as Cythera uses it.
//
// Fifteen of the seventeen QuickTime symbols the game imports belong to the
// QuickTime Music Architecture -- the Tune player and the Note Allocator -- which
// is how Cythera plays its score. The game refuses to start without QuickTime
// 3.0, so this reports it present and implements the music interface as a
// consistent silent sink: channels really are allocated, tunes really are
// accepted and tracked as playing, and only the audio output is missing. That
// keeps the game's own sequencing logic on its normal path, which is what a real
// audio back end will later plug into.
#include <cstdio>
#include <map>

#include "mac/heap.h"
#include "resfork.h"
#include "toolbox.h"

namespace cyt {
namespace {

constexpr s16 kNoErr = 0;

// A tune player's state, keyed by the guest handle handed back to the game.
struct TunePlayer {
  bool playing = false;
  u32 timeScale = 600;    // QuickTime's default, in units per second
  u32 volume = 0x00010000;
};
std::map<GuestAddr, TunePlayer> g_tunes;

}  // namespace

void registerQuickTime(Toolbox& tb) {
  tb.add("EnterMovies", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, kNoErr);
  });
  tb.add("ExitMovies", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, kNoErr);
  });

  // ---- Component Manager --------------------------------------------------
  // The Note Allocator and Tune Player are reached as components. A component
  // instance is opaque to the caller, so a distinct non-null block per open is
  // all the game can observe.
  tb.add("OpenDefaultComponent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u32v();                      // component type
    a.u32v();                      // component subtype
    GuestAddr inst = tb.heap().newPtr(16, true);
    Toolbox::ret(c, inst);
  });
  tb.add("OpenComponent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u32v();
    Toolbox::ret(c, tb.heap().newPtr(16, true));
  });
  tb.add("CloseComponent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr inst = a.ptr();
    g_tunes.erase(inst);
    tb.heap().disposePtr(inst);
    Toolbox::ret(c, kNoErr);
  });
  tb.add("FindNextComponent", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.u32v(); a.ptr();
    Toolbox::ret(c, 0);            // no further components to enumerate
  });

  // ---- Note Allocator -----------------------------------------------------
  tb.add("NAStuffToneDescription", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                       // the note allocator instance
    u32 instrumentNumber = a.u32v();
    GuestAddr toneDesc = a.ptr();
    // ToneDescription: { BigEndianOSType synthesizerType; Str31 synthesizerName;
    //                    Str31 instrumentName; BigEndianLong instrumentNumber;
    //                    BigEndianLong gmNumber; }
    if (toneDesc) {
      tb.mem().fill(toneDesc, 0, 76);
      tb.mem().w32(toneDesc + 0, resType("gmid"));   // General MIDI synthesiser
      tb.mem().w32(toneDesc + 68, instrumentNumber);
      tb.mem().w32(toneDesc + 72, instrumentNumber);
    }
    Toolbox::ret(c, kNoErr);
  });
  tb.add("NANewNoteChannel", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();                       // note allocator
    a.ptr();                       // tone description
    GuestAddr out = a.ptr();
    // The channel must be non-null: the game checks it and reports a fatal
    // error if allocation fails.
    GuestAddr chan = tb.heap().newPtr(32, true);
    if (out) tb.mem().w32(out, chan);
    Toolbox::ret(c, chan ? kNoErr : u32(s32(-108)));
  });
  tb.add("NADisposeNoteChannel", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr();
    tb.heap().disposePtr(a.ptr());
    Toolbox::ret(c, kNoErr);
  });
  tb.add("NAPlayNote", [](Toolbox& tb, PpcCpu& c, Args& a) {
    a.ptr(); a.ptr(); a.u32v(); a.u32v();
    Toolbox::ret(c, kNoErr);
  });
  tb.add("NASetNoteChannelVolume", [](Toolbox& tb, PpcCpu& c, Args& a) {
    Toolbox::ret(c, kNoErr);
  });

  // ---- Tune Player --------------------------------------------------------
  tb.add("TuneSetHeader", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    a.ptr();                       // the tune header
    g_tunes[tp];                   // materialise the player's state
    Toolbox::ret(c, kNoErr);
  });
  tb.add("TuneQueue", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    g_tunes[tp].playing = true;
    Toolbox::ret(c, kNoErr);
  });
  tb.add("TuneGetStatus", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    GuestAddr status = a.ptr();
    // TunePlayStatus: { queueTime, queueCount, time, ... , queueSpaceLeft }.
    // Reporting an empty queue is what lets the game's music loop advance to
    // the next piece instead of waiting forever for the current one to finish.
    if (status) {
      tb.mem().fill(status, 0, 40);
      tb.mem().w32(status + 4, 0);          // queueCount: nothing pending
      tb.mem().w32(status + 36, 0x1000);    // queueSpaceLeft: room to spare
    }
    if (auto it = g_tunes.find(tp); it != g_tunes.end()) it->second.playing = false;
    Toolbox::ret(c, kNoErr);
  });
  tb.add("TuneSetTimeScale", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    g_tunes[tp].timeScale = a.u32v();
    Toolbox::ret(c, kNoErr);
  });
  tb.add("TuneSetVolume", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    g_tunes[tp].volume = a.u32v();
    Toolbox::ret(c, kNoErr);
  });
  tb.add("TuneStop", [](Toolbox& tb, PpcCpu& c, Args& a) {
    GuestAddr tp = a.ptr();
    if (auto it = g_tunes.find(tp); it != g_tunes.end()) it->second.playing = false;
    Toolbox::ret(c, kNoErr);
  });
  for (const char* ok : {"TunePreroll", "TuneUnroll", "TuneFlush",
                         "TuneSetPartTranspose", "TuneInstant"})
    tb.add(ok, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, kNoErr);
    });

  // ---- previews -----------------------------------------------------------
  // Saved-game thumbnails are cosmetic; refusing them leaves saves without a
  // Finder preview and changes nothing the game itself reads back.
  for (const char* preview : {"MakeThumbnailFromPixMap", "MakeFilePreview",
                              "AddFilePreview"})
    tb.add(preview, [](Toolbox& tb, PpcCpu& c, Args& a) {
      Toolbox::ret(c, kNoErr);
    });
  // StandardGetFilePreview is the Standard File open dialogue with a preview
  // pane, not a QuickTime call, and belongs to standard_file.cpp.
}

}  // namespace cyt
