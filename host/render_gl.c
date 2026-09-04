/* render_gl — an OpenGL 3.3 core backend for the GE.
 *
 * Why this lives in the host and not in the runtime: the core has no external
 * dependencies on purpose and SDL2 is the host's (tools/psprecomp/CMakeLists.txt
 * says so in its header), so a backend needing a window and a GL context
 * belongs on this side. It reaches the interpreter through
 * psp_render_register(), after which ge.c cannot tell which side it came from.
 *
 * Why the context is claimed lazily rather than in init(): a GL context belongs
 * to exactly one thread, and init() is called by boot.c on the main thread
 * while display lists execute on a guest thread. So init() only records the
 * size, and the first entry point that arrives *on the GE thread* claims the
 * context and builds the resources. The GE is driven by exactly one host thread
 * -- measured, findings item 51 -- which is what makes that safe; if a second
 * one ever appears this refuses loudly rather than issuing calls against a
 * context that is not current.
 *
 * SCOPE, and what this deliberately does not do yet. This is the first
 * increment: untextured geometry with vertex colours, no depth test, no
 * blending, no scissor. Those arrive with the oracle to check them against.
 * State the interpreter sets is recorded and ignored rather than approximated,
 * because a backend that silently half-implements a rule is worse than one that
 * has not implemented it -- the software path is exact on all of them and is
 * what this gets diffed against.
 */

#include "present.h"
#include "psprecomp/render.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SDL2

#include <SDL_opengl.h>
#include <pthread.h>

enum { GL_MAX_VERTS = 64 * 1024 };

/* ---- the entry points we need, loaded by hand ------------------------------
 *
 * No glad, no GLEW: SDL_GL_GetProcAddress is already there and SDL's own
 * headers carry the types and enums, so a loader would be a dependency bought
 * for nothing. */
/* The GL 1.1 entry points have no PFN typedefs -- they predate the extension
 * mechanism and are exported straight from the library. Declaring them here
 * and loading them through SDL like everything else keeps -lGL out of the link,
 * which matters for the Windows port: there GL 1.1 comes from opengl32 and
 * anything newer has to come from wglGetProcAddress, so one uniform path is
 * the only one that works on both. */
typedef void (APIENTRY *PFN_glViewport)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glDrawArrays)(GLenum, GLint, GLsizei);
typedef void (APIENTRY *PFN_glReadPixels)(GLint, GLint, GLsizei, GLsizei,
                                          GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glGenTextures)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glTexImage2D)(GLenum, GLint, GLint, GLsizei,
                                          GLsizei, GLint, GLenum, GLenum,
                                          const void *);
typedef void (APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void (APIENTRY *PFN_glDeleteTextures)(GLsizei, const GLuint *);
typedef GLenum (APIENTRY *PFN_glGetError)(void);
typedef void (APIENTRY *PFN_glEnable)(GLenum);
typedef void (APIENTRY *PFN_glDisable)(GLenum);
typedef void (APIENTRY *PFN_glDepthFunc)(GLenum);
typedef void (APIENTRY *PFN_glDepthMask)(GLboolean);
typedef void (APIENTRY *PFN_glColorMask)(GLboolean, GLboolean, GLboolean, GLboolean);
typedef void (APIENTRY *PFN_glScissor)(GLint, GLint, GLsizei, GLsizei);
typedef void (APIENTRY *PFN_glBlendFunc)(GLenum, GLenum);
typedef void (APIENTRY *PFN_glClear)(GLbitfield);
typedef void (APIENTRY *PFN_glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClearDepth)(GLdouble);

#define GL_FUNCS(X) \
    X(PFN_glViewport,                   glViewport) \
    X(PFN_glDrawArrays,                 glDrawArrays) \
    X(PFN_glReadPixels,                 glReadPixels) \
    X(PFN_glGenTextures,                glGenTextures) \
    X(PFN_glBindTexture,                glBindTexture) \
    X(PFN_glTexImage2D,                 glTexImage2D) \
    X(PFN_glTexParameteri,              glTexParameteri) \
    X(PFN_glDeleteTextures,             glDeleteTextures) \
    X(PFN_glGetError,                   glGetError) \
    X(PFN_glEnable,                     glEnable) \
    X(PFN_glDisable,                    glDisable) \
    X(PFN_glDepthFunc,                  glDepthFunc) \
    X(PFN_glDepthMask,                  glDepthMask) \
    X(PFN_glColorMask,                  glColorMask) \
    X(PFN_glScissor,                    glScissor) \
    X(PFN_glBlendFunc,                  glBlendFunc) \
    X(PFN_glClear,                      glClear) \
    X(PFN_glClearColor,                 glClearColor) \
    X(PFN_glClearDepth,                 glClearDepth) \
    X(PFNGLBLENDEQUATIONPROC,           glBlendEquation) \
    X(PFNGLBLENDCOLORPROC,              glBlendColor) \
    X(PFNGLCREATESHADERPROC,            glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,            glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,           glCompileShader) \
    X(PFNGLGETSHADERIVPROC,             glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,        glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,            glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,           glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,            glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,             glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,            glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,       glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,              glUseProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,      glGetUniformLocation) \
    X(PFNGLUNIFORM2FPROC,               glUniform2f) \
    X(PFNGLUNIFORM1IPROC,               glUniform1i) \
    X(PFNGLUNIFORM1FPROC,               glUniform1f) \
    X(PFNGLUNIFORM3FPROC,               glUniform3f) \
    X(PFNGLGENVERTEXARRAYSPROC,         glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,         glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC,              glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,              glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,              glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,           glBufferSubData) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,     glVertexAttribPointer) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLGENFRAMEBUFFERSPROC,         glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,         glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,    glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,  glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC,         glBlitFramebuffer) \
    X(PFNGLGENRENDERBUFFERSPROC,        glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC,        glBindRenderbuffer) \
    X(PFNGLRENDERBUFFERSTORAGEPROC,     glRenderbufferStorage) \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer)

#define X(type, name) static type p_##name;
GL_FUNCS(X)
#undef X

static int gl_load(void) {
    int missing = 0;
#define X(type, name)                                                        \
    p_##name = (type)present_gl_proc(#name);                                 \
    if (!p_##name) { fprintf(stderr, "gl: missing %s\n", #name); missing++; }
    GL_FUNCS(X)
#undef X
    return missing ? -1 : 0;
}

/* ---- the texture cache ------------------------------------------------------
 *
 * Keyed on everything that changes the decoded texels: where they live, how
 * they are laid out, and -- for the CLUT formats -- the palette and the paging
 * applied to the index. Two bindings with the same key decode identically, so
 * one upload serves both.
 *
 * What this does NOT do yet is notice the game overwriting a texture in place
 * at the same address. That is real (render-to-texture is one way it happens,
 * and the census in item 50 found one such target) and it needs an
 * invalidation signal the interface does not carry. Until then a stale entry
 * is possible, which is a wrong picture rather than a crash -- recorded here
 * so it is looked for rather than discovered.
 */
enum { TEXCACHE_MAX = 512, TEXEL_CAP = 512 * 512 };

typedef struct {
    int      used;
    uint32_t addr, stride, clut_addr;
    int      w, h, fmt, swizzled;
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    GLuint   tex;
} texcache_entry;

/* ---- state ------------------------------------------------------------------
 *
 * Everything the interpreter sets is recorded. Most of it is not acted on yet;
 * see the scope note at the top. It is kept rather than dropped so that the
 * increment that implements a rule has the value already arriving. */
static struct {
    int      w, h;
    int      ready, failed;
    unsigned long thread;

    GLuint   prog, vao, vbo, fbo, colour, depth;
    GLint    u_viewport, u_atest, u_aref;
    GLint    u_texenable, u_texfunc, u_tcc, u_double, u_env, u_tex;

    uint32_t target_addr, target_stride;
    int      target_fmt;

    /* The state the interpreter last set. Applied at flush time rather than
     * as it arrives, because a state change mid-batch would otherwise apply
     * retroactively to geometry already in the buffer -- so every setter
     * flushes what is pending first. */
    int      sc_x0, sc_y0, sc_x1, sc_y1, sc_valid;
    int      z_test, z_func, z_write;
    psp_blend_state bs;
    uint64_t unsupported_blend_eq, unsupported_blend_factor;

    /* Texture state as the interpreter last set it, plus what is bound. */
    psp_tex_state tex;
    int      tex_enable;
    uint32_t clut_addr;
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    GLuint   bound;

    texcache_entry cache[TEXCACHE_MAX];
    uint64_t tex_uploads, tex_hits, tex_evictions, tex_too_big;

    float   *batch;          /* x, y, r, g, b, a per vertex */
    size_t   batch_n;

    uint64_t draws, verts, unsupported_prims, readbacks;
} g;

enum { FLOATS_PER_VERT = 9 };   /* x, y, z, r, g, b, a, u, v */

static void flush(void);
static int  claim(void);

static unsigned long this_thread(void) {
    return (unsigned long)pthread_self();
}

/* ---- shaders ---------------------------------------------------------------
 *
 * Positions arrive in PSP screen pixels -- psp_vertex carries 12.4 fixed point,
 * converted on the way in -- so the vertex shader's whole job is the viewport
 * transform. Y is flipped because the PSP's origin is top-left and GL's is
 * bottom-left; getting that wrong renders a correct frame upside down, which
 * looks like a transform bug and is not one. */
static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec4 a_col;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "uniform vec2 u_viewport;\n"
    "out vec4 v_col;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    vec2 ndc = vec2( (a_pos.x / u_viewport.x) * 2.0 - 1.0,\n"
    "                     1.0 - (a_pos.y / u_viewport.y) * 2.0 );\n"
    /* Window depth arrives on the PSP's 0..65535 scale, already divided by
     * w by the interpreter. GL wants clip space, and with w = 1 the
     * perspective divide is the identity, so mapping to -1..1 here puts it
     * on GL's depth range without a second projection. */
    "    gl_Position = vec4(ndc, a_pos.z * 2.0 - 1.0, 1.0);\n"
    "    v_col = a_col;\n"
    "    v_uv = a_uv;\n"
    "}\n";

/* GL 3.3 core removed the fixed-function alpha test, so it is a discard. The
 * comparison codes are the GE's own, shared with the depth test. */
/* The five texture functions are the GE's own codes, and their arithmetic is
 * what gpu/texfunc pinned for the software path: MODULATE multiplies, DECAL
 * interpolates by the texture's alpha when the alpha channel takes part and
 * replaces when it does not, BLEND mixes toward the environment colour, REPLACE
 * takes the texel, ADD sums colour and keeps the vertex alpha. `u_tcc` says
 * whether the texture's alpha participates at all; `u_double` is the doubling
 * bit, applied after the function and clamped. */
static const char *FS_SRC =
    "#version 330 core\n"
    "in vec4 v_col;\n"
    "in vec2 v_uv;\n"
    "uniform int u_atest;\n"
    "uniform float u_aref;\n"
    "uniform int u_texenable;\n"
    "uniform int u_texfunc;\n"
    "uniform int u_tcc;\n"
    "uniform int u_double;\n"
    "uniform vec3 u_env;\n"
    "uniform sampler2D u_tex;\n"
    "out vec4 o_col;\n"
    "vec4 texfunc(vec4 c) {\n"
    "    vec4 t = texture(u_tex, v_uv);\n"
    "    vec3 tc = t.rgb; float ta = (u_tcc != 0) ? t.a : 1.0;\n"
    "    vec3 rgb; float a;\n"
    "    if (u_texfunc == 1) {\n"          /* DECAL */
    "        rgb = (u_tcc != 0) ? mix(c.rgb, tc, t.a) : tc;\n"
    "        a   = (u_tcc != 0) ? c.a : c.a;\n"
    "    } else if (u_texfunc == 2) {\n"    /* BLEND */
    "        rgb = mix(c.rgb, u_env, tc);\n"
    "        a   = c.a * ta;\n"
    "    } else if (u_texfunc == 3) {\n"    /* REPLACE */
    "        rgb = tc;\n"
    "        a   = (u_tcc != 0) ? t.a : c.a;\n"
    "    } else if (u_texfunc == 4) {\n"    /* ADD */
    "        rgb = c.rgb + tc;\n"
    "        a   = c.a * ta;\n"
    "    } else {\n"                        /* MODULATE */
    "        rgb = c.rgb * tc;\n"
    "        a   = c.a * ta;\n"
    "    }\n"
    "    if (u_double != 0) rgb = min(rgb * 2.0, vec3(1.0));\n"
    "    return vec4(rgb, a);\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = (u_texenable != 0) ? texfunc(v_col) : v_col;\n"
    "    if (u_atest != 1) {\n"
    "        float a = c.a;\n"
    "        bool pass = true;\n"
    "        if      (u_atest == 0) pass = false;\n"
    "        else if (u_atest == 2) pass = (a == u_aref);\n"
    "        else if (u_atest == 3) pass = (a != u_aref);\n"
    "        else if (u_atest == 4) pass = (a <  u_aref);\n"
    "        else if (u_atest == 5) pass = (a <= u_aref);\n"
    "        else if (u_atest == 6) pass = (a >  u_aref);\n"
    "        else if (u_atest == 7) pass = (a >= u_aref);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    o_col = c;\n"
    "}\n";

static GLuint compile(GLenum type, const char *src, const char *what) {
    GLuint sh = p_glCreateShader(type);
    p_glShaderSource(sh, 1, &src, NULL);
    p_glCompileShader(sh);
    GLint ok = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "gl: %s shader failed to compile:\n%s\n", what, log);
        p_glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC, "vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC, "fragment");
    if (!vs || !fs) return -1;
    g.prog = p_glCreateProgram();
    p_glAttachShader(g.prog, vs);
    p_glAttachShader(g.prog, fs);
    p_glLinkProgram(g.prog);
    GLint ok = 0;
    p_glGetProgramiv(g.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        p_glGetProgramInfoLog(g.prog, sizeof log, NULL, log);
        fprintf(stderr, "gl: program failed to link:\n%s\n", log);
        return -1;
    }
    p_glDeleteShader(vs);
    p_glDeleteShader(fs);
    g.u_viewport = p_glGetUniformLocation(g.prog, "u_viewport");
    g.u_atest    = p_glGetUniformLocation(g.prog, "u_atest");
    g.u_aref     = p_glGetUniformLocation(g.prog, "u_aref");
    g.u_texenable = p_glGetUniformLocation(g.prog, "u_texenable");
    g.u_texfunc   = p_glGetUniformLocation(g.prog, "u_texfunc");
    g.u_tcc       = p_glGetUniformLocation(g.prog, "u_tcc");
    g.u_double    = p_glGetUniformLocation(g.prog, "u_double");
    g.u_env       = p_glGetUniformLocation(g.prog, "u_env");
    g.u_tex       = p_glGetUniformLocation(g.prog, "u_tex");
    return 0;
}

/* An off-screen target at the PSP's own size. Rendering into the window
 * directly would tie the picture to whatever the user dragged the window to;
 * this keeps the backend's output the same shape as the software path's, which
 * is what makes the two comparable at all. */
static int build_fbo(void) {
    p_glGenFramebuffers(1, &g.fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);

    p_glGenTextures(1, &g.colour);
    p_glBindTexture(GL_TEXTURE_2D, g.colour);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.w, g.h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g.colour, 0);

    p_glGenRenderbuffers(1, &g.depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, g.depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g.w, g.h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                GL_RENDERBUFFER, g.depth);

    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: framebuffer incomplete\n");
        return -1;
    }
    return 0;
}

/* Claim the context, once, on whichever thread the GE turns out to be. Every
 * entry point goes through here, so a call arriving on a second thread is
 * caught at the boundary rather than as corruption inside the driver. */
static int claim(void) {
    const unsigned long me = this_thread();
    if (g.ready) {
        if (g.thread != me) {
            static int said;
            if (!said++)
                fprintf(stderr, "gl: the GE reached this backend on a second "
                                "host thread (%lu, expected %lu). A GL context "
                                "belongs to one thread; refusing rather than "
                                "drawing through a context that is not "
                                "current.\n", me, g.thread);
            return -1;
        }
        return 0;
    }
    if (g.failed) return -1;

    if (present_gl_make_current() != 0 || gl_load() != 0) { g.failed = 1; return -1; }
    g.thread = me;

    if (build_program() != 0 || build_fbo() != 0) { g.failed = 1; return -1; }

    p_glGenVertexArrays(1, &g.vao);
    p_glBindVertexArray(g.vao);
    p_glGenBuffers(1, &g.vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferData(GL_ARRAY_BUFFER,
                   (GLsizeiptr)(GL_MAX_VERTS * FLOATS_PER_VERT * sizeof(float)),
                   NULL, GL_STREAM_DRAW);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(3 * sizeof(float)));
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(7 * sizeof(float)));
    p_glEnableVertexAttribArray(2);

    g.batch = calloc(GL_MAX_VERTS * FLOATS_PER_VERT, sizeof(float));
    if (!g.batch) { g.failed = 1; return -1; }

    fprintf(stderr, "gl: context claimed on thread %lu, %dx%d target\n",
            me, g.w, g.h);
    g.ready = 1;
    return 0;
}

/* ---- the interface ---------------------------------------------------------- */

static int gl_init(int w, int h) {
    /* No GL here on purpose: this runs on boot.c's thread, not the GE's. */
    g.w = w; g.h = h;
    return 0;
}

static void gl_shutdown(void) { }

static void gl_target(uint32_t addr, uint32_t stride, int fmt) {
    /* No claim() here: set_target is the one setter ge.c calls while the
     * register is still being assembled, long before any drawing, and on a
     * run that never draws it would otherwise create a context for nothing. */
    if (g.ready) flush();
    g.target_addr = addr; g.target_stride = stride; g.target_fmt = fmt;
}

/* Every setter flushes what is pending before recording. A state change
 * applies from the next primitive onward; without the flush it would apply
 * retroactively to geometry already sitting in the batch. */
static void gl_scissor(int x0, int y0, int x1, int y1) {
    if (claim() != 0) return;
    flush();
    g.sc_x0 = x0; g.sc_y0 = y0; g.sc_x1 = x1; g.sc_y1 = y1; g.sc_valid = 1;
}
/* GE wrap codes: 0 repeat, 1 clamp. GE filter: bit 0 selects linear within a
 * level; 4..7 are the mipmap variants, which this does not build yet, so the
 * level bit is all that is read. */
static GLenum gl_wrap(int w) { return w ? GL_CLAMP_TO_EDGE : GL_REPEAT; }
static GLenum gl_filter(int f) { return (f & 1) ? GL_LINEAR : GL_NEAREST; }

static GLuint texcache_get(const psp_tex_state *t) {
    if (!t->addr || t->w <= 0 || t->h <= 0) return 0;
    if ((size_t)t->w * (size_t)t->h > TEXEL_CAP) { g.tex_too_big++; return 0; }

    size_t slot = ((size_t)t->addr >> 4) ^ ((size_t)t->w * 31u)
                ^ ((size_t)t->h * 131u) ^ ((size_t)t->fmt * 7919u)
                ^ ((size_t)g.clut_addr >> 3);
    slot %= TEXCACHE_MAX;

    for (size_t probe = 0; probe < 8; probe++) {
        texcache_entry *e = &g.cache[(slot + probe) % TEXCACHE_MAX];
        if (e->used && e->addr == t->addr && e->w == t->w && e->h == t->h &&
            e->fmt == t->fmt && e->stride == t->stride &&
            e->swizzled == t->swizzled && e->clut_addr == g.clut_addr &&
            e->clut_fmt == g.clut_fmt && e->clut_shift == g.clut_shift &&
            e->clut_mask == g.clut_mask && e->clut_start == g.clut_start) {
            g.tex_hits++;
            return e->tex;
        }
        if (!e->used) { slot = (slot + probe) % TEXCACHE_MAX; goto upload; }
    }
    /* Every probe taken: reuse the first, which is a cheap eviction rather than
     * an LRU. If this counter climbs the table is too small or the key is too
     * loose, and the number says which. */
    g.tex_evictions++;
upload: {
    texcache_entry *e = &g.cache[slot];
    static uint32_t *texels;
    if (!texels) texels = malloc(TEXEL_CAP * sizeof(uint32_t));
    if (!texels) return 0;

    int dw = 0, dh = 0;
    /* One decoder for every backend -- the formats, the CLUT paging and the
     * swizzle are the runtime's, not repeated here. */
    const psp_clut_state clut = { g.clut_addr, g.clut_fmt, g.clut_shift,
                                  g.clut_mask, g.clut_start };
    if (psp_render_decode_level(t, 0, &clut, texels, TEXEL_CAP, &dw, &dh) == 0)
        return 0;

    if (!e->tex) p_glGenTextures(1, &e->tex);
    p_glBindTexture(GL_TEXTURE_2D, e->tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, texels);
    e->used = 1; e->addr = t->addr; e->stride = t->stride;
    e->w = t->w; e->h = t->h; e->fmt = t->fmt; e->swizzled = t->swizzled;
    e->clut_addr = g.clut_addr; e->clut_fmt = g.clut_fmt;
    e->clut_shift = g.clut_shift; e->clut_mask = g.clut_mask;
    e->clut_start = g.clut_start;
    g.tex_uploads++;
    return e->tex;
}
}

static void gl_texture(const psp_tex_state *t) {
    if (claim() != 0) return;
    flush();
    g.tex = *t;
    g.tex_enable = t->addr != 0;
    g.bound = g.tex_enable ? texcache_get(t) : 0;
    if (g.bound) {
        p_glBindTexture(GL_TEXTURE_2D, g.bound);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (GLint)gl_wrap(t->wrap_s));
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (GLint)gl_wrap(t->wrap_t));
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)gl_filter(t->min_filter));
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)gl_filter(t->mag_filter));
    } else {
        g.tex_enable = 0;
    }
}
static void gl_clut(uint32_t a, int f, int sh, int m, int st) {
    if (claim() != 0) return;
    flush();
    /* Part of the cache key rather than state of its own: the palette is what
     * a CLUT texture's texels decode through, so a new palette is a new
     * texture even at the same address. */
    g.clut_addr = a; g.clut_fmt = f;
    g.clut_shift = sh; g.clut_mask = m; g.clut_start = st;
}
static void gl_depth(int test, int func, int write) {
    if (claim() != 0) return;
    flush();
    g.z_test = test; g.z_func = func; g.z_write = write;
}

static void gl_blend(const psp_blend_state *b) {
    if (claim() != 0) return;
    flush();
    g.bs = *b;
}
static void gl_fog(int enable, uint32_t colour) { (void)enable; (void)colour; }

static void push(const psp_vertex *v) {
    if (g.batch_n + 1 > GL_MAX_VERTS) return;
    float *o = g.batch + g.batch_n * FLOATS_PER_VERT;
    /* 12.4 fixed point to pixels. The quarter-pixel this throws away is the
     * PSP's own precision, not ours -- see ROADMAP M7 on why that caps the
     * backend at 1x. */
    o[0] = (float)v->x / (float)PSP_SUBPX;
    o[1] = (float)v->y / (float)PSP_SUBPX;
    o[2] = v->z / 65535.0f;                /* the PSP's window depth scale */
    o[3] = (float)( v->rgba        & 0xFF) / 255.0f;
    o[4] = (float)((v->rgba >>  8) & 0xFF) / 255.0f;
    o[5] = (float)((v->rgba >> 16) & 0xFF) / 255.0f;
    o[6] = (float)((v->rgba >> 24) & 0xFF) / 255.0f;
    /* UVs arrive in texels, as the software path takes them; GL wants them
     * normalised, and dividing here keeps the shader free of the texture's
     * dimensions. */
    o[7] = g.tex.w > 0 ? v->u / (float)g.tex.w : 0.0f;
    o[8] = g.tex.h > 0 ? v->v / (float)g.tex.h : 0.0f;
    g.batch_n++;
}

/* A sprite is two triangles from opposite corners, axis-aligned, taking its
 * colour from the second vertex the way the software path does. */
static void push_sprite(const psp_vertex *v) {
    psp_vertex a = v[1], b = v[1], c = v[1], d = v[1];
    a.x = v[0].x; a.y = v[0].y;
    b.x = v[1].x; b.y = v[0].y;
    c.x = v[1].x; c.y = v[1].y;
    d.x = v[0].x; d.y = v[1].y;
    push(&a); push(&b); push(&c);
    push(&a); push(&c); push(&d);
}

static void gl_draw(int prim, const psp_vertex *v, int count) {
    if (claim() != 0) return;
    g.draws++;
    g.verts += (uint64_t)count;

    switch (prim) {
    case 3:                                        /* triangles */
        for (int i = 0; i + 2 < count; i += 3) {
            push(&v[i]); push(&v[i + 1]); push(&v[i + 2]);
        }
        break;
    case 4:                                        /* triangle strip */
        for (int i = 0; i + 2 < count; i++) {
            /* Winding alternates along a strip; preserve it so a later
             * increment can turn face culling on without the strip flipping. */
            if (i & 1) { push(&v[i + 1]); push(&v[i]); push(&v[i + 2]); }
            else       { push(&v[i]); push(&v[i + 1]); push(&v[i + 2]); }
        }
        break;
    case 5:                                        /* triangle fan */
        for (int i = 1; i + 1 < count; i++) {
            push(&v[0]); push(&v[i]); push(&v[i + 1]);
        }
        break;
    case 6:                                        /* sprites, in pairs */
        for (int i = 0; i + 1 < count; i += 2) push_sprite(&v[i]);
        break;
    default:
        /* Points and lines. The software path does not draw them either, so
         * counting them is the honest thing rather than inventing geometry
         * the reference does not produce. */
        g.unsupported_prims++;
        break;
    }
}

/* The GE's comparison codes, shared by the depth test, the alpha test and the
 * stencil test. Same order as software's depth_pass(). */
static GLenum gl_compare(int func) {
    switch (func) {
    case 0:  return GL_NEVER;
    case 2:  return GL_EQUAL;
    case 3:  return GL_NOTEQUAL;
    case 4:  return GL_LESS;
    case 5:  return GL_LEQUAL;
    case 6:  return GL_GREATER;
    case 7:  return GL_GEQUAL;
    default: return GL_ALWAYS;
    }
}

/* Blend factors. Codes 6..9 are the doubled forms, which GL has no factor for
 * -- they need the shader, and until they have it they are counted rather than
 * approximated, because a wrong factor is a plausible-looking picture and a
 * counted one is a number in the report. */
static GLenum gl_factor(int f, int is_src, int *ok) {
    switch (f) {
    case 0:  return is_src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 1:  return is_src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 2:  return GL_SRC_ALPHA;
    case 3:  return GL_ONE_MINUS_SRC_ALPHA;
    case 4:  return GL_DST_ALPHA;
    case 5:  return GL_ONE_MINUS_DST_ALPHA;
    case 10: return GL_CONSTANT_COLOR;
    default: *ok = 0; return GL_ONE;
    }
}

static GLenum gl_equation(int eq, int *ok) {
    switch (eq) {
    case 0:  return GL_FUNC_ADD;
    case 1:  return GL_FUNC_SUBTRACT;
    case 2:  return GL_FUNC_REVERSE_SUBTRACT;
    case 3:  return GL_MIN;
    case 4:  return GL_MAX;
    default: *ok = 0; return GL_FUNC_ADD;   /* 5 is |src-dst|, shader work */
    }
}

/* Apply what the interpreter last set. Called once per flush rather than per
 * primitive: the batch is by construction all one state. */
static void apply_state(void) {
    if (g.z_test) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(gl_compare(g.z_func));
    } else {
        p_glDisable(GL_DEPTH_TEST);
    }
    p_glDepthMask(g.z_write ? GL_TRUE : GL_FALSE);

    /* write_colour is clear mode's colour mask. Alpha is the stencil byte and
     * an ordinary draw does not write it, which is why the alpha channel is
     * masked off unless the state says otherwise. */
    p_glColorMask(g.bs.write_colour ? GL_TRUE : GL_FALSE,
                  g.bs.write_colour ? GL_TRUE : GL_FALSE,
                  g.bs.write_colour ? GL_TRUE : GL_FALSE,
                  g.bs.write_alpha  ? GL_TRUE : GL_FALSE);

    if (g.sc_valid) {
        p_glEnable(GL_SCISSOR_TEST);
        /* GE corners are inclusive and top-left; GL's origin is bottom-left. */
        const int w = g.sc_x1 - g.sc_x0 + 1, h = g.sc_y1 - g.sc_y0 + 1;
        p_glScissor(g.sc_x0, g.h - g.sc_y0 - h, w > 0 ? w : 0, h > 0 ? h : 0);
    } else {
        p_glDisable(GL_SCISSOR_TEST);
    }

    if (g.bs.enable) {
        int ok = 1;
        const GLenum src = gl_factor(g.bs.src, 1, &ok);
        const GLenum dst = gl_factor(g.bs.dst, 0, &ok);
        if (!ok) g.unsupported_blend_factor++;
        int eq_ok = 1;
        const GLenum eq = gl_equation(g.bs.eq, &eq_ok);
        if (!eq_ok) g.unsupported_blend_eq++;
        p_glEnable(GL_BLEND);
        p_glBlendFunc(src, dst);
        p_glBlendEquation(eq);
        /* The fixed colours, for factor 10. src takes fixa, dst fixb; GL has
         * one constant, so a draw using both is the case this does not cover
         * and the counter above is where it shows up. */
        const uint32_t fx = (g.bs.src == 10) ? g.bs.fixa : g.bs.fixb;
        p_glBlendColor((float)( fx        & 0xFF) / 255.0f,
                       (float)((fx >>  8) & 0xFF) / 255.0f,
                       (float)((fx >> 16) & 0xFF) / 255.0f,
                       (float)((fx >> 24) & 0xFF) / 255.0f);
    } else {
        p_glDisable(GL_BLEND);
    }

    p_glUniform1i(g.u_texenable, g.tex_enable ? 1 : 0);
    p_glUniform1i(g.u_texfunc, g.tex.func);
    p_glUniform1i(g.u_tcc, g.tex.tcc_rgba ? 1 : 0);
    p_glUniform1i(g.u_double, g.tex.color_double ? 1 : 0);
    p_glUniform3f(g.u_env, (float)( g.tex.env        & 0xFF) / 255.0f,
                           (float)((g.tex.env >>  8) & 0xFF) / 255.0f,
                           (float)((g.tex.env >> 16) & 0xFF) / 255.0f);
    p_glUniform1i(g.u_tex, 0);
    if (g.bound) p_glBindTexture(GL_TEXTURE_2D, g.bound);

    p_glUniform1i(g.u_atest, g.bs.alpha_test ? g.bs.alpha_func : 1);
    p_glUniform1f(g.u_aref,  (float)g.bs.alpha_ref / 255.0f);
}

static void flush(void) {
    if (!g.ready || g.batch_n == 0) return;
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
    p_glViewport(0, 0, g.w, g.h);
    p_glUseProgram(g.prog);
    p_glUniform2f(g.u_viewport, (float)g.w, (float)g.h);
    apply_state();
    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0,
                      (GLsizeiptr)(g.batch_n * FLOATS_PER_VERT * sizeof(float)),
                      g.batch);
    p_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g.batch_n);
    g.batch_n = 0;
}

static void gl_finish(void) {
    if (claim() != 0) return;
    flush();
}

/* Read the colour attachment back into the guest's framebuffer.
 *
 * This is what keeps the rest of the project working. score_frame,
 * dump_frame_seq, dump_framebuffer, survey_vram and the frame comparison that
 * passed the M2 gate all read guest memory, and a backend that never writes it
 * makes every one of them blind. It is done here, once per flip, rather than
 * per draw -- which is the difference between affordable and not. */
static void readback(void) {
    if (!g.target_addr) return;
    if (g.target_fmt != 3) {          /* 8888; the census says the display
                                       * buffers are always this */
        static int said;
        if (!said++)
            fprintf(stderr, "gl: readback skipped, target format %d is not "
                            "8888 -- the guest framebuffer will be stale\n",
                    g.target_fmt);
        return;
    }
    const uint32_t stride = g.target_stride ? g.target_stride : 512;
    const size_t bytes = (size_t)stride * (size_t)g.h * 4u;
    void *dst = psp_mem_ptr(g.target_addr, bytes);
    if (!dst) return;

    /* GL hands back bottom-up; the guest's framebuffer is top-down, and the
     * rows are `stride` pixels wide rather than `w`. One row at a time is
     * slower than one call and is the version that is obviously correct;
     * making it fast belongs with the increment that measures it. */
    static uint8_t *row;
    if (!row) row = malloc((size_t)g.w * 4u);
    if (!row) return;
    uint8_t *out = (uint8_t *)dst;
    for (int y = 0; y < g.h; y++) {
        p_glReadPixels(0, g.h - 1 - y, g.w, 1, GL_RGBA, GL_UNSIGNED_BYTE, row);
        memcpy(out + (size_t)y * stride * 4u, row, (size_t)g.w * 4u);
    }
    g.readbacks++;
}

static void gl_present(void) {
    if (claim() != 0) return;
    flush();

    /* The off-screen target, scaled into whatever size the window is now. */
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glBlitFramebuffer(0, 0, g.w, g.h, 0, 0, g.w * 2, g.h * 2,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    present_gl_swap();

    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.fbo);
    readback();
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.fbo);
}

static const psp_render_backend gl_backend = {
    .name        = "gl",
    .init        = gl_init,
    .shutdown    = gl_shutdown,
    .set_target  = gl_target,
    .set_scissor = gl_scissor,
    .set_texture = gl_texture,
    .set_clut    = gl_clut,
    .set_depth   = gl_depth,
    .set_blend   = gl_blend,
    .set_fog     = gl_fog,
    .draw        = gl_draw,
    .finish      = gl_finish,
    .present     = gl_present,
};

const psp_render_backend *render_gl_backend(void) { return &gl_backend; }

void render_gl_report(FILE *out) {
    if (!g.ready && !g.failed) return;
    fprintf(out, "gl:       %s", g.failed ? "failed to start" : "ran");
    if (g.ready)
        fprintf(out, " -- %llu draw(s), %llu vertices, %llu readback(s)",
                (unsigned long long)g.draws, (unsigned long long)g.verts,
                (unsigned long long)g.readbacks);
    fprintf(out, "\n          textures: %llu upload(s), %llu hit(s), %llu eviction(s),"
                 " %llu too big",
            (unsigned long long)g.tex_uploads, (unsigned long long)g.tex_hits,
            (unsigned long long)g.tex_evictions, (unsigned long long)g.tex_too_big);
    if (g.unsupported_prims)
        fprintf(out, ", %llu point/line draw(s) skipped",
                (unsigned long long)g.unsupported_prims);
    /* Counted rather than approximated: the doubled blend factors and the
     * absolute-difference equation have no GL equivalent and need the shader.
     * A wrong factor renders a plausible picture; a counted one is a number. */
    if (g.unsupported_blend_factor || g.unsupported_blend_eq)
        fprintf(out, ", blend not represented: %llu factor, %llu equation",
                (unsigned long long)g.unsupported_blend_factor,
                (unsigned long long)g.unsupported_blend_eq);
    fprintf(out, "\n");
}

#else   /* no SDL2: there is no window to put a context on */

const psp_render_backend *render_gl_backend(void) { return NULL; }
void render_gl_report(FILE *out) { (void)out; }

#endif
