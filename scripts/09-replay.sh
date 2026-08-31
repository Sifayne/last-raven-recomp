#!/usr/bin/env bash
# Run the boot host against a scenario file, and keep the logs.
#
# 06-boot.sh runs the game. This runs it *the same way twice*, which is the
# thing that was missing: a bug three menus in is only worth chasing if
# reaching it is a command rather than a person with a gamepad.
#
# The build recipe is not duplicated here. 06-boot.sh owns it -- it probes for
# SDL2 before compiling boot.c, and that ordering is load-bearing -- so this
# invokes it with BOOT_NO_RUN=1 and execs the result.
set -euo pipefail
. "$(dirname "$0")/common.sh"

usage() {
    cat <<'EOF'
usage: scripts/09-replay.sh [options] <scenario.pad> [elf] [iso]

  --window          run in an SDL window (implies real-time pacing: only @poll
                    stamps reproduce, seconds do not)
  --record <file>   also write a scenario from what the game actually saw
  --decode          PSPRECOMP_MPEG_DECODE=1 -- required to get past the intro
  --trace           use the PSPRECOMP_TRACE build, so faults name a function
  --drain <s>       run length; overrides the scenario's own `drain` header
  --stop <n>        stop the run at the nth bad memory access
  --gdb [n]         SIGTRAP at the nth bad access (default 1) under gdb
  --merge           one interleaved log instead of separate stdout/stderr
  --repeat <n>      run n times and diff the summaries -- the determinism check
  --env K=V         pass an extra environment variable (repeatable)

Logs land in reports/09-<scenario>-<stamp>.{txt,err}.
EOF
    exit "${1:-0}"
}

WINDOW="" RECORD="" DECODE="" TRACE_ON="" DRAIN="" STOP="" GDB="" MERGE="" REPEAT=1
EXTRA_ENV=()
while [ $# -gt 0 ]; do
    case "$1" in
        --window)  WINDOW=1; shift ;;
        --record)  RECORD="$2"; shift 2 ;;
        --decode)  DECODE=1; shift ;;
        --trace)   TRACE_ON=1; shift ;;
        --drain)   DRAIN="$2"; shift 2 ;;
        --stop)    STOP="$2"; shift 2 ;;
        --gdb)     if [ "${2:-}" ] && [ -z "${2##[0-9]*}" ]; then GDB="$2"; shift 2
                   else GDB=1; shift; fi ;;
        --merge)   MERGE=1; shift ;;
        --repeat)  REPEAT="$2"; shift 2 ;;
        --env)     EXTRA_ENV+=("$2"); shift 2 ;;
        -h|--help) usage 0 ;;
        --*)       die "unknown option $1 (try --help)" ;;
        *)         break ;;
    esac
done

[ $# -ge 1 ] || usage 2
SCENARIO="$1"; shift
[ -f "$SCENARIO" ] || die "no scenario at $SCENARIO"

ELF="${1:-$ROOT/game/extracted/ACLR_App.elf}"
ISO="${2:-$(ls "$ROOT"/game/*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ELF" ] || die "no module at $ELF"

OUT="$ROOT/build/host"
[ -n "$TRACE_ON" ] && OUT="$ROOT/build/host-trace"

info "building"
BOOT_NO_RUN=1 TRACE="${TRACE_ON:-0}" "$ROOT/scripts/06-boot.sh" >/dev/null

export PSPRECOMP_REPLAY="$SCENARIO"
[ -n "$WINDOW" ] && export PSPRECOMP_WINDOW=1
[ -n "$DECODE" ] && export PSPRECOMP_MPEG_DECODE=1
[ -n "$RECORD" ] && export PSPRECOMP_REPLAY_REC="$RECORD"
[ -n "$DRAIN"  ] && export PSPRECOMP_DRAIN="$DRAIN"
[ -n "$STOP"   ] && export PSPRECOMP_BAD_STOP="$STOP"
[ -n "$GDB"    ] && export PSPRECOMP_BAD_TRAP="$GDB"
for kv in ${EXTRA_ENV+"${EXTRA_ENV[@]}"}; do export "${kv?}"; done

# Cap stderr, never stdout.
#
# stdout is the boot summary: a page of printf, and the thing anyone actually
# reads. stderr is every instrument, and state.md records PSPRECOMP_SEMA=Movie
# producing 2.5 GB of it in sixty seconds.
#
# The cap is an awk filter and NOT `head -c`, deliberately: head closes the
# pipe when it has enough, the host takes SIGPIPE mid-run, and the run being
# measured dies of the measurement. This keeps draining stdin forever and just
# stops writing.
MAXLOG="${MAXLOG:-268435456}"
cap() {
    awk -v max="$MAXLOG" -v path="$1" '
        BEGIN { n = 0; capped = 0 }
        {
            if (capped) next
            n += length($0) + 1
            if (n > max) {
                print "--- log capped at " max " bytes; the run continued ---" > path
                capped = 1
                next
            }
            print > path
        }
        END { fflush(path) }'
}

name="$(basename "$SCENARIO" .pad)"
rc_any=0
summaries=()

for i in $(seq 1 "$REPEAT"); do
    stamp="$(date +%Y%m%d-%H%M%S)"
    [ "$REPEAT" -gt 1 ] && stamp="$stamp-run$i"
    log="$REPORTS/09-$name-$stamp.txt"
    err="$REPORTS/09-$name-$stamp.err"

    info "running $SCENARIO${REPEAT:+ ($i/$REPEAT)} -> $(basename "$log")"

    set +e
    if [ -n "$GDB" ]; then
        # SIGTRAP is only useful with a debugger to catch it; without one the
        # process dies and there is no summary at all.
        gdb -q -batch -ex run -ex "bt 40" -ex "info registers" \
            --args "$OUT/boot" "$ELF" ${ISO:+"$ISO"} 2>&1 | tee "$log"
        rc=${PIPESTATUS[0]}
    elif [ -n "$MERGE" ]; then
        "$OUT/boot" "$ELF" ${ISO:+"$ISO"} 2>&1 | tee "$log"
        rc=${PIPESTATUS[0]}
    else
        "$OUT/boot" "$ELF" ${ISO:+"$ISO"} \
            2> >(cap "$err") \
            | tee "$log"
        rc=${PIPESTATUS[0]}
    fi
    set -e

    [ "$rc" -ne 0 ] && rc_any=$rc
    summaries+=("$log")
done

if [ "$REPEAT" -gt 1 ]; then
    # The determinism check. Wall-clock quantities are filtered out: raster
    # time and the drain measure the host, not the guest, and will never match
    # between two runs. What must match is the guest-visible trajectory.
    info "comparing $REPEAT runs (wall-clock lines filtered)"
    strip() { sed -e '/raster time/d' -e '/^ *drain /d' -e '/drain  *[0-9]*s/d' "$1"; }
    ok=1
    for ((i = 1; i < ${#summaries[@]}; i++)); do
        if ! diff <(strip "${summaries[0]}") <(strip "${summaries[i]}") > /dev/null; then
            ok=0
            diff <(strip "${summaries[0]}") <(strip "${summaries[i]}") | head -40
        fi
    done
    if [ "$ok" = 1 ]; then
        info "identical: the guest-visible trajectory reproduced across $REPEAT runs"
    else
        die "runs differ -- see the diff above"
    fi
fi

exit "$rc_any"
