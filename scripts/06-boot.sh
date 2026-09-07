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
[ "${OPT:-0}" != "0" ] && OUT="$OUT-opt"        # the -O2 generated code, see 04-emit-build.sh
[ "${TRACE:-0}" != "0" ] && OUT="$OUT-trace"
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
cc -O2 -std=gnu11 -c "$ROOT/host/settings.c" -o "$OUT/settings.o"
# The loader is part of the recompiler tool, not the runtime library, so its
# sources are compiled in here rather than linked from an archive.
cc -O2 -std=gnu11 $SDL_DEF \
   -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
   -c "$ROOT/host/boot.c" -o "$OUT/boot.o"

# Native replacements for the functions host/replace.txt names. Linked into the
# boot host as well as stage 04's probe, because this is the build that runs.
#
# The emitted header declares psp_func_<addr>__orig only for the addresses the
# list held when stage 04 last ran, so a list edited since then fails here as
# an implicit declaration of that one symbol -- which reads like a typo in
# replacements.c and is not. Say what it is.
if [ "$ROOT/host/replace.txt" -nt "$GEN/aclr_funcs.h" ]; then
    die "host/replace.txt is newer than the emitted C -- run scripts/04-emit-build.sh first"
fi
cc -O2 -std=gnu11 $SDL_DEF -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" -I "$GEN" \
   -c "$ROOT/host/replacements.c" -o "$OUT/replacements.o"

for src in loader container decode; do
    cc -O2 -std=gnu11 -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$RECOMP_DIR/$src.c" -o "$OUT/$src.o"
done

if [ -n "$SDL_DEF" ]; then
    info "compiling presentation layer (SDL2)"
    cc -O2 -std=gnu11 $SDL_DEF $(pkg-config --cflags sdl2) \
       -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$ROOT/host/present.c" -o "$OUT/present.o"
    # The GL backend is compiled here rather than with the runtime: it needs a
    # window and a GL context, and the core stays dependency-free on purpose.
    cc -O2 -std=gnu11 $SDL_DEF $(pkg-config --cflags sdl2) \
       -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$ROOT/host/render_gl.c" -o "$OUT/render_gl.o"
    PRESENT="$OUT/present.o $OUT/render_gl.o $(pkg-config --libs sdl2)"
fi

# Without SDL2 there is no window, so render_gl.c compiles to its
# no-backend stub and boot.c still links.
if [ -z "$SDL_DEF" ]; then
    cc -O2 -std=gnu11 -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
       -c "$ROOT/host/render_gl.c" -o "$OUT/render_gl.o"
    PRESENT="$OUT/render_gl.o"
fi

# gereplay: one captured frame through a chosen backend. Built beside the boot
# host because it shares the presentation layer -- the gl backend needs a
# window wherever it runs.
info "compiling gereplay"
cc -O2 -std=gnu11 $SDL_DEF ${SDL_DEF:+$(pkg-config --cflags sdl2)} \
   -I "$ROOT/tools/psprecomp/include" -I "$RECOMP_DIR" \
   -c "$ROOT/host/gereplay.c" -o "$OUT/gereplay.o"

info "linking"
cc "$OUT/settings.o" "$OUT/boot.o" "$OUT/loader.o" "$OUT/container.o" "$OUT/decode.o" $PRESENT \
   "$OUT/aclr_funcs.o" "$OUT/aclr_imports.o" "$OUT/replacements.o" "$LIB" \
   -o "$OUT/boot" -lm -lpthread $HOST_LIBS

cc "$OUT/settings.o" "$OUT/gereplay.o" $PRESENT "$LIB" \
   -o "$OUT/gereplay" -lm -lpthread $HOST_LIBS

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
EXTRA=()
if [[ "${1:-}" == --* ]]; then
    EXTRA=("$@")
    set --
fi
ELF="${1:-$ROOT/game/extracted/ACLR_App.elf}"
ISO="${2:-$(ls "$ROOT"/game/*.iso 2>/dev/null | head -1 || true)}"
[ -f "$ELF" ] || die "no module at $ELF"

exec "$OUT/boot" "$ELF" ${ISO:+"$ISO"} "${@:3}" "${EXTRA[@]}"
