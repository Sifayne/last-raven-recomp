#!/usr/bin/env bash
# Stage 02 — decode coverage and function discovery.
#
# Two questions:
#   cover  can the decoder read every instruction in .text? Anything it cannot
#          is either a decoder gap or data being misread as code, and both are
#          real problems.
#   funcs  can discovery find the function boundaries? Coverage without
#          boundaries is not something you can emit C from.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
need_tool
[ -f "$ELF" ] || die "no $ELF — run scripts/01-extract-decrypt.sh first"

info "decode coverage"
"$AR" cover "$ELF" | tee "$REPORTS/02-cover.txt"
echo

info "function discovery (this walks 26k+ functions; give it a moment)"
"$AR" funcs "$ELF" > "$REPORTS/02-funcs.txt"
head -8  "$REPORTS/02-funcs.txt"
echo "..."
tail -12 "$REPORTS/02-funcs.txt"
echo
info "wrote $REPORTS/02-cover.txt and $REPORTS/02-funcs.txt"
