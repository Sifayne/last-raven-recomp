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
# You build the tests yourself with your own PSP toolchain (pspdev/pspautotests)
# and point this at the directory of ELFs. Nothing prebuilt is downloaded here,
# and nothing built from them is committed — same policy as game data.
#
# Current capability, stated plainly: each ELF's module_start runs under the
# interpreter with HLE imports bound, and HLE re-entry -- thread starts and
# dispatched callbacks -- is served interpreted and nested. Threads run to
# completion at their start point; that is sequential semantics, not
# scheduling. See docs/findings/autotests.md for the full seam list.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

DIR="${1:-$GAME_DIR/autotests}"
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
    printf '    %-28s %s\n' "$name" "${status:-no output}"
    [ -n "$refused" ] && printf '    %-28s %s\n' "" "$refused"
done

info "$PASS returned cleanly, $FAIL stopped early — details in $REPORTS/07-*.txt"
