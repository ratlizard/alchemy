# Handoff

> **This file describes the PowerPC slice.** Every address in it — and every
> mechanism built on routine descriptors, `NewRoutineDescriptor` or the run-time
> `0x4EF9` patching — was read from Cythera's PEF with `tools/pefdisasm.py` and
> `reference/cythera_symbols.txt`. systemless runs the game's **68K** slice, so
> claims here must be re-derived from the `CODE` resources before being asserted
> about it. Three were not, and all three were wrong: see the findings section of
> `../SYSTEMLESS.md`. `CLAUDE.md` explains the split.


For the next session. `README.md` has the architecture and the format notes;
this is the state of play, the traps, and what to do next.

**Status: this port is retired as a development target** — active work on
running the game moved to the systemless fork, where a missing Toolbox call
is a handler rather than a subsystem (`../CLAUDE.md` § Current direction,
`../SYSTEMLESS.md` § What each tree can lend the other). Everything below
stays accurate and the smoke suite still holds; read the "what to do next"
sections as a record of where this tree stopped and as reference material
for systemless work, not as a queue. The one live use this tree's code has
is named in the cross-checks section: its PowerPC-ABI Toolbox handlers are
worked answers for the imports systemless's PowerPC loader is missing.

## Start here

```sh
cd cythera/port
./smoke.sh          # build + ten invariants, about a minute
./run.sh            # build + run in a real window
```

`smoke.sh` is the regression check. All ten invariants pass as of this handoff:
every Toolbox call the game makes is implemented, no out-of-range guest memory
accesses, the main event loop is reached, the launch Apple event reaches the
application's own handler, the game's own artwork is drawn, every text style
resolves to a real strike, the resource fork writer round-trips through the
reader, **the game's preferences are written and read back**, **clicking New
Game creates a player file**, and the build is warning-free. **Run it before and
after any change.** If it passes, the machinery underneath is intact and any new
problem is in what you just wrote.

It now makes two runs of the game — one ordinary start-up, one that clicks New
Game — so it takes about a minute rather than forty seconds.

If `cmake` complains that `CMakeCache.txt` was generated for a different source
directory, the tree has been moved: delete `build/CMakeCache.txt` and
`build/CMakeFiles`. Nothing else in `build/` needs to go — in particular
`../build/extract` holds the decoded forks and is expensive to rebuild.

## What actually works

The port loads the original PowerPC binary, interprets it, and reimplements the
Classic Mac Toolbox underneath. Cythera starts up, reaches its start screen, and
the screen is live: the wooden signboard, the candles, the animated torch and
the six choices set in the game's own display face, all drawn by the original
code (`doc/start-screen.png`).

**Every choice on that screen now leads somewhere.**

- **New Game** runs the game's own widget layer into `TDelverApp::NewGame`,
  which asks `StandardPutFile` where to keep the new player. The port answers
  from the support directory, the game creates `Bellerophon` — its own default
  name — as a document of type `'DelP'`, and goes on to open its
  character-creation dialogue. That is where it now stops, and the Dialog
  Manager is why.
- **Open Game** reaches `TDelverApp::DoOpen` → `StandardGetFilePreview`, gets
  the saved player back, opens it, reads it, and reports in the game's own
  words: *"This file is corrupted."* Which it is — nothing has filled it in yet.
  The whole path is real; only the contents are missing.
- **Quit** walks the game's shutdown path to `ExitToShell`, as before.

The game also **keeps its preferences** now. `TPrefs` creates
`Cythera Preferences`, adds a `'Pref'` resource named "UI Prefs" to it and
commits it; the next run reads it back and rewrites it byte for byte.

Text rendering works: `DrawChar`, `DrawString`, `DrawText` and `TETextBox` draw
real glyphs, and `StringWidth`, `TextWidth`, `CharWidth`, `GetFontInfo`,
`MeasureText`, `StyledLineBreak` and `TruncString` measure them. Ten of the
eleven strikes a start-up builds are exact, from Cythera's own resources.

It is **not yet playable**: character creation needs the Dialog and List
Managers. Roughly 11,000 lines across 44 files.

## What this session added

Nothing that runs — no new manager. What it added is the ability to **read the
game's code before implementing against it**, and then a day of reading with it.

`--dump-glue` writes the address of every cross-TOC glue stub in the binary
paired with the import it reaches, and `tools/pefdisasm.py` disassembles any
address range with branches resolved and named from both that map and
`cythera_symbols.txt`. Without the glue map a static disassembly shows
`bl 0xc3258` where it means `GetNewDialog`, which is most of why reading this
binary was previously not worth the trouble. 561 of the 563 imports are
resolved. See *Reading a function before it runs* below.

Everything under §1's *What is already known about it* was then read that
way: the `WDEF`/`CDEF` patch mechanism, the whole `LDEF` 128 → `refCon` →
`MyLDEF` → `LDEFDraw` protocol, the `ListRec` offsets the game writes through,
how `TDialog` installs its user-item draw procs, the `ModalDialog` filter chain,
and the exact construction order of `TCreatePlayerDialog`. That last one turned
up something the plan had wrong: **the character-creation dialogue's background
is drawn through a pixel pattern**, so `PixPat` support is part of §1 rather
than a later nicety.

`smoke.sh` still passes all ten invariants, warning-free.

## What the session before added

**Standard File** (`mac/standard_file.cpp`). `StandardPutFile` accepts under the
name the caller suggested; `StandardGetFile` and `StandardGetFilePreview` return
the first file in the support directory whose Finder type is one the caller
asked for. No host UI, so the whole new-game and load-game path runs headlessly
and is now part of `smoke.sh`. A native chooser would be truer to the original
but cannot be part of a regression check; it belongs behind a flag, layered on
top of this.

**Finder metadata.** A host file has nowhere to keep a type and creator, so the
port writes a ten-byte `.finf` sidecar beside the data fork. `FSpCreate`,
`HCreate` and the `SetFInfo` family record it and `GetFInfo` reports it. This is
not decoration: without it "Open Game" could not tell a saved game from the
licence file and the log, which live in the same directory.

**Resource writing** (`mac/resource_mgr.cpp`, `resfork.cpp`). `CreateResFile`
now writes a real, empty fork; `AddResource`, `ChangedResource`,
`WriteResource`, `RemoveResource`, `SetResInfo` and `SetResAttrs` build an
overlay on the parsed fork, and `UpdateResFile`/`CloseResFile` serialise the
whole thing back. Edits carry the guest **handle**, not a copy of its bytes,
because the application keeps writing through the handle after `AddResource`.
A file is only writable if it lives under `--support-dir`, so no permission the
game asks for can touch the extracted originals.

**Three bugs found on the way**, each of which would have bitten later:

- `GetHandleSize` reported the *physical* block size, which the heap rounds up
  and may enlarge further to swallow a free-list tail too small to track. An
  application that reads a resource by its handle's size — which Cythera does —
  would have read the padding as data. `Heap::Block` now carries the logical
  size as well.
- The resource fork reader could not read an **empty** fork. A zero type count
  is stored as `0xFFFF`, and `+ 1` made it 65536 types.
- `StandardGetFilePreview` was registered by **two** managers, and the later
  registration silently won. See trap 6.

## The launch event, and why it was the gate

A classic Macintosh application is not started by its `main` alone. The Finder
launches it and then *sends* it the `'oapp'` ("open application") Apple event,
and a framework-based application does its real start-up work in that handler.
Cythera is one of those: `main` initialises the Toolbox, shows the splash, waits
120 ticks and enters the event loop, while everything that opens the start
screen hangs off the `'oapp'` handler installed during `TApp::InitMac`. With no
Finder to send the event, the game reached its event loop and idled there
forever — 2.9 million passes, drawing nothing, asking for nothing.

`mac/apple_event.cpp` delivers the event itself. The path through the
application is the real one: `WaitNextEvent` returns a high-level event, the
game calls `AEProcessAppleEvent`, and that dispatches through Mixed Mode to
`HandleOpenApplication`, which calls `TDelverApp::DoStartup` →
`TDelverApp::RunStart` → `TRunStart`, the start screen. Two details matter:

- The event is offered only once **a handler for it exists**. Delivered before
  `AEInstallEventHandler` ran, it would be answered with
  `errAEEventNotHandled` and the game would never start.
- For a high-level event the `EventRecord`'s `message` holds the event class and
  its `where` field, read as a long, holds the event ID. It is not a point.

Two consequences reach further than the file itself:

**Nested guest calls can run for the rest of the program.** Cythera loads its
entire world inside that handler — hundreds of `FSRead`s and 800 M instructions
before it draws. The handler is therefore called with no budget of its own, and
`--budget` is enforced by the interpreter (`Interp::setInstructionLimit`) so
that it still applies inside nested calls.

**Scheduled input has to be delivered at the frame boundary.** `--click-at-pass`
acts from `pumpHost`, through `setHostPumpHook`, because the outer interpreter
slices never come round while a nested call is running — which is now most of
the game's life. That is also the only place that works for a mouse-tracking
loop, which polls `Button()` and never returns to the event loop while the
button is down.

The same reasoning applies to diagnostics, which is why
`CYT_TRACE_CALLS_FROM_PC` exists alongside `--trace-from`: see below.

## Traps — read before debugging anything

These each cost an hour or more to find.

**1. When the game idles, ask what it is waiting *for*.** The symptom that took
the longest to read was an application sitting in a perfectly healthy event
loop. Nothing was missing; nothing was wrong. It was waiting for an event that
only the operating system sends. Before implementing a manager because the game
"must be stuck on it", check that the game has actually asked for it —
`--stop-after` and the call census in the log are the evidence.

**2. Instruction counts are not reproducible between runs.** The game's delay
loops poll `TickCount`, so they consume however many instructions the host burns
while real time passes. Use `--click-at-pass N` and `--cmd-key-at-pass N:C`,
which trigger on the application's own event-loop pass count, and
`CYT_TRACE_CALLS_FROM_PC`, which triggers on an address.

**3. The budget must cover real seconds, not instructions.** Start-up fades the
splash in over 300 ticks and out over 120, spinning on `TickCount` throughout,
then loads the world. `smoke.sh` uses 2.5 billion; the start screen appears at
about 1.5 billion and a click at pass 600000 lands at about 1.78 billion.

**4. `Button()` is live state, not a queued event.** Delay loops of the form
"wait for time to pass or for the user to click" poll `Button()` and never touch
the event queue. `Display::injectClick` therefore holds the button down and
`injectRelease` lifts it.

**5. Symptoms point away from causes here.** The green-on-black artwork was a
`pltt` header size. The "starved worker thread" was really the budget. The
"missing Dialog Manager" was a missing Apple event. When something looks
structurally broken, measure before theorising.

**6. Two managers must never claim the same Toolbox name.** `quicktime.cpp`
registered `StandardGetFilePreview` — reasonably enough, since it is the open
dialogue with a QuickTime preview pane — and because it is registered after
`standard_file.cpp`, it silently replaced it. The symptom was a call that was
plainly being dispatched and a handler that plainly never ran, which reads like
a build problem and is not one. `Toolbox::add` now keeps the **first**
registration and prints a warning; the six other collisions it immediately
found have been resolved by deleting whichever copy was in the wrong manager.
One of them mattered: `SystemTask` was registered as a no-op by `core.cpp` and
as `pumpHost` by `event_mgr.cpp`, and only the registration order was keeping
input alive.

**7. `CTFontCreateWithName` never fails.** Ask it for "Chicago" and it hands
back Helvetica without a word. `hostfont.cpp` asks instead by matching a font
descriptor with the family name marked *mandatory*, which is the only call that
answers the real question.

**8. The font registry must be rescanned, not built once.** `InitFonts` runs
*before* the game opens `Cythera Data`, so a single scan sees five families and
misses three of the five faces the game names. `scanFonds` is idempotent and
re-runs on every `GetFNum`.

## Diagnostics available

| Tool | Use |
|---|---|
| `--profile N` | sample the PC every N instructions; prints where time went |
| `--trace-from N` | run quietly to instruction N, then log every Toolbox call |
| `CYT_TRACE_CALLS_FROM_PC=0x…` | the same, but armed when the interpreter *reaches an address* — which is reproducible, and works inside a nested guest call where `--trace-from` cannot reach |
| `--trace-calls` | log every Toolbox call with its arguments |
| `--stop-after N` | halt after N unimplemented calls (default 40) |
| `--click-at-pass N[:H,V]`, `--cmd-key-at-pass N:C` | scheduled input |
| `--dump-screen F` + `tools/screen_to_png.py` | see the framebuffer headless |
| `build/cyt_font_test <app.rsrc> <data.rsrc> [--art]` | resolve every `TxSt` style |
| `build/cyt_resfork_test` | round-trip the resource fork writer |
| `--dump-glue FILE` + `tools/pefdisasm.py` | read any function's Toolbox calls *without running it* |
| `CYT_WATCH_PC=0x…[,0x…]` | log registers, instruction count, symbol and caller at each address |
| `CYT_TRACE_RANGE=0xlo:0xhi` | log every instruction inside one address range |
| `CYT_DEBUG_MEM=1` | log the first forty out-of-range guest accesses |
| `CYT_DEBUG_QD=1` | log window creation and `DrawPicture` clipping |
| `CYT_DEBUG_FILE=1` | log file creation, Finder metadata and every Standard File answer |
| `CYT_DEBUG_FONT=1` | log the family registry and where each strike came from |
| `CYT_DEBUG_THREADS=1` | thread states, switch counts, scheduler queries |
| `CYT_DEBUG_TICKS=1` | `TickCount` values, every five million calls |

The pairing worth knowing is **`CYT_WATCH_PC` then
`CYT_TRACE_CALLS_FROM_PC`**. The watchpoint answers "does the game reach this
function, and with what arguments", printing the symbol it landed in and the
symbol it was called from — a call trace at one address for almost no cost.
Arming call tracing at the same address then answers "and what does it do
next", which is how `TDelverApp::DoOpen` was found to be calling
`StandardGetFilePreview` through a vtable slot. `CYT_TRACE_RANGE` reads one
function's control flow, which is how `DoOpen`'s early return was read.

### Reading a function before it runs

The watchpoint pairing above answers questions about code the game is already
reaching. `tools/pefdisasm.py` answers them about code it has not reached yet,
which is what you want when implementing a manager the game stops at.

```sh
cd cythera
port/build/cythera build/extract/Cythera.data --dump-glue port/build/glue.txt
python3 tools/pefdisasm.py build/extract/Cythera.data 0x0091A4 +120
```

The second line prints `TDialog::TDialog` with every branch resolved to an
absolute address and named from `cythera_symbols.txt`. The first line is what
makes it worth reading: **every call the application makes to an imported
routine goes through a six-instruction cross-TOC glue stub**, so without the
glue map a disassembly shows `bl 0xc3258` where it means `GetNewDialog`.
`--dump-glue` recognises the stub pattern, resolves each stub's TOC slot to the
synthetic transition vector relocation put there, and writes `address name` for
all 561 of them. `pefdisasm.py` picks the file up automatically.

Everything in §1 below was read this way, before a line of the Dialog Manager
existed. It is much cheaper than guessing and then debugging the guess.

The single most valuable technique remains **letting the game diagnose itself**:
the alert layer resolves `ALRT`/`DITL` resources and `ParamText` into readable
text, so a startup failure prints the game's own sentence. "This file is
corrupted" came free. Keep that working.

The other is **demand-driven implementation**: leave `--stop-after` on, run, and
implement whatever the game asks for next.

## Driving the start screen

The six choices are the game's own `T7Widget` layout, not a `DITL`, so the item
numbers are not the ones in `DITL` 128. Mapped by watching
`TRunStart::DoItemHit` (`CYT_WATCH_PC=0x116480`) with a scheduled click:

| Click | Item | Choice |
|---|---|---|
| `245,118` | none | Onward — no hit; disabled with no player loaded |
| `245,196` | 1 | New Game → `TDelverApp::NewGame` → `StandardPutFile` |
| `245,268` | 2 | Open Game → `TDelverApp::DoOpen` → `StandardGetFilePreview` |
| `500,268` | 5 | Quit — runs the game's shutdown path to `ExitToShell` |

```sh
CYT_WATCH_PC=0x116480 ./build/cythera <args> --click-at-pass 600000:245,196
```

Pass 600000 is comfortably after the screen appears. The right column's other
two items (Preferences, About) are not mapped yet; the same watchpoint will say.

`./drive.sh` is the harness for this: it builds, runs headless with a long
budget, keeps its own support directory between runs so a player file created
by one run is there for the next to open, and prints whatever the game asked
for that is still missing.

```sh
./drive.sh --fresh --click-at-pass 600000:245,196        # New Game
./drive.sh --click-at-pass 600000:245,268                # then Open Game
CYT_DEBUG_FILE=1 ./drive.sh --click-at-pass 600000:245,196
CYT_TRACE_CALLS_FROM_PC=0x115190 ./drive.sh --click-at-pass 600000:245,268
```

## What to do next

### 1. The Dialog Manager, and the List Manager with it

**Partly done.** The Dialog Manager's record and item list are implemented
(`mac/dialog_mgr.cpp`), and so is the Control Manager's state
(`mac/control_mgr.cpp`). Clicking New Game now builds `DLOG` 133 -- the debug
line under `CYT_DEBUG_DIALOG=1` reads *"from DLOG bounds(37,65,440,450) procID
16001, DITL 133 with 13 items, keys \";;Mm;Fm\""* -- walks all thirteen items,
installs a drawing routine on each of the six `userItem`s, and creates four
controls. What it asks for and does not get is now **eight** calls, not twelve:

```
DrawDialog  DialogSelect
LNew  LAddRow  LSetCell  LGetSelect  LSetSelect  LScroll
```

So the List Manager is the whole of what remains for the constructor, and the
two Dialog Manager calls that draw come after it, because what they draw is
mostly lists.

**One thing read from the binary needs correcting.** The note below says the
inline items in `DITL` 133 are created with the *standard* `procID`s 0, 1 and 2
and so have to be drawn here. What the game actually passes to `NewControl` is
`procID` **16000** twice and **16002** twice -- its own `CDEF` 1000, variations
0 and 2 -- matching OK/Cancel and Male/Female. It also passes a null owner and
an empty rectangle, and presumably positions them later. If those really are the
dialogue's four controls, then drawing them is a call into the game's own patched
`CDEF` rather than three shapes reimplemented here, which is both less work and
more correct. **Verify this before writing any control drawing**: run
`CYT_DEBUG_DIALOG=1 ./drive.sh --click-at-pass 600000:245,196` and compare the
four `[Control]` lines against the item list.

Neither manager draws yet, deliberately. Everything on this path happens while
the dialogue is still invisible, and `fillRect`/`frameRect` are file-local to
`mac/qd_draw.cpp` while the text calls take a `TextStyle` and a byte range. A
small drawing interface exported from QuickDraw once, for the Dialog, Control,
List and Menu Managers together, beats four private copies.

The original survey of the work, which still holds:

The full import list is larger and bounds the work: the Dialog Manager is
`GetNewDialog`, `DisposeDialog`, `ModalDialog`, `DialogSelect`, `DrawDialog`,
`GetDialogItem`, `SetDialogItem`, `GetDialogItemText`, `CountDITL`, `ParamText`
and the three alert calls (already implemented as a reporting layer). The List
Manager is `LNew`, `LDispose`, `LAddRow`, `LDelRow`, `LAddColumn`, `LDelColumn`,
`LSetCell`, `LGetCell`, `LAddToCell`, `LGetSelect`, `LSetSelect`, `LScroll`,
`LClick`, `LUpdate`, `LSize`, `LRect`, `LSearch` and `LSetDrawingMode`. The
Control Manager is nineteen calls, all of them ordinary.

**What the dialogue actually is.** `DLOG` 133 is `rect(37,65,440,450)`,
`procID 16001`, initially invisible, item list `DITL` 133:

```
 1 button      (360,240,392,336) 'OK'
 2 button      (360, 90,392,186) 'Cancel'
 3 userItem    ( 90,260,156,342)            <- LNew: the portrait picker
 4 statText    ( 40,270, 81,332) 'Select Portrait'
 5 radioButton (170,270,188,342) 'Male'     <- SetControlValue
 6 radioButton (190,270,208,341) 'Female'
 7 userItem    ( 30, 40,152,230)            <- LNew: nine character archetypes
 8 userItem    (160, 40,223,230)
 9 userItem    (240,120,258,310)
10 userItem    (270,120,333,310)
11 statText    (  8, 49, 24,220) 'Character Archetype'
12 statText    (240, 40,256,115) 'Attributes'
13 statText    (270, 40,286,115) 'Aptitudes'
```

`DITL` 130–132 are an older three-step sequence (Set Scenario, then the
archetype radio buttons, then portrait and sex) that this build does not use.

**`DITL` 131 does not name the archetypes.** An earlier version of this file
said it did. Its twenty items are `Next`, `Cancel`, twelve starting-rank
presets — `Normal Student Apprentice Guard Journeyman Dilletante Fighter Ranger
Bard Mage Hero Legend` — and six difficulty settings, `Standard` through
`Hardest`. Building the archetype list from it would fill the list with twelve
wrong strings, in a dialogue that shows nine.

**Where the four archetype resources actually go.** They are Delver data
resources, read with `../tools/delv_compat.py`, and each one feeds one of the
four `userItem`s in `DITL` 133 rather than three of them feeding nothing:

| Resource | Contents | Item |
|---|---|---|
| 515 (`0x0203`) | the nine names | 7, the archetype list |
| 516 (`0x0204`) | one description per archetype | 8 |
| 517 (`0x0205`) | `Body: 16  Reflex: 16  Mind: 16` | 9, under the 'Attributes' label |
| 518 (`0x0206`) | `Attack[2], Defense[2], Mana[2], Casting[2]` | 10, under 'Aptitudes' |

That is what `AdjustCurArch(0)` is for: it repopulates items 8, 9 and 10 from
the row selected in item 7.

**The acceptance test for the List Manager, exactly.** Resource 515 holds these
nine, in this order, and the list is one column of nine rows built by
`LAddRow(1, i+1, list)` then `LSetCell(...)`:

```
Explorer  Fighter  Swordsman  Beserker  Mage
Wizard    Mystic   Storyteller  Rogue
```

`Beserker` is the game's own spelling [sic] — cytheraguides.com writes
"Berserker". Anything that reads back as twelve rows, or as `Berserker`, is
reading the wrong resource. The portrait list is the other shape and is
confirmed independently: `dataBounds(0,0,3,2)` is three portraits by two sexes,
which is exactly what cytheraguides.com's Archetypes page shows.

(One disagreement worth knowing about rather than resolving: the guide gives
Berserker `Mind 12`, resource 517 says `Mind: 6`. The resource is the game.)

**Four things about this dialogue that will otherwise cost time:**

- **The `DLOG` title is not a title.** Every game dialogue stores something like
  `';;Mm;Fm'` or `'sSyY;cC;DdnN'` there. These are semicolon-separated
  *keyboard equivalents*, one field per item — `sSyY;cC;DdnN` on the save-changes
  dialogue is Save, Cancel, Don't Save. Drawing it as a window title would put
  gibberish on screen. Confirm the indexing before relying on it.
- **`procID 16001` is the game's own `WDEF`.** `16001 = 1000 * 16 + 1`, and
  `WDEF` 1000 is one of the six-byte `JMP $0` placeholders the game patches at
  run time. The Window Manager currently ignores `procID` entirely and draws no
  frame. Calling the game's own `WDEF` is both more correct and less work than
  inventing a frame — and the patch mechanism is now known exactly, below.
- **The `CNTL` resources use the game's own `CDEF`** (`procID 16000` = `CDEF`
  1000). But the buttons and radio buttons in `DITL` 133 are not `CNTL`
  references — they are inline items, so the Dialog Manager creates them with
  the *standard* `procID`s 0, 1 and 2, and nothing in the game's fork draws
  those. Push buttons, check boxes and radio buttons have to be drawn here.
  Note that ovals are currently approximated by their bounding rectangles, so a
  radio button would come out square until that is fixed.
- **`LDEF` 128 is the one piece of genuine 68k**, and it has now been decoded —
  see below. Reimplement its semantics natively rather than emulating 68k.

#### What is already known about it, from reading the binary

None of this has been implemented. All of it was read statically with
`tools/pefdisasm.py` and the glue map (see *Reading a function before it runs*),
so it is what the code actually does rather than what the documentation says.
It should save the next session a day.

**How the game patches its own definition procedures.** `TWDEFRegister` and
`TCDEFRegister` do the same three steps: build a routine descriptor over
`MyWDEF` (`0x06F9B0`) or `MyCDEF` (`0x06E73C`), `GetResource('WDEF', id)`, then
write `0x4EF9` into the first word of the resource and **the descriptor address
into the long at offset 2**, and `HNoPurge` the handle. So calling the game's
own frame or control drawing is: read the long at offset 2 of the resource, and
treat it as a routine descriptor exactly as `resolveUpp` in
`mac/apple_event.cpp` already does. The signature is the classic one,
`(short varCode, WindowPtr, short message, long param)`. `MDEF` 128 is *not*
patched this way — its six bytes are all zero.

**How the List Manager reaches the game's drawing code.** `LDEF` 128 is a
Pascal-convention 68k stub whose whole job is a trampoline, and it does three
things worth knowing:

- Its parameters are the standard `(lMessage, lSelect, lRect, lCell,
  lDataOffset, lDataLen, lHandle)`, and it removes all twenty bytes of them
  itself.
- On `lInitMsg` (0) it **clears `refCon`** and returns; on any other message it
  reads `(**lHandle).refCon` — offset 60 — and, if non-zero, calls it with the
  identical argument list.
- `TListBox::TListBox` (`0x06CB5C`) then fills that in: `LNew(rView,
  dataBounds, cellSize, 128, dialog, drawIt, hasGrow, scrollHoriz,
  scrollVert)`, then `(**list).refCon = NewRoutineDescriptor(MyLDEF, …)`,
  `(**list).userHandle = this` (offset 68), and `(**list).selFlags = 0x80`
  (`lOnlyOne`).

So the port's List Manager never has to draw a cell. It calls `refCon`, and
`MyLDEF` (`0x06D5A4`) does the rest: it ignores messages 0 and 3, reads
`(**lHandle).userHandle` as the client object, and dispatches on message to
vtable slot 64 (`LDEFDraw`) or slot 68 (`LDEFHilite`), passing
`(this, lSelect, lRect, lCell, lDataOffset, lDataLen)`. Nine classes override
`LDEFDraw`; the two this dialogue needs are `TPortraitList` (`0x0A06FC`) and
`TArchetypeList` (`0x0A09E4`).

The one ordering trap: **do not send `lInitMsg` after the caller has set
`refCon`**, or the trampoline will erase it. Zeroing `refCon` inside `LNew` is
equivalent and safe.

**How the Dialog Manager's user items are drawn.** `TDialog::TDialog`
(`0x0091A4`) calls `GetNewDialog(id, nil, -1)`, then `TWindow::Init`, which sets
`windowKind = 31364` and `SetWRefCon(w, this)` — that pairing is how
`TWindow::GetTWindow` finds the C++ object again, so both must be preserved.
Then `TDialog::InstallUserItems` (`0x009240`) walks `1..CountDITL(d)` calling
`GetDialogItem`, and **for every item whose type masked with `~0x80` is zero —
that is, every `userItem` —** calls `SetDialogItem` to store a routine
descriptor over `MyDrawDialogItem` (`0x009348`) as the item's handle.
`MyDrawDialogItem(theDialog, item)` then looks the `TDialog` up by refCon and
calls vtable slot 136, `DialogDrawRoutine(item)`. So `DrawDialog` and
`UpdateDialog` must call a `userItem`'s handle as a Pascal UPP taking
`(DialogPtr, short)`.

**The modal loop.** `TApp::MoveableModalDialog` (`0x00F12C`) calls
`ModalDialog(descriptor(MoveableDialogerRoutine), &itemHit)`.
`MoveableDialogerRoutine` (`0x00EFB8`) is an ordinary `ModalFilterProc`
`(DialogPtr, EventRecord*, short*) -> Boolean`: it handles `updateEvt` for
other windows and `inDrag` on the dialogue itself, and otherwise chains to a
second, application-level filter through `CallUniversalProc`. So `ModalDialog`
needs to fetch an event, offer it to the filter, and fall through to standard
item handling when the filter declines.

**What `TCreatePlayerDialog::TCreatePlayerDialog` (`0x0A0A9C`) actually does**,
in order — this is the acceptance test for §1:

1. `TDialog::TDialog(133)`, then `SetPort(dialog)`.
2. `FaceADialog(dialog)` — `SetPort`, then `BackPixPat(SetTilePat(420))`.
3. `GetDialogItem(d, 3)`, `InsetRect(&box, 1, 1)`, `box.bottom -= 16`, then
   `new TPortraitList(box, dataBounds(0,0,3,2), cellSize, dialog)` — a
   **two-column** list — stored at `this+20`, and its `selFlags` reset to 0.
4. The same for item 7 with `dataBounds(0,0,0,1)` and `cellSize (v=20,h=200)`:
   `new TArchetypeList(...)` at `this+24`, **zero rows to begin with**.
5. Reads its archetype names from data resource 515, and for each entry
   `LAddRow(1, i+1, list)` then `LSetCell(name, len, Cell(i,0), list)`. Note
   `rowNum` is `i+1` against a list that holds `i` rows, so **`LAddRow` must
   clamp an out-of-range `rowNum` to the end** rather than leaving a gap.
   Resources 516, 517 and 518 are loaded the same way but feed no list.
6. `GetDialogItem(d, 5)` then `SetControlValue(handle, 1)` — "Male" on.
7. `ShowWindow(dialog)`, then
   **`FillCRect(&dialog->portRect, SetTilePat(420))`**.
8. `AdjustCurArch(0)`.

**Pixel patterns are no longer optional.** Steps 2 and 7 are the dialogue's
background, and they are the first thing in the game to draw through a `PixPat`
— which the *Known gaps* section below correctly predicted would be where it
first mattered. `SetTilePat` (`0x070FB4`) builds a real one: `NewPixPat`,
`SetHandleSize(patData, 1024)`, a 50-byte `PixMap` template copied in from the
`TOC`, `(**patMap).baseAddr = tileBase + id * 1024`, `BlockMove` of those 1024
bytes into `patData`, `patXValid = -1`, `PixPatChanged`. So it is a **32×32
eight-bit tile** with its own `PixMap` and colour table, and `BackPixPat`,
`PenPixPat`, `FillCRect` and the erase path all need to honour it.

A `DITL` walker already exists: `ditlText` in `mac/core.cpp` parses item lists
for alert reporting and is the right thing to generalise. Static text items can
go straight to `TETextBox`, which wraps. `font_mgr.h` already exposes
`measureText` and `drawTextRun` "shared with the Dialog, Menu and List
Managers", and `qd_region.h` exposes `portClipArea`, `portForeIndex` and
`portBackIndex` — any new manager that draws should use those rather than
re-reading the port, so that it clips and colours like everything else.

Watchpoints for the path, all in `TDelverApp`:

| Watch | Symbol |
|---|---|
| `0x1152A8` | `NewGame` — reached, and now gets past `StandardPutFile` |
| `0x115190` | `DoOpen` — reached |
| `0x113E08` | `NewModel` |
| `0x114400` | `BeginPlay` |
| `0x112E3C` | `InitWorld` — reached, from `DoStartup` |

### 1a. The three record layouts, now confirmed against Apple

`reference/apple-documentation/` has Inside Macintosh, and the three
structures this work invents are all in it. Two were guesses that turned out
right and one had a real bug. **Do not re-derive these.**

**`ListRec`** — *More Macintosh Toolbox*, page 4-110, which prints the offsets
directly rather than making you add up field widths:

```
 0  rView       8   the list's display rectangle
 8  port        4   GrafPtr
12  indent      4   Point
16  cellSize    4   Point
20  visible     8   boundary of visible cells
28  vScroll     4   ControlHandle
32  hScroll     4   ControlHandle
36  selFlags    1   lOnlyOne is 0x80
37  lActive     1
38  lReserved   1
39  listFlags   1
40  clikTime    4
44  clikLoc     4
48  mouseLoc    4
52  lClikLoop   4
56  lastClick   4   Cell
60  refCon      4   <- the game writes its routine descriptor here
64  listDefProc 4
68  userHandle  4   <- the game writes `this` here
72  dataBounds  8
80  cells       4   DataHandle
84  maxIndex    2
86  cellArray   variable, offsets into `cells`
```

88 bytes before `cellArray`. **`refCon` at 60 and `userHandle` at 68 are
exactly what §1 derived by reading the binary** — the reverse engineering and
Apple's documentation agree, which is as much confidence as this is going to
get. `LNew`'s signature matches too: `LNew(rView, dataBounds, cSize, theProc,
theWindow, drawIt, hasGrow, scrollHoriz, scrollVert)`.

**`DialogRecord`** — *Macintosh Toolbox Essentials*, page 6-166: a whole
`WindowRecord`, then `items: Handle`, `textH: TEHandle`, `editField: Integer`,
`editOpen: Integer`, `aDefItem: Integer`. That is what `mac/dialog_mgr.cpp`
already has. `editField` is documented as "editable text item number **minus
1**", so -1 for "none" is right.

**`ControlRecord`** — *Macintosh Toolbox Essentials*, page 5-126:
`nextControl`, `contrlOwner`, `contrlRect`, `contrlVis`, `contrlHilite`,
`contrlValue`, `contrlMin`, `contrlMax`, `contrlDefProc`, `contrlData`,
`contrlAction`, `contrlRfCon`, `contrlTitle: Str255`. Order and offsets as
`mac/control_mgr.cpp` had them, with one real correction: **`contrlVis` is 255
when visible, not 1.** This port wrote 1, which any test comparing against 0
accepts and any test comparing against 255 silently rejects. Fixed. It is the
kind of error that would have surfaced as a control that draws but never
responds, days later and nowhere near its cause.

### 1b. Open Game with a real saved game — DONE, zero calls missing

**Loading an existing save now serves every Toolbox call Cythera makes.** With
`I.M.Cheater` in the support directory, clicking Open Game runs to the
interpreter's 4-billion-instruction budget with **0 distinct, 0 total**
unimplemented calls, and the game has created and erased its main window. The
screen is that window on the desktop dither — nothing of the world is painted,
because painting it is the gameplay rendering path (§5) and none of that has
run yet.

What closed the gap, in order: the List Manager (`mac/list_mgr.cpp`), then
`RGB2HSL`/`HSL2RGB`, `PBFlushFileSync` and `LAddToCell`. The last four were
each a few lines; the List Manager was the work.

Two bugs worth not repeating:

- **`LNew`'s `cSize` is a `Point` passed BY VALUE**, not by pointer. Inside
  Macintosh's C prototype writes `Point *cSize`, transliterating the Pascal
  declaration, but the real convention passes the four bytes in a register.
  Read as an address it produced cell sizes of `-32000x26753`; read correctly
  the game's lists are `303x16` and `1024x16`, which match their display
  rectangles.
- **A selection change is `lHiliteMsg`, not a redraw.** `MyLDEF` dispatches
  message 0 to the client's `LDEFDraw` and message 1 to its `LDEFHilite`, so
  sending a draw where a hilite belongs repaints a cell instead of inverting
  it.

The original comparison and the setup steps follow.

### 1b (original). The route in, and how to reproduce it

**Loading an existing save gets further than creating a character, and needs
strictly less.** `res/user_addons/606_CheaterSavedGame.sit.hqx` unpacks (with
`unar`) to `I.M.Cheater`, a genuine 1999 player file: type `DelP`, creator
`Delv`, a 332 KB Delver Archive in the data fork and a 4 KB resource fork
holding the `PICT` preview, `SCEN` and `pnot` that the delvmod wiki describes.
delvmod reads it: player name `I.M.Cheater`, scenario `Cythera: Fate of
Alaric`.

Dropped into the support directory as `I.M.Cheater.data` / `.rsrc` / `.finf`
(the sidecar is just `DelPDelv\0\0`), Standard File picks it and the game loads
it **without the "This file is corrupted" alert** -- so the archive parses and
the world loads. It then runs 2.46 billion instructions and stops only on the
unimplemented-call limit, deep inside a nested call, with just **four** distinct
calls missing:

```
LNew  LAddRow  LSetCell (x37)  LSetDrawingMode
```

Compare the New Game path, which is missing eight: those four plus
`LGetSelect`, `LSetSelect`, `LScroll`, `DrawDialog` and `DialogSelect`. So this
route needs no Dialog Manager drawing at all, and the thirty-seven `LSetCell`
calls are a list being filled with real game data rather than an empty one being
set up.

To reproduce:

```sh
unar -o /tmp/save res/user_addons/606_CheaterSavedGame.sit.hqx   # decode .hqx first
cp .../I.M.Cheater  port/build/drive/support/I.M.Cheater.data
cat '.../I.M.Cheater/..namedfork/rsrc' > port/build/drive/support/I.M.Cheater.rsrc
printf 'DelPDelv\0\0' > port/build/drive/support/I.M.Cheater.finf
cd port && ./drive.sh --click-at-pass 600000:245,268
```

Remove any zero-byte `Bellerophon.*` from the support directory first, or
Standard File picks that instead -- it is also type `DelP` and sorts earlier.

The screen at the stopping point is the 50% desktop dither, so nothing of the
world has been painted yet; the list is being built before anything draws.

### 2. Menu bar drawing and `MenuSelect`

`MenuKey` already works and dispatches through the application's own handler.
`MenuSelect` returns 0 and `DrawMenuBar` is a no-op, so the menus cannot be used
with the mouse. The contents are already correct in memory — `GetMenu` copies
`MENU` resources almost verbatim. Draw with the system font (family 0, 12pt) and
use `grayishTextOr` (mode 49), which `blitGlyph` already implements, for
disabled items. Note that the game hides the menu bar on the start screen
(`TApp::HideMenuBar`), so this is worth doing after §1.

### 3. TextEdit

Nineteen `TE` calls are imported, including `TEStyleNew`, `TEStyleInsert` and
`TECalText`, so this is narrower than a full TextEdit but wider than the
previous handoff assumed. `TETextBox` already works.

### 4. The gameplay rendering path — described by the person who wrote it

`../reference/community/delver-homepage/Delving into Details.html` is Glenn
Andreas's own account of the Delver engine, and it specifies this section
better than anything reconstructable from the binary. It is much less of an
unknown than the heading below claims.

- **Two things are drawn**: ground tiles on a grid (static -- the map) and
  props in a list (dynamic, and kept in the saved game). Both are built from
  32x32 tiles. Multi-part tiles go up to 64x64 -- a horizontal door is two
  tiles side by side, trees are 64x64. Roughly 8192 tiles exist.
- **Props sort twice**: by X/Y, so a prop anchored south/east of another draws
  after it; then by **layer**, of which there are about half a dozen -- floor,
  shadows/rugs, small/low items, large items, flying objects, high layer --
  drawn bottom up. That is what lets a rug sit on a floor with a key on it
  under a flower pot.
- **Lighting is a post-process, not a draw mode.** A light map is built from
  ambient light (which changes from day to night), the player's spot light, and
  every light-producing object -- and because it is driven by tile flags, both
  props and ground tiles can emit light. The map is a grid finer than the tile
  grid but coarser than pixels. After the scene is rendered, a darkening filter
  is applied per grid cell.
- **Lights flicker by picking randomly between several variations of the light
  map.** That is almost certainly what the 25 `Lite` resources in the
  application's fork are: the variations.
- **Palette animation** does lava, water waves and pulsing magic by cycling the
  colours at certain indices. This port stubs `AnimatePalette` as a no-op, and
  that has to change before any of those look right. `SetEntries` is already
  real, and the fade leans on it heavily.
- Some tiles animate on their own, separately from palette animation.

Two other post-process effects existed, including a greyscale view that with
line-of-sight disabled gave an x-ray effect.

#### The original survey of this section


None of it has executed: tiles, sprites, the map viewer, the lighting behind the
25 `Lite` resources. This is the widest confidence interval in the project.
Watch for heavy `CopyMask`/`CopyDeepMask` use with real masks (currently they
alias plain `CopyBits` and ignore the mask), transfer modes beyond copy and XOR,
and any direct framebuffer access that bypasses the `PixMap` abstraction. The
first thing it will want is **pixel pattern tiling** — see the gaps below.

### 5. Audio, then packaging

`SndNewChannel` reports `notEnoughHardwareErr` and the Tune Player is a silent
sink. Route `SndPlayDoubleBuffer` and the tune sequencer to SDL audio;
`grimoire/utilities/qtma2midi.py` already decodes the QuickTime Music tune
format — the game's score. Then a signed `Cythera.app`, saves under
`~/Library/Application Support`, and first-run extraction from the user's copy.

## Known gaps and shortcuts

- **Standard File opens no chooser.** A save is accepted under the name the
  caller suggested; an open returns the first file of an acceptable type in the
  support directory, alphabetically. There is no way for the user to decline, or
  to choose between two saved games. That is deliberate — it is what makes the
  path testable headlessly — but a native chooser behind a flag is the obvious
  next refinement.
- **A file filter procedure is never called**, and the dialogue hooks
  `CustomGetFile` and `CustomPutFile` take are ignored, because no dialogue
  opens.
- **Finder metadata is a sidecar.** Ten bytes in `<name>.finf` beside the data
  fork. A file with no sidecar — anything extracted before this existed — is
  reported as Cythera's own document type.
- **`CloseResFile` does not close.** It commits the file's bytes but leaves it
  in the search order, because nothing in this port reopens a resource file
  within a run and the game reads its preferences back through the same handles.
- **Pixel patterns are allocated but not drawn**, and this is now blocking §1.
  `NewPixPat` builds a real `PixPat` record — the application resizes
  `(**pp).patData` and writes through `(**pp).patMap` itself, so a null handle
  sent both into unmapped memory — but `BackPixPat` and `PenPixPat` still record
  nothing and the fill path ignores colour patterns. Tiling was left rather than
  guessed because nothing had drawn through one yet. Something now has: the
  character-creation dialogue paints its background with `BackPixPat` and
  `FillCRect` over a 32×32 eight-bit tile built by `SetTilePat` (`0x070FB4`).
  The fix is the one already sketched — extend `PenPattern` in `mac/qd_draw.cpp`
  with the tile's pixels mapped into the destination palette through `ColorMap`,
  and have `fillRect` index it by `x % w`, `y % h` — but it now has a caller to
  verify against, and §1 describes the record it has to read.
- **Apple event parameters are all reported absent.** Correct for the launch
  event, which carries none — it is how `CheckAppleEventForMissingParams`
  concludes that nothing is missing. Opening a document by dropping it on the
  application would need `AEGetParamDesc`, `AECountItems` and `AEGetNthPtr` to
  be real, and `HandleOpenDocuments` is already in the binary waiting for them.
- **A white rectangle sits at the bottom left of the start screen.** Small, and
  present since the screen first drew. Not diagnosed.
- **Chicago and Espy Sans are substituted by Geneva.** The port's one real
  text-fidelity compromise, kept in one table (`kHostMappings` in
  `mac/font_mgr.cpp`). Over a full start-up the game builds eleven strikes and
  only Chicago 12 is substituted.
- **Seldane is not a text face.** It defines twenty-six uppercase runes and
  nothing else — no lowercase, no space — so any other string set in it comes
  out as missing-symbol boxes. That is correct behaviour.
- **Underlines do not break around descenders.**
- **Regions** are stored host-side as disjoint rectangle lists and exposed to the
  guest as a ten-byte rectangular region holding the bounding box. Exact for
  everything Cythera does, but code that walked a region's scanline data would
  not see the real shape.
- **Ovals, arcs and polygons** are approximated by their bounding rectangles.
  §1 will want real ovals for radio buttons.
- **`SetThreadScheduler`** records the game's custom scheduler but keeps
  round-robin order.
- **`CopyMask`/`CopyDeepMask`** ignore the mask argument.
- **Cursors** are recorded but not drawn; the host cursor shows through. The
  start screen calls `SetCCursor` once per event-loop pass, so this will be
  visible as soon as the game changes cursor over a widget.
- **~30% of the code section is not covered by the symbol table**, so the opcode
  census may under-report. The interpreter halts loudly with a disassembly and
  symbol on an unknown opcode.
- **`TickCount` reads the host clock on every call** — better than a million
  times a second when the game idles, about two percent of run time. The cache
  that used to sit there was keyed on the guest instruction count, which never
  repeats between two calls, so it never hit; and a cache keyed on anything the
  guest does cannot know whether the clock has moved. Left honest rather than
  fast.

## Cross-checks against the rest of this repository

This port now lives beside a static site that reads the same files from the
other direction: `../js/mac-resfork.js` and `../js/mac-rsrc-types.js` decode
classic-Mac resource forks, PICT and NFNT in JavaScript, and `grimoire/utilities/`
holds the harness that exercises them. Two independent implementations of one
undocumented format are worth more than either alone, and the site's own
harnesses say so about delvmod: a snapshot hash proves a decoder is
*unchanged*, never that it is *right*.

There is now a third implementation beside these two: the systemless fork's
68K trap work covers List Manager scrolling, `StyledLineBreak` /
`VisibleLength`, the PixPat family and visRgn hole clipping — the same calls
this port serves at `list_mgr.cpp:257-616`, `font_mgr.cpp:959-1001` and
`quickdraw.cpp`, proven against the other slice of the same binary.
`../SYSTEMLESS.md` § "What each tree can lend the other" is the full map,
including the most direct payoff: this port's `tb.add(...)` handlers are
PowerPC-ABI worked answers for the 72 `InterfaceLib` imports missing from
systemless's `src/loader/ppc.rs`, which is what halts its PowerPC slice at
3,446 cycles.

Three differences are known between this port and the site's JavaScript, and
they are written down here so the next session can use them rather than
rediscover them.

- **PICT `0x0090` / `0x0091`.** `src/mac/pict.cpp` renders uncompressed
  BitsRect and BitsRgn; the JavaScript at `../js/mac-rsrc-types.js` walks past
  them to keep its opcode stream aligned and renders only the first *packed*
  image opcode it reaches. A picture built from several `CopyBits` therefore
  draws in full here and partially there. No longer an even split:
  systemless's `src/trap/pict.rs` renders them too, so two of three
  implementations agree and the JavaScript is the one to fix, with two
  references for how.
- **PICT `0x8200` / `0x8201`.** The reverse: the JavaScript handles
  QuickTime-compressed opcodes, and `pict.cpp` throws on them. Nothing in the
  21 pictures this port has verified against uses one, so it has not mattered
  yet.
- **NFNT metrics.** `src/mac/font_mgr.cpp` parses the offset/width table, with
  the two traps that cost the most time already paid for: it has one fewer
  entry than the location table, and `owTLoc` is a word offset measured from
  its own field, storing a bearing relative to `kernMax` rather than to the
  pen. The JavaScript reads `owTLoc` and never uses it, so it shows glyph
  images with no advance width or bearing.

Going the other way, `src/resfork.cpp` is the only resource fork *writer*
either side has, and the site's stated direction is reading "and eventually
editing".

## Layout

```
port/src/mem.*            guest address space, big-endian accessors
port/src/pef.*            PEF loader: sections, pattern data, relocation, imports
port/src/ppc.*            PowerPC interpreter, nested guest calls, watchpoints,
                          range tracing, address-armed call tracing, the overall
                          instruction limit
port/src/resfork.*        resource fork reader and writer
port/src/toolbox.*        import dispatch, Gestalt, call tracing and statistics
port/src/mac/             one file per manager (see README for the full list)
port/src/mac/apple_event.cpp  the launch event and Apple event dispatch
port/src/mac/standard_file.cpp  Standard File, answered from the support directory
port/src/mac/file_mgr.*   host directory mapping, forks, Finder metadata
port/src/mac/resource_mgr.cpp  resource reading, and the writable overlay
port/src/mac/font_mgr.*   FOND/NFNT parsing, strike cache, style synthesis,
                          measurement and the text drawing calls
port/src/host/hostfont.*  CoreText rasteriser for outlines and host families
port/src/host/display.*   SDL2 window, framebuffer presentation, input translation
port/tests/font_test.cpp  resolves every TxSt style; one smoke invariant
port/tests/resfork_test.cpp  round-trips the fork writer; one smoke invariant
../tools/                 PEF, resource and opcode analysis; screen_to_png.py;
                          pefdisasm.py, which reads any function statically.
                          Copies -- see tools/COPIES.txt
../reference/game/        the two .hqx archives this port reads

Not in this repository, in siblings checked out beside it:

grimoire/utilities/       the site's harnesses and converters, including
                          qtma2midi.py for the tune format
grimoire/js/              a second, independent PICT / NFNT / resource-fork
                          decoder in JavaScript -- see "Cross-checks" below
grimoire/delvmod/         data-format library (modern-Python shims live in
                          ../tools/delv_compat.py)
```

Shared drawing state worth knowing about: `portClipArea`, `portForeIndex` and
`portBackIndex` (declared in `mac/qd_region.h`, defined in `mac/qd_draw.cpp`)
are the whole of what a drawing operation needs from a port. Text uses them, so
it clips and colours identically to shapes. Any new manager that draws should
use them too rather than re-reading the port.

## The start screen: what is fixed and what is measured

**The torch is fixed** -- it was six 32-bit direct-colour pictures whose pixels
were being reduced to a luminance and then used as a palette index. See the
commit; the decoder now keeps true colour and matches it to the screen's table
at draw time.

**Quit works, in every test available here.** A click at `510,268` on the start
screen reaches `ExitToShell` through the game's own widget layer -- headless,
and with the release delivered instantly as well as after the usual hold, so it
does not depend on the button being observed as held. What could *not* be
tested here is a real windowed click: launched from a non-interactive shell the
SDL window receives a genuine `SDL_QUIT` about three seconds in and the run ends
with "the window was closed", with nothing having closed it. That is macOS
declining to keep a non-bundled binary alive, not a port bug, but it does mean
windowed interaction has to be checked by hand or from a real `Cythera.app`.
`Display::pushHostClick` exists for that: with `CYT_SDL_CLICK=1`,
`--click-at-pass` drives genuine SDL button events through
`SDL_RenderWindowToLogical` and `handleInput` rather than the queue behind them.

**The fade now works** -- see the section below, which is kept because the way
it was found is the point. Measured across the transition, mean screen
brightness now runs 122.7 -> 1.9 (fully black) -> 11.3 -> 23.9, where before it
went 232.5 -> 52.8 with nothing in between.

**Budget note, which bit immediately:** the fade spends real time, so an
instruction budget now buys far fewer event-loop passes -- `smoke.sh` reports
about 571,000 where it used to report 940,000. `--click-at-pass 600000` is
consequently much closer to the edge than it was. If a scheduled click stops
arriving, raise the budget rather than assuming the click is broken.

### How the missing fade was found (kept for the method)

**The fade between the Ambrosia logo and the Delver splash did not happen, and
this was measured rather than guessed.** Dumping the screen at 0.3, 0.7, 1.1 and
1.5 billion instructions gives mean brightness 232.5, 52.8, 52.8, 53.5: the
white logo, then the splash already at full brightness, unchanged across the
whole period the fade should occupy. It cuts rather than fades.

The useful part is *why nothing was found*. During that period the game makes
**no palette, gamma or colour-table Toolbox call at all** -- a full call trace
over the window shows only five `SetPalette`, three `GetNewPalette`, two each of
`NewPalette`, `CTab2Palette` and `ActivatePalette`, all one-time setup, and
nothing repeated. A fade would call something hundreds of times. So Cythera's
`GammaFadeIn` is reading state directly out of a structure this port fills in,
finding it unusable, and returning without ever asking the Toolbox for
anything. Two candidates were checked and one is eliminated:

- **`gdRefNum` is -1**, the "no driver" sentinel, and a gamma fade drives the
  video driver. Setting it to a plausible driver number produced **no new
  Toolbox calls** -- `Control` and `PBControl` are unimplemented, so a call
  would have shown up in the missing list, and none did. Not this.
- **`gdITable` (GDevice offset 6) is left null**, because the handle is
  allocated cleared and nothing writes it. Still a candidate, and untested.

That is exactly where it stopped being guessable from the outside, and reading
`IsOneGammaAvailable` statically settled it in minutes. **`reference/cythera_symbols.txt`
is in the repository now**, and `tools/pefdisasm.py` finds it there.

What it said (0x074F28):

1. `NGetTrapAddress(0xAA29)` against `NGetTrapAddress(0xA89F, _Unimplemented)`;
   equal means no gamma support, and it returns false without doing anything
   else. **This is what the port failed**, because it answers every
   unimplemented trap with one shared address.
2. `TestDeviceAttribute(gd, 13)` screenDevice, or `(gd, 14)` noDriver -- passed.
3. `(**gd).gdType` must not be 1 -- passed.

So `gdRefNum` was never the problem: the function never got as far as the
driver. `mac/video_driver.cpp` now answers trap 0xAA29 as present -- safe
because the game only ever *compares* the address and never calls through it --
and implements what it does call:

| Function | Call | csCode |
|---|---|---|
| `GetDevGammaTable` (0x075544) | `PBStatusSync` | 8, `cscGetGamma` |
| `SetDevGammaTable` (0x075624) | `PBControlSync` | 4, `cscSetGamma` |

**The one thing that is not what the documentation implies**, and which cost
the most time: `GetDevGammaTable` does `stw 29, 28(31)` -- it puts its *own
output pointer* in `csParam` and expects the driver to write the gamma table
pointer **through** it, not into `csParam` the way a plain `VDGammaRecord`
would be filled. Filling `csParam` alone leaves the game with an all-zero
table, which it then hands back to `SetDevGammaTable` 494,140 times without
ever changing a value -- a fade that runs perfectly and does nothing. The port
writes both places now.

The ramp is applied in `Display::present`, and also to the palette written by
`--dump-screen`, so that a headless dump taken mid-fade looks like what a
person would have been seeing. Without that second one the fade cannot be
verified headlessly at all, because nothing headless reaches `present`.

**Music is not a bug, it is item 6.** `SndNewChannel` reports
`notEnoughHardwareErr` and the Tune Player is a silent sink by design; the
start-up calls `SndNewChannel` three times and gets nothing. Making it play
means routing the QuickTime Music Architecture and `SndPlayDoubleBuffer` to SDL
audio, and QTMA is a synthesiser -- note events against instrument voices, not
sample playback. `grimoire/utilities/qtma2midi.py` already decodes the tune format
and `grimoire/utilities/midi2wav.py` renders MIDI, so the cheapest first version is
probably to decode the score to MIDI and play it with a small software
synthesiser rather than to implement QTMA note-for-note.

## The engine's own GUI model, from the person who decoded it

Worth knowing before any more of the Dialog, List or Control Managers is
written, because it says the awkward part is inherent rather than a porting
mistake.

Bryce Schroeder, reverse-engineering the scripting system, described Delver's
GUI as "modelled on classic Mac OS rather than modern callback-driven systems:
a script that opens a modal window runs its own event loop, spinning until it
gets a response", and judged that "a complication rather than a fundamental
problem for a reimplementation"
(`../reference/community/forum-writing/CYTHERA-COMPENDIUM.md`, "DELVER GUI MODEL",
`[BryceSchroeder@t2020]`).

That is exactly the shape this port already found from the other direction: a
nested guest call can legitimately run for the rest of the program, which is
why the instruction budget moved into the interpreter and why scheduled input
is delivered at the frame boundary. So `ModalDialog` spinning its own loop is
the design, not a symptom, and it will happen again inside script-driven
windows once gameplay runs. Do not try to flatten it.

One more from the same source, relevant to the File Manager: **the game will
not boot without a resource fork** on its data file — modders working on the
data fork directly have to copy the resource fork across with ResEdit. The port
reads both forks already; this is why that matters.

## Three start-screen bugs still open, with what is ruled out

Reported from a real windowed run. None is fixed; each has evidence attached so
the next attempt does not start from zero.

### Clicks still do nothing — the Retina fix was not it

`SDL_WINDOW_ALLOW_HIGHDPI` really does mean SDL reports mouse positions in
window points while the renderer measures in backing pixels, and that really
was wrong, so scaling points up before `SDL_RenderWindowToLogical` is a
correct change. **It did not fix the symptom.** Clicking still produces no
highlight, no cursor change and no action.

That "no cursor change" is the useful part and it narrows things: the game
polls `GetMouse`, `FindWindow`, `GlobalToLocal` and `SetCCursor` roughly 2.8
million times over a start-screen run, so it is continuously asking where the
pointer is. Something is answering wrongly, or the answer is not reaching the
widget layer. Scheduled clicks work throughout, which means the event *queue*
and the game's own dispatch are fine -- `--click-at-pass` hands the game
logical coordinates directly and never goes near the renderer.

**The next step is data, not another hypothesis.** Log `mouseH_`/`mouseV_` on
every `SDL_MOUSEMOTION` and every `GetMouse`, run windowed, wave the pointer
over the Quit sign, and see what the game is being told. If the numbers are
right, the bug is above the Display layer.

### The Ambrosia logo has a white surround, and should be black

At 250 million instructions the screen is 73% **index 0**, and index 0 in the
palette the game installs is white. The port never fills the backdrop, so what
shows is whatever the framebuffer held -- zeroed bytes, which are index 0.

Not yet established: whether the real game paints that area black explicitly
(and the port is dropping the call) or relies on the screen already being
black. `FillRect`, `FillCRect` and `PaintBehind` are all called during
start-up and are the places to look first.

### The colours are close but not right — the mechanism, found

**The start screen's artwork and the screen's colour table are from different
palettes, and that is not a port bug so far as anyone can yet show.**

Measured:

- The 640x480 background picture carries its own 224-entry colour table.
  **223 of those 224 colours exist in `clut 256`** (equivalently in
  `js/delv-graphics.js`'s `PALETTE`, which is the same family) and **6 of 224
  exist in `pltt 130`**. It is `clut 256` artwork.
- At start-up the screen's colour table *is* `clut 256` -- the port builds it
  that way. The game then calls `SetPalette` with `pltt 130`, four times, and
  from then on the screen table is `pltt 130`, which the port reproduces
  exactly, all 256 entries.
- The game also calls `SetEntries(0, 255, ...)` 161,035 times, and **the source
  table it passes is the screen's own `ctTable`** -- the argument is literally
  `ctab + kCtTable`. On real hardware that is how you push a colour table out
  to the video card, so a program that scales the table in place and calls
  `SetEntries` is fading the display. In this port the framebuffer is read
  through that same table every frame, so the copy changes nothing, correctly.
  It is therefore *not* the last effective write; `SetPalette` is.

So the port installs the palette the game asked for, and then colour-matches
224-colour artwork into a table holding six of those colours. Every pixel of
the start screen is a nearest-match approximation, which is exactly the "a bit
off" that was reported.

**What is not settled is what the original does instead**, and there are two
candidates worth testing in order:

1. **The match itself may be poor.** Classic QuickDraw matches through the
   GDevice's *inverse table* (`gdITable`), which this port leaves null, using a
   naive nearest-RGB search instead. An inverse table is not merely an
   optimisation -- it encodes which entries are eligible. Building one, or at
   least excluding entries the palette marks unusable, may be the whole fix.
2. **The order may be wrong.** If the original draws the picture while
   `clut 256` is still installed, its indices land ~1:1, and the later switch
   to `pltt 130` recolours the *stored indices* -- because a real framebuffer
   holds indices and the CLUT is a live lookup. This port matches at draw time
   and bakes the result in, so a later palette change cannot recolour anything
   already drawn. That difference is invisible until a program changes palettes,
   and this one changes them constantly.

The second is the more likely and the more consequential: the engine uses
palette animation for lava, water and pulsing magic (see section 4), and none
of that can work at all while colour matching is baked in at draw time.

### Why `reference/screenshots/Cythera-title.png` cannot arbitrate this

What is established, and can be trusted:

- The port's runtime colour table matches **`pltt 130` exactly, all 256
  entries**. Palette loading, `SetPalette` and `SetEntries(0, 255, ...)` are
  not the problem.
- The gamma ramp is back to **identity** by the time the start screen is up,
  so the fade is not leaving the screen dimmed.
- The 640x480 background picture carries **its own 224-entry colour table**
  that shares exactly **1 of 256** entries with `pltt 130`. So every pixel of
  the start screen goes through colour matching, and the quality of
  `ColorMap`/`nearestIndex` decides how the whole screen looks. That is the
  most likely place for the remaining error.

Its 98 colours match **3 of 98** against `pltt 130`, 3 against `clut 256` and 4
against `PALETTE` -- but every one of them is a *near* miss: the median
single-channel error to the nearest `PALETTE` entry is **9**, the maximum 30.
That is the signature of a colour-managed screen capture, not of a different
palette. The image is right about what the screen looked like and wrong about
the exact values, so it is good evidence for "this should be a dark red" and
useless for "this should be index 74". **Do not use it to verify a palette
fix; use `clut 256` and the picture's own table, which are exact.**

## Where systemless already gets to, tested

Run against `../reference/game/installed-folders/Cythera Installed Folder.sit` -- a StuffIt of the whole
installed folder, which is the form systemless needs, because it populates its
VFS from the archive and cannot see a sibling file on disk. Handed the bare
application it fails identically whether or not `Cythera Data` sits next to it:
*"Sorry - there has been a fatal error: Unable to refer to scenario data"*.

With the archive it reaches the start screen. What works and what does not, as
observed:

| | systemless | this port |
|---|---|---|
| Start screen | yes | yes |
| Torch colours | correct | correct (fixed here first) |
| Splash fade | — | yes |
| `About`, `Quit` | work | Quit reaches ExitToShell |
| "No Player Selected" / "Not Registered" text | drawn | **not drawn** (a white box) |
| `Open Game` | events, nothing on screen | loads a real save, 0 calls missing |
| `Preferences` | dialog opens, then freezes | — |
| `New Game` → name → character creation | works | reaches the dialogue, no drawing |
| Archetype list (change class) | works | `LSetCell` implemented, undrawn |
| Gender radio button | **does not work** | `SetControlValue` implemented |
| Portrait picker | **does not work** | two-column `LNew` understood |
| Intro text after OK | background appears, text never dissolves in; halts on `[TRAP] ScriptUtil: unhandled encoded selector $84080028; popped 8 arg bytes` | not reached |

**The gaps are complementary, which is the useful part.** Cythera imports no
Script Manager routine at all -- `pefdump.py --symbols` shows `CharExtra`,
`CharWidth`, `DrawText`, `GetFontInfo` and nothing selector-based -- so that
trap is systemless reaching its own internal dispatcher, not the game asking
for something exotic. And the two things it cannot drive in character creation
are the two this tree has already worked out from the binary: the gender
buttons are inline `DITL` items the game creates with `procID` 16000/16002, its
own patched `CDEF`, and the portrait picker is `LNew` with
`dataBounds(0,0,3,2)` -- three portraits by two sexes -- with the client's own
`MyLDEF` installed in `refCon` and the trampoline in `LDEF` 128 skipped. Both
are written up in section 1 above.

That is worth more to systemless than another manager is to this port.

## Prior art: two projects doing the same job

Worth knowing before writing another manager by hand.

- **`benletchford/systemless`** (Rust, GPL-3.0, active, ~920 commits) is the
  same idea as this port and further along: a ROM-free, System-free high-level
  runtime that executes **both 68K and PowerPC** Mac binaries against a Toolbox
  reimplemented in native Rust, with Cranelift JIT. It already has Memory,
  Resource, File, QuickDraw (PICT, CopyBits, GWorlds, colour tables), Event,
  Menu, Window, **Control, Dialog, TextEdit**, Cursor, Process, **Sound** and
  Thread. It runs Marathon, Escape Velocity, SimCity 2000 and fifty-odd others
  in a browser at systemless.org, and it takes a user-supplied application:
  `systemless path/to/application.sit`. **Nobody here has tried Cythera on it.**
  That is the single cheapest experiment available: either it runs, and most of
  this port's remaining work is answered, or it fails somewhere specific and
  the failure is worth more than a guess. GPL-3 means its code cannot be lifted
  into this tree without relicensing, and it is Rust against this C++ anyway --
  it is a reference and a yardstick, not a parts bin.

- **Executor / ROMlib** (`autc04/executor`, a modern fork of `ctm/executor`) is
  **MIT-licensed**, so it can be borrowed from without conditions (and since
  this repository moved to GPL-3.0-or-later, GPL sources such as systemless
  may now be ported from too — see `../CLAUDE.md` § Licensing).
  ROMlib is a Toolbox implementation written from Inside Macintosh and checked
  against real hardware, so it covers the List, Dialog and Control Managers
  this port has been writing from scratch. It is **68K only**, so it cannot run
  Cythera's PowerPC binary -- its value is entirely as a reference for
  behaviour that Inside Macintosh describes ambiguously.

Neither was consulted while the Dialog, Control and List Managers were written
here. That is worth fixing before the next manager rather than after.

## Two things not to change

`Gestalt` answers steer which code paths the game takes for the entire run.
Report present **only** what is actually implemented; `gestaltUndefSelectorErr`
makes the game fall back to code it already carries. Saying "present" for
something unimplemented is much worse than "absent" — it will then call it. The
same logic applies to `NGetTrapAddress`, which must return one shared address for
every unsupported trap so availability checks fail correctly.

**Only files under `--support-dir` may be written.** The File Manager's delete
path, the resource fork writer and the Finder-metadata sidecar all check it
independently. The extracted originals are the only copy of the game's data that
this tree has, and no permission the game asks for should be able to reach them.
