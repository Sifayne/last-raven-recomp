#!/usr/bin/env bash
# Stage 07 — run pspautotests through the interpreter.
#
# The differential oracle validates *translation*, and everything left is
# *environment* — the HLE, the scheduler, save data. Anything missing from the
# execution environment is missing from both sides of that oracle, so it agrees
# perfectly and sees nothing. pspautotests are small PSP programs with
# assertions in them; run through the interpreter they give ground truth where
# the oracle is structurally blind. See docs/findings/autotests.md for what
# this can and cannot tell you yet.
#
# The tests live in hrydgard/pspautotests, and no toolchain is needed to run
# them: the .prx binaries are committed there alongside .expected files holding
# real-hardware output. Clone it into game/ (gitignored) and point this at a
# test directory. Nothing is downloaded by this script and nothing from it is
# committed — same policy as game data.
#
#   git clone --depth 1 https://github.com/hrydgard/pspautotests game/pspautotests
#   scripts/07-autotests.sh game/pspautotests/tests/cpu/vfpu
#
# Current capability, stated plainly: each ELF's module_start runs under the
# interpreter with HLE imports bound, and HLE re-entry -- thread starts and
# dispatched callbacks -- is served interpreted and nested. Threads run to
# completion at their start point; that is sequential semantics, not
# scheduling. See docs/findings/autotests.md for the full seam list.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DIR="${1:-$GAME_DIR/pspautotests/tests/cpu/vfpu}"
BUDGET="${BUDGET:-100000000}"
[ -d "$DIR" ] || die "no autotest directory at $DIR — build pspautotests and point this at it"

shopt -s nullglob
ELFS=("$DIR"/*.elf "$DIR"/*.prx)
shopt -u nullglob
[ ${#ELFS[@]} -gt 0 ] || die "no .elf/.prx in $DIR"

mkdir -p "$REPORTS"
PASS=0; FAIL=0

for f in "${ELFS[@]}"; do
    name="$(basename "$f")"
    out="$REPORTS/07-${name%.*}.txt"
    info "interp $name (budget $BUDGET)"
    # --dispatch: a test's main thread is started by HLE and its callbacks
    # dispatch into guest code; serving them is what makes anything past
    # module_start execute at all. See docs/findings/autotests.md.
    if "$AR" interp "$f" --dispatch --budget "$BUDGET" > "$out" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    # Pull the one-line status out of the report so the summary is readable.
    # `|| true` because an absent pattern exits 1 under pipefail, and set -e
    # would take the whole run down with it.
    status="$(grep -m1 '^stopped:' "$out" | cut -d' ' -f2- || true)"
    refused="$(grep -m1 '^re-entry:' "$out" | cut -d' ' -f2- || true)"

    # Compare against the recorded hardware output, which is the entire point:
    # a test that runs and returns has proved nothing until what it printed is
    # checked. The guest's own prints are interleaved with the interpreter's
    # report, so the comparison takes the lines between the two `---` markers.
    exp="${f%.*}.expected"
    verdict="no .expected"
    if [ -f "$exp" ]; then
        got="$REPORTS/07-${name%.*}.got"
        sed -n '/^---$/,/^---$/p' "$out" | sed '1d;$d' > "$got"
        if [ ! -s "$got" ]; then
            verdict="NO OUTPUT (test ran but printed nothing)"
        elif diff -q "$got" "$exp" >/dev/null 2>&1; then
            verdict="MATCHES hardware"
        else
            verdict="differs: $(diff "$got" "$exp" | grep -c '^[<>]') line(s)"
        fi
    fi
    printf '    %-20s %-22s %s\n' "$name" "${status:-no output}" "$verdict"
    [ -n "$refused" ] && printf '    %-20s %s\n' "" "$refused"
done

info "$PASS returned cleanly, $FAIL stopped early — details in $REPORTS/07-*.txt"
