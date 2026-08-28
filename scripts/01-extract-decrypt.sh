#!/usr/bin/env bash
# Stage 01 — pull the main module off the disc and decrypt it.
#
# Stages 01 and 02 from the original plan are merged: they are one dependency
# chain with nothing useful to inspect in between, and splitting them just
# leaves a ~PSP blob on disk that no later stage consumes.
#
# Decryption uses pspdecrypt, not psprecomp. psprecomp's decryptor implements
# the KIRK primitives but not the mode-9 transform that builds a CMD1 header
# from a ~PSP header — its docs/DECRYPT.md says so and points here. Revisit if
# upstream finishes Phase 2b.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need_tool
[ -x "$PSPDECRYPT" ] || die "pspdecrypt not built. Run: scripts/build-tools.sh"

ISO="$(find_iso)"

info "extracting $EBOOT_PATH"
"$AR" extract "$ISO" "$EBOOT_PATH" "$WORK"

ENC="$(find "$WORK" -maxdepth 1 -name '*EBOOT.BIN' | head -1)"
[ -n "$ENC" ] || die "extraction produced no EBOOT.BIN"

info "encrypted module:"
"$AR" info "$ENC" | tee "$REPORTS/01-eboot-header.txt"

info "decrypting -> $(basename "$ELF")"
"$PSPDECRYPT" -o "$ELF" "$ENC"

# The ~PSP header declares the plaintext size up front, so this is a real check
# rather than a formality: a wrong key yields a file of the wrong length.
WANT="$(grep -oP 'elf size\s+\K[0-9]+' "$REPORTS/01-eboot-header.txt" || echo 0)"
GOT="$(stat -c%s "$ELF")"
[ "$WANT" = "$GOT" ] || die "decrypted size $GOT != declared elf_size $WANT — wrong key or corrupt dump"
info "size matches declared elf_size ($GOT bytes)"

"$AR" info "$ELF" | tee "$REPORTS/01-elf-info.txt"
