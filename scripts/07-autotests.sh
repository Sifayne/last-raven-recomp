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
# A cap, not a cost: a test that finishes stops on its own, so raising this
# only changes what happens to the ones that do not. At 100M, cpu/vfpu/vector
# was reported as "instruction budget exhausted" when what it actually needs is
# more than 100M -- a misleading reason, and the wrong thing to go and
# investigate. (This note used to go on to say the test then hit an
# unimplemented instruction. It did not: it was cut off by the interp's 10 s
# drain, see DRAIN_S below, and runs to its last line at this budget.)
BUDGET="${BUDGET:-800000000}"
DRAIN_S="${DRAIN_S:-600}"
[ -d "$DIR" ] || die "no autotest directory at $DIR — build pspautotests and point this at it"

shopt -s nullglob
ELFS=("$DIR"/*.elf "$DIR"/*.prx)
shopt -u nullglob
[ ${#ELFS[@]} -gt 0 ] || die "no .elf/.prx in $DIR"

mkdir -p "$REPORTS"
PASS=0; FAIL=0

# Where `host0:/__testoutput.txt` lands.
#
# iofilemgr resolves a device to `<root>/<device>/<path>`, and the root is the
# process's working directory -- nothing calls psp_io_set_root. From the repo
# root that puts `host0:` on top of host/, which is the boot host's *source*
# directory, so a test writes its scratch files in among boot.c. Run each test
# from a directory of our own instead, and the guest's writes land there.
HOSTFS="$REPORTS/hostfs"
rm -rf "$HOSTFS"; mkdir -p "$HOSTFS/host"
HOSTOUT="$HOSTFS/host/__testoutput.txt"

for f in "${ELFS[@]}"; do
    name="$(basename "$f")"
    out="$REPORTS/07-${name%.*}.txt"
    rm -f "$HOSTOUT"
    info "interp $name (budget $BUDGET)"
    # --dispatch: a test's main thread is started by HLE and its callbacks
    # dispatch into guest code; serving them is what makes anything past
    # module_start execute at all. See docs/findings/autotests.md.
    # --drain: how long the interp waits for the test's threads after
    # module_start returns, in wall seconds. Its own default of 10 is sized
    # for the sweep's per-test timeout and is wrong here: at the real budget a
    # test is bounded by its instructions, not the clock, and cpu/vfpu/vector
    # needs well over 10 s on the interpreter -- it was cut mid-line at 4452
    # of 5329 for a week and reported as an unimplemented instruction.
    if (cd "$HOSTFS" && "$AR" interp "$f" --dispatch --budget "$BUDGET" --drain "$DRAIN_S") > "$out" 2>&1; then
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
    # checked.
    #
    # The output does not come back on stdout. The harness redirects its own
    # stdout to `host0:/__testoutput.txt` -- usbhostfs, which is how a test
    # talks to the PC it is tethered to -- and our iofilemgr maps `host0:` to
    # host/. So the file is the output, and reading the console instead is what
    # made every test look like it printed nothing.
    #
    # Removed before the run, not just after: a stale file from the previous
    # test would otherwise be compared against this one's .expected, and a test
    # that emitted nothing would inherit its predecessor's verdict.
    exp="${f%.*}.expected"
    verdict="no .expected"
    if [ -f "$exp" ]; then
        got="$REPORTS/07-${name%.*}.got"
        [ -f "$HOSTOUT" ] && cp "$HOSTOUT" "$got" || : > "$got"
        # Compare with CR stripped from both sides.
        #
        # cpu/vfpu/vector.expected is CRLF and every other .expected in the
        # same directory is LF -- an artifact of whichever machine captured it,
        # not something the PSP printed. Left alone it decides that test's
        # verdict entirely: all 5329 lines "differ", every one of them by a
        # byte the guest never emitted.
        expc="$REPORTS/07-${name%.*}.exp"
        gotc="$REPORTS/07-${name%.*}.cmp"
        tr -d '\r' < "$exp" > "$expc"
        tr -d '\r' < "$got" > "$gotc"
        # And the same kind of artifact at the other end of the file: ten of the
        # 435 .expected files have no final newline, though the guest printed
        # one -- threads/k0/k0's last statement is a printf ending in \n. Left
        # alone it costs those tests their verdict over a byte the recording
        # dropped rather than one the PSP withheld. Empty files are left empty,
        # since silence is the right answer for the framebuffer tests below.
        [ -s "$expc" ] && [ -n "$(tail -c1 "$expc")" ] && printf '\n' >> "$expc"
        [ -s "$gotc" ] && [ -n "$(tail -c1 "$gotc")" ] && printf '\n' >> "$gotc"
        :
        # Compare *before* concluding anything from an empty file.
        #
        # "printed nothing" is only a failure when hardware printed something.
        # Several gpu tests check a framebuffer rather than text and their
        # .expected is empty, so silence is the correct answer -- and reporting
        # it as NO OUTPUT hid the fix that produced it. Implementing
        # sceDisplayGetFrameBuf turned nineteen of them from a flood of
        # "ERROR: Invalid format" into exactly the silence hardware produces,
        # and this branch called every one of them a failure.
        if diff -q "$gotc" "$expc" >/dev/null 2>&1; then
            verdict="MATCHES hardware"
        elif [ ! -s "$got" ]; then
            verdict="NO OUTPUT (test ran but printed nothing)"
        else
            # `|| true` because diff exits 1 when the files differ, pipefail
            # promotes that to the whole substitution, and set -e then ends the
            # run on the first test that differs. Latent until now: while every
            # test emitted nothing this branch was unreachable.
            ndiff="$(diff "$gotc" "$expc" | grep -c '^[<>]' || true)"
            verdict="differs: $ndiff line(s)"
        fi
    fi
    printf '    %-20s %-22s %s\n' "$name" "${status:-no output}" "$verdict"
    # An `if` rather than `&&`: as the last command of the loop body, a false
    # test is the body's exit status, and set -e would end the run on the first
    # report that happens not to carry the line.
    if [ -n "$refused" ]; then printf '    %-20s %s\n' "" "$refused"; fi
done

info "$PASS returned cleanly, $FAIL stopped early — details in $REPORTS/07-*.txt"
