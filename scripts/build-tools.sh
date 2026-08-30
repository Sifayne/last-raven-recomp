#!/usr/bin/env bash
# Build both vendored tools, applying the local patches first.
#
# The patches are upstream bugs found during Phase 0 bring-up, kept here rather
# than as submodule commits so a fresh clone reproduces the same build. See
# docs/findings/phase0.md for what each one fixes. Re-running is safe: a patch
# that is already applied is skipped.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

info "applying patches to tools/psprecomp"
SERIES_VERIFIED=""
for p in "$ROOT"/patches/*.patch; do
    [ -e "$p" ] || continue
    if git -C tools/psprecomp apply --check "$p" 2>/dev/null; then
        git -C tools/psprecomp apply "$p"
        echo "    applied  $(basename "$p")"
    elif git -C tools/psprecomp apply --reverse --check "$p" 2>/dev/null; then
        echo "    already  $(basename "$p")"
    elif [ -z "$SERIES_VERIFIED" ] && "$ROOT/scripts/verify-patches.sh" >/dev/null 2>&1; then
        # Per-patch reverse-checks assume each patch owns its hunks outright.
        # The series' boundaries have drifted at least once (0001 carries
        # 0011's umd.c line), so one patch's reverse-check can fail even
        # though the tree already holds exactly what the series produces.
        # verify-patches is the authority: if the tree is byte-identical to
        # pristine-plus-series, every patch is already applied.
        SERIES_VERIFIED=1
        echo "    already  $(basename "$p") (whole series verified)"
    elif [ -n "$SERIES_VERIFIED" ]; then
        echo "    already  $(basename "$p")"
    else
        die "$(basename "$p") does not apply and is not already applied — submodule may have moved"
    fi
done

info "building psprecomp"
cmake -S tools/psprecomp -B build/psprecomp -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build/psprecomp -j"$(nproc)"

info "running psprecomp test suite"
ctest --test-dir build/psprecomp -C Release --output-on-failure

# pspdecrypt handles the mode-9 header transform that psprecomp's own decryptor
# has not implemented yet (docs/DECRYPT.md calls this out and recommends it).
info "building pspdecrypt"
make -C tools/pspdecrypt -j"$(nproc)" >/dev/null 2>&1 || make -C tools/pspdecrypt -j"$(nproc)"

info "tools ready"
