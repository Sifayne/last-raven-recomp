/* Fixed 30 Hz simulation scheduling, independent of the guest and SDL.
 * Phase is nanoseconds multiplied by 30, so 1/30 second never rounds down. */
#ifndef LR_FPS_CLOCK_H
#define LR_FPS_CLOCK_H
#include <stdint.h>

enum { FPS_MAX_CATCHUP = 4 };
typedef struct {
    uint64_t last_ns, phase;
    int started;
    unsigned dropped;
    float alpha;
} fps_clock;

static inline unsigned fps_clock_step(fps_clock *c, uint64_t now) {
    if (!c->started) {
        c->started=1; c->last_ns=now; c->alpha=1;
        return 1;
    }
    uint64_t elapsed=now>=c->last_ns?now-c->last_ns:0;
    c->last_ns=now;
    /* Suspend, debugger stops and loading cannot become seconds of catch-up. */
    if (elapsed>(1000000000ull*FPS_MAX_CATCHUP+29)/30) {
        elapsed=(1000000000ull*FPS_MAX_CATCHUP+29)/30;
        c->phase=0; c->dropped++;
    }
    c->phase+=elapsed*30;
    unsigned ticks=(unsigned)(c->phase/1000000000ull);
    c->phase%=1000000000ull;
    c->alpha=(float)((double)c->phase/1000000000.0);
    return ticks;
}
#endif
