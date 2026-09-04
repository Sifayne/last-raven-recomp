/* gereplay — run one captured frame of GE work through a chosen backend.
 *
 * Why this exists. Comparing two render backends on the running game does not
 * work: a GL run needs a window and a window implies real-time pacing, while a
 * headless software run is unpaced, so the two drift and "the same frame" stops
 * meaning anything (findings item 56). Every attempt to work around that was a
 * probe; this removes time from the comparison instead. The same lists, the
 * same memory, the same starting registers, run twice.
 *
 * It is also what M5's gate asks for in as many words -- "pixel-comparable to
 * the software path on a fixed set of display lists".
 *
 *   PSPRECOMP_GE_CAPTURE=frame.gcap PSPRECOMP_GE_CAPTURE_FRAME=310 \
 *       scripts/09-replay.sh --decode scenarios/hanger.pad
 *   build/host/gereplay frame.gcap software out-sw.ppm
 *   build/host/gereplay frame.gcap gl       out-gl.ppm
 *
 * The capture holds guest memory, so the replay needs no ELF, no disc and no
 * scheduler -- only the GE and a backend.
 */

#include "present.h"
#include "render_gl.h"

#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GE_CAP_MAGIC  0x50414347u
#define GE_CAP_VER    2

typedef struct {
    uint32_t magic, version;
    uint32_t state_bytes, n_lists;
    uint32_t ram_base, ram_bytes;
    uint32_t vram_base, vram_bytes;
    uint32_t mod_base, mod_bytes;
} ge_cap_header;

typedef struct { uint32_t list, stall, base; } ge_cap_list;

enum { SCREEN_W = 480, SCREEN_H = 272 };

static int die(const char *msg) { fprintf(stderr, "gereplay: %s\n", msg); return 1; }

/* The framebuffer as the GE left it, in the guest's own layout. Written
 * straight out rather than through the display code: what is wanted here is
 * the render target, not whatever the game would have chosen to scan out. */
static int write_ppm(const char *path, uint32_t addr, uint32_t stride, int fmt) {
    const uint8_t *fb = psp_mem_ptr(addr, (uint32_t)stride * SCREEN_H * 4u);
    if (!fb) return die("the target is not in guest memory");
    FILE *f = fopen(path, "wb");
    if (!f) return die("cannot write the image");
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint8_t rgb[3] = { 0, 0, 0 };
            if (fmt == 3) {                      /* 8888, the display format */
                const uint8_t *p = fb + ((size_t)y * stride + x) * 4u;
                rgb[0] = p[0]; rgb[1] = p[1]; rgb[2] = p[2];
            } else {                             /* 5650 / 5551 / 4444 */
                const uint16_t v = *(const uint16_t *)(fb + ((size_t)y * stride + x) * 2u);
                if (fmt == 0) {
                    rgb[0] = (uint8_t)((v & 31) << 3);
                    rgb[1] = (uint8_t)(((v >> 5) & 63) << 2);
                    rgb[2] = (uint8_t)(((v >> 11) & 31) << 3);
                } else if (fmt == 1) {
                    rgb[0] = (uint8_t)((v & 31) << 3);
                    rgb[1] = (uint8_t)(((v >> 5) & 31) << 3);
                    rgb[2] = (uint8_t)(((v >> 10) & 31) << 3);
                } else {
                    rgb[0] = (uint8_t)((v & 15) << 4);
                    rgb[1] = (uint8_t)(((v >> 4) & 15) << 4);
                    rgb[2] = (uint8_t)(((v >> 8) & 15) << 4);
                }
            }
            fwrite(rgb, 3, 1, f);
        }
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr,
            "gereplay <capture.gcap> <backend> <out.ppm>\n"
            "\n"
            "Replays one captured frame of GE work into a backend and writes\n"
            "the render target as a PPM. Backends: software, null, gl (gl\n"
            "needs a window, so it needs a display).\n"
            "\n"
            "A fourth argument overrides which buffer is written, as hex. The\n"
            "GE's target at the end of a frame is not always the one the frame\n"
            "drew into -- this game alternates two display buffers -- and the\n"
            "per-target primitive counts in the summary say which is wanted.\n");
        return 2;
    }
    const char *path = argv[1], *backend = argv[2], *out = argv[3];

    FILE *f = fopen(path, "rb");
    if (!f) return die("cannot open the capture");
    ge_cap_header h;
    if (fread(&h, sizeof h, 1, f) != 1) return die("short capture");
    if (h.magic != GE_CAP_MAGIC) return die("not a GE capture");
    if (h.version != GE_CAP_VER) return die("capture is a different version");

    uint8_t *state = malloc(h.state_bytes);
    ge_cap_list *lists = malloc(sizeof *lists * (h.n_lists ? h.n_lists : 1));
    if (!state || !lists) return die("out of memory");
    if (fread(state, h.state_bytes, 1, f) != 1) return die("short state");
    if (h.n_lists && fread(lists, sizeof *lists, h.n_lists, f) != h.n_lists)
        return die("short list table");

    if (psp_mem_init() != 0) return die("no guest memory");
    void *ram  = psp_mem_ptr(h.ram_base,  h.ram_bytes);
    void *vram = psp_mem_ptr(h.vram_base, h.vram_bytes);
    if (!ram || !vram) return die("guest memory does not match the capture");
    if (fread(ram,  h.ram_bytes,  1, f) != 1) return die("short RAM image");
    if (fread(vram, h.vram_bytes, 1, f) != 1) return die("short VRAM image");
    /* The module image is mapped outside the RAM window and psp_mem_ptr looks
     * there first, so without it every list address in this game resolves to
     * nothing at all. */
    if (h.mod_bytes) {
        if (psp_mem_map_module(h.mod_base, h.mod_bytes) != 0)
            return die("cannot map the module region");
        void *mod = psp_mem_ptr(h.mod_base, h.mod_bytes);
        if (!mod) return die("module region did not map");
        if (fread(mod, h.mod_bytes, 1, f) != 1) return die("short module image");
    }
    fclose(f);

    psp_hle_init();

    const psp_render_backend *gl = render_gl_backend();
    if (gl) psp_render_register(gl);
    if (psp_render_select(backend) != 0) {
        fprintf(stderr, "gereplay: unknown backend \"%s\"\navailable:", backend);
        for (size_t i = 0; psp_render_backend_name(i); i++)
            fprintf(stderr, " %s", psp_render_backend_name(i));
        fprintf(stderr, "\n");
        return 2;
    }
    /* GL draws into a window, so one has to exist before anything is drawn --
     * exactly as in boot.c, and for the same reason. */
    if (strcmp(backend, "gl") == 0) {
        present_want_gl();
        if (present_start() != 0)
            return die("the gl backend needs a window and there is none");
    }
    if (psp_render_current()->init(SCREEN_W, SCREEN_H) != 0)
        return die("the backend did not initialise");

    /* The registers as they stood when the frame began. Without this the first
     * commands land on a reset GE and the replay draws something the run never
     * did -- the whole reason the capture carries state at all. */
    psp_ge_init();
    psp_ge_state_load(state);
    psp_ge_sync_backend();

    for (uint32_t i = 0; i < h.n_lists; i++)
        psp_ge_replay_list(lists[i].list, lists[i].stall, lists[i].base);
    psp_ge_drain_all();
    psp_render_current()->finish();
    psp_render_current()->present();

    uint32_t addr = 0, stride = 0; int fmt = 3;
    psp_ge_current_target(&addr, &stride, &fmt);
    if (argc > 4) addr = (uint32_t)strtoul(argv[4], NULL, 16);
    printf("gereplay: %u list(s) through %s, target %08X stride %u fmt %d\n",
           h.n_lists, psp_render_current()->name, addr, stride, fmt);
    psp_ge_dump_stats(stdout);
    render_gl_report(stdout);

    if (getenv("GEREPLAY_SCAN")) {
        /* Where did the pixels actually go? Scan both guest regions in 64K
         * blocks and name the ones that are not all zero. */
        const struct { const char *n; uint32_t base, size; } regs[] = {
            { "vram", h.vram_base, h.vram_bytes },
            { "ram",  h.ram_base,  h.ram_bytes  },
        };
        for (size_t r = 0; r < 2; r++) {
            for (uint32_t o = 0; o + 0x10000 <= regs[r].size; o += 0x10000) {
                const uint8_t *p8 = psp_mem_ptr(regs[r].base + o, 0x10000);
                if (!p8) continue;
                uint32_t nz = 0;
                for (uint32_t i = 0; i < 0x10000; i++) if (p8[i]) nz++;
                if (nz > 4096)
                    printf("  %s %08X: %u non-zero bytes of 65536\n",
                           regs[r].n, regs[r].base + o, nz);
            }
        }
    }
    return write_ppm(out, addr, stride ? stride : 512, fmt);
}
