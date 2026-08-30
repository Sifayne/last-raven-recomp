/* present — the windowed presentation layer for the boot host.
 *
 * SDL2 window, real gamepad, audio out. Lives in the host, not the library:
 * the library learns only that *someone* may want frames and samples, through
 * the hooks in hle.h, and stays dependency-free.
 *
 * All of SDL runs on one dedicated thread -- the thread that initialises
 * video owns the event queue -- so the guest threads never touch SDL. They
 * publish converted frames and PCM through small critical sections and get
 * back to the guest immediately. */

#ifndef BOOT_PRESENT_H
#define BOOT_PRESENT_H

/* Turn the presentation layer on. Enables the real-time clock, registers the
 * display and audio hooks, and spawns the SDL thread. Returns 0 on success,
 * -1 if SDL could not start (the run continues headless either way). */
#ifdef HAVE_SDL2
int present_start(void);
#else
/* No SDL2 when this was built, so present.c was never compiled and there is
 * nothing to link against. The declaration becomes a stub rather than the call
 * site becoming conditional: a host without SDL2 has to build, and asking for a
 * window on one should say why it did not get one instead of failing to link. */
#include <stdio.h>
static inline int present_start(void) {
    fprintf(stderr, "present: built without SDL2 -- no window. Install the "
                    "SDL2 development package and re-run scripts/06-boot.sh\n");
    return -1;
}
#endif

#endif
