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
# Match 04-emit-build.sh: TRACE=1 boots the traced objects, from their own
# directory, without disturbing the plain build.
OUT="$ROOT/build/host"
[ "${TRACE:-0}" != "0" ] && OUT="$ROOT/build/host-trace"
LIB="$ROOT/build/psprecomp/libpsprecomp.a"

[ -f "$LIB" ] || die "no runtime library; run scripts/build-tools.sh first"
[ -f "$OUT/aclr_funcs.o" ] || die "no emitted module; run scripts/04-emit-build.sh first"

# Rebuild the runtime library before linking against it.
#
# This script compiles the boot host and links; it did not build the library,
# so an edit to src/hle or src/render was linked in only if the caller happened
# to run cmake first. It looks like a full build -- "compiling boot host",
# "linking", a fresh set of numbers -- and silently measures the previous
# library. It cost two wrong attributions in one afternoon: a change was
# declared harmless, then declared harmful, on runs that had neither.
#
# Cheap when nothing changed: ninja says "no work to do".
info "building runtime library"
cmake --build "$ROOT/build/psprecomp" -j"$(nproc)" >/dev/null

# The presentation layer is optional at build time as well as run time: with
# SDL2 installed the window, pad and audio exist; without it the host builds
# and runs exactly as it did before. PSPRECOMP_WINDOW selects it at run time.
#
# Probed before the host is compiled, not after: boot.c calls present_start,
# and present.h turns that into a stub when HAVE_SDL2 is absent. Probing later
# left the call with nothing to link against on a machine without SDL2.
PRESENT=""
SDL_DEF=""
if pkg-config --exists sdl2 2>/dev/null; then
    SDL_DEF="-DHAVE_SDL2"
else
    info "no SDL2 -- building headless only"
fi

info "compiling boot host"
# The loader is part of the recompiler tool, not the runtime library, so its
# sources are compiled in here rather than linked from an archive.
cc -O2 -std=gnu11 $SDL_DEF \
   -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
   -c "$ROOT/host/boot.c" -o "$OUT/boot.o"

for src in loader container decode; do
    cc -O2 -std=gnu11 -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$RECOMP_DIR/$src.c" -o "$OUT/$src.o"
done

if [ -n "$SDL_DEF" ]; then
    info "compiling presentation layer (SDL2)"
    cc -O2 -std=gnu11 $SDL_DEF $(pkg-config --cflags sdl2) \
       -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$ROOT/host/present.c" -o "$OUT/present.o"
    PRESENT="$OUT/present.o $(pkg-config --libs sdl2)"
fi

info "linking"
cc "$OUT/boot.o" "$OUT/loader.o" "$OUT/container.o" "$OUT/decode.o" $PRESENT \
   "$OUT/aclr_funcs.o" "$OUT/aclr_imports.o" "$LIB" \
   -o "$OUT/boot" -lm -lpthread $HOST_LIBS

# Build without running, so 09-replay.sh reuses this recipe instead of copying
# it. The recipe is worth not duplicating: it probes SDL2 *before* compiling
# boot.c (see above), and a second copy would drift out of that ordering.
if [ -n "${BOOT_NO_RUN:-}" ]; then
    info "built $OUT/boot"
    exit 0
fi

info "booting"
# The ISO is optional: without it the raw UMD device has nothing behind it and
# reads fail, which is worth being able to run deliberately.
ELF="${1:-$ROOT/game/extracted/ACLR_App.elf}"
ISO="${2:-$(ls "$ROOT"/game/*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ELF" ] || die "no module at $ELF"

exec "$OUT/boot" "$ELF" ${ISO:+"$ISO"}
