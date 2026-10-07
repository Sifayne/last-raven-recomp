/* Shared mission render scheduling. Included by a title's replacements.
 * Input and authoritative simulation run together, once per original tick.
 * Extra frames do not poll the controller or consume recorded mouse motion. */
#ifndef LR_FPS_H
#define LR_FPS_H
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "settings.h"
#include "psprecomp/clock.h"
#include "fps_clock.h"

static struct {
    fps_clock clock;
    unsigned active, ticks_left, inserted, frames, ticks, catchup;
    uint64_t deadline, synthetic_frame;
    unsigned test_gap_frame, test_gap_ms;
    int cap, interpolate;
    FILE *log;
} fps;

/* Wall time less host pauses (psprecomp/safepoint.h), so a pause is neither
 * a late frame nor a burst of catch-up ticks. A pause cannot fall inside a
 * sleep here -- the sleeping thread keeps the scheduler token, which a pause
 * needs -- so the sleep is the same span of wall time. */
static uint64_t fps_now(void) { return psp_clock_run_ns(); }
static void fps_sleep_until(uint64_t deadline) {
    const uint64_t now=fps_now();
    if (deadline<=now) return;
    const uint64_t ns=deadline-now;
    struct timespec t={(time_t)(ns/1000000000ull),(long)(ns%1000000000ull)};
    while (nanosleep(&t,&t)==-1 && errno==EINTR) {}
}
static int fps_start(void) {
    if (!psp_settings_current()->number[LR_HIGH_FPS]) return 0;
    memset(&fps,0,sizeof fps);
    fps.active=1;
    fps.cap=(int)psp_settings_current()->number[LR_FPS_CAP];
    /* Deterministic regression injection; never affects windowed play. */
    const char *gap=getenv("PSPRECOMP_FPS_TEST_GAP");
    if (!psp_clock_is_realtime() && gap) {
        unsigned frame,ms; char extra;
        if (sscanf(gap,"%u:%u%c",&frame,&ms,&extra)==2 && frame && ms && ms<=10000) {
            fps.test_gap_frame=frame; fps.test_gap_ms=ms;
        }
    }
    const char *interp=getenv("PSPRECOMP_FPS_INTERPOLATE");
    fps.interpolate=!interp?3:!strcmp(interp,"0")?0:!strcmp(interp,"joints")?1:!strcmp(interp,"camera")?2:3;
    const char *path=getenv("PSPRECOMP_FPS_LOG");
    if (path && *path) fps.log=fopen(path,"a");
    if (fps.log) setvbuf(fps.log,NULL,_IOLBF,0);
    if (fps.log) fprintf(fps.log,"enter poll=%u cap=%d realtime=%d\n",psp_ctrl_polls(),fps.cap,psp_clock_is_realtime());
    return 1;
}
static void fps_stop(void) {
    if (fps.log) {
        fprintf(fps.log,"exit frames=%u ticks=%u dropped=%u\n",fps.frames,fps.ticks,fps.clock.dropped);
        fclose(fps.log);
    }
    memset(&fps,0,sizeof fps);
}
static void fps_frame_begin(void) {
    uint64_t now;
    if (psp_clock_is_realtime()) {
        now=fps_now();
        if (fps.cap>0) {
            const uint64_t period=1000000000ull/(unsigned)fps.cap;
            if (fps.deadline && now<fps.deadline) { fps_sleep_until(fps.deadline); now=fps_now(); }
            /* Do not accumulate a burst of presents after a slow frame. */
            fps.deadline=(fps.deadline && now<fps.deadline+period?fps.deadline:now)+period;
        }
    } else {
        /* Explicitly enabled headless runs have a deterministic display clock.
         * Unlimited uses 60 Hz here; it has no wall-clock meaning headless. */
        unsigned hz=fps.cap>0?(unsigned)fps.cap:60;
        now=(fps.synthetic_frame++*1000000000ull+hz-1)/hz;
        if (fps.test_gap_ms && fps.synthetic_frame>fps.test_gap_frame)
            now+=(uint64_t)fps.test_gap_ms*1000000ull;
    }
    fps.ticks_left=fps_clock_step(&fps.clock,now);
    fps.inserted=!fps.ticks_left;
    fps.frames++;
    if (!fps.inserted) fps.ticks++;
}
#endif
