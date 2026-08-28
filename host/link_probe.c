/* link_probe.c — the smallest host that closes the link against the
 * recompiled game.
 *
 * This is a Phase 0 measurement, not a runnable game. Its only job is to
 * answer one question: once the three symbols a host is *expected* to supply
 * are supplied, does the recompiled module link with no other gaps?
 *
 * psprecomp's generated header declares these but its runtime does not define
 * them — they are the host's contract:
 *
 *   psp_syscall        the Allegrex `syscall` instruction. On PSP a syscall is
 *                      how a module reaches firmware it did not import through
 *                      a stub, so a real host routes this into HLE.
 *   psp_unimplemented  an instruction the emitter could not translate. For this
 *                      module that is 331 VFPU instructions out of 741,848.
 *
 * Both abort here rather than returning. A stub that returns quietly would let
 * execution continue past an instruction that did not happen, and the damage
 * would surface somewhere unrelated — the same failure mode psprecomp's
 * BRINGUP.md warns about. Failing loudly at the real site is the point.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

void psp_syscall(uint32_t id);
void psp_unimplemented(uint32_t addr, const char *what);

void psp_syscall(uint32_t id) {
    fprintf(stderr, "psp_syscall: unhandled syscall 0x%05X\n", id);
    abort();
}

void psp_unimplemented(uint32_t addr, const char *what) {
    fprintf(stderr, "psp_unimplemented: %s at 0x%08X\n", what, addr);
    abort();
}

int main(void) {
    /* Deliberately does not start the game. Reaching this point means the
     * whole recompiled module linked; running it is Phase 1 and needs a
     * thread scheduler, the missing HLE, and a GE that rasterises. */
    puts("link probe: recompiled module linked successfully");
    return 0;
}
