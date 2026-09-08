# Alchemy

The proving ground for running *Cythera* (1999) somewhere other than a
Macintosh. Two attempts live here. Both got somewhere and were superseded, and are kept
whole because each settled things the successors rely on. The third, the game
running in a browser on a WebAssembly build of the systemless fork
[`ratlizard/wolflizard`](https://github.com/ratlizard/wolflizard), outgrew this
repository and left it — see below. `CLAUDE.md` says how they relate to the
other repositories.

## The browser player has moved

It was `web/` here until 8 September 2026 and is now its own repository,
[`ratlizard/ratlizard.github.io`](https://github.com/ratlizard/ratlizard.github.io),
playable at **https://ratlizard.github.io/**. It moved with `git subtree
split -P web`, so its history went with it. Nothing in this repository is
deployed any more.

## `port/` — a native port, PowerPC slice

A native arm64 port for modern macOS built without the game's source: it
loads the original PowerPC executable, interprets it, and reimplements the
Mac OS Toolbox underneath. C++20, CMake, SDL2. It reaches the start screen and
is not playable. Retired in August 2026 when running the game moved to the
systemless fork [`ratlizard/wolflizard`](https://github.com/ratlizard/wolflizard),
which does the same job for both the 68K and PowerPC slices and is further along. It stays as a
reference: it serves every Toolbox call on the load-a-saved-game path against
the PowerPC calling convention. `port/README.md` is its front door,
`port/POWERPC-NOTES.md` its working state; `cd port && ./smoke.sh` builds it
and checks ten invariants, given the game at `reference/game/` (gitignored;
supply your own).

## `mobile/` — the game on a phone, through an emulator

`mobile.html` is a touch shell around an [infinitemac.org](https://infinitemac.org)
embed, with a keystroke-only installer that puts an edited data file into the
emulated Mac without a pointer. It worked end to end — an edit made in the
browser was read back off the emulated screen — and it was always a workaround
for an iframe boundary that the emulator's server sits behind, which is also
why a phone cannot hand it a file. Superseded by the browser player, which runs
systemless's own WebAssembly build and is handed the data directly. `mobile/MOBILE.md` records what was measured
and what must not be undone by anyone who revives it; the checks in
`mobile/utilities/` are the ones that guarded it.

## `tools/`

Seven Python scripts `port/` calls or cites — PEF disassembly and dumps, an
opcode census, a resource-fork inventory, a framebuffer-to-PNG converter, and
the delvmod compatibility shim. **All are copies**, so this repository stands
alone; the canonical files are `tools/` in the private workbench, each copy
says so in its header, and `tools/check_copies.sh` verifies them when that
repository is checked out beside this one.

Both trees are GPL-3.0-or-later, as the rest of the effort. The game itself is
not here and never will be, and Cythera's words and images are Ambrosia
Software's and Glenn Andreas's.
