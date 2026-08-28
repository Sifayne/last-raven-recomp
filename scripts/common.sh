#!/usr/bin/env bash
# Shared settings for the Phase 0 pipeline.
#
# Every stage sources this. Paths are resolved from the repo root so the
# scripts work regardless of where they are invoked from.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

AR="$ROOT/build/psprecomp/tools/allegrexrecomp/allegrexrecomp"
PSPDECRYPT="$ROOT/tools/pspdecrypt/pspdecrypt"

GAME_DIR="$ROOT/game"
WORK="$GAME_DIR/extracted"      # decrypted binaries — gitignored
GEN="$GAME_DIR/generated"       # emitted C — gitignored, regenerate don't commit
REPORTS="$ROOT/reports"

# The main module. Note this is SYSDIR/EBOOT.BIN, NOT SYSDIR/UPDATE/EBOOT.BIN —
# the latter is the firmware updater Sony bundled on the disc, not game code.
EBOOT_PATH="SYSDIR/EBOOT.BIN"
MODULE="ACLR_App"
ELF="$WORK/$MODULE.elf"

# The disc this pipeline was measured against. Phase 0 ran on the US PSN SKU,
# not the UMD retail SKU (ULUS-10493) — different EBOOT, re-signed with a
# 6.xx-era key. 00-identify.sh warns if yours differs; addresses are per-build.
EXPECT_DISC_ID="NPUH10024"

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

find_iso() {
    local iso
    iso="$(find "$GAME_DIR" -maxdepth 1 -type f \( -iname '*.iso' -o -iname '*.cso' \) | head -1)"
    [ -n "$iso" ] || die "no .iso/.cso in game/ — supply your own dump of media you own"
    printf '%s' "$iso"
}

need_tool() {
    [ -x "$AR" ] || die "allegrexrecomp not built. Run: scripts/build-tools.sh"
}

mkdir -p "$WORK" "$GEN" "$REPORTS"
