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
# interpreter with HLE imports bound. A test's main thread is started by HLE
# thread creation, and the interpreter does not yet service that re-entry — it
# counts it. So a test today exercises its startup path and its direct
# module_start code, and the report says how much never ran.

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
    if "$AR" interp "$f" --budget "$BUDGET" > "$out" 2>&1; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
    fi
    # Pull the one-line status out of the report so the summary is readable.
    # `|| true` because an absent pattern exits 1 under pipefail, and set -e
    # would take the whole run down with it.
    status="$(grep -m1 '^stopped:' "$out" | cut -d' ' -f2- || true)"
    reentry="$(grep -m1 '^re-entry:' "$out" | cut -d' ' -f2- || true)"
    printf '    %-28s %s\n' "$name" "${status:-no output}"
    [ -n "$reentry" ] && printf '    %-28s %s\n' "" "$reentry"
done

info "$PASS returned cleanly, $FAIL stopped early — details in $REPORTS/07-*.txt"
