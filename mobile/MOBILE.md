# mobile.html — what was measured, and what must not be undone

`CLAUDE.md` § `mobile.html` carries the embed contract (`paused`, no volume,
messages before `emulator_loaded`, `pressKey`/`releaseKey`, the gesture table,
`EMBED_ORIGIN`) and the reason a local file cannot be handed to the emulator.
This file is the rest: everything the export-and-install work established by
running it, kept out of `CLAUDE.md` so that sessions on the other pages do not
load it. Read it in full before touching `mobile.html`, `coi-serviceworker.js`,
the installer script in `js/mac-hfs.js`, or the patch path in `explorer.html`.

**This was re-checked against infinite-mac's source, not taken on trust, and
it holds.** `Mac.tsx`'s message listener sends `emulator_load_disk` straight to
`getCDROMInfo(url)`, which for any URL outside its own library does
`fetch("/CD-ROM/" + btoa(url), {method: "PUT"})` — its worker, not the browser.
There is a client-side path in there (`fetchClientSide`, which fetches into a
Blob and hands the worker a `blob:` URL) but only `fetchCompressedCDROMInfo`
sets that flag, for archive.org, and it is reached through the same server
round trip. `handleDrop` is the only entry a local `File` has, and it is the
iframe's own event.

**The way in on a phone is not a file at all — it is a patch typed as
keystrokes.** Cythera's resource cipher is a position-indexed keystream XOR
(`decryptResource` in `js/delv-archive.js`: the key evolves from the resource
id and the byte index and never from the data), so editing plaintext changes
the ciphertext at exactly the same positions and nowhere else. An edit that
keeps a resource's length is therefore a few (file offset, old byte, new byte)
triples — `summer` → `bummer` in the opening dream is **one byte**, `f0fa:5b:4a`
— which fits in a URL. `buildResourcePatch` in `explorer.html` computes it
against `PRISTINE_BYTES` (the archive as it first arrived, because an edit
relays the whole file and no later offset agrees with anyone's installed copy),
and `mobile.html?patch=…` types an AppleScript that writes it into the game's
own `Cythera Data`. Nothing crosses the file system, so nothing depends on a
picker, a drag, or a disk. It is the only route that works on a phone
unassisted, and it has been read back off the emulated screen.

Four things about it were settled by running a reconnaissance script in a real
Mac OS 7.6 rather than assumed:

- **Script Editor is at `Macintosh HD:Apple Extras:AppleScript™:`** — the
  folder's name really does end in a trademark sign, which is why naming it
  fails and why the Finder's type-select (a prefix) is what reaches it.
- **`write … starting at N` is one-based.** A patch offset is an ordinary
  0-based file offset, so it goes out as `offset + 1`. Getting this wrong
  writes to the byte before the one meant, silently.
- `open for access` / `write` / `read` / `close access` / `set eof` all work.
- **`display dialog` refuses a string of 256 characters or more**, which is why
  what the script reports is short.

**Bytes travel as hex, and the typist pauses.** Spelling each byte as
`(ASCII character 74)` costs 46 typed characters per patched byte once the
check and the write are counted; a hex pair costs two, with a fixed ~400-
character decoder. That is the difference between a portrait taking six hours
and forty minutes. The decoder accumulates into a *list* and coerces once —
AppleScript strings are immutable, so `d to d & c` in a loop is quadratic, and
on a 68K that is seconds against minutes.

**And typing has a speed limit that is not politeness.** infinite-mac's
`updateInputBufferWithEvents` carries one key event per sync — *"currently only
one key event can be sent per sync"*, says its own comment — and writes the
code into a single slot with the flag set, unconditionally. Nothing is dropped
on the browser side, but if the guest has not polled since the last key, the
next one overwrites it and that keystroke is gone. A file name is short enough
that it never showed; a three-thousand-character AppleScript showed it at once,
with `permission` arriving as `pssion` and the compile failing on nonsense. So
`typeAtTheMac` breathes — 260ms every 40 characters and 160ms at every newline
— which costs about a quarter of the time and buys a script that is what was
sent. Measured end to end, typing runs at roughly **210ms per character**
(a shifted character is four key events, not two), so budget from that and not
from the nominal 60ms gap.

A patch too long for one pass is split, and each pass is a whole script:
Command-A then the next one, so typing replaces rather than appends. A run is
never split across passes — the read, the check and the write for one run have
to compile together.

`try` needs its `on error` here exactly as the installer's script does — it was
written without one and failed to compile with `Expected "on" but found "end"`
after every other step had worked, which reads as the patch being wrong rather
than the script. `mobile_install_check.mjs` now pins that for both scripts, and
pins the one-based offset and the old-byte check with it. The script verifies
every old byte before writing any of them: a patch is only valid against the
file it was diffed from, and one read proves it. It ends by opening Cythera,
because otherwise it leaves someone in Script Editor with an unsaved document
whose Command-W puts up a Save dialog about something they never asked for.

**The file that goes in is a zip, not a disk image, when size matters.**
`uploadsFromFile` checks two conventions before anything else, and the first is
infinite-mac's own: a `.zip` with `.rsrc/` and `.finf/` directories **at the
root** — Basilisk II's ExtFS layout, where the data fork is the plain entry, the
resource fork is the same name under `.rsrc/`, and `.finf/` holds 32 bytes of
Finder info (FInfo 16, FXInfo 16). Both forks and the type/creator survive.
`.hqx` is not one of the two: BinHex would land in Downloads still wrapped and
need StuffIt Expander, which is a step added rather than removed.

Measured on the real forks, the zip is **3.6 MB against the disk image's
7.9 MB** — less than half, which on a phone is the difference between fine and
annoying, and it keeps the HFS writer and its modal desktop-file alert off the
path entirely. `buildZip` in `js/mac-export.js` grew a `deflated` field for it:
the caller compresses with `CompressionStream('deflate-raw')` and passes both,
because the header still needs the original size and CRC. Omit it and the entry
is stored, exactly as before — the galleries it was written for hold PNGs and
WAVs, which do not compress.

What the zip cannot do is arrive somewhere the Finder reaches in one step: the
contents land in The Outside World, under Downloads, in a folder named after
the zip. So its installer is **typed** rather than opened — the script searches
for the folder instead of navigating to it, which costs about two minutes and
depends on nothing being where it was last time.

**A file, when there is one, goes in through infinite-mac's own file picker,
and one query parameter is what makes it reachable.** `Mac.tsx` builds a **Load File**
control that calls `input.click()` on a `<input type="file">` — the system file
picker, which on iOS and Android is the Files app. On the bare `/embed` route
that control is in the DOM at zero size: `screenSize` is the string `"embed"`,
which puts its `ScreenFrame` in fullscreen-bezel mode, and
`.ScreenFrame-Fullscreen` sets `--screen-controls-display: none`. Passing
`screenSize=WIDTHxHEIGHT` instead makes `screenSize` an *object*, the bezel an
ordinary one, and the control real and tappable. (On touch it is always
visible, because the rule that hides controls until hover is inside
`@media (hover: hover)`.)

So `mobile.html` asks for `screenSize` at boot, and the **File** button hands
the window to the emulator's own page for as long as it takes to tap that
control. Nothing reloads, so nothing running is lost — which is the whole
reason the parameter is set at boot rather than when the button is pressed.

**The layout that parameter brings with it is constant, not something to
measure at run time.** Across six iframe sizes the `ScreenFrame` is always
`(w+22) × (h+22)` with the screen exactly 11px inside it, left-anchored and
vertically centred (so slack below the frame pushes everything down by half of
it), and the control row hangs 18px below the screen and is 15px tall. So the
iframe is made frame-sized and pulled up and left by `FRAME_INSET = 11`, which
puts the emulated screen back on `#scale-target`'s own origin and leaves every
touch coordinate exactly as it was; `overflow: hidden` clips the control row
until file mode wants it.

**File mode magnifies a strip of that row rather than showing the emulator.**
Scaling the whole frame to fit was the obvious thing and it was wrong twice
over: the screen is rendered at its own size, 640×1385 on a phone, so fitting
it made the control about nine pixels tall — unreadable — and put it within a
few pixels of the bottom edge, under the browser's own toolbar. The iframe is
scaled *up* instead (1.4–3.2, capped so `Load File` stays on screen
horizontally), translated so the row lands in a strip clear of the bottom, and
`clip-path` shows only the row — which clips hit-testing too, so taps outside
it fall through to the panel behind. The whole-emulator view is kept as an
escape hatch, because the strip depends on measured constants of someone else's
UI.

**Two CSS traps had to be cleared first, and both fail silently.**
`position: fixed` is positioned against a transformed ancestor rather than the
viewport, and `#scale-target` carries the fit-to-window scale while
`#pan-zoom-container` carries `will-change: transform`, which is enough on its
own to make a containing block — so the strip was scaled a second time by the
page's own factor and landed 270px above where it was aimed. And
`#emulator-wrapper` has `z-index: 1`, making it a stacking context, so the
iframe's own `z-index` cannot lift it above anything outside that wrapper: the
panel pointing at the control was painted over it. File mode clears the two
transforms and raises the wrapper, and `mobile_handoff_check.mjs` pins all of
it.

**Everything else is a way of getting a file into this page's hands**, which is
a different and lesser thing: `?handoff=1` collects a disk `explorer.html` left
in IndexedDB, a file dropped anywhere but the screen is caught rather than
thrown away, and the card's own picker takes one from the device. All three end
at the same card, whose primary button is **Load it** (file mode) and whose
secondary is a `draggable` chip — desktop only, hidden wherever a touch is
detected, and offered second. *Save the file* is what bridges the two on a
phone: the Mac's picker reads the file system, not this page's memory.

Three things about that card are scars. The chip was the *primary* affordance
in the first version, on the page that exists for phones. `#disk-chip` sets
`display: block`, which beats the UA stylesheet's `[hidden] { display: none }`
on specificity, so hiding it did nothing at all until `#disk-chip[hidden]` was
added. And whether a page-started file drag survives the drop is still
unverified — Playwright's synthetic drag replaces the data store (`dragstart`
sees `types: ["Files"]`, the drop sees `types: ["text/plain"]`) — which is one
more reason it is not the route anyone is told to take.

**Two things about this page were measured rather than guessed, and both are
worth not re-deriving.**

*Cross-origin isolation is where the speed went.* infinite-mac runs its
emulator against a `SharedArrayBuffer`, which only exists in a cross-origin
isolated document, and isolation is inherited — an iframe is isolated only if
its embedder is. GitHub Pages sends no headers, so the embed fell back to
message passing, and its own console says what that costs. Measured on the
real emulator (Mac OS 7.6, Quadra 650, All Out, `currentIPMS` from the
emulator's own stats over 30 seconds after boot): **56,233 instructions/ms
with the buffer against 1,896 without** — 3.4%, about thirty times slower, and
the whole of the difference between this page and infinitemac.org itself.
`coi-serviceworker.js` re-serves *only* `mobile.html`'s own navigation with
`COOP: same-origin` and `COEP: require-corp`, which is the only way an origin
that cannot set a header gets one; the page reloads once on the first visit to
come back through it, records the attempt in `sessionStorage` so a browser
that refuses cannot loop, and `?coi=0` unregisters. `require-corp` and not
`credentialless` because Safari is the browser this page exists for and does
not implement the latter. The iframe's `allow="cross-origin-isolated"` was
already there and is what delegates it.

*Two other suspects were measured at the same time and are innocent.*
`screen_update_messages` costs nothing detectable (1,896 with it on against
1,892 off), and neither does the tall emulated screen the page asks for
(640×1385 against 640×480 was inside the noise). Do not "optimise" either of
them: the frames are how the page learns the resolution the emulator settled
on, and the undither needs them.

*Installing a mod needs no pointer at all, and that is not a stylistic
choice.* Replacing the game's `Cythera Data` means copying a file between two
Finder windows, and the classic Finder has no keyboard copy — there is no Copy
and Paste for files in System 7 — so the only pointer-free route is
AppleScript, which Mac OS 7.6 has. The **Install** button sends six groups of
keystrokes and nothing else: Command-W a few times (which leaves the desktop
holding the keyboard), the disk's name (type-select), Command-O, the script's
name, Command-O (a `TEXT`/`ToyS` file opens in Script Editor), Command-R. Then
the script on the disk does the copy and launches the game. Nothing depends on
a window position, a resolution or a scroll offset, and with no such disk
mounted every step is a no-op.

**The whole chain has been driven end to end through both pages' own UI**, and
that is what found the last two bugs in it. The 1.0.4 `Cythera Data` was pulled
off the shipped CD image, opened in `explorer.html` through `?src=`, edited with
its own Edit Bytes, exported with its own button, dropped on the emulator,
installed with `mobile.html`'s own Install, and the game started on it — Create
Player then lists the game folder with the replaced `Cythera Data` in it.

**And the edit has been read back off the emulated screen, in the game's own
voice.** One word of the opening dream, resource `0x0240` byte 53, was changed
from `summer` to `bummer` in the edit box; after New Game → Save → the archetype
sheet's OK, Cythera narrates *“It was another hot bummer evening”* over the
bedroom art. Nothing between those two points is stubbed: the archive was
re-serialized by `writeDelverArchive`, laid out by `writeHfsImage`, mounted as a
disk by the emulator, copied over the shipped file by the AppleScript, and
decrypted by the game. It is the one test that covers the writer, the disk
image, the installer and the encryption at once, and it is worth re-running by
hand after a change to any of them.

Two things about driving it are worth knowing before spending an afternoon on
them. A synthetic tap with no dwell — a press and a release with nothing between
— is taken by Cythera's own start-screen buttons perhaps half the time and by
the Standard File dialog's `TrackControl` almost never; hold the button for a
few hundred milliseconds, as a finger does, and both take it first time. And a
dialog's default button is reached with Return, which sidesteps the question
entirely.

Four failure modes came out of doing that, and each names something a person
could not otherwise connect to this repository:

| what is wrong | what the Mac says |
|---|---|
| the data fork does not parse | `Scenario is not compatible with engine - please upgrade` |
| the resource fork is missing | `Sorry - there has been a fatal error:Unable to open RT` |
| the disk's catalog needs two leaves | the desktop-file alert, **modal**, and Install dies in it |
| the archive is fine | Cythera's start screen |

The third is the one only an end-to-end run could find: every test before it
used a two-file disk, which fits one catalog leaf and raises no alert. The
export writes two files for that reason and the Read Me's text lives in the
script's comments; `mobile_install_check.mjs` counts the entries so a third
cannot creep back in.

Three things about the script were settled by compiling it in a real Mac OS
7.6 and are pinned by `mobile_install_check.mjs`: `try` needs its `on error`
clause in this AppleScript (`Expected "on" but found "end"` otherwise),
`duplicate … with replacing` is what makes the replace silent, and
`display dialog` compiles, so the Dialogs scripting addition is there. The
same check reads both pages and fails if the names drift apart — the disk's
name and the script's are written by `explorer.html` and typed by
`mobile.html`, which is the arrangement `mobile_undither_check.mjs` exists
for.

*Direct mode's misalignment was a release sent at the next tap's position.*
Every button transition now carries the coordinates it was queued at. Before
that the outbox held only `{button, down}` and flushed whatever position was
pending when the gap expired, so two taps closer together than `BUTTON_GAP_MS`
reached the Mac as a drag from the first to the second — recorded off the real
emulator as `down(164,726) … move(542,907) … up`. `mobile_input_check.mjs`
compares the *positions* of the four transitions across every combination of
sync window and gap; it used to compare only their shape, which is why the bug
survived it.
