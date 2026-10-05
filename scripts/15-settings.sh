#!/usr/bin/env bash
# Normal play entry point, with a separate headless inspection path.
set -euo pipefail
# A GAME the caller set opens the launcher on that title; common.sh would
# default it to Last Raven, so remember it first.
GAME_REQUESTED="${GAME:-}"
. "$(dirname "$0")/common.sh"
OUT="$ROOT/build/settings"
mkdir -p "$OUT"
cc -O2 -Wall -Wextra -std=gnu11 -I "$ROOT/tools/psprecomp/include" "$ROOT/host/settings.c" "$ROOT/tools/psprecomp/src/host/settings.c" "$ROOT/host/settings_tool.c" \
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
Open the pre-launch settings screen. Every title in scripts/games/ that has
an emitted module gets a tab (its boot host is rebuilt first); the tab you
pick is remembered. GAME=<slug> in the environment opens on that title.
Save & Play starts the game.

Headless commands (no SDL, game data or runtime build required):
  scripts/15-settings.sh --print-settings [--config FILE] [--preset NAME]
  scripts/15-settings.sh --list-presets --config FILE
  scripts/15-settings.sh --create-defaults FILE
HELP
    exit 0
fi
pkg-config --exists sdl2 SDL2_ttf || die "settings screen requires SDL2 and SDL2_ttf development packages; --print-settings works without them"
read -r -a UI_FLAGS <<< "$(pkg-config --cflags sdl2 SDL2_ttf)"
read -r -a UI_LIBS <<< "$(pkg-config --libs sdl2 SDL2_ttf)"
cc -O2 -Wall -Wextra -std=gnu11 "${UI_FLAGS[@]}" \
    -I "$ROOT/tools/psprecomp/include" \
    "$ROOT/host/settings.c" "$ROOT/tools/psprecomp/src/host/settings.c" \
    "$ROOT/host/launcher_info.c" "$ROOT/tools/psprecomp/src/host/launcher.c" \
    "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${UI_LIBS[@]}" "${HOST_LINK_FLAGS[@]}" \
    -o "$OUT/launcher"
# One --game per profile whose module has been emitted, in profile order. The
# profile is read through common.sh in a subshell so the paths are the ones
# every other stage uses; the boot host is relinked so a tab never starts a
# stale build. OPT/TRACE in the environment select the build as elsewhere.
ARGS=()
# Release order for the supported trilogy, then any additional profiles.
profiles=("$ROOT/scripts/games/ac3p.sh" "$ROOT/scripts/games/acsl.sh" "$ROOT/scripts/games/aclr.sh")
for profile in "$ROOT"/scripts/games/*.sh; do
    case "$(basename "$profile")" in ac3p.sh|acsl.sh|aclr.sh) continue ;; esac
    profiles+=("$profile")
done
for profile in "${profiles[@]}"; do
    [ -f "$profile" ] || continue
    slug="$(basename "$profile" .sh)"
    if ! entry="$(GAME="$slug" bash -c '
            . "$1/scripts/common.sh"
            d="$(host_build_dir)"
            [ -f "$d/${PREFIX}_funcs.o" ] || exit 3
            iso="$(find -L "$GAME_DIR" -maxdepth 1 -type f -iname "*.iso" -print -quit)"
            printf "%s|%s|%s|%s|%s" "$GAME" "$TITLE" "$d/boot" "$ELF" "$iso"' _ "$ROOT")"; then
        info "no emitted module for $slug -- run GAME=$slug scripts/04-emit-build.sh to add its tab"
        continue
    fi
    GAME="$slug" BOOT_NO_RUN=1 "$ROOT/scripts/06-boot.sh" >/dev/null
    ARGS+=(--game "$entry")
done
[ "${#ARGS[@]}" -gt 0 ] || die "no title has an emitted module; run scripts/04-emit-build.sh first"
[ -z "$GAME_REQUESTED" ] || ARGS+=(--select "$GAME_REQUESTED")
exec "$OUT/launcher" "${ARGS[@]}" "$@"
