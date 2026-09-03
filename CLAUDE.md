# CLAUDE.md

Guidance for AI assistants working in this repository.

**You are in `alchemy`: two retired attempts at running *Cythera*, kept whole
for what they settled.** Nothing here is being developed. Read it, cite it,
port findings out of it — do not extend it. If a session's goal is to make the
game run, it belongs in `systemless`, not here.

## The repositories

The work is split across six, and they are checked out flat beside each other.
A session that clones one gets none of the rest, so paths across them are never
assumed:

| | |
|---|---|
| `ratlizard/grimoire` | public, GitHub Pages. The site: browser tools that read and narrowly edit Cythera's files. |
| **`ratlizard/alchemy`** | **public. This one.** `port/` and `mobile/`, both superseded. |
| `ratlizard/cythera-tools` | private. The Python tools and the notes of the systemless work. Holds `tools/`, which `port/`'s scripts still call — see *A known break*. |
| `ratlizard/systemless` | public fork of benletchford/systemless. Where running the game happens now, on branch `cythera-detailed`. |
| `ratlizard/delvmod` | public fork. The correctness oracle for Cythera's formats; a submodule of `grimoire`, not of this one. |
| `e-z-g/cythera-reference` | private. The game, its documentation, the community's writing, the cited Apple documentation. Expected here as `reference/`. |

`reference/` is gitignored and is not in this repository; the usual arrangement
is a symlink to a checkout of `cythera-reference`. Without it neither tree runs.

## `port/` — the native PowerPC port

A native arm64 port for modern macOS built without the game's source: it loads
Cythera's PowerPC PEF, interprets it, and reimplements the Mac OS Toolbox
underneath. C++20, CMake, SDL2. It reaches the start screen and is not
playable.

Retired in August 2026 when running the game moved to the `systemless` fork,
which does the same job for both the 68K and the PowerPC slices and is further
along. It stays because it is the only thing that serves every Toolbox call on
the load-a-saved-game path against the PowerPC calling convention — where
`systemless` and this disagree about a record layout, this one has been run.

- `port/README.md` is the front door and says how to build.
- `port/POWERPC-NOTES.md` is the working state, and the more useful of the two.
- `cd port && ./smoke.sh` builds it and checks its invariants.

**It describes the PowerPC slice.** `systemless` runs the game's **68K** slice.
Every address and finding here — `IsOneGammaAvailable` at `0x074F28`, `MyLDEF`
at `0x06D5A4`, the routine descriptors, the run-time `0x4EF9` patching — was
read from PowerPC code and must be re-derived from the `CODE` resources before
it is asserted about `systemless`. That mistake has been made twice.

## A known break, not yet fixed

The repository split left `port/` calling into a tree it no longer has:

- `port/smoke.sh:83` runs `$root/tools/screen_to_png.py` and `:149` runs
  `$root/tools/rsrcdump.py`. There is no `tools/` here — those scripts are in
  `cythera-tools`. The script is `set -uo pipefail` with no `-e`, so it does
  not abort: the screen PNG is silently never written (stderr goes to
  `/dev/null`), and the preferences invariant fails with `pref_count=0`.
  `README.md` advertises ten invariants; it is nine and a false failure.
- `port/README.md` and `port/POWERPC-NOTES.md` cite `../tools/pefdisasm.py`,
  `../tools/opcensus.py` and `../tools/delv_compat.py`. Dead paths here.
- `cythera_symbols.txt` — the 1,877 PowerPC function names — now lives at the
  root of `cythera-tools`. `run.sh`, `smoke.sh` and `drive.sh` look for it at
  `reference/cythera_symbols.txt` or this repository's root, find neither, and
  pass no `--symbols`, so traces print addresses where they would print names.

The agreed fix is to vendor the six scripts `port/` actually calls into a
`tools/` here — copies, not a submodule, marked as copies with
`cythera-tools` named as canonical — since a retired tree's copies cannot
drift. It has not been done.

## `mobile/` — the game on a phone, through an emulator

`mobile.html` is a touch shell around an infinitemac.org embed, with a
keystroke-only installer that puts an edited data file into the emulated Mac
without a pointer. It worked end to end: an edit made in the browser was read
back off the emulated screen. Superseded by the plan to host `systemless`'s own
WebAssembly build beside `grimoire`.

`mobile/MOBILE.md` records what was measured and what must not be undone by
anyone reviving it — in particular the four failure messages the Mac gives when
a link in the export-and-install chain is broken. `mobile/utilities/` holds the
checks and the Playwright drivers that guarded it.

## Conventions

**Nobody is named here.** The maintainer is "the maintainer" in every file and
every commit; commits are authored `e-z-g <e-z-g@users.noreply.github.com>`.
Do not write a name, an email address or a home-directory path into the tree.

The game is not in this repository and never will be. Both trees are
GPL-3.0-or-later.
