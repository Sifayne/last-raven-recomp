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

#include "decode.h"          /* PSP_RA_INDEX */
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
#define STACK_TOP   (PSP_RAM_BASE + PSP_RAM_SIZE - 0x1000)

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
        psp_sched_stop_all();   /* does not return */
        return;
    }
    siglongjmp(g_abort, 1);
}

static void on_signal(int sig) {
    g_reason = (sig == SIGALRM) ? 2 : 3;
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
    sa.sa_handler = on_signal;
    sa.sa_flags   = SA_ONSTACK | SA_NODEFER;
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

    /* 2 — registration and firmware. */
    psp_recomp_register();
    psp_hle_init();
    if (iso) psp_io_set_umd_image(iso);
    psp_hle_register(0x8F2DF740u, "ModuleMgrForUser", "StopUnloadSelfModule",
                     hle_stop_unload_self);
    printf("  [2] runtime   %u functions registered\n", psp_dispatch_count());
    printf("      disc      %s\n", iso ? iso : "(none -- raw umd: opens will fail)");

    /* 3 — machine state. $k0 points at a thread control block; the allocator
     *     reaches through it for the reent structure, so it has to be real
     *     before any allocation, not just before the first C++ object. */
    memset(&psp_cpu, 0, sizeof psp_cpu);
    psp_cpu.r[PSP_REG_SP] = STACK_TOP;
    psp_cpu.r[PSP_RA_INDEX] = RA_DONE;
    printf("  [3] state     sp=0x%08X  (k0/reent not yet set up)\n", psp_cpu.r[PSP_REG_SP]);

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
    if (live > 0) live = psp_sched_drain(60);

    printf("---\n");
    printf("ctors:    %s\n", ctors_ok == 0 ? "ok" : "incomplete");
    if (g_guest_exited) printf("entry:    guest exited with status %u\n", g_exit_status);
    else                printf("entry:    %s\n", entry_ok == 0 ? "returned" : "stopped");
    printf("threads:  %s\n",
           live == 0 ? "all finished" : "still alive (see the deadlock report above)");
    printf("bad mem:  %llu accesses\n", (unsigned long long)psp_mem_bad_access);
    printf("disc read: %llu bytes\n", (unsigned long long)psp_io_bytes_read());
    printf("  most-called firmware functions:\n");
    psp_hle_dump_calls(stdout, 12);
    psp_hle_dump_recent(stdout);

    psp_mem_free();
    psp_blob_free(&b);
    return (ctors_ok == 0 && entry_ok == 0) ? 0 : 1;
}
