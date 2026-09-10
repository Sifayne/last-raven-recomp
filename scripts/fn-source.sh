#!/usr/bin/env bash
# Print one recompiled function's emitted C, by guest address.
#
#   scripts/fn-source.sh 0002E370
#   scripts/fn-source.sh 0x0002E370 | less
#
# game/generated/aclr_funcs.c is 78 MB and 2.1M lines, and every function in it
# is readable -- each statement carries its address and disassembly -- but
# nothing in it is greppable by name, because nothing has one. This is the
# address-to-listing step that every investigation of a guest routine starts
# with: WATCHMEM names a writer, this shows what it does.
#
# Prints from the function's header comment through its public wrapper
# (psp_func_<addr> or psp_func_<addr>__orig when it is replaced), including the
# psp_at_ thunks for any interior labels -- their presence is worth knowing
# before replacing the function, see host/replace.txt.

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

[ $# -eq 1 ] || die "usage: $0 <hex address>"
addr=$(printf '%08X' "$((16#${1#0x}))")
src="$GEN/${PREFIX}_funcs.c"
[ -f "$src" ] || die "no $src -- run scripts/04-emit-build.sh first"

awk -v a="$addr" '
    $0 ~ ("^ \\* psp_func_" a "  --")        { on = 1; print "/*"; }
    on                                        { print }
    on && $0 ~ ("^void psp_func_" a "(__orig)?\\(void\\)") { done = 1 }
    done && $0 !~ ("^void psp_(func|at)_")    { exit }
' "$src"
