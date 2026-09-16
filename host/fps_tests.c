#include "fps_clock.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    const unsigned rates[]={30,60,90,120,144,165,240,1000};
    for (unsigned k=0;k<sizeof rates/sizeof rates[0];k++) {
        fps_clock c={0}; unsigned ticks=0,render_only=0;
        for (unsigned frame=0;frame<=rates[k]*10;frame++) {
            unsigned n=fps_clock_step(&c,((uint64_t)frame*1000000000ull+rates[k]-1)/rates[k]);
            ticks+=n; render_only+=!n;
            assert(n<=1 && isfinite(c.alpha) && c.alpha>=0 && c.alpha<=1);
        }
        assert(ticks==301 && !c.dropped);
        assert(render_only==rates[k]*10+1-ticks);
    }
    fps_clock c={0}; assert(fps_clock_step(&c,0)==1);
    assert(fps_clock_step(&c,10000000)==0);
    assert(fps_clock_step(&c,65000000)==1);
    assert(fps_clock_step(&c,120000000)==2); /* two ticks after a 55 ms stall */
    assert(fps_clock_step(&c,10000000000ull)==FPS_MAX_CATCHUP);
    assert(c.dropped==1 && c.phase<1000000000ull);
    assert(fps_clock_step(&c,10000000001ull)==0);
    assert(fps_clock_step(&c,1)==0); /* a backwards clock never underflows */
    puts("fps: exact 30 Hz simulation at eight display rates, stalls and bounded catch-up passed");
}
