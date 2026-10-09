/* Shared AC3 Portable / Silent Line mission adapter. Addresses are supplied
 * by each title's replacement file. Their bone records are 256 bytes each;
 * the world matrix at +64 is consumed by the part/mesh rebuild below. */
#ifndef LR_FPS_AC3_H
#define LR_FPS_AC3_H
#include "fps_joints.h"

static struct {
    struct fps_joint pose;
    float saved[16];
    uint32_t view, tick, mode;
    int valid,borrowed;
} fps_camera;
static int fps_borrowed;
static void fps_native_rebuild(uint32_t _entry);

static uint32_t fps_ac3_view(void) {
    uint32_t context=psp_read32(FPS_VIEW_CONTEXT);
    uint32_t view=context?psp_read32(context):0;
    return view && psp_mem_ptr(view,64)?view:0;
}
static int fps_ac3_paused(void) {
    psp_cpu_state saved=psp_cpu;
    AC3_FN(FPS_STOP_QUERY)();
    int stopped=r_v0!=0 || psp_read8(FPS_PAUSE_BYTE)!=0;
    psp_cpu=saved; return stopped;
}
static void fps_ac3_history_reset(void) {
    memset(fps_joints,0,sizeof fps_joints);
    fps_camera.valid=0;
}
static void fps_ac3_begin_frame(void) {
    unsigned dropped=fps.clock.dropped;
    fps_frame_begin();
    if (fps.clock.dropped!=dropped) fps_ac3_history_reset();
}
void AC3_FN(FPS_BUILD_JOINTS)(void) {
    if (!fps.active) { AC3_ORIG(FPS_BUILD_JOINTS)(); return; }
    uint32_t model=r_a0;
    uint32_t pose=model?psp_read32(model+160):0;
    unsigned count=model?psp_read16(model+4):0;
    uint32_t block=pose?pose+64:0;
    struct fps_joint *s=fps_joint_begin(model,block,256,count);
    AC3_ORIG(FPS_BUILD_JOINTS)();
    fps_joint_end(s,block);
}
void AC3_FN(FPS_UPDATE_VIEWS)(void) {
    if (!fps.active) { AC3_ORIG(FPS_UPDATE_VIEWS)(); return; }
    uint32_t view=fps_ac3_view();
    uint32_t mode=psp_read32(FPS_CAMERA_MODE);
    int valid=view && fps_camera.tick+1==fps.ticks && view==fps_camera.view && mode==fps_camera.mode;
    if (view) for (int i=0;i<16;i++) fps_camera.pose.prev[0][i]=fps_read_float(view+4*i);
    AC3_ORIG(FPS_UPDATE_VIEWS)();
    fps_camera.valid=valid && view==fps_ac3_view() && mode==psp_read32(FPS_CAMERA_MODE);
    fps_camera.view=fps_ac3_view(); fps_camera.tick=fps.ticks; fps_camera.mode=psp_read32(FPS_CAMERA_MODE);
    fps_camera.pose.block=fps_camera.view; fps_camera.pose.count=1; fps_camera.pose.stride=64;
    if (fps_camera.view) for (int i=0;i<16;i++) fps_camera.pose.cur[0][i]=fps_read_float(fps_camera.view+4*i);
    if (fps_ac3_paused()) fps_ac3_history_reset();
}
#ifndef FPS_AC_MAX
#define FPS_AC_MAX 16u
#endif
static void fps_ac3_rebuild(void) {
    unsigned count=psp_read32(FPS_AC_COUNT);
    /* FPS_AC_MAX is each title's real array bound (host/replacements-*.c);
     * a larger count is corrupt. Every AC is also checked to be mapped before
     * the rebuild runs on it. */
    if (count>FPS_AC_MAX) count=FPS_AC_MAX;
    for (unsigned i=0;i<count;i++) {
        const uint32_t ac=AC3_PLAYER_AC+i*FPS_AC_STRIDE;
        if (!psp_mem_ptr(ac,FPS_AC_STRIDE)) break;
        r_a0=ac;
        fps_native_rebuild(0);
    }
}
static void fps_ac3_publish_view(void) {
    r_a0=psp_read32(FPS_VIEW_CONTEXT); AC3_FN(FPS_PUBLISH_VIEW)();
}
static void fps_ac3_render_open(void) {
    if (!fps.interpolate || fps.clock.alpha>=0.999999f) return;
    psp_cpu_state saved=psp_cpu;
    fps_borrowed=fps.interpolate;
    if (fps.interpolate&1) { fps_phase=1; fps_ac3_rebuild(); fps_phase=0; }
    if ((fps.interpolate&2) && fps_camera.valid && fps_camera.view==fps_ac3_view()) {
        for (int i=0;i<16;i++) fps_camera.saved[i]=fps_read_float(fps_camera.view+4*i);
        fps_lerp_joints(&fps_camera.pose);
        fps_ac3_publish_view(); fps_camera.borrowed=1;
    }
    psp_cpu=saved;
}
static void fps_ac3_render_close(void) {
    if (!fps_borrowed) return;
    psp_cpu_state saved=psp_cpu;
    if (fps_borrowed&1) { fps_phase=2; fps_ac3_rebuild(); fps_phase=0; }
    if (fps_camera.borrowed) {
        for (int i=0;i<16;i++) fps_write_float(fps_camera.view+4*i,fps_camera.saved[i]);
        fps_ac3_publish_view(); fps_camera.borrowed=0;
    }
    fps_borrowed=0; psp_cpu=saved;
}
static void fps_ac3_frame_log(void) {
    if (fps.log) fprintf(fps.log,"frame %u poll=%u ticks=%u inserted=%u alpha=%.6f tick=%u timer=%u pos=%.9g,%.9g yaw=%.9g ns=%llu dropped=%u\n",
        fps.frames,psp_ctrl_polls(),fps.ticks,fps.inserted,fps.clock.alpha,
        psp_read32(FPS_TICK),psp_read32(FPS_TIMER),
        fps_read_float(AC3_PLAYER_AC+80),fps_read_float(AC3_PLAYER_AC+88),fps_read_float(AC3_PLAYER_AC+100),
        (unsigned long long)fps.clock.last_ns,fps.clock.dropped);
}
void AC3_FN(FPS_PACER)(void) {
    if (!fps.active) AC3_ORIG(FPS_PACER)();
}
#include <fps_loop.inc>
void AC3_FN(FPS_MISSION_LOOP)(void) {
    if (!fps_start()) { AC3_ORIG(FPS_MISSION_LOOP)(); return; }
    fps_ac3_history_reset(); fps_phase=0; fps_borrowed=0;
    memset(&fps_camera,0,sizeof fps_camera);
    fps_native_loop(0); /* The generated switch's default is the function entry. */
    fps_ac3_render_close(); fps_ac3_history_reset(); fps_stop();
}
/* A thread a save state restored inside the mission loop: the loop it was
 * running, native or the original, from the return site it was at. */
#define FPS_LOOP_ADDR AC3_CAT(0x, AC3_CAT(FPS_MISSION_LOOP, u))
static void fps_resume(uint32_t site) {
    psp_nest_enter(PSP_NEST_REPLACED,FPS_LOOP_ADDR);
    if (!fps.active) AC3_CAT(psp_resume_, FPS_MISSION_LOOP)(site);
    else {
        fps_native_loop(site);
        fps_ac3_render_close(); fps_ac3_history_reset(); fps_stop();
    }
    psp_nest_leave();
}
static void fps_ac3_keep(void) {
    fps_keep();
    PSP_STATE_KEEP(fps_camera);
    PSP_STATE_KEEP(fps_borrowed);
    PSP_STATE_KEEP(fps_joints);
    PSP_STATE_KEEP(fps_phase);
    psp_resume_override(FPS_LOOP_ADDR,AC3_CAT(psp_resume_, FPS_MISSION_LOOP),fps_resume);
}
#endif
