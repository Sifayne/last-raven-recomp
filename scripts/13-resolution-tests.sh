#!/usr/bin/env bash
# Real GPU pixels, compatible texture views, CPU writes and resize history.
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="$ROOT/build/render-tests"
mkdir -p "$OUT"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
pkg-config --exists sdl2 || die "resolution checks require SDL2 and a desktop display"
read -r -a SDL_FLAGS <<< "$(pkg-config --cflags sdl2)"
read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl2)"
cc -O2 -Wall -Wextra -std=gnu11 -DHAVE_SDL2 "${SDL_FLAGS[@]}" \
    -I "$ROOT/tools/psprecomp/include" \
    "$ROOT/host/resolution_tests.c" "$ROOT/host/render_gl.c" "$ROOT/host/present.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${SDL_LIBS[@]}" $HOST_LIBS \
    -o "$OUT/resolution-tests"
for aspect in native window; do
    PSPRECOMP_RESOLUTION=window PSPRECOMP_ASPECT="$aspect" PSPRECOMP_WINDOW_SIZE=960x544 \
        "$OUT/resolution-tests"
done
