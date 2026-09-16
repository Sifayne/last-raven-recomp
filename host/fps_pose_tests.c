/* Exercise the real interpolation code against a small guest-memory fixture. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LR_FPS_H
static struct { struct { float alpha; } clock; unsigned ticks; } fps;
static unsigned char ram[65536];
static void *psp_mem_ptr(uint32_t p,uint32_t n) {
    return p<=sizeof ram && n<=sizeof ram-p?ram+p:NULL;
}
static uint32_t psp_read32(uint32_t p) {
    uint32_t v; assert(psp_mem_ptr(p,4)); memcpy(&v,ram+p,4); return v;
}
static void psp_write32(uint32_t p,uint32_t v) {
    assert(psp_mem_ptr(p,4)); memcpy(ram+p,&v,4);
}
#include "fps_joints.h"
static void pose(uint32_t p,float x,float angle) {
    float m[16]={cosf(angle),sinf(angle),0,0,-sinf(angle),cosf(angle),0,0,0,0,1,0,x,0,0,1};
    memcpy(ram+p,m,sizeof m);
}
int main(void) {
    for (unsigned stride=64;stride<=256;stride+=192) {
        memset(fps_joints,0,sizeof fps_joints); fps_phase=0;
        pose(1024,0,0); pose(1024+stride,0,0);
        fps.ticks=1;
        struct fps_joint *s=fps_joint_begin(100,1024,stride,2);
        assert(s && s->count==2 && !s->valid); fps_joint_end(s,1024);
        fps.ticks=2; s=fps_joint_begin(100,1024,stride,2);
        pose(1024,10,1.57079632679f); pose(1024+stride,20,0);
        fps_joint_end(s,1024); assert(s->valid);
        fps.clock.alpha=0.5f; fps_phase=1;
        /* A speculative animation rebuild must not become authoritative. */
        pose(1024,99,0); fps_joint_end(s,1024);
        assert(fabsf(fps_read_float(1024+48)-5)<1e-5f);
        float m[16]; memcpy(m,ram+1024,sizeof m);
        assert(fabsf(fps_dot(m,m)-1)<1e-5f && fabsf(fps_dot(m,m+4))<1e-5f);
        assert(fabsf(fps_read_float(1024+stride+48)-10)<1e-5f);
        fps_phase=2; pose(1024,101,0); fps_joint_end(s,1024);
        assert(!memcmp(ram+1024,s->cur[0],64));
        assert(!memcmp(ram+1024+stride,s->cur[1],64));
        /* Reject a cut or non-finite history for the entire model. */
        s->prev[0][12]=-100; fps_phase=1; fps_joint_end(s,1024);
        assert(!memcmp(ram+1024,s->cur[0],64));
        s->prev[0][12]=0; s->prev[1][0]=NAN; fps_joint_end(s,1024);
        assert(!memcmp(ram+1024,s->cur[0],64));
        fps_phase=0; fps.ticks=4;
        assert(!fps_joint_begin(100,1024,stride,2)->valid);
        assert(!fps_joint_begin(100,65530,stride,2));
    }
    puts("fps: interpolated poses, exact restoration, cuts and invalid history passed");
}
