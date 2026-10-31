// Menu music: decodes an MP3 with minimp3 a little ahead of playback and queues it to SDL audio.
// Fades out when a game starts (the game needs the audio device) and back in when the menu returns.
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "music.h"
#include "../rw_core.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* g_file;
static size_t g_size, g_pos;
static mp3dec_t g_dec;
static SDL_AudioDeviceID g_dev;
static int g_hz;
static float g_level;            // peak of the last queued chunk, for the menu's equalizer
static float g_vol = 1.0f;       // current volume 0..1 (applied as samples are queued)
static float g_target = 1.0f;
static float g_step;             // volume change per sample frame

#define MENU_VOLUME 0.39f // 0.7 at first; turned down 20%, then 30% more (still loud on the console)
static float g_user_volume = 1.0f; // settings menu choice

// Sound effect mixed over the music (stereo s16 at g_hz), synthesized at startup.
static int16_t* g_sfx;
static int g_sfx_len, g_sfx_pos = -1;

// "Start game" chime: four quick rising notes (C, E, G, high C) with a soft attack and decay.
static void make_launch_sfx(void) {
    static const float notes[] = { 523.25f, 659.25f, 783.99f, 1046.5f };
    int note_len = g_hz * 7 / 100;      // 70 ms apart
    int tail = g_hz * 30 / 100;         // the last note rings longer
    g_sfx_len = note_len * 4 + tail;
    free(g_sfx); // (another track may have another sample rate)
    g_sfx_pos = -1;
    g_sfx =(int16_t*)calloc((size_t)g_sfx_len * 2, sizeof(int16_t));
    if (!g_sfx) return;
    for (int n = 0; n < 4; n++) {
        int start = n * note_len, len = n == 3 ? note_len + tail : note_len * 2;
        for (int i = 0; i < len && start + i < g_sfx_len; i++) {
            float t = (float)i / (float)g_hz;
            float env = (i < g_hz / 200 ? (float)i / (float)(g_hz / 200) : 1.0f) * expf(-t * (n == 3 ? 6.0f : 14.0f));
            float ph = 2.0f * 3.14159265f * notes[n] * t;
            float s = (sinf(ph) + 0.25f * sinf(2.0f * ph)) * env * 0.22f;
            int v = g_sfx[(start + i) * 2] + (int)(s * 32767.0f);
            v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
            g_sfx[(start + i) * 2] = g_sfx[(start + i) * 2 + 1] = (int16_t)v;
        }
    }
}

void music_sfx_launch(void) {
    if (g_sfx) g_sfx_pos = 0;
}
#define QUEUE_SECONDS 0.1f       // short queue so fades are heard quickly

static int decode_frame(int16_t* out, int* hz) {
    mp3dec_frame_info_t info;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (int tries = 0; tries < 2; tries++) {
        int n = mp3dec_decode_frame(&g_dec, g_file + g_pos, (int)(g_size - g_pos), pcm, &info);
        g_pos += (size_t)info.frame_bytes;
        if (n > 0) {
            *hz = info.hz;
            for (int i = 0; i < n; i++) {
                out[i * 2] = pcm[info.channels == 2 ? i * 2 : i];
                out[i * 2 + 1] = pcm[info.channels == 2 ? i * 2 + 1 : i];
            }
            return n;
        }
        if (info.frame_bytes == 0 || g_pos >= g_size) {
            g_pos = 0; // end of the track: loop
            mp3dec_init(&g_dec);
        }
    }
    return 0;
}

static int open_device(void) {
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return -1;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof(want));
    want.freq = g_hz;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) return -1;
    SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

int music_start(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    g_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_file = (uint8_t*)malloc(g_size);
    if (!g_file || fread(g_file, 1, g_size, f) != g_size) {
        fclose(f);
        free(g_file);
        g_file = NULL;
        return -1;
    }
    fclose(f);
    // Tags at the front (cover art can be megabytes) aren't music: decoding gives up on a long run of them.
    if (g_size > 10 && !memcmp(g_file, "ID3", 3)) {
        size_t tag = 10 + ((size_t)(g_file[6] & 127) << 21 | (size_t)(g_file[7] & 127) << 14 |
                           (size_t)(g_file[8] & 127) << 7 | (size_t)(g_file[9] & 127));
        if (tag < g_size) {
            memmove(g_file, g_file + tag, g_size - tag);
            g_size -= tag;
        }
    }
    mp3dec_init(&g_dec);
    g_pos = 0;
    // The first frame tells the sample rate the device needs.
    int16_t tmp[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    if (decode_frame(tmp, &g_hz) <= 0) return -1;
    g_pos = 0;
    mp3dec_init(&g_dec);
    make_launch_sfx();
    // Fade in when the app opens instead of starting at full volume.
    g_vol = 0.0f;
    music_fade_to(1.0f, 2.0f);
    return open_device();
}

void music_update(void) {
    if (!g_dev) return;
    int16_t buf[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    while (SDL_GetQueuedAudioSize(g_dev) < (Uint32)(g_hz * QUEUE_SECONDS) * 4) {
        int hz, n = decode_frame(buf, &hz);
        if (n <= 0) break;
        int peak = 0;
        for (int i = 0; i < n; i++) {
            if (g_vol < g_target) g_vol = g_vol + g_step > g_target ? g_target : g_vol + g_step;
            if (g_vol > g_target) g_vol = g_vol - g_step < g_target ? g_target : g_vol - g_step;
            float k = g_vol * g_vol * MENU_VOLUME * g_user_volume; // squared: sounds like an even fade
            int sfx_on = g_sfx_pos >= 0 && g_sfx_pos < g_sfx_len;
            for (int c = 0; c < 2; c++) {
                int v = (int)((float)buf[i * 2 + c] * k);
                if (sfx_on) v += g_sfx[g_sfx_pos * 2 + c]; // not faded with the music
                v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
                buf[i * 2 + c] = (int16_t)v;
                if (abs(v) > peak) peak = abs(v);
            }
            if (sfx_on && ++g_sfx_pos >= g_sfx_len) g_sfx_pos = -1;
        }
        g_level = g_level * 0.6f + (float)peak / 32768.0f * 0.4f;
        SDL_QueueAudio(g_dev, buf, (Uint32)n * 4);
    }
}

void music_fade_to(float target, float seconds) {
    g_target = target;
    float frames = seconds * (float)(g_hz ? g_hz : 44100);
    g_step = frames > 1 ? 1.0f / frames : 1.0f;
}

int music_faded_out(void) {
    // Silent, the sound effect finished, and nothing loud left in the queue.
    return g_vol <= 0.0f && g_sfx_pos < 0 && (!g_dev || SDL_GetQueuedAudioSize(g_dev) == 0 || g_level < 0.001f);
}

float music_level(void) { return g_level; }

void music_set_volume(float volume) { g_user_volume = volume; }

void music_pause(void) {
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    g_level = 0;
}

void music_resume(float fade_seconds) {
    if (!g_file || g_dev) return;
    g_vol = 0.0f;
    music_fade_to(1.0f, fade_seconds);
    open_device();
}

void music_stop(void) {
    music_pause();
    free(g_file);
    g_file = NULL;
}

int music_switch(const char* path) {
    music_stop();
    return music_start(path);
}
