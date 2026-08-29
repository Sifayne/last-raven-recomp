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

info "building oracle_diff"
cc -c -O1 -I "$ROOT/tools/psprecomp/include" \
         -I "$ROOT/tools/psprecomp/tools/allegrexrecomp" \
         -o "$GEN_OBJ/oracle_diff.o" "$ROOT/host/oracle_diff.c"

cc -o "$BIN" "$GEN_OBJ/oracle_diff.o" \
       "$GEN_OBJ/aclr_funcs.o" "$GEN_OBJ/aclr_imports.o" \
       "$ROOT/build/psprecomp/tools/allegrexrecomp/liballegrex_core.a" \
       "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread

# The work-list is the discovered function *entries*, not every address the
# dispatch table happens to resolve. The table also holds interior labels —
# 42,681 of them against 16,494 entries on this module — and entering a function
# past its own prologue is not a call, so those comparisons mean nothing and the
# $sp-balance check discards them only after paying to run each one.
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
