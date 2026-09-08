#!/bin/sh
# Have the vendored copies in this directory drifted from the canonical ones?
#
# Every file listed in COPIES.txt is a copy of tools/<name> from the
# disassembly toolkit these scripts came out of, which is kept outside this
# repository. Copies are used rather than a submodule because this repository
# is retired and must stand alone: a session that clones it flat gets no
# siblings, and port/smoke.sh has to run anyway.
#
# Point $CYTHERA_TOOLS at that toolkit's tools/ directory to check for drift.
# Skips, with status 0, when it is unset or absent -- there is nothing to
# compare against, which is not a failure.
set -u
wb="${CYTHERA_TOOLS:-}"
here=$(cd "$(dirname "$0")" && pwd)

if [ -z "$wb" ] || [ ! -d "$wb" ]; then
  echo "skip: set \$CYTHERA_TOOLS to the canonical tools/ directory to check drift"
  exit 0
fi

fails=0
while read -r want name; do
  [ -z "${name:-}" ] && continue
  case "$want" in \#*) continue;; esac
  if [ ! -f "$wb/$name" ]; then
    echo "FAIL $name: gone from the canonical toolkit"; fails=$((fails + 1)); continue
  fi
  got=$(shasum -a 256 "$wb/$name" | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then
    echo "  ok  $name"
  else
    echo "FAIL $name: the canonical copy has changed since this one was taken"
    echo "      canonical $got"
    echo "      recorded  $want"
    fails=$((fails + 1))
  fi
done <<INNER
$(sed -n 's/^\([0-9a-f]\{64\}\)  \(.*\)$/\1 \2/p' "$here/COPIES.txt")
INNER

if [ "$fails" -ne 0 ]; then
  echo
  echo "$fails file(s) drifted. Re-copy from the canonical toolkit, keep the COPY header,"
  echo "and update COPIES.txt with the new sha256."
  exit 1
fi
echo "all copies current"
