#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
P="$ROOT/tools/psprecomp"
case "${1:-}" in ""|--ui) ;; *) echo "usage: scripts/16-settings-tests.sh [--ui]" >&2; exit 2;; esac
mkdir -p "$ROOT/build/settings"
cc -O2 -Wall -Wextra -Werror -std=gnu11 -I "$P/include" \
    "$ROOT/host/settings.c" "$P/src/host/settings.c" "$ROOT/host/settings_tests.c" -lm -lpthread \
    -o "$ROOT/build/settings/settings-tests"
"$ROOT/build/settings/settings-tests"
if [ "${1:-}" = --ui ]; then
    . "$ROOT/scripts/common.sh"
    cmake --build "$ROOT/build/psprecomp" --target psprecomp psprecomp_imgui -j"$(nproc)" >/dev/null
    pkg-config --exists sdl2 || die "UI checks require SDL2"
    read -r -a FLAGS <<< "$(pkg-config --cflags sdl2)"
    read -r -a LIBS <<< "$(pkg-config --libs sdl2)"
    IMGUI="$P/third_party/imgui"
    OUT="$ROOT/build/settings"
    for src in "$P/src/host/ui.cpp" "$IMGUI/backends/imgui_impl_sdl2.cpp" "$IMGUI/backends/imgui_impl_sdlrenderer2.cpp"; do
        c++ -O2 -std=c++11 -fno-exceptions -fno-rtti -fno-threadsafe-statics "${FLAGS[@]}" \
            -I "$IMGUI" -c "$src" -o "$OUT/$(basename "$src" .cpp).o"
    done
    # The checks include psprecomp's launcher.c for its internals.
    cc -O2 -Wall -Wextra -Werror -std=gnu11 -DHAVE_SDL2 "${FLAGS[@]}" -I "$P/include" -I "$P/src/host" \
        "$ROOT/host/settings.c" "$P/src/host/settings.c" "$ROOT/host/launcher_info.c" \
        "$P/src/host/launcher_one.c" "$P/src/host/pages.c" "$ROOT/host/launcher_tests.c" \
        "$OUT/ui.o" "$OUT/imgui_impl_sdl2.o" "$OUT/imgui_impl_sdlrenderer2.o" \
        "$ROOT/build/psprecomp/libpsprecomp_imgui.a" "$ROOT/build/psprecomp/libpsprecomp.a" \
        -rdynamic -ldl -lm -lpthread "${LIBS[@]}" "${HOST_LINK_FLAGS[@]}" -o "$OUT/launcher-tests"
    mkdir -p "$OUT/ui-checks"
    SDL_VIDEODRIVER=dummy "$OUT/launcher-tests" "$OUT/ui-checks"
fi
