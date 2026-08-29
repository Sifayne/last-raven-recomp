#!/usr/bin/env bash
# Stage 04 — emit C, compile it, and link it.
#
# The Phase 0 bar. psprecomp claims a retail module "compiles, links, and
# executes"; this is where that claim gets tested against Last Raven rather
# than against the toolkit's own bring-up title.
#
# Linking needs three symbols the generated header declares but the runtime
# does not define — they are the host's contract. host/link_probe.c supplies
# the minimum. It does not start the game: that needs a thread scheduler, the
# missing HLE, and a GE that rasterises. See docs/findings/phase0.md.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need_tool
[ -f "$ELF" ] || die "no $ELF — run scripts/01-extract-decrypt.sh first"

PREFIX=aclr
OBJ="$ROOT/build/host"
mkdir -p "$OBJ"

info "emitting C"
rm -rf "${GEN:?}"/*
"$AR" emit "$ELF" "$GEN" "$PREFIX" | tee "$REPORTS/04-emit.txt"
echo
info "$(cat "$GEN"/*.c | wc -l) lines of C in $(ls "$GEN" | wc -l) files, $(du -sh "$GEN" | cut -f1)"

# -O0 on purpose: this is 2.1M lines and the question is whether it is valid C,
# not how fast it runs. Optimised builds are a Phase 1 concern.
CFLAGS=(-O0 -I "$ROOT/tools/psprecomp/include")

# TRACE=1 records every function entry, so a failure names the functions that
# led to it rather than just the one it happened in. Off by default: it costs a
# store per call, and the ring is only useful when something has gone wrong.
if [ "${TRACE:-0}" != "0" ]; then
    CFLAGS+=(-DPSPRECOMP_TRACE)
    info "trace instrumentation ON"
fi

info "compiling (~45s)"
for src in "$GEN"/*.c; do
    cc -c "${CFLAGS[@]}" -o "$OBJ/$(basename "${src%.c}").o" "$src"
done
cc -c "${CFLAGS[@]}" -o "$OBJ/link_probe.o" "$ROOT/host/link_probe.c"

info "linking"
# Named explicitly rather than globbed: stage 05 also builds into this
# directory, and sweeping up its objects drags the oracle harness into the
# probe with none of the libraries it needs.
GEN_OBJS=()
for src in "$GEN"/*.c; do GEN_OBJS+=("$OBJ/$(basename "${src%.c}").o"); done
cc -o "$OBJ/${PREFIX}_probe" "${GEN_OBJS[@]}" "$OBJ/link_probe.o" \
      "$ROOT/build/psprecomp/libpsprecomp.a" -lm

info "link closed: $(du -h "$OBJ/${PREFIX}_probe" | cut -f1) executable"
"$OBJ/${PREFIX}_probe"
