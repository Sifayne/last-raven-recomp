#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build/fps"
cc -O2 -Wall -Wextra -Werror -std=gnu11 "$ROOT/host/fps_tests.c" -lm -o "$ROOT/build/fps/fps-tests"
"$ROOT/build/fps/fps-tests"
cc -O2 -Wall -Wextra -Werror -std=gnu11 "$ROOT/host/fps_pose_tests.c" -lm -o "$ROOT/build/fps/fps-pose-tests"
"$ROOT/build/fps/fps-pose-tests"
"$ROOT/scripts/16-settings-tests.sh" "${@}"
