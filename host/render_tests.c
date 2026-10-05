/* Synthetic backend-contract checks, with full RGBA readback. No game data.
 * Run identically on software and GL; expectations do not call either renderer. */
#include "psprecomp/host/present.h"
#include "psprecomp/host/title.h"
#include "render_gl.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include <stdio.h>
#include <string.h>

/* Not a game: the shared host offers this program nothing title-specific
 * (psprecomp/host/title.h). */
const psp_title psp_title_info = { .name = "Render test" };

enum { W = 512, H = 272 };
#define FB 0x04000000u
#define AUX 0x04140000u
/* The depth buffer is guest VRAM, as on the PSP, and starts at the VRAM base:
 * left there, depth writes would land in FB. Put it after FB instead. */
#define ZB 0x04088000u
static const psp_render_backend *be;
static psp_blend_state bs;
static unsigned checks, failures;
typedef struct { int x, y; uint32_t rgba; const char *why; } expectation;
static expectation expected[2048];
static unsigned n_expected;

static void expect(int x, int y, uint32_t rgba, const char *why) {
    expected[n_expected++] = (expectation){x, y, rgba, why};
}
static void rect(int x, int y, int w, int h, uint32_t rgba, float z) {
    const psp_vertex v[] = {
        {.x=x*16, .y=y*16, .z=z, .rgba=rgba, .inv_w=1, .tex_q=1, .fog=255},
        {.x=(x+w)*16, .y=(y+h)*16, .z=z, .rgba=rgba, .inv_w=1, .tex_q=1, .fog=255}
    };
    be->set_blend(&bs);
    be->draw(PSP_PRIM_SPRITES, v, 2);
}
static void seed(int x, int y, unsigned a) {
    bs = (psp_blend_state){.write_colour=1, .write_alpha=1};
    /* Prime the depth buffer: an enabled test that always passes. A *disabled*
     * test writes no depth, so this cannot be set_depth(0, 1, 1). */
    be->set_depth(1, 1, 1);
    rect(x, y, 1, 1, (a << 24) | 0x302010, 10000);
    bs = (psp_blend_state){.write_colour=1, .stencil_test=1,
        .stencil_func=1, .stencil_ref=0x61, .stencil_mask=255};
    be->set_depth(0, 1, 0);
}
static unsigned op_value(int op, unsigned old, unsigned ref) {
    const unsigned values[] = {old, 0, ref, 255-old, old == 255 ? 255 : old+1, old ? old-1 : 0};
    return values[op];
}
static void test_stencil(void) {
    /* Exercise every bit on both transfers, with no RGB writes. */
    for (unsigned a = 0; a < 256; a++) {
        seed((int)a, 1, a);
        bs.write_colour=0; bs.op_zpass=3;
        rect((int)a, 1, 1, 1, 0xDEADBEEF, 0);
        expect((int)a, 1, ((255-a) << 24) | 0x302010, "all-byte stencil invert/export");
    }
    const unsigned values[] = {0, 0x31, 0x32, 0x34, 0xF3, 255};
    for (int f = 0; f < 8; f++) for (int k = 0; k < 6; k++) {
        const int x = f*8+k;
        seed(x, 3, values[k]);
        bs.stencil_func=f; bs.stencil_ref=0x32; bs.stencil_mask=0xF3;
        bs.op_sfail=2; bs.op_zpass=3;
        const unsigned a = values[k] & 0xF3, r = 0x32 & 0xF3;
        const int pass[] = {0, 1, a==r, a!=r, a<r, a<=r, a>r, a>=r};
        rect(x, 3, 1, 1, 0xFFA09080, 0);
        expect(x, 3, pass[f] ? ((255-values[k])<<24)|0xA09080 : 0x32302010,
               "masked stencil comparison (mask does not mask replacement)");
    }
    const unsigned edges[] = {0, 1, 128, 254, 255};
    for (int outcome = 0; outcome < 3; outcome++) for (int op = 0; op < 6; op++)
        for (int k = 0; k < 5; k++) {
            const int x = op*6+k, y = 5+outcome;
            seed(x, y, edges[k]);
            bs.stencil_func = outcome == 0 ? 0 : 1;
            bs.op_sfail = outcome == 0 ? op : 0;
            bs.op_zfail = outcome == 1 ? op : 0;
            bs.op_zpass = outcome == 2 ? op : 0;
            be->set_depth(outcome == 1, 0, 0);
            rect(x, y, 1, 1, 0xC0A09080, 20000);
            expect(x, y, (op_value(op, edges[k], 0x61)<<24) |
                   (outcome == 2 ? 0xA09080 : 0x302010), "sfail/zfail/zpass and saturation");
        }
    /* Alpha rejection precedes even the stencil-fail operation. */
    seed(0, 10, 0x73); bs.stencil_func=0; bs.op_sfail=2;
    bs.alpha_test=1; bs.alpha_func=0; bs.alpha_mask=255;
    rect(0, 10, 1, 1, 0xFFFFFFFF, 0);
    expect(0, 10, 0x73302010, "alpha kill suppresses stencil fail");
    for (int fail = 0; fail < 2; fail++) {
        seed(1+fail, 10, 0x40);
        bs.stencil_func=fail ? 1 : 0;
        bs.op_sfail=4; bs.op_zfail=4;
        be->set_depth(1, fail ? 0 : 1, 1);
        rect(1+fail, 10, 1, 1, 0xFFFFFFFF, 100);
        bs.stencil_test=0;
        be->set_depth(1, 4, 0);
        rect(1+fail, 10, 1, 1, 0xFFA09080, 5000);
        expect(1+fail, 10, 0x41A09080, "failed stencil/depth must not write depth");
    }
    /* A partial alpha clear must preserve earlier stencil outside it and
     * invalidate the imported stencil before the next test. */
    seed(0, 12, 0x40); seed(1, 12, 0x40);
    bs.op_zpass=4; rect(0, 12, 2, 1, 0xFFA09080, 0);
    bs=(psp_blend_state){.write_alpha=1};
    be->set_scissor(0, 12, 0, 12);
    rect(0, 12, 2, 1, 0x99000000, 0);
    be->set_scissor(0, 0, W-1, H-1);
    bs=(psp_blend_state){.write_colour=1, .stencil_test=1, .stencil_func=2,
        .stencil_ref=0x99, .stencil_mask=255};
    rect(0, 12, 2, 1, 0xFF776655, 0);
    expect(0, 12, 0x99776655, "alpha clear invalidates hardware stencil");
    expect(1, 12, 0x41A09080, "scissored alpha clear preserves adjacent stencil");
    /* Destination alpha changes inside a batch of overlapping primitives. */
    seed(0, 14, 255);
    bs.enable=1; bs.src=10; bs.fixa=0; bs.dst=4; bs.op_zpass=1;
    rect(0, 14, 1, 1, 0xFFFFFFFF, 0);
    rect(0, 14, 1, 1, 0xFFFFFFFF, 0);
    expect(0, 14, 0, "overlapping draws blend with updated destination alpha");
    seed(1, 14, 255); bs.op_zpass=1; bs.write_colour=0;
    rect(1, 14, 1, 1, 0xFFFFFFFF, 0);
    bs.stencil_test=0; bs.write_colour=1; bs.enable=1; bs.src=10; bs.fixa=0; bs.dst=4;
    rect(1, 14, 1, 1, 0xFFFFFFFF, 0);
    expect(1, 14, 0, "destination-alpha consumer after stencil disabled");
}

static void test_lines(void) {
    be->set_depth(0, 1, 0);
    bs=(psp_blend_state){.write_colour=1}; be->set_blend(&bs);
    psp_vertex v[] = {
        {.x=10*16, .y=20*16, .rgba=0xFF00FF00, .fog=255, .inv_w=1, .tex_q=1},
        {.x=12*16, .y=20*16, .rgba=0xFF00FF00, .fog=255, .inv_w=1, .tex_q=1},
        {.x=24*16, .y=20*16, .rgba=0xFFFF00FF, .fog=255, .inv_w=1, .tex_q=1},
        {.x=22*16, .y=20*16, .rgba=0xFFFF00FF, .fog=255, .inv_w=1, .tex_q=1},
        {.x=26*16, .y=20*16, .rgba=0xFFFFFFFF, .fog=255, .inv_w=1, .tex_q=1},
    };
    be->draw(PSP_PRIM_LINES, v, 5);
    for (int x=8; x<28; x++) expect(x, 20, 0x44000000 |
        (x==10 || x==11 ? 0x00FF00 : x==22 || x==23 ? 0xFF00FF : 0x332211), "line endpoint coverage");
    for (int i=0; i<5; i++) v[i].y=22*16;
    be->draw(PSP_PRIM_POINTS, v, 5);
    expect(10,22,0x4400FF00,"point first endpoint");
    expect(11,22,0x44332211,"point does not fill interval");
    expect(12,22,0x4400FF00,"point last endpoint");

    /* Perspective texture interpolation on a scissored line, read at each
     * pixel's centre and divided by w there, as through-mode lines read
     * on fw 6.60 (psprecomp docs/RENDERER.md, "Texture coordinates on
     * sprites and lines"): u = 8t/(2-t), t = (x+1/2)/8. */
    const uint32_t addr=0x08800000;
    for (unsigned i=0; i<8; i++) psp_write32(addr+i*4,0xFF000010+i);
    psp_tex_state tex={.addr=addr,.stride=8,.w=8,.h=1,.fmt=3,.func=3,.tcc_rgba=1};
    be->set_texture(&tex); be->set_scissor(2,26,5,26);
    v[0].x=0; v[1].x=8*16; v[0].y=v[1].y=26*16;
    v[0].u=0; v[1].u=8; v[1].inv_w=0.5f;
    be->draw(PSP_PRIM_LINES,v,2);
    expect(1,26,0x44332211,"line scissor excludes left");
    expect(2,26,0x44000011,"line perspective texture at clipped start");
    expect(4,26,0x44000013,"line perspective texture midpoint");
    expect(5,26,0x44000014,"line perspective texture at clipped end");
    expect(6,26,0x44332211,"line scissor excludes right");
    tex=(psp_tex_state){0}; be->set_texture(&tex);
    be->set_scissor(0,0,W-1,H-1);
}

static void test_target_texture_alpha(void) {
    be->set_target(AUX, 8, 3); be->set_scissor(0,0,7,7);
    be->set_depth(0,1,0);
    bs=(psp_blend_state){.write_colour=1, .write_alpha=1};
    rect(0,0,8,8,0xAAFFFFFF,0);
    bs=(psp_blend_state){.stencil_test=1, .stencil_func=1, .stencil_ref=0x40,
        .stencil_mask=255, .op_zpass=2};
    rect(0,0,8,8,0,0);
    be->set_target(FB,W,3); be->set_scissor(0,0,W-1,H-1);
    psp_tex_state tex={.addr=AUX,.stride=8,.w=8,.h=8,.fmt=3,.func=3,.tcc_rgba=1};
    be->set_texture(&tex);
    bs=(psp_blend_state){.write_colour=1, .alpha_test=1, .alpha_func=2,
        .alpha_ref=0x40, .alpha_mask=255};
    rect(0,24,1,1,0xFFFFFFFF,0);
    expect(0,24,0x44FFFFFF,"render-target texture sees updated stencil alpha");
    tex=(psp_tex_state){0}; be->set_texture(&tex);
}

int main(int argc, char **argv) {
    if (argc != 2 || psp_mem_init() != 0) return 2;
    psp_hle_init();
    for (unsigned i=0; i<W*H; i++) psp_write32(FB+i*4,0x44332211);
    if (!strcmp(argv[1],"gl")) {
        const psp_render_backend *gl=render_gl_backend();
        if (!gl || psp_render_register(gl)) return 2;
        present_want_gl(); if (present_start()) return 2;
    }
    if (psp_render_select(argv[1])) return 2;
    be=psp_render_current(); if (be->init(W,H)) return 2;
    be->set_target(FB,W,3); be->set_scissor(0,0,W-1,H-1);
    psp_render_set_depth_buffer(ZB,W);
    psp_tex_state tex={0}; be->set_texture(&tex); be->set_fog(0,0);
    test_stencil(); test_lines(); test_target_texture_alpha();
    be->finish(); be->present();
    for (unsigned i=0; i<n_expected; i++) {
        const expectation *e=&expected[i];
        const uint32_t got=psp_read32(FB+(unsigned)(e->y*W+e->x)*4);
        checks++;
        if (got != e->rgba) {
            if (failures < 20) fprintf(stderr,"FAIL %s (%d,%d): %08X want %08X\n",e->why,e->x,e->y,got,e->rgba);
            failures++;
        }
    }
    printf("render-tests %s: %u checks, %u failures\n",argv[1],checks,failures);
    render_gl_report(stdout);
    return failures ? 1 : 0;
}
