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

# PREFIX (the emit prefix) and the build directory come from common.sh, keyed
# on GAME. A traced build and a plain one are different objects answering
# different questions, and they used to share a directory: TRACE=1 only changed
# CFLAGS, so it silently overwrote the fast build's objects and left a
# build/host-trace that someone had moved aside by hand. Keep them apart, so
# having one never means losing the other. OPT=1 likewise compiles the
# generated code at -O2 into its own directory, so the -O0 build the rest of
# the project links against stays where it is -- see the compile step below;
# scripts/06-boot.sh and 09-replay.sh take the same OPT.
OBJ="$(host_build_dir)"
mkdir -p "$OBJ"

info "emitting C"
rm -rf "${GEN:?}"/*
# The replace list (host/replace.txt, or host/replace-<slug>.txt for another
# title) names the functions this project implements natively. Their bodies
# are still emitted, as psp_func_<addr>__orig, but the public symbol is left
# for the replacements source to define. An empty list changes nothing.
"$AR" emit "$ELF" "$GEN" "$PREFIX" --replace @"$REPLACE_LIST" \
    | tee "$REPORTS/04-emit.txt"
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

if [ "${OPT:-0}" != "0" ]; then
    # The optimised build. One 2.1M-line translation unit at -O2 is not
    # practical, but the emitted file splits at function boundaries: every
    # body is static and self-contained and every call goes through a public
    # symbol. Thirty-two chunks at -O2, as many at a time as there are cores,
    # take about 30 s; the registration table (one 59k-line function) stays at
    # -O0; ld -r merges the pieces back into the one <prefix>_funcs.o that
    # everything downstream names. -fno-strict-aliasing and -fwrapv keep the
    # generated code's memory and integer semantics exactly as at -O0; the
    # mission replay matches the -O0 build in every logged column
    # (docs/GAME-TICK-MAP-REVIEW.md, section 14).
    info "compiling at -O2 in $(nproc) jobs"
    SPLIT="$OBJ/split"; rm -rf "$SPLIT"; mkdir -p "$SPLIT"
    python3 "$ROOT/scripts/emit-split.py" "$GEN/${PREFIX}_funcs.c" "$SPLIT" 32
    # The chunks live outside $GEN, so the generated header needs an -I.
    OPTFLAGS=(-O2 -fno-strict-aliasing -fwrapv "${CFLAGS[@]:1}" -I "$GEN")
    ls "$SPLIT/${PREFIX}_funcs_"[0-9][0-9].c | xargs -P "$(nproc)" -I{} \
        sh -c 'cc -c '"${OPTFLAGS[*]}"' -I '"$SPLIT"' -o "${1%.c}.o" "$1"' _ {}
    cc -c -O0 "${CFLAGS[@]:1}" -I "$GEN" -I "$SPLIT" -o "$SPLIT/${PREFIX}_funcs_reg.o" "$SPLIT/${PREFIX}_funcs_reg.c"
    ld -r -o "$OBJ/${PREFIX}_funcs.o" "$SPLIT/${PREFIX}_funcs_"[0-9][0-9].o "$SPLIT/${PREFIX}_funcs_reg.o"
    for src in "$GEN"/*.c; do
        [ "$(basename "$src")" = "${PREFIX}_funcs.c" ] && continue
        cc -c "${OPTFLAGS[@]}" -o "$OBJ/$(basename "${src%.c}").o" "$src"
    done
else
    info "compiling (~45s)"
    for src in "$GEN"/*.c; do
        cc -c "${CFLAGS[@]}" -o "$OBJ/$(basename "${src%.c}").o" "$src"
    done
fi
cc -c "${CFLAGS[@]}" -o "$OBJ/link_probe.o" "$ROOT/host/link_probe.c"
# The native replacements for whatever the replace list names. Compiled even
# when that list is empty, so adding the first one needs no build change here.
cc -c "${CFLAGS[@]}" -I "$GEN" -o "$OBJ/replacements.o" "$REPLACEMENTS_SRC"

cc -O2 -std=gnu11 -c "$ROOT/host/settings.c" -o "$OBJ/settings.o"

info "linking"
# Named explicitly rather than globbed: stage 05 also builds into this
# directory, and sweeping up its objects drags the oracle harness into the
# probe with none of the libraries it needs.
GEN_OBJS=()
for src in "$GEN"/*.c; do GEN_OBJS+=("$OBJ/$(basename "${src%.c}").o"); done
cc -o "$OBJ/${PREFIX}_probe" "${GEN_OBJS[@]}" "$OBJ/link_probe.o" \
      "$OBJ/replacements.o" "$OBJ/settings.o" \
      "$ROOT/build/psprecomp/libpsprecomp.a" -lm -lpthread "${HOST_LINK_FLAGS[@]}"

info "link closed: $(du -h "$OBJ/${PREFIX}_probe" | cut -f1) executable"
"$OBJ/${PREFIX}_probe"
