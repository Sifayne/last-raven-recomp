#!/usr/bin/env bash
# Game-camera regression, without an SDL window or a full boot/replay.
set -euo pipefail
. "$(dirname "$0")/common.sh"
# host/aspect_tests.c drives Last Raven's own camera and render functions by
# address; there is nothing for it to test on another title.
[ "$GAME" = aclr ] || die "the aspect tests are a Last Raven fixture; nothing to run for GAME=$GAME"
OUT="$ROOT/build/aspect-tests"
MODULE_OBJ="$BUILD_HOST_BASE"
[ "${OPT:-0}" != 0 ] && MODULE_OBJ="$MODULE_OBJ-opt"
RECOMP="$ROOT/tools/psprecomp/tools/allegrexrecomp"
[ -f "$MODULE_OBJ/${PREFIX}_funcs.o" ] || die "run scripts/04-emit-build.sh first"
mkdir -p "$OUT"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
# HAVE_SDL2 exposes the presentation API, which the fixture supplies itself.
cc -O2 -std=gnu11 -DHAVE_SDL2 -I "$ROOT/tools/psprecomp/include" -I "$GEN" -I "$RECOMP" \
    "$ROOT/host/settings.c" "$ROOT/tools/psprecomp/src/host/settings.c" "$ROOT/host/aspect_tests.c" "$ROOT/host/replacements.c" \
    "$RECOMP/loader.c" "$RECOMP/container.c" "$RECOMP/decode.c" \
    "$MODULE_OBJ/${PREFIX}_funcs.o" "$MODULE_OBJ/${PREFIX}_imports.o" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${HOST_LINK_FLAGS[@]}" -o "$OUT/aspect-tests"
"$OUT/aspect-tests" "${1:-$ELF}"
