/* Shared skeletal render interpolation; game layouts are supplied by each adapter. */
#ifndef LR_FPS_JOINTS_H
#define LR_FPS_JOINTS_H
#include "fps.h"
enum { FPS_MODELS=32, FPS_JOINTS=128 };
static struct fps_joint {
    uint32_t model, block, tick, stride;
    int count, valid;
    float prev[FPS_JOINTS][16],cur[FPS_JOINTS][16];
} fps_joints[FPS_MODELS];
static int fps_phase;

static float fps_read_float(uint32_t p) {
    union { float f; uint32_t u; } v={.u=psp_read32(p)}; return v.f;
}
static void fps_write_float(uint32_t p,float f) {
    union { float f; uint32_t u; } v={.f=f}; psp_write32(p,v.u);
}
static float fps_dot(const float *a,const float *b) {
    return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
}
static void fps_joint_restore(struct fps_joint *s) {
    for (int k=0;k<s->count;k++) for (int i=0;i<16;i++)
        fps_write_float(s->block+s->stride*k+4*i,s->cur[k][i]);
}
static void fps_lerp_joints(struct fps_joint *s) {
    float result[FPS_JOINTS][16];
    /* Invalid history leaves the authoritative pose intact, including when
     * the caller has just sampled animation ahead of the last real tick. */
    fps_joint_restore(s);
    for (int k=0;k<s->count;k++) {
        float cur[16],*out=result[k];
        for (int i=0;i<16;i++) {
            cur[i]=s->cur[k][i];
            out[i]=s->prev[k][i]+fps.clock.alpha*(cur[i]-s->prev[k][i]);
        }
        for (int i=0;i<16;i++) if (!isfinite(cur[i]) || !isfinite(out[i])) return;
        /* A teleported root is a discontinuity, not a pose to blend through. */
        if (!k) for (int i=12;i<15;i++) if (fabsf(cur[i]-s->prev[k][i])>80) return;
        /* Preserve rotation row lengths and orthogonality through the lerp. */
        for (int a=0;a<3;a++) {
            float *r=out+4*a;
            float prev_len=sqrtf(fps_dot(s->prev[k]+4*a,s->prev[k]+4*a));
            float cur_len=sqrtf(fps_dot(cur+4*a,cur+4*a));
            float want=prev_len+fps.clock.alpha*(cur_len-prev_len);
            for (int b=0;b<a;b++) {
                float *q=out+4*b, d=fps_dot(q,q);
                if (d>1e-12f) { float scale=fps_dot(r,q)/d; for (int j=0;j<3;j++) r[j]-=scale*q[j]; }
            }
            float len=sqrtf(fps_dot(r,r));
            if (len>1e-12f) for (int j=0;j<3;j++) r[j]*=want/len;
            else for (int j=0;j<3;j++) r[j]=cur[4*a+j];
        }
    }
    for (int k=0;k<s->count;k++) for (int i=0;i<16;i++)
        if (i%4!=3 && isfinite(result[k][i]))
            fps_write_float(s->block+s->stride*k+4*i,result[k][i]);
}

/* Record each model once per authoritative tick. The title supplies its
 * matrix layout: Last Raven packs matrices, AC3/SL use 256-byte bone records. */
static struct fps_joint *fps_joint_begin(uint32_t model,uint32_t block,
                                         uint32_t stride,unsigned limit) {
    struct fps_joint *s=NULL;
    if (!block || !limit || limit>FPS_JOINTS) return NULL;
    if (!psp_mem_ptr(block,(limit-1)*stride+64)) return NULL;
    for (int i=0;i<FPS_MODELS;i++) if (fps_joints[i].model==model) { s=&fps_joints[i]; break; }
    if (!s && !fps_phase) for (int i=0;i<FPS_MODELS;i++)
        if (!fps_joints[i].model || fps_joints[i].tick+1<fps.ticks) {
            s=&fps_joints[i]; memset(s,0,sizeof *s); s->model=model; break;
        }
    if (s && !fps_phase && s->tick!=fps.ticks) {
        int old_count=s->count;
        s->valid=s->tick+1==fps.ticks && s->block==block && s->stride==stride;
        s->block=block; s->stride=stride; s->tick=fps.ticks; s->count=0;
        for (unsigned k=0;k<limit;k++) {
            uint32_t p=block+stride*k;
            if (psp_read32(p+12) || psp_read32(p+28) || psp_read32(p+44) || psp_read32(p+60)!=0x3f800000u) break;
            int finite=1;
            for (int i=0;i<16;i++) { s->prev[k][i]=fps_read_float(p+4*i); finite &= isfinite(s->prev[k][i]); }
            if (!finite) break;
            s->count++;
        }
        if (s->count!=old_count) s->valid=0;
    }
    return s;
}
static void fps_joint_end(struct fps_joint *s,uint32_t block) {
    if (!s || s->tick!=fps.ticks || s->block!=block) return;
    if (!fps_phase) {
        for (int k=0;k<s->count;k++) for (int i=0;i<16;i++)
            s->cur[k][i]=fps_read_float(block+s->stride*k+4*i);
    } else if (fps_phase==1 && s->valid) fps_lerp_joints(s);
    else {
        /* The animation sampler can advance after the authoritative rebuild.
         * Re-evaluating it here would leave next tick's pose in collision data.
         * Restore the exact matrices produced by the real tick instead. */
        fps_joint_restore(s);
    }
}
#endif
