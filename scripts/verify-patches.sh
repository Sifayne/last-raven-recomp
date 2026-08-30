#!/usr/bin/env bash
# Verify the patch series against a pristine submodule checkout.
#
# state.md's bar for patches/: apply the whole series to a pristine checkout
# and compare every file the series owns against this checkout's working tree
# -- byte-identical. Green tests only approximate it, because a stale build
# directory happily reports 12/12 for code you did not build. This script is
# that check, so it gets run instead of being a ritual described in prose.
#
# With --build the pristine clone is also configured, compiled and tested,
# which is the other half of the bar ("builds green there"). Slower, so
# opt-in: the tree diff is what usually fails (a hand-edit in the submodule
# that never made it into a patch, or a regenerated patch missing a file it
# creates -- the `git add -N` trap).
#
# Comparison is by git tree hash with untracked non-ignored files included,
# which respects the submodule's .gitignore and counts files the series
# creates that exist only as untracked files in the working tree.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

BUILD=0
[ "${1:-}" = "--build" ] && BUILD=1

TMP="$(mktemp -d "${TMPDIR:-/tmp}/verify-patches.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

SUB="tools/psprecomp"

# The clone is of the submodule's committed HEAD, so it is pristine however
# dirty the working tree is -- that is the point.
info "cloning pristine $SUB"
git clone --quiet "$ROOT/$SUB" "$TMP/$SUB"

shopt -s nullglob
PATCHES=("$ROOT"/patches/*.patch)
shopt -u nullglob
[ ${#PATCHES[@]} -gt 0 ] || die "no patches in patches/"

info "applying ${#PATCHES[@]} patches in order"
for p in "${PATCHES[@]}"; do
    if ! git -C "$TMP/$SUB" apply --whitespace=nowarn "$p"; then
        die "$(basename "$p") does not apply to a pristine checkout -- the submodule moved without the series being regenerated"
    fi
done

# Tree of the patched pristine clone.
TREE_CLONE="$(git -C "$TMP/$SUB" add -A && git -C "$TMP/$SUB" write-tree)"

# Tree of this checkout's submodule working tree, via a throwaway index so the
# real index is not touched.
export GIT_INDEX_FILE="$TMP/index"
TREE_WORK="$(git -C "$ROOT/$SUB" add -A && git -C "$ROOT/$SUB" write-tree)"
unset GIT_INDEX_FILE

if [ "$TREE_CLONE" = "$TREE_WORK" ]; then
    info "tree diff: byte-identical"
else
    printf '\033[31merror:\033[0m the series and the working tree differ. Files affected:\n' >&2
    diff -rq --no-dereference -x .git -x build -x out "$ROOT/$SUB" "$TMP/$SUB" | sed 's/^/    /' >&2
    die "regenerate the affected patch (see state.md, 'The patch series')"
fi

if [ "$BUILD" -eq 1 ]; then
    info "building the pristine clone (--build)"
    cmake -S "$TMP/$SUB" -B "$TMP/$SUB/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$TMP/$SUB/build" -j"$(nproc)"
    ctest --test-dir "$TMP/$SUB/build" -C Release --output-on-failure
fi

info "patch series verified"
