#!/usr/bin/env bash
# Runtime contract plus real host dialog input/presentation. Cards live in /tmp.
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="$ROOT/build/savedata-checks"
mkdir -p "$OUT"
cmake --build "$ROOT/build/psprecomp" --target test_savedata -j"$(nproc)" >/dev/null
"$ROOT/build/psprecomp/tests/test_savedata"
read -r -a flags <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
read -r -a libs <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
cc -O2 -Wall -Wextra -Werror -UNDEBUG -std=gnu11 -DHAVE_SDL2 "${flags[@]}" \
    -I "$ROOT/tools/psprecomp/include" "$ROOT/host/save_dialog_tests.c" \
    "$ROOT/tools/psprecomp/src/host/save_dialog.c" "$ROOT/host/present.c" "$ROOT/host/render_gl.c" "$ROOT/host/settings.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${libs[@]}" "${HOST_LINK_FLAGS[@]}" \
    -o "$OUT/save-dialog-tests"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$OUT/save-dialog-tests" software "$OUT"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$OUT/save-dialog-tests" controller "$OUT"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$OUT/save-dialog-tests" nofont "$OUT"
if [[ "${1:-}" != --software ]]; then
    # Offscreen: a desktop window loses focus to notifications mid-run, and a
    # focus loss disarms the dialog by design.
    SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy PSPRECOMP_GL_SHOT="$OUT/gl" PSPRECOMP_GL_SHOT_EVERY=20 \
        "$OUT/save-dialog-tests" gl "$OUT"
fi
