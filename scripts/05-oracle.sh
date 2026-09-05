#!/usr/bin/env bash
# Stage 05 — differential oracle.
#
# Runs each discovered function twice, once under the Allegrex interpreter and
# once as recompiled C, and reports where the two disagree. Because both sides
# share the decoder, the CPU state, the memory model and the semantic helpers,
# a genuine disagreement localises to the emitter or to the interpreter's
# sequencing — see tools/psprecomp/docs/ORACLE.md.
#
# Read docs/findings/oracle.md before acting on the output. A reported
# divergence is a lead, not a bug: three separate harness artifacts were found
# and fixed while bringing this up, each of which produced divergences that
# looked exactly like codegen errors.
#
# Usage: scripts/05-oracle.sh [LIMIT] [extra oracle_diff flags...]
#
# LIMIT bounds attempts, not comparisons; 0 runs the whole entry list. A capped
# run strides the list rather than taking a prefix, so it spans the module
# instead of reporting on the bottom of it. Any further arguments go straight to
# oracle_diff — `--verbose`, `--prefix`, or `--text-walk` for the label-coverage
# scan this used to do by default.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need_tool
[ -f "$ELF" ] || die "no $ELF — run scripts/01-extract-decrypt.sh first"

GEN_OBJ="$ROOT/build/host"
BIN="$GEN_OBJ/oracle_diff"
LIMIT="${1:-400}"
FUNCLIST="$REPORTS/05-funclist.txt"

[ -f "$GEN_OBJ/aclr_funcs.o" ] || die "no recompiled objects — run scripts/04-emit-build.sh first"

# Existence is not freshness, and the difference cost a wrong conclusion.
#
# The oracle compares this checkout's *interpreter*, rebuilt on every run,
# against generated C that was emitted whenever 04-emit-build.sh last ran. Edit
# the emitter and re-run only this script and the two sides are no longer the
# same program: the interpreter has the change and the recompiled side does
# not. It reports that as `differ`, which reads exactly like a codegen bug and
# is not one -- a COP1 conversion fix produced precisely this, one function
# diverging in $v0/hi/lo, and it took a detour through the harness to find
# that the artifact was 54 minutes older than the emitter.
#
# So: refuse, rather than warn. A warning in a 300-line log is a warning that
# gets read after the wrong conclusion has already been drawn.
# The *sources* that decide what gets emitted, not the binary: allegrexrecomp
# is relinked by any build of the toolkit, so testing its mtime would force a
# full re-emit after an unrelated change to the HLE.
for src in "$ROOT/tools/psprecomp/tools/allegrexrecomp/emit.c" \
           "$ROOT/tools/psprecomp/tools/allegrexrecomp/decode.c" \
           "$ROOT/tools/psprecomp/tools/allegrexrecomp/decode.h" \
           "$ROOT/tools/psprecomp/include/psprecomp/recomp_rt.h"; do
    [ -e "$src" ] || continue
    [ "$src" -nt "$GEN_OBJ/aclr_funcs.o" ] && die \
        "$(basename "$src") is newer than the recompiled objects — the emitted C
     predates it, so a 'differ' here would be a stale artifact rather than a
     codegen bug. Run scripts/04-emit-build.sh first."
done

info "building oracle_diff"
cc -c -O1 -I "$ROOT/tools/psprecomp/include" \
         -I "$ROOT/tools/psprecomp/tools/allegrexrecomp" \
         -o "$GEN_OBJ/oracle_diff.o" "$ROOT/host/oracle_diff.c"

# Replacements are linked here too, because the emitted objects reference their
# symbols. Note what that means for a comparison: this harness runs
# psp_func_<addr> against the interpreter executing the original instructions,
# so a *replaced* function differs by construction rather than by codegen fault.
# Exclude replaced addresses from a sweep instead of reading the differ as a
# bug -- see host/replace.txt.
cc -o "$BIN" "$GEN_OBJ/oracle_diff.o" \
       "$GEN_OBJ/aclr_funcs.o" "$GEN_OBJ/aclr_imports.o" \
       "$GEN_OBJ/replacements.o" \
       "$ROOT/build/psprecomp/tools/allegrexrecomp/liballegrex_core.a" \
       "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread $HOST_LIBS

# The work-list is the discovered function *entries*, not every address the
# dispatch table happens to resolve. The table also holds interior labels —
# roughly five labels for every entry on this module — and entering a function
# past its own prologue is not a call, so those comparisons mean nothing and the
# $sp-balance check discards them only after paying to run each one.
#
# Exact counts are deliberately not quoted here: they move every time discovery
# is rebuilt. The run prints its own worklist size and sampled span.
#
# The .text walk still answers how much of the table is reachable at all, so it
# stays available; asking for it means the entry list is not the work-list.
WORKLIST=()
for arg in "${@:2}"; do
    [ "$arg" = "--text-walk" ] && WORKLIST=(none)
done

if [ "${WORKLIST[0]:-}" = "none" ]; then
    WORKLIST=()
    info "walking .text — every dispatch-table address, interior labels included"
else
    # Regenerated every time rather than cached. It costs about 50 ms, and a
    # stale list silently tests entries the emitted C was not built from.
    info "listing function entries"
    "$AR" funcs "$ELF" --list > "$FUNCLIST"
    NENTRIES="$(grep -c '^0x' "$FUNCLIST" || true)"
    [ "$NENTRIES" -gt 0 ] || die "no function entries in $FUNCLIST — did discovery fail?"
    info "$NENTRIES entries -> $FUNCLIST"
    WORKLIST=(--funcs "$FUNCLIST")
fi

if [ "$LIMIT" = 0 ]; then
    info "comparing every entry (interpreter vs recompiled C)"
else
    info "comparing up to $LIMIT functions, strided across the module"
fi
# stderr carries the runtime's own unimplemented-firmware chatter, which is
# expected here and drowns the result. The report goes to stdout.
"$BIN" "$ELF" "${WORKLIST[@]}" --limit "$LIMIT" "${@:2}" 2>/dev/null \
    | tee "$REPORTS/05-oracle.txt"

echo
info "wrote $REPORTS/05-oracle.txt"
