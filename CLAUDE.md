# CLAUDE.md

Guidance for AI assistants working in this repository.

**You are in `alchemy`: the proving ground for running *Cythera* off a
Macintosh.** `web/` is live — the game in a browser on the systemless fork's
WebAssembly build, published to GitHub Pages from this repository — and is
where a session about the browser or the phone belongs. `port/` and `mobile/`
are the two retired attempts, kept whole for what they settled: read them,
cite them, port findings out of them, do not extend them. Emulator fixes
still belong in `systemless`, not here; this repository only hosts and drives
the build.

## The repositories

The work is split across six, and they are checked out flat beside each other.
A session that clones one gets none of the rest, so paths across them are never
assumed:

| | |
|---|---|
| `ratlizard/grimoire` | public, GitHub Pages. The site: browser tools that read and narrowly edit Cythera's files. |
| **`ratlizard/alchemy`** | **public. This one.** `web/`, live; `port/` and `mobile/`, superseded. **Pushing `main` rebuilds `web/www/` onto `gh-pages`, which Pages serves.** |
| `ratlizard/cythera-workbench` | private. The Python tools and the notes of the systemless work. Canonical home of the seven scripts vendored here as `tools/` — see below. |
| `ratlizard/systemless` | public fork of benletchford/systemless. Where running the game happens now, on branch `cythera-detailed`. |
| `ratlizard/delvmod` | public fork. The correctness oracle for Cythera's formats; a submodule of `grimoire`, not of this one. |
| `e-z-g/cythera-reference` | private. The game, its documentation, the community's writing, the cited Apple documentation. Expected here as `reference/`. |

`reference/` is gitignored and is not in this repository; the usual arrangement
is a symlink to a checkout of `cythera-reference`. Without it neither tree runs.

## `web/` — the game in the browser

`web/cythera-web` is a `cdylib` over `../../../systemless` (the fork, checked
out beside this repository) with `default-features = false`, exporting a C ABI
the page calls directly; `src/lib.rs` documents each export. `web/build.sh`
builds it — read its comments, they are the two toolchain facts that cost a
build each. `web/www/index.html` is the page; `bench.mjs` and
`play_smoke.mjs` run the same module under Node, which is how the executor
was measured and how a change is checked without a browser. The Pages
workflow checks the fork out at `cythera-detailed` and builds from that, so a
fork change reaches the site on the next push here. The game is fetched from
archive.org at run time and is never in this repository. The workbench's
`doc/mobile-web-feasibility.md` (private) has the measurements and the list of
what the page still lacks; the touch shell in `mobile/` is what to port for
the controls.

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

## `tools/` — vendored copies, not originals

Seven scripts that `port/` calls or cites: `pefdisasm.py`, `pefdump.py`,
`pefreloc_sim.py`, `opcensus.py`, `screen_to_png.py`, `rsrcdump.py` and
`delv_compat.py`. **Every one is a copy.** The canonical file is
`tools/<name>` in `cythera-workbench`; each carries a `# COPY.` header saying
so, with the source's sha256.

Copies rather than a submodule because this repository has to stand alone — a
session that clones it flat gets no siblings, and `port/smoke.sh` has to run
anyway — and because a retired tree's copies cannot drift by being developed.
`tools/check_copies.sh` verifies them against the workbench when it is checked
out beside this one, and skips cleanly when it is not. Run it if you touch
anything here. **Fix bugs in the workbench and re-copy; do not edit these to
diverge.**

This directory did not exist between the repository split and 3 September 2026,
and `port/smoke.sh` called into it the whole time: with `set -uo pipefail` and
no `-e` it did not abort, so the screen PNG was silently never written and the
preferences invariant failed at `pref_count=0` — ten advertised invariants were
nine and a false failure. That is what the vendoring fixed.

`cythera_symbols.txt`, the 1,877 PowerPC function names, is *not* vendored: it
is 200 KB of generated data and lives at the root of `cythera-workbench`.
`run.sh`, `smoke.sh` and `drive.sh` try `reference/`, this repository's root,
and `../cythera-workbench/` in that order, and pass `--symbols` only if one
hits. Without it traces print addresses instead of names, which is not a
failure.

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
