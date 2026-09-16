#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "${1:-}" in ""|--ui) ;; *) echo "usage: scripts/16-settings-tests.sh [--ui]" >&2; exit 2;; esac
mkdir -p "$ROOT/build/settings"
cc -O2 -Wall -Wextra -Werror -std=gnu11 \
    "$ROOT/host/settings.c" "$ROOT/host/settings_tests.c" -lm -lpthread \
    -o "$ROOT/build/settings/settings-tests"
"$ROOT/build/settings/settings-tests"
if [ "${1:-}" = --ui ]; then
    . "$ROOT/scripts/common.sh"
    cmake --build "$ROOT/build/psprecomp" --target psprecomp -j"$(nproc)" >/dev/null
    pkg-config --exists sdl2 SDL2_ttf || die "UI checks require SDL2 and SDL2_ttf"
    read -r -a FLAGS <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
    read -r -a LIBS <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
    cc -O2 -Wall -Wextra -Werror -std=gnu11 "${FLAGS[@]}" \
        -I "$ROOT/tools/psprecomp/include" \
        "$ROOT/host/settings.c" "$ROOT/host/launcher_tests.c" \
        "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${LIBS[@]}" "${HOST_LINK_FLAGS[@]}" \
        -o "$ROOT/build/settings/launcher-tests"
    mkdir -p "$ROOT/build/settings/ui-checks"
    SDL_VIDEODRIVER=dummy "$ROOT/build/settings/launcher-tests" "$ROOT/build/settings/ui-checks"
fi
