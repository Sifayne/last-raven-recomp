#!/usr/bin/env bash
set -euo pipefail
. "$(dirname "$0")/common.sh"
mkdir -p "$ROOT/build/present-checks"
cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
read -r -a flags <<< "$(pkg-config --cflags sdl2)"
read -r -a libs <<< "$(pkg-config --libs sdl2)"
cc -O2 -Wall -Wextra -Werror -std=gnu11 -DHAVE_SDL2 "${flags[@]}" \
    -I "$ROOT/tools/psprecomp/include" "$ROOT/host/present_tests.c" "$ROOT/host/settings.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${libs[@]}" "${HOST_LINK_FLAGS[@]}" \
    -o "$ROOT/build/present-checks/present-tests"
"$ROOT/build/present-checks/present-tests"
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$ROOT/build/present-checks/present-tests" keyboard
# Virtual-controller integration is also run in the isolated package builder,
# where a physical controller cannot take ownership before the virtual device.
