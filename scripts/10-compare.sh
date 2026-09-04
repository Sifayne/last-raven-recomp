#!/usr/bin/env bash
# Render one scenario through two backends and put the frames side by side.
#
# The software path is the oracle: what this produces is a picture of the gap,
# not a verdict. Read it with the caveats in tools/psprecomp/docs/RENDERER.md --
# in particular that GL cannot be bit-identical through a blend, because the
# software term is ((c+1)*f)>>8 and GL's is c*f/255.
#
# usage: scripts/10-compare.sh <scenario> [drain] [out.png]
set -euo pipefail
. "$(dirname "$0")/common.sh"

SCEN="${1:?usage: 10-compare.sh <scenario> [drain] [out.png]}"
DRAIN="${2:-25}"
OUT="${3:-$ROOT/reports/compare-$(basename "$SCEN" .pad).png}"
NAME=$(basename "$SCEN" .pad)

shot() {   # backend -> $2
    local be="$1" dst="$2"
    PSPRECOMP_RENDER="$be" "$ROOT/scripts/09-replay.sh" --decode --drain "$DRAIN" \
        --env "PSPRECOMP_RENDER=$be" "$SCEN" >/dev/null 2>&1 || true
    [ -f "$ROOT/frame.ppm" ] || die "no frame from the $be backend"
    magick "$ROOT/frame.ppm" -label "$be" "$dst"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

info "software"; shot software "$TMP/sw.png"
info "gl";       shot gl       "$TMP/gl.png"

# Labelled, 2x, nearest-neighbour: at 480x272 a smooth upscale hides exactly
# the per-pixel differences this exists to show.
magick montage "$TMP/sw.png" "$TMP/gl.png" \
    -filter point -resize 200% -tile 2x1 -geometry +8+8 \
    -background '#222' -fill '#ddd' -pointsize 20 \
    -title "$NAME  --  software (oracle)  vs  gl" "$OUT"

info "wrote $OUT"
magick compare -metric RMSE "$TMP/sw.png" "$TMP/gl.png" null: 2>&1 | head -1 || true
echo
