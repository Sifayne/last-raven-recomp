#!/usr/bin/env bash
# Render one scenario through both backends and put the frames side by side.
#
# These are independent live runs with different clock/presentation paths.
# For a fixed-input comparison and regression verdict, use 11-render-check.py;
# it replays the same captured GE lists and memory through both backends.
#
# usage: scripts/10-compare.sh <scenario> [drain] [out.png]
set -euo pipefail
. "$(dirname "$0")/common.sh"

SCEN="${1:?usage: 10-compare.sh <scenario> [drain] [out.png]}"
DRAIN="${2:-25}"
OUT="${3:-$ROOT/reports/compare-$(basename "$SCEN" .pad).png}"
NAME=$(basename "$SCEN" .pad)

# ImageMagick's default font resolves to nothing on this machine and every
# label renders as empty boxes, which is worse than no label because it looks
# like the image is broken. Name one explicitly.
FONT=$(fc-match -f '%{file}' 'DejaVu Sans' 2>/dev/null || true)
[ -n "${FONT:-}" ] && [ -f "$FONT" ] || FONT=$(fc-match -f '%{file}' sans-serif)
[ -f "$FONT" ] || die "no usable font for labels"

shot() {   # backend -> $2
    local be="$1" dst="$2"
    "$ROOT/scripts/09-replay.sh" --decode --drain "$DRAIN" \
        --env "PSPRECOMP_RENDER=$be" "$SCEN" >/dev/null 2>&1 || true
    [ -f "$ROOT/frame.ppm" ] || die "no frame from the $be backend"
    magick "$ROOT/frame.ppm" "$dst"
}

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

info "software"; shot software "$TMP/sw.png"
info "gl";       shot gl       "$TMP/gl.png"

# 2x, nearest-neighbour: at 480x272 a smooth upscale hides exactly the
# per-pixel differences this exists to show.
for b in sw gl; do
    magick "$TMP/$b.png" -filter point -resize 200% "$TMP/$b-2x.png"
done
label() {   # text -> $2
    magick -background '#222' -fill '#ddd' -font "$FONT" -pointsize 22 \
           label:"$1" -bordercolor '#222' -border 6 "$2"
}
label "software (oracle)" "$TMP/sw-l.png"
label "gl"                "$TMP/gl-l.png"
magick "$TMP/sw-l.png" "$TMP/sw-2x.png" -background '#222' -gravity center -append "$TMP/sw-c.png"
magick "$TMP/gl-l.png" "$TMP/gl-2x.png" -background '#222' -gravity center -append "$TMP/gl-c.png"
magick "$TMP/sw-c.png" "$TMP/gl-c.png" -background '#222' -gravity north +append "$TMP/pair.png"
label "$NAME" "$TMP/title.png"
magick "$TMP/title.png" "$TMP/pair.png" -background '#222' -gravity center \
       -append -bordercolor '#222' -border 10 "$OUT"

info "wrote $OUT"
magick compare -metric RMSE "$TMP/sw.png" "$TMP/gl.png" null: 2>&1 | head -1 || true
echo
