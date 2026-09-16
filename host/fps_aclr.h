/* NPUH10024 mission adapter. See docs/FPS.md for the loop boundaries.
 * Interpolation borrows only render state and restores it before simulation.
 * All extra guest calls preserve the complete CPU register file. */
#ifndef LR_FPS_ACLR_H
#define LR_FPS_ACLR_H
#include "fps.h"

#include "fps_joints.h"
static void fps_history_reset(void);
static void fps_native_hud(uint32_t _entry);
void psp_func_0022E800(void) {
    if (!fps.active) psp_func_0022E800__orig();
    else fps_native_hud(0);
}
void psp_func_00045DAC(void) {
    if (!fps.active) { psp_func_00045DAC__orig(); return; }
    uint32_t model=r_a0, skel=model?psp_read32(model+120):0;
    uint32_t block=skel?psp_read32(skel+96):0;
    struct fps_joint *s=fps_joint_begin(model,block,64,32);
    psp_func_00045DAC__orig();
    fps_joint_end(s,block);
}

enum { FPS_CAMERA=0x0043C080u };
static struct {
    float prev[11],cur[11],saved[11];
    uint8_t mode;
    unsigned tick;
    int valid,borrowed;
} fps_camera;
static const uint32_t fps_camera_offsets[]={16,20,24,48,52,56,32,36,240,244,248};
static void fps_camera_read(float *out) {
    for (int i=0;i<11;i++) out[i]=fps_read_float(FPS_CAMERA+fps_camera_offsets[i]);
}
static void fps_camera_write(const float *in) {
    for (int i=0;i<11;i++) fps_write_float(FPS_CAMERA+fps_camera_offsets[i],in[i]);
}
static void fps_camera_rebuild(void) {
    r_a0=FPS_CAMERA; psp_func_00074FF8();
    r_a0=psp_read32(0x00307B38u); psp_func_00004410();
}
void psp_func_000FF280(void) {
    if (!fps.active) { psp_func_000FF280__orig(); return; }
    fps_camera_read(fps_camera.prev);
    uint8_t mode=psp_read8(FPS_CAMERA);
    psp_func_000FF280__orig();
    fps_camera_read(fps_camera.cur);
    /* A pair is only worth blending when it spans consecutive ticks, as the
     * AC3 adapter requires; a skipped update would otherwise lerp stale poses. */
    fps_camera.valid=mode==psp_read8(FPS_CAMERA) && fps_camera.tick+1==fps.ticks;
    fps_camera.tick=fps.ticks;
    for (int i=0;i<11;i++) {
        if (!isfinite(fps_camera.prev[i]) || !isfinite(fps_camera.cur[i])) fps_camera.valid=0;
        if (i<6 && fabsf(fps_camera.prev[i]-fps_camera.cur[i])>50) fps_camera.valid=0;
    }
    /* Never blend through a pause or a load boundary. */
    if (psp_read8(0x0042D30Bu) || psp_read32(0x003179A0u)) fps_history_reset();
}
static void fps_history_reset(void) {
    memset(fps_joints,0,sizeof fps_joints); fps_camera.valid=0;
}
static void fps_aclr_begin_frame(void) {
    unsigned dropped=fps.clock.dropped;
    fps_frame_begin();
    if (fps.clock.dropped!=dropped) fps_history_reset();
}
static int fps_borrowed;
static void fps_native_rebuild(uint32_t _entry);
static void fps_aclr_rebuild(void) {
    uint32_t table=psp_read32(0x0042D6B0u);
    unsigned count=table?psp_read8(table+8):0;
    /* The chase camera struct sits 6.14 strides past AC[0], so the array holds
     * at most six; a larger count is corrupt, not a bigger mission. */
    if (count>6) count=6;
    for (unsigned i=0;i<count;i++) {
        const uint32_t ac=0x0042D6C0u+i*9744;
        if (!psp_mem_ptr(ac,9744)) break;
        r_a0=ac; r_a1=0; fps_native_rebuild(0);
    }
}
static void fps_aclr_render_open(void) {
    if (!fps.interpolate || fps.clock.alpha>=0.999999f) return;
    psp_cpu_state saved=psp_cpu;
    if (fps.interpolate&1) { fps_phase=1; fps_aclr_rebuild(); fps_phase=0; }
    fps_borrowed=fps.interpolate;
    if ((fps.interpolate&2) && fps_camera.valid) {
        float out[11]; fps_camera_read(fps_camera.saved);
        for (int i=0;i<11;i++) {
            float delta=fps_camera.cur[i]-fps_camera.prev[i];
            if (i==6 || i==7) delta=remainderf(delta,6.28318530718f);
            out[i]=fps_camera.prev[i]+fps.clock.alpha*delta;
        }
        fps_camera_write(out); fps_camera_rebuild(); fps_camera.borrowed=1;
    }
    psp_cpu=saved;
}
static void fps_aclr_render_close(void) {
    if (!fps_borrowed) return;
    psp_cpu_state saved=psp_cpu;
    if (fps_borrowed&1) { fps_phase=2; fps_aclr_rebuild(); fps_phase=0; }
    if (fps_camera.borrowed) {
        fps_camera_write(fps_camera.saved); fps_camera_rebuild(); fps_camera.borrowed=0;
    }
    fps_borrowed=0; psp_cpu=saved;
}
static void fps_aclr_catchup_updates(void) {
    /* The HUD's update runs after world emission in the original loop. Its
     * lifecycle changes lock-on state, so catch-up must advance it as well. */
    if (!psp_read8(0x0043D880u+27)) return;
    psp_cpu_state saved=psp_cpu;
    r_a0=psp_read8(0x0042D30Bu)!=0 || psp_read32(0x003179A0u)!=0;
    fps_native_hud(0);
    psp_cpu=saved;
}
static void fps_aclr_frame_log(void) {
    if (fps.log) fprintf(fps.log,"frame %u poll=%u ticks=%u inserted=%u alpha=%.6f tick=%u timer=%u pos=%.9g,%.9g yaw=%.9g ns=%llu dropped=%u\n",
        fps.frames,psp_ctrl_polls(),fps.ticks,fps.inserted,fps.clock.alpha,
        psp_read32(0x0030F008u),psp_read32(0x00448780u),
        fps_read_float(0x0042D6D0u),fps_read_float(0x0042D6D8u),fps_read_float(0x0042D6E4u),
        (unsigned long long)fps.clock.last_ns,fps.clock.dropped);
}
void psp_func_00259250(void) {
    if (!fps.active) psp_func_00259250__orig();
}
#include <fps_loop.inc>
void psp_func_00102018(void) {
    if (!fps_start()) { psp_func_00102018__orig(); return; }
    fps_history_reset(); fps_phase=0; fps_borrowed=0;
    memset(&fps_camera,0,sizeof fps_camera);
    fps_native_loop(0x00102018u);
    fps_aclr_render_close(); fps_history_reset(); fps_stop();
}
#endif
