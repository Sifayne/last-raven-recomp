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
int present_start(void);

#endif
