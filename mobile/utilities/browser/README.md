# Driving the real pages in a browser

Everything in `utilities/` above this directory runs the pages' JavaScript in a
`node:vm` against a hand-written DOM stub, on stock Node with nothing installed.
These four files are the exception, and they are kept apart for that reason:
they need Playwright and a Chromium, so **`check_all.mjs` does not run them and
must not learn to**. They are how a person checks by hand when the answer has to
be a screenshot — and the end-to-end proof recorded in `CLAUDE.md` (an edit made
in `explorer.html` read back off the emulated screen in Cythera's own narration)
was obtained with exactly these.

They were scratch files for one afternoon. They are here because the afternoon
is not worth spending twice: every obstacle below cost between twenty minutes
and an hour to find, and none of them is guessable.

They were written in a cloud sandbox: no GPU (hence `--enable-unsafe-swiftshader`),
no network from the browser (hence `mirror.mjs` and the rewritten `EMBED_ORIGIN`),
and Chromium preinstalled under `$PLAYWRIGHT_BROWSERS_PATH`. On a local Mac with
Playwright's Chromium installed the mirror is unnecessary and the flags are
harmless; the obstacles about clicks, the window's shape and the change detector
are about the pages and the emulator, and hold anywhere.

```
edit_and_export.cjs   drive explorer.html: load an archive, edit one resource
                      through its own Edit Bytes, export the .dsk
install_and_play.cjs  drive mobile.html: drop that .dsk, press Install, then
                      New Game -> Save -> OK, and photograph the narration
serve.mjs             serve mobile.html and coi-serviceworker.js over http,
                      because a service worker cannot register from file://
mirror.mjs            a caching mirror of infinitemac.org, needed only where
                      the browser cannot reach the internet
```

## Running them

```sh
node utilities/browser/serve.mjs &            # :8123 -- mobile.html
node utilities/browser/mirror.mjs &           # :9000 -- infinitemac.org, if needed
python3 -m http.server 8200 &                 # the repo, for explorer.html

RESID=240 NEEDLE=summer REPLACE=bummer \
    node utilities/browser/edit_and_export.cjs      # writes edited.dsk
DSK=edited.dsk node utilities/browser/install_and_play.cjs
```

`install_and_play.cjs` writes a numbered screenshot at every step, which is the
whole point of it: `NN-narration.png` is the one that carries the answer.

Long-running servers must be started with `setsid … &` from a subshell in some
sandboxes or they die with the shell that launched them, and `pgrep -f` matches
the invoking shell too, so it will cheerfully report a server that is not
running.

## What these know that you would otherwise have to rediscover

**Playwright's package will not `import`.** `Named export 'chromium' not found`.
That is why these are `.cjs` and use `require()`.

**Chromium needs `--no-sandbox` and `--enable-unsafe-swiftshader`** in a
container: there is no GPU, and infinite-mac asks for WebGL.

**Reproduce cross-origin isolation or nothing finishes.** infinite-mac runs its
emulator against a `SharedArrayBuffer`, which exists only in a cross-origin
isolated document, and isolation is inherited from the embedder. Without it the
emulator is about thirty times slower — measured, see `CLAUDE.md` — and every
timeout written for the fast path expires in the middle of a boot. `serve.mjs`
exists so `coi-serviceworker.js` can register and the page can re-serve itself
with the two headers; `file://` cannot register a service worker at all.

**Where the browser cannot reach the internet, `mirror.mjs` is the way
through.** In at least one sandbox every host resets inside Chromium while
`curl` fetches it fine. Two things about that mirror, both of which cost an
hour: infinite-mac **PUTs** `/CD-ROM/<base64>`, so the mirror has to forward the
method — one that turned it into a GET produced `Malformed CD-ROM src chunk`
from the far end — and the method has to be part of the cache key. Point
`EMBED_ORIGIN` at it by setting `$MIRROR`, which `serve.mjs` rewrites into the
copy of the page it serves. With a working network, leave `$MIRROR` unset and
the real infinitemac.org is used.

**A fingerprint of the whole canvas is not a change detector.** Cythera's start
screen animates a torch, so the pixels differ every frame whether or not the
click landed, and a "did anything happen?" test built on one reports success
every time. `install_and_play.cjs` looks instead for what the click should
*produce*: the Standard File dialog is a large light rectangle on a screen that
is otherwise dark red, so the fraction of near-white pixels in the middle of the
frame separates them cleanly. That mistake cost an afternoon spent looking for a
bug in the installer, which was working the whole time.

**Hold the button.** A press and a release with nothing in between is taken by
Cythera's own start-screen buttons perhaps half the time and by the Standard
File dialog's `TrackControl` almost never. `click()` here holds for 350ms, as a
finger does, and both take it first time. A dialog's default button is better
reached with Return, which sidesteps the question entirely.

## The path through the game, in emulated pixels

Coordinates are in the **emulated screen's** own pixels, not the browser's, and
`install_and_play.cjs` converts at run time by reading `#scale-target`'s box, so
they survive a different device pixel ratio or fit-to-window scale.

**They do not survive a different window shape, and that is the trap here.**
`mobile.html` asks for a tall screen and infinite-mac hands back one that fits
the window, so the emulated screen comes out 640 × (viewportHeight × 640 ÷
viewportWidth) — 640×522 in an 800×620 window, 640×480 in an 800×600 one. Cythera
then lays its start screen out on whatever desktop it was given, so every
coordinate below moves. The driver asks for **800×600** for that reason and
prints the emulated size after boot; if it is not 640×480, re-measure before
trusting the table. An afternoon went into a driver that clicked 40 pixels above
`New Game` for exactly this reason and reported only that nothing happened.

| step | how | emulated | what it does |
|---|---|---|---|
| New Game | click, held | 244, 195 | opens the Create Player standard-file dialog |
| Save | **Return** | — | the dialog's default button; accepts the offered name |
| OK | click, held | 414, 414 | accepts the Explorer archetype as offered |
| — | wait | — | the opening dream, narrated over the bedroom art |

The name and the archetype are both taken as offered, which is deliberate: every
extra field is another thing to get wrong, and neither affects what the intro
narrates. Cancel is at 240, 414 if you need it.

Two things about the first click of a session. If the AppleScript installer has
just run, the frontmost application is Script Editor, and the first click on
Cythera is spent activating it — so `clickUntil()` repeats rather than assuming
one is enough. And nothing at all should be clicked before the install has
finished; there is no completion signal to wait for, only the clock.
