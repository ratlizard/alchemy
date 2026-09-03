# Cythera, ported to modern macOS

A native arm64 port of *Cythera: Fate of Alaric* (Ambrosia Software / Glenn
Andreas, 1999), built without access to the original source code.

## Why this shape

Cythera 1.0.4 is a fat Macintosh application: a PowerPC PEF container in the
data fork (840 KB of code, 1877 symbolized functions) plus a 68k `CODE`-resource
build in the resource fork. Three routes were possible, and the evidence pointed
firmly at one of them.

**Reimplementing the engine** from the original data files — the OpenMW or Exult
model — is not viable here. The provided `delvmod` reads every data format
Cythera uses, but it is a *format* library: there is no simulation, no combat
resolution, no creature AI, no rendering pipeline, and its script VM is
documented rather than executed. All of that behaviour lives in the 1877 PowerPC
functions, and the symbol table gives their names but nothing about what they
do. A reimplementation would be guesswork dressed as a port.

**Shipping an emulator** (SheepShaver, QEMU) needs a copyrighted Mac OS ROM, is
not a port, and was unavailable offline in any case.

**Translating the original code** is what this does: load the PEF, interpret its
PowerPC instructions, and reimplement the Mac OS Toolbox underneath on modern
frameworks. The game logic is the original binary, so behaviour is exact by
construction, and the work is a bounded host API rather than an open-ended
reconstruction. Three measurements made this the clear choice:

| Measurement | Value | Why it matters |
|---|---|---|
| Distinct PowerPC opcode forms used | **76** of ~200 | The interpreter is ~650 lines, not a research project |
| Imported symbols | **563** across 10 libraries | A bounded, enumerable host surface |
| Of those, non-weak | **498**, all from InterfaceLib | Nine libraries are optional; one must be real |

The nine weak libraries matter more than the count suggests. Because Appearance
Manager is reported absent, the game falls back to the widget-drawing code it
already carries for pre-8.5 systems — the `T7Widget` class family visible in the
symbol table. That single decision removes an entire manager from the work.
The same is true of Navigation Services, InputSprocket and the Contextual Menu
Manager. Two of the "optional" libraries turned out to be mandatory anyway:
Cythera refuses to start without the Thread Manager or QuickTime.

A useful consequence: the custom `WDEF`, `CDEF` and `MDEF` resources in the fork
are six-byte `JMP $0` placeholders that the game patches at run time with Mixed
Mode routine descriptors. The real definition procedures are native PowerPC
inside the PEF, so **no 68k emulator is needed** — only `LDEF` 128 is genuine
68k, and it is a 128-byte trampoline into a client callback.

## Layout

```
src/mem.h .cpp          256 MiB guest address space, big-endian accessors
src/pef.h .cpp          PEF container: sections, pattern data, relocation, imports
src/ppc.h .cpp          user-mode PowerPC interpreter + nested guest calls
src/resfork.h .cpp      classic resource fork reader
src/toolbox.h .cpp      import -> native handler dispatch, Gestalt, call tracing
src/mac/heap.h .cpp     Memory Manager zone: pointers, handles, master pointers
src/mac/memory_mgr.cpp  Memory Manager entry points
src/mac/resource_mgr.cpp Resource Manager over the real forks
src/mac/file_mgr.h .cpp  HFS-to-host-directory mapping, two forks per file,
                         Finder type and creator
src/mac/file_calls.cpp   File Manager entry points
src/mac/standard_file.cpp Standard File, answered from the support directory
src/mac/core.cpp         init sequence, Mixed Mode, strings, time, alerts
src/mac/menu_mgr.cpp     Menu Manager
src/mac/quickdraw.h .cpp screen GDevice, PixMap, colour table, ports
src/mac/thread_mgr.cpp   cooperative Thread Manager (real context switching)
src/mac/event_mgr.cpp    Event Manager and Sound Manager
src/mac/apple_event.cpp  the launch Apple event and its dispatch
src/mac/quicktime.cpp    QuickTime Music Architecture (the game's score)
src/mac/font_mgr.h .cpp  Font Manager: FOND/NFNT, strike cache, style synthesis,
                         text measurement and drawing
src/host/hostfont.h .cpp CoreText rasteriser for outlines and host families
tests/font_test.cpp      resolves every 'TxSt' style the game carries
tests/resfork_test.cpp   round-trips the resource fork writer through the reader
../tools/                PEF, resource fork and opcode analysis tools;
                         pefdisasm.py reads any function in the binary
                         statically, with imported calls named
```

Continuing this work: **[POWERPC-NOTES.md](POWERPC-NOTES.md)** has the current state, the
traps that cost the most time to find, and what to do next. `./smoke.sh` checks
that the machinery is still intact.

## Building and running

Requires the original `Cythera.hqx` and `Cythera Data.hqx`, which this
repository keeps in `../res/`. `run.sh` decodes both forks out of them with
`../utilities/binhex_decode.py` on first use and leaves the result in
`build/extract/`. Nothing is downloaded and no original game data is
redistributed.

`cythera_symbols.txt` — the 1877 function names, recovered once by walking the
binary's traceback tables — is *not* in this repository. Everything here runs
without it; traces simply print addresses where they would print names. Drop it
at the repository root and the scripts pick it up. `../tools/opcensus.py` and
`../tools/pefdisasm.py` do require it.

```sh
./run.sh                    # extract forks, build, run
./run.sh --trace-calls      # log every Toolbox call with its arguments
./run.sh --trace            # log every interpreted instruction (very verbose)
./smoke.sh                  # build and check ten invariants, about a minute
./drive.sh --click-at-pass 600000:245,196   # run headless and click New Game
```

## Where it stands

The port **starts Cythera up and reaches its start screen**, driven entirely by
the original PowerPC code: the title art, then the wooden signboard with its
candles, animated torch and six choices, set in the game's own display face. The
screen is live — a click is routed through the application's own widget layer to
its item handler, and choosing **Quit** walks the game's shutdown path to
`ExitToShell`.

All six choices now lead somewhere. **New Game** reaches `TDelverApp::NewGame`,
which asks Standard File where to keep the new player, creates the file the port
names, and goes on to open its character-creation dialogue. **Open Game** reads
that file back and, finding it empty, says so in the game's own words: *"This
file is corrupted."* The game also keeps its preferences now, in a resource fork
the port writes. It is **not yet playable**: character creation needs the Dialog
and List Managers, which is where the next session starts.

![the start screen](doc/start-screen.png)

*Above: Cythera's start screen, as the port draws it. The title art that
precedes it is in [`doc/title-screen.png`](doc/title-screen.png).*

Getting from the splash to that screen was one missing piece, and it was not a
manager. A classic Macintosh application is not started by its `main` alone: the
Finder launches it and then *sends* it the `'oapp'` Apple event, and a
framework-based application does its real start-up work in that handler.
Cythera's `main` initialises the Toolbox, shows the splash and enters the event
loop; everything that opens the start screen hangs off the `'oapp'` handler.
With no Finder to send it, the game idled in a perfectly healthy event loop
forever, drawing nothing and asking for nothing — which looks exactly like a
missing manager and was diagnosed as one. The port now sends the event itself,
and the path through the application is the real one: `WaitNextEvent` returns a
high-level event, the game calls `AEProcessAppleEvent`, and that dispatches
through Mixed Mode to its own `HandleOpenApplication`.

Two things fell out of that. Cythera loads its entire world *inside* the handler,
so a nested guest call can legitimately run for the rest of the program: the
overall instruction budget is now enforced by the interpreter rather than by the
caller's slicing. And scheduled input is delivered at the port's frame boundary,
which is the only place that is reached both from inside a long nested call and
from a mouse-tracking loop that never returns to the event loop while the button
is down.

Instruction counts are *not* reproducible between runs, because the game's
delay loops poll `TickCount` and therefore consume however many instructions the
host needs to burn while real time passes. Progress is measured against the
application's own state instead — `--click-at-pass` and `--cmd-key-at-pass`
schedule input against its event-loop pass count.

Each of these gates was diagnosed from the game's own words or its own
behaviour. The alert layer resolves `ALRT`/`DITL` resources and `ParamText` into
readable text, which turned every opaque exit into an instruction.

| Gate | Diagnosis |
|---|---|
| `NewPtr` returned null | no Memory Manager |
| "Unable to open data file" | no File Manager |
| "No suitable monitor available (640x480, 256 colors)" | no screen GDevice |
| "You need to install the thread manager" | `Gestalt('thds')` |
| "You need to install QuickTime 3.0 or later" | `Gestalt('qtim')` **and** `'qtrs'` |
| Green-on-black artwork | `pltt` header is 16 bytes, not 8 |
| Worker thread never ran | the game only yields once it is interactive |
| Which fonts to implement | the 20 `TxSt` resources name all five, and nothing else |
| Splash never advanced | nobody sent the `'oapp'` Apple event |
| New Game does nothing | `StandardPutFile` reported "cancelled" |
| Open Game does nothing | `StandardGetFilePreview` was claimed by two managers |
| Preferences never saved | `CreateResFile` made no fork, and writing was refused |

### Working

- PEF loading: container, pattern-initialised data expansion, the full
  relocation opcode set, import binding, traceback-table symbolization
- PowerPC interpreter: integer, floating point, condition register, carry and
  overflow, every branch form, and calls from host code back into guest code.
  Roughly 85 million instructions a second, against perhaps 100 million on the
  hardware this was written for
- Memory, Resource, File and Menu Managers; `MenuKey` resolves command keys
- Resource **writing**: `CreateResFile` produces a real fork, and `AddResource`,
  `ChangedResource`, `WriteResource`, `RemoveResource`, `SetResInfo` and
  `SetResAttrs` commit through `UpdateResFile`. The game's preferences
  round-trip byte for byte
- Standard File, answered from the support directory with no host UI, so the
  new-game and load-game paths run headlessly under `smoke.sh`. Finder type and
  creator are recorded per file, so an open filters by type as the original did
- Cooperative Thread Manager with genuine context switching
- QuickDraw: screen GDevice and PixMap built from the game's own `clut` 256,
  regions with exact set algebra, ports and clipping, solid shapes, lines,
  `CopyBits` with scaling and colour matching, offscreen GWorlds, palettes
- PICT playback: all six `CopyBits` opcodes, PackBits, indexed 1/2/4/8-bit and
  direct 16/32-bit, verified against all 21 pictures in the game's forks
- Window Manager with correct visible-region computation for overlapping windows
- Cooperative Thread Manager, QuickTime Music Architecture as a silent sink
- SDL2 front end: the framebuffer presented through the live colour table,
  scaled with nearest-neighbour, and host input translated to classic events
- Text: the Font Manager over the game's own `FOND`/`NFNT`/`sfnt` resources,
  with style synthesis, measurement and the drawing calls. Ten of the eleven
  strikes a start-up builds are exact; only Chicago is substituted
- Apple events: the launch event, and dispatch to the application's own handlers
  through Mixed Mode
- Diagnostics: a sampling profiler (`--profile`), call tracing from a chosen
  instruction (`--trace-from`) or from a chosen *function*
  (`CYT_TRACE_CALLS_FROM_PC`, which works inside a nested guest call where the
  instruction form cannot reach), scheduled input, framebuffer dumps, thread
  stats, and a static disassembler that names the imported routine behind every
  cross-TOC glue stub, so a function can be read before it is ever reached

### Next, in dependency order

1. **Dialog Manager** — `GetNewDialog`, `DrawDialog`, `DialogSelect`,
   `GetDialogItem` and `CountDITL`. This is now the only thing between the port
   and character creation: clicking New Game gets as far as `GetNewDialog(133)`
   and stops there. `DLOG` 133 and its `DITL` are the whole character sheet.
   It also needs **pixel pattern tiling**, which the dialogue paints its
   background with — see POWERPC-NOTES.md §1, where the whole path is written out
   from a static read of the binary.
2. **List Manager** — `LNew`, `LAddRow`, `LSetCell`, `LGetSelect`, `LSetSelect`
   and `LScroll`. The archetype and portrait pickers in that dialogue are
   lists, so the two managers land together.
3. **Menu bar drawing and `MenuSelect`**, so menus can be used with the mouse
   and not only by command key.
4. **Control and TextEdit Managers.** Thinner than usual, because Cythera
   draws its own widgets through the `T7Widget` family.
5. **The gameplay rendering path** — tiles, sprites, the map viewer, the
   lighting behind the 25 `Lite` resources. None of it has executed yet. The
   first thing it will want is pixel pattern tiling, which the character
   creation dialogue now needs first.
6. **Audio** — route `SndPlayDoubleBuffer` and the Tune Player to SDL audio.
   `../utilities/qtma2midi.py` already decodes the tune format.
7. **Packaging** — a signed `Cythera.app`, saves under
   `~/Library/Application Support`, and first-run extraction from the user's own
   copy of the game.

### Known gaps worth naming

- **Standard File opens no chooser.** A save is accepted under the name the
  caller suggested and an open returns the first file of an acceptable type in
  the support directory, so there is no way for the user to decline or to pick
  among several saves. That is what makes the path testable headlessly; a
  native chooser belongs behind a flag, layered on top.
- **A file filter procedure is not called**, and `CustomGetFile`'s and
  `CustomPutFile`'s dialogue hooks are ignored, because no dialogue opens.
- **Finder metadata lives in a sidecar.** A host file has nowhere to keep a
  type and creator, so the port writes ten bytes beside the data fork. Files
  extracted before the sidecar existed report Cythera's own document type.
- Pixel patterns are allocated as real `PixPat` records — the application
  reshapes them itself — but installing one in a port does not yet change what
  is drawn. Cythera builds three during start-up, one of them a 128×128
  eight-bit tile that is almost certainly the backdrop behind its windows.
- **Chicago** and **Espy Sans** are substituted by Geneva: one was retired after
  Mac OS 8 and the other never shipped as a system font, so neither exists on a
  modern host or in the game's forks. Everything else Cythera names is exact.
  Underlines also draw straight through descenders rather than breaking around
  them.
- Regions are stored host-side as rectangle lists and exposed to the guest as
  their bounding box. Correct for everything Cythera does with them, but code
  that walked a region's scanline data would not see it.
- `SetThreadScheduler` records the game's custom scheduler
  (`TTaskMaster::MyScheduler`) but keeps round-robin order.

## Format notes worth keeping

These were established empirically against this binary and are easy to get
wrong from documentation alone.

- **PEF relocation opcodes** are dispatched on `word >> 9`. For
  `kPEFRelocBySectDWithSkip` the fields are **skip(8) then count(6)**
  — `skip = (w >> 6) & 0xFF`, `count = w & 0x3F`. With the widths the other way
  round, thousands of relocations land on garbage. `kPEFRelocIncrPosition`
  advances by `(w & 0xFFF) + 1` **bytes**, and those bytes are not always a
  multiple of four: Metrowerks used 68k-compatible two-byte struct alignment, so
  some relocation targets are genuinely unaligned. PowerPC tolerates that in
  hardware, and the accessors must too.
- **Pattern-initialised data**: opcode is `byte >> 5`, first argument is
  `byte & 0x1F` with zero meaning "a variable-length integer follows". For
  `kPEFPkDataRepeatBlock` the common block is read from the container once; for
  `kPEFPkDataRepeatZero` **the common block is the zeroes** and it is the custom
  blocks that come from the container. Getting these two backwards is the most
  likely single mistake. Both emit `repeatCount + 1` common blocks separated by
  `repeatCount` custom ones. Correct decoding expands Cythera's data section to
  exactly its declared 49548 bytes while consuming exactly its declared 34490.
- **`extsb` (954), `extsh` (922), `sraw` (792), `srawi` (824)** and the other
  logical forms carry bit 9 of the extended opcode as part of the opcode, not as
  overflow-enable. Only the arithmetic forms may be decoded with that bit
  stripped.
- **Trap availability**: every unsupported trap must return the *same* address
  from `NGetTrapAddress`, because the standard check compares against
  `_Unimplemented`. Returning a distinct fake address per trap makes the
  application believe the trap exists and call into nothing — which is exactly
  what Cythera does when it probes for the Cursor Device Manager at `0xAADB`.
- **`NFNT` tables** both begin `lastChar - firstChar + 2` entries long -- the
  character range plus the missing-symbol image -- but only the *location* table
  carries the extra entry that bounds the last glyph's right edge. Reading the
  offset/width table as though it had that entry too runs two bytes past the end
  of the resource. `owTLoc` is a word offset measured from the `owTLoc` field
  itself, and the bearing it stores is relative to `kernMax`, not to the pen.
- **A `FOND` association at size 0** means "scalable, ask the outline", which is
  how ArgosANouveau's single entry points at `sfnt` 7289 rather than at a bitmap
  strike. Treating 0 as a real point size finds nothing.
- **Gestalt** answers steer the whole run. Report present only what is actually
  implemented; `gestaltUndefSelectorErr` makes the game use the fallback code it
  already carries. Cythera checks `'thds'` for the Thread Manager and both
  `'qtim'` and `'qtrs'` for QuickTime — answering only the first of that pair
  still fails.

## Legal

This port contains no Ambrosia Software or Glenn Andreas code or data. It reads
the forks of a copy of the game that you supply. "Cythera" and "Delver" are
trademarks of Ambrosia Software, Inc. or Glenn Andreas. `delvmod`, used here only
for data inspection, is GPLv3 by Bryce Schroeder.
