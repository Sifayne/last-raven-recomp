#!/usr/bin/env bash
# Build both vendored tools.
#
# There is nothing to apply first any more. tools/psprecomp used to be a
# pristine upstream checkout that a 152-patch series was replayed over on every
# build; it is now a checkout of our fork, tracked like any other submodule, and
# `git log upstream/main..` is the delta the series used to describe.

set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
. "$ROOT/scripts/ffmpeg-config.sh"

# Configure before common.sh checks the cache, so this also migrates existing
# checkouts that previously discovered a GPL-enabled system FFmpeg.
python3 "$ROOT/scripts/ffmpeg.py" build
cmake -S tools/psprecomp -B build/psprecomp -G Ninja -DCMAKE_BUILD_TYPE=Release \
    "${FFMPEG_CMAKE_ARGS[@]}"
source "$ROOT/scripts/common.sh"

info "building psprecomp"
cmake --build build/psprecomp -j"$(nproc)"

info "running psprecomp test suite"
ctest --test-dir build/psprecomp -C Release --output-on-failure

# pspdecrypt handles the mode-9 header transform that psprecomp's own decryptor
# has not implemented yet (docs/DECRYPT.md calls this out and recommends it).
info "building pspdecrypt"
make -C tools/pspdecrypt -j"$(nproc)" >/dev/null 2>&1 || make -C tools/pspdecrypt -j"$(nproc)"

info "tools ready"
