#!/bin/bash
# Build the port and run it against the original game files.
#
#   ./run.sh                 # build, then run
#   ./run.sh --trace-calls   # ... and log every Toolbox call
#
# Any extra arguments are passed straight through to the cythera binary.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

# Apple's toolchain wants a writable temporary directory for its xcrun cache;
# without this, ar and ranlib print (harmless but noisy) permission errors.
export TMPDIR="${TMPDIR:-/tmp}"

# Ninja if it is installed, otherwise plain makefiles.
GEN="Unix Makefiles"
command -v ninja >/dev/null 2>&1 && GEN="Ninja"

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
  echo "==> extracting the original forks from the BinHex archives"
  mkdir -p "$extract"
  python3 "$root/port/binhex_decode.py" "$root/reference/Cythera.hqx" "$extract"
  python3 "$root/port/binhex_decode.py" "$root/reference/Cythera Data.hqx" "$extract"
fi

echo "==> building"
cmake -S "$here" -B "$here/build" -G "$GEN" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      >/dev/null
cmake --build "$here/build" 2>&1 | grep -vE "xcrun_db|^ar: error|^ranlib: error" || true

# No instruction budget when a person is driving it: the budget exists to stop
# a headless diagnostic run, and interactively it only decides how many seconds
# of play you get before the port stops for no reason the player can see. The
# gamma fade made that acute -- it spends five seconds of real time and roughly
# 1.5 billion instructions before the start screen appears at all, so the old
# two-billion default left almost nothing behind it.
echo "==> running"
exec "$here/build/cythera" \
  "$extract/Cythera.data" \
  ${syms[@]+"${syms[@]}"} \
  --rsrc "$extract/Cythera.rsrc" \
  --game-dir "$extract" \
  --support-dir "$here/build/saves" \
  --run --budget 0 \
  "$@"
