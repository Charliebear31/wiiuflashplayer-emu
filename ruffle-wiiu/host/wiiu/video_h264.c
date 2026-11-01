// H.264 video through the console's own decoder (h264.rpl). Frames come out as NV12 and go into a texture as RGBA.
#include "../rw_core.h"
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <h264/decode.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DECODERS 8

typedef struct {
    int used;
    void* mem;
    uint8_t* fb;
    uint32_t fb_size;
    uint8_t* stream;
    uint32_t stream_size;
    uint8_t* rgba;
    uint32_t w, h;
    // the last frame the decoder gave
    uint8_t* out;
    int out_w, out_h, out_line;
} Decoder;

static Decoder g_dec[MAX_DECODERS];

// Decoding runs on a thread of its own, on another core, and the game only waits so long for it: a decoder that
// never comes back must cost the video, not freeze the game. (Cemu 2.6 doesn't return from H264DECExecute.)
static OSThread g_worker __attribute__((aligned(16)));
static uint8_t g_worker_stack[64 * 1024] __attribute__((aligned(16)));
static int g_worker_started;
static volatile Decoder* g_job;
static volatile uint32_t g_job_len;
static volatile int g_job_error, g_job_busy, g_decoder_stuck;

static int worker_main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    for (;;) {
        Decoder* d = (Decoder*)g_job;
        if (!d) {
            OSSleepTicks(OSMicrosecondsToTicks(200));
            continue;
        }
        H264Error e = H264DECSetBitstream(d->mem, d->stream, g_job_len, 0.0);
        if (e == H264_ERROR_OK) e = H264DECExecute(d->mem, d->fb);
        g_job_error = (int)e;
        g_job = NULL;
        __sync_synchronize();
        g_job_busy = 0;
    }
    return 0;
}

static H264Error run_decode(Decoder* d, uint32_t len) {
    if (g_decoder_stuck) return H264_ERROR_GENERIC;
    if (!g_worker_started) {
        if (!OSCreateThread(&g_worker, worker_main, 0, NULL, g_worker_stack + sizeof(g_worker_stack), sizeof(g_worker_stack), 15,
                            OS_THREAD_ATTRIB_AFFINITY_CPU2 | OS_THREAD_ATTRIB_DETACHED))
            return H264_ERROR_GENERIC;
        OSResumeThread(&g_worker);
        g_worker_started = 1;
    }
    g_job_len = len;
    g_job_busy = 1;
    __sync_synchronize();
    g_job = d;
    for (uint64_t waited = 0; g_job_busy; waited++) {
        OSSleepTicks(OSMicrosecondsToTicks(250));
        if (waited > 8000) { // 2 s
            g_decoder_stuck = 1;
            plat_log(0, "H.264: the decoder isn't responding, so this game's H.264 video won't play");
            return H264_ERROR_GENERIC;
        }
    }
    return (H264Error)g_job_error;
}

static void on_output(H264DecodeOutput* output) {
    Decoder* d = (Decoder*)output->userMemory; // (SetParam took a pointer to this, but the value itself comes back)
    if (output->frameCount <= 0 || !d) return;
    H264DecodeResult* r = output->decodeResults[output->frameCount - 1];
    d->out = (uint8_t*)r->framebuffer;
    d->out_w = r->width;
    d->out_h = r->height;
    d->out_line = r->nextLine;
}

static void close_decoder(Decoder* d) {
    if (!d->used) return;
    if (g_decoder_stuck) { // it may still be using its memory
        memset(d, 0, sizeof(*d));
        return;
    }
    H264DECFlush(d->mem);
    H264DECEnd(d->mem);
    H264DECClose(d->mem);
    free(d->mem);
    free(d->fb);
    free(d->stream);
    free(d->rgba);
    memset(d, 0, sizeof(*d));
}

// The decoder's own memory has to pass the system's check, so a block that fails is held while the next one is taken.
static void* alloc_decoder_memory(uint32_t size) {
    void* refused[4];
    int nrefused = 0;
    void* p = memalign(0x100, size);
    while (p && H264DECCheckMemSegmentation(p, size) != H264_ERROR_OK) {
        if (nrefused == 4) {
            free(p);
            p = NULL;
            break;
        }
        refused[nrefused++] = p;
        p = memalign(0x100, size);
    }
    while (nrefused) free(refused[--nrefused]);
    return p;
}

void gfx_h264_close(uint32_t id) {
    if (id && id <= MAX_DECODERS) close_decoder(&g_dec[id - 1]);
}

void gfx_h264_reset(void) {
    for (int i = 0; i < MAX_DECODERS; i++) close_decoder(&g_dec[i]);
}

uint32_t gfx_h264_open(uint32_t w, uint32_t h) {
    uint32_t id = 0;
    if (g_decoder_stuck) return 0;
    for (uint32_t i = 0; i < MAX_DECODERS; i++)
        if (!g_dec[i].used) {
            id = i + 1;
            break;
        }
    if (!id) return 0;
    Decoder* d = &g_dec[id - 1];
    uint32_t aw = (w + 15) & ~15u, ah = (h + 15) & ~15u, size = 0;
    // (Level 4.2 covers 1080p. The memory comes from malloc: the system heap has little left, and the console
    // ran out there on a 256x192 video.)
    H264Error e = H264DECMemoryRequirement(100, 42, (int32_t)aw, (int32_t)ah, &size);
    char m[120];
    if (e != H264_ERROR_OK || !size) {
        snprintf(m, sizeof(m), "H.264: no decoder for %ux%u (error %x)", (unsigned)w, (unsigned)h, (unsigned)e);
        plat_log(1, m);
        return 0;
    }
    uint32_t pitch = (aw + 255) & ~255u;
    d->fb_size = pitch * ah * 3 / 2;
    d->mem = alloc_decoder_memory(size);
    d->fb = (uint8_t*)memalign(0x100, d->fb_size);
    d->rgba = (uint8_t*)malloc((size_t)w * h * 4);
    if (!d->mem || !d->fb || !d->rgba) {
        snprintf(m, sizeof(m), "H.264: out of memory for %ux%u (decoder %u KB%s, picture %u KB%s)", (unsigned)w, (unsigned)h,
                 (unsigned)(size >> 10), d->mem ? "" : " failed", (unsigned)(d->fb_size >> 10), d->fb ? "" : " failed");
        plat_log(1, m);
        free(d->mem);
        free(d->fb);
        free(d->rgba);
        memset(d, 0, sizeof(*d));
        return 0;
    }
    snprintf(m, sizeof(m), "H.264: decoder for %ux%u, %u KB", (unsigned)w, (unsigned)h, (unsigned)(size >> 10));
    plat_log(2, m);
    OSSleepTicks(OSMillisecondsToTicks(100)); // lets the log thread save that, in case the decoder never comes back
    d->w = w;
    d->h = h;
    static Decoder* user[MAX_DECODERS];
    user[id - 1] = d;
    e = H264DECInitParam((int32_t)size, d->mem);
    if (e == H264_ERROR_OK) e = H264DECSetParam_FPTR_OUTPUT(d->mem, on_output);
    if (e == H264_ERROR_OK) e = H264DECSetParam_OUTPUT_PER_FRAME(d->mem, 1);
    if (e == H264_ERROR_OK) e = H264DECSetParam_USER_MEMORY(d->mem, (void**)&user[id - 1]);
    if (e == H264_ERROR_OK) e = H264DECOpen(d->mem);
    if (e == H264_ERROR_OK) e = H264DECBegin(d->mem);
    d->used = 1;
    if (e != H264_ERROR_OK) {
        snprintf(m, sizeof(m), "H.264: decoder didn't start (error %x)", (unsigned)e);
        plat_log(1, m);
        close_decoder(d);
        return 0;
    }
    return id;
}

static uint8_t clamp8(int v) {
    return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
}

// Returns 1 if a new picture went into the texture.
int gfx_h264_decode(uint32_t id, uint32_t tex, const uint8_t* data, uint32_t len) {
    if (!id || id > MAX_DECODERS || !g_dec[id - 1].used || g_decoder_stuck) return 0;
    Decoder* d = &g_dec[id - 1];
    if (len + 16 > d->stream_size) {
        free(d->stream);
        d->stream_size = (len + 16 + 65535) & ~65535u;
        d->stream = (uint8_t*)memalign(0x100, d->stream_size);
        if (!d->stream) {
            d->stream_size = 0;
            return 0;
        }
    }
    memcpy(d->stream, data, len);
    memset(d->stream + len, 0, 16);
    DCFlushRange(d->stream, len + 16);
    d->out = NULL;
    H264Error e = run_decode(d, len);
    if (e != H264_ERROR_OK && (e & 0xffff0000u) == H264_ERROR_GENERIC) {
        static int reported;
        if (reported++ < 5) {
            char m[80];
            snprintf(m, sizeof(m), "H.264: decoding failed (error %x)", (unsigned)e);
            plat_log(1, m);
        }
    }
    if (!d->out) return 0;
    static int first_picture;
    if (!first_picture++) {
        char m[80];
        snprintf(m, sizeof(m), "H.264: first picture decoded, %dx%d", d->out_w, d->out_h);
        plat_log(2, m);
    }
    DCInvalidateRange(d->fb, d->fb_size);
    int w = d->out_w < (int)d->w ? d->out_w : (int)d->w;
    int h = d->out_h < (int)d->h ? d->out_h : (int)d->h;
    const uint8_t* yp = d->out;
    const uint8_t* uvp = d->out + (size_t)d->out_line * ((d->out_h + 15) & ~15);
    for (int y = 0; y < h; y++) {
        const uint8_t* yl = yp + (size_t)y * d->out_line;
        const uint8_t* uvl = uvp + (size_t)(y / 2) * d->out_line;
        uint8_t* o = d->rgba + (size_t)y * d->w * 4;
        for (int x = 0; x < w; x++) {
            int c = 298 * ((int)yl[x] - 16) + 128;
            int u = (int)uvl[x & ~1] - 128, v = (int)uvl[x | 1] - 128;
            o[0] = clamp8((c + 409 * v) >> 8);
            o[1] = clamp8((c - 100 * u - 208 * v) >> 8);
            o[2] = clamp8((c + 516 * u) >> 8);
            o[3] = 255;
            o += 4;
        }
    }
    gfx_tex_update(tex, 0, 0, (uint32_t)w, (uint32_t)h, d->rgba, d->w * 4);
    return 1;
}
