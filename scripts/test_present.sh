#!/usr/bin/env bash
set -euo pipefail
. "$(dirname "$0")/common.sh"
mkdir -p "$ROOT/build/present-checks"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
read -r -a flags <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
read -r -a libs <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
cc -O2 -Wall -Wextra -Werror -std=gnu11 -DHAVE_SDL2 "${flags[@]}" \
    -I "$ROOT/tools/psprecomp/include" "$ROOT/tools/psprecomp/tests/test_present.c" "$ROOT/tools/psprecomp/src/host/settings.c" "$ROOT/tools/psprecomp/src/host/save_dialog.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${libs[@]}" "${HOST_LINK_FLAGS[@]}" \
    -o "$ROOT/build/present-checks/present-tests"
"$ROOT/build/present-checks/present-tests"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$ROOT/build/present-checks/present-tests" keyboard
# The test keeps SDL to its virtual controller, so a physical one cannot take
# ownership before it here either.
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$ROOT/build/present-checks/present-tests" controller
