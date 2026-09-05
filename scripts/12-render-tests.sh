#!/usr/bin/env bash
# GPU-optional, game-independent RGBA backend conformance fixtures.
set -euo pipefail
. "$(dirname "$0")/common.sh"
case "${1:-}" in
    ""|--software) ;;
    *) die "usage: scripts/12-render-tests.sh [--software]" ;;
esac
OUT="$ROOT/build/render-tests"
mkdir -p "$OUT"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
SDL_FLAGS=()
SDL_LIBS=()
SOURCES=("$ROOT/host/render_tests.c" "$ROOT/host/render_gl.c")
if pkg-config --exists sdl2; then
    read -r -a SDL_FLAGS <<< "-DHAVE_SDL2 $(pkg-config --cflags sdl2)"
    read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl2)"
    SOURCES+=("$ROOT/host/present.c")
fi
cc -O2 -Wall -Wextra -std=gnu11 "${SDL_FLAGS[@]}" \
    -I "$ROOT/tools/psprecomp/include" "${SOURCES[@]}" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread \
    "${SDL_LIBS[@]}" $HOST_LIBS -o "$OUT/render-tests"
"$OUT/render-tests" software
if [ "${1:-}" != --software ]; then
    [ "${#SDL_LIBS[@]}" != 0 ] || die "GL checks require SDL2 (or use --software)"
    "$OUT/render-tests" gl
fi
