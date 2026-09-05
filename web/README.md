# web/ — Cythera in the browser, on the systemless fork

`cythera-web/` is a C-ABI WebAssembly binding over `../../../systemless`
(the fork, checked out beside this repository); `www/index.html` is the page
that runs it. `build.sh` builds the module locally and its comments record
the two toolchain facts that cost a build each. The Pages workflow builds
from the fork's `cythera-detailed` branch and publishes `www/` to `gh-pages`.

The Node runners in `www/` drive the same module without a browser:

| runner | what it proves |
|---|---|
| `bench.mjs` | instructions per second on the headless boot, and a frame |
| `play_smoke.mjs` | the wall-clock path runs with audio and input |
| `audio_smoke.mjs` | the module produces sound from the start screen |
| `saves_smoke.mjs` | import before start, scan, acknowledge, exclusions |
| `menus_smoke.mjs` | the menu snapshot and a selection through MenuSelect |
| `realtime_frame.mjs` | a frame after N seconds on the wall-clock path |
| `drive.mjs` | a scripted run: import a save store, wait, click, key, menu, frame |
| `page_smoke.mjs` | the page's script runs top to bottom under a stub document |

Each takes the game archive as its first argument; `game.sit` beside the page
(gitignored) is the usual symlink. The game is not in this repository.
