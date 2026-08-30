#!/usr/bin/env bash
# Build and run the boot host against the recompiled module.
#
# 04-emit-build.sh proves the emitted C compiles and links. This goes one step
# further and actually runs it: load the module, run its constructors, call
# module_start, then let whatever threads it left behind run.
#
# Kept as a script rather than a remembered command line because the boot host
# links against three things that are built in different places -- the emitted
# module, the runtime library, and the loader sources that live with the
# recompiler tool rather than in the library.
set -euo pipefail
. "$(dirname "$0")/common.sh"

RECOMP_DIR="$ROOT/tools/psprecomp/tools/allegrexrecomp"
OUT="$ROOT/build/host"
LIB="$ROOT/build/psprecomp/libpsprecomp.a"

[ -f "$LIB" ] || die "no runtime library; run scripts/build-tools.sh first"
[ -f "$OUT/aclr_funcs.o" ] || die "no emitted module; run scripts/04-emit-build.sh first"

info "compiling boot host"
# The loader is part of the recompiler tool, not the runtime library, so its
# sources are compiled in here rather than linked from an archive.
cc -O2 -std=gnu11 \
   -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
   -c "$ROOT/host/boot.c" -o "$OUT/boot.o"

for src in loader container decode; do
    cc -O2 -std=gnu11 -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$RECOMP_DIR/$src.c" -o "$OUT/$src.o"
done

info "linking"
cc "$OUT/boot.o" "$OUT/loader.o" "$OUT/container.o" "$OUT/decode.o" \
   "$OUT/aclr_funcs.o" "$OUT/aclr_imports.o" "$LIB" \
   -o "$OUT/boot" -lm -lpthread $HOST_LIBS

info "booting"
# The ISO is optional: without it the raw UMD device has nothing behind it and
# reads fail, which is worth being able to run deliberately.
ELF="${1:-$ROOT/game/extracted/ACLR_App.elf}"
ISO="${2:-$(ls "$ROOT"/game/*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ELF" ] || die "no module at $ELF"

exec "$OUT/boot" "$ELF" ${ISO:+"$ISO"}
