# Alchemy

The proving ground for running *Cythera* (1999) somewhere other than a
Macintosh. Two attempts live here. Both got somewhere and were superseded, and
are kept whole because each settled things the successors rely on — and because
either may be picked up again: they are dormant, not abandoned. The third —
the game running in a browser on a WebAssembly build of a fork of
[systemless](https://github.com/benletchford/systemless), Ben Letchford's
ROM-free runtime for classic Mac software — outgrew this repository and left
it; see below. `CLAUDE.md` says how they relate to the other repositories.

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
is not playable. Set aside in August 2026 when running the game moved to the
systemless fork [`ratlizard/wolflizard`](https://github.com/ratlizard/wolflizard),
which does the same job for both the 68K and PowerPC slices and is further along.
It is **dormant rather than retired**: it stays as a reference — it serves every
Toolbox call on the load-a-saved-game path against the PowerPC calling
convention — and it may be resumed. `port/README.md` is its front door,
`port/POWERPC-NOTES.md` its working state; `cd port && ./smoke.sh` builds it
and checks ten invariants, given the game at `reference/game/` (gitignored;
supply your own). It still passes them, on a toolchain newer than the one it
was written against, which is the only fact about resuming it worth having —
so run it first rather than believing this paragraph.

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
alone; the canonical files are kept with the disassembly toolkit they came out
of, outside this repository, each copy says so in its header, and
`tools/check_copies.sh` verifies them when `$CYTHERA_TOOLS` points at it.

## Credits

Almost nothing here would exist without other people's work.

- **Ben Letchford** — [systemless](https://github.com/benletchford/systemless),
  the ROM-free runtime that actually runs the game. The browser player is a
  thin C-ABI binding over a fork of it; the emulation is his, not ours.
- **Bryce Schroeder** — [delvmod](https://github.com/BryceSchroeder/delvmod)
  and the Delver Technical Documentation Project, which worked out Cythera's
  formats in the first place. `tools/delv_compat.py` exists to run it.
- **Glenn Andreas and Ambrosia Software** — who made *Cythera* and the Delver
  engine.

## Licensing

Everything here is GPL-3.0-or-later, as the rest of the effort; `LICENSE` has
the text. Code ported from systemless keeps its origin and its copyright
notice in the file's header.

The game itself is not here and never will be. *Cythera: Fate of Alaric* is
Ambrosia Software's and Glenn Andreas's, "Cythera" and "Delver" are their
trademarks, and nothing here is distributed with or derived from a licence to
redistribute it. Use a copy you are entitled to use.
