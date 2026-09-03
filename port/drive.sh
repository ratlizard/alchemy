#!/bin/bash
# Run the port headless and drive the start screen, for reading what the game
# does next. This is the harness the Standard File work was written against.
#
#   ./drive.sh --click-at-pass 600000:245,196          # New Game
#   ./drive.sh --click-at-pass 600000:245,268          # Open Game
#   CYT_DEBUG_FILE=1 ./drive.sh --click-at-pass 600000:245,196
#   CYT_TRACE_CALLS_FROM_PC=0x115190 ./drive.sh --click-at-pass 600000:245,268
#
# Output goes to build/drive/: run.log (the census and the halt reason),
# run.err (traces and manager logging) and screen.bin. The support directory
# is *kept* between runs, so a player file created by one run is there for the
# next one to open; pass --fresh to start from nothing.
#
# The start screen's item numbers are the game's own T7Widget layout, not those
# in DITL 128 -- see POWERPC-NOTES.md for the map.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
export TMPDIR="${TMPDIR:-/tmp}"

out="$here/build/drive"
mkdir -p "$out"
if [[ "${1:-}" == "--fresh" ]]; then
  shift
  rm -rf "$out/support"
fi

GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"
cmake -S "$here" -B "$here/build" -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      >/dev/null || exit 1
cmake --build "$here/build" 2>&1 \
  | grep -vE "xcrun_db|^ar: error|^ranlib: error" | grep -E "error|warning" || true

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
  echo "no extracted forks in $extract -- run ./run.sh or ./smoke.sh once first" >&2
  exit 1
fi
"$here/build/cythera" "$extract/Cythera.data" \
  ${syms[@]+"${syms[@]}"} \
  --rsrc "$extract/Cythera.rsrc" \
  --game-dir "$extract" \
  --support-dir "$out/support" \
  --run --headless --budget 4000000000 \
  --dump-screen "$out/screen.bin" \
  "$@" >"$out/run.log" 2>"$out/run.err"
status=$?

echo "log: $out/run.log   traces: $out/run.err   support: $out/support"
sed -n '/still missing/,$p' "$out/run.log" | head -20
exit "$status"
