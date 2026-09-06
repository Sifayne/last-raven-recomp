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
 * The backend translates points, lines, triangles, strips, fans and sprites at
 * PSP resolution or the window's physical resolution, including texture
 * decode/cache, perspective UVs, the full mip chain and measured PSP LOD/filter
 * rules, depth, scissor, blending, alpha
 * test, RGBA8888 alpha-backed stencil and fog. The software path remains the
 * differential oracle. Reduced-bit-depth stencil and blend operations without
 * a fixed GL equivalent stay explicitly counted rather than approximated.
 */

#include "present.h"
#include "render_gl.h"
#include "psprecomp/render.h"
#include "psprecomp/mem.h"
#include "psprecomp/os.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int render_gl_resolution_mode(void) {
    const char *s = getenv("PSPRECOMP_RESOLUTION");
    if (!s || !*s || !strcmp(s, "psp")) return 0;
    if (!strcmp(s, "window")) return 1;
    fprintf(stderr, "unknown resolution mode \"%s\"; expected psp or window\n", s);
    return -1;
}

#ifdef HAVE_SDL2

#include <SDL_opengl.h>
#include <pthread.h>

enum { GL_MAX_VERTS = 64 * 1024, GPU_QUERY_RING = 8 };

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
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glDeleteRenderbuffers)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glGetIntegerv)(GLenum, GLint *);
typedef void (APIENTRY *PFN_glTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                          GLsizei, GLsizei, GLenum, GLenum, const void *);
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
typedef void (APIENTRY *PFN_glStencilFunc)(GLenum, GLint, GLuint);
typedef void (APIENTRY *PFN_glStencilOp)(GLenum, GLenum, GLenum);
typedef void (APIENTRY *PFN_glStencilMask)(GLuint);
typedef void (APIENTRY *PFN_glClearStencil)(GLint);
typedef void (APIENTRY *PFN_glCopyTexSubImage2D)(GLenum, GLint, GLint, GLint,
                                               GLint, GLint, GLsizei, GLsizei);

#define GL_FUNCS(X) \
    X(PFN_glViewport,                   glViewport) \
    X(PFN_glDrawArrays,                 glDrawArrays) \
    X(PFN_glReadPixels,                 glReadPixels) \
    X(PFN_glGenTextures,                glGenTextures) \
    X(PFN_glBindTexture,                glBindTexture) \
    X(PFN_glTexImage2D,                 glTexImage2D) \
    X(PFN_glTexParameteri,              glTexParameteri) \
    X(PFN_glDeleteTextures,             glDeleteTextures) \
    X(PFN_glDeleteFramebuffers,         glDeleteFramebuffers) \
    X(PFN_glDeleteRenderbuffers,        glDeleteRenderbuffers) \
    X(PFN_glGetIntegerv,                glGetIntegerv) \
    X(PFN_glTexSubImage2D,              glTexSubImage2D) \
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
    X(PFN_glStencilFunc,                glStencilFunc) \
    X(PFN_glStencilOp,                  glStencilOp) \
    X(PFN_glStencilMask,                glStencilMask) \
    X(PFN_glClearStencil,               glClearStencil) \
    X(PFN_glCopyTexSubImage2D,           glCopyTexSubImage2D) \
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
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) \
    X(PFNGLGENQUERIESPROC,               glGenQueries) \
    X(PFNGLBEGINQUERYPROC,               glBeginQuery) \
    X(PFNGLENDQUERYPROC,                 glEndQuery) \
    X(PFNGLGETQUERYOBJECTIVPROC,         glGetQueryObjectiv) \
    X(PFNGLGETQUERYOBJECTUI64VPROC,      glGetQueryObjectui64v)

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
 * The key is paired with the guest-memory generation of every byte range the
 * decoder can read. CPU stores, GE copies and render-target readbacks advance
 * those generations, so an in-place update reuses the GL texture object but
 * uploads fresh texels. A global serial makes the common no-write case O(1);
 * page ranges are scanned only after guest memory changed somewhere.
 */
enum {
    TEXCACHE_MAX = 512,
    TEXCACHE_PROBES = 32,
    TEXEL_CAP = 512 * 512,
    RT_MAX = 8
};

/* One framebuffer object per render target.
 *
 * A single shared FBO was wrong in a way that took a while to see. This game
 * double-buffers and then composites: it draws the room into one display
 * buffer, and in a later frame draws a handful of full-screen passes that read
 * that buffer back. With one FBO those two live in the same pixels, so the
 * compositing frame painted over the room and the hangar came back as walls
 * with no floor. The census in findings item 50 said three targets are drawn
 * into; this gives each its own colour and depth.
 *
 * A texture bound at a target's address samples that target's colour texture
 * rather than being uploaded from guest memory, which is what makes the
 * read-back-and-composite pattern work at all. */
typedef struct {
    int      used, dirty, configured;
    uint32_t addr, stride;
    int      fmt, w, h;
    /* Guest memory and coordinates retain their PSP extent. w/h describe GPU
     * storage, including padding. Scene and HUD share sy; aspect changes only
     * the scene's horizontal scale and the HUD's horizontal placement. */
    int      guest_w, guest_h, wide, wide_w, display;
    int      visible_w, visible_h;
    double   sx, sy, ui_scale;
    uint8_t *cpu_dirty;
    int      cpu_pending;
    GLuint   fbo, colour, depth;
    int      stencil_valid, alpha_dirty;
} rendertarget;

/* Where a batch is placed on a widened target. SCENE geometry is spread across
 * the whole target; HUD geometry is translated to the centre and kept 1:1. */
enum { CLASS_SCENE = 0, CLASS_HUD = 1 };

typedef struct {
    int      used;
    uint32_t addr, stride, clut_addr;
    int      w, h, fmt, swizzled, max_level, uploaded_top;
    uint32_t lv_addr[8], lv_stride[8];
    int      lv_w[8], lv_h[8];
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    uint64_t content_generation, validated_serial;
    uint64_t last_used;
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
    int      adaptive_aspect, resolution;
    int      pixel_w, pixel_h, max_size;
    uint64_t resizes, cpu_uploads, rt_views;
    int      exporting;
    unsigned long thread;

    GLuint   prog, vao, vbo;
    GLuint   stencil_prog, stencil_copy;
    GLint    s_mode, s_bit, s_value;
    int      stencil_copy_w, stencil_copy_h;
    GLint    u_viewport, u_placement, u_ybias, u_atest, u_aref, u_amask, u_preblend_src;
    GLint    u_texenable, u_texfunc, u_tcc, u_double, u_env, u_tex;
    GLint    u_minfilter, u_magfilter, u_wraps, u_wrapt, u_miptop;
    GLint    u_fogenable, u_fogcolour;
    GLint    u_texscale;
    float    tex_sx, tex_sy;
    GLuint   view_fbo, view_tex, copy_fbo;
    int      view_w, view_h;

    uint32_t target_addr, target_stride;
    int      target_fmt;

    /* The state the interpreter last set. Applied at flush time rather than
     * as it arrives, because a state change mid-batch would otherwise apply
     * retroactively to geometry already in the buffer -- so every setter
     * flushes what is pending first. */
    int      sc_x0, sc_y0, sc_x1, sc_y1, sc_valid;
    int      z_test, z_func, z_write;
    int      fog_enable;
    uint32_t fog_colour;
    psp_blend_state bs;
    uint64_t unsupported_blend_eq, unsupported_blend_factor;
    uint64_t unsupported_stencil_draws;
    uint64_t stencil_draws, stencil_imports, stencil_exports;

    /* Texture state as the interpreter last set it, plus what is bound. */
    psp_tex_state tex;
    int      tex_enable;
    uint32_t clut_addr;
    int      clut_fmt, clut_shift, clut_mask, clut_start;
    GLuint   bound;
    int      bound_top;

    rendertarget rts[RT_MAX];
    int      n_rts, cur_rt;
    uint64_t rt_overflow;

    /* Adaptive aspect (PSPRECOMP_ASPECT=window). wide_w is the virtual PSP
     * width, latched once per frame; g.w means the identity. batch_class is
     * the placement of the geometry now in the batch. The staging target
     * downsamples a wide attachment to guest width for the readback. */
    int      wide_w, batch_class, batch_glyph;
    uint64_t glyph_draws;
    GLuint   stage_fbo, stage_tex;
    int      stage_w, stage_h;
    uint64_t class_scene, class_hud, hud_flushes, wide_allocs, wide_retired;
    uint64_t hud_hazard_depth, hud_hazard_stencil, hud_hazard_dst_alpha;
    uint64_t hud_depth_func[8], hud_depth_writes;
    /* PSPRECOMP_ASPECT_LOG: what the HUD class holds, per texture. */
    struct { uint32_t addr; int w, h; uint64_t batches, tested; float z0, z1; } hud_by_tex[48];
    int      hud_by_tex_n;

    texcache_entry cache[TEXCACHE_MAX];
    int      cache_entries;
    uint64_t cache_clock;
    uint64_t tex_requests, tex_uploads, tex_hits, tex_fast_hits, tex_revalidated;
    uint64_t tex_invalidations, tex_misses, tex_evictions, tex_too_big;
    uint64_t tex_vram_uploads, tex_upload_pixels;
    uint64_t tex_from_rt, tex_alias_from_rt;
    uint64_t mip_chains, mip_levels, mip_incomplete;
    uint64_t tex_bind_ns, tex_generation_ns, tex_decode_ns, tex_upload_ns;

    float   *batch;          /* position, colour, UV, fog and homogeneous terms */
    size_t   batch_n;

    uint64_t draws, verts, unsupported_prims, readbacks, batch_overflows;
    uint64_t readback_ns;
    uint64_t presents, frames, frame_first_ns, frame_last_ns, frame_prev_ns;
    uint64_t frame_max_ns;
    uint64_t frame_ms[256];
    GLuint   gpu_query[GPU_QUERY_RING];
    uint8_t  gpu_query_pending[GPU_QUERY_RING];
    int      gpu_query_active, gpu_query_suppressed;
    unsigned gpu_query_next;
    uint64_t gpu_samples, gpu_total_ns, gpu_max_ns, gpu_dropped;
    uint64_t gpu_tenth_ms[256];
} g = { .gpu_query_active = -1 };

enum { FLOATS_PER_VERT = 13 };  /* x,y,z, r,g,b,a, u,v, fog, 1/w, texture q, lod16 */

static void flush(void);
static int  claim(void);
static void readback_rt(int i);
static void stencil_to_alpha(rendertarget *r);
static void rt_import(rendertarget *r);

static unsigned long this_thread(void) {
    return (unsigned long)pthread_self();
}

/* ---- asynchronous GPU frame timing --------------------------------------
 *
 * Wall cadence answers whether the game is paced; it does not answer how
 * much of the frame budget the GPU used. TIME_ELAPSED queries bracket the
 * native-resolution draws and final blit. Eight objects are rotated so a
 * result is only read after the driver says it is ready; the measurement must
 * never introduce the stall it is trying to measure. A readback later in the
 * same present normally makes the just-ended result available anyway. */
static void gpu_query_record(uint64_t ns) {
    g.gpu_samples++;
    g.gpu_total_ns += ns;
    if (ns > g.gpu_max_ns) g.gpu_max_ns = ns;
    uint64_t bin = ns / UINT64_C(100000);       /* tenths of a millisecond */
    if (bin > 255) bin = 255;
    g.gpu_tenth_ms[bin]++;
}

static void gpu_query_poll(void) {
    for (int i = 0; i < GPU_QUERY_RING; i++) {
        if (!g.gpu_query_pending[i]) continue;
        GLint available = 0;
        p_glGetQueryObjectiv(g.gpu_query[i], GL_QUERY_RESULT_AVAILABLE,
                             &available);
        if (!available) continue;
        GLuint64 ns = 0;
        p_glGetQueryObjectui64v(g.gpu_query[i], GL_QUERY_RESULT, &ns);
        g.gpu_query_pending[i] = 0;
        gpu_query_record((uint64_t)ns);
    }
}

static void gpu_query_begin_frame(void) {
    if (g.gpu_query_active >= 0 || g.gpu_query_suppressed) return;
    gpu_query_poll();
    for (int n = 0; n < GPU_QUERY_RING; n++) {
        const int i = (int)((g.gpu_query_next + (unsigned)n) % GPU_QUERY_RING);
        if (g.gpu_query_pending[i]) continue;
        p_glBeginQuery(GL_TIME_ELAPSED, g.gpu_query[i]);
        g.gpu_query_active = i;
        g.gpu_query_next = ((unsigned)i + 1u) % GPU_QUERY_RING;
        return;
    }
    g.gpu_query_suppressed = 1;
    g.gpu_dropped++;
}

static void gpu_query_end_frame(void) {
    if (g.gpu_query_active >= 0) {
        p_glEndQuery(GL_TIME_ELAPSED);
        g.gpu_query_pending[g.gpu_query_active] = 1;
        g.gpu_query_active = -1;
    }
    g.gpu_query_suppressed = 0;
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
    "layout(location=3) in float a_fog;\n"
    "layout(location=4) in float a_inv_w;\n"
    "layout(location=5) in float a_tex_q;\n"
    "layout(location=6) in float a_lod16;\n"
    "uniform vec2 u_viewport;\n"
    "uniform vec3 u_placement; uniform float u_ybias;\n"
    "out vec4 v_col;\n"
    /* gl_Position deliberately remains post-divide screen space with w=1 so
     * GL cannot alter the PSP coverage or depth already resolved by the GE.
     * Carry UV/W and Q/W as explicitly non-perspective varyings and perform
     * only the texture divide in the fragment shader. */
    "noperspective out vec3 v_uvq;\n"
    "out float v_fog;\n"
    "flat out int v_lod16;\n"
    "void main() {\n"
    "    vec2 p = a_pos.xy * u_placement.xy + vec2(u_placement.z, 0);\n"
    "    vec2 ndc = vec2( (p.x / u_viewport.x) * 2.0 - 1.0,\n"
    /* The PSP owns a pixel on its top edge; GL's lower-left half-open rule
     * owns the opposite horizontal edge after the Y flip. Move geometry by
     * one GL subpixel, sixteen times smaller than the PSP's 1/16-pixel vertex
     * grid, so exact horizontal ties land on the PSP-owned side. */
    "                     1.0 - ((p.y - u_ybias) / u_viewport.y) * 2.0 );\n"
    /* Window depth arrives on the PSP's 0..65535 scale, already divided by
     * w by the interpreter. GL wants clip space, and with w = 1 the
     * perspective divide is the identity, so mapping to -1..1 here puts it
     * on GL's depth range without a second projection. */
    "    gl_Position = vec4(ndc, a_pos.z * 2.0 - 1.0, 1.0);\n"
    "    v_col = a_col;\n"
    "    v_uvq = vec3(a_uv * a_inv_w, a_tex_q * a_inv_w);\n"
    "    v_fog = a_fog;\n"
    "    v_lod16 = int(a_lod16);\n"
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
    "noperspective in vec3 v_uvq;\n"
    "in float v_fog;\n"
    "flat in int v_lod16;\n"
    "uniform int u_atest;\n"
    "uniform int u_aref;\n"
    "uniform int u_amask;\n"
    "uniform int u_preblend_src;\n"
    "uniform int u_texenable;\n"
    "uniform int u_texfunc;\n"
    "uniform int u_tcc;\n"
    "uniform int u_double;\n"
    "uniform vec3 u_env;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texscale;\n"
    "uniform int u_minfilter;\n"
    "uniform int u_magfilter;\n"
    "uniform int u_wraps;\n"
    "uniform int u_wrapt;\n"
    "uniform int u_miptop;\n"
    "uniform int u_fogenable;\n"
    "uniform vec3 u_fogcolour;\n"
    "out vec4 o_col;\n"
    /* Use texelFetch and reproduce the PSP sampler explicitly. Native GL
     * filtering has a different minification switch for one legal PSP filter
     * combination, and its bilinear precision is implementation-defined; both
     * are visible at the 1:1 UI boundary. Coordinates and mip blending here
     * follow the software oracle's measured 1/16 rules. */
    "int wrap_axis(int p, int n, int clamp_it) {\n"
    "    if (clamp_it != 0) return clamp(p, 0, n - 1);\n"
    "    int r = p % n; return r < 0 ? r + n : r;\n"
    "}\n"
    "ivec4 texel_i(ivec2 p, int level) {\n"
    "    ivec2 sz = textureSize(u_tex, level);\n"
    "    p.x = wrap_axis(p.x, sz.x, u_wraps);\n"
    "    p.y = wrap_axis(p.y, sz.y, u_wrapt);\n"
    "    return ivec4(floor(texelFetch(u_tex, p, level) * 255.0 + 0.5));\n"
    "}\n"
    "ivec4 sample_level(vec2 uv, int level, bool linear) {\n"
    "    ivec2 sz = textureSize(u_tex, level);\n"
    "    ivec2 base_sz = textureSize(u_tex, 0);\n"
    "    vec2 tc = uv * vec2(sz) / vec2(base_sz);\n"
    "    if (!linear) return texel_i(ivec2(floor(tc)), level);\n"
    "    ivec2 fq = ivec2(floor((tc - vec2(0.5)) * 16.0 + vec2(0.001)));\n"
    "    ivec2 p0 = ivec2(floor(vec2(fq) / 16.0));\n"
    "    ivec2 a = fq - p0 * 16;\n"
    "    ivec4 t00 = texel_i(p0,                 level);\n"
    "    ivec4 t10 = texel_i(p0 + ivec2(1, 0), level);\n"
    "    ivec4 t01 = texel_i(p0 + ivec2(0, 1), level);\n"
    "    ivec4 t11 = texel_i(p0 + ivec2(1, 1), level);\n"
    "    int w00 = (16 - a.x) * (16 - a.y);\n"
    "    int w10 = a.x * (16 - a.y);\n"
    "    int w01 = (16 - a.x) * a.y;\n"
    "    int w11 = a.x * a.y;\n"
    "    return (t00 * w00 + t10 * w10 + t01 * w01 + t11 * w11) / 256;\n"
    "}\n"
    "vec4 sample_psp(vec2 uv) {\n"
    "    uv *= u_texscale;\n"
    "    int lod = v_lod16;\n"
    "    bool linear = (((lod > 0 ? u_minfilter : u_magfilter) & 1) != 0);\n"
    "    if (u_minfilter < 4 || u_miptop <= 0)\n"
    "        return vec4(sample_level(uv, 0, linear)) / 255.0;\n"
    "    lod = clamp(lod, 0, u_miptop * 16);\n"
    "    if ((u_minfilter & 2) == 0) {\n"
    "        int level = min((lod + 8) / 16, u_miptop);\n"
    "        return vec4(sample_level(uv, level, linear)) / 255.0;\n"
    "    }\n"
    "    int level = lod / 16, f = lod & 15;\n"
    "    ivec4 c0 = sample_level(uv, level, linear);\n"
    "    if (f == 0 || level >= u_miptop) return vec4(c0) / 255.0;\n"
    "    ivec4 c1 = sample_level(uv, level + 1, linear);\n"
    "    return vec4(c0 + (c1 - c0) * f / 16) / 255.0;\n"
    "}\n"
    /* The software oracle and the GE tests put an RGBA8 quantisation point at
     * the interpolated vertex colour, and another after the texture function.
     * Leaving either value fractional lets fog and blending see precision the
     * PSP did not carry, which becomes a two-level error after colour-double. */
    "vec4 rgba8(vec4 c) {\n"
    "    return floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5) / 255.0;\n"
    "}\n"
    "vec4 texfunc(vec4 c) {\n"
    "    c = rgba8(c);\n"
    "    vec2 uv = (v_uvq.z != 0.0) ? v_uvq.xy / v_uvq.z : vec2(0.0);\n"
    "    vec4 t = sample_psp(uv);\n"
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
    "    vec4 q = rgba8(vec4(rgb, a));\n"
    "    if (u_double != 0) q.rgb = min(q.rgb * 2.0, vec3(1.0));\n"
    "    return q;\n"
    "}\n"
    "vec4 fog_psp(vec4 c) {\n"
    "    ivec4 ci = ivec4(floor(clamp(c, 0.0, 1.0) * 255.0 + 0.5));\n"
    "    int f = int(floor(clamp(v_fog, 0.0, 1.0) * 255.0 + 0.5));\n"
    "    if (f < 255) {\n"
    "        ivec3 fc = ivec3(floor(clamp(u_fogcolour, 0.0, 1.0) * 255.0 + 0.5));\n"
    "        ci.rgb = (ci.rgb * f + fc * (255 - f) + ivec3(255)) / 256;\n"
    "    }\n"
    "    return vec4(ci) / 255.0;\n"
    "}\n"
    "void main() {\n"
    "    vec4 c = (u_texenable != 0) ? texfunc(v_col) : rgba8(v_col);\n"
    "    if (u_fogenable != 0)\n"
    "        c = fog_psp(c);\n"
    "    if (u_atest != 1) {\n"
    "        int a = int(floor(c.a * 255.0 + 0.5)) & u_amask;\n"
    "        int ref = u_aref & u_amask;\n"
    "        bool pass = true;\n"
    "        if      (u_atest == 0) pass = false;\n"
    "        else if (u_atest == 2) pass = (a == ref);\n"
    "        else if (u_atest == 3) pass = (a != ref);\n"
    "        else if (u_atest == 4) pass = (a <  ref);\n"
    "        else if (u_atest == 5) pass = (a <= ref);\n"
    "        else if (u_atest == 6) pass = (a >  ref);\n"
    "        else if (u_atest == 7) pass = (a >= ref);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    /* Fixed-function GL keeps the source-alpha multiplication fractional and
     * rounds after combining both blend terms. The GE first truncates each
     * term with ((channel + 1) * factor) >> 8. Source alpha depends only on
     * this fragment, so do that term exactly here and ask GL to add it whole;
     * the original alpha remains available to scale the destination term. */
    "    if (u_preblend_src != 0) {\n"
    "        ivec4 ci = ivec4(floor(c * 255.0 + 0.5));\n"
    "        ci.rgb = ((ci.rgb + ivec3(1)) * ci.a) / 256;\n"
    "        if (u_preblend_src == 2) {\n"
    /* For destination (1-src-alpha), make GL's final round reproduce
     * floor((dst+1)*(255-a)/256): use the exact f/256 scale, then add
     * f/256 - 1/2 to the already-integer source term before that round. The
     * 1/4096 bias resolves an exact half tie upward and is smaller than the
     * expression's smallest non-zero 1/256 fractional step. */
    "            float f = float(255 - ci.a) / 256.0;\n"
    "            c.rgb = (vec3(ci.rgb) + vec3(f - 0.5 + 1.0 / 4096.0)) / 255.0;\n"
    "            c.a = float(ci.a + 1) / 256.0;\n"
    "        } else {\n"
    "            c.rgb = vec3(ci.rgb) / 255.0;\n"
    "        }\n"
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

/* GL 3.3 cannot sample the stencil attachment directly. These tiny bit-plane
 * passes keep the hardware stencil and the PSP's framebuffer alpha byte in
 * sync entirely on the GPU. Import copies the colour first to avoid texture
 * feedback; export tests each stencil bit and adds its exact byte weight.
 * Normal rendering does not pay for a copy on every draw. */
static int build_stencil_program(void) {
    const char *vs_src =
        "#version 330 core\n"
        "void main() {\n"
        " vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        " gl_Position = vec4(p * 2.0 - 1.0, 0, 1);\n"
        "}\n";
    const char *fs_src =
        "#version 330 core\n"
        "uniform sampler2D u_copy; uniform int u_mode, u_bit;\n"
        "uniform float u_value; out vec4 o_col;\n"
        "void main() {\n"
        " if (u_mode == 1) {\n"
        "  int a = int(floor(texelFetch(u_copy, ivec2(gl_FragCoord.xy), 0).a * 255.0 + 0.5));\n"
        "  if ((a & u_bit) == 0) discard;\n"
        " }\n"
        " o_col = vec4(0, 0, 0, u_value);\n"
        "}\n";
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "stencil vertex");
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "stencil fragment");
    if (!vs || !fs) return -1;
    g.stencil_prog = p_glCreateProgram();
    p_glAttachShader(g.stencil_prog, vs); p_glAttachShader(g.stencil_prog, fs);
    p_glLinkProgram(g.stencil_prog);
    p_glDeleteShader(vs); p_glDeleteShader(fs);
    GLint ok = 0;
    p_glGetProgramiv(g.stencil_prog, GL_LINK_STATUS, &ok);
    if (!ok) { fprintf(stderr, "gl: stencil program failed to link\n"); return -1; }
    g.s_mode = p_glGetUniformLocation(g.stencil_prog, "u_mode");
    g.s_bit = p_glGetUniformLocation(g.stencil_prog, "u_bit");
    g.s_value = p_glGetUniformLocation(g.stencil_prog, "u_value");
    p_glGenTextures(1, &g.stencil_copy);
    return 0;
}

static void stencil_pass_state(rendertarget *r) {
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    p_glViewport(0, 0, r->w, r->h);
    p_glUseProgram(g.stencil_prog);
    p_glBindVertexArray(g.vao);
    p_glDisable(GL_SCISSOR_TEST);
    p_glDisable(GL_DEPTH_TEST);
    p_glDepthMask(GL_FALSE);
    p_glDisable(GL_BLEND);
    p_glEnable(GL_STENCIL_TEST);
}

static void alpha_to_stencil(rendertarget *r) {
    if (r->stencil_valid || r->fmt != 3) return;
    stencil_pass_state(r);
    p_glBindTexture(GL_TEXTURE_2D, g.stencil_copy);
    if (g.stencil_copy_w != r->w || g.stencil_copy_h != r->h) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w, r->h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        g.stencil_copy_w = r->w; g.stencil_copy_h = r->h;
    }
    p_glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, r->w, r->h);
    p_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    p_glStencilMask(255);
    p_glClearStencil(0); p_glClear(GL_STENCIL_BUFFER_BIT);
    p_glUniform1i(g.s_mode, 1);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    for (int bit = 1; bit <= 128; bit <<= 1) {
        p_glStencilMask((GLuint)bit);
        p_glStencilFunc(GL_ALWAYS, bit, 255);
        p_glUniform1i(g.s_bit, bit);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    r->stencil_valid = 1;
    g.stencil_imports++;
}

static void stencil_to_alpha(rendertarget *r) {
    if (!r->alpha_dirty) return;
    stencil_pass_state(r);
    p_glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    p_glClearColor(0, 0, 0, 0); p_glClear(GL_COLOR_BUFFER_BIT);
    p_glStencilMask(0);
    p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    p_glUniform1i(g.s_mode, 0);
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_ONE, GL_ONE); p_glBlendEquation(GL_FUNC_ADD);
    for (int bit = 1; bit <= 128; bit <<= 1) {
        p_glStencilFunc(GL_NOTEQUAL, 0, (GLuint)bit);
        p_glUniform1f(g.s_value, (float)bit / 255.0f);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    r->alpha_dirty = 0;
    g.stencil_exports++;
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
    g.u_placement = p_glGetUniformLocation(g.prog, "u_placement");
    g.u_ybias = p_glGetUniformLocation(g.prog, "u_ybias");
    g.u_atest    = p_glGetUniformLocation(g.prog, "u_atest");
    g.u_aref     = p_glGetUniformLocation(g.prog, "u_aref");
    g.u_amask    = p_glGetUniformLocation(g.prog, "u_amask");
    g.u_preblend_src = p_glGetUniformLocation(g.prog, "u_preblend_src");
    g.u_texenable = p_glGetUniformLocation(g.prog, "u_texenable");
    g.u_texfunc   = p_glGetUniformLocation(g.prog, "u_texfunc");
    g.u_tcc       = p_glGetUniformLocation(g.prog, "u_tcc");
    g.u_double    = p_glGetUniformLocation(g.prog, "u_double");
    g.u_env       = p_glGetUniformLocation(g.prog, "u_env");
    g.u_tex       = p_glGetUniformLocation(g.prog, "u_tex");
    g.u_texscale  = p_glGetUniformLocation(g.prog, "u_texscale");
    g.u_minfilter = p_glGetUniformLocation(g.prog, "u_minfilter");
    g.u_magfilter = p_glGetUniformLocation(g.prog, "u_magfilter");
    g.u_wraps     = p_glGetUniformLocation(g.prog, "u_wraps");
    g.u_wrapt     = p_glGetUniformLocation(g.prog, "u_wrapt");
    g.u_miptop    = p_glGetUniformLocation(g.prog, "u_miptop");
    g.u_fogenable = p_glGetUniformLocation(g.prog, "u_fogenable");
    g.u_fogcolour = p_glGetUniformLocation(g.prog, "u_fogcolour");
    return 0;
}

/* Find the record for an address.  Storage is deliberately deferred until the
 * first draw: FBP, FBW and FBFMT are separate GE registers, so set_target sees
 * several half-assembled combinations while a surface is being selected.  If
 * we allocated here, the first transient format would become permanent. */
static int rt_for(uint32_t addr) {
    addr &= PSP_ADDR_MASK;
    for (int i = 0; i < g.n_rts; i++)
        if (g.rts[i].addr == addr) return i;
    if (g.n_rts >= RT_MAX) { g.rt_overflow++; return g.cur_rt; }

    const int i = g.n_rts++;
    rendertarget *r = &g.rts[i];
    r->addr = addr; r->used = 1;

    return i;
}

/* ---- adaptive aspect ---------------------------------------------------------
 *
 * The guest framebuffer stays 480x272: movies, the 2D layer, CPU reads and the
 * alpha byte that is the stencil all depend on its layout. What changes is the
 * GL attachment behind the display targets, which grows to the virtual width
 * the camera replacement projected for (present_aspect_wide_width), and the
 * placement of each batch on it:
 *
 *   SCENE  guest x -> x * s, s = wide_w / 480. The camera compressed a wider
 *          view into 480 columns; this spreads it back out, rasterised at the
 *          target's own resolution rather than stretched afterwards.
 *   HUD    guest x -> x + off, off = (wide_w - 480) / 2. A whole-pixel shift,
 *          so a glyph lands on exactly the pixels it would at 1x. This is the
 *          property that keeps the UI sharp (findings: bilinear equals nearest
 *          at 1:1), and the reason no squeeze-and-stretch of the HUD can work.
 *
 * Both are viewports, not vertex transforms: the shader still maps guest x over
 * the guest width, so nothing is requantised. With wide_w == 480 every mapping
 * is the identity and the run is bit-identical to native. */
static double rt_off(const rendertarget *r) {
    if (!r->wide) return 0;
    return g.resolution ? (r->visible_w - g.w * r->ui_scale) * 0.5
                        : (r->wide_w - g.w) / 2;
}
static int rt_scene_w(const rendertarget *r) {
    return (int)lround(r->guest_w * r->sx);
}

/* One coherent drawable snapshot drives allocation for the next frame. */
static void resolution_size(void) {
    int w, h;
    present_gl_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    g.wide_w = g.w;
    if (g.adaptive_aspect) {
        long long wide = ((long long)g.h * w + h / 2) / h;
        g.wide_w = (int)(wide < g.w ? g.w : wide > 8192 ? 8192 : wide);
    }
    g.pixel_w = g.wide_w; g.pixel_h = g.h;
    if (!g.resolution) return;
    if (g.adaptive_aspect && (long long)w * g.h >= (long long)h * g.w) {
        g.pixel_w = w; g.pixel_h = h;
    } else {
        g.pixel_w = w;
        g.pixel_h = (int)((long long)w * g.h / g.w);
        if (g.pixel_h > h) {
            g.pixel_h = h;
            g.pixel_w = (int)((long long)h * g.w / g.h);
        }
    }
    if (g.pixel_w < 1) g.pixel_w = 1;
    if (g.pixel_h < 1) g.pixel_h = 1;
}

static int pixel_edge(double x) { return (int)ceil(x - 0.5); }

static void rt_layout(rendertarget *r) {
    r->display = r->stride >= (uint32_t)g.w && r->guest_h >= g.h;
    r->wide = r->display && g.adaptive_aspect && g.wide_w > g.w;
    r->wide_w = r->wide ? g.wide_w : g.w;
    r->visible_w = r->wide_w;
    r->visible_h = g.h;
    if (g.resolution && r->display) {
        r->visible_w = g.pixel_w;
        r->visible_h = g.pixel_h;
    }
    r->sx = r->display ? (double)r->visible_w / g.w : 1.0;
    r->sy = r->display ? (double)r->visible_h / g.h : 1.0;
    r->ui_scale = r->sy;
    double cap =
        fmin((double)g.max_size / (r->guest_w * r->sx), (double)g.max_size / (r->guest_h * r->sy));
    if (cap < 1.0) {
        r->visible_w = (int)fmax(1, floor(r->visible_w * cap));
        r->visible_h = (int)fmax(1, floor(r->visible_h * cap));
        r->sx = (double)r->visible_w / g.w;
        r->sy = (double)r->visible_h / g.h;
        r->ui_scale *= cap;
        fprintf(stderr, "gl: resolution capped to %dx%d by GL limit %d\n", r->visible_w,
                r->visible_h, g.max_size);
    }
    r->w = (int)ceil(r->guest_w * r->sx);
    r->h = (int)ceil(r->guest_h * r->sy);
}

static void rt_release(rendertarget *r) {
    if (r->fbo) p_glDeleteFramebuffers(1, &r->fbo);
    if (r->colour) p_glDeleteTextures(1, &r->colour);
    if (r->depth) p_glDeleteRenderbuffers(1, &r->depth);
    free(r->cpu_dirty);
    r->fbo = r->colour = r->depth = 0;
    r->cpu_dirty = NULL;
    r->configured = 0;
}

static uint32_t decode_pixel(uint32_t v, int fmt) {
    if (fmt == 3) return v;
    unsigned red, green, blue, alpha;
    if (fmt == 2) {
        red = (v & 15) * 17;
        green = ((v >> 4) & 15) * 17;
        blue = ((v >> 8) & 15) * 17;
        alpha = ((v >> 12) & 15) * 17;
    } else {
        red = v & 31;
        red = (red << 3) | (red >> 2);
        if (fmt == 0) {
            green = (v >> 5) & 63;
            green = (green << 2) | (green >> 4);
            blue = (v >> 11) & 31;
            alpha = 255;
        } else {
            green = (v >> 5) & 31;
            green = (green << 3) | (green >> 2);
            blue = (v >> 10) & 31;
            alpha = (v & 0x8000) ? 255 : 0;
        }
        blue = (blue << 3) | (blue >> 2);
    }
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static uint32_t pack_pixel(uint32_t rgba, int fmt) {
    if (fmt == 3) return rgba;
    unsigned r = rgba & 255, g = (rgba >> 8) & 255, b = (rgba >> 16) & 255, a = rgba >> 24;
    if (fmt == 0) return (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
    if (fmt == 1) return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
    return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
}

static uint32_t guest_pixel(const rendertarget *r, int x, int y) {
    if (x >= (int)r->stride) return 0;
    const int bpp = r->fmt == 3 ? 4 : 2;
    const void *p = psp_mem_ptr(r->addr + (uint32_t)(y * r->stride + x) * bpp, bpp);
    uint32_t v = 0;
    if (p) memcpy(&v, p, bpp);
    return decode_pixel(v, r->fmt);
}

/* Track precise touched pixels, including a CPU store of the same byte value.
 * Generation/byte comparisons alone miss such a store over newer GPU pixels. */
static void rt_guest_write(uint32_t addr, uint32_t size) {
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || !r->cpu_dirty || g.exporting == i + 1) continue;
        const unsigned bpp = r->fmt == 3 ? 4 : 2;
        const uint64_t end = (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * bpp;
        if ((uint64_t)addr + size <= r->addr || addr >= end) continue;
        const uint64_t first = addr > r->addr ? addr - r->addr : 0;
        const uint64_t last =
            (uint64_t)addr + size < end ? (uint64_t)addr + size - r->addr : end - r->addr;
        for (uint64_t p = first / bpp; p < (last + bpp - 1) / bpp; p++) {
            unsigned mask = 0;
            for (unsigned b = 0; b < bpp; b++)
                if (p * bpp + b >= first && p * bpp + b < last) mask |= 1u << b;
            r->cpu_dirty[p] |= mask;
        }
        r->cpu_pending = 1;
    }
}

static int rt_allocate(rendertarget *r, int inherit) {
    p_glGenFramebuffers(1, &r->fbo);
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    p_glGenTextures(1, &r->colour);
    p_glBindTexture(GL_TEXTURE_2D, r->colour);
    uint32_t *initial = NULL;
    if (inherit && (r->fmt == 3 || g.resolution)) {
        initial = calloc((size_t)r->w * r->h, 4);
        if (!initial) {
            rt_release(r);
            return -1;
        }
        for (int y = 0; y < r->h; y++)
            for (int x = 0; x < r->w; x++) {
                int gx = (int)((x + 0.5) / r->sx), gy = (int)((y + 0.5) / r->sy);
                if (gx >= r->guest_w) gx = r->guest_w - 1;
                if (gy >= r->guest_h) gy = r->guest_h - 1;
                initial[(size_t)(r->h - 1 - y) * r->w + x] = guest_pixel(r, gx, gy);
            }
    }
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r->w, r->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, initial);
    free(initial);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->colour, 0);
    p_glGenRenderbuffers(1, &r->depth);
    p_glBindRenderbuffer(GL_RENDERBUFFER, r->depth);
    p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, r->w, r->h);
    p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                r->depth);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: target %08X allocation %dx%d failed\n", r->addr, r->w, r->h);
        rt_release(r);
        return -1;
    }
    if (g.resolution) {
        r->cpu_dirty = calloc((size_t)r->stride * r->guest_h, 1);
        if (!r->cpu_dirty) {
            rt_release(r);
            return -1;
        }
    }
    r->configured = 1;
    if (r->wide) g.wide_allocs++;
    return 0;
}

/* Guest reconfiguration reinterprets bytes; resizing preserves GPU history. */
static int rt_prepare(int i) {
    rendertarget *r = &g.rts[i];
    const int stride = g.target_stride ? (int)g.target_stride : g.w;
    int w = stride, h = g.sc_valid ? g.sc_y1 + 1 : g.h;
    if (g.sc_valid && g.sc_x1 + 1 > w) w = g.sc_x1 + 1;
    if (w < 1) w = g.w;
    if (h < 1) h = g.h;
    if (r->configured) {
        if (r->stride == (uint32_t)stride && r->fmt == g.target_fmt && r->guest_w >= w &&
            r->guest_h >= h)
            return 0;
        if (r->dirty || r->cpu_pending) readback_rt(i);
        if (r->stride == (uint32_t)stride && r->fmt == g.target_fmt) {
            if (w < r->guest_w) w = r->guest_w;
            if (h < r->guest_h) h = r->guest_h;
        }
        rt_release(r);
    }
    r->stride = stride;
    r->fmt = g.target_fmt;
    r->guest_w = w;
    r->guest_h = h;
    r->dirty = r->alpha_dirty = r->stencil_valid = r->cpu_pending = 0;
    rt_layout(r);
    return rt_allocate(r, 1);
}

static void rt_import(rendertarget *r) {
    if (!r->cpu_pending || !r->configured) return;
    stencil_to_alpha(r);
    uint32_t *row = malloc((size_t)r->w * 4), *values = malloc((size_t)r->stride * 4);
    if (!row || !values) {
        free(row);
        free(values);
        g.failed = 1;
        return;
    }
    p_glBindTexture(GL_TEXTURE_2D, r->colour);
    for (int y = 0; y < r->guest_h; y++) {
        int gy0 = pixel_edge(y * r->sy), gy1 = pixel_edge((y + 1) * r->sy);
        for (int x = 0; x < (int)r->stride;) {
            if (!r->cpu_dirty[(size_t)y * r->stride + x]) {
                x++;
                continue;
            }
            const int start = x;
            while (x < (int)r->stride && r->cpu_dirty[(size_t)y * r->stride + x]) x++;
            int left = pixel_edge(start * r->sx), right = pixel_edge(x * r->sx);
            if (right > r->w) right = r->w;
            if (gy1 > r->h) gy1 = r->h;
            if (right <= left || gy1 <= gy0) continue;
            const int bpp = r->fmt == 3 ? 4 : 2;
            const unsigned full = (1u << bpp) - 1;
            int partial = 0;
            for (int gx = start; gx < x; gx++) {
                values[gx] = guest_pixel(r, gx, y);
                if (r->cpu_dirty[(size_t)y * r->stride + gx] != full) partial = 1;
            }
            /* A byte write owns only those channels. Preserve every physical
             * sample of the others, not just one resolved PSP pixel -- an
             * alpha-only CPU write must not erase a fine RGB edge. Full-pixel
             * uploads keep the fast path with no GPU read. */
            uint32_t *previous = NULL;
            if (partial) {
                previous = malloc((size_t)(right - left) * (gy1 - gy0) * 4);
                if (!previous) {
                    free(row);
                    free(values);
                    g.failed = 1;
                    return;
                }
                p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
                p_glReadPixels(left, r->h - gy1, right - left, gy1 - gy0, GL_RGBA, GL_UNSIGNED_BYTE,
                               previous);
            }
            for (int py = gy0; py < gy1; py++) {
                for (int k = left; k < right; k++) {
                    const int gx = (int)((k + 0.5) / r->sx);
                    const unsigned mask = r->cpu_dirty[(size_t)y * r->stride + gx];
                    uint32_t value = values[gx];
                    if (mask != full) {
                        uint32_t old = pack_pixel(
                            previous[(size_t)(gy1 - 1 - py) * (right - left) + k - left], r->fmt);
                        uint32_t cpu = pack_pixel(value, r->fmt), bits = 0;
                        for (int b = 0; b < bpp; b++)
                            if (mask & (1u << b)) bits |= 255u << (b * 8);
                        value = decode_pixel((old & ~bits) | (cpu & bits), r->fmt);
                    }
                    row[k - left] = value;
                }
                p_glTexSubImage2D(GL_TEXTURE_2D, 0, left, r->h - 1 - py, right - left, 1, GL_RGBA,
                                  GL_UNSIGNED_BYTE, row);
            }
            free(previous);
        }
    }
    free(row);
    free(values);
    memset(r->cpu_dirty, 0, (size_t)r->stride * r->guest_h);
    r->cpu_pending = 0;
    r->stencil_valid = 0;
    r->dirty = 1;
    g.cpu_uploads++;
}

static void rt_resize_all(void) {
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || !r->display) continue;
        rendertarget next = *r;
        rt_layout(&next);
        if (r->w == next.w && r->h == next.h && r->wide_w == next.wide_w &&
            r->visible_w == next.visible_w && r->visible_h == next.visible_h)
            continue;
        rt_import(r);
        stencil_to_alpha(r);
        next.fbo = next.colour = next.depth = 0;
        next.cpu_dirty = NULL;
        if (rt_allocate(&next, 0) != 0) continue; /* keep the working allocation */
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, next.fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, 0, r->w, r->h, 0, 0, next.w, next.h,
                            GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT,
                            GL_NEAREST);
        next.stencil_valid = r->stencil_valid;
        next.alpha_dirty = 0;
        next.cpu_pending = 0;
        next.dirty = r->dirty;
        rt_release(r);
        *r = next;
        g.resizes++;
        g.wide_retired++;
    }
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
    GLint tex_limit, rb_limit, viewport_limit[2];
    p_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &tex_limit);
    p_glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &rb_limit);
    p_glGetIntegerv(GL_MAX_VIEWPORT_DIMS, viewport_limit);
    g.max_size = tex_limit < rb_limit ? tex_limit : rb_limit;
    if (viewport_limit[0] < g.max_size) g.max_size = viewport_limit[0];
    if (viewport_limit[1] < g.max_size) g.max_size = viewport_limit[1];

    if (build_program() != 0) { g.failed = 1; return -1; }
    if (build_stencil_program() != 0) { g.failed = 1; return -1; }

    /* Desktop GL starts with dithering enabled. The backend contract is the
     * currently undithered software GE, so an implicit host dither pattern is
     * never valid state and turns exact RGBA8 arithmetic back into LSB noise. */
    p_glDisable(GL_DITHER);

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
    p_glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(9 * sizeof(float)));
    p_glEnableVertexAttribArray(3);
    p_glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(10 * sizeof(float)));
    p_glEnableVertexAttribArray(4);
    p_glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(11 * sizeof(float)));
    p_glEnableVertexAttribArray(5);
    p_glVertexAttribPointer(6, 1, GL_FLOAT, GL_FALSE,
                            FLOATS_PER_VERT * sizeof(float),
                            (void *)(12 * sizeof(float)));
    p_glEnableVertexAttribArray(6);

    g.batch = calloc(GL_MAX_VERTS * FLOATS_PER_VERT, sizeof(float));
    if (!g.batch) { g.failed = 1; return -1; }
    p_glGenQueries(GPU_QUERY_RING, g.gpu_query);

    fprintf(stderr, "gl: context claimed on thread %lu, %dx%d target\n",
            me, g.w, g.h);
    g.ready = 1;
    resolution_size();
    if (g.resolution) psp_mem_set_write_observer(rt_guest_write);
    /* Whatever the GE last named, or the primary display buffer if it has not
     * named one yet -- a target has to exist before the first draw. */
    g.cur_rt = rt_for(g.target_addr ? g.target_addr : 0x04000000u);
    return 0;
}

/* ---- the interface ---------------------------------------------------------- */

static int gl_init(int w, int h) {
    /* No GL here on purpose: this runs on boot.c's thread, not the GE's. */
    g.w = w; g.h = h;
    g.wide_w = w;
    g.pixel_w = w; g.pixel_h = h;
    g.resolution = render_gl_resolution_mode();
    if (g.resolution < 0) return -1;
    g.adaptive_aspect = present_adaptive_aspect();
    return 0;
}

static void gl_shutdown(void) {
    if (g.resolution) psp_mem_set_write_observer(NULL);
}

static void gl_target(uint32_t addr, uint32_t stride, int fmt) {
    /* No claim() here: set_target is the one setter ge.c calls while the
     * register is still being assembled, long before any drawing, and on a
     * run that never draws it would otherwise create a context for nothing. */
    if (g.ready) flush();
    g.target_addr = addr; g.target_stride = stride; g.target_fmt = fmt;
    if (g.ready && addr) {
        g.cur_rt = rt_for(addr);
    }
}

/* Every setter flushes what is pending before recording. A state change
 * applies from the next primitive onward; without the flush it would apply
 * retroactively to geometry already sitting in the batch. */
static void gl_scissor(int x0, int y0, int x1, int y1) {
    if (claim() != 0) return;
    flush();
    g.sc_x0 = x0; g.sc_y0 = y0; g.sc_x1 = x1; g.sc_y1 = y1; g.sc_valid = 1;
}
/* Only a mipmap minification filter consumes the extra levels. TEXMODE may
 * retain a non-zero top while a draw deliberately selects ordinary nearest or
 * linear filtering; uploading those unreachable levels would make a sampling
 * state change invalidate an otherwise identical cache entry for no result. */
static int texture_top(const psp_tex_state *t) {
    if (t->min_filter < 4 || t->max_level <= 0) return 0;
    return t->max_level > 7 ? 7 : t->max_level;
}

static uint32_t level_addr(const psp_tex_state *t, int level) {
    return level ? t->lv_addr[level] : t->addr;
}
static uint32_t level_stride(const psp_tex_state *t, int level) {
    return level ? t->lv_stride[level] : t->stride;
}
static int level_w(const psp_tex_state *t, int level) {
    return level ? t->lv_w[level] : t->w;
}
static int level_h(const psp_tex_state *t, int level) {
    return level ? t->lv_h[level] : t->h;
}

/* Bytes the shared decoder can touch for one level. A swizzled texture is laid
 * out in eight-row blocks, so a short final block occupies its padded height;
 * tracking only visible rows would miss a write to a texel the swizzle maps
 * beyond stride*height. */
static uint32_t level_bytes(const psp_tex_state *t, int level) {
    static const int halfbytes[8] = { 4, 4, 4, 8, 1, 2, 4, 8 };
    const int fmt = t->fmt;
    const uint32_t stride = level_stride(t, level);
    const int h = level_h(t, level);
    if (fmt < 0 || fmt >= 8 || !stride || h <= 0) return 0;
    const uint64_t row = ((uint64_t)stride * (uint64_t)halfbytes[fmt]) / 2u;
    const uint64_t rows = t->swizzled && row >= 16u
                        ? (uint64_t)(h + 7) & ~UINT64_C(7)
                        : (uint64_t)h;
    const uint64_t bytes = row * rows;
    return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

static uint64_t texture_generation(const psp_tex_state *t, int top) {
    uint64_t newest = 0;
    for (int level = 0; level <= top; level++) {
        const uint64_t gen = psp_mem_range_generation(level_addr(t, level),
                                                       level_bytes(t, level));
        if (gen > newest) newest = gen;
    }
    if (t->fmt >= 4 && t->fmt <= 7 && g.clut_addr) {
        /* CLUT start is already in entries (multiples of sixteen), and mask is
         * applied before ORing it. Tracking through their greatest possible
         * index covers every palette read without knowing the texel values. */
        const uint32_t entries = (uint32_t)(g.clut_start | g.clut_mask) + 1u;
        const uint32_t bytes = entries * (g.clut_fmt == 3 ? 4u : 2u);
        const uint64_t gen = psp_mem_range_generation(g.clut_addr, bytes);
        if (gen > newest) newest = gen;
    }
    return newest;
}

static uint32_t hash_word(uint32_t hash, uint32_t word) {
    return (hash ^ word) * UINT32_C(16777619);
}

static size_t texture_slot(const psp_tex_state *t, int top) {
    uint32_t hash = UINT32_C(2166136261);
    hash = hash_word(hash, (uint32_t)t->fmt);
    hash = hash_word(hash, (uint32_t)t->swizzled);
    hash = hash_word(hash, (uint32_t)top);
    for (int level = 0; level <= top; level++) {
        hash = hash_word(hash, level_addr(t, level));
        hash = hash_word(hash, level_stride(t, level));
        hash = hash_word(hash, (uint32_t)level_w(t, level));
        hash = hash_word(hash, (uint32_t)level_h(t, level));
    }
    if (t->fmt >= 4 && t->fmt <= 7) {
        hash = hash_word(hash, g.clut_addr);
        hash = hash_word(hash, (uint32_t)g.clut_fmt);
        hash = hash_word(hash, (uint32_t)g.clut_shift);
        hash = hash_word(hash, (uint32_t)g.clut_mask);
        hash = hash_word(hash, (uint32_t)g.clut_start);
    }
    return (size_t)hash % TEXCACHE_MAX;
}

static int cache_matches(const texcache_entry *e, const psp_tex_state *t,
                         int top) {
    if (!e->used || e->fmt != t->fmt || e->swizzled != t->swizzled ||
        e->max_level != top)
        return 0;
    /* Palette state has no bearing on direct-colour formats. Including the
     * GE's incidental last CLUT there multiplied identical cache entries and
     * was responsible for most of the mission's post-generation evictions. */
    if (t->fmt >= 4 && t->fmt <= 7 &&
        (e->clut_addr != g.clut_addr || e->clut_fmt != g.clut_fmt ||
         e->clut_shift != g.clut_shift || e->clut_mask != g.clut_mask ||
         e->clut_start != g.clut_start))
        return 0;
    for (int level = 0; level <= top; level++)
        if (e->lv_addr[level] != level_addr(t, level) ||
            e->lv_stride[level] != level_stride(t, level) ||
            e->lv_w[level] != level_w(t, level) ||
            e->lv_h[level] != level_h(t, level))
            return 0;
    return 1;
}

static void cache_record(texcache_entry *e, const psp_tex_state *t, int top,
                         int uploaded_top, uint64_t generation,
                         uint64_t serial) {
    if (!e->used) g.cache_entries++;
    e->used = 1;
    e->addr = t->addr; e->stride = t->stride; e->w = t->w; e->h = t->h;
    e->fmt = t->fmt; e->swizzled = t->swizzled;
    e->max_level = top; e->uploaded_top = uploaded_top;
    for (int level = 0; level <= top; level++) {
        e->lv_addr[level] = level_addr(t, level);
        e->lv_stride[level] = level_stride(t, level);
        e->lv_w[level] = level_w(t, level);
        e->lv_h[level] = level_h(t, level);
    }
    e->clut_addr = g.clut_addr; e->clut_fmt = g.clut_fmt;
    e->clut_shift = g.clut_shift; e->clut_mask = g.clut_mask;
    e->clut_start = g.clut_start;
    e->content_generation = generation;
    e->validated_serial = serial;
    e->last_used = g.cache_clock;
}

static GLuint texcache_native(const psp_tex_state *t) {
    if (!t->addr || t->w <= 0 || t->h <= 0) return 0;
    g.tex_requests++;
    const int top = texture_top(t);
    /* A target can only be sampled directly when the texture describes the
     * same pixel layout.  The AC scratch surface is the important opposite
     * case: it is rendered as 5551, then deliberately read as CLUT8 through a
     * palette.  Returning its RGBA attachment used to skip that byte-level
     * reinterpretation and produced the bright menu shards and mirrored
     * mission foreground.  Synchronise such aliases to guest memory and send
     * them through the common decoder instead. */
    for (int i = 0; i < g.n_rts; i++) {
        if (g.rts[i].used && g.rts[i].addr == t->addr) {
            g.tex_from_rt++;
            rendertarget *r = &g.rts[i];
            if (!g.resolution && r->configured && r->fmt == 3 && t->fmt == 3 &&
                !t->swizzled && t->stride == r->stride &&
                t->w == r->w && t->h == r->h && top == 0) {
                g.bound_top = 0;
                stencil_to_alpha(r);
                return r->colour;
            }
            if (r->dirty) {
                readback_rt(i);
                r->dirty = 0;
            }
            g.tex_alias_from_rt++;
            break;
        }
    }
    /* A lower mip can independently alias a render target. Synchronise every
     * level the sampler can reach before decoding; checking only level zero
     * leaves a perfectly keyed cache holding yesterday's generated mip. */
    for (int level = 1; level <= top; level++) {
        const uint32_t addr = level_addr(t, level);
        for (int i = 0; addr && i < g.n_rts; i++) {
            rendertarget *r = &g.rts[i];
            if (!r->used || r->addr != addr) continue;
            g.tex_from_rt++;
            if (r->dirty) { readback_rt(i); r->dirty = 0; }
            g.tex_alias_from_rt++;
            break;
        }
    }
    for (int level = 0; level <= top; level++)
        if (level_w(t, level) <= 0 || level_h(t, level) <= 0 ||
            (size_t)level_w(t, level) * (size_t)level_h(t, level) > TEXEL_CAP) {
            g.tex_too_big++;
            return 0;
        }

    size_t slot = texture_slot(t, top);

    /* Textures in VRAM used to skip this lookup altogether. That avoided a
     * stale render-to-texture result, but decoded and uploaded every binding --
     * 232,297 times in the first full mission measurement. Write generations
     * let VRAM use the same cache while preserving the in-place update. */
    int in_vram = 0;
    for (int level = 0; level <= top; level++)
        if ((level_addr(t, level) & 0xFF000000u) == 0x04000000u)
            in_vram = 1;

    const uint64_t memory_serial = psp_mem_write_serial();
    g.cache_clock++;
    size_t victim = slot;
    uint64_t oldest = UINT64_MAX;
    for (size_t probe = 0; probe < TEXCACHE_PROBES; probe++) {
        const size_t at = (slot + probe) % TEXCACHE_MAX;
        texcache_entry *e = &g.cache[at];
        if (cache_matches(e, t, top)) {
            if (e->validated_serial == memory_serial) {
                g.tex_fast_hits++;
            } else {
                const uint64_t gen_t0 = psp_os_mono_ns();
                const uint64_t generation = texture_generation(t, top);
                g.tex_generation_ns += psp_os_mono_ns() - gen_t0;
                if (generation != e->content_generation) {
                    g.tex_invalidations++;
                    slot = at;
                    goto upload;
                }
                e->validated_serial = memory_serial;
                g.tex_revalidated++;
            }
            g.tex_hits++;
            e->last_used = g.cache_clock;
            g.bound_top = e->uploaded_top;
            return e->tex;
        }
        if (!e->used) { slot = (slot + probe) % TEXCACHE_MAX; goto upload; }
        if (e->last_used < oldest) { oldest = e->last_used; victim = at; }
    }
    /* The bounded probe window keeps lookup cost predictable. If it fills,
     * retain the hot entries instead of repeatedly replacing the hash's first
     * slot; the eviction counter says whether 512 entries/32 probes suffices. */
    slot = victim;
    g.tex_evictions++;
upload: {
    texcache_entry *e = &g.cache[slot];
    if (!cache_matches(e, t, top)) g.tex_misses++;
    static uint32_t *texels;
    if (!texels) texels = malloc(TEXEL_CAP * sizeof(uint32_t));
    if (!texels) return 0;

    /* One decoder for every backend -- the formats, the CLUT paging and the
     * swizzle are the runtime's, not repeated here. */
    const psp_clut_state clut = { g.clut_addr, g.clut_fmt, g.clut_shift,
                                  g.clut_mask, g.clut_start };
    if (!e->tex) p_glGenTextures(1, &e->tex);
    p_glBindTexture(GL_TEXTURE_2D, e->tex);
    int uploaded_top = -1;
    for (int level = 0; level <= top; level++) {
        int dw = 0, dh = 0;
        const uint64_t decode_t0 = psp_os_mono_ns();
        const size_t decoded = psp_render_decode_level(t, level, &clut,
                                                       texels, TEXEL_CAP,
                                                       &dw, &dh);
        g.tex_decode_ns += psp_os_mono_ns() - decode_t0;
        if (decoded == 0)
            break;
        const uint64_t upload_t0 = psp_os_mono_ns();
        p_glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA8, dw, dh, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, texels);
        g.tex_upload_ns += psp_os_mono_ns() - upload_t0;
        g.tex_upload_pixels += decoded;
        uploaded_top = level;
        if (level) g.mip_levels++;
    }
    if (uploaded_top < 0) return 0;
    if (uploaded_top < top) g.mip_incomplete++;
    if (uploaded_top > 0) g.mip_chains++;
    /* texelFetch performs the PSP's filtering in the shader. Keeping the GL
     * sampler itself non-mipmapped also permits the PSP's independently-sized
     * levels without making the texture incomplete under GL's stricter
     * halving rule. */
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, uploaded_top);
    const uint64_t gen_t0 = psp_os_mono_ns();
    const uint64_t generation = texture_generation(t, top);
    g.tex_generation_ns += psp_os_mono_ns() - gen_t0;
    cache_record(e, t, top, uploaded_top, generation,
                 psp_mem_write_serial());
    g.bound_top = uploaded_top;
    g.tex_uploads++;
    if (in_vram) g.tex_vram_uploads++;
    return e->tex;
}
}

/* A view has the texture's declared extent, populated from guest bytes and
 * overlaid with all compatible GPU-owned rows. Its top row is texture row 0,
 * unlike a render attachment. Snapshotting also makes self-composites safe. */
static GLuint texcache_get(const psp_tex_state *t) {
    g.tex_sx = g.tex_sy = 1;
    int hit = 0;
    double sx = 1, sy = 1;
    const uint32_t base = t->addr & PSP_ADDR_MASK;
    const uint64_t end = (uint64_t)base + (uint64_t)t->stride * t->h * (t->fmt == 3 ? 4 : 2);
    if (g.resolution)
        for (int i = 0; i < g.n_rts; i++) {
            rendertarget *r = &g.rts[i];
            const uint64_t re =
                (uint64_t)r->addr + (uint64_t)r->stride * r->guest_h * (r->fmt == 3 ? 4 : 2);
            if (!r->configured || base >= re || end <= r->addr) continue;
            if (r->dirty || r->cpu_pending) {
                readback_rt(i);
                r->dirty = 0;
            }
            if (t->fmt == 3 && r->fmt == 3 && !t->swizzled && texture_top(t) == 0 &&
                t->stride == r->stride &&
                ((int64_t)r->addr - base) % ((int64_t)t->stride * 4) == 0) {
                hit = 1;
                if (r->sx > sx) sx = r->sx;
                if (r->sy > sy) sy = r->sy;
            }
        }
    GLuint native = texcache_native(t);
    if (!hit || !native) return native;
    const int w = (int)ceil(t->w * sx), h = (int)ceil(t->h * sy);
    if (w > g.max_size || h > g.max_size || w < 1 || h < 1) return native;
    if (!g.view_fbo) {
        p_glGenFramebuffers(1, &g.view_fbo);
        p_glGenFramebuffers(1, &g.copy_fbo);
        p_glGenTextures(1, &g.view_tex);
    }
    p_glBindTexture(GL_TEXTURE_2D, g.view_tex);
    if (g.view_w != w || g.view_h != h) {
        p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        g.view_w = w;
        g.view_h = h;
    }
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.view_fbo);
    p_glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.view_tex,
                             0);
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.copy_fbo);
    p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, native, 0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glBlitFramebuffer(0, 0, t->w, t->h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || r->fmt != 3 || t->stride != r->stride) continue;
        const int64_t delta = (int64_t)r->addr - base, pitch = (int64_t)t->stride * 4;
        if (delta % pitch) continue;
        const int row = (int)(delta / pitch);
        const int y0 = row < 0 ? -row : 0;
        const int y1 = row + r->guest_h > t->h ? t->h - row : r->guest_h;
        if (y0 >= y1) continue;
        const int cols = t->w < r->guest_w ? t->w : r->guest_w;
        rt_import(r);
        stencil_to_alpha(r);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.view_fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glBlitFramebuffer(0, r->h - pixel_edge(y0 * r->sy), pixel_edge(cols * r->sx),
                            r->h - pixel_edge(y1 * r->sy), 0, pixel_edge((row + y0) * sy),
                            pixel_edge(cols * sx), pixel_edge((row + y1) * sy), GL_COLOR_BUFFER_BIT,
                            GL_NEAREST);
    }
    g.tex_sx = (float)sx;
    g.tex_sy = (float)sy;
    g.bound_top = 0;
    g.rt_views++;
    return g.view_tex;
}

static void gl_texture(const psp_tex_state *t) {
    if (claim() != 0) return;
    flush();
    g.tex = *t;
    g.tex_enable = t->addr != 0;
    g.bound_top = 0;
    g.tex_sx = g.tex_sy = 1;
    const uint64_t bind_t0 = psp_os_mono_ns();
    g.bound = g.tex_enable ? texcache_get(t) : 0;
    g.tex_bind_ns += psp_os_mono_ns() - bind_t0;
    if (g.bound) {
        p_glBindTexture(GL_TEXTURE_2D, g.bound);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, g.bound_top);
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
static void gl_fog(int enable, uint32_t colour) {
    if (claim() != 0) return;
    flush();
    g.fog_enable = enable;
    g.fog_colour = colour;
}

static void reserve_vertices(size_t n) {
    /* Flush only between complete triangles. GL_MAX_VERTS is not divisible by
     * three, so checking one vertex at a time can strand one endpoint at the
     * end of one draw and two at the start of the next. */
    if (g.batch_n + n > GL_MAX_VERTS) { flush(); g.batch_overflows++; }
}

static void push(const psp_vertex *v, int lod16) {
    float *o = g.batch + g.batch_n * FLOATS_PER_VERT;
    /* The reference path keeps PSP quantization. Enhanced display targets can
     * use the float position retained by the GE; byte-interpreted scratch
     * surfaces continue using the exact 1x coordinate contract. */
    const int precise = g.resolution && g.rts[g.cur_rt].display && v->precise;
    o[0] = precise ? v->precise_x : (float)v->x / PSP_SUBPX;
    o[1] = precise ? v->precise_y : (float)v->y / PSP_SUBPX;
    o[2] = v->z / 65535.0f;                /* the PSP's window depth scale */
    o[3] = (float)( v->rgba        & 0xFF) / 255.0f;
    o[4] = (float)((v->rgba >>  8) & 0xFF) / 255.0f;
    o[5] = (float)((v->rgba >> 16) & 0xFF) / 255.0f;
    o[6] = (float)((v->rgba >> 24) & 0xFF) / 255.0f;
    /* Keep UVs in the texel units the backend contract supplies. The shader
     * scales those units for each mip level; normalising here and multiplying
     * back there moves exact 1/16 boundaries through an avoidable round trip. */
    o[7] = v->u;
    o[8] = v->v;
    o[9] = (float)v->fog / 255.0f;
    o[10] = v->inv_w;
    o[11] = v->tex_q;
    o[12] = (float)lod16;
    g.batch_n++;
}

/* Match sw_tri's per-primitive scale calculation. In particular this is based
 * on the submitted texel coordinates, not on GL's per-fragment derivatives:
 * CONST and SLOPE have no derivative at all, while AUTO is measured by the PSP
 * once for the primitive and quantised to a sixteenth before adding its bias. */
static int triangle_lod16(const psp_vertex *a, const psp_vertex *b,
                          const psp_vertex *c) {
    if (!g.tex_enable) return 0;
    const float e1x = (float)(b->x - a->x) / 16.0f;
    const float e1y = (float)(b->y - a->y) / 16.0f;
    const float e2x = (float)(c->x - a->x) / 16.0f;
    const float e2y = (float)(c->y - a->y) / 16.0f;
    const float det = e1x * e2y - e1y * e2x;
    if (det == 0.0f) return 0;
    const float du1 = b->u - a->u, du2 = c->u - a->u;
    const float dv1 = b->v - a->v, dv2 = c->v - a->v;
    const float dudx = (du1 * e2y - du2 * e1y) / det;
    const float dudy = (du2 * e1x - du1 * e2x) / det;
    const float dvdx = (dv1 * e2y - dv2 * e1y) / det;
    const float dvdy = (dv2 * e1x - dv1 * e2x) / det;
    const float rx = sqrtf(dudx * dudx + dvdx * dvdx);
    const float ry = sqrtf(dudy * dudy + dvdy * dvdy);
    return psp_render_lod16(&g.tex, rx > ry ? rx : ry);
}

static void push_triangle(const psp_vertex *a, const psp_vertex *b,
                          const psp_vertex *c) {
    const int lod16 = triangle_lod16(a, b, c);
    reserve_vertices(3);
    push(a, lod16); push(b, lod16); push(c, lod16);
}

/* A sprite is two triangles from opposite corners, axis-aligned, taking its
 * colour and fog from the second vertex and depth from the first the way the
 * software path does. The other two UVs have to be made here: copying the
 * second vertex to all four corners collapses every textured sprite to one
 * texel.
 *
 * With exactly one screen axis reversed, the PSP transposes the mapping: u
 * follows y and v follows x. This is the same rule measured and implemented
 * by sw_sprite(), rather than a GL-specific approximation. */
static void push_sprite(const psp_vertex *v) {
    psp_vertex a = v[1], b = v[1], c = v[1], d = v[1];
    a.x = v[0].x; a.y = v[0].y;
    b.x = v[1].x; b.y = v[0].y;
    c.x = v[1].x; c.y = v[1].y;
    d.x = v[0].x; d.y = v[1].y;
    a.precise_x = d.precise_x = v[0].precise_x;
    a.precise_y = b.precise_y = v[0].precise_y;
    b.precise_x = c.precise_x = v[1].precise_x;
    c.precise_y = d.precise_y = v[1].precise_y;
    a.precise = b.precise = c.precise = d.precise = v[0].precise && v[1].precise;
    a.z = b.z = c.z = d.z = v[0].z;
    /* A sprite's two-corner mapping is affine even when its endpoints came
     * through the transform pipeline.  The software rectangle path has the
     * same rule; forcing homogeneous ones keeps the GL expansion equivalent. */
    a.inv_w = b.inv_w = c.inv_w = d.inv_w = 1.0f;
    a.tex_q = b.tex_q = c.tex_q = d.tex_q = 1.0f;
    a.u = v[0].u; a.v = v[0].v;
    c.u = v[1].u; c.v = v[1].v;
    const int transposed = (v[1].x < v[0].x) != (v[1].y < v[0].y);
    if (transposed) {
        b.u = v[0].u; b.v = v[1].v;
        d.u = v[1].u; d.v = v[0].v;
    } else {
        b.u = v[1].u; b.v = v[0].v;
        d.u = v[0].u; d.v = v[1].v;
    }
    int lod16 = 0;
    const int dx = v[1].x - v[0].x, dy = v[1].y - v[0].y;
    const int uden = transposed ? dy : dx;
    const int vden = transposed ? dx : dy;
    if (g.tex_enable && uden && vden) {
        const float du = (v[1].u - v[0].u) / (float)uden;
        const float dv = (v[1].v - v[0].v) / (float)vden;
        const float rx = fabsf(du) * 16.0f, ry = fabsf(dv) * 16.0f;
        lod16 = psp_render_lod16(&g.tex, rx > ry ? rx : ry);
    }
    reserve_vertices(6);
    push(&a, lod16); push(&b, lod16); push(&c, lod16);
    push(&a, lod16); push(&c, lod16); push(&d, lod16);
}

/* Explicit pixel quads avoid GL's implementation-dependent native line/point
 * coverage. The shared walker defines coverage only; these fragments still
 * pass through the normal texture, alpha, depth, stencil and blend pipeline. */
static void push_point_sample(const psp_vertex *v, void *opaque) {
    psp_vertex a = *v, b = *v, c = *v, d = *v;
    a.precise = b.precise = c.precise = d.precise = 0;
    const int x = (int)floorf((float)v->x / PSP_SUBPX);
    const int y = (int)floorf((float)v->y / PSP_SUBPX);
    if (x < 0 || y < 0 || x >= g.rts[g.cur_rt].guest_w || y >= g.rts[g.cur_rt].guest_h) return;
    a.x = d.x = x * PSP_SUBPX; b.x = c.x = a.x + PSP_SUBPX;
    a.y = b.y = y * PSP_SUBPX; c.y = d.y = a.y + PSP_SUBPX;
    const int lod16 = *(const int *)opaque;
    reserve_vertices(6);
    push(&a, lod16); push(&b, lod16); push(&c, lod16);
    push(&a, lod16); push(&c, lod16); push(&d, lod16);
}

/* Last Raven's body font: 512x512 CLUT4 atlas, axis-aligned 13-pixel-high
 * glyph triangles with a 1:1 texel mapping. At 1x LINEAR samples texel centres
 * exactly. At higher resolution it interpolates the bitmap's already shaded
 * edges again, reducing stroke contrast. Preserve those texels with NEAREST.
 * Recognize the draw rather than an allocation address (which changes across
 * menus). This policy belongs to the game host, not the PSP sampler. */
static int bitmap_glyph_draw(int prim, const psp_vertex *v, int count) {
    const rendertarget *r = &g.rts[g.cur_rt];
    if (!g.resolution || !r->display || r->sy <= 1.0 || !g.tex_enable || g.tex.fmt != 4 ||
        g.tex.w != 512 || g.tex.h != 512 || g.bound_top || prim != PSP_PRIM_TRIANGLES ||
        count <= 0 || count % 3 || (g.z_test && g.z_func != 1))
        return 0;
    for (int i = 0; i < count; i += 3) {
        int x0 = v[i].x, x1 = x0, y0 = v[i].y, y1 = y0;
        const float du = v[i].u - (float)v[i].x / PSP_SUBPX;
        const float dv = v[i].v - (float)v[i].y / PSP_SUBPX;
        for (int j = i; j < i + 3; j++) {
            if (!v[j].screen_space || fabsf(v[j].tex_q - 1.0f) > 0.0001f ||
                fabsf(v[j].u - (float)v[j].x / PSP_SUBPX - du) > 0.01f ||
                fabsf(v[j].v - (float)v[j].y / PSP_SUBPX - dv) > 0.01f)
                return 0;
            if (v[j].x < x0) x0 = v[j].x;
            if (v[j].x > x1) x1 = v[j].x;
            if (v[j].y < y0) y0 = v[j].y;
            if (v[j].y > y1) y1 = v[j].y;
        }
        if (y1 - y0 != 13 * PSP_SUBPX || x1 - x0 <= 0 || x1 - x0 > 13 * PSP_SUBPX) return 0;
        for (int j = i; j < i + 3; j++)
            if ((v[j].x != x0 && v[j].x != x1) || (v[j].y != y0 && v[j].y != y1)) return 0;
    }
    return 1;
}

static void gl_draw(int prim, const psp_vertex *v, int count) {
    if (claim() != 0) return;
    g.draws++;
    g.verts += (uint64_t)count;
    if (g.bs.stencil_test) {
        const rendertarget *r = &g.rts[g.cur_rt];
        if (g.target_fmt == 3 && (!r->configured || r->fmt == 3)) g.stencil_draws++;
        else g.unsupported_stencil_draws++;
    }
    if ((g.resolution || prim <= PSP_PRIM_LINE_STRIP) && rt_prepare(g.cur_rt) != 0) return;
    const int glyph = bitmap_glyph_draw(prim, v, count);
    if (glyph != g.batch_glyph) { flush(); g.batch_glyph = glyph; }
    if (glyph) g.glyph_draws++;

    /* Adaptive aspect: place this draw. SCENE is anything with projected
     * geometry, and any screen-space draw spanning the full guest width --
     * clears, fades, a movie in strips, a bar -- which must cover the wide
     * target. HUD is the rest of the screen-space geometry, kept 1:1. The
     * class is batch state, so a change flushes like any other state does. */
    if (g.adaptive_aspect) {
        if (rt_prepare(g.cur_rt) != 0) return;
        int cls = CLASS_SCENE;
        if (g.rts[g.cur_rt].wide && count > 0) {
            int ss = 1, min_x = v[0].x, max_x = v[0].x;
            for (int i = 0; i < count; i++) {
                if (!v[i].screen_space) { ss = 0; break; }
                if (v[i].x < min_x) min_x = v[i].x;
                if (v[i].x > max_x) max_x = v[i].x;
            }
            const int full = min_x <= PSP_SUBPX && max_x >= (g.w - 1) * PSP_SUBPX;
            /* A screen-space draw that tests depth reads the scene's depth
             * and sits where the scene put its subject: the lock-on reticle
             * is one, at the target's projected x. It belongs to the scene
             * mapping, or it would miss both the target and the walls. */
            const int reads_depth = g.z_test && g.z_func != 1;
            if (ss && !full && !reads_depth) cls = CLASS_HUD;
            if (ss && !full && reads_depth && getenv("PSPRECOMP_ASPECT_LOG")) {
                static int said;
                if (said++ < 16)
                    fprintf(stderr, "aspect: screen-space draw tests depth (func %d)"
                                    " -> scene: %d verts, x %.1f..%.1f, tex %08X %dx%d,"
                                    " at present %llu\n",
                            g.z_func, count, (float)min_x / PSP_SUBPX,
                            (float)max_x / PSP_SUBPX,
                            g.tex_enable ? g.tex.addr : 0u, g.tex.w, g.tex.h,
                            (unsigned long long)g.presents);
            }
        }
        if (cls != g.batch_class) { flush(); g.batch_class = cls; }
        if (cls == CLASS_HUD) g.class_hud++; else g.class_scene++;
    }

    switch (prim) {
    case PSP_PRIM_POINTS: {
        int lod16 = psp_render_lod16(&g.tex, 1.0f);
        for (int i = 0; i < count; i++) push_point_sample(&v[i], &lod16);
        break;
    }
    case PSP_PRIM_LINES:
    case PSP_PRIM_LINE_STRIP: {
        const rendertarget *r = &g.rts[g.cur_rt];
        const int x0 = g.sc_valid && g.sc_x0 > 0 ? g.sc_x0 : 0;
        const int y0 = g.sc_valid && g.sc_y0 > 0 ? g.sc_y0 : 0;
        const int x1 = g.sc_valid && g.sc_x1 < r->guest_w - 1 ? g.sc_x1 : r->guest_w - 1;
        const int y1 = g.sc_valid && g.sc_y1 < r->guest_h - 1 ? g.sc_y1 : r->guest_h - 1;
        for (int i = 0; i + 1 < count; i += prim == PSP_PRIM_LINES ? 2 : 1) {
            int lod16 = psp_render_line_lod16(&g.tex, &v[i], &v[i + 1]);
            psp_render_walk_line(&v[i], &v[i + 1], x0, y0, x1, y1, push_point_sample, &lod16);
        }
        break;
    }
    case 3:                                        /* triangles */
        for (int i = 0; i + 2 < count; i += 3)
            push_triangle(&v[i], &v[i + 1], &v[i + 2]);
        break;
    case 4:                                        /* triangle strip */
        for (int i = 0; i + 2 < count; i++) {
            /* Winding alternates along a strip; preserve it so a later
             * increment can turn face culling on without the strip flipping. */
            if (i & 1) push_triangle(&v[i + 1], &v[i], &v[i + 2]);
            else       push_triangle(&v[i], &v[i + 1], &v[i + 2]);
        }
        break;
    case 5:                                        /* triangle fan */
        for (int i = 1; i + 1 < count; i++)
            push_triangle(&v[0], &v[i], &v[i + 1]);
        break;
    case 6:                                        /* sprites, in pairs */
        for (int i = 0; i + 1 < count; i += 2) push_sprite(&v[i]);
        break;
    default:
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

static GLenum gl_stencil_op(int op) {
    switch (op) {
    case 1: return GL_ZERO;
    case 2: return GL_REPLACE;
    case 3: return GL_INVERT;
    case 4: return GL_INCR;
    case 5: return GL_DECR;
    default: return GL_KEEP;
    }
}

static int uses_dest_alpha(void) {
    return g.bs.enable && ((g.bs.src >= 4 && g.bs.src <= 5) ||
                           (g.bs.src >= 8 && g.bs.src <= 9) ||
                           (g.bs.dst >= 4 && g.bs.dst <= 5) ||
                           (g.bs.dst >= 8 && g.bs.dst <= 9));
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
    default: *ok = 0; return GL_ONE;
    }
}

/* PSP fixed blend factors are separate RGB constants for the source and
 * destination terms. GL has only one glBlendColor, but the two important
 * endpoints need no constant at all: fixed black is GL_ZERO and fixed white
 * is GL_ONE. That makes the hangar compositor's FIXA=808080/FIXB=000000 pair
 * exactly representable. `uses_constant` tells apply_state whether the one GL
 * constant is still needed for this side. */
static GLenum gl_fixed_factor(uint32_t colour, int *uses_constant) {
    colour &= 0xFFFFFFu;
    if (colour == 0x000000u) return GL_ZERO;
    if (colour == 0xFFFFFFu) return GL_ONE;
    *uses_constant = 1;
    return GL_CONSTANT_COLOR;
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
    if (g.bs.stencil_test && g.rts[g.cur_rt].fmt == 3) {
        p_glEnable(GL_STENCIL_TEST);
        p_glStencilMask(255);
        /* GL compares reference against stored stencil; the GE backend
         * contract compares stored stencil against reference. */
        const int func = g.bs.stencil_func;
        p_glStencilFunc(gl_compare(func >= 4 && func <= 7 ? func ^ 2 : func),
                         g.bs.stencil_ref & 255, (GLuint)g.bs.stencil_mask & 255);
        p_glStencilOp(gl_stencil_op(g.bs.op_sfail), gl_stencil_op(g.bs.op_zfail),
                       gl_stencil_op(g.bs.op_zpass));
    } else {
        p_glDisable(GL_STENCIL_TEST);
        p_glStencilMask(0);
    }
    /* In OpenGL, disabling GL_DEPTH_TEST also disables depth-buffer writes,
     * regardless of glDepthMask.  The GE treats those controls separately:
     * clear-mode draws disable the comparison but still write the clear depth.
     * Keep the GL test enabled with ALWAYS whenever a depth write is requested
     * so those clears actually establish the value later geometry tests. */
    if (g.z_test || g.z_write) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(g.z_test ? gl_compare(g.z_func) : GL_ALWAYS);
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
        const rendertarget *r = &g.rts[g.cur_rt];
        const int hud = r->wide && g.batch_class == CLASS_HUD;
        const double sx = hud ? r->ui_scale : r->sx;
        const double sy = hud ? r->ui_scale : r->sy;
        const double off = hud ? rt_off(r) : 0;
        int x0, x1, y0, y1;
        if (g.resolution) {
            x0 = pixel_edge(g.sc_x0*sx+off);
            x1 = pixel_edge((g.sc_x1+1)*sx+off);
            y0 = pixel_edge(g.sc_y0*sy);
            y1 = pixel_edge((g.sc_y1+1)*sy);
        } else {
            x0 = (int)floor(g.sc_x0*sx+off);
            x1 = (int)ceil((g.sc_x1+1)*sx+off);
            y0 = g.sc_y0; y1 = g.sc_y1+1;
        }
        p_glScissor(x0, r->h-y1, x1>x0?x1-x0:0, y1>y0?y1-y0:0);
    } else {
        p_glDisable(GL_SCISSOR_TEST);
    }

    if (g.bs.enable) {
        int ok = 1;
        int src_constant = 0, dst_constant = 0;
        GLenum src = g.bs.src == 10
                   ? gl_fixed_factor(g.bs.fixa, &src_constant)
                   : gl_factor(g.bs.src, 1, &ok);
        const GLenum dst = g.bs.dst == 10
                         ? gl_fixed_factor(g.bs.fixb, &dst_constant)
                         : gl_factor(g.bs.dst, 0, &ok);
        if (!ok) g.unsupported_blend_factor++;
        if (src_constant && dst_constant &&
            (g.bs.fixa & 0xFFFFFFu) != (g.bs.fixb & 0xFFFFFFu))
            g.unsupported_blend_factor++;
        int eq_ok = 1;
        const GLenum eq = gl_equation(g.bs.eq, &eq_ok);
        if (!eq_ok) g.unsupported_blend_eq++;
        /* This rewrite changes fragment alpha to carry an exact destination
         * factor, so only use it when alpha is the framebuffer's masked-off
         * stencil byte. MIN/MAX ignore factors; abs-difference is not
         * represented by this fixed-function path. */
        const int preblend_src = g.bs.src == 2 && g.bs.eq <= 2 &&
                                 !g.bs.write_alpha;
        if (preblend_src) src = GL_ONE;
        p_glEnable(GL_BLEND);
        p_glBlendFunc(src, dst);
        p_glBlendEquation(eq);
        /* If both sides need a non-trivial, unequal fixed colour the counter
         * above records the case GL's fixed pipeline cannot represent. Equal
         * constants, or one non-trivial constant paired with zero/one, are
         * exact. */
        const uint32_t fx = src_constant ? g.bs.fixa : g.bs.fixb;
        p_glBlendColor((float)( fx        & 0xFF) / 255.0f,
                       (float)((fx >>  8) & 0xFF) / 255.0f,
                       (float)((fx >> 16) & 0xFF) / 255.0f,
                       (float)((fx >> 24) & 0xFF) / 255.0f);
        p_glUniform1i(g.u_preblend_src,
                      preblend_src ? (g.bs.dst == 3 ? 2 : 1) : 0);
    } else {
        p_glDisable(GL_BLEND);
        p_glUniform1i(g.u_preblend_src, 0);
    }

    p_glUniform1i(g.u_texenable, g.tex_enable ? 1 : 0);
    p_glUniform1i(g.u_texfunc, g.tex.func);
    p_glUniform1i(g.u_tcc, g.tex.tcc_rgba ? 1 : 0);
    p_glUniform1i(g.u_double, g.tex.color_double ? 1 : 0);
    p_glUniform3f(g.u_env, (float)( g.tex.env        & 0xFF) / 255.0f,
                           (float)((g.tex.env >>  8) & 0xFF) / 255.0f,
                           (float)((g.tex.env >> 16) & 0xFF) / 255.0f);
    p_glUniform1i(g.u_tex, 0);
    p_glUniform2f(g.u_texscale, g.tex_sx, g.tex_sy);
    p_glUniform1i(g.u_minfilter, g.batch_glyph ? g.tex.min_filter & ~1 : g.tex.min_filter);
    p_glUniform1i(g.u_magfilter, g.batch_glyph ? g.tex.mag_filter & ~1 : g.tex.mag_filter);
    p_glUniform1i(g.u_wraps, g.tex.wrap_s ? 1 : 0);
    p_glUniform1i(g.u_wrapt, g.tex.wrap_t ? 1 : 0);
    p_glUniform1i(g.u_miptop, g.bound_top);
    if (g.bound) p_glBindTexture(GL_TEXTURE_2D, g.bound);

    p_glUniform1i(g.u_fogenable, g.fog_enable ? 1 : 0);
    p_glUniform3f(g.u_fogcolour,
                  (float)( g.fog_colour        & 0xFF) / 255.0f,
                  (float)((g.fog_colour >>  8) & 0xFF) / 255.0f,
                  (float)((g.fog_colour >> 16) & 0xFF) / 255.0f);

    p_glUniform1i(g.u_atest, g.bs.alpha_test ? g.bs.alpha_func : 1);
    p_glUniform1i(g.u_aref,  g.bs.alpha_ref);
    p_glUniform1i(g.u_amask, g.bs.alpha_mask);
}

static void apply_placement(const rendertarget *r) {
    const int hud = r->wide && g.batch_class == CLASS_HUD;
    p_glUseProgram(g.prog);
    if (g.resolution) {
        p_glViewport(0, 0, r->w, r->h);
        p_glUniform2f(g.u_viewport, (float)r->w, (float)r->h);
        p_glUniform3f(g.u_placement, hud?r->ui_scale:r->sx,
                      hud?r->ui_scale:r->sy, hud?rt_off(r):0);
    } else {
        p_glViewport(hud?(int)rt_off(r):0, 0, hud?r->guest_w:rt_scene_w(r), r->h);
        p_glUniform2f(g.u_viewport, (float)r->guest_w, (float)r->guest_h);
        p_glUniform3f(g.u_placement, 1, 1, 0);
    }
    p_glUniform1f(g.u_ybias, 1.0f/256.0f);
}

static void flush(void) {
    if (!g.ready || g.batch_n == 0) return;
    if (rt_prepare(g.cur_rt) != 0) { g.batch_n = 0; return; }
    rendertarget *r = &g.rts[g.cur_rt];
    gpu_query_begin_frame();
    rt_import(r);
    /* Clear-mode alpha writes invalidate the hardware stencil. Synchronise
     * pending stencil first so a scissored alpha clear preserves the rest. */
    if (g.bs.write_alpha || uses_dest_alpha()) stencil_to_alpha(r);
    if (g.bs.stencil_test) alpha_to_stencil(r);
    p_glBindFramebuffer(GL_FRAMEBUFFER, r->fbo);
    r->dirty = 1;
    /* Retain the aspect diagnostics; apply_placement sets the final viewport
     * and, in window-resolution mode, the physical coordinate transform. */
    if (r->wide && g.batch_class == CLASS_HUD) {
        p_glViewport(rt_off(r), 0, r->guest_w, r->h);
        g.hud_flushes++;
        /* A HUD batch reading depth, stencil or destination alpha would meet
         * scene pixels placed by the other mapping. Counted, not handled:
         * this game's HUD is expected never to do it, and the report says. */
        if (g.z_test) {
            g.hud_depth_func[g.z_func & 7]++;
            if (g.z_func != 1) g.hud_hazard_depth++;   /* ALWAYS reads nothing */
        }
        if (getenv("PSPRECOMP_ASPECT_LOG")) {
            const uint32_t key = g.tex_enable ? g.tex.addr : 0u;
            int k = 0;
            while (k < g.hud_by_tex_n && g.hud_by_tex[k].addr != key) k++;
            if (k == g.hud_by_tex_n && k < 48) {
                g.hud_by_tex_n++;
                g.hud_by_tex[k].addr = key; g.hud_by_tex[k].w = g.tex.w; g.hud_by_tex[k].h = g.tex.h;
                g.hud_by_tex[k].z0 = 1e9f; g.hud_by_tex[k].z1 = -1e9f;
            }
            if (k < 48) {
                g.hud_by_tex[k].batches++;
                if (g.z_test && g.z_func != 1) g.hud_by_tex[k].tested++;
                for (size_t i = 0; i < g.batch_n; i++) {
                    const float z = g.batch[i * FLOATS_PER_VERT + 2];
                    if (z < g.hud_by_tex[k].z0) g.hud_by_tex[k].z0 = z;
                    if (z > g.hud_by_tex[k].z1) g.hud_by_tex[k].z1 = z;
                }
            }
        }
        if (g.z_write) g.hud_depth_writes++;
        if (g.bs.stencil_test) g.hud_hazard_stencil++;
        if (uses_dest_alpha()) g.hud_hazard_dst_alpha++;
    } else {
        p_glViewport(0, 0, rt_scene_w(r), r->h);
    }
    p_glUseProgram(g.prog);
    apply_placement(r);
    apply_state();
    p_glBindVertexArray(g.vao);
    p_glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    p_glBufferSubData(GL_ARRAY_BUFFER, 0,
                      (GLsizeiptr)(g.batch_n * FLOATS_PER_VERT * sizeof(float)),
                      g.batch);
    const int stencil_writes = g.bs.stencil_test && r->fmt == 3 &&
                              (g.bs.op_sfail || g.bs.op_zfail || g.bs.op_zpass);
    if (stencil_writes && uses_dest_alpha()) {
        /* Later overlapping primitives must blend against the updated alpha,
         * not the value at the start of the batch. Only this dependency needs
         * a synchronisation per triangle; ordinary stencil batches stay batched. */
        for (size_t i = 0; i < g.batch_n; i += 3) {
            if (i) {
                stencil_to_alpha(r);
                apply_placement(r);
                apply_state();
            }
            p_glDrawArrays(GL_TRIANGLES, (GLint)i, 3);
            r->alpha_dirty = 1;
        }
    } else {
        p_glDrawArrays(GL_TRIANGLES, 0, (GLsizei)g.batch_n);
        if (stencil_writes) r->alpha_dirty = 1;
    }
    if (g.bs.write_alpha) { r->stencil_valid = 0; r->alpha_dirty = 0; }
    g.batch_n = 0;
}

static void gl_finish(void) {
    if (claim() != 0) return;
    flush();
}

/* The guest-sized staging target a wide attachment is resolved into before a
 * readback. One, resized when the guest extent changes. */
static int stage_prepare(int w, int h) {
    if (g.stage_fbo && g.stage_w == w && g.stage_h == h) return 0;
    if (!g.stage_fbo) {
        p_glGenFramebuffers(1, &g.stage_fbo);
        p_glGenTextures(1, &g.stage_tex);
    }
    p_glBindTexture(GL_TEXTURE_2D, g.stage_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                   GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g.stage_fbo);
    p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, g.stage_tex, 0);
    /* The sampler binding was borrowed; the next flush rebinds g.bound too. */
    if (g.bound) p_glBindTexture(GL_TEXTURE_2D, g.bound);
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gl: the aspect staging target is incomplete\n");
        return -1;
    }
    g.stage_w = w; g.stage_h = h;
    return 0;
}

/* Read one colour attachment back into the guest's framebuffer.
 *
 * This is what keeps the rest of the project working. score_frame,
 * dump_frame_seq, dump_framebuffer, survey_vram and the frame comparison that
 * passed the M2 gate all read guest memory, and render-target aliases need the
 * actual PSP bytes before the shared texture decoder can reinterpret them.
 *
 * One glReadPixels for the whole surface matters: the old row-at-a-time path
 * forced 272 GPU/CPU synchronisation points at every flip, which made complex
 * mission frames disproportionately slow.  Rows are flipped and packed on the
 * CPU after that single transfer. */
static void readback_rt(int i) {
    rendertarget *r = &g.rts[i];
    if (!r->configured || !r->addr || !r->stride) return;
    rt_import(r);
    const int bpp = r->fmt == 3 ? 4 : 2;
    const size_t bytes = (size_t)r->stride * (size_t)r->guest_h * (size_t)bpp;
    void *dst = psp_mem_ptr(r->addr, bytes);
    if (!dst) return;

    static uint8_t *pixels;
    static size_t capacity;
    const int rw = r->guest_w;
    const int rh = r->guest_h;
    const size_t need = (size_t)rw * (size_t)r->guest_h * 4u;
    if (need > capacity) {
        uint8_t *larger = realloc(pixels, need);
        if (!larger) return;
        pixels = larger;
        capacity = need;
    }

    const uint64_t readback_t0 = psp_os_mono_ns();
    stencil_to_alpha(r);
    if (r->w != rw || r->h != rh) {
        /* Guest memory keeps its 480x272 picture: the wide attachment is
         * resolved to guest width first. Nearest, because the alpha byte is
         * the stencil and the bytes are re-read as palette indices; and the
         * HUD comes out narrower here, which is the guest's view of it. */
        if (stage_prepare(rw, rh) != 0) return;
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.stage_fbo);
        p_glDisable(GL_SCISSOR_TEST);
        p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        p_glBlitFramebuffer(0, 0, rt_scene_w(r), r->h, 0, 0, rw, rh,
                            GL_COLOR_BUFFER_BIT, GL_NEAREST);
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, g.stage_fbo);
    } else {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
    }
    p_glReadPixels(0, 0, rw, rh, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

    uint8_t *out = (uint8_t *)dst;
    const int copy_w = rw < (int)r->stride ? rw : (int)r->stride;
    for (int y = 0; y < rh; y++) {
        const uint8_t *row = pixels + (size_t)(rh - 1 - y) * (size_t)rw * 4u;
        if (r->fmt == 3) {
            memcpy(out + (size_t)y * r->stride * 4u, row, (size_t)copy_w * 4u);
            continue;
        }
        uint16_t *row16 = (uint16_t *)(out + (size_t)y * r->stride * 2u);
        for (int x = 0; x < copy_w; x++) {
            const uint32_t red   = row[x * 4 + 0];
            const uint32_t green = row[x * 4 + 1];
            const uint32_t blue  = row[x * 4 + 2];
            const uint32_t alpha = row[x * 4 + 3];
            if (r->fmt == 0)
                row16[x] = (uint16_t)((red >> 3) | ((green >> 2) << 5) |
                                      ((blue >> 3) << 11));
            else if (r->fmt == 1)
                row16[x] = (uint16_t)((red >> 3) | ((green >> 3) << 5) |
                                      ((blue >> 3) << 10) | ((alpha >> 7) << 15));
            else
                row16[x] = (uint16_t)((red >> 4) | ((green >> 4) << 4) |
                                      ((blue >> 4) << 8) | ((alpha >> 4) << 12));
        }
    }
    /* The raw pointer deliberately avoids one mark per output pixel. One range
     * mark after conversion gives every texture overlapping this target the
     * same precise invalidation signal. */
    g.exporting = i+1;
    psp_mem_mark_write(r->addr, (uint32_t)bytes);
    g.exporting = 0;
    g.readbacks++;
    g.readback_ns += psp_os_mono_ns() - readback_t0;
}

static void note_frame_time(void) {
    const uint64_t now = psp_os_mono_ns();
    if (!g.frames) {
        g.frame_first_ns = now;
    } else {
        const uint64_t gap = now - g.frame_prev_ns;
        uint64_t ms = gap / UINT64_C(1000000);
        if (ms > 255) ms = 255;
        g.frame_ms[ms]++;
        if (gap > g.frame_max_ns) g.frame_max_ns = gap;
    }
    g.frame_prev_ns = g.frame_last_ns = now;
    g.frames++;
}

/* PSPRECOMP_GL_SHOT=<prefix> writes every Nth presented *window* image as
 * <prefix>-NNNN.ppm, N from PSPRECOMP_GL_SHOT_EVERY (default 30). The frame
 * dumps in boot.c and display.c read guest memory, which in this backend is
 * the readback -- never the drawable. This is the only view of what the
 * window actually shows, and it is what a text-sharpness claim rests on. */
static void gl_shot(int draw_w, int draw_h) {
    static const char *prefix;
    static int every = -1;
    static unsigned n;
    if (every < 0) {
        prefix = getenv("PSPRECOMP_GL_SHOT");
        if (prefix && !*prefix) prefix = NULL;
        const char *e = getenv("PSPRECOMP_GL_SHOT_EVERY");
        every = e && *e ? atoi(e) : 30;
        if (every < 1) every = 1;
    }
    if (!prefix || draw_w <= 0 || draw_h <= 0) return;
    const unsigned idx = n++;
    if (idx % (unsigned)every) return;
    uint8_t *px = malloc((size_t)draw_w * (size_t)draw_h * 4u);
    uint8_t *row = malloc((size_t)draw_w * 3u);
    if (px && row) {
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        p_glReadPixels(0, 0, draw_w, draw_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        char path[512];
        snprintf(path, sizeof path, "%s-%04u.ppm", prefix, idx / (unsigned)every);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", draw_w, draw_h);
            for (int y = draw_h - 1; y >= 0; y--) {
                const uint8_t *src = px + (size_t)y * (size_t)draw_w * 4u;
                for (int x = 0; x < draw_w; x++) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                }
                fwrite(row, 1, (size_t)draw_w * 3u, f);
            }
            fclose(f);
        }
    }
    free(px);
    free(row);
}

static void gl_present(void) {
    if (claim() != 0) return;
    g.presents++;
    flush();
    /* Count deferred alpha/stencil transfers inside the GPU frame timer. */
    for (int i = 0; i < g.n_rts; i++) stencil_to_alpha(&g.rts[i]);

    /* The current target, scaled into the window's physical GL drawable. SDL
     * window sizes are logical pixels on a high-DPI desktop; blitting to the
     * fixed 960x544 logical size therefore occupied only the lower-left
     * quarter of a 1920x1088 drawable. Preserve aspect ratio for arbitrary
     * resizes and clear the letterbox before the blit. */
    int draw_w = 0, draw_h = 0;
    present_gl_drawable_size(&draw_w, &draw_h);
    if (draw_w <= 0) draw_w = g.w * 2;
    if (draw_h <= 0) draw_h = g.h * 2;
    if (rt_prepare(g.cur_rt) != 0) {
        gpu_query_end_frame();
        gpu_query_poll();
        return;
    }
    rendertarget *shown = &g.rts[g.cur_rt];
    rt_import(shown);
    /* The picture is 480 guest pixels wide, or the virtual width they were
     * spread over. The fit is uniform either way: a wide target has the
     * window's shape by construction, so its letterbox is only the rounding. */
    const int pic_w = shown->wide ? shown->wide_w : g.w;
    const int src_w = shown->display ? shown->visible_w : (shown->w < g.w ? shown->w : g.w);
    const int src_h = shown->display ? shown->visible_h : (shown->h < g.h ? shown->h : g.h);
    int out_w = draw_w;
    int out_h = (int)((long long)draw_w * (g.resolution?src_h:g.h) /
                     (g.resolution?src_w:pic_w));
    if (out_h > draw_h) {
        out_h = draw_h;
        out_w = (int)((long long)draw_h * (g.resolution?src_w:pic_w) /
                      (g.resolution?src_h:g.h));
    }
    const int out_x = (draw_w - out_w) / 2;
    const int out_y = (draw_h - out_h) / 2;
    p_glBindFramebuffer(GL_READ_FRAMEBUFFER, shown->fbo);
    p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    p_glDisable(GL_SCISSOR_TEST);
    p_glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    p_glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    p_glClear(GL_COLOR_BUFFER_BIT);
    p_glBlitFramebuffer(0, shown->h-src_h, src_w, shown->h,
                        out_x, out_y, out_x + out_w, out_y + out_h,
                        GL_COLOR_BUFFER_BIT, GL_LINEAR);
    gl_shot(draw_w, draw_h);
    gpu_query_end_frame();
    present_gl_swap();

    /* Read every target that has been drawn into since the last flip. The
     * instruments in display.c and boot.c all read guest memory, and which
     * buffer they read is not this backend's to know -- so all of them are
     * made true rather than guessing at one. */
    int rendered = 0;
    for (int i = 0; i < g.n_rts; i++) {
        if (!g.rts[i].dirty && !g.rts[i].cpu_pending) continue;
        readback_rt(i);
        g.rts[i].dirty = 0;
        rendered = 1;
    }
    gpu_query_poll();

    resolution_size();
    rt_resize_all();
    if (g.rts[g.cur_rt].configured)
        p_glBindFramebuffer(GL_FRAMEBUFFER, g.rts[g.cur_rt].fbo);
    /* This game calls sceDisplaySetFrameBuf twice per rendered frame. Counting
     * both callbacks produced a fictitious 63 fps with alternating 12/20 ms
     * intervals; a dirty target is the evidence that new work was presented. */
    if (rendered) note_frame_time();
}

unsigned char *render_gl_capture(uint32_t addr, int *w, int *h) {
    if (claim() != 0) return NULL;
    flush();
    for (int i = 0; i < g.n_rts; i++) {
        rendertarget *r = &g.rts[i];
        if (!r->configured || r->addr != (addr & PSP_ADDR_MASK)) continue;
        rt_import(r);
        stencil_to_alpha(r);
        uint8_t *out = malloc((size_t)r->w * r->h * 4), *row = malloc((size_t)r->w * 4);
        if (!out || !row) {
            free(out);
            free(row);
            return NULL;
        }
        p_glBindFramebuffer(GL_READ_FRAMEBUFFER, r->fbo);
        p_glReadPixels(0, 0, r->w, r->h, GL_RGBA, GL_UNSIGNED_BYTE, out);
        for (int y = 0; y < r->h / 2; y++) {
            uint8_t *a = out + (size_t)y * r->w * 4, *b = out + (size_t)(r->h - 1 - y) * r->w * 4;
            memcpy(row, a, (size_t)r->w * 4);
            memcpy(a, b, (size_t)r->w * 4);
            memcpy(b, row, (size_t)r->w * 4);
        }
        free(row);
        *w = r->w;
        *h = r->h;
        return out;
    }
    return NULL;
}

static const psp_render_backend gl_backend = {
    .name = "gl",
    .init = gl_init,
    .shutdown = gl_shutdown,
    .set_target = gl_target,
    .set_scissor = gl_scissor,
    .set_texture = gl_texture,
    .set_clut = gl_clut,
    .set_depth = gl_depth,
    .set_blend = gl_blend,
    .set_fog = gl_fog,
    .draw = gl_draw,
    .finish = gl_finish,
    .present = gl_present,
};

const psp_render_backend *render_gl_backend(void) { return &gl_backend; }

void render_gl_report(FILE *out) {
    if (!g.ready && !g.failed) return;
    fprintf(out, "gl:       %s", g.failed ? "failed to start" : "ran");
    if (g.ready)
        fprintf(out, " -- %llu draw(s), %llu vertices, %llu readback(s)",
                (unsigned long long)g.draws, (unsigned long long)g.verts,
                (unsigned long long)g.readbacks);
    fprintf(out, "\n          targets: %d%s", g.n_rts,
            g.rt_overflow ? " (more than the table holds)" : "");
    for (int i = 0; i < g.n_rts; i++)
        if (g.rts[i].configured)
            fprintf(out, " %08X(%dx%d,s%u,f%d)", g.rts[i].addr,
                    g.rts[i].w, g.rts[i].h, g.rts[i].stride, g.rts[i].fmt);
        else
            fprintf(out, " %08X(unused)", g.rts[i].addr);
    if (g.resolution)
        fprintf(out, "\n          resolution: window %dx%d, %llu resize(s), %llu CPU upload(s), %llu GPU texture view(s), %llu bitmap font draw(s)",
                g.pixel_w, g.pixel_h, (unsigned long long)g.resizes,
                (unsigned long long)g.cpu_uploads, (unsigned long long)g.rt_views,
                (unsigned long long)g.glyph_draws);
    if (g.adaptive_aspect)
        fprintf(out, "\n          aspect: virtual width %d (x%.4f), %llu scene draw(s),"
                     " %llu HUD draw(s) in %llu batch(es), HUD hazards depth %llu"
                     " stencil %llu dst-alpha %llu, %llu wide allocation(s), %llu retired",
                g.wide_w, (double)g.wide_w / (double)g.w,
                (unsigned long long)g.class_scene, (unsigned long long)g.class_hud,
                (unsigned long long)g.hud_flushes,
                (unsigned long long)g.hud_hazard_depth,
                (unsigned long long)g.hud_hazard_stencil,
                (unsigned long long)g.hud_hazard_dst_alpha,
                (unsigned long long)g.wide_allocs, (unsigned long long)g.wide_retired);
    for (int k = 0; k < g.hud_by_tex_n; k++)
        fprintf(out, "\n          HUD tex %08X %dx%d: %llu batch(es), %llu depth-tested, z %.4f..%.4f",
                g.hud_by_tex[k].addr, g.hud_by_tex[k].w, g.hud_by_tex[k].h,
                (unsigned long long)g.hud_by_tex[k].batches,
                (unsigned long long)g.hud_by_tex[k].tested,
                g.hud_by_tex[k].z0, g.hud_by_tex[k].z1);
    if (g.adaptive_aspect && (g.hud_hazard_depth || g.hud_depth_writes)) {
        fprintf(out, "\n          HUD depth: %llu write(s); tests by func",
                (unsigned long long)g.hud_depth_writes);
        for (int f = 0; f < 8; f++)
            if (g.hud_depth_func[f])
                fprintf(out, " %d:%llu", f, (unsigned long long)g.hud_depth_func[f]);
    }
    fprintf(out, "\n          textures: %llu request(s), %llu upload(s), %llu hit(s)"
                 " (%llu immediate, %llu revalidated), %llu dirty invalidation(s),"
                 " %llu miss(es), %llu eviction(s), %d/%d resident, %llu too big, %llu from VRAM,"
                 " %llu sampled from a target",
            (unsigned long long)g.tex_requests,
            (unsigned long long)g.tex_uploads, (unsigned long long)g.tex_hits,
            (unsigned long long)g.tex_fast_hits,
            (unsigned long long)g.tex_revalidated,
            (unsigned long long)g.tex_invalidations,
            (unsigned long long)g.tex_misses,
            (unsigned long long)g.tex_evictions, g.cache_entries, TEXCACHE_MAX,
            (unsigned long long)g.tex_too_big,
            (unsigned long long)g.tex_vram_uploads,
            (unsigned long long)g.tex_from_rt);
    if (g.tex_alias_from_rt)
        fprintf(out, ", %llu target alias decode(s)",
                (unsigned long long)g.tex_alias_from_rt);
    if (g.mip_chains || g.mip_incomplete) {
        fprintf(out, ", mipmaps: %llu chain upload(s), %llu extra level(s)",
                (unsigned long long)g.mip_chains,
                (unsigned long long)g.mip_levels);
        if (g.mip_incomplete)
            fprintf(out, ", %llu incomplete",
                    (unsigned long long)g.mip_incomplete);
    }
    fprintf(out, "\n          timing: texture bind %.3f s (generation %.3f, decode %.3f,"
                 " upload %.3f; %.1f MiB RGBA), readback %.3f s",
            g.tex_bind_ns / 1.0e9, g.tex_generation_ns / 1.0e9,
            g.tex_decode_ns / 1.0e9, g.tex_upload_ns / 1.0e9,
            g.tex_upload_pixels * 4.0 / (1024.0 * 1024.0),
            g.readback_ns / 1.0e9);
    if (g.gpu_samples) {
        const uint64_t p50_at = (g.gpu_samples + 1) / 2;
        const uint64_t p95_at = (g.gpu_samples * 95 + 99) / 100;
        uint64_t seen = 0;
        int p50 = -1, p95 = -1;
        for (int tenth = 0; tenth < 256; tenth++) {
            seen += g.gpu_tenth_ms[tenth];
            if (p50 < 0 && seen >= p50_at) p50 = tenth;
            if (seen >= p95_at) { p95 = tenth; break; }
        }
        fprintf(out, "\n          gpu: %llu draw+blit sample(s), mean %.2f ms, "
                     "p50 %.1f ms, p95 %.1f ms, max %.2f ms",
                (unsigned long long)g.gpu_samples,
                g.gpu_total_ns / (double)g.gpu_samples / 1.0e6,
                p50 / 10.0, p95 / 10.0, g.gpu_max_ns / 1.0e6);
        if (g.gpu_dropped)
            fprintf(out, ", %llu frame(s) unmeasured because the query ring "
                         "was full",
                    (unsigned long long)g.gpu_dropped);
    }
    if (g.frames > 1 && g.frame_last_ns > g.frame_first_ns) {
        const uint64_t intervals = g.frames - 1;
        const uint64_t p50_at = (intervals + 1) / 2;
        const uint64_t p95_at = (intervals * 95 + 99) / 100;
        uint64_t seen = 0;
        int p50 = -1, p95 = -1;
        for (int ms = 0; ms < 256; ms++) {
            seen += g.frame_ms[ms];
            if (p50 < 0 && seen >= p50_at) p50 = ms;
            if (seen >= p95_at) { p95 = ms; break; }
        }
        const double seconds = (g.frame_last_ns - g.frame_first_ns) / 1.0e9;
        fprintf(out, "\n          frames: %llu rendered / %llu present call(s) over %.3f s"
                     " (%.2f/s), interval p50 %d ms, p95 %d ms, max %.1f ms",
                (unsigned long long)g.frames, (unsigned long long)g.presents, seconds,
                intervals / seconds, p50, p95, g.frame_max_ns / 1.0e6);
    }
    if (g.batch_overflows)
        fprintf(out, ", %llu batch flush(es) from overflow",
                (unsigned long long)g.batch_overflows);
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
    if (g.unsupported_stencil_draws)
        fprintf(out, ", %llu draw(s) need alpha-backed stencil",
                (unsigned long long)g.unsupported_stencil_draws);
    if (g.stencil_draws)
        fprintf(out, "\n          stencil: %llu RGBA8888 draw(s), %llu alpha import(s), %llu export(s)",
                (unsigned long long)g.stencil_draws,
                (unsigned long long)g.stencil_imports, (unsigned long long)g.stencil_exports);
    fprintf(out, "\n");
}

#else   /* no SDL2: there is no window to put a context on */

const psp_render_backend *render_gl_backend(void) { return NULL; }
void render_gl_report(FILE *out) { (void)out; }
unsigned char *render_gl_capture(uint32_t addr, int *w, int *h) {
    (void)addr; (void)w; (void)h; return NULL;
}

#endif
