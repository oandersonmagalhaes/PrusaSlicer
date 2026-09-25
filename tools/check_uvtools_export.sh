#!/bin/sh
# Slices the 20 mm test cube for each printer preset and checks the result with UVtools.
set -eu
cd "$(dirname "$0")/.."
BIN=$(find build/src -maxdepth 5 -type f -perm -u+x \( -name prusa-slicer -o -name PrusaSlicer \) | head -1)
UV=${UVTOOLS_CMD:-/Applications/UVtools.app/Contents/MacOS/UVtoolsCmd}
OUT=$(mktemp -d)
R=resources/sbm_resin
check() { # printer-ini material-ini ext expected-x expected-y
  "$BIN" --export-sla --load "$R/printer/$1" --load "$R/sla_print/0.05 Normal @SBM.ini" \
         --load "$R/sla_material/$2" --output "$OUT/cube.$3" tests/data/20mm_cube.obj
  PROPS=$("$UV" print-properties "$OUT/cube.$3" -n ResolutionX ResolutionY LayerCount)
  echo "$PROPS"
  echo "$PROPS" | grep -q "ResolutionX: $4" && echo "$PROPS" | grep -q "ResolutionY: $5" \
    || { echo "FAIL: $3 resolution"; exit 1; }
  LAYERS=$(echo "$PROPS" | sed -n 's/^LayerCount: //p')
  [ "${LAYERS:-0}" -ge 400 ] || { echo "FAIL: $3 has $LAYERS layers, expected >= 400"; exit 1; }
  echo "OK: $3"
}
check "Elegoo Saturn 3 Ultra (GOO).ini" "Generic Resin @Saturn3Ultra 0.05.ini" goo 11520 5120
check "Creality Halot R6 (CXDLPv4).ini" "Generic Resin @HalotR6 0.05.ini" cxdlpv4 1620 2560
echo "Files kept in $OUT"
