/* boot — bring the recompiled module up far enough to run.
 *
 * The oracle established that the translation is faithful. It could say nothing
 * about whether the game *runs*, because both sides execute the same guest code
 * and anything missing from the execution environment is missing from both.
 * This is that environment, built in the order psprecomp's BRINGUP.md sets out:
 *
 *   1. load and relocate the module
 *   2. give it memory — a stack, and a heap the allocator can carve from
 *   3. $k0 -> a thread control block, because the allocator reaches through it
 *   4. run the static constructors
 *   5. call module_start
 *
 * Each step is reported, because the useful output of a failed boot is *which
 * step* failed, not that it failed. A step that cannot run says so and the run
 * continues, so one missing piece does not hide the state of the next.
 *
 * This runs the recompiled C, not the interpreter. When something here goes
 * wrong the interpreter is the instrument for finding out why:
 *
 *   allegrexrecomp interp <elf> --from 0x<addr> --regs
 */

#include "loader.h"
#include "container.h"
#include "present.h"

#include "decode.h"          /* PSP_RA_INDEX */
#include "psprecomp/clock.h"
#include "psprecomp/cpu.h"
#include "psprecomp/ctors.h"
#include "psprecomp/dispatch.h"
#include "psprecomp/sched.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"

#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void psp_recomp_register(void);

/* The guest's user-mode stack. PSP puts it at the top of user RAM and grows it
 * down; the exact value matters less than leaving room below for the heap. */
/* The entry stack. 256K is generous for module_start, and generous is right:
 * a recompiled frame is larger than the MIPS one it came from, and a stack
 * that overflows here corrupts the heap it was allocated from. */
#define MAIN_STACK_SIZE  0x40000u

/* Where a `jr $ra` out of the outermost call lands. Deliberately unmapped, so
 * arriving here is unambiguous rather than something that might be real code. */
#define RA_DONE     0x0DEAD000u

/* ---- surviving the guest ---------------------------------------------------
 *
 * Recompiled code has no error return: an instruction it could not translate
 * calls psp_unimplemented(), and a wild jump reaches a dispatch miss. Both are
 * fatal to the caller by default, which is right for a shipped port and useless
 * here -- the whole point is to see how far the boot gets before it stops. */

static sigjmp_buf g_abort;
static int        g_reason;         /* 1 trap, 2 timeout, 3 fault, 4 dispatch miss */
static uint32_t   g_reason_addr;
static const char *g_reason_what;

void psp_unimplemented(uint32_t addr, const char *what) {
    g_reason = 1; g_reason_addr = addr; g_reason_what = what;
    siglongjmp(g_abort, 1);
}

void psp_syscall(uint32_t id) {
    g_reason = 1; g_reason_addr = id; g_reason_what = "syscall";
    siglongjmp(g_abort, 1);
}

static void on_miss(uint32_t addr) {
    g_reason = 4; g_reason_addr = addr; g_reason_what = "dispatch miss";
    siglongjmp(g_abort, 1);
}

/* The guest's exit path.
 *
 * `ModuleMgrForUser` NID 0x8F2DF740 stops and unloads the calling module. The
 * runtime does not implement it, so it returned 0 -- and the guest's exit
 * routine is the shape every libc uses:
 *
 *     002A7B34  jal   0x00253748        ; exit(status)
 *     002A7B38  ori   $a0, $zero, 0x1   ; delay slot: status = 1
 *     002A7B3C  beq   $zero, $zero, 002A7B34   ; exit never returns... but if
 *                                              ; it does, go round again
 *
 * A no-op exit turns that safety loop into a real one, which is what the boot
 * was hitting: sixteen consecutive calls and a timeout. The recompiled code was
 * correct throughout; the environment was not.
 *
 * Terminating here is both accurate and useful -- "the module called exit(1)"
 * is a diagnosis, where a spin is not. */
static int      g_guest_exited;
static uint32_t g_exit_status;

static void hle_stop_unload_self(void) {
    g_guest_exited = 1;
    g_exit_status  = psp_arg(0);
    /* Built with -DPSPRECOMP_TRACE this prints the functions entered on the way
     * here, which is the only practical way to tell *which* abort fired: the
     * abort wrapper has several callers and static analysis cannot say which
     * one ran. Without the define it compiles to nothing and says so. */
    g_reason = 5;

    /* Only the main context may jump: g_abort belongs to its stack. A guest
     * thread stops the scheduler instead, and the main context reports the exit
     * when its drain returns. */
    if (psp_sched_current() != 0) {
        psp_sched_stop_all("sceKernelExitGame");   /* does not return */
        return;
    }
    siglongjmp(g_abort, 1);
}

/* PSPRECOMP_WATCH=<hex address> prints the argument registers on entry to that
 * function, plus what the pointer-looking ones point at.
 *
 * Bring-up keeps arriving at the same question -- one function out of tens of
 * thousands decides something, and the decision is made on a value nobody has
 * seen. Rebuilding 2.1M lines of generated C with a printf in it answers that
 * once, slowly, and leaves debris. */
static void watch_hit(uint32_t addr) {
    fprintf(stderr, "watch: psp_func_%08X(a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X)\n",
            addr, psp_arg(0), psp_arg(1), psp_arg(2), psp_arg(3));
    for (int i = 0; i < 4; i++) {
        const uint32_t v = psp_arg(i);
        if (v < PSP_RAM_BASE || v >= PSP_RAM_BASE + PSP_RAM_SIZE) continue;
        const uint32_t deref = psp_read32(v);
        fprintf(stderr, "         a%d -> 0x%08X", i, deref);
        if (deref >= PSP_RAM_BASE && deref < PSP_RAM_BASE + PSP_RAM_SIZE)
            fprintf(stderr, "  -> first bytes %02X %02X %02X %02X",
                    psp_read8(deref), psp_read8(deref + 1),
                    psp_read8(deref + 2), psp_read8(deref + 3));
        fprintf(stderr, "\n");
    }
    /* The object `this` points at, when it is one.
     *
     * A guard on `this->[28]` is decided by a value that only exists while the
     * run is going, and PSPRECOMP_PEEK reads memory after it has stopped. Those
     * are different questions, and answering the second while asking the first
     * is how a field that is zero at the end gets read as a condition that was
     * never true. */
    {
        const uint32_t o = psp_arg(0);
        if (o >= PSP_RAM_BASE && o + 64 < PSP_RAM_BASE + PSP_RAM_SIZE) {
            fprintf(stderr, "         [this]");
            for (int i = 0; i < 12; i++)
                fprintf(stderr, " +%d:%08X", i * 4, psp_read32(o + (uint32_t)i * 4));
            fprintf(stderr, "\n");
        }
    }
    /* Argument 2 as a matrix, when it points at RAM. The uploader takes the
     * source matrix there, and "what did the game actually build" is the only
     * way to tell a bad matrix from a bad upload of a good one. */
    {
        const uint32_t m = psp_arg(2);
        /* Module data as well as the heap: a static matrix buffer lives in the
         * loaded image, and excluding it printed nothing at all. */
        if (m > 0x1000u && m + 64 < PSP_RAM_BASE + PSP_RAM_SIZE) {
            fprintf(stderr, "         [a2 as 4x4]");
            for (int i = 0; i < 16; i++) {
                union { uint32_t u; float f; } c; c.u = psp_read32(m + (uint32_t)i * 4);
                fprintf(stderr, "%s%.2f", (i % 4) ? " " : " | ", (double)c.f);
            }
            fprintf(stderr, "\n");
        }
    }
    /* And how it got here. A watched function reached through a vtable has no
     * `jal` naming its caller, so the arguments alone say what was asked and
     * not who asked. The ring is entries-in-order rather than a call stack --
     * see the note in the findings -- so read the nearest entries as "what ran
     * just before", not as a stack, and confirm anything structural with gdb. */
    psp_trace_dump();
}

/* Write the framebuffer the display is scanning out to a PPM.
 *
 * The whole point of a rasterizer is that you can look at what it produced,
 * and until now nothing here could: the GE counted geometry and the summary
 * reported numbers. A PPM because it needs no library and any viewer opens it.
 *
 * The image is what the *guest* believes it is showing -- the address comes
 * from its last sceDisplaySetFrameBuf -- so an empty file is a real answer
 * too: it means the game never pointed the display anywhere. */
/* Which parts of VRAM have anything in them.
 *
 * "The texture sampled black" has two very different causes -- the texture is
 * not there, or it is somewhere else -- and guessing between them has already
 * cost more than measuring would have. This walks VRAM in 64K blocks and says
 * which ones are non-zero, so the answer is visible rather than inferred. */
/* PSPRECOMP_FINDPTR=<hex> reports every address in the loaded module holding
 * that value as a 32-bit word.
 *
 * A relocatable PRX writes its function-pointer tables at load time, so a
 * vtable entry does not exist in the file on disk -- searching the ELF for it
 * finds nothing, which reads as "nothing references this" when the truth is the
 * opposite. This searches the image the loader actually produced.
 *
 * The question it answers keeps coming up: a function that no `jal` targets is
 * reached through a pointer, and the only way to find out which object owns it
 * is to find where the pointer is stored. */
static void find_pointer(uint32_t lo, uint32_t size) {
    const char *v = getenv("PSPRECOMP_FINDPTR");
    if (!v || !*v) return;
    const uint32_t want = (uint32_t)strtoul(v, NULL, 0);

    /* The module image, then RAM, then VRAM.
     *
     * Scanning only the module was enough for a function pointer written by
     * the loader, and useless for anything the game allocates -- a kernel uid
     * lives in a heap object, and reporting "nowhere" for it invites the
     * conclusion that only one place holds it. Which offset a value sits at is
     * only an argument about *one* struct: the same uid can live at two
     * offsets in two objects, and a static search that assumes otherwise
     * proves nothing. */
    const struct { const char *what; uint32_t lo, size; } region[] = {
        { "module", lo,            size          },
        { "ram",    PSP_RAM_BASE,  PSP_RAM_SIZE  },
        { "vram",   PSP_VRAM_BASE, PSP_VRAM_SIZE },
    };
    printf("findptr:  0x%08X stored at:", want);
    int n = 0;
    for (size_t r = 0; r < sizeof region / sizeof *region; r++)
        for (uint32_t a = region[r].lo;
             a + 4 <= region[r].lo + region[r].size && n < 32; a += 4)
            if (psp_read32(a) == want) {
                printf(" %s:0x%08X", region[r].what, a);
                n++;
            }
    if (!n) printf(" nowhere");
    printf("\n");
}

/* PSPRECOMP_REACHED=<hex>[,<hex>...] says, for each address, whether control
 * ever arrived there.
 *
 * "Does this code run" has been answered here with gdb breakpoints and with
 * PSPRECOMP_WATCH. Breakpoints are external and one batch per run; the watch
 * hooks PSP_ENTER, which the emitter writes once per function *body*, so a
 * watch on anything that is not a function entry never fires and reports a
 * negative it cannot actually see. dispatch.c records that flaw producing a
 * confidently wrong conclusion. The mark bitmap covers every label instead.
 *
 * It has a smaller version of the same limit, and it matters. PSP_MARK is
 * emitted only at *labels* -- branch and jump targets, function entries, split
 * entries, fall-through targets. A `jal` site, or a store in the middle of a
 * block, is not a label and can never be marked, so asking about one gets
 * "not reached" when the truth is "cannot be seen from here".
 * psp_trace_was_marked only reports -1 for an address outside the module; it
 * cannot tell an unlabelled address from an unvisited one. So map an address
 * to the label that covers it before asking:
 *
 *     grep -o 'L_[0-9A-F]\{8\}: PSP_MARK' game/generated/aclr_funcs.c | sort -u
 *
 * and take the greatest label <= the address within the same function. Control
 * reaching that label is control reaching the block the instruction sits in,
 * which is the question worth asking anyway. */
/* PSPRECOMP_PEEK=<hex>[,<hex>...] prints what is at each address when the run
 * stops: the word, and the byte, because guest structs mix both and which one
 * a field is decides what "zero" means.
 *
 * The recurring question here is not "where is this value" -- FINDPTR answers
 * that -- but "what is this field", and answering it has meant a watch, a
 * rebuild, or a guess. A branch on a byte flag is the commonest guard in this
 * game's movie code and the commonest thing to be wrong about. */
static void peek(void) {
    const char *v = getenv("PSPRECOMP_PEEK");
    if (!v || !*v) return;
    for (const char *p = v; *p; ) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(p, &end, 0);
        if (end == p) break;
        printf("peek:     0x%08X  word 0x%08X  byte 0x%02X\n",
               a, psp_read32(a), psp_read8(a));
        p = (*end == ',') ? end + 1 : end;
    }
}

/* PSPRECOMP_WATCHMEM=<hex> names the code that writes one word.
 *
 * PEEK reads memory once the run has stopped and WATCH fires on function entry;
 * neither answers "who wrote this". The write path can, and mem.c has been able
 * to since it was written -- psp_mem_watch_write was declared, implemented and
 * never called, which is the third instrument in this runtime to have been
 * finished and left unreachable.
 *
 * Reports the writing function from the trace ring, so it wants a
 * PSPRECOMP_TRACE build; without one the address is right and the attribution
 * is zero. */
static void watch_memory(void) {
    const char *v = getenv("PSPRECOMP_WATCHMEM");
    if (!v || !*v) return;
    char *end;
    const uint32_t a = (uint32_t)strtoul(v, &end, 0);
    if (*end == ',') {
        const uint32_t want = (uint32_t)strtoul(end + 1, NULL, 0);
        psp_mem_watch_write_value(a, want);
        printf("      watchmem  0x%08X = 0x%08X only "
               "(needs a PSPRECOMP_TRACE build to name the writer)\n", a, want);
    } else {
        psp_mem_watch_write(a);
        printf("      watchmem  0x%08X (needs a PSPRECOMP_TRACE build to name the writer)\n", a);
    }
}

enum { MAX_REACHED = 32 };
static uint32_t g_reached[MAX_REACHED];
static int      g_reached_n;

static void reached_init(uint32_t lo, uint32_t hi) {
    const char *v = getenv("PSPRECOMP_REACHED");
    if (!v || !*v) return;
    for (const char *p = v; *p && g_reached_n < MAX_REACHED; ) {
        char *end;
        const uint32_t a = (uint32_t)strtoul(p, &end, 0);
        if (end == p) break;
        g_reached[g_reached_n++] = a;
        p = (*end == ',') ? end + 1 : end;
    }
    psp_trace_marks_init(lo, (hi - lo) / 4);
    printf("      reached   %d address(es) watched (needs a PSPRECOMP_TRACE build)\n",
           g_reached_n);
}

static void reached_report(void) {
    for (int i = 0; i < g_reached_n; i++) {
        const int m = psp_trace_was_marked(g_reached[i]);
        printf("reached:  0x%08X  %s\n", g_reached[i],
               m < 0 ? "not observable -- outside the loaded module"
                     : m ? "reached" : "not reached (or not a label -- see PSPRECOMP_REACHED)");
    }
}

static void survey_vram(void) {
    enum { BLOCK = 0x10000 };
    printf("vram:     ");
    int any = 0;
    for (uint32_t off = 0; off < PSP_VRAM_SIZE; off += BLOCK) {
        uint64_t nz = 0;
        for (uint32_t i = 0; i < BLOCK; i += 4)
            if (psp_read32(PSP_VRAM_BASE + off + i)) nz++;
        if (nz) {
            printf("%s0x%08X:%llu", any ? "  " : "",
                   PSP_VRAM_BASE + off, (unsigned long long)nz);
            any = 1;
        }
    }
    if (!any) printf("entirely zero");
    printf("\n");
}

/* Dump one buffer.
 *
 * Both are written, because picking one has already been misleading. The GE's
 * render target is the back buffer, and a run that stops just after a
 * frame-start clear finds it freshly cleared -- which reads as "nothing was
 * drawn" when a whole frame was drawn and then presented. The buffer the
 * display is scanning out is the one a player would be looking at. */
static void dump_one(uint32_t base, const char *path, const char *label) {
    const uint32_t stride = psp_display_stride();
    if (!base || !stride) {
        printf("frame%s:  not dumped -- no framebuffer\n", label);
        return;
    }

    enum { W = 480, H = 272 };          /* the panel, whatever the stride is */
    FILE *f = fopen(path, "wb");
    if (!f) { printf("frame:    cannot write %s\n", path); return; }
    fprintf(f, "P6\n%d %d\n255\n", W, H);

    const uint32_t fmt = psp_display_format();
    uint64_t nonzero = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint32_t at = base + (uint32_t)(y * (int)stride + x) *
                                (fmt == 3 ? 4u : 2u);
            uint8_t rgb[3];
            if (fmt == 3) {                       /* 8888 */
                const uint32_t p = psp_read32(at);
                rgb[0] = (uint8_t)(p & 0xFF);
                rgb[1] = (uint8_t)((p >> 8) & 0xFF);
                rgb[2] = (uint8_t)((p >> 16) & 0xFF);
            } else {                              /* 5650 / 5551 / 4444 */
                const uint16_t p = (uint16_t)psp_read16(at);
                rgb[0] = (uint8_t)((p & 0x1F) << 3);
                rgb[1] = (uint8_t)(((p >> 5) & 0x3F) << 2);
                rgb[2] = (uint8_t)(((p >> 11) & 0x1F) << 3);
            }
            if (rgb[0] || rgb[1] || rgb[2]) nonzero++;
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    printf("frame%s:  %s  (0x%08X stride %u fmt %u, %llu of %d pixels differing from the corner)\n",
           label, path, base, stride, fmt, (unsigned long long)nonzero, W * H);
}

static void dump_framebuffer(void) {
    const char *path = getenv("PSPRECOMP_FRAME");
    if (!path || !*path) path = "frame.ppm";

    const uint32_t ge = psp_ge_target();
    const uint32_t disp = psp_display_framebuffer();

    dump_one(ge ? ge : disp, path, "");
    if (disp && disp != ge) {
        char alt[1024];
        snprintf(alt, sizeof alt, "%s.display.ppm", path);
        dump_one(disp, alt, "(shown)");
    }

    /* And the fullest frame the run ever presented, which is the one that
     * answers "did anything appear" -- the two above only say what was in the
     * buffers at the instant the run stopped. */
    const uint64_t score = psp_display_best_score();
    if (!score) {
        printf("frame(best): no presented frame had a non-black pixel\n");
        return;
    }
    char best[1024];
    snprintf(best, sizeof best, "%s.best.ppm", path);
    FILE *f = fopen(best, "wb");
    if (!f) { printf("frame(best): cannot write %s\n", best); return; }
    enum { W = 480, H = 272 };
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    const uint32_t *px = psp_display_best();
    for (int i = 0; i < W * H; i++) {
        const uint32_t p = px[i];
        const uint8_t rgb[3] = { (uint8_t)(p & 0xFF), (uint8_t)((p >> 8) & 0xFF),
                                 (uint8_t)((p >> 16) & 0xFF) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("frame(best): %s  (0x%08X, %llu of %d pixels differing from the corner)\n",
           best, psp_display_best_addr(), (unsigned long long)score, W * H);
}

static void install_watch(void) {
    const char *v = getenv("PSPRECOMP_WATCH");
    if (!v || !*v) return;
    const uint32_t addr = (uint32_t)strtoul(v, NULL, 0);
    psp_trace_watch(addr, watch_hit);
    printf("      watch     psp_func_%08X (needs a PSPRECOMP_TRACE build)\n", addr);
}

#define ALT_STACK_SIZE ((size_t)(SIGSTKSZ < 65536 ? 65536 : SIGSTKSZ))

/* Each thread needs its own: sigaltstack is per-thread, and the fault most
 * worth catching is a blown stack, which leaves no room to run a handler. */
static void install_alt_stack(void) {
    stack_t ss;
    ss.ss_sp    = malloc(ALT_STACK_SIZE);
    ss.ss_size  = ALT_STACK_SIZE;
    ss.ss_flags = 0;
    if (ss.ss_sp) sigaltstack(&ss, NULL);
}

static void on_signal(int sig);

static void *g_fault_addr;

static void on_signal_info(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    /* The address a fault touched is the difference between "a crash" and a
     * diagnosis: zero means a null dereference, a small value means an offset
     * from one, and a wild value means a corrupt pointer. */
    g_fault_addr = (sig == SIGSEGV || sig == SIGBUS) && si ? si->si_addr : NULL;
    on_signal(sig);
}

static void on_signal(int sig) {
    g_reason = (sig == SIGALRM) ? 2 : 3;

    /* A guest thread cannot jump to g_abort: that belongs to the main stack.
     * Nor can it be unwound -- it is part-way down a host call stack of
     * generated code. So it reports and ends the process. Losing the boot
     * summary is a fair trade for a diagnosis instead of a core dump. */
    if (psp_sched_current() != 0) {
        fprintf(stderr, "\npsprecomp: %s in guest thread 0x%08X, last fn 0x%08X",
                sig == SIGALRM ? "timed out" : "host fault (SIGSEGV/SIGBUS)",
                psp_sched_current(), psp_trace_last());
        if (sig != SIGALRM) fprintf(stderr, ", touching %p", g_fault_addr);
        fprintf(stderr, "\n");
        psp_trace_dump();
        fflush(NULL);
        _exit(2);
    }
    siglongjmp(g_abort, 1);
}

static const char *reason_str(void) {
    switch (g_reason) {
    case 1: return g_reason_what ? g_reason_what : "unimplemented";
    case 2: return "timed out";
    case 3: return "host fault (SIGSEGV/SIGBUS)";
    case 4: return "dispatch miss";
    case 5: return "guest called exit";
    }
    return "?";
}

static void install_handlers(void) {
    static char altstack[SIGSTKSZ < 65536 ? 65536 : SIGSTKSZ];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack, .ss_flags = 0 };
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags     = SA_ONSTACK | SA_NODEFER | SA_SIGINFO;
    sa.sa_sigaction = on_signal_info;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}

/* Run one guest call with a wall-clock bound, reporting rather than dying. */
static int guarded_call(uint32_t addr, unsigned timeout_s, const char *what) {
    g_reason = 0;
    if (sigsetjmp(g_abort, 1) == 0) {
        alarm(timeout_s);
        psp_dispatch(addr);
        alarm(0);
        return 0;
    }
    alarm(0);
    printf("    %-28s stopped: %s", what, reason_str());
    if (g_reason == 1 || g_reason == 4) printf(" at 0x%08X", g_reason_addr);
    if (g_reason == 5) printf("(%u)", g_exit_status);
    printf("\n");
    /* However the call stopped, the functions entered on the way there are the
     * useful part -- and a timeout needs them most, because unlike a trap it
     * carries no address of its own. Empty unless built -DPSPRECOMP_TRACE. */
    psp_trace_dump();
    return -1;
}

/* ---- static constructors ---------------------------------------------------
 *
 * `.cplinit` is an array of (constructor, priority) pairs, null-terminated by a
 * zero constructor. The module carries its own routine to sort and run it, but
 * calling that means identifying it per-game; the table is named in the section
 * headers, so walking it here works for any module.
 *
 * Priority is zero for every entry in this module, so the sort the game's own
 * routine performs is a no-op and running them in table order matches. A module
 * that used priorities would need the sort; this does not, and the difference
 * would show up as constructors running out of order rather than silently. */

static int report_cplinit(const psp_blob *b, const elf_info *e) {
    psp_section s;
    if (psp_find_section(b, e, ".cplinit", &s) != 0 || s.size < 8) {
        printf("    .cplinit                     absent\n");
        return 0;
    }

    const uint32_t n = s.size / 8;
    uint32_t listed = 0, undiscovered = 0;

    for (uint32_t i = 0; i < n; i++) {
        const uint32_t fn = psp_read32(s.addr + i * 8);
        if (!fn) break;                       /* null terminator */
        listed++;
        /* Every constructor must be a function the emitter generated, or the
         * module's own initialiser will dispatch into nothing. This is the
         * check that was failing before .cplinit fed discovery. */
        if (!psp_lookup(fn)) undiscovered++;
    }

    printf("    .cplinit at 0x%08X    %u constructors, %u undiscovered\n",
           s.addr, listed, undiscovered);
    printf("    (run by the module's own initialiser, not from here)\n");
    return undiscovered ? -1 : 0;
}

/* ---- entry ----------------------------------------------------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "boot <module.elf> [disc.iso]\n"
                        "\n"
                        "The disc image backs the raw UMD device. A PSP title\n"
                        "opens `umd1:` by bare name to read its own sectors, and\n"
                        "without an image that open fails and the game retries\n"
                        "forever.\n");
        return 2;
    }
    const char *iso = (argc > 2) ? argv[2] : NULL;

    psp_blob b;
    if (psp_blob_read(argv[1], &b) != 0) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    elf_info e;
    if (elf_parse(b.data, b.size, &e) != 0) { fprintf(stderr, "not an ELF/PRX\n"); return 1; }
    if (psp_mem_init() != 0) { fprintf(stderr, "no guest memory\n"); return 1; }

    install_handlers();
    psp_set_miss_handler(on_miss);

    printf("module:   %s\n", argv[1]);

    /* 1 — load and relocate. */
    psp_load_info li;
    if (psp_load_module(&b, &e, &li) != 0) { fprintf(stderr, "cannot load module\n"); return 1; }
    printf("  [1] load      0x%08X + %u bytes, %d relocations\n",
           li.lo, li.hi - li.lo, li.nrelocs);
    reached_init(li.lo, li.hi);
    watch_memory();

    /* 2 — registration and firmware. */
    psp_recomp_register();
    psp_hle_init();
    /* After psp_hle_init, which resets the allocator. */
    psp_sysmem_reserve_module(li.lo, li.hi);
    if (iso) psp_io_set_umd_image(iso);
    psp_hle_register(0x8F2DF740u, "ModuleMgrForUser", "StopUnloadSelfModule",
                     hle_stop_unload_self);
    printf("  [2] runtime   %u functions registered\n", psp_dispatch_count());

    /* Presentation, before the module loads: the real-time clock anchors to
     * the moment it is enabled, and that moment should be the start of the
     * run rather than the end of the load. PSPRECOMP_WINDOW enables the SDL
     * layer -- window, pad, audio -- and implies pacing; PSPRECOMP_REALTIME
     * paces a headless run on its own, which is what makes a wall-clock
     * measurement of a real scene honest. */
    if (getenv("PSPRECOMP_WINDOW")) {
        if (present_start() == 0)
            printf("      window    on (SDL2: video, pad, audio; implies real-time pacing)\n");
        else
            printf("      window    unavailable -- running headless\n");
    } else if (getenv("PSPRECOMP_REALTIME")) {
        psp_clock_realtime(1);
        printf("      pacing    real-time (headless)\n");
    }

    install_watch();
    psp_sched_set_thread_hook(install_alt_stack);
    printf("      disc      %s\n", iso ? iso : "(none -- raw umd: opens will fail)");

    /* 3 — machine state. $k0 points at a thread control block; the allocator
     *     reaches through it for the reent structure, so it has to be real
     *     before any allocation, not just before the first C++ object. */
    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu_reset_fp();      /* a fresh thread's float/vector registers are NaN */

    /* The stack is *allocated*, not just pointed at the top of RAM.
     *
     * It used to sit at a fixed 0x09FFF000, which is inside the user heap --
     * and the heap hands out blocks from the top down, so the guest's very
     * first allocation straddled it. On hardware the initial thread's stack
     * comes out of the same partition as everything else, and taking it the
     * same way here is both more faithful and the only way the allocator can
     * know not to hand it to someone else. */
    const uint32_t stack = psp_sysmem_alloc(MAIN_STACK_SIZE, 1);
    if (!stack) { fprintf(stderr, "cannot allocate the entry stack\n"); return 1; }
    psp_cpu.r[PSP_REG_SP] = (stack + MAIN_STACK_SIZE - 64) & ~15u;
    psp_cpu.r[PSP_RA_INDEX] = RA_DONE;
    printf("  [3] state     sp=0x%08X  (%uK stack, k0/reent not yet set up)\n",
           psp_cpu.r[PSP_REG_SP], MAIN_STACK_SIZE / 1024);

    /* 4 — static constructors.
     *
     * Not run here, deliberately, and the reason is worth keeping: doing so was
     * wrong and the guest said so. Running `.cplinit` from the host and then
     * calling module_start means the module runs the same table again through
     * its own initialiser, every static object is registered for destruction
     * twice, and the C++ runtime aborts with
     *
     *   C++ runtime abort: internal error:
     *   static object marked for destruction more than once
     *
     * which is exactly the check working. The module runs its own constructors;
     * what it needed was not a host that runs them but a *discovery* that finds
     * them -- all 151 were missing from the function list, so the module's own
     * initialiser was dispatching to code that had never been emitted. Seeding
     * discovery from .cplinit fixed that, and this step became redundant.
     *
     * Kept as a report so the table is still visible at boot. */
    printf("  [4] ctors\n");
    const int ctors_ok = report_cplinit(&b, &e);

    /* 5 — module_start. */
    printf("  [5] entry     0x%08X\n", e.entry);
    const int entry_ok = guarded_call(e.entry, 10, "module_start");

    /* 6 — the threads module_start left behind.
     *
     * The usual shape is that module_start creates a thread, starts it, and
     * returns immediately: the game is in the thread, not the entry point. So
     * returning from module_start is the beginning of the run, not the end. */
    int live = psp_sched_live();
    printf("  [6] threads   %d spawned by module_start\n", live);
    if (g_guest_exited) printf("      (the guest already exited during entry)\n");
    /* 60s is the bring-up default: long enough to reach a deadlock, short
     * enough to stay interactive. A movie is longer than that, so watching
     * one end needs a bigger window -- PSPRECOMP_DRAIN=<seconds>. */
    int drain_s = 60;
    if (getenv("PSPRECOMP_DRAIN")) {
        drain_s = atoi(getenv("PSPRECOMP_DRAIN"));
        if (drain_s <= 0) drain_s = 60;
        printf("      drain     %ds (PSPRECOMP_DRAIN)\n", drain_s);
    }
    if (live > 0) live = psp_sched_drain(drain_s);

    printf("---\n");
    printf("ctors:    %s\n", ctors_ok == 0 ? "ok" : "incomplete");
    if (g_guest_exited) printf("entry:    guest exited with status %u\n", g_exit_status);
    else                printf("entry:    %s\n", entry_ok == 0 ? "returned" : "stopped");
    /* A force-stopped run also leaves zero threads alive -- stop_all marks them
     * dead -- so "all finished" would be printed for a run the host just
     * killed. The stop reason is the difference between a run that ended and
     * one that was ended. */
    const char *stopped = live == 0 ? psp_sched_stop_reason() : NULL;
    if (stopped)
        printf("threads:  stopped by the host (%s)\n", stopped);
    else
        printf("threads:  %s\n",
               live == 0 ? "all finished" : "still alive (see the deadlock report above)");
    printf("bad mem:  %llu accesses\n", (unsigned long long)psp_mem_bad_access);
    printf("disc read: %llu bytes\n", (unsigned long long)psp_io_bytes_read());
    printf("pixels:   %llu drawn by the rasterizer\n",
           (unsigned long long)psp_ge_pixels());

    /* Stack-balance totals, not just the first few.
     *
     * psp_trace_sp stops printing after 24 sites and psp_trace_sp_call after
     * 16, which is the right call for a log -- but it means counting lines in
     * the output measures the cap rather than the module. Both counters
     * saturate on this game, so a change that halved the real number would
     * look identical from the log alone. Only meaningful in a TRACE=1 build;
     * both are zero otherwise, so the line reports that rather than implying a
     * clean run. */
    {
        const unsigned long long sp_bad  = (unsigned long long)psp_sp_violations();
        const unsigned long long spc_bad = (unsigned long long)psp_sp_call_violations();
        if (sp_bad || spc_bad) {
            /* Leaks and sites first: the raw total counts returns, so one hot
             * function buries the rest, and a positive delta is an artifact of
             * discovery splitting a function rather than a defect. */
            printf("sp:       %llu leak(s) in %u site(s); %llu unbalanced return(s), "
                   "%llu callee(s) that did not restore\n",
                   (unsigned long long)psp_sp_leaks(), psp_sp_sites(),
                   sp_bad, spc_bad);
            psp_sp_dump(stdout, 12);
        } else {
            printf("sp:       no imbalance recorded (build with TRACE=1 to measure)\n");
        }
    }
    psp_ge_dump_stats(stdout);
    find_pointer(li.lo, li.hi - li.lo);
    reached_report();
    peek();
    survey_vram();
    dump_framebuffer();
    if (getenv("PSPRECOMP_SEMA")) {
        psp_threadman_dump_threads(stdout);
        psp_threadman_dump_signalled(stdout);
    }
    printf("  most-called firmware functions:\n");
    psp_hle_dump_calls(stdout, 12);
    psp_hle_dump_recent(stdout);

    /* Guest threads that would not stop are still running guest code, and that
     * code is still reading and writing guest memory. Freeing it here is a
     * use-after-free against a thread we have already admitted we cannot
     * unwind -- it showed up as an intermittent SIGSEGV inside psp_write32,
     * three frames deep, on a pointer the memory layer had correctly judged to
     * be in range before the range was handed back to the allocator.
     *
     * There is nothing to be gained by freeing at all here: the process is
     * about to end and the kernel reclaims everything. So the teardown is
     * skipped whenever anything is still live, and _exit avoids running
     * atexit handlers that would reach the same memory. */
    const int rc = (ctors_ok == 0 && entry_ok == 0) ? 0 : 1;
    if (live > 0) {
        fflush(NULL);
        _exit(rc);
    }

    psp_mem_free();
    psp_blob_free(&b);
    return rc;
}
