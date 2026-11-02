// BitmapData.perlinNoise, natively: a port of the engine's turbulence.rs (itself the SVG feTurbulence reference
// code), kept step for step so the pixels come out the same. As wasm on one core, Happy Wheels' menu clouds took
// 15 s on the console; rw_core.c spreads the rows over the Wii U's three cores.
#include "rw_core.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RAND_M 2147483647LL
#define RAND_A 16807LL
#define RAND_Q 127773LL
#define RAND_R 2836LL
#define B_SIZE 0x100
#define BM 0xff
#define PERLIN_N 0x1000

typedef struct {
    int width, height, wrap_x, wrap_y;
} StitchInfo;

struct PerlinJob {
    int32_t lattice[B_SIZE + B_SIZE + 2];
    double gradient[4][B_SIZE + B_SIZE + 2][2];
    uint32_t w, h, octaves, flags;
    double base_freq[2];
    const double* offsets; // 2 per octave
    uint8_t* out;          // w * h RGBA
};

static int64_t setup_seed(int64_t seed) {
    if (seed <= 0) seed = -(seed % (RAND_M - 1)) + 1;
    if (seed > RAND_M - 1) seed = RAND_M - 1;
    return seed;
}

static int64_t prand(int64_t seed) {
    int64_t r = RAND_A * (seed % RAND_Q) - RAND_R * (seed / RAND_Q);
    if (r <= 0) r += RAND_M;
    return r;
}

static double s_curve(double t) { return t * t * (3. - 2. * t); }
static double lerp(double t, double a, double b) { return a + t * (b - a); }

PerlinJob* perlin_job_new(uint32_t w, uint32_t h, double base_x, double base_y, uint32_t octaves, int64_t seed,
                          uint32_t flags, const double* offsets, uint8_t* out) {
    PerlinJob* j = (PerlinJob*)calloc(1, sizeof(PerlinJob));
    if (!j) return NULL;
    j->w = w;
    j->h = h;
    j->octaves = octaves;
    j->flags = flags;
    j->offsets = offsets;
    j->out = out;
    j->base_freq[0] = base_x == 0.0 ? 0.0 : 1.0 / base_x;
    j->base_freq[1] = base_y == 0.0 ? 0.0 : 1.0 / base_y;

    seed = setup_seed(seed);
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < B_SIZE; i++) {
            j->lattice[i] = i;
            for (int c = 0; c < 2; c++) {
                seed = prand(seed);
                j->gradient[k][i][c] = (double)((seed % (B_SIZE + B_SIZE)) - B_SIZE) / (double)B_SIZE;
            }
            double s = sqrt(j->gradient[k][i][0] * j->gradient[k][i][0] + j->gradient[k][i][1] * j->gradient[k][i][1]);
            j->gradient[k][i][0] /= s;
            j->gradient[k][i][1] /= s;
        }
    }
    for (int i = B_SIZE - 1; i >= 1; i--) {
        int32_t k = j->lattice[i];
        seed = prand(seed);
        int jj = (int)(seed % B_SIZE);
        j->lattice[i] = j->lattice[jj];
        j->lattice[jj] = k;
    }
    for (int i = 0; i < B_SIZE + 2; i++) {
        j->lattice[B_SIZE + i] = j->lattice[i];
        for (int k = 0; k < 4; k++)
            for (int c = 0; c < 2; c++) j->gradient[k][B_SIZE + i][c] = j->gradient[k][i][c];
    }
    return j;
}

void perlin_job_free(PerlinJob* j) { free(j); }

static double noise2(const PerlinJob* j, int channel, double vx, double vy, const StitchInfo* st) {
    double t = vx + (double)PERLIN_N;
    int bx0 = (int)t, bx1 = bx0 + 1;
    double rx0 = t - (double)(int)t, rx1 = rx0 - 1.0;
    t = vy + (double)PERLIN_N;
    int by0 = (int)t, by1 = by0 + 1;
    double ry0 = t - (double)(int)t, ry1 = ry0 - 1.0;
    if (st) {
        if (bx0 >= st->wrap_x) bx0 -= st->width;
        if (bx1 >= st->wrap_x) bx1 -= st->width;
        if (by0 >= st->wrap_y) by0 -= st->height;
        if (by1 >= st->wrap_y) by1 -= st->height;
    }
    bx0 &= BM; bx1 &= BM; by0 &= BM; by1 &= BM;
    int i = j->lattice[bx0], jj = j->lattice[bx1];
    int b00 = j->lattice[i + by0], b10 = j->lattice[jj + by0], b01 = j->lattice[i + by1], b11 = j->lattice[jj + by1];
    double sx = s_curve(rx0), sy = s_curve(ry0);
    const double* q = j->gradient[channel][b00];
    double u = rx0 * q[0] + ry0 * q[1];
    q = j->gradient[channel][b10];
    double v = rx1 * q[0] + ry0 * q[1];
    double a = lerp(sx, u, v);
    q = j->gradient[channel][b01];
    u = rx0 * q[0] + ry1 * q[1];
    q = j->gradient[channel][b11];
    v = rx1 * q[0] + ry1 * q[1];
    double b = lerp(sx, u, v);
    return lerp(sy, a, b);
}

static double turbulence(const PerlinJob* j, int channel, double px, double py, int fractal, int stitch) {
    double fx = j->base_freq[0], fy = j->base_freq[1];
    StitchInfo st, *stp = NULL;
    if (stitch) {
        double tw = (double)j->w, th = (double)j->h;
        if (fx != 0.0) {
            double lo = floor(tw * fx) / tw, hi = ceil(tw * fx) / tw;
            fx = (fx / lo < hi / fx) ? lo : hi;
        }
        if (fy != 0.0) {
            // (The engine has the same slip here: lo uses the x frequency, as the SVG reference code does.)
            double lo = floor(th * fx) / th, hi = ceil(th * fy) / th;
            fy = (fy / lo < hi / fy) ? lo : hi;
        }
        int w = (int)(tw * fx + 0.5), h = (int)(th * fy + 0.5);
        st.width = w;
        st.height = h;
        st.wrap_x = (int)(0.0 * fx) + PERLIN_N + w;
        st.wrap_y = (int)(0.0 * fy) + PERLIN_N + h;
        stp = &st;
    }
    double sum = 0.0, ratio = 1.0;
    for (uint32_t o = 0; o < j->octaves; o++) {
        double vx = (px + j->offsets[o * 2]) * fx * ratio, vy = (py + j->offsets[o * 2 + 1]) * fy * ratio;
        double n = noise2(j, channel, vx, vy, stp);
        sum += (fractal ? n : fabs(n)) / ratio;
        ratio *= 2.0;
        if (stp) {
            st.width *= 2;
            st.wrap_x = 2 * st.wrap_x - PERLIN_N;
            st.height *= 2;
            st.wrap_y = 2 * st.wrap_y - PERLIN_N;
        }
    }
    return sum;
}

// Rust's `as u8`: truncation, saturating, NaN to 0.
static uint8_t to_u8(double v) {
    if (!(v > 0.0)) return 0;
    if (v >= 255.0) return 255;
    return (uint8_t)v;
}

void perlin_rows(const PerlinJob* j, uint32_t y0, uint32_t y1) {
    int stitch = j->flags & 1, fractal = (j->flags >> 1) & 1, gray = (j->flags >> 2) & 1, transparent = (j->flags >> 3) & 1;
    uint32_t channels = (j->flags >> 4) & 15;
    for (uint32_t y = y0; y < y1; y++) {
        uint8_t* row = j->out + (size_t)y * j->w * 4;
        for (uint32_t x = 0; x < j->w; x++) {
            double px = (double)x, py = (double)y, noise[4];
            if (gray) {
                noise[0] = turbulence(j, 0, px, py, fractal, stitch);
                noise[1] = noise[2] = noise[0];
                noise[3] = (channels & 8) ? turbulence(j, 1, px, py, fractal, stitch) : 1.0;
            } else {
                int channel = 0;
                for (int c = 0; c < 4; c++) {
                    noise[c] = c == 3 ? 1.0 : -1.0;
                    if (channels & (1u << c)) {
                        noise[c] = turbulence(j, channel, px, py, fractal, stitch);
                        channel++;
                    }
                }
            }
            for (int c = 0; c < 4; c++) {
                // Exactly how Flash turns the -1..1 or 0..1 values into bytes (see turbulence.rs).
                double v = fractal ? ((noise[c] * 255.0 + 255.0) + 0.5) / 2.0 : (noise[c] * 255.0) + 0.5;
                row[x * 4 + c] = to_u8(v);
            }
            if (!transparent) row[x * 4 + 3] = 255;
        }
    }
}
