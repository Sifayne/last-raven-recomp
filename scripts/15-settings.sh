#!/usr/bin/env bash
# Normal play entry point, with a separate headless inspection path.
set -euo pipefail
. "$(dirname "$0")/common.sh"
OUT="$ROOT/build/settings"
mkdir -p "$OUT"
cc -O2 -Wall -Wextra -std=gnu11 "$ROOT/host/settings.c" "$ROOT/host/settings_tool.c" \
    -lm -lpthread -o "$OUT/settings-tool"
for arg in "$@"; do
    case "$arg" in
        --print-settings|--list-presets|--create-defaults)
            exec "$OUT/settings-tool" "$@" ;;
    esac
done
if [ "${1:-}" = --help ]; then
    cat <<'HELP'
scripts/15-settings.sh [--config FILE] [--preset NAME] [--font TTF]
                      [--boot EXECUTABLE] [--module ELF] [--iso DISC]
Open the pre-launch settings screen. Save & Play starts the game.

Headless commands (no SDL, game data or runtime build required):
  scripts/15-settings.sh --print-settings [--config FILE] [--preset NAME]
  scripts/15-settings.sh --list-presets --config FILE
  scripts/15-settings.sh --create-defaults FILE
HELP
    exit 0
fi
pkg-config --exists sdl2 SDL2_ttf || die "settings screen requires SDL2 and SDL2_ttf development packages; --print-settings works without them"
BOOT_NO_RUN=1 "$ROOT/scripts/06-boot.sh"
read -r -a UI_FLAGS <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
read -r -a UI_LIBS <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
cc -O2 -Wall -Wextra -std=gnu11 "${UI_FLAGS[@]}" \
    -I "$ROOT/tools/psprecomp/include" \
    "$ROOT/host/settings.c" "$ROOT/host/launcher.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${UI_LIBS[@]}" $HOST_LIBS \
    -o "$OUT/launcher"
BOOT_DIR="$ROOT/build/host"
[ "${OPT:-0}" != 0 ] && BOOT_DIR="$BOOT_DIR-opt"
[ "${TRACE:-0}" != 0 ] && BOOT_DIR="$BOOT_DIR-trace"
ARGS=(--boot "$BOOT_DIR/boot" --module "$ELF")
ISO="$(find "$GAME_DIR" -maxdepth 1 -type f -iname '*.iso' -print -quit)"
[ -z "$ISO" ] || ARGS+=(--iso "$ISO")
exec "$OUT/launcher" "${ARGS[@]}" "$@"
