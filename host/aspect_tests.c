/* Exercise the game's camera code: renderer fixtures alone cannot detect a
 * mission projection that still uses the PSP aspect, or a HUD projection
 * that follows the window. Requires the local ELF and generated module, but
 * no window, ISO, save, or recorded RAM snapshot. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "loader.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "aclr_funcs.h"
#include "psprecomp/host/present.h"

enum {
    SCENE = 0x00421040u, RENDER = 0x0043D8D0u,
    CAMERA = PSP_RAM_BASE + 0x1000, DESCRIPTOR = PSP_RAM_BASE + 0x2000,
    STACK = PSP_RAM_BASE + PSP_RAM_SIZE - 0x1000,
};
static int adaptive, wide = 480, checks, failures;

/* Only the drawable input is substituted; projection and culling execute the
 * actual recompiled game functions and the production replacement. */
int present_adaptive_aspect(void) { return adaptive; }
int present_aspect_wide_width(void) { return wide; }
void present_gl_drawable_size(int *w, int *h) { *w = wide; *h = 272; }
void psp_syscall(uint32_t id) {
    fprintf(stderr, "unexpected syscall %x\n", id); abort();
}
void psp_unimplemented(uint32_t addr, const char *what) {
    fprintf(stderr, "unexpected instruction %x: %s\n", addr, what); abort();
}
static void check(int ok, const char *name) {
    checks++;
    if (!ok) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
static int near(float a, float b) { return fabsf(a - b) < 0.00001f; }
static void call(void (*fn)(void), uint32_t arg) {
    r_a0 = arg; r_sp = STACK; r_ra = 0xDEAD000u;
    fn();
    check(r_sp == STACK && r_ra == 0xDEAD000u, "guest stack and return address");
}
static void seed(int w, int h) {
    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu_reset_fp();
    memset(psp_mem_ptr(CAMERA, 736), 0, 736);
    call(psp_func_00088928, CAMERA);
    call(psp_func_002586A8, SCENE + 64);
    psp_write_f32(SCENE + 256, 45.0f);
    psp_write_f32(SCENE + 260, 4.0f);
    psp_write_f32(SCENE + 264, 6000.0f);
    psp_write_f32(SCENE + 268, 480.0f / 272.0f);
    psp_write_f32(SCENE + 448, 4.0f);
    psp_write_f32(SCENE + 452, 6000.0f);
    psp_write32(RENDER + 260, DESCRIPTOR);
    psp_write32(DESCRIPTOR + 12, w);
    psp_write32(DESCRIPTOR + 16, h);
}
static void rebuild(void) {
    call(psp_func_000889B4, CAMERA);
    /* 0000100C invokes this shared-camera rebuild through its vtable. */
    call(psp_func_002588D0, SCENE + 64);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    psp_blob blob; elf_info elf; psp_load_info loaded;
    if (psp_blob_read(argv[1], &blob) ||
        elf_parse(blob.data, blob.size, &elf) || psp_mem_init() ||
        psp_load_module(&blob, &elf, &loaded)) return 1;

    unsigned char original[736], native[736];
    seed(480, 272);
    call(psp_func_000889B4__orig, CAMERA);
    memcpy(original, psp_mem_ptr(CAMERA, sizeof original), sizeof original);
    seed(480, 272); adaptive = 0; wide = 640; rebuild();
    memcpy(native, psp_mem_ptr(CAMERA, sizeof native), sizeof native);
    check(!memcmp(original, native, sizeof native), "disabled camera matches original");
    const float px = psp_read_f32(SCENE + 192);
    const float py = psp_read_f32(SCENE + 212);
    const float side = fabsf(psp_read_f32(CAMERA + 456) / psp_read_f32(CAMERA + 448));

    adaptive = 1; wide = 480; rebuild();
    check(!memcmp(native, psp_mem_ptr(CAMERA, sizeof native), sizeof native),
          "native-width camera matches original");
    wide = 640; rebuild();
    check(near(psp_read_f32(SCENE + 192), px * 480 / 640),
          "mission horizontal projection follows drawable");
    check(psp_read_f32(SCENE + 212) == py, "vertical projection unchanged");
    /* The camera's own matrices place the lock box and reticle on the CPU;
     * the GL backend draws that HUD 1:1, so they must stay at 480x272. The
     * width-derived fields +716/+720 stay native for the same reason; only
     * the aspect the garage copies into the shared camera widens. */
    check(!memcmp(native + 64, psp_mem_ptr(CAMERA + 64, 256), 256),
          "HUD projection matrices stay native");
    check(psp_read_f32(CAMERA + 716) == 1.0f && psp_read_f32(CAMERA + 720) == 1.0f,
          "camera width and height scales stay native");
    check(!memcmp(native, psp_mem_ptr(CAMERA, 448), 448) &&
          !memcmp(native + 544, psp_mem_ptr(CAMERA + 544, 724 - 544), 724 - 544),
          "only the cull planes at +448 and the aspect at +724 differ from native");
    check(near(psp_read_f32(CAMERA + 724), 640.0f / 272.0f), "garage aspect follows drawable");
    check(near(fabsf(psp_read_f32(CAMERA + 456) / psp_read_f32(CAMERA + 448)),
               side * 640 / 480), "horizontal culling expands with projection");
    check(psp_read_f32(CAMERA + 724) == psp_read_f32(SCENE + 268),
          "garage and mission agree on aspect");
    check(psp_read32(DESCRIPTOR + 12) == 480 &&
          psp_read32(DESCRIPTOR + 16) == 272, "guest framebuffer dimensions restored");
    wide = 960; rebuild();
    check(near(psp_read_f32(SCENE + 192) * 2, px), "second resize follows width");
    wide = 480; rebuild();
    check(psp_read_f32(SCENE + 192) == px, "resize back restores native projection");
    check(!memcmp(native, psp_mem_ptr(CAMERA, sizeof native), sizeof native),
          "resize back restores native culling and camera");

    /* The lock target's on-screen test. The render system's screen matrix at
     * +400 is the identity here; the projector scales by 16 into fixed point
     * and the test converts x>>4 - 2048 + 240 and y>>3 - 4096 + 136 to
     * pixels, so a point is written as x = px + 1808, y = (py + 3960) / 2. */
    enum { TARGET = 0x004D4B40u, HELD = 0x004D4B50u + 12, OUT = PSP_RAM_BASE + 0x3000, MATRIX = RENDER + 400 };
    psp_write32(HELD, 1);
    for (int i = 0; i < 16; i++) psp_write_f32(MATRIX + 4 * i, i % 5 == 0 ? 1.0f : 0.0f);
    psp_write32(DESCRIPTOR + 40, 0); psp_write32(DESCRIPTOR + 44, 0);
    psp_write32(DESCRIPTOR + 48, 480); psp_write32(DESCRIPTOR + 52, 272);
    struct { int px, py, z; int native, wide; const char *name; } const points[] = {
        { 240, 136,  100, 1, 1, "centre is on screen either way" },
        { 500, 136,  100, 0, 1, "right band counts as on screen only when wide" },
        { -70, 136,  100, 0, 1, "left band counts as on screen only when wide" },
        { 570, 136,  100, 0, 0, "beyond the right band stays off screen" },
        { -90, 136,  100, 0, 0, "beyond the left band stays off screen" },
        { 500, 300,  100, 0, 0, "below the screen stays off screen in the band" },
        { 500, 136, -5000, 0, 0, "behind the camera stays off screen in the band" },
    };
    memset(&psp_cpu, 0, sizeof psp_cpu); psp_cpu_reset_fp();
    for (unsigned i = 0; i < sizeof points / sizeof points[0]; i++) {
        for (int pass = 0; pass < 2; pass++) {
            adaptive = pass; wide = pass ? 640 : 480;
            psp_write_f32(TARGET + 0, (float)(points[i].px + 1808));
            psp_write_f32(TARGET + 4, (float)(points[i].py + 3960) / 2);
            psp_write_f32(TARGET + 8, (float)points[i].z);
            psp_write_f32(TARGET + 12, 1.0f);
            r_v0 = 0xBAD;
            call(psp_func_001FBDF8, OUT);
            const int want = pass ? points[i].wide : points[i].native;
            check((int)r_v0 == want, points[i].name);
            if (want && points[i].z > -4096)
                check(psp_read32(OUT) == (uint32_t)points[i].px &&
                      psp_read32(OUT + 4) == (uint32_t)points[i].py, "on-screen test leaves the pixel position");
        }
    }
    psp_write32(HELD, 0); adaptive = 1; wide = 640;
    psp_write_f32(TARGET + 0, (float)(500 + 1808));
    r_v0 = 0xBAD;
    call(psp_func_001FBDF8, OUT);
    check(r_v0 == 0, "no lock target stays off screen in the band");
    adaptive = 0; wide = 480;

    seed(256, 128);
    call(psp_func_000889B4__orig, CAMERA);
    memcpy(original, psp_mem_ptr(CAMERA, sizeof original), sizeof original);
    seed(256, 128); wide = 640; rebuild();
    check(!memcmp(original, psp_mem_ptr(CAMERA, sizeof original), sizeof original),
          "scratch preview camera matches original");
    check(psp_read_f32(SCENE + 268) == 480.0f / 272.0f,
          "scratch preview does not change shared aspect");
    check(psp_mem_bad_access == 0, "no invalid guest accesses");
    printf("aspect: %d/%d checks passed\n", checks - failures, checks);
    psp_blob_free(&blob); psp_mem_free();
    return failures != 0;
}
