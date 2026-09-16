#!/usr/bin/env bash
# Shared settings for the Phase 0 pipeline.
#
# Every stage sources this. Paths are resolved from the repo root so the
# scripts work regardless of where they are invoked from.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

AR="$ROOT/build/psprecomp/tools/allegrexrecomp/allegrexrecomp"

die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

# libpsprecomp links openh264 when sceMpeg was built with it. CMake records that
# for CMake consumers; these scripts link by hand, so they have to ask too.
# Empty when the library is absent, which is the supported configuration -- the
# runtime then keeps its bookkeeping-only sceMpeg.
HOST_LINK_FLAGS=()
if pkg-config --exists openh264 2>/dev/null; then
    read -r -a HOST_LINK_FLAGS <<< "$(pkg-config --libs openh264)"
fi
# CMake and the hand-linked hosts must use the same pinned headers/libraries.
# In particular, pkg-config on the system can return a GPL-enabled FFmpeg.
. "$ROOT/scripts/ffmpeg-config.sh"
RUNTIME_CACHE="$ROOT/build/psprecomp/CMakeCache.txt"
if [ -f "$RUNTIME_CACHE" ]; then
    for entry in "FFMPEG_INCLUDE_DIR:PATH=$FFMPEG_PREFIX/include" \
                 "AVCODEC_LIBRARY:FILEPATH=$FFMPEG_PREFIX/lib/libavcodec.so" \
                 "AVUTIL_LIBRARY:FILEPATH=$FFMPEG_PREFIX/lib/libavutil.so"; do
        grep -Fxq "$entry" "$RUNTIME_CACHE" || \
            die "the runtime in build/psprecomp was configured before the bundled LGPL FFmpeg existed (or against a system one).
       Run scripts/build-tools.sh once: it downloads the pinned FFmpeg source (see third_party/ffmpeg/source.json),
       builds only the two ATRAC decoders into build/deps/ffmpeg (a few minutes) and reconfigures the runtime.
       Offline: place the pinned tarball at build/deps/downloads/ first; the checksum is still verified."
    done
    [ -f "$FFMPEG_PREFIX/lib/libavcodec.so" ] && [ -f "$FFMPEG_PREFIX/lib/libavutil.so" ] || \
        die "the bundled FFmpeg under build/deps/ffmpeg is missing; run scripts/build-tools.sh (downloads the pinned
       source and builds the two ATRAC decoders; offline, pre-place the tarball in build/deps/downloads/)"
    HOST_LINK_FLAGS+=("$FFMPEG_PREFIX/lib/libavcodec.so" "$FFMPEG_PREFIX/lib/libavutil.so"
        '-Wl,-rpath,$ORIGIN/lib:$ORIGIN/../deps/ffmpeg/lib')
fi
# Compatibility for older/local scripts; maintained scripts use the array so
# library paths containing spaces remain individual arguments.
HOST_LIBS="${HOST_LINK_FLAGS[*]}"
PSPDECRYPT="$ROOT/tools/pspdecrypt/pspdecrypt"

# Which title. GAME names a profile in scripts/games/<slug>.sh -- the module
# name, the emit prefix, the disc id to expect and the window title. Unset
# means Last Raven at the paths this project has always used; any other slug
# gets its own game, build and report directories, so two titles never
# overwrite each other's objects and Last Raven's regression rows do not move.
GAME="${GAME:-aclr}"
PROFILE="$ROOT/scripts/games/$GAME.sh"
[ -f "$PROFILE" ] || die "no profile for GAME=$GAME -- expected $PROFILE"
. "$PROFILE"
: "${MODULE:?} ${PREFIX:?} ${EXPECT_DISC_ID:?} ${TITLE:?}"

if [ "$GAME" = aclr ]; then
    GAME_DIR="$ROOT/game"
    BUILD_HOST_BASE="$ROOT/build/host"
    REPORTS="$ROOT/reports"
else
    GAME_DIR="$ROOT/games/$GAME"
    BUILD_HOST_BASE="$ROOT/build/$GAME/host"
    REPORTS="$ROOT/reports/$GAME"
fi
WORK="$GAME_DIR/extracted"      # decrypted binaries — gitignored
GEN="$GAME_DIR/generated"       # emitted C — gitignored, regenerate don't commit

# The host build directory for the OPT/TRACE selection in force: the -O2
# generated code and the traced objects each live beside the plain build (see
# 04-emit-build.sh). 09-replay.sh passes its own --trace flag as the argument;
# everyone else takes TRACE from the environment.
host_build_dir() {
    local d="$BUILD_HOST_BASE"
    [ "${OPT:-0}" != "0" ] && d="$d-opt"
    [ "${1:-${TRACE:-0}}" != "0" ] && d="$d-trace"
    printf '%s' "$d"
}

# pspautotests are the runtime's, not a title's: they stay in game/ whatever
# GAME says (07-autotests.sh has the clone command).
PSPAUTOTESTS="$ROOT/game/pspautotests"

# Native replacements: the list of guest functions the host implements, and
# the file that implements them. Last Raven's keep their historical names; any
# other title gets host/replace-<slug>.txt and host/replacements-<slug>.c. An
# empty list emits byte-identical C, so a new title starts with none.
if [ "$GAME" = aclr ]; then
    REPLACE_LIST="$ROOT/host/replace.txt"
    REPLACEMENTS_SRC="$ROOT/host/replacements.c"
else
    REPLACE_LIST="$ROOT/host/replace-$GAME.txt"
    REPLACEMENTS_SRC="$ROOT/host/replacements-$GAME.c"
fi
[ -f "$REPLACE_LIST" ]     || die "no $REPLACE_LIST for GAME=$GAME (an empty, comment-only file is fine)"
[ -f "$REPLACEMENTS_SRC" ] || die "no $REPLACEMENTS_SRC for GAME=$GAME (an empty translation unit is fine)"

# The main module. Note this is SYSDIR/EBOOT.BIN, NOT SYSDIR/UPDATE/EBOOT.BIN —
# the latter is the firmware updater Sony bundled on the disc, not game code.
EBOOT_PATH="${EBOOT_PATH:-SYSDIR/EBOOT.BIN}"
ELF="$WORK/$MODULE.elf"

find_iso() {
    local iso
    # -L: a symlink into the directory the dump actually lives in is fine.
    iso="$(find -L "$GAME_DIR" -maxdepth 1 -type f \( -iname '*.iso' -o -iname '*.cso' \) | head -1)"
    [ -n "$iso" ] || die "no .iso/.cso in ${GAME_DIR#"$ROOT"/}/ — supply your own dump of media you own"
    printf '%s' "$iso"
}

need_tool() {
    [ -x "$AR" ] || die "allegrexrecomp not built. Run: scripts/build-tools.sh"
}

mkdir -p "$WORK" "$GEN" "$REPORTS"
