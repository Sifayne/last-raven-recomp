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

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[ -f "$ELF" ] || die "no $ELF — run scripts/01-extract-decrypt.sh first"

GEN_OBJ="$ROOT/build/host"
BIN="$GEN_OBJ/oracle_diff"
LIMIT="${1:-400}"

[ -f "$GEN_OBJ/aclr_funcs.o" ] || die "no recompiled objects — run scripts/04-emit-build.sh first"

info "building oracle_diff"
cc -c -O1 -I "$ROOT/tools/psprecomp/include" \
         -I "$ROOT/tools/psprecomp/tools/allegrexrecomp" \
         -o "$GEN_OBJ/oracle_diff.o" "$ROOT/host/oracle_diff.c"

cc -o "$BIN" "$GEN_OBJ/oracle_diff.o" \
       "$GEN_OBJ/aclr_funcs.o" "$GEN_OBJ/aclr_imports.o" \
       "$ROOT/build/psprecomp/tools/allegrexrecomp/liballegrex_core.a" \
       "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread

info "comparing $LIMIT functions (interpreter vs recompiled C)"
# stderr carries the runtime's own unimplemented-firmware chatter, which is
# expected here and drowns the result. The report goes to stdout.
"$BIN" "$ELF" --limit "$LIMIT" 2>/dev/null | tee "$REPORTS/05-oracle.txt"

echo
info "wrote $REPORTS/05-oracle.txt"
