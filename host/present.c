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

static SDL_AudioDeviceID g_audio_dev;
static int64_t  g_audio_bytes_per_us_hi;   /* bytes per microsecond, scaled 2^20 */
static uint32_t g_audio_target_bytes;      /* keep the queue near this depth */
static int16_t *g_audio_scratch;
static uint32_t g_audio_scratch_samples;
static int      g_audio_warned_fmt;

/* The audio hook. Runs on the guest audio thread, inside the output call:
 * the buffer is complete -- the game filled it before calling -- so read it
 * out and queue it. Returns the backlog in microseconds for the blocking
 * calls to pay. */
static int64_t present_audio(int ch, uint32_t samples, uint32_t fmt,
                             uint32_t buf) {
    (void)ch;
    if (!g_audio_dev || !samples) return 0;

    /* PSP audio output is interleaved stereo S16 at 44100Hz; the format
     * argument distinguishes bit depths in the reserve call, and anything
     * unusual is reported once rather than misplayed forever. */
    if (fmt != 0x10 && !g_audio_warned_fmt++) {
        fprintf(stderr, "present: channel format 0x%X is not 16-bit stereo; "
                        "queueing it as if it were\n", fmt);
    }

    if (samples > g_audio_scratch_samples) {
        free(g_audio_scratch);
        g_audio_scratch = malloc(sizeof(int16_t) * samples * 2);
        if (!g_audio_scratch) { g_audio_scratch_samples = 0; return 0; }
        g_audio_scratch_samples = samples;
    }
    for (uint32_t i = 0; i < samples * 2; i++)
        g_audio_scratch[i] = (int16_t)psp_read16(buf + i * 2u);
    SDL_QueueAudio(g_audio_dev, g_audio_scratch,
                   (uint32_t)samples * 2u * sizeof(int16_t));

    const int64_t queued = (int64_t)SDL_GetQueuedAudioSize(g_audio_dev);
    const int64_t over   = queued - (int64_t)g_audio_target_bytes;
    if (over <= 0) return 0;
    /* bytes_per_us = 44100 * 4 / 1e6, scaled 2^20 to stay in integers. */
    const int64_t us = over * g_audio_bytes_per_us_hi >> 20;
    return us > 100000 ? 100000 : us;          /* never claim more than 100ms */
}

/* ---- the SDL thread -------------------------------------------------------- */

static void *sdl_thread(void *arg) {
    (void)arg;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "present: SDL_Init: %s -- running headless\n",
                SDL_GetError());
        return NULL;
    }

    SDL_Window *win = SDL_CreateWindow(
        "Armored Core: Last Raven -- recompiled",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        SCREEN_W * 2, SCREEN_H * 2,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *ren = win ?
        SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                    SDL_RENDERER_PRESENTVSYNC) : NULL;
    if (ren) SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    /* The window is resizable, so pin the aspect: SDL letterboxes 480x272
     * inside whatever the user drags it to rather than stretching it. */
    if (ren) SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    SDL_Texture *tex = ren ?
        SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                          SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H) : NULL;
    if (!tex) fprintf(stderr, "present: no window/renderer -- frames go nowhere\n");

    SDL_AudioSpec want = { 0 };
    want.freq     = 44100;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 1024;
    SDL_AudioSpec have;
    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev) SDL_PauseAudioDevice(g_audio_dev, 0);
    else fprintf(stderr, "present: no audio device: %s\n", SDL_GetError());

    fprintf(stderr, "present: keys arrows dpad | z cross, x circle, "
                    "a square, s triangle | q/e shoulders | enter start | "
                    "backspace select | close window to stop\n");

    for (;;) {
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
    psp_display_set_present(present_frame);
    psp_audio_set_output(present_audio);

    /* Queue depth target: two buffers -- deep enough that jitter never
     * underruns, shallow enough that the backlog tracks real playback. */
    g_audio_bytes_per_us_hi = ((int64_t)44100 * 4 << 20) / 1000000;
    g_audio_target_bytes    = (uint32_t)(44100 * 4 * 2 * 2);

    if (pthread_cond_init(&g_frame_cv, NULL) != 0) return -1;
    if (pthread_create(&g_thread, NULL, sdl_thread, NULL) != 0) {
        fprintf(stderr, "present: cannot start the SDL thread\n");
        return -1;
    }
    return 0;
}
