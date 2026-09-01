#!/usr/bin/env bash
# Stage 08 — survey every pspautotests directory at once.
#
# 07-autotests.sh works one directory at the real instruction budget, which is
# right when you are fixing a suite and wrong for a landscape survey: an
# unknown fraction of 400+ tests exercise subsystems that are not implemented
# at all, and a single one spinning to 800M instructions costs more wall clock
# than the rest of the sweep. So this runs the same comparison with a per-test
# wall timeout and a reduced budget, and writes a TSV instead of a report.
#
# Read the output as a map, not as a verdict. A reduced budget truncates tests
# that legitimately run long -- cpu/vfpu/vector needs the full 800M and shows
# 3281 differing lines here against 8 at the real budget -- so anything the
# sweep flags as interesting gets re-run through 07-autotests.sh before a line
# of code is written about it.
#
#   scripts/08-autotest-sweep.sh              # everything
#   BUDGET=800000000 TMO=120 scripts/08-autotest-sweep.sh
#
# Columns: test, verdict, differing-lines, stop-reason.
# Ranking by the third column is the point -- a test differing by one line is
# one bug away, a test differing by five hundred is an unimplemented library,
# and the two deserve very different amounts of attention.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

TESTS="${1:-$GAME_DIR/pspautotests/tests}"
BUDGET="${BUDGET:-200000000}"
TMO="${TMO:-25}"
OUT="$REPORTS/08-sweep.tsv"
# Not WORK: common.sh already uses that name for game/extracted.
SWEEPFS="$REPORTS/sweepfs"

[ -d "$TESTS" ] || die "no pspautotests at $TESTS — see scripts/07-autotests.sh for the clone command"
# Absolute, because each test is run from a working directory of its own and a
# relative path stops resolving the moment we cd into it.
TESTS="$(cd "$TESTS" && pwd)"

mkdir -p "$REPORTS"
: > "$OUT"
total=0; match=0

while read -r f; do
    exp="${f%.*}.expected"
    [ -f "$exp" ] || continue
    rel="${f#"$TESTS"/}"
    total=$((total + 1))
    rm -rf "$SWEEPFS"; mkdir -p "$SWEEPFS/host"
    # An `if`, not a bare subshell followed by `rc=$?`: under `set -e` a
    # failing subshell ends the run before the assignment executes, and most
    # of these tests exit non-zero. 07-autotests.sh has the same construct for
    # the same reason.
    if ( cd "$SWEEPFS" && timeout "$TMO" "$AR" interp "$f" --dispatch \
            --budget "$BUDGET" >"$SWEEPFS/run.log" 2>&1 ); then rc=0; else rc=$?; fi

    got="$SWEEPFS/host/__testoutput.txt"
    [ -f "$got" ] || : > "$got"
    tr -d '\r' < "$got" > "$SWEEPFS/g"
    tr -d '\r' < "$exp" > "$SWEEPFS/e"
    # Ten of the 435 .expected files end without a newline the guest did print;
    # see the longer note in 07-autotests.sh. Both scripts have to agree about
    # this or the sweep and the per-directory run give different verdicts for
    # the same test, which is worse than either answer on its own.
    [ -s "$SWEEPFS/e" ] && [ -n "$(tail -c1 "$SWEEPFS/e")" ] && printf '\n' >> "$SWEEPFS/e"
    [ -s "$SWEEPFS/g" ] && [ -n "$(tail -c1 "$SWEEPFS/g")" ] && printf '\n' >> "$SWEEPFS/g"
    :

    # Compare before concluding anything from an empty file -- see the note in
    # 07-autotests.sh. Tests that check a framebuffer have an empty .expected,
    # and for those, printing nothing is the correct answer.
    if cmp -s "$SWEEPFS/g" "$SWEEPFS/e"; then
        verdict="MATCH"; n=0; match=$((match + 1))
    elif [ ! -s "$SWEEPFS/g" ]; then
        verdict="NOOUTPUT"; n=0
    else
        verdict="DIFFER"; n=$(diff "$SWEEPFS/g" "$SWEEPFS/e" | grep -c '^[<>]' || true)
    fi

    stop="$(grep -m1 '^stopped:' "$SWEEPFS/run.log" | cut -d' ' -f2- | tr '\t' ' ' || true)"
    [ "$rc" = 124 ] && stop="TIMEOUT(${TMO}s)"
    printf '%s\t%s\t%s\t%s\n' "$rel" "$verdict" "$n" "${stop:-none}" >> "$OUT"
done < <(find "$TESTS" -name '*.prx' | sort)

rm -rf "$SWEEPFS"
info "$match of $total match hardware — $OUT"
info "closest misses:"
awk -F'\t' '$2=="DIFFER" && $3+0<=4 {printf "    %-44s %s line(s)\n", $1, $3}' "$OUT" | head -20
