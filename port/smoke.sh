#!/bin/bash
# Regression check: build the port, run it headless, and assert the invariants
# that have held since the title screen first rendered.
#
#   ./smoke.sh            # build and check
#   ./smoke.sh --keep     # also leave build/smoke-screen.png behind to look at
#
# Budget note: the game's start-up is wall-clock bound, not instruction bound.
# It fades the splash in over 300 ticks (5 seconds) and out over 120 (2 seconds),
# spinning on TickCount throughout. A budget that does not cover roughly twenty
# seconds of real time will stop inside GammaFadeIn and look like a hang.
#
# And the fade is real now, so it costs what it always should have. The event
# loop does not start until about 1.5 billion instructions in, and pass 600000
# -- where the New Game click is scheduled -- lands at about 2.55 billion. The
# old 2.5 billion budget reached roughly 552000 passes, so the click silently
# never fired and the player-file check failed with no explanation. If that
# check starts failing again, look here before looking at the click.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
export TMPDIR="${TMPDIR:-/tmp}"

# Ninja if it is installed, otherwise plain makefiles.
GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

BUDGET=3400000000
out="$here/build/smoke"
mkdir -p "$out"

# cythera_symbols.txt names the 1877 functions in traces, and is what
# tools/opcensus.py and tools/pefdisasm.py read. It is not in this repository
# -- it was produced once by walking the binary's own traceback tables -- and
# the port runs perfectly well without it, printing addresses where it would
# print names. So it is passed only when it is actually present.
syms=()
for cand in "$root/reference/cythera_symbols.txt" "$root/cythera_symbols.txt"; do
  [[ -f "$cand" ]] && { syms=(--symbols "$cand"); break; }
done

extract="$here/build/extract"
if [[ ! -f "$extract/Cythera.data" ]]; then
  echo "==> extracting the original forks"
  mkdir -p "$extract"
  python3 "$root/port/binhex_decode.py" "$root/reference/Cythera.hqx" "$extract" >/dev/null
  python3 "$root/port/binhex_decode.py" "$root/reference/Cythera Data.hqx" "$extract" >/dev/null
fi

echo "==> building"
cmake -S "$here" -B "$here/build" -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      >/dev/null || { echo "FAIL: cmake configure"; exit 1; }
build_log=$(cmake --build "$here/build" 2>&1 | grep -vE "xcrun_db|^ar: error|^ranlib: error")
if grep -q "error:" <<<"$build_log"; then
  echo "FAIL: build errors"; grep "error:" <<<"$build_log" | head; exit 1
fi
warnings=$(grep -c "warning:" <<<"$build_log" || true)

# run <support-dir> <log-stem> [extra args...]
run() {
  local support="$1" stem="$2"; shift 2
  rm -rf "$support"
  "$here/build/cythera" "$extract/Cythera.data" \
    ${syms[@]+"${syms[@]}"} \
    --rsrc "$extract/Cythera.rsrc" \
    --game-dir "$extract" \
    --support-dir "$support" \
    --run --headless --budget "$BUDGET" \
    "$@" >"$out/$stem.log" 2>"$out/$stem.err"
}

echo "==> running (budget $BUDGET, about twenty seconds)"
log="$out/run.log"
run "$out/support" run --dump-screen "$out/screen.bin"

# A second run drives the start screen: clicking New Game must reach the game's
# own Standard File call and come back with a player file to create. Pass
# 600000 is comfortably after the start screen appears, and the item numbers
# are the game's own T7Widget layout, not DITL 128 -- see POWERPC-NOTES.md.
echo "==> running again, clicking New Game"
run "$out/newgame" newgame --click-at-pass 600000:245,196

python3 "$root/tools/screen_to_png.py" "$out/screen.bin" "$out/smoke-screen.png" \
  >/dev/null 2>&1

# ---- invariants -------------------------------------------------------------
fails=0
check() {  # check <description> <condition-result>
  if [[ "$2" == "ok" ]]; then printf '  PASS  %s\n' "$1"
  else printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); fi
}

missing=$(sed -n 's/.*still missing (\([0-9]*\) distinct.*/\1/p' "$log" | head -1)
check "every Toolbox call the game makes is implemented (missing=${missing:-?})" \
      "$([[ "${missing:-1}" == "0" ]] && echo ok)"

faults=$(grep -c "out-of-range guest accesses" "$log" || true)
check "no out-of-range guest memory accesses" \
      "$([[ "$faults" == "0" ]] && echo ok)"

passes=$(sed -n 's/.*[^0-9]\([0-9][0-9]*\) event-loop passes.*/\1/p' "$log" | head -1)
check "reached the main event loop (passes=${passes:-0})" \
      "$([[ "${passes:-0}" -gt 1000 ]] && echo ok)"

launched=$(grep -c "AppleEvent] dispatching" "$out/run.err" || true)
check "the launch event reached the application's own handler" \
      "$([[ "$launched" -ge 1 ]] && echo ok)"

colours=$(python3 - "$out/smoke-screen.png" <<'PY'
import sys
try:
    from PIL import Image
    im = Image.open(sys.argv[1]).convert('RGB')
    print(len(im.getcolors(maxcolors=300000) or []))
except Exception:
    print(0)
PY
)
# The port now runs past the splash to Cythera's start screen, which is lit by
# candlelight and so carries far fewer distinct colours than the title art did.
# What the count still proves is the thing worth proving: the game's own artwork
# reached the framebuffer, rather than a blank or single-colour screen.
check "the game's own artwork was drawn (distinct colours=$colours)" \
      "$([[ "$colours" -gt 40 ]] && echo ok)"

# Text rendering cannot be seen in the screenshot yet, because the game stops
# at its splash before drawing a character. Check it against the game's own
# demand instead: every 'TxSt' style resource must resolve to a real strike.
font_out=$("$here/build/cyt_font_test" "$extract/Cythera.rsrc" \
           "$extract/Cythera Data.rsrc" 2>&1)
font_fails=$(sed -n 's/.*checked, \([0-9]*\) failed.*/\1/p' <<<"$font_out" | head -1)
font_ok=$(sed -n 's/^\([0-9]*\) style resources checked.*/\1/p' <<<"$font_out" | head -1)
check "every text style resolves to a strike (${font_ok:-0} checked, ${font_fails:-?} failed)" \
      "$([[ "${font_fails:-1}" == "0" && "${font_ok:-0}" -gt 0 ]] && echo ok)"

# Preferences are the port's only writable resource fork, and the whole write
# path runs during an ordinary start-up: the game creates the file, adds a named
# 'Pref' resource and commits it. Reading it back is checked by the absence of
# the complaint the File Manager makes when a fork will not open.
# The resource fork writer, round-tripped through the reader. This is checked
# separately from the game because the format is easy to get subtly wrong in
# ways one small preferences file would never reach.
rf_out=$("$here/build/cyt_resfork_test" 2>&1)
check "the resource fork writer round-trips ($(grep -c PASS <<<"$rf_out") cases)" \
      "$(grep -q "all checks passed" <<<"$rf_out" && echo ok)"

prefs="$out/support/Cythera Preferences.rsrc"
pref_count=0
[[ -f "$prefs" ]] && pref_count=$(python3 "$root/tools/rsrcdump.py" "$prefs" \
  2>/dev/null | sed -n 's/^TOTAL *\([0-9]*\).*/\1/p')
no_fork=$(grep -c "no resource fork" "$out/run.err" || true)
check "the game's preferences were written and read back (${pref_count:-0} saved)" \
      "$([[ "${pref_count:-0}" -ge 1 && "$no_fork" == "0" ]] && echo ok)"

# Clicking New Game runs the game's own widget layer into TDelverApp::NewGame,
# which asks Standard File where to put the player. The port answers from the
# support directory, and the proof is that the game went on to create the file:
# a document of type 'DelP', named by the game rather than by this script.
player=""
for f in "$out/newgame"/*.finf; do
  [[ -f "$f" ]] || continue
  if [[ "$(head -c 4 "$f")" == "DelP" ]]; then
    player=$(basename "$f" .finf)
    break
  fi
done
check "clicking New Game created a player file (${player:-none})" \
      "$([[ -n "$player" ]] && echo ok)"

check "no compiler warnings (count=$warnings)" \
      "$([[ "$warnings" == "0" ]] && echo ok)"

echo
if [[ "$fails" == "0" ]]; then
  echo "smoke: all checks passed.  screenshot: $out/smoke-screen.png"
else
  echo "smoke: $fails check(s) failed.  log: $log"
fi
[[ "${1:-}" == "--keep" ]] || true
exit "$fails"
