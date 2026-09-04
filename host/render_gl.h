/* render_gl — the OpenGL 3.3 core backend, host-side.
 *
 * The runtime cannot carry this: it needs a window and a GL context, and the
 * core has no external dependencies on purpose. boot.c registers it with
 * psp_render_register() and it becomes selectable as "gl" like any other
 * backend. Returns NULL when the host was built without SDL2, in which case
 * there is nowhere to put a context and asking for "gl" should say so rather
 * than fail to link.
 *
 * See tools/psprecomp/docs/RENDERER.md for why it lives here and which thread
 * owns the context. */

#ifndef BOOT_RENDER_GL_H
#define BOOT_RENDER_GL_H

#include "psprecomp/render.h"
#include <stdio.h>

const psp_render_backend *render_gl_backend(void);

/* One line for the end-of-run summary: whether it ran, and what it drew. */
void render_gl_report(FILE *out);

#endif
