/* Physical-output regressions. Guest readback cannot prove enhanced detail. */
#include "present.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "render_gl.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { W = 480, H = 272, STRIDE = 512 };
#define A 0x04000000u
#define B 0x04088000u
#define SCRATCH 0x04154000u
/* present.c offers the adaptive aspect only to a title with a camera
 * replacement; the fixture stands in for host/replacements.c here, or its
 * window-aspect configurations would silently run the original view. */
const int lr_adaptive_aspect_available = 1;
static const psp_render_backend *be;
static unsigned checks, failures;
static void check(int yes, const char *why) {
    checks++;
    if (!yes) {
        fprintf(stderr, "FAIL %s\n", why);
        failures++;
    }
}
static void target(uint32_t addr) {
    be->set_target(addr, STRIDE, 3);
    be->set_scissor(0, 0, 479, 271);
}
static void untextured(void) {
    psp_tex_state t = {0};
    be->set_texture(&t);
}
static void blend(int alpha) {
    psp_blend_state bs = {.write_colour = 1, .write_alpha = alpha};
    be->set_blend(&bs);
}
static void rect(float x0, float y0, float x1, float y1, uint32_t color, float z, int precise) {
    psp_vertex v[2] = {{.x = (int)lroundf(x0 * 16),
                        .y = (int)lroundf(y0 * 16),
                        .z = z,
                        .rgba = color,
                        .inv_w = 1,
                        .tex_q = 1,
                        .fog = 255,
                        .precise = precise,
                        .precise_x = x0,
                        .precise_y = y0},
                       {.x = (int)lroundf(x1 * 16),
                        .y = (int)lroundf(y1 * 16),
                        .z = z,
                        .rgba = color,
                        .inv_w = 1,
                        .tex_q = 1,
                        .fog = 255,
                        .precise = precise,
                        .precise_x = x1,
                        .precise_y = y1}};
    v[1].u = x1 - x0;
    v[1].v = y1 - y0;
    be->draw(PSP_PRIM_SPRITES, v, 2);
}
static uint32_t pixel(const unsigned char *image, int w, int x, int y) {
    uint32_t value;
    memcpy(&value, image + ((size_t)y * w + x) * 4, 4);
    return value;
}
static void expected_pixel(uint32_t addr, int x, int y, uint32_t want, const char *why) {
    int w = 0, h = 0;
    unsigned char *p = render_gl_capture(addr, &w, &h);
    uint32_t got = p && x >= 0 && y >= 0 && x < w && y < h ? pixel(p, w, x, y) : 0;
    if (got != want)
        fprintf(stderr, "%s: pixel %d,%d got %08X want %08X (%dx%d)\n", why, x, y, got, want, w, h);
    check(p && got == want, why);
    free(p);
}

static void detail_and_views(void) {
    target(A);
    untextured();
    blend(1);
    /* Prime colour, alpha and depth history: an enabled test that always
     * passes. A disabled test writes no depth. */
    be->set_depth(1, 1, 1);
    rect(0, 0, 512, 272, 0x6D332211, 10000, 0);
    /* A quarter PSP-pixel stripe misses all 1x sample centres, but contains
     * a 2x sample. The second stripe's float edges round to the same 12.4
     * coordinates around a tie; only retaining the floats covers x=80. */
    rect(20.1875f, 4, 20.4375f, 30, 0x6D00FF00, 10000, 0);
    rect(40.24f, 4, 40.26f, 30, 0x6DFF0000, 10000, 1);
    int w, h;
    unsigned char *p = render_gl_capture(A, &w, &h);
    check(p && w == 1024 && h == 544, "physical attachment includes scaled stride padding");
    free(p);
    expected_pixel(A, 40, 20, 0x6D00FF00, "detail finer than a PSP pixel is rasterized");
    expected_pixel(A, 80, 20, 0x6DFF0000, "pre-quantization positions reach rasterization");
    target(B);
    untextured();
    blend(1);
    be->set_depth(0, 1, 0);
    rect(0, 0, 512, 272, 0x6D000000, 0, 0);
    /* The declared height exceeds the 272 rows the target actually owns. */
    psp_tex_state t = {
        .addr = A, .stride = 512, .w = 512, .h = 512, .fmt = 3, .func = 3, .tcc_rgba = 1};
    be->set_texture(&t);
    blend(0);
    rect(0, 0, 480, 272, 0xFFFFFFFF, 0, 0);
    expected_pixel(B, 40, 20, 0x6D00FF00, "512-row texture view preserves detail and orientation");
    expected_pixel(B, 80, 20, 0x6DFF0000, "compositing preserves float-position detail");
    t.addr = B;
    be->set_texture(&t);
    rect(0, 0, 480, 272, 0xFFFFFFFF, 0, 0);
    expected_pixel(B, 40, 20, 0x6D00FF00, "self-composite samples a separate snapshot");
    /* Sampling a row-offset view must not use stale decoded guest memory. */
    t.addr = A + 512 * 4 * 4;
    t.h = 256;
    be->set_texture(&t);
    rect(0, 40, 480, 80, 0xFFFFFFFF, 0, 0);
    expected_pixel(B, 40, 80, 0x6D00FF00, "row-offset target texture view");
}

static void cpu_writes_and_layout(void) {
    target(A);
    untextured();
    blend(1);
    be->set_depth(0, 1, 0);
    psp_write8(A + (10 * 512 + 20) * 4 + 3, 0x99);
    expected_pixel(A, 40, 20, 0x9900FF00, "CPU alpha write keeps fine RGB detail inside a guest pixel");
    expected_pixel(A, 41, 20, 0x99332211, "CPU alpha write keeps the other physical pixel distinct");
    rect(10, 40, 12, 42, 0x98765432, 0, 0);
    be->finish();
    psp_write8(A + (40 * 512 + 10) * 4, 0xAB);
    expected_pixel(A, 20, 80, 0x987654AB, "partial CPU byte write preserves GPU-owned channels");
    expected_pixel(A, 22, 80, 0x98765432, "CPU write preserves adjacent high-resolution pixels");
    /* No byte-difference heuristic: this equals the old guest value. */
    const uint32_t old = psp_read32(A + (40 * 512 + 11) * 4);
    psp_write32(A + (40 * 512 + 11) * 4, old);
    expected_pixel(A, 22, 80, old, "same-value CPU store replaces newer GPU data");
    be->present();
    check(psp_read32(A + (40 * 512 + 10) * 4) == 0x987654AB, "guest resolve packs imported bytes");
    /* Changing the scratch format must allocate from the new guest layout. */
    be->set_target(SCRATCH, 256, 1);
    be->set_scissor(0, 0, 255, 127);
    blend(1);
    rect(0, 0, 256, 128, 0xFF332211, 0, 0);
    be->finish();
    be->set_target(SCRATCH, 128, 3);
    be->set_scissor(0, 0, 127, 63);
    blend(1);
    rect(0, 0, 128, 64, 0x7F665544, 0, 0);
    int w, h;
    unsigned char *p = render_gl_capture(SCRATCH, &w, &h);
    check(p && w == 128 && h == 64, "scratch format reconfiguration stays at 1x");
    free(p);
    expected_pixel(SCRATCH, 3, 3, 0x7F665544, "scratch format changes update color and alpha");
}

static void font_quad(int x, int y, int h, int screen_space) {
    psp_vertex v[6] = {0};
    const int xy[][2] = {{0, 0}, {5, 0}, {0, h}, {0, h}, {5, 0}, {5, h}};
    for (int i = 0; i < 6; i++) {
        v[i].x = (x + xy[i][0]) * 16;
        v[i].y = (y + xy[i][1]) * 16;
        v[i].u = xy[i][0];
        v[i].v = xy[i][1];
        v[i].inv_w = v[i].tex_q = 1;
        v[i].rgba = 0xFFFFFFFF;
        v[i].fog = 255;
        v[i].screen_space = screen_space;
    }
    be->draw(PSP_PRIM_TRIANGLES, v, 6);
}

static void bitmap_font(void) {
    const uint32_t atlas = 0x08040000, palette = 0x08080000;
    /* A one-texel white stem against transparent black, the edge which loses
     * weight when both colour and alpha are interpolated on magnification. */
    for (int y = 0; y < 512; y++) psp_write8(atlas + y * 256, 0x10);
    psp_write32(palette, 0);
    psp_write32(palette + 4, 0xFFFFFFFF);
    target(A);
    untextured();
    blend(1);
    be->set_depth(0, 1, 0);
    rect(90, 90, 150, 130, 0xFF000000, 0, 0);
    be->set_clut(palette, 3, 0, 15, 0);
    psp_tex_state t = {.addr = atlas,
                       .stride = 512,
                       .w = 512,
                       .h = 512,
                       .fmt = 4,
                       .func = 3,
                       .tcc_rgba = 1,
                       .min_filter = 1,
                       .mag_filter = 1};
    be->set_texture(&t);
    psp_blend_state bs = {.write_colour = 1, .enable = 1, .src = 2, .dst = 3};
    be->set_blend(&bs);
    font_quad(100, 100, 13, 1);
    /* Same binding, another draw class: the font policy must flush and stop. */
    font_quad(120, 100, 13, 0);
    font_quad(140, 100, 12, 1);
    /* The PSP source-alpha blend uses /256, so opaque white becomes 254. */
    expected_pixel(A, 202, 204, 0xFFFEFEFE, "bitmap font preserves the full stroke weight");
    expected_pixel(A, 203, 204, 0xFFFEFEFE, "bitmap font preserves both physical columns");
    int w, h;
    unsigned char *p = render_gl_capture(A, &w, &h);
    uint32_t c = p ? pixel(p, w, 242, 204) : 0;
    check(p && (c & 255) > 0 && (c & 255) < 255,
          "same atlas on 3D geometry retains linear filtering");
    c = p ? pixel(p, w, 282, 204) : 0;
    check(p && (c & 255) > 0 && (c & 255) < 255,
          "other screen-space draws retain linear filtering");
    free(p);
}

static void resize_history(void) {
    target(B);
    untextured();
    blend(1);
    be->set_depth(1, 1, 1);          /* as above: a disabled test writes no depth */
    rect(0, 0, 512, 272, 0x5A332211, 10000, 0);
    be->finish();
    const int sizes[][2] = {{1365, 767},  {2560, 1440}, {3440, 1440},
                            {3840, 2160}, {641, 961},   {960, 544}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        present_request_window_size(sizes[i][0], sizes[i][1]);
        int dw = 0, dh = 0;
        for (int n = 0; n < 400; n++) {
            present_gl_drawable_size(&dw, &dh);
            if (dw == sizes[i][0] && dh == sizes[i][1]) break;
            const struct timespec delay = {0, 5000000};
            nanosleep(&delay, NULL);
        }
        check(dw == sizes[i][0] && dh == sizes[i][1], "SDL resize publishes a coherent size");
        be->present();
        int w, h;
        unsigned char *p = render_gl_capture(B, &w, &h);
        int cw = dw, ch = dw * 272 / 480;
        if (ch > dh) {
            ch = dh;
            cw = dh * 480 / 272;
        }
        if (present_adaptive_aspect() && (long long)dw * 272 >= (long long)dh * 480) {
            cw = dw;
            ch = dh;
        }
        check(p && w == (int)ceil(512.0 * cw / 480) && h == ch,
              "attachment follows the fitted physical drawable");
        check(p && pixel(p, w, w / 2, h / 2) == 0x5A332211,
              "resize preserves color and alpha history");
        free(p);
    }
    /* Scissored stencil writes and depth tests consume the migrated history. */
    psp_blend_state bs = {.write_colour = 1,
                          .stencil_test = 1,
                          .stencil_func = 2,
                          .stencil_ref = 0x5A,
                          .stencil_mask = 255,
                          .op_zpass = 4};
    be->set_blend(&bs);
    be->set_depth(1, 4, 0);
    be->set_scissor(10, 10, 10, 10);
    rect(0, 0, 480, 272, 0xFFFFFFFF, 5000, 0);
    expected_pixel(B, 20, 20, 0x5BFFFFFF, "resize preserves depth and stencil tests");
    expected_pixel(B, 22, 20, 0x5A332211, "scaled scissor excludes the adjacent pixel");
}

int main(void) {
    if (render_gl_resolution_mode() != 1 || psp_mem_init()) return 2;
    psp_hle_init();
    be = render_gl_backend();
    if (!be || psp_render_register(be) || psp_render_select("gl")) return 2;
    present_want_gl();
    if (present_start() || be->init(W, H)) return 2;
    detail_and_views();
    cpu_writes_and_layout();
    bitmap_font();
    resize_history();
    unsigned (*gl_error)(void) = (unsigned (*)(void))present_gl_proc("glGetError");
    check(gl_error && gl_error() == 0, "rendering, views and resize leave no GL errors");
    render_gl_report(stdout);
    printf("resolution-tests: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
