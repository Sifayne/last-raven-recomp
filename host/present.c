/* present — the windowed presentation layer. See present.h.
 *
 * Three streams cross between guest and host here, each in the cheapest
 * direction for its rate:
 *
 *   video   guest frame thread  ->  converted RGBA, one slot, mutex   (~60/s)
 *   input   SDL event thread    ->  atomics read by sceCtrl            (on change)
 *   audio   guest audio thread  ->  SDL_QueueAudio, real-time drain    (~100/s)
 *
 * The one known dishonesty is audio mixing: channels play back sequentially
 * rather than mixed, because SDL_QueueAudio is a single stream. This game's
 * intro uses one channel; a proper mixer is the cost of a callback and a
 * ring, deferred until a second channel is actually heard colliding with the
 * first. */

#include "present.h"

#include "psprecomp/clock.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/sched.h"

#include <SDL2/SDL.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { SCREEN_W = 480, SCREEN_H = 272 };

/* ---- frames -------------------------------------------------------------- */

static pthread_t      g_thread;
static pthread_mutex_t g_frame_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frame_cv;
static uint32_t        g_frame_rgba[SCREEN_W * SCREEN_H];
static int             g_frame_fresh;
/* The SDL thread's own copy, so the upload happens outside g_frame_lock: the
 * publishing thread holds the scheduler token, and must never wait on a GPU. */
static uint32_t        g_frame_present[SCREEN_W * SCREEN_H];
static _Atomic int     g_quit;

/* The display hook. Runs on a guest thread holding the scheduler token, so
 * convert and publish, and get out -- the ~130k reads take well under a
 * millisecond, and the lock is held only across that. */
static void present_frame(uint32_t addr, uint32_t stride, uint32_t fmt) {
    /* The window is gone and the run is being torn down; there is nobody to
     * hand a frame to, and the guest thread should not pay for converting one. */
    if (atomic_load(&g_quit)) return;

    /* Bring-up diagnostics: the difference between "the window is black"
     * because nothing was ever published and because SDL failed to paint it
     * is the first thing to know, and neither leaves another trace. */
    static int published;
    if (!published++)
        fprintf(stderr, "present: first frame addr=0x%08X stride=%u fmt=%u\n",
                addr, stride, fmt);
    else if (published % 60 == 0)
        fprintf(stderr, "present: %d frames (last addr=0x%08X)\n", published, addr);

    pthread_mutex_lock(&g_frame_lock);

    /* `stride` is sceDisplaySetFrameBuf's bufferwidth, which is in *pixels*
     * (512 for a 480-wide panel) -- so a row step is stride * bytes-per-pixel.
     * Using it as a byte pitch advanced 512 bytes instead of 2048 and sheared
     * the frame: the unwritten 480..511 stride padding walked across the
     * picture as three black bands, and only the top 68 scanlines were ever
     * read. Nothing faults when you do this, which is why it read as a
     * rasterizer fault. The bpp split matches the switch below, where anything
     * that is not 565/5551/4444 is 8888. */
    const uint32_t bpp = (fmt <= 2) ? 2u : 4u;

    for (int y = 0; y < SCREEN_H; y++) {
        uint32_t *out = g_frame_rgba + (size_t)y * SCREEN_W;
        const uint32_t row = addr + (uint32_t)y * stride * bpp;
        for (int x = 0; x < SCREEN_W; x++) {
            uint32_t r, g, b, a = 255;
            switch (fmt) {
            case 0:                                     /* 565 */
                r = psp_read16(row + x * 2u);
                g = (r >> 5) & 0x3F;  b = (r >> 11) & 0x1F;  r &= 0x1F;
                out[x] = 0xFF000000u | (b << 3 | b >> 2) << 16 |
                         (g << 2 | g >> 4) << 8 | (r << 3 | r >> 2);
                continue;
            case 1:                                     /* 5551 */
                r = psp_read16(row + x * 2u);
                g = (r >> 5) & 0x1F;  b = (r >> 10) & 0x1F;
                a = (r & 0x8000) ? 255 : 0;  r &= 0x1F;
                out[x] = a << 24 | (b << 3 | b >> 2) << 16 |
                         (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
                continue;
            case 2:                                     /* 4444 */
                r = psp_read16(row + x * 2u);
                g = (r >> 4) & 0xF;  b = (r >> 8) & 0xF;  a = (r >> 12) & 0xF;
                r &= 0xF;
                out[x] = (a << 4 | a) << 24 | (b << 4 | b) << 16 |
                         (g << 4 | g) << 8 | (r << 4 | r);
                continue;
            default:                                    /* 8888 */
                /* Guest RGBA8888 and SDL ABGR8888 are the same bytes in the
                 * same order on little-endian: R G B A in memory, alpha in
                 * the high byte of the word. */
                out[x] = psp_read32(row + x * 4u);
                continue;
            }
        }
    }

    g_frame_fresh = 1;
    pthread_cond_signal(&g_frame_cv);
    pthread_mutex_unlock(&g_frame_lock);
}

/* ---- input ---------------------------------------------------------------- */

static _Atomic uint32_t g_pad_buttons;
static _Atomic uint8_t  g_pad_ax = 128, g_pad_ay = 128;

/* SDL key -> PSP button. Headless mode holds buttons for a whole run; here a
 * button is held exactly as long as its key is. */
static const struct { SDL_Keycode key; uint32_t bit; } KEYS[] = {
    { SDLK_RIGHT,      0x000020 }, { SDLK_LEFT,      0x000080 },
    { SDLK_DOWN,       0x000040 }, { SDLK_UP,        0x000010 },
    { SDLK_RETURN,     0x000008 }, { SDLK_BACKSPACE, 0x000001 },
    { SDLK_z,          0x004000 }, { SDLK_x,         0x002000 },
    { SDLK_a,          0x008000 }, { SDLK_s,         0x001000 },
    { SDLK_q,          0x000100 }, { SDLK_e,         0x000200 },
};

static void set_key(SDL_Keycode k, int down) {
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) {
        if (KEYS[i].key != k) continue;
        const uint32_t b = atomic_load(&g_pad_buttons);
        atomic_store(&g_pad_buttons, down ? (b | KEYS[i].bit)
                                          : (b & ~KEYS[i].bit));
        return;
    }
}

static void set_button(uint8_t b, int down) {
    static const struct { uint8_t sdl; uint32_t psp; } MAP[] = {
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,          0x000020 },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT,           0x000080 },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN,           0x000040 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP,             0x000010 },
        { SDL_CONTROLLER_BUTTON_START,               0x000008 },
        { SDL_CONTROLLER_BUTTON_BACK,                0x000001 },
        { SDL_CONTROLLER_BUTTON_A,                   0x004000 },
        { SDL_CONTROLLER_BUTTON_B,                   0x002000 },
        { SDL_CONTROLLER_BUTTON_X,                   0x008000 },
        { SDL_CONTROLLER_BUTTON_Y,                   0x001000 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,        0x000100 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,       0x000200 },
    };
    for (size_t i = 0; i < sizeof MAP / sizeof *MAP; i++) {
        if (MAP[i].sdl != b) continue;
        const uint32_t v = atomic_load(&g_pad_buttons);
        atomic_store(&g_pad_buttons, down ? (v | MAP[i].psp) : (v & ~MAP[i].psp));
        return;
    }
}

static void set_axis(uint8_t axis, int16_t value) {
    if (axis != SDL_CONTROLLER_AXIS_LEFTX && axis != SDL_CONTROLLER_AXIS_LEFTY)
        return;
    /* 8192 of deadzone, then the full 0..255 range across the remaining
     * ~24k of travel, centred on 128 like the real stick. */
    const int dead = 8192;
    int v = 128;
    if (value > dead)  v = 128 + (value - dead) * 127 / (32767 - dead);
    if (value < -dead) v = 128 + (value + dead) * 127 / (32768 - dead);
    if (axis == SDL_CONTROLLER_AXIS_LEFTX) atomic_store(&g_pad_ax, (uint8_t)v);
    else                                   atomic_store(&g_pad_ay, (uint8_t)v);
}

/* Publish the whole pad state after any change, so a guest poll between two
 * events of one press never sees the press half-applied. Cheap. */
static void publish_pad(void) {
    psp_ctrl_set(atomic_load(&g_pad_buttons),
                 atomic_load(&g_pad_ax), atomic_load(&g_pad_ay));
}

/* ---- audio ---------------------------------------------------------------- */

/* A mixer, not a queue.
 *
 * This used to hand every channel's buffer to SDL_QueueAudio, which is one
 * stream: two channels playing at once -- the music on one, the SAS effects
 * on another -- came out interleaved block by block, each at the wrong
 * moment. The PSP has eight hardware channels and sums them, so this keeps a
 * ring of stereo frames per channel and the device callback sums whatever
 * each ring holds, silence for an empty one. The volumes are applied on the
 * way in, on the 0..0x8000 scale the output call was given.
 *
 * Pacing: each blocking output call is told how far its own channel is ahead
 * of the speaker, and the scheduler delays it by that. The target depth is
 * how far a channel may run ahead before it is made to wait -- short enough
 * that an effect is heard when it happens, long enough to ride out a frame
 * that rasterizes slowly. */
/* A channel does not play until it holds a pre-roll, and after running dry it
 * waits for one again. A stream the game paces itself -- the movie's sound
 * thread waits on the movie's own clock, not on our backlog -- arrives one
 * block at a time at exactly the rate it is consumed, so the ring hovers near
 * empty and every push that lands a few milliseconds late is a gap the
 * callback fills with silence: the popping Sif heard through the intro. A
 * pre-roll of 4096 frames is 93 ms of slack against that jitter, paid once as
 * latency at the start of each stream. */
enum { MIX_CHANNELS = 8, MIX_RING_FRAMES = 44100 * 8 };
typedef struct {
    int16_t *pcm;
    uint32_t head, tail, count;
    int      playing;          /* holds a pre-roll, or has not run dry since */
    uint32_t pushed, dropped, underruns, silence;
    /* Push timing, for the report: when the first and last pushes came, the
     * longest wait between two, and how many waits were longer than the
     * pre-roll -- each of those a gap the pre-roll could not cover. */
    uint64_t first_ms, last_ms, max_gap_ms;
    uint32_t long_gaps;
} mix_ring;

static SDL_AudioDeviceID g_audio_dev;
static mix_ring g_mix[MIX_CHANNELS];
/* How far a channel may run ahead of the speaker before its blocking output
 * is made to wait. Hardware's answer is two of the channel's own buffers:
 * sceAudioOutputBlocking returns when the previous buffer has finished
 * playing, so at most the one playing and the one queued are ever in
 * flight. That is also the sound-to-picture latency, and this used to be
 * half a second -- the game's movie player takes its sound thread as the
 * clock, that thread ran half a second ahead of the speaker, and the picture
 * followed the decode while the sound arrived later: Sif saw the intro out
 * of sync. Two buffers it is, but never less than the pre-roll, since a
 * channel cannot start until it holds that much. PSPRECOMP_AUDIO_LEAD_MS
 * overrides it, and PSPRECOMP_AUDIO_PREROLL_MS the pre-roll, for listening
 * without a rebuild. */
static uint32_t g_audio_target_frames  = 0;      /* 0: two of the channel's buffers */
static uint32_t g_audio_preroll_frames = 4096;
static int      g_audio_warned_fmt;

static uint32_t env_ms_frames(const char *name, uint32_t dflt_frames) {
    const char *v = getenv(name);
    if (!v || !*v) return dflt_frames;
    const long ms = strtol(v, NULL, 10);
    if (ms < 0) return dflt_frames;
    uint64_t f = (uint64_t)ms * 44100u / 1000u;
    if (f >= MIX_RING_FRAMES / 2) f = MIX_RING_FRAMES / 2;
    return (uint32_t)f;
}

/* The device callback: runs on SDL's audio thread with the device lock held,
 * which is the lock present_audio takes to push. */
static void audio_callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    const int frames = len / 4;
    for (int i = 0; i < frames; i++) {
        int32_t l = 0, r = 0;
        for (int ch = 0; ch < MIX_CHANNELS; ch++) {
            mix_ring *m = &g_mix[ch];
            if (!m->playing) {
                if (m->count < g_audio_preroll_frames) { if (m->pushed) m->silence++; continue; }
                m->playing = 1;
            } else if (!m->count) {
                m->playing = 0;
                m->underruns++;
                m->silence++;
                continue;
            }
            l += m->pcm[m->tail * 2];
            r += m->pcm[m->tail * 2 + 1];
            m->tail = (m->tail + 1) % MIX_RING_FRAMES;
            m->count--;
        }
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[i * 2] = (int16_t)l;
        out[i * 2 + 1] = (int16_t)r;
    }
}

/* The audio hook. Runs on the guest audio thread, inside the output call:
 * the buffer is complete -- the game filled it before calling -- so read it
 * out, scale it, and push it. Returns the backlog in microseconds for the
 * blocking calls to pay. */
static int64_t present_audio(int ch, uint32_t samples, uint32_t fmt,
                             uint32_t buf, uint32_t lvol, uint32_t rvol) {
    if (!g_audio_dev || !samples || ch < 0 || ch >= MIX_CHANNELS) return 0;
    mix_ring *m = &g_mix[ch];
    if (!m->pcm) {
        m->pcm = malloc(sizeof(int16_t) * 2 * MIX_RING_FRAMES);
        if (!m->pcm) return 0;
    }

    /* PSP_AUDIO_FORMAT_STEREO is 0 and MONO is 0x10; the mono form carries
     * one sample per frame and plays on both sides. Anything else in the
     * format word is reported once and treated as stereo. */
    const int mono = (fmt & 0x10) != 0;
    if ((fmt & ~0x10u) && !g_audio_warned_fmt++)
        fprintf(stderr, "present: channel format 0x%X is unfamiliar; treating it as %s\n",
                fmt, mono ? "mono" : "stereo");
    if (lvol > 0x8000) lvol = 0x8000;
    if (rvol > 0x8000) rvol = 0x8000;

    const uint64_t now = SDL_GetTicks64();
    SDL_LockAudioDevice(g_audio_dev);
    if (!m->first_ms) m->first_ms = now;
    else {
        const uint64_t gap = now - m->last_ms;
        if (gap > m->max_gap_ms) m->max_gap_ms = gap;
        if (gap * 44100u / 1000u > g_audio_preroll_frames) m->long_gaps++;
    }
    m->last_ms = now;
    for (uint32_t i = 0; i < samples; i++) {
        int32_t sl, sr;
        if (mono) { sl = sr = (int16_t)psp_read16(buf + i * 2u); }
        else      { sl = (int16_t)psp_read16(buf + i * 4u); sr = (int16_t)psp_read16(buf + i * 4u + 2u); }
        sl = (sl * (int32_t)lvol) >> 15;
        sr = (sr * (int32_t)rvol) >> 15;
        if (m->count >= MIX_RING_FRAMES) { m->dropped++; continue; }
        m->pcm[m->head * 2] = (int16_t)sl;
        m->pcm[m->head * 2 + 1] = (int16_t)sr;
        m->head = (m->head + 1) % MIX_RING_FRAMES;
        m->count++;
        m->pushed++;
    }
    const int64_t queued = m->count;
    SDL_UnlockAudioDevice(g_audio_dev);

    int64_t allow = g_audio_target_frames ? g_audio_target_frames : 2 * (int64_t)samples;
    if (allow < (int64_t)g_audio_preroll_frames) allow = g_audio_preroll_frames;
    const int64_t over = queued - allow;
    if (over <= 0) return 0;
    const int64_t us = over * 1000000 / 44100;
    return us > 100000 ? 100000 : us;          /* never claim more than 100ms */
}

/* ---- the GL handoff -------------------------------------------------------
 *
 * A GL context belongs to one thread, and it is not this one. SDL owns the
 * window and the event queue here, but display lists execute on the guest
 * thread that submitted them -- measured as exactly one thread, which is what
 * makes this arrangement possible at all (findings item 51).
 *
 * So the SDL thread creates the window and the context and then *releases*
 * the context, and the GE thread claims it once and keeps it. The handshake
 * below exists because present_start returns as soon as the thread is spawned:
 * the GE thread can reach present_gl_make_current before the window exists,
 * and has to wait rather than fail. */
static int             g_gl_want;        /* set before present_start */
static SDL_Window     *g_gl_win;
static SDL_GLContext   g_gl_ctx;
static int             g_gl_state;       /* 0 pending, 1 ready, -1 failed */
static _Atomic int     g_gl_draw_w;
static _Atomic int     g_gl_draw_h;
static pthread_mutex_t g_gl_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_gl_cv   = PTHREAD_COND_INITIALIZER;

void present_want_gl(void) { g_gl_want = 1; }

static void gl_publish(SDL_Window *win, SDL_GLContext ctx, int ok) {
    pthread_mutex_lock(&g_gl_lock);
    g_gl_win = win; g_gl_ctx = ctx; g_gl_state = ok ? 1 : -1;
    pthread_cond_broadcast(&g_gl_cv);
    pthread_mutex_unlock(&g_gl_lock);
}

int present_gl_make_current(void) {
    pthread_mutex_lock(&g_gl_lock);
    while (g_gl_state == 0) pthread_cond_wait(&g_gl_cv, &g_gl_lock);
    const int st = g_gl_state;
    SDL_Window *win = g_gl_win;
    SDL_GLContext ctx = g_gl_ctx;
    pthread_mutex_unlock(&g_gl_lock);
    if (st < 0) return -1;
    if (SDL_GL_MakeCurrent(win, ctx) != 0) {
        fprintf(stderr, "present: cannot make the GL context current: %s\n",
                SDL_GetError());
        return -1;
    }
    /* Swap on the GE thread's own schedule; the game paces itself and the
     * frame limiter is the clock's job, not the driver's. */
    SDL_GL_SetSwapInterval(0);
    return 0;
}

void present_gl_swap(void) { if (g_gl_win) SDL_GL_SwapWindow(g_gl_win); }

/* SDL owns window queries on its presentation thread.  Publish the physical
 * drawable size through atomics so the GE thread can scale its final blit
 * correctly on high-DPI displays without reaching back into SDL. */
void present_gl_drawable_size(int *w, int *h) {
    if (w) *w = atomic_load(&g_gl_draw_w);
    if (h) *h = atomic_load(&g_gl_draw_h);
}

void *present_gl_proc(const char *name) { return SDL_GL_GetProcAddress(name); }

/* ---- the SDL thread -------------------------------------------------------- */

static void *sdl_thread(void *arg) {
    (void)arg;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "present: SDL_Init: %s -- running headless\n",
                SDL_GetError());
        return NULL;
    }

    if (g_gl_want) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
    SDL_Window *win = SDL_CreateWindow(
        "Armored Core: Last Raven -- recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * 2, SCREEN_H * 2,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
        (g_gl_want ? SDL_WINDOW_OPENGL : 0));

    /* With GL the renderer, the streaming texture and the blit below are all
     * skipped: the backend draws into the window itself. Creating an
     * SDL_Renderer on the same window would fight it for the context. */
    if (g_gl_want) {
        SDL_GLContext ctx = win ? SDL_GL_CreateContext(win) : NULL;
        if (ctx) {
            /* Created here because SDL wants it on the thread that made the
             * window, released here because it has to be current on the GE
             * thread instead. */
            SDL_GL_MakeCurrent(win, NULL);
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            atomic_store(&g_gl_draw_w, dw);
            atomic_store(&g_gl_draw_h, dh);
            fprintf(stderr, "present: GL 3.3 core context created, "
                            "handed to the GE thread\n");
        } else {
            fprintf(stderr, "present: no GL 3.3 core context: %s\n",
                    SDL_GetError());
        }
        gl_publish(win, ctx, ctx != NULL);
    }

    SDL_Renderer *ren = (win && !g_gl_want) ?
        SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (ren) SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    /* The window is resizable, so pin the aspect: SDL letterboxes 480x272
     * inside whatever the user drags it to rather than stretching it. */
    if (ren) SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    SDL_Texture *tex = ren ?
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                          SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H) : NULL;
    if (!tex && !g_gl_want)
        fprintf(stderr, "present: no window/renderer -- frames go nowhere\n");

    SDL_AudioSpec want = { 0 };
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    want.callback = audio_callback;
    SDL_AudioSpec have;
    g_audio_target_frames  = env_ms_frames("PSPRECOMP_AUDIO_LEAD_MS",    g_audio_target_frames);
    g_audio_preroll_frames = env_ms_frames("PSPRECOMP_AUDIO_PREROLL_MS", g_audio_preroll_frames);
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev) SDL_PauseAudioDevice(g_audio_dev, 0);
    else fprintf(stderr, "present: no audio device: %s\n", SDL_GetError());

    fprintf(stderr, "present: keys arrows dpad | z cross, x circle, "
                    "a square, s triangle | q/e shoulders | enter start | "
                    "backspace select | close window to stop\n");

    for (;;) {
        /* Logical window pixels and GL drawable pixels differ under desktop
         * scaling.  Refresh this on the SDL thread so resize and display-scale
         * changes are reflected by the next GL presentation. */
        if (g_gl_want && win) {
            int dw = 0, dh = 0;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            atomic_store(&g_gl_draw_w, dw);
            atomic_store(&g_gl_draw_h, dh);
        }

        /* Publish the latest frame, if the guest has produced one. Waiting
         * bounded rather than forever keeps the window alive -- and showing
         * its last frame -- when the guest stalls, which it does. */
        struct timespec due;
        clock_gettime(CLOCK_REALTIME, &due);
        due.tv_nsec += 17 * 1000000;
        if (due.tv_nsec >= 1000000000) {
            due.tv_nsec -= 1000000000;
            due.tv_sec  += 1;
        }

        pthread_mutex_lock(&g_frame_lock);
        if (!g_frame_fresh)
            pthread_cond_timedwait(&g_frame_cv, &g_frame_lock, &due);
        const int fresh = g_frame_fresh;
        if (fresh) {
            memcpy(g_frame_present, g_frame_rgba, sizeof g_frame_present);
            g_frame_fresh = 0;
        }
        pthread_mutex_unlock(&g_frame_lock);

        /* Outside the lock. On a timeout there is no new frame, so the texture
         * keeps the last one and the window still repaints -- which is the
         * point of the bounded wait. */
        if (tex && fresh)
            SDL_UpdateTexture(tex, NULL, g_frame_present, SCREEN_W * 4);

        if (tex) {
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                /* What the mixer saw, per channel: frames pushed, dropped
                 * for a full ring, and the times the ring ran dry -- each
                 * of those a gap the listener heard. */
                if (g_audio_dev) {
                    SDL_LockAudioDevice(g_audio_dev);
                    for (int ch = 0; ch < MIX_CHANNELS; ch++) {
                        const mix_ring *m = &g_mix[ch];
                        if (m->pushed)
                            fprintf(stderr, "present: audio ch %d  %.1f s pushed over %.1f s  %u dropped  "
                                            "%u underruns (%.2f s of silence)  longest wait between pushes %llu ms, "
                                            "%u waits longer than the pre-roll\n",
                                    ch, m->pushed / 44100.0,
                                    (m->last_ms - m->first_ms) / 1000.0, m->dropped,
                                    m->underruns, m->silence / 44100.0,
                                    (unsigned long long)m->max_gap_ms, m->long_gaps);
                    }
                    SDL_UnlockAudioDevice(g_audio_dev);
                }
                /* Stop the run the way the host already stops one, rather than
                 * _exit(0): that killed the process mid-drain and took the
                 * whole end-of-run report with it. This is not a guest thread,
                 * so it marks the guest threads dead and wakes the main
                 * context in psp_sched_drain, which then reports "stopped by
                 * the host (window closed)" and prints the summary. */
                atomic_store(&g_quit, 1);
                psp_sched_stop_all("window closed");
                return NULL;
            case SDL_CONTROLLERDEVICEADDED:
                SDL_GameControllerOpen(e.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_GameController *c = SDL_GameControllerFromInstanceID(
                    e.cdevice.which);
                if (c) SDL_GameControllerClose(c);
                break;
            }
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                set_button(e.cbutton.button, e.cbutton.state);
                publish_pad();
                break;
            case SDL_CONTROLLERAXISMOTION:
                set_axis(e.caxis.axis, e.caxis.value);
                publish_pad();
                break;
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                set_key(e.key.keysym.sym, e.key.state == SDL_PRESSED);
                publish_pad();
                break;
            }
        }
    }
}

int present_start(void) {
    psp_clock_realtime(1);
    /* Under GL the backend owns the window and swaps for itself, so the
     * guest-thread conversion this hook does -- 130k pixels into RGBA, every
     * frame -- would be work whose result nothing reads. */
    if (!g_gl_want) psp_display_set_present(present_frame);
    psp_audio_set_output(present_audio);

    /* Queue depth target: two buffers -- deep enough that jitter never
     * underruns, shallow enough that the backlog tracks real playback. */

    if (pthread_cond_init(&g_frame_cv, NULL) != 0) return -1;
    if (pthread_create(&g_thread, NULL, sdl_thread, NULL) != 0) {
        fprintf(stderr, "present: cannot start the SDL thread\n");
        return -1;
    }
    return 0;
}
