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

/* ---- the GL handoff --------------------------------------------------------
 *
 * A GL context belongs to one thread and SDL's is not it. present_want_gl,
 * called before present_start, asks for a GL-capable window and a 3.3 core
 * context; the SDL thread creates both and releases the context so the GE
 * thread can claim it with present_gl_make_current, which blocks until the
 * window exists. See tools/psprecomp/docs/RENDERER.md. */
#ifdef HAVE_SDL2
void  present_want_gl(void);
int   present_gl_make_current(void);
void  present_gl_drawable_size(int *w, int *h);
void  present_gl_swap(void);
void *present_gl_proc(const char *name);
#else
static inline void  present_want_gl(void) { }
static inline int   present_gl_make_current(void) { return -1; }
static inline void  present_gl_drawable_size(int *w, int *h) {
    if (w) *w = 0;
    if (h) *h = 0;
}
static inline void  present_gl_swap(void) { }
static inline void *present_gl_proc(const char *name) { (void)name; return 0; }
#endif

#endif
