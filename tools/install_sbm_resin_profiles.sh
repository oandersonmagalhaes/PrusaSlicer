#!/bin/sh
# Copies the Saturn 3 Ultra / Halot R6 presets into PrusaSlicer's user presets.
set -eu
SRC="$(cd "$(dirname "$0")/.." && pwd)/resources/sbm_resin"
DST="$HOME/Library/Application Support/PrusaSlicer"
for kind in printer sla_print sla_material; do
  mkdir -p "$DST/$kind"
  cp "$SRC/$kind/"*.ini "$DST/$kind/"
done
echo "Installed presets into $DST"
