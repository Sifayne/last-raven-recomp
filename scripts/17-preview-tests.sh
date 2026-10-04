#!/usr/bin/env bash
# Inset 3D previews: GE commands through both transforms to physical pixels.
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="$ROOT/build/render-tests"
mkdir -p "$OUT"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
pkg-config --exists sdl2 SDL2_ttf || die "preview checks require SDL2 and SDL2_ttf"
read -r -a SDL_FLAGS <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
read -r -a SDL_LIBS <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
cc -O2 -Wall -Wextra -Werror -std=gnu11 -DHAVE_SDL2 "${SDL_FLAGS[@]}" \
    -I "$ROOT/tools/psprecomp/include" \
    "$ROOT/host/settings.c" "$ROOT/host/preview_tests.c" "$ROOT/host/render_gl.c" "$ROOT/host/present.c" "$ROOT/tools/psprecomp/src/host/save_dialog.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${SDL_LIBS[@]}" $HOST_LIBS \
    -o "$OUT/preview-tests"
for resolution in psp window; do
    for aspect in native window; do
        for transform in cpu gpu; do
            printf 'preview configuration: resolution=%s aspect=%s transform=%s\n' \
                "$resolution" "$aspect" "$transform"
            PSPRECOMP_RESOLUTION="$resolution" PSPRECOMP_ASPECT="$aspect" \
                PSPRECOMP_GL_TRANSFORM="$transform" PSPRECOMP_WINDOW_SIZE=960x544 \
                "$OUT/preview-tests"
        done
    done
done
