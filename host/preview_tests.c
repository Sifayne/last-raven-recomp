/* Synthetic GE lists to physical GL pixels: inset perspective views must
 * share menu placement, scissor and depth clears on both transform paths,
 * and a depth-tested screen-space sprite (the lock-on reticle) must be
 * placed with the HUD. */
#include "psprecomp/host/present.h"
#include "psprecomp/host/title.h"
#include "psprecomp/host/render_gl.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FB 0x04000000u
#define SCRATCH 0x04154000u
#define LIST 0x08800000u
#define VERTS 0x08810000u
/* present.c offers the adaptive aspect only to a title with a camera
 * replacement; the fixture stands in for host/replacements.c here, or its
 * window-aspect configurations would silently run the original view. */
const psp_title psp_title_info = {
    .name = "Fixture", .capabilities = PSP_TITLE_ADAPTIVE_ASPECT | PSP_TITLE_HUD_BANDS,
};
static const psp_render_backend *be;
static unsigned pc, va, checks, failures;
static double scene_scale, ui_scale, yscale, ui_offset;

static void check(int ok, const char *why) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); failures++; }
}
static void cmd(unsigned op, unsigned arg) {
    psp_write32(LIST + pc, op << 24 | (arg & 0xFFFFFF)); pc += 4;
}
static void flt(unsigned op, float v) {
    unsigned bits; memcpy(&bits, &v, 4); cmd(op, bits >> 8);
}
static void scissor(int x0, int y0, int x1, int y1) {
    cmd(0xD4, x0 | y0 << 10); cmd(0xD5, x1 | y1 << 10);
}
static void viewport(float x, float y, float w, float h) {
    flt(0x42, w / 2); flt(0x43, -h / 2);
    flt(0x44, 32767.5f); flt(0x45, 1808 + x + w / 2);
    flt(0x46, 1912 + y + h / 2); flt(0x47, 32767.5f);
    cmd(0x4C, 1808 * 16); cmd(0x4D, 1912 * 16);
}
static void vertex(float x, float y, float z, unsigned color) {
    psp_write32(va, color); psp_write_f32(va + 4, x);
    psp_write_f32(va + 8, y); psp_write_f32(va + 12, z); va += 16;
}
static void clear(int flags, float z) {
    cmd(0xD3, 1 | flags << 8);
    cmd(0x12, (7 << 2) | (3 << 7) | (1 << 23));
    cmd(0x01, va & 0xFFFFFF);
    vertex(0, 0, z, 0xFF000000); vertex(480, 272, z, 0xFF000000);
    cmd(0x04, (6 << 16) | 2); cmd(0xD3, 0);
}
static void model(unsigned color, float extent, float z) {
    cmd(0x12, (7 << 2) | (3 << 7)); cmd(0x01, va & 0xFFFFFF);
    const int xy[6][2] = {{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}};
    for (int i = 0; i < 6; i++) vertex(xy[i][0]*extent, xy[i][1]*extent, z, color);
    cmd(0x04, (3 << 16) | 6);
}
static void begin(unsigned addr, unsigned stride) {
    pc = 0; va = VERTS;
    cmd(0x10, (VERTS >> 8) & 0xFF0000);
    cmd(0x9C, addr); cmd(0x9D, ((addr >> 8) & 0xFF0000) | stride);
    cmd(0xD2, 3); cmd(0x1E, 0); cmd(0x17, 0); cmd(0x1D, 0);
    cmd(0x21, 0); cmd(0x22, 0); cmd(0xDB, 0); cmd(0xE7, 0);
    cmd(0x23, 1); cmd(0xDE, 4); /* LESS, with normal depth writes. */
    scissor(0, 0, stride - 1, stride == 512 ? 271 : 127);
    viewport(0, 0, 480, 272);
    clear(7, 0); /* Preview draws fail unless their own clear lands correctly. */
    static const float identity[12] = {1,0,0,0,1,0,0,0,1,0,0,0};
    static const float projection[16] = {1,0,0,0,0,1,0,0,0,0,-1,-1,0,0,-2,0};
    cmd(0x3A, 0); for (int i = 0; i < 12; i++) flt(0x3B, identity[i]);
    cmd(0x3C, 0); for (int i = 0; i < 12; i++) flt(0x3D, identity[i]);
    cmd(0x3E, 0); for (int i = 0; i < 16; i++) flt(0x3F, projection[i]);
}
static void end(void) {
    cmd(0x0F, 0); cmd(0x0C, 0);
    psp_ge_replay_list(LIST, 0, 0); be->finish();
}
static void expect(unsigned addr, float x, float y, int centered, unsigned want, const char *why) {
    int w, h; unsigned char *p = render_gl_capture(addr, &w, &h);
    const int px = (int)floor(x * (centered ? ui_scale : scene_scale) +
                              (centered ? ui_offset : 0));
    const int py = (int)floor(y * yscale);
    unsigned got = 0;
    int valid = p && px >= 0 && py >= 0 && px < w && py < h;
    if (valid) memcpy(&got, p + ((size_t)py*w + px)*4, 4);
    if (!valid || (got & 0xFFFFFF) != want)
        fprintf(stderr, "%s: (%d,%d) got %06X expected %06X\n", why, px, py, got & 0xFFFFFF, want);
    check(valid && (got & 0xFFFFFF) == want, why); free(p);
}
static void inset_views(void) {
    /* Odd-sized, half-pixel viewports from the captured part and AC panels. */
    const float panels[2][4] = {{96.5f,48.5f,169,105},{287.5f,54,181,106}};
    for (int i = 0; i < 2; i++) {
        const float *v = panels[i];
        begin(FB, 512); viewport(v[0], v[1], v[2], v[3]);
        scissor((int)v[0], (int)v[1], (int)v[0]+(int)v[2]-1, (int)v[1]+(int)v[3]-1);
        clear(4, 65535);
        model(0xFF00FF00, 4, -2); /* Oversized model: clipping must follow the panel. */
        /* Cross the GPU's 16-draw batch boundary without changing render
         * state: the next batch must retain the centered GL viewport. */
        for (int n = 0; n < 17; n++) model(0xFF0000FF, 4, -3);
        model(0xFFFF0000, 0.4f, -1.5f); /* Nearer blue wins only at the center. */
        end();
        const float cx = v[0]+v[2]/2, cy = v[1]+v[3]/2;
        expect(FB, cx, cy, 1, 0xFF0000, "preview center and depth survive model batch boundary");
        expect(FB, v[0]+2, cy, 1, 0x00FF00, "preview depth clear reaches left edge");
        expect(FB, v[0]+v[2]-3, cy, 1, 0x00FF00, "preview reaches right edge");
        expect(FB, v[0]-2, cy, 1, 0, "preview scissor excludes left neighbor");
        expect(FB, v[0]+v[2]+2, cy, 1, 0, "preview scissor excludes right neighbor");
    }
}
static void scene_views(void) {
    /* A small scissor alone must not turn the main camera into a preview. */
    begin(FB, 512); scissor(287, 54, 467, 159);
    clear(4, 65535); model(0xFF0000FF, 4, -2); end();
    expect(FB, 378, 107, 0, 0x0000FF, "scissored full-screen camera uses scene placement");
    expect(FB, 280, 107, 0, 0, "full-screen camera retains scene scissor");
    /* An inset viewport with unrelated clipping also remains scene geometry. */
    begin(FB, 512); clear(4, 65535); viewport(287.5f, 54, 181, 106);
    model(0xFF0000FF, 0.4f, -2); end();
    expect(FB, 378, 107, 0, 0x0000FF, "inset viewport requires matching scissor");
}
static void hud_views(void) {
    /* The lock-on reticle: a screen-space sprite that tests depth. The game
     * projects it with the display camera's native focal length, so the
     * centred 1:1 HUD placement is where its target is; spreading it with
     * the scene would multiply its offset from the centre a second time. */
    begin(FB, 512); clear(4, 65535);
    cmd(0xDE, 7); /* GEQUAL: passes against the cleared depth. */
    cmd(0x12, (7 << 2) | (3 << 7) | (1 << 23)); cmd(0x01, va & 0xFFFFFF);
    vertex(300, 100, 65535, 0xFFFF00FF); vertex(316, 116, 65535, 0xFFFF00FF);
    cmd(0x04, (6 << 16) | 2);
    end();
    expect(FB, 308, 108, 1, 0xFF00FF, "depth-tested screen-space sprite is placed with the HUD");
    if (ui_offset > 0 || fabs(scene_scale - ui_scale) > 0.01)
        expect(FB, 308, 108, 0, 0, "depth-tested screen-space sprite is not spread with the scene");
    if (ui_offset <= 0) return;
    /* A marker or ring whose target sits in the bands beside the centred 480
     * columns: the game draws it there (beyond the stride too), and it must
     * show -- a ring with its depth test even while it straddles the edge,
     * a marker once it lies entirely off the screen. Backdrop pieces that
     * start on the screen and run into the stride padding, tiled 64x64 or
     * taller than the screen, stay clipped at the edge as the PSP kept them. */
    begin(FB, 512); scissor(0, 0, 479, 271); clear(4, 65535);
    cmd(0xDE, 7);
    cmd(0x12, (7 << 2) | (3 << 7) | (1 << 23)); cmd(0x01, va & 0xFFFFFF);
    vertex(520, 100, 65535, 0xFFFF00FF); vertex(536, 116, 65535, 0xFFFF00FF);
    cmd(0x04, (6 << 16) | 2); /* one sprite per draw, as the game issues them */
    cmd(0x01, va & 0xFFFFFF);
    vertex(-40, 100, 65535, 0xFF00FFFF); vertex(-24, 116, 65535, 0xFF00FFFF);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(470, 180, 65535, 0xFF00FFFF); vertex(500, 196, 65535, 0xFF00FFFF);
    cmd(0x04, (6 << 16) | 2);
    cmd(0xDE, 1); cmd(0x01, va & 0xFFFFFF);
    vertex(520, 140, 65535, 0xFFFFFF00); vertex(536, 156, 65535, 0xFFFFFF00);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(448, 200, 65535, 0xFF00FF00); vertex(512, 264, 65535, 0xFF00FF00);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(478, 0, 65535, 0xFF0000FF); vertex(512, 512, 65535, 0xFF0000FF);
    cmd(0x04, (6 << 16) | 2);
    end();
    begin(FB, 512); scissor(0, 0, 479, 271); clear(4, 65535);
    cmd(0xDE, 7);
    cmd(0x12, (7 << 2) | (3 << 7) | (1 << 23)); cmd(0x01, va & 0xFFFFFF);
    vertex(520, 100, 65535, 0xFFFF00FF); vertex(536, 116, 65535, 0xFFFF00FF);
    cmd(0x04, (6 << 16) | 2); /* one sprite per draw, as the game issues them */
    cmd(0x01, va & 0xFFFFFF);
    vertex(-40, 100, 65535, 0xFF00FFFF); vertex(-24, 116, 65535, 0xFF00FFFF);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(470, 180, 65535, 0xFF00FFFF); vertex(500, 196, 65535, 0xFF00FFFF);
    cmd(0x04, (6 << 16) | 2);
    cmd(0xDE, 1); cmd(0x01, va & 0xFFFFFF);
    vertex(520, 140, 65535, 0xFFFFFF00); vertex(536, 156, 65535, 0xFFFFFF00);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(448, 200, 65535, 0xFF00FF00); vertex(512, 264, 65535, 0xFF00FF00);
    cmd(0x04, (6 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(478, 0, 65535, 0xFF0000FF); vertex(512, 512, 65535, 0xFF0000FF);
    cmd(0x04, (6 << 16) | 2);
    end();
    expect(FB, 528, 108, 1, 0xFF00FF, "depth-tested sprite beyond the right edge shows in the band");
    expect(FB, -32, 108, 1, 0x00FFFF, "depth-tested sprite beyond the left edge shows in the band");
    expect(FB, 495, 188, 1, 0x00FFFF, "depth-tested sprite straddling the edge shows past it");
    expect(FB, 528, 148, 1, 0xFFFF00, "2D marker entirely beyond the right edge shows in the band");
    expect(FB, 470, 232, 1, 0x00FF00, "backdrop tile shows up to the screen edge");
    expect(FB, 490, 232, 1, 0, "backdrop tile stays clipped at the screen edge");
    expect(FB, 500, 100, 1, 0, "backdrop piece taller than the screen stays out of the band");
    /* The lock box is lines: a line entirely past the edge, and one that
     * crosses it, both reach into the band. */
    begin(FB, 512); scissor(0, 0, 479, 271); clear(4, 65535);
    cmd(0x12, (7 << 2) | (3 << 7) | (1 << 23)); cmd(0x01, va & 0xFFFFFF);
    vertex(490, 60, 0, 0xFF00FFFF); vertex(530, 60, 0, 0xFF00FFFF);
    cmd(0x04, (1 << 16) | 2);
    cmd(0x01, va & 0xFFFFFF);
    vertex(460, 80, 0, 0xFF00FFFF); vertex(530, 80, 0, 0xFF00FFFF);
    cmd(0x04, (1 << 16) | 2);
    end();
    /* Sample the one-pixel lines at their centre row: at window resolution
     * row 80 begins at a fractional physical row. */
    expect(FB, 510, 60.5f, 1, 0x00FFFF, "line entirely past the right edge shows in the band");
    expect(FB, 510, 80.5f, 1, 0x00FFFF, "line crossing the right edge continues into the band");
    expect(FB, 470, 80.5f, 1, 0x00FFFF, "line crossing the right edge keeps its on-screen part");
}
static void set_size(int dw, int dh) {
    present_request_window_size(dw, dh);
    int w = 0, h = 0;
    for (int n = 0; n < 400; n++) {
        present_gl_drawable_size(&w, &h);
        if (w == dw && h == dh) break;
        const struct timespec pause = {0, 5000000}; nanosleep(&pause, NULL);
    }
    check(w == dw && h == dh, "requested drawable size reached"); be->present();
    const int wide = present_adaptive_aspect() ? (int)lround(272.0 * w / h) : 480;
    if (render_gl_resolution_mode()) {
        int cw = wide > 480 ? w : h * 480 / 272;
        scene_scale = (double)cw / 480; yscale = (double)h / 272;
        ui_scale = wide > 480 ? yscale : scene_scale;
        ui_offset = wide > 480 ? (cw - 480*ui_scale)/2 : 0;
    } else {
        scene_scale = (double)wide / 480; yscale = ui_scale = 1;
        ui_offset = (wide - 480) / 2;
    }
}
int main(void) {
    if (psp_mem_init()) return 2;
    psp_hle_init(); be = render_gl_backend();
    if (!be || psp_render_register(be) || psp_render_select("gl")) return 2;
    present_want_gl(); if (present_start() || be->init(480, 272)) return 2;
    const int sizes[][2] = {{1920,720},{960,544},{1440,544}};
    for (unsigned i = 0; i < sizeof sizes/sizeof sizes[0]; i++) {
        set_size(sizes[i][0], sizes[i][1]); inset_views(); scene_views(); hud_views();
    }
    begin(SCRATCH, 256); viewport(16, 16, 96, 64); scissor(16,16,111,79);
    clear(4, 65535); model(0xFF00FF00, 4, -2); end();
    scene_scale = ui_scale = yscale = 1; ui_offset = 0;
    expect(SCRATCH, 64, 48, 1, 0x00FF00, "scratch preview stays in guest pixels");
    expect(SCRATCH, 14, 48, 1, 0, "scratch scissor stays in guest pixels");
    check(!psp_mem_bad_access, "no invalid guest memory accesses");
    unsigned (*error)(void) = (unsigned (*)(void))present_gl_proc("glGetError");
    check(error && !error(), "no GL errors");
    render_gl_report(stdout);
    printf("preview-tests: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
