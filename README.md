# Alchemy

Two attempts at running *Cythera* (1999) that got somewhere and were then
superseded. Kept whole, because each settled things the successors rely on.
Neither is being developed: read them, cite them, take findings out of them.
`CLAUDE.md` says how they relate to the other repositories.

## `port/` — a native port, PowerPC slice

A native arm64 port for modern macOS built without the game's source: it
loads the original PowerPC executable, interprets it, and reimplements the
Mac OS Toolbox underneath. C++20, CMake, SDL2. It reaches the start screen and
is not playable. Retired in August 2026 when running the game moved to a fork
of [systemless](https://github.com/ratlizard/systemless), which does the same
job for both the 68K and PowerPC slices and is further along. It stays as a
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
why a phone cannot hand it a file. Superseded by the plan to host systemless's
own WebAssembly build beside [Grimoire](https://github.com/ratlizard/grimoire)
and hand it the data directly. `mobile/MOBILE.md` records what was measured
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
