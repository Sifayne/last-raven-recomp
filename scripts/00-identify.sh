#!/usr/bin/env bash
# Stage 00 — identify the disc.
#
# Every address in this project is per-build. Before anything else, confirm
# which SKU is in the game directory, because the PSN and UMD releases of a
# title are different binaries with different entry points and different
# signing keys. The profile GAME selects (scripts/games/<slug>.sh) says which
# disc its findings were measured on.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need_tool

ISO="$(find_iso)"
info "disc: $(basename "$ISO")"

"$AR" info "$ISO" | tee "$REPORTS/00-identify.txt"
"$AR" ls   "$ISO" > "$REPORTS/00-contents.txt"
echo
info "wrote $REPORTS/00-identify.txt and $REPORTS/00-contents.txt"

DISC_ID="$(grep -oP 'DISC_ID\s+\K\S+' "$REPORTS/00-identify.txt" || true)"
if [ "$DISC_ID" != "$EXPECT_DISC_ID" ]; then
    printf '\033[33mwarning:\033[0m disc is %s, the %s profile expects %s.\n' \
           "${DISC_ID:-unknown}" "$GAME" "$EXPECT_DISC_ID"
    printf '         Findings recorded for that profile may not transfer —\n'
    printf '         re-run the full pipeline and compare before trusting them.\n'
else
    info "disc ID $DISC_ID matches the $GAME profile"
fi
