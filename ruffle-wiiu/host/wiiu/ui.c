// Game picker, drawn on the CPU into the screen texture (the Ruffle player isn't running yet).
// Look: an early-2000s math/Flash-games homepage - black page, chunky cartoon lettering with outlines,
// rows of bright geometric shapes, thick red/yellow/green box borders, ">>>>> heading <<<<<" arrows,
// oval and slanted buttons, blue underlined game names. (Our own title and art; the font is Chewy.)
// Menu music: "Slimers" by (T-T)b. Port by Charliebear31.
#include "ui.h"
#include "../rw_core.h"
#include "font_chewy.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- drawing primitives

static uint8_t* C;                  // canvas being drawn
static int CW = UI_W, CH = UI_H;    // its size (text sprites are drawn into small canvases)
static int g_sprite;                // drawing into a sprite: keep coverage in alpha ("over" compositing)

static void blend_px(int x, int y, uint32_t rgb, int a) {
    if ((unsigned)x >= (unsigned)CW || (unsigned)y >= (unsigned)CH || a <= 0) return;
    if (a > 255) a = 255;
    uint8_t* p = C + ((size_t)y * CW + x) * 4;
    if (g_sprite) {
        // Straight-alpha "over": the sprite is later blended onto the screen with its alpha.
        int da = p[3], oa = a + (da * (255 - a) + 127) / 255;
        if (!oa) return;
        for (int k = 0; k < 3; k++) {
            int sc = (int)(rgb >> (16 - k * 8) & 255);
            p[k] = (uint8_t)((sc * a + p[k] * da * (255 - a) / 255) / oa);
        }
        p[3] = (uint8_t)oa;
        return;
    }
    if (a >= 255) {
        p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb;
    } else {
        p[0] = (uint8_t)(p[0] + (((int)(rgb >> 16 & 255) - p[0]) * a >> 8));
        p[1] = (uint8_t)(p[1] + (((int)(rgb >> 8 & 255) - p[1]) * a >> 8));
        p[2] = (uint8_t)(p[2] + (((int)(rgb & 255) - p[2]) * a >> 8));
    }
    p[3] = 255;
}

static void rect(int x, int y, int w, int h, uint32_t rgb, int a) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > CW ? CW : x + w, y1 = y + h > CH ? CH : y + h;
    if (a >= 255 && !g_sprite && x1 > x0) {
        // Opaque: whole RGBA words (R G B 255 in memory = 0xRRGGBBFF on the big-endian console).
        uint8_t px[4] = { (uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255 };
        uint32_t word;
        memcpy(&word, px, 4);
        for (int yy = y0; yy < y1; yy++) {
            uint32_t* row = (uint32_t*)(C + ((size_t)yy * CW + x0) * 4);
            for (int xx = x0; xx < x1; xx++) *row++ = word;
        }
        return;
    }
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) blend_px(xx, yy, rgb, a);
}

// Outline of a rectangle, t pixels thick, inside (x, y, w, h).
static void frame(int x, int y, int w, int h, int t, uint32_t rgb) {
    rect(x, y, w, t, rgb, 255);
    rect(x, y + h - t, w, t, rgb, 255);
    rect(x, y, t, h, rgb, 255);
    rect(x + w - t, y, t, h, rgb, 255);
}

static void ellipse(float cx, float cy, float rx, float ry, uint32_t rgb, int a) {
    int x0 = (int)(cx - rx - 1), x1 = (int)(cx + rx + 1), y0 = (int)(cy - ry - 1), y1 = (int)(cy + ry + 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = ((float)x + 0.5f - cx) / rx, dy = ((float)y + 0.5f - cy) / ry;
            // distance from the edge in pixels, roughly
            float d = (sqrtf(dx * dx + dy * dy) - 1.0f) * (rx < ry ? rx : ry);
            if (d > 0.5f) continue;
            blend_px(x, y, rgb, d > -0.5f ? (int)((float)a * (0.5f - d)) : a);
        }
}

// Filled convex polygon (shapes, slanted buttons), anti-aliased: per pixel row, 4 sub-rows each give a
// span whose exact horizontal overlap with every pixel is added up as coverage.
static void polygon(const float* xy, int n, uint32_t rgb, int a) {
    float miny = xy[1], maxy = xy[1], minx = xy[0], maxx = xy[0];
    for (int i = 1; i < n; i++) {
        miny = fminf(miny, xy[i * 2 + 1]); maxy = fmaxf(maxy, xy[i * 2 + 1]);
        minx = fminf(minx, xy[i * 2]); maxx = fmaxf(maxx, xy[i * 2]);
    }
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx);
    if (x0 < 0) x0 = 0;
    if (x1 > CW) x1 = CW;
    if (x1 <= x0) return;
    static float cov[UI_W + 2];
    for (int y = (int)floorf(miny); y <= (int)maxy; y++) {
        if (y < 0 || y >= CH) continue;
        memset(cov + x0, 0, sizeof(float) * (size_t)(x1 - x0 + 1));
        for (int s = 0; s < 4; s++) {
            float py = (float)y + 0.125f + 0.25f * (float)s;
            float lo = 1e9f, hi = -1e9f;
            for (int i = 0, j = n - 1; i < n; j = i++) {
                float xi = xy[i * 2], yi = xy[i * 2 + 1], xj = xy[j * 2], yj = xy[j * 2 + 1];
                if ((yi > py) == (yj > py)) continue;
                float x = (xj - xi) * (py - yi) / (yj - yi) + xi;
                lo = fminf(lo, x);
                hi = fmaxf(hi, x);
            }
            if (hi <= lo) continue;
            for (int x = (int)floorf(lo); x <= (int)hi; x++) {
                if (x < x0 || x >= x1) continue;
                float l = fmaxf(lo, (float)x), r = fminf(hi, (float)x + 1.0f);
                if (r > l) cov[x] += (r - l) * 0.25f;
            }
        }
        for (int x = x0; x < x1; x++)
            if (cov[x] > 0.0f) blend_px(x, y, rgb, (int)((float)a * fminf(cov[x], 1.0f)));
    }
}

// Regular polygon with `sides` corners around (cx, cy), first corner straight up.
static void regular(float cx, float cy, float r, int sides, uint32_t rgb) {
    float xy[16];
    for (int i = 0; i < sides && i < 8; i++) {
        float ang = -1.5707963f + 6.2831853f * (float)i / (float)sides;
        xy[i * 2] = cx + r * cosf(ang);
        xy[i * 2 + 1] = cy + r * sinf(ang);
    }
    polygon(xy, sides, rgb, 255);
}

// ---------------------------------------------------------------- lettering (Chewy)

#define NO_LINE 0xff000000u // outline colour meaning "no outline"

// One glyph mask (fill or outline) scaled by s, drawn with its cell's top-left at (x0, y0).
static void glyph_mask(const uint8_t* mask, const int16_t* gl, float x0, float y0, float s, uint32_t rgb, int alpha) {
    int dw = (int)ceilf((float)gl[1] * s), dh = (int)ceilf((float)CHEWY_H * s);
    int ox = (int)floorf(x0), oy = (int)floorf(y0);
    for (int dy = 0; dy < dh; dy++) {
        float v = ((float)dy + 0.5f) / s - 0.5f;
        int v0 = (int)floorf(v);
        float fv = v - (float)v0;
        for (int dx = 0; dx < dw; dx++) {
            float u = ((float)dx + 0.5f) / s - 0.5f;
            int u0 = (int)floorf(u);
            float fu = u - (float)u0;
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                int uu = u0 + (k & 1), vv = v0 + (k >> 1);
                if (uu < 0 || uu >= gl[1] || vv < 0 || vv >= CHEWY_H) continue;
                float wgt = ((k & 1) ? fu : 1.0f - fu) * ((k >> 1) ? fv : 1.0f - fv);
                sum += wgt * (float)mask[vv * CHEWY_STRIP_W + gl[0] + uu];
            }
            if (sum >= 1.0f) blend_px(ox + dx, oy + dy, rgb, (int)(sum * (float)alpha / 255.0f));
        }
    }
}

static const int16_t* glyph(char c) {
    unsigned char u = (unsigned char)c;
    return chewy_glyphs[(u < 32 || u > 126) ? '?' - 32 : u - 32];
}

// Width in pixels of s at size px (a capital letter is about 0.7 px tall).
static int ftext_w(const char* s, int px) {
    float sc = (float)px / (float)CHEWY_EM, w = 0.0f;
    for (; *s; s++) w += (float)glyph(*s)[3] * sc;
    return (int)(w + 0.5f);
}

// Text with its cell top at y: a drop shadow, an outline (unless NO_LINE) and the fill, drawn straight
// into the current canvas (slow: scaled from the masks).
static void ftext_raw(int x, int y, const char* s, int px, uint32_t fill, uint32_t line, int shadow) {
    float sc = (float)px / (float)CHEWY_EM;
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 0 && !shadow) continue;
        if (pass == 1 && line == NO_LINE) continue;
        float pen = (float)x;
        for (const char* p = s; *p; p++) {
            const int16_t* gl = glyph(*p);
            float gx = pen + (float)gl[2] * sc;
            if (*p != ' ') {
                if (pass == 0) glyph_mask(line == NO_LINE ? chewy_fill : chewy_line, gl, gx + (float)shadow, (float)(y + shadow), sc, 0x000000, 170);
                else if (pass == 1) glyph_mask(chewy_line, gl, gx, (float)y, sc, line, 255);
                else glyph_mask(chewy_fill, gl, gx, (float)y, sc, fill, 255);
            }
            pen += (float)gl[3] * sc;
        }
    }
}

// Rendered strings are kept as small RGBA sprites (straight alpha) and only copied after the first time:
// scaling the masks for every string on every frame cost too much of the console's CPU for a 60 fps menu.
typedef struct {
    char key[160];
    int ox, w, h;      // sprite left edge relative to the pen x; size
    uint8_t* px;
    uint32_t used;
} TextSprite;
#define NSPRITES 192
static TextSprite g_spr[NSPRITES];
static uint32_t g_spr_clock;

static TextSprite* text_sprite(const char* s, int px, uint32_t fill, uint32_t line, int shadow) {
    char key[160];
    snprintf(key, sizeof(key), "%d|%06x|%08x|%d|%s", px, (unsigned)fill, (unsigned)line, shadow, s);
    g_spr_clock++;
    TextSprite* victim = &g_spr[0];
    for (int i = 0; i < NSPRITES; i++) {
        TextSprite* sp = &g_spr[i];
        if (sp->px && !strcmp(sp->key, key)) {
            sp->used = g_spr_clock;
            return sp;
        }
        if (!sp->px || sp->used < victim->used) victim = sp;
    }
    // Extents: glyph cells can reach left of the pen (outline) and past the last advance.
    float sc = (float)px / (float)CHEWY_EM, pen = 0.0f, lo = 0.0f, hi = 0.0f;
    for (const char* p = s; *p; p++) {
        const int16_t* gl = glyph(*p);
        lo = fminf(lo, pen + (float)gl[2] * sc);
        hi = fmaxf(hi, pen + (float)(gl[2] + gl[1]) * sc);
        pen += (float)gl[3] * sc;
    }
    int ox = (int)floorf(lo) - 1;
    int w = (int)ceilf(hi) - ox + shadow + 2, h = (int)ceilf((float)CHEWY_H * sc) + shadow + 2;
    uint8_t* buf = (uint8_t*)calloc((size_t)w * h, 4);
    if (!buf) return NULL;
    free(victim->px);
    snprintf(victim->key, sizeof(victim->key), "%s", key);
    victim->ox = ox;
    victim->w = w;
    victim->h = h;
    victim->px = buf;
    victim->used = g_spr_clock;
    uint8_t* saved = C;
    int sw = CW, sh = CH;
    C = buf;
    CW = w;
    CH = h;
    g_sprite = 1;
    ftext_raw(-ox, 0, s, px, fill, line, shadow);
    g_sprite = 0;
    C = saved;
    CW = sw;
    CH = sh;
    return victim;
}

static void ftext(int x, int y, const char* s, int px, uint32_t fill, uint32_t line, int shadow) {
    TextSprite* sp = text_sprite(s, px, fill, line, shadow);
    if (!sp) return;
    for (int yy = 0; yy < sp->h; yy++) {
        int dy = y + yy;
        if ((unsigned)dy >= (unsigned)CH) continue;
        const uint8_t* src = sp->px + (size_t)yy * sp->w * 4;
        uint8_t* dst = C + ((size_t)dy * CW) * 4;
        for (int xx = 0; xx < sp->w; xx++, src += 4) {
            int a = src[3], dx = x + sp->ox + xx;
            if (!a || (unsigned)dx >= (unsigned)CW) continue;
            uint8_t* p = dst + dx * 4;
            if (a == 255) {
                p[0] = src[0]; p[1] = src[1]; p[2] = src[2];
            } else {
                p[0] = (uint8_t)(p[0] + ((src[0] - p[0]) * a >> 8));
                p[1] = (uint8_t)(p[1] + ((src[1] - p[1]) * a >> 8));
                p[2] = (uint8_t)(p[2] + ((src[2] - p[2]) * a >> 8));
            }
            p[3] = 255;
        }
    }
}

static void ftext_c(int cx, int y, const char* s, int px, uint32_t fill, uint32_t line, int shadow) {
    ftext(cx - ftext_w(s, px) / 2, y, s, px, fill, line, shadow);
}

// Shortened with "..." to fit max_w.
static void ftext_fit(int x, int y, const char* s, int px, uint32_t fill, uint32_t line, int shadow, int max_w, int center) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", s);
    size_t n = strlen(buf);
    while (n > 3 && ftext_w(buf, px) > max_w) {
        n--;
        buf[n] = 0;
        if (n > 3) memcpy(buf + n - 3, "...", 3);
    }
    if (center) ftext_c(x, y, buf, px, fill, line, shadow);
    else ftext(x, y, buf, px, fill, line, shadow);
}

// ---------------------------------------------------------------- theme

typedef struct { uint32_t red, yellow, green, blue, purple, orange, white, link, bg, sel; } Theme;
static const Theme THEMES[UI_THEME_COUNT] = {
    { 0xff1a1a, 0xffe600, 0x1ed31e, 0x1e8fff, 0xa21ec8, 0xff8c00, 0xffffff, 0x2a9dff, 0x000000, 0x0a2a0a },
    // Adobe Flash: the authoring program's dark grey panels, red accents, blue selections.
    { 0xe0261c, 0xf0f0f0, 0x4f9fe8, 0x3d6f9e, 0x5c5c5c, 0xe8822a, 0xffffff, 0x9cc8f0, 0x2b2b2b, 0x1d3a55 },
    // Halloween: pumpkin orange on a night sky.
    { 0xf26a0f, 0xffb81c, 0x8bd32a, 0x6b3fb8, 0x4d2a80, 0xff7a00, 0xf6f0dc, 0xc9a2ff, 0x120a22, 0x2a1440 },
};
static const Theme* T = &THEMES[0];
static int g_theme;

#define RED (T->red)
#define YELLOW (T->yellow)
#define GREEN (T->green)
#define BLUE (T->blue)
#define PURPLE (T->purple)
#define ORANGE (T->orange)
#define WHITE (T->white)
#define LINK_BLUE (T->link)
#define PAGE_BG (T->bg)

#define BRIGHT ((const uint32_t[]){ RED, YELLOW, GREEN, BLUE, PURPLE, ORANGE })
#define NBRIGHT 6

static uint32_t darker(uint32_t c, int pct) {
    return (uint32_t)(((c >> 16 & 255) * pct / 100) << 16 | ((c >> 8 & 255) * pct / 100) << 8 | (c & 255) * pct / 100);
}

// The plain page.
static void background(void) {
    rect(0, 0, UI_W, UI_H, PAGE_BG, 255);
}

// Rounded rectangle (opaque).
static void rrect(int x, int y, int w, int h, int r, uint32_t rgb) {
    rect(x, y + r, w, h - 2 * r, rgb, 255);
    for (int i = 0; i < r; i++) {
        float d = (float)r - (float)i - 0.5f;
        int in = r - (int)sqrtf((float)(r * r) - d * d);
        rect(x + in, y + i, w - 2 * in, 1, rgb, 255);
        rect(x + in, y + h - 1 - i, w - 2 * in, 1, rgb, 255);
    }
}

// Pixel art: one letter a pixel, '.' is see-through.
static void pixel_art(const char* const* rows, int n, int x, int y, int s, const uint32_t* pal, const char* keys) {
    for (int r = 0; r < n; r++)
        for (const char* p = rows[r]; *p; p++) {
            if (*p == '.') continue;
            const char* k = strchr(keys, *p);
            if (k) rect(x + (int)(p - rows[r]) * s, y + r * s, s, s, pal[k - keys], 255);
        }
}

// Darkens the whole canvas to a quarter (behind the settings / credits panels).
static void dim_all(void) {
    uint8_t keep[4] = { 0x3f, 0x3f, 0x3f, 0x00 }, alpha[4] = { 0, 0, 0, 255 };
    uint32_t mask, opaque;
    memcpy(&mask, keep, 4);
    memcpy(&opaque, alpha, 4);
    uint32_t* p = (uint32_t*)C;
    for (size_t i = 0; i < (size_t)UI_W * UI_H; i++) p[i] = ((p[i] >> 2) & mask) | opaque;
}

// One of the header shapes: 0 square, 1 circle, 2 triangle, 3 pentagon.
static void shape(int kind, float cx, float cy, float r, uint32_t rgb) {
    switch (kind) {
    case 0: rect((int)(cx - r * 0.85f), (int)(cy - r * 0.85f), (int)(r * 1.7f), (int)(r * 1.7f), rgb, 255); break;
    case 1: ellipse(cx, cy, r, r, rgb, 255); break;
    case 2: regular(cx, cy + r * 0.2f, r * 1.15f, 3, rgb); break;
    default: regular(cx, cy + r * 0.05f, r * 1.05f, 5, rgb); break;
    }
}

// ">>>>> text <<<<<" in red/yellow arrows, centered on cx.
static void arrow_heading(int cx, int y, const char* s, int px) {
    const char* r = ">>>>>";
    const char* l = "<<<<<";
    int aw = ftext_w(r, px), tw = ftext_w(s, px), gap = px / 2;
    int x = cx - (aw * 2 + tw + gap * 2) / 2;
    float sc = (float)px / (float)CHEWY_EM;
    for (int i = 0; i < 5; i++) {
        char ch[2] = { r[i], 0 };
        ftext(x, y, ch, px, i % 2 ? YELLOW : RED, NO_LINE, 2);
        x += (int)((float)glyph('>')[3] * sc);
    }
    x += gap;
    ftext(x, y, s, px, YELLOW, 0x000000, 3);
    x += tw + gap;
    for (int i = 0; i < 5; i++) {
        char ch[2] = { l[i], 0 };
        ftext(x, y, ch, px, i % 2 ? YELLOW : RED, NO_LINE, 2);
        x += (int)((float)glyph('<')[3] * sc);
    }
}

// Oval button with outlined yellow text.
static void oval_button(int x, int y, int w, int h, uint32_t rgb, const char* label, int px, int lit) {
    float cx = (float)x + (float)w / 2, cy = (float)y + (float)h / 2;
    if (lit) ellipse(cx, cy, (float)w / 2 + 5, (float)h / 2 + 5, WHITE, 255);
    ellipse(cx + 3, cy + 4, (float)w / 2, (float)h / 2, 0x000000, 160);
    ellipse(cx, cy, (float)w / 2, (float)h / 2, rgb, 255);
    ellipse(cx, cy - (float)h * 0.18f, (float)w * 0.40f, (float)h * 0.22f, WHITE, 40); // shine
    ftext_c((int)cx, (int)(cy - (float)px * 0.80f), label, px, YELLOW, 0x000000, 2);
}

// Slanted button (a trapezoid), the kind stacked down the sides of those old pages.
static void slant_button(int x, int y, int w, int h, uint32_t rgb, int left_high) {
    int k = h / 4;
    float xy[8] = { (float)x, (float)(y + (left_high ? 0 : k)), (float)(x + w), (float)(y + (left_high ? k : 0)),
                    (float)(x + w), (float)(y + h - (left_high ? k : 0)), (float)x, (float)(y + h - (left_high ? 0 : k)) };
    float sh[8];
    for (int i = 0; i < 8; i++) sh[i] = xy[i] + 4.0f;
    polygon(sh, 4, 0x000000, 150);
    polygon(xy, 4, rgb, 255);
}

static void dirty_add(int x, int y, int w, int h);
static void copy_rect(uint8_t* to, const uint8_t* from, int x, int y, int w, int h);
static uint8_t* g_base;

// ---------------------------------------------------------------- theme animations
// Only the two ends of the header move (x < ANIM_W and x >= UI_W - ANIM_W, above the link strip), so only
// those small areas are restored, redrawn and uploaded each frame and the menu keeps its 60 fps.
#define ANIM_W 290
#define ANIM_H 98

static const char* const BAT_UP[] = {
    "w.........w",
    "ww..k.k..ww",
    "wwwwkkkwwww",
    ".wwwkkkwww.",
    "..w..k..w..",
};
static const char* const BAT_DOWN[] = {
    "....k.k....",
    "....kkk....",
    "..wwkkkww..",
    ".wwwwkwwww.",
    "ww.......ww",
};
static const char* const PUMPKIN[] = {
    "......gg.......",
    ".....gg........",
    "...ooooooooo...",
    ".ooddooooooddoo",
    "ooddoooooooddoo",
    "oodkkooookkodoo",
    "oodkykoookykdoo",
    "ooddooookoooddo",
    "ooddoooooooodoo",
    "oodykyokyokydoo",
    "ooddkykykykkdoo",
    ".ooddoooooddoo.",
    "...ooooooooo...",
};
static const char* const GHOST_A[] = {
    "...wwwww...",
    "..wwwwwww..",
    ".wwkkwkkww.",
    ".wwkkwkkww.",
    ".wwwwwwwww.",
    ".wwwkkkwww.",
    ".wwwwwwwww.",
    ".wwwwwwwww.",
    ".wwwwwwwww.",
    ".ww.www.ww.",
    ".w...w...w.",
};
static const char* const GHOST_B[] = {
    "...wwwww...",
    "..wwwwwww..",
    ".wwkkwkkww.",
    ".wwkkwkkww.",
    ".wwwwwwwww.",
    ".wwwkkkwww.",
    ".wwwwwwwww.",
    ".wwwwwwwww.",
    ".wwwwwwwww.",
    ".w.www.www.",
    "...w...w...",
};

// What stays still behind the moving parts, drawn with the rest of the header.
static void theme_header_base(void) {
    if (g_theme == UI_THEME_FLASH) {
        // Panels with a timeline on the right, and the path the ball follows on the left.
        rrect(10, 8, ANIM_W - 20, 84, 6, 0x3a3a3a);
        rrect(UI_W - ANIM_W + 10, 8, ANIM_W - 20, 84, 6, 0x3a3a3a);
        rrect(20, 22, 56, 56, 8, RED);
        ftext_c(46, 18, "f", 58, 0xffffff, NO_LINE, 0);
        for (int i = 0; i < 9; i++) {
            float u = (float)i / 8.0f;
            ellipse(98.0f + u * 160.0f, 68.0f - 44.0f * 4.0f * u * (1.0f - u), 2.5f, 2.5f, 0x7a7a7a, 255);
        }
        int tx = UI_W - ANIM_W + 22;
        for (int i = 0; i <= 24; i++) {
            uint32_t c = i % 5 ? 0x4a4a4a : 0x6a6a6a;
            rect(tx + i * 10, 34, 1, 46, c, 255);
        }
        rect(tx, 34, 241, 1, 0x6a6a6a, 255);
        rect(tx, 80, 241, 1, 0x6a6a6a, 255);
        rect(tx, 50, 241, 1, 0x4a4a4a, 255);
        rect(tx, 65, 241, 1, 0x4a4a4a, 255);
        rect(tx + 1, 51, 239, 14, 0x8fa8c8, 255); // a motion tween
        rect(tx + 1, 66, 119, 14, 0xa8d08f, 255);
        ellipse((float)tx + 5, 58, 3, 3, 0x202020, 255);
        ellipse((float)tx + 235, 58, 3, 3, 0x202020, 255);
    } else if (g_theme == UI_THEME_HALLOWEEN) {
        // A night sky with a big moon.
        static const short stars[][2] = { { 22, 14 }, { 96, 8 }, { 170, 28 }, { 250, 12 }, { 60, 62 }, { 214, 70 },
                                          { 1012, 18 }, { 1090, 62 }, { 1150, 10 }, { 1196, 72 }, { 1262, 26 } };
        for (size_t i = 0; i < sizeof(stars) / sizeof(stars[0]); i++) rect(stars[i][0], stars[i][1], 3, 3, 0xf6f0dc, 255);
        for (int r = -36; r <= 36; r += 4) {
            int half = (int)sqrtf((float)(36 * 36 - r * r));
            rect(1090 - (half & ~3), 48 + r, (half & ~3) * 2, 4, 0xf4e9b8, 255);
        }
        rect(1072, 30, 12, 8, 0xd8cc98, 255);
        rect(1100, 56, 16, 8, 0xd8cc98, 255);
        rect(1076, 64, 8, 8, 0xd8cc98, 255);
    }
}

// A little stage in the gap between the footer's texts: original characters acting out Flash-game scenes, a
// different one every few seconds. Small, so it costs no more than the rest of the animations.
#define STAGE_X 826
#define STAGE_Y 662
#define STAGE_W 160
#define STAGE_H 54
#define STAGE_FLOOR (STAGE_Y + 50)

static const char* const BIKE_A[] = {
    ".....hh.....",
    ".....hhs....",
    "....bbb.....",
    "...bbbbb....",
    "....bb.bb...",
    "..rrrrrrrr..",
    ".kkk.rr.kkk.",
    "k...krrk...k",
    "k.w.k..k.w.k",
    ".kkk....kkk.",
};
static const char* const STICK_A[] = {
    "..ss..",
    "..ss..",
    "..bb..",
    ".bbbb.",
    "b.bb.b",
    "..bb..",
    ".k..k.",
    "k....k",
};
static const char* const STICK_B[] = {
    "..ss..",
    "..ss..",
    "..bb..",
    ".bbbb.",
    ".bbbb.",
    "..bb..",
    "..kk..",
    "..kk..",
};
static const char* const CANNON[] = {
    "......kkkk",
    "....kkkkkk",
    "..kkkkkkk.",
    "kkkkkkkk..",
    ".kkkkk....",
    ".rrrrrr...",
    "rkkrrkkr..",
    ".kk..kk...",
};
static const char* const CRITTER[] = {
    "..ggg..",
    ".gwgwg.",
    "ggggggg",
    ".ggggg.",
    "g.g.g.g",
};
static const char* const BALLOON[] = {
    ".rrrr.",
    "rrwrrr",
    "rrrrrr",
    "rrrrrr",
    ".rrrr.",
    "..rr..",
    "..k...",
    "...k..",
};
static const char* const CHEF[] = {
    "..wwww..",
    ".wwwwww.",
    "..wwww..",
    "..ssss..",
    "..ssss..",
    "..wwww..",
    ".wwwwwkk",
    "w.wwww.k",
    "..wwww..",
    "..b..b..",
};
static const char* const BURGER[] = {
    ".yyyy.",
    "gggggg",
    "rrrrrr",
    ".yyyy.",
};
static const char* const WITCH[] = {
    ".....k.......",
    "....kkk......",
    "...kkkkk.....",
    "..kkkkkkk....",
    "....ggg......",
    "....kkkk.....",
    "yyy.kkkkk.bbb",
    "yyybbbbbbbbbb",
    "yyy..kk......",
};
static const char* const SKELETON_A[] = {
    "..www..",
    "..kwk..",
    "..www..",
    "...w...",
    ".wwwww.",
    "w..w..w",
    "...w...",
    "..w.w..",
    ".w...w.",
};
static const char* const SKELETON_B[] = {
    "..www..",
    "..kwk..",
    "..www..",
    "...w...",
    ".wwwww.",
    ".w.w.w.",
    "...w...",
    "..w.w..",
    "..w.w..",
};
static const char* const GRAVE[] = {
    "..pppp..",
    ".pppppp.",
    ".pkpkpp.",
    ".pppppp.",
    ".ppkkpp.",
    ".pppppp.",
    "dddddddd",
};
static const char* const ZOMBIE_HAND[] = {
    "g.g.g",
    "g.g.g",
    "ggggg",
    ".ggg.",
    ".ggg.",
    ".ggg.",
};
static const char* const CAT_A[] = {
    "k...k.....",
    "kk.kk....k",
    "kykyk....k",
    "kkkkk...k.",
    ".kkkkkkkk.",
    ".kkkkkkkk.",
    ".k.k..k.k.",
};
static const char* const CAT_B[] = {
    "k...k.....",
    "kk.kk.....",
    "kykyk...kk",
    "kkkkk...k.",
    ".kkkkkkkk.",
    ".kkkkkkkk.",
    "k.k....k.k",
};

#define ROWS(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void art_on_floor(const char* const* rows, int n, int x, int s) {
    // k black, w white, r red, g green, b blue, y yellow, s skin, h helmet, p stone, d dirt
    const uint32_t pal[10] = { 0x181818, 0xf4f4f4, 0xe0302a, 0x4cc23a, 0x3a78d8, 0xf5c52c, 0xf0c090, 0xe8691a, 0x8a8a98, 0x5a3a20 };
    pixel_art(rows, n, x, STAGE_FLOOR - n * s, s, pal, "kwrgbyshpd");
}

static void art_at(const char* const* rows, int n, int x, int y, int s) {
    pixel_art(rows, n, x, y, s, (const uint32_t[10]){ 0x181818, 0xf4f4f4, 0xe0302a, 0x4cc23a, 0x3a78d8, 0xf5c52c, 0xf0c090, 0xe8691a, 0x8a8a98, 0x5a3a20 }, "kwrgbyshpd");
}

static void stage_scene(float t) {
    const int scene = (int)(t / 7.0f) % 3;
    const float u = fmodf(t, 7.0f) / 7.0f;
    const int step = (int)(t * 8.0f) & 1;
    rect(STAGE_X, STAGE_FLOOR, STAGE_W, 2, darker(YELLOW, 60), 255);
    if (g_theme == UI_THEME_HALLOWEEN) {
        if (scene == 0) { // a witch flies over two graves
            art_on_floor(GRAVE, ROWS(GRAVE), STAGE_X + 14, 3);
            art_on_floor(GRAVE, ROWS(GRAVE), STAGE_X + 120, 3);
            art_at(WITCH, ROWS(WITCH), STAGE_X + (int)(u * 134.0f), STAGE_Y + 2 + (int)(4.0f * sinf(t * 4.0f)), 2);
        } else if (scene == 1) { // a skeleton walks past, chased by a black cat
            int x = STAGE_X + (int)(u * 139.0f);
            art_on_floor(step ? SKELETON_A : SKELETON_B, 9, x, 3);
            if (x - 30 >= STAGE_X) art_on_floor(step ? CAT_A : CAT_B, 7, x - 30, 2);
        } else { // hands come up from the ground
            art_on_floor(GRAVE, ROWS(GRAVE), STAGE_X + 64, 4);
            for (int i = 0; i < 3; i++) {
                float rise = fminf(1.0f, fmaxf(0.0f, u * 3.0f - (float)i * 0.5f));
                int h = (int)(rise * 18.0f);
                if (h > 0) art_at(ZOMBIE_HAND, h / 3 > 6 ? 6 : h / 3, STAGE_X + 10 + i * 62, STAGE_FLOOR - h, 3);
            }
        }
    } else if (g_theme == UI_THEME_FLASH) {
        // Stick figures, as animated on a timeline: walking, then a tweened jump between keyframes.
        if (scene == 2) {
            float j = fmodf(u * 3.0f, 1.0f);
            int x = STAGE_X + 26 + (int)(u * 112.0f);
            for (int i = 2; i >= 0; i--) { // onion skins
                float o = fmaxf(0.0f, j - (float)i * 0.08f);
                art_at(STICK_B, 8, x - i * 12, STAGE_FLOOR - 24 - (int)(22.0f * 4.0f * o * (1.0f - o)), 3);
            }
        } else {
            int dir = scene == 0;
            int x = STAGE_X + 34 + (int)((dir ? u : 1.0f - u) * 106.0f);
            art_on_floor(step ? STICK_A : STICK_B, 8, x, 3);
            art_on_floor(step ? STICK_B : STICK_A, 8, x - 34, 3);
        }
    } else if (scene == 0) { // a stunt bike jumps a ramp
        int x = STAGE_X + (int)(u * 136.0f);
        int ramp = STAGE_X + 40;
        for (int i = 0; i < 30; i += 2) rect(ramp + i, STAGE_FLOOR - i / 2, 2, i / 2, darker(ORANGE, 70), 255);
        float air = (float)(x - (ramp + 20)) / 76.0f;
        int y = air > 0.0f && air < 1.0f ? (int)(30.0f * 4.0f * air * (1.0f - air)) + 14 : air <= 0.0f && air > -0.3f ? (int)((air + 0.3f) / 0.3f * 14.0f) : 0;
        art_at(BIKE_A, ROWS(BIKE_A), x, STAGE_FLOOR - 20 - y, 2);
    } else if (scene == 1) { // a cannon fires a critter at a balloon, which pops
        art_on_floor(CANNON, ROWS(CANNON), STAGE_X + 2, 3);
        float f = fminf(1.0f, u * 2.0f);
        if (u < 0.5f) art_at(BALLOON, ROWS(BALLOON), STAGE_X + 136, STAGE_Y + 2 + (int)(3.0f * sinf(t * 3.0f)), 2);
        else if (u < 0.6f) for (int i = 0; i < 6; i++) rect(STAGE_X + 142 + (i - 3) * (int)((u - 0.5f) * 90.0f), STAGE_Y + 12 + (i % 3 - 1) * (int)((u - 0.5f) * 90.0f), 3, 3, RED, 255);
        art_at(CRITTER, ROWS(CRITTER), STAGE_X + 32 + (int)(f * 108.0f), STAGE_Y + 22 - (int)(28.0f * 4.0f * f * (1.0f - f) * 0.5f), 2);
    } else { // a chef flips a burger
        art_on_floor(CHEF, ROWS(CHEF), STAGE_X + 60, 3);
        float f = fmodf(u * 3.0f, 1.0f);
        art_at(BURGER, ROWS(BURGER), STAGE_X + 88, STAGE_Y + 22 - (int)(26.0f * 4.0f * f * (1.0f - f) * 0.7f), 2);
    }
}

// One of the three moving areas (0 left of the header, 1 right of it, 2 the footer stage). One a frame, in turn:
// uploading all three every frame cost the console 3 ms and took the menu below 60 fps.
static void theme_animate(float t, int part) {
    if (!g_base) return;
    int ax = part == 0 ? 0 : part == 1 ? UI_W - ANIM_W : STAGE_X - 2;
    int ay = part == 2 ? STAGE_Y - 2 : 0;
    int aw = part == 2 ? STAGE_W + 4 : ANIM_W, ah = part == 2 ? STAGE_H + 6 : ANIM_H;
    copy_rect(C, g_base, ax, ay, aw, ah);
    dirty_add(ax, ay, aw, ah);
    if (part == 2) {
        stage_scene(t);
        return;
    }
    if (g_theme == UI_THEME_CLASSIC) {
        // The shapes either side of the title hop in turn.
        const uint32_t cols[4] = { YELLOW, BLUE, PURPLE, GREEN };
        for (int i = 0; i < 4; i++) {
            float hop = 8.0f * sinf(t * 2.5f + (float)i * 0.9f);
            if (part == 0) shape(i, 42.0f + (float)i * 70.0f, 50.0f + hop, 27.0f, cols[i]);
            else shape(i, (float)UI_W - 42.0f - (float)i * 70.0f, 50.0f - hop, 27.0f, cols[i]);
        }
    } else if (g_theme == UI_THEME_FLASH) {
        // The ball tweens along its path, with onion skins behind it; the playhead runs along the timeline.
        if (part == 1) {
            int tx = UI_W - ANIM_W + 22, head = tx + (int)(fmodf(t * 24.0f, 24.0f)) * 10;
            rect(head - 4, 22, 9, 11, RED, 255);
            rect(head, 33, 1, 48, RED, 255);
            return;
        }
        float u = fmodf(t * 0.4f, 2.0f);
        if (u > 1.0f) u = 2.0f - u;
        for (int i = 3; i >= 0; i--) {
            float o = fmaxf(0.0f, u - (float)i * 0.05f);
            float x = 98.0f + o * 160.0f, y = 68.0f - 44.0f * 4.0f * o * (1.0f - o);
            uint32_t c = i == 0 ? RED : darker(RED, 60 - i * 12);
            ellipse(x, y, 13.0f - (float)i, 13.0f - (float)i, c, 255);
        }
    } else {
        if (part == 1) {
            const uint32_t ghost_pal[2] = { 0xf6f0dc, 0x201030 };
            int gx = UI_W - 96, gy = 22 + (int)(9.0f * sinf(t * 1.6f));
            pixel_art((int)(t * 4.0f) % 2 ? GHOST_A : GHOST_B, 11, gx, gy, 5, ghost_pal, "wk");
            return;
        }
        const uint32_t pal[6] = { 0xf26a0f, 0xc24d08, 0x101010, 0xffd23c, 0x5d9e1e, 0xf6f0dc };
        // A jack-o'-lantern with a flickering candle.
        uint32_t lit[6];
        memcpy(lit, pal, sizeof(lit));
        int flick = (int)(t * 12.0f) % 5;
        lit[3] = flick == 0 ? 0xff9a1c : flick == 3 ? 0xfff07a : 0xffd23c;
        pixel_art(PUMPKIN, 13, 14, 26, 5, lit, "odkygw");
        // Bats flap across the two ends; a ghost drifts up and down by the moon.
        const uint32_t bat_pal[2] = { 0x7a58b8, 0xc9a2ff };
        int flap = (int)(t * 8.0f) % 2;
        for (int i = 0; i < 2; i++) {
            float ph = fmodf(t * 0.25f + (float)i * 0.5f, 1.0f);
            int x = 100 + (int)(ph * 150.0f), y = 26 + i * 26 + (int)(8.0f * sinf(t * 3.0f + (float)i));
            pixel_art(flap ^ i ? BAT_UP : BAT_DOWN, 5, x, y, 3, bat_pal, "wk");
        }
    }
}

// ---------------------------------------------------------------- header (drawn once, then copied)

#define HEADER_H 144
static uint8_t* g_header; // UI_W x HEADER_H RGBA
static int g_header_games = -1;

// The name at the top, in each theme's own style (descenders must end above the strip at y 100).
static void draw_logo(void) {
    const int cx = UI_W / 2;
    if (g_theme == UI_THEME_FLASH) {
        // Like the authoring program's title: plain letters, the last word in its red.
        const char* a = "Flashplayer ";
        const char* b = "Emu";
        int x = cx - (ftext_w(a, 76) + ftext_w(b, 76)) / 2;
        ftext(x, -6, a, 76, 0xffffff, NO_LINE, 2);
        ftext(x + ftext_w(a, 76), -6, b, 76, RED, NO_LINE, 2);
    } else if (g_theme == UI_THEME_HALLOWEEN) {
        // Pumpkin letters with slime running down from them.
        static const short drips[][3] = { { 370, 74, 16 }, { 430, 78, 10 }, { 505, 72, 20 }, { 590, 78, 12 },
                                          { 668, 74, 18 }, { 742, 78, 10 }, { 820, 72, 20 }, { 905, 76, 14 } };
        for (size_t i = 0; i < sizeof(drips) / sizeof(drips[0]); i++) {
            rect(drips[i][0], drips[i][1], 8, drips[i][2], GREEN, 255);
            rect(drips[i][0] + 2, drips[i][1] + drips[i][2], 4, 4, GREEN, 255);
        }
        ftext_c(cx, -10, "Flashplayer Emu", 80, ORANGE, 0x1a0a2e, 5);
    } else {
        // Behind the title: a long flat blue oval with a taller green oval in the middle.
        ellipse((float)UI_W / 2 + 4, 54.0f, 350.0f, 30.0f, 0x000000, 140);
        ellipse((float)UI_W / 2, 50.0f, 350.0f, 30.0f, BLUE, 255);
        ellipse((float)UI_W / 2, 49.0f, 330.0f, 10.0f, WHITE, 45); // shine
        ellipse((float)UI_W / 2, 50.0f, 95.0f, 46.0f, GREEN, 255);
        // Red letters, fat yellow outline, dark shadow.
        ftext_c(cx, -10, "Flashplayer Emu", 80, RED, YELLOW, 5);
    }
}

static void draw_header(int ngames) {
    theme_header_base(); // what's behind the side animations (theme_animate)
    draw_logo();

    // Link strip between yellow rules, words in different colours.
    rect(14, 100, UI_W - 28, 3, YELLOW, 255);
    rect(14, 139, UI_W - 28, 3, YELLOW, 255);
    char games[32];
    snprintf(games, sizeof(games), "GAMES (%d)", ngames);
    struct { const char* s; uint32_t c; } items[] = {
        { games, RED }, { ">>>", YELLOW }, { "touch = mouse", g_theme ? LINK_BLUE : BLUE }, { "d-pad = arrows", GREEN },
        { "A = space", WHITE }, { "+ = enter", g_theme ? LINK_BLUE : PURPLE }, { "- = menu", ORANGE },
    };
    int n = (int)(sizeof(items) / sizeof(items[0])), total = 0;
    for (int i = 0; i < n; i++) total += ftext_w(items[i].s, 28);
    int gap = (UI_W - 60 - total) / (n - 1), x = 30;
    for (int i = 0; i < n; i++) {
        ftext(x, 101, items[i].s, 28, items[i].c, NO_LINE, 0);
        x += ftext_w(items[i].s, 28) + gap;
        if (i >= 2 && i < n - 1) rect(x - gap / 2 - 1, 108, 3, 24, YELLOW, 255);
    }
}

static void header(int ngames) {
    if (!g_header) g_header = (uint8_t*)malloc((size_t)UI_W * HEADER_H * 4);
    if (g_header && g_header_games == ngames) {
        memcpy(C, g_header, (size_t)UI_W * HEADER_H * 4);
        return;
    }
    draw_header(ngames);
    if (g_header) {
        memcpy(g_header, C, (size_t)UI_W * HEADER_H * 4);
        g_header_games = ngames;
    }
}

// ---------------------------------------------------------------- picker layout

// Vertical plan (no overlaps): title 0..~96, link strip 100..142, red box 146..614 (heading 146..~199,
// grid 206..606), footer rows from 616 and 662.
#define BOX_X 14
#define BOX_Y 146
#define BOX_W (UI_W - 28)
#define BOX_H 468
#define GRID_X 30
#define GRID_Y 206
#define TILE_W 293
#define TILE_H 200
#define GAP 0
#define GRID_W (TILE_W * UI_COLS)
#define BAR_X (GRID_X + GRID_W + 10) // scroll bar
#define BAR_W 28
#define FOOT_Y 616

#define SET_W 190
#define SET_H 44
#define SET_X (BOX_X + BOX_W - SET_W - 14)
#define SET_Y (BOX_Y + 8)

static int g_top_row;

int ui_settings_button_at(int x, int y) {
    return x >= SET_X - 6 && x < SET_X + SET_W + 6 && y >= SET_Y - 6 && y < SET_Y + SET_H + 6;
}

int ui_tile_at(int n, int x, int y) {
    if (x < GRID_X || y < GRID_Y || x >= GRID_X + GRID_W) return -1;
    int col = (x - GRID_X) / (TILE_W + GAP), row = (y - GRID_Y) / (TILE_H + GAP);
    if (col >= UI_COLS || row >= UI_ROWS) return -1;
    int i = (g_top_row + row) * UI_COLS + col;
    return i < n ? i : -1;
}

int ui_tile_middle_at(int n, int x, int y) {
    int i = ui_tile_at(n, x, y);
    if (i < 0) return -1;
    // The middle half of the picture (y + 12 .. y + 162 in the cell, see tile()).
    int cx = GRID_X + ((x - GRID_X) / TILE_W) * TILE_W + TILE_W / 2;
    int cy = GRID_Y + ((y - GRID_Y) / TILE_H) * TILE_H + 12 + UI_THUMB_H / 2;
    if (x < cx - UI_THUMB_W / 4 || x > cx + UI_THUMB_W / 4 || y < cy - UI_THUMB_H / 4 || y > cy + UI_THUMB_H / 4) return -1;
    return i;
}

int ui_insert_at(int n, int x, int y) {
    if (x < GRID_X - 20 || x >= GRID_X + GRID_W + 20 || y < GRID_Y || y >= GRID_Y + UI_ROWS * TILE_H) return -1;
    if (x < GRID_X) x = GRID_X;
    if (x >= GRID_X + GRID_W) x = GRID_X + GRID_W - 1;
    int col = (x - GRID_X) / TILE_W, row = (y - GRID_Y) / TILE_H;
    int i = (g_top_row + row) * UI_COLS + col;
    if (x - GRID_X - col * TILE_W >= TILE_W / 2) i++; // the right half: after it
    return i > n ? n : i;
}

// Folder pages and dragging (set by main_wiiu.c before drawing).
static char g_folder_name[64];
static int g_in_folder;
static int g_drag_index = -1, g_drag_x, g_drag_y, g_drag_target = -1, g_drag_insert = -1, g_drag_back;

#define BACK_X (BOX_X + 14)
#define BACK_W 150

void ui_set_folder(const char* name) {
    g_in_folder = name != NULL;
    snprintf(g_folder_name, sizeof(g_folder_name), "%s", name ? name : "");
}

int ui_back_button_at(int x, int y) {
    return g_in_folder && x >= BACK_X - 6 && x < BACK_X + BACK_W + 6 && y >= SET_Y - 6 && y < SET_Y + SET_H + 6;
}

void ui_set_drag(int index, int x, int y, int target, int insert, int on_back) {
    g_drag_index = index;
    g_drag_x = x;
    g_drag_y = y;
    g_drag_target = target;
    g_drag_insert = insert;
    g_drag_back = on_back;
}

int ui_top_row(void) {
    return g_top_row;
}

void ui_set_top_row(int row, int n) {
    int rows = (n + UI_COLS - 1) / UI_COLS;
    if (row > rows - UI_ROWS) row = rows - UI_ROWS;
    if (row < 0) row = 0;
    g_top_row = row;
}

static int g_dragging;

int ui_scroll_at(int n, int x, int y, int first) {
    int rows = (n + UI_COLS - 1) / UI_COLS;
    if (rows <= UI_ROWS) return -1;
    int on_bar = x >= BAR_X - 16 && x < BAR_X + BAR_W + 16 && y >= GRID_Y - 10 && y < GRID_Y + UI_ROWS * TILE_H + 10;
    if (first) g_dragging = on_bar;
    if (!g_dragging) return -1;
    float f = (float)(y - GRID_Y) / (float)(UI_ROWS * TILE_H);
    if (f < 0.0f) f = 0.0f;
    if (f > 0.999f) f = 0.999f;
    return (int)(f * (float)rows);
}

static void scroll_bar(int n) {
    int rows = (n + UI_COLS - 1) / UI_COLS;
    if (rows <= UI_ROWS) return;
    int h = UI_ROWS * TILE_H;
    // Track: black with a yellow border, up/down triangles at the ends.
    frame(BAR_X, GRID_Y, BAR_W, h, 3, YELLOW);
    float up[6] = { (float)BAR_X + BAR_W / 2.0f, (float)GRID_Y + 8, (float)BAR_X + 6, (float)GRID_Y + 24,
                    (float)BAR_X + BAR_W - 6, (float)GRID_Y + 24 };
    float dn[6] = { (float)BAR_X + BAR_W / 2.0f, (float)(GRID_Y + h - 8), (float)BAR_X + 6, (float)(GRID_Y + h - 24),
                    (float)BAR_X + BAR_W - 6, (float)(GRID_Y + h - 24) };
    polygon(up, 3, RED, 255);
    polygon(dn, 3, RED, 255);
    // Thumb: how much of the list is on screen, and where.
    int track = h - 60, ty = GRID_Y + 30;
    int th = track * UI_ROWS / rows;
    if (th < 24) th = 24;
    int pos = rows > UI_ROWS ? (track - th) * g_top_row / (rows - UI_ROWS) : 0;
    rect(BAR_X + 5, ty + pos + 2, BAR_W - 10, th, darker(GREEN, 40), 255);
    rect(BAR_X + 5, ty + pos, BAR_W - 10, th, GREEN, 255);
    rect(BAR_X + 8, ty + pos + 4, 4, th - 8, WHITE, 90);
}

// ---------------------------------------------------------------- game info / tiles

void ui_game_info(UiGame* g, const char* sd_root) {
    const char* name = strrchr(g->path, '/');
    name = name ? name + 1 : g->path;
    snprintf(g->title, sizeof(g->title), "%s", name);
    char* dot = strrchr(g->title, '.');
    if (dot && !strcmp(dot, ".swf")) *dot = 0;
    for (char* p = g->title; *p; p++)
        if (*p == '_' || *p == '-') *p = ' ';
    if (g->title[0] >= 'a' && g->title[0] <= 'z') g->title[0] = (char)(g->title[0] - 32);

    char full[400];
    snprintf(full, sizeof(full), "%s%s", sd_root, g->path);
    FILE* f = fopen(full, "rb");
    g->version = 0;
    g->size = 0;
    if (!f) return;
    uint8_t h[8];
    if (fread(h, 1, 8, f) == 8 && (h[0] == 'F' || h[0] == 'C' || h[0] == 'Z') && h[1] == 'W' && h[2] == 'S')
        g->version = h[3];
    fseek(f, 0, SEEK_END);
    g->size = (uint32_t)ftell(f);
    fclose(f);
}

static void fmt_size(uint32_t n, char* out, size_t len) {
    if (n >= 1024 * 1024) snprintf(out, len, "%u.%u MB", n >> 20, (n % (1024 * 1024)) * 10 >> 20);
    else snprintf(out, len, "%u KB", (n + 1023) >> 10);
}

// A game picture (UI_THUMB_W x UI_THUMB_H) scaled into (px, py, w, h).
static void scaled_thumb(const uint8_t* thumb, int px, int py, int w, int h) {
    for (int yy = 0; yy < h; yy++) {
        int dy = py + yy;
        if ((unsigned)dy >= UI_H) continue;
        const uint8_t* row = thumb + (size_t)(yy * UI_THUMB_H / h) * UI_THUMB_W * 4;
        uint8_t* dst = C + (size_t)dy * UI_W * 4;
        for (int xx = 0; xx < w; xx++) {
            int dx = px + xx;
            if ((unsigned)dx < UI_W) memcpy(dst + dx * 4, row + (xx * UI_THUMB_W / w) * 4, 4);
        }
    }
}

// A folder: an orange card with its first four games' pictures in a 2 x 2 grid and how many it holds.
static void folder_picture(const UiGame* g, int px, int py, int iw, int ih) {
    rect(px, py, iw, ih, ORANGE, 255);
    rect(px, py, iw / 3, 8, YELLOW, 255); // the tab
    int gap = 6, cw = (iw - gap * 3) / 2, ch = (ih - 12 - gap * 3) / 2;
    for (int i = 0; i < 4; i++) {
        int cx = px + gap + (i % 2) * (cw + gap), cy = py + 12 + gap + (i / 2) * (ch + gap);
        if (g->sub[i]) scaled_thumb(g->sub[i], cx, cy, cw, ch);
        else rect(cx, cy, cw, ch, darker(ORANGE, 70), 255);
    }
    char n[12];
    snprintf(n, sizeof(n), "%d", g->count);
    int bw = ftext_w(n, 26) + 16;
    ellipse((float)(px + iw - bw / 2 - 2), (float)(py + 14), (float)bw / 2.0f, 15.0f, RED, 255);
    ftext_c(px + iw - bw / 2 - 2, py - 2, n, 26, WHITE, NO_LINE, 2);
}

// The game's picture (zoom 1: copied; otherwise scaled to iw x ih), or a big letter if it has none yet.
static void picture(const UiGame* g, int index, int px, int py, int iw, int ih) {
    if (g->folder) {
        folder_picture(g, px, py, iw, ih);
    } else if (g->thumb && iw == UI_THUMB_W && ih == UI_THUMB_H) {
        for (int yy = 0; yy < UI_THUMB_H; yy++) {
            int dy = py + yy;
            if ((unsigned)dy >= UI_H) continue;
            const uint8_t* src = g->thumb + (size_t)yy * UI_THUMB_W * 4;
            int x0 = px < 0 ? -px : 0, x1 = px + UI_THUMB_W > UI_W ? UI_W - px : UI_THUMB_W;
            if (x1 > x0) memcpy(C + ((size_t)dy * UI_W + px + x0) * 4, src + x0 * 4, (size_t)(x1 - x0) * 4);
        }
    } else if (g->thumb) {
        for (int yy = 0; yy < ih; yy++) {
            int dy = py + yy;
            if ((unsigned)dy >= UI_H) continue;
            const uint8_t* row = g->thumb + (size_t)(yy * UI_THUMB_H / ih) * UI_THUMB_W * 4;
            uint8_t* dst = C + (size_t)dy * UI_W * 4;
            for (int xx = 0; xx < iw; xx++) {
                int dx = px + xx;
                if ((unsigned)dx < UI_W) memcpy(dst + dx * 4, row + (xx * UI_THUMB_W / iw) * 4, 4);
            }
        }
    } else {
        // No picture yet: a big letter on a bright square, like the puzzle pictures on those pages.
        uint32_t c = BRIGHT[(index + 3) % NBRIGHT];
        rect(px, py, iw, ih, c == YELLOW ? BLUE : c, 255);
        char letter[2] = { g->title[0] ? g->title[0] : '?', 0 };
        ftext_c(px + iw / 2, py + ih / 2 - ih * 6 / 10, letter, ih * 9 / 10, YELLOW, 0x000000, 3);
    }
}

// zoom > 1 grows the picture around its center; flash (0..1) washes the cell white (the launch animation).
static void tile(const UiGame* g, int index, int x, int y, int selected, float t, float zoom, float flash) {
    // Cell plan: green selection frame y+2..y+6, picture y+12..y+162 (its border from y+9 to y+165),
    // name y+162 (letters ~y+171..y+197), underline y+192, cell divider from y+198.
    int iw = (int)(UI_THUMB_W * zoom), ih = (int)(UI_THUMB_H * zoom);
    int px = x + (TILE_W - iw) / 2, py = y + 12 - (ih - UI_THUMB_H) / 2;
    if (selected) {
        // A pulsing green box, like the highlighted section boxes.
        int glow = (int)(150.0f + 105.0f * (0.5f + 0.5f * sinf(t * 6.0f)));
        rect(x + 6, y + 6, TILE_W - 12, TILE_H - 12, T->sel, 255);
        frame(x + 2, y + 2, TILE_W - 4, TILE_H - 4, 4, GREEN);
        rect(x + 2, y + 2, TILE_W - 4, 4, WHITE, 255 - glow);
    }
    rect(px - 3, py - 3, iw + 6, ih + 6, selected ? YELLOW : PAGE_BG, 255);
    picture(g, index, px, py, iw, ih);
    // The name as a blue underlined link (yellow when selected).
    uint32_t c = selected ? YELLOW : LINK_BLUE;
    int max_w = TILE_W - 20, ny = y + 162;
    ftext_fit(x + TILE_W / 2, ny, g->title, 28, c, NO_LINE, 2, max_w, 1);
    int w = ftext_w(g->title, 28);
    if (w > max_w) w = max_w;
    rect(x + (TILE_W - w) / 2, ny + 30, w, 2, c, 255);
    if (flash > 0.0f) rect(x, y, TILE_W, TILE_H, WHITE, (int)(flash * 220.0f));
}

// Yellow lines between the cells, like a table of puzzles.
static void grid_lines(void) {
    frame(GRID_X - 4, GRID_Y - 4, GRID_W + 8, UI_ROWS * TILE_H + 8, 4, YELLOW);
    for (int c = 1; c < UI_COLS; c++) rect(GRID_X + c * TILE_W - 2, GRID_Y, 4, UI_ROWS * TILE_H, YELLOW, 255);
    for (int r = 1; r < UI_ROWS; r++) rect(GRID_X, GRID_Y + r * TILE_H - 2, GRID_W, 4, YELLOW, 255);
}


// ---- changed areas
// The console's CPU is slow at copying whole 1280x720 pictures (the menu ran well under 60 fps copying
// the cached base, then uploading it all, every frame). While the picker or a settings page stays up,
// only the areas that change are restored from the cached picture, redrawn and uploaded.
// Kinds of the last frame drawn into the canvas (a new kind means everything changed).
enum { KIND_OTHER, KIND_PICKER, KIND_SETTINGS, KIND_CREDITS };
static int g_kind = KIND_OTHER;
static uint32_t g_kind_key;
static const uint8_t* g_kind_canvas;
static int g_dirty[32][4], g_ndirty, g_dirty_all = 1;
static int g_prev[12][4], g_nprev; // what was drawn over the cached picture last frame

static void dirty_add(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < g_ndirty; i++) // (an area restored and redrawn is uploaded once)
        if (g_dirty[i][0] == x && g_dirty[i][1] == y && g_dirty[i][2] == w && g_dirty[i][3] == h) return;
    if (g_ndirty == 32) { g_dirty_all = 1; return; }
    int* d = g_dirty[g_ndirty++];
    d[0] = x; d[1] = y; d[2] = w; d[3] = h;
}

// Starts a frame of `kind`: 1 if the canvas still holds the last frame of the same kind and key (only
// changed areas need drawing), 0 if everything must be drawn.
static int frame_begin(int kind, uint32_t key) {
    int same = kind != KIND_OTHER && g_kind == kind && g_kind_key == key && g_kind_canvas == C;
    g_kind = kind;
    g_kind_key = key;
    g_kind_canvas = C;
    g_ndirty = 0;
    g_dirty_all = !same;
    if (!same) g_nprev = 0;
    return same;
}

// Puts back the areas drawn over the cached picture last frame (they are uploaded again too).
static void restore_prev(const uint8_t* from) {
    for (int i = 0; i < g_nprev; i++) {
        int* r = g_prev[i];
        for (int y = r[1]; y < r[1] + r[3]; y++)
            memcpy(C + ((size_t)y * UI_W + r[0]) * 4, from + ((size_t)y * UI_W + r[0]) * 4, (size_t)r[2] * 4);
        dirty_add(r[0], r[1], r[2], r[3]);
    }
    g_nprev = 0;
}

// An area drawn over the cached picture this frame.
static void drawn_over(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > UI_W) w = UI_W - x;
    if (y + h > UI_H) h = UI_H - y;
    if (w <= 0 || h <= 0 || g_nprev == 12) return;
    int* r = g_prev[g_nprev++];
    r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    dirty_add(x, y, w, h);
}

void ui_forget_screen(void) {
    g_kind = KIND_OTHER;
}

int ui_take_dirty(int (*rects)[4], int max) {
    int n;
    if (g_dirty_all || g_ndirty > max) {
        rects[0][0] = rects[0][1] = 0;
        rects[0][2] = UI_W;
        rects[0][3] = UI_H;
        n = 1;
    } else {
        for (int i = 0; i < g_ndirty; i++) memcpy(rects[i], g_dirty[i], sizeof(rects[i]));
        n = g_ndirty;
    }
    g_dirty_all = 1;
    g_ndirty = 0;
    return n;
}

// Everything on the picker that only changes with the scroll position or the game list (page, header,
// red box, heading, settings oval, grid with every visible game, scroll bar, footer captions). Drawn
// once into g_base and copied each frame; only the selection, the info line and the equalizer are drawn
// per frame. (Redrawing it all every frame took ~23 ms of CPU, too much for 60 fps.)
static uint8_t* g_base;
static uint32_t g_base_key;

static uint32_t key_str(uint32_t k, const char* s) {
    while (*s) k = (k ^ (uint8_t)*s++) * 16777619u;
    return k * 16777619u;
}

static uint32_t base_key(const UiGame* games, int n) {
    uint32_t k = 2166136261u;
    uint32_t parts[4] = { (uint32_t)n, (uint32_t)(uintptr_t)games, (uint32_t)g_drag_index, (uint32_t)g_in_folder };
    for (int i = 0; i < 4; i++) k = (k ^ parts[i]) * 16777619u;
    k = key_str(k, g_folder_name);
    for (int i = 0; i < n; i++) {
        // Each tile: its picture (or, for a folder, its pictures, count and name) and its game.
        k = (k ^ (uint32_t)(uintptr_t)games[i].thumb) * 16777619u;
        k = key_str(k, games[i].path);
        if (games[i].folder) {
            k = key_str(k, games[i].title);
            k = (k ^ (uint32_t)games[i].count) * 16777619u;
            for (int s = 0; s < 4; s++) k = (k ^ (uint32_t)(uintptr_t)games[i].sub[s]) * 16777619u;
        }
    }
    return k | 1; // never 0 (0 = nothing cached)
}

// The grid's lines, the tiles on screen and the scroll bar.
static void draw_grid(const UiGame* games, int n) {
    grid_lines();
    for (int r = 0; r < UI_ROWS; r++)
        for (int col = 0; col < UI_COLS; col++) {
            int i = (g_top_row + r) * UI_COLS + col;
            if (i >= n) break;
            if (i == g_drag_index) continue; // picked up: its place stays empty
            tile(&games[i], i, GRID_X + col * (TILE_W + GAP), GRID_Y + r * (TILE_H + GAP), 0, 0.0f, 1.0f, 0.0f);
        }
    scroll_bar(n);
}

static void draw_base(const UiGame* games, int n) {
    background();
    header(n);
    frame(BOX_X, BOX_Y, BOX_W, BOX_H, 5, RED);
    if (g_in_folder) {
        // A folder's page: its name, a back button (B) and a rename button (X).
        arrow_heading(GRID_X + GRID_W / 2, BOX_Y, g_folder_name, 42);
        oval_button(BACK_X, SET_Y, BACK_W, SET_H, BLUE, "< back", 30, 0);
        oval_button(SET_X, SET_Y, SET_W, SET_H, PURPLE, "rename", 30, 0);
    } else {
        arrow_heading(GRID_X + GRID_W / 2, BOX_Y, "Pick a game!", 42);
        oval_button(SET_X, SET_Y, SET_W, SET_H, PURPLE, "settings", 30, 0);
    }
    if (!n) {
        ftext_c(UI_W / 2, GRID_Y + 40, "No games yet!", 70, RED, YELLOW, 4);
        ftext_c(UI_W / 2, GRID_Y + 160, "Put .swf files in sd:/wiiu/apps/flashplayeremu/swf/", 34, WHITE, NO_LINE, 2);
        ftext_c(UI_W / 2, GRID_Y + 210, "(a game with XML or other files: give it its own folder in there)", 28, GREEN, NO_LINE, 2);
    } else {
        draw_grid(games, n);
    }
    const char* keys = g_in_folder ? "A = play   B = back   X = rename" : "A = play   L/R = page   + = settings";
    ftext(UI_W - 24 - ftext_w(keys, 30), FOOT_Y, keys, 30, YELLOW, NO_LINE, 2);
    ftext(90, FOOT_Y + 44, "Now playing:", 30, WHITE, NO_LINE, 2);
    const char* song = g_theme == UI_THEME_FLASH ? "\"The World's Hardest Game\" by Stephen Critoph"
                       : g_theme == UI_THEME_HALLOWEEN ? "\"Not As It Seems\" by Kevin MacLeod"
                                                       : "\"Slimers\" by (T-T)b";
    ftext_fit(90 + ftext_w("Now playing: ", 30), FOOT_Y + 44, song, 30, GREEN, NO_LINE, 2, 620, 0);
    // "Port by Charliebear31", the name in bright letters
    const char* by = "Port by ";
    const char* name = "Charliebear31";
    int x = UI_W - 24 - ftext_w(by, 30) - ftext_w(name, 30);
    ftext(x, FOOT_Y + 44, by, 30, WHITE, NO_LINE, 2);
    x += ftext_w(by, 30);
    for (int i = 0; name[i]; i++) {
        char ch[2] = { name[i], 0 };
        ftext(x, FOOT_Y + 44, ch, 30, BRIGHT[(i + 1) % NBRIGHT] == YELLOW ? ORANGE : BRIGHT[(i + 1) % NBRIGHT], NO_LINE, 2);
        x += ftext_w(ch, 30);
    }
}

// The chosen tile and its info line stay on the canvas until the choice changes; only the glow on its top
// edge and the equalizer change each frame. (Redrawing the tile every frame took most of the menu's time.)
static int g_sel_drawn = -1;
static int g_sel_rects[2][4], g_nsel_rects;
static uint32_t g_base_nokey; // the picture's key without the scroll position

static void copy_rect(uint8_t* to, const uint8_t* from, int x, int y, int w, int h);

static void restore_sel(void) {
    for (int i = 0; i < g_nsel_rects; i++) {
        int* r = g_sel_rects[i];
        copy_rect(C, g_base, r[0], r[1], r[2], r[3]);
        dirty_add(r[0], r[1], r[2], r[3]);
    }
    g_nsel_rects = 0;
    g_sel_drawn = -1;
}

static void sel_rect(int x, int y, int w, int h) {
    int* r = g_sel_rects[g_nsel_rects++];
    r[0] = x; r[1] = y; r[2] = w; r[3] = h;
    dirty_add(x, y, w, h);
}

// The chosen tile's details, bottom left.
static void info_line(const UiGame* g) {
    char info[96], size[24];
    fmt_size(g->size, size, sizeof(size));
    if (g->folder)
        snprintf(info, sizeof(info), "%s  (folder, %d game%s)", g->title, g->count, g->count == 1 ? "" : "s");
    else if (g->version) snprintf(info, sizeof(info), "%s  (SWF %d, %s)", g->title, g->version, size);
    else snprintf(info, sizeof(info), "%s  (%s)", g->title, size);
    ftext_fit(24, FOOT_Y, info, 30, WHITE, NO_LINE, 2, 600, 0);
}

void ui_draw_picker(uint8_t* canvas, const UiGame* games, int n, int sel, float t, float level, float launch) {
    C = canvas;
    if (n && g_drag_index < 0) { // (while dragging, the grid scrolls by itself: ui_set_top_row)
        int row = sel / UI_COLS;
        if (row < g_top_row) g_top_row = row;
        if (row >= g_top_row + UI_ROWS) g_top_row = row - UI_ROWS + 1;
    }
    uint32_t nokey = base_key(games, n);
    uint32_t key = ((nokey ^ (uint32_t)g_top_row) * 16777619u) | 1;
    if (!g_base) g_base = (uint8_t*)malloc((size_t)UI_W * UI_H * 4);
    int picker_up = g_kind == KIND_PICKER && g_kind_canvas == C;
    if (g_base && g_base_key == key && frame_begin(KIND_PICKER, key)) {
        restore_prev(g_base); // only last frame's equalizer (and dragged tile)
        if (g_drag_index >= 0 || launch > 0.0f || g_sel_drawn != sel) restore_sel();
    } else if (g_base && g_base_key && picker_up && g_base_nokey == nokey) {
        // Only scrolled: just the grid is drawn again and uploaded, not the whole screen.
        restore_prev(g_base);
        restore_sel();
        int saved[32][4], nsaved = g_ndirty;
        memcpy(saved, g_dirty, sizeof(saved));
        int gx = GRID_X - 4, gy = GRID_Y - 4, gw = BAR_X + BAR_W + 2 - gx, gh = UI_ROWS * TILE_H + 8;
        rect(gx, gy, gw, gh, PAGE_BG, 255);
        draw_grid(games, n);
        copy_rect(g_base, C, gx, gy, gw, gh);
        g_base_key = key;
        frame_begin(KIND_PICKER, key);
        g_dirty_all = 0;
        for (int i = 0; i < nsaved; i++) dirty_add(saved[i][0], saved[i][1], saved[i][2], saved[i][3]);
        dirty_add(gx, gy, gw, gh);
    } else if (g_base && g_base_key == key) {
        memcpy(C, g_base, (size_t)UI_W * UI_H * 4);
        g_sel_drawn = -1;
        g_nsel_rects = 0;
    } else {
        frame_begin(KIND_OTHER, 0);
        draw_base(games, n);
        g_sel_drawn = -1;
        g_nsel_rects = 0;
        if (g_base) {
            memcpy(g_base, C, (size_t)UI_W * UI_H * 4);
            g_base_key = key;
            g_base_nokey = nokey;
            frame_begin(KIND_PICKER, key); // from here on the canvas holds the cached picture
            g_dirty_all = 1;
        }
    }

    if (n && g_drag_index >= 0 && g_drag_index < n) {
        // Dragging: where it would land (a red bar between tiles, or a highlighted tile to make a folder
        // with), and the tile itself under the finger.
        if (g_drag_target >= 0) {
            int r = g_drag_target / UI_COLS - g_top_row, c = g_drag_target % UI_COLS;
            if (r >= 0 && r < UI_ROWS) {
                int x = GRID_X + c * TILE_W, y = GRID_Y + r * TILE_H;
                frame(x + 2, y + 2, TILE_W - 4, TILE_H - 4, 6, GREEN);
                const char* what = games[g_drag_target].folder ? "add to folder" : "make a folder";
                ftext_c(x + TILE_W / 2, y + 125, what, 30, YELLOW, 0x000000, 2);
                drawn_over(x, y, TILE_W, TILE_H);
            }
        } else if (g_drag_insert >= 0) {
            int slot = g_drag_insert, r = slot / UI_COLS - g_top_row, c = slot % UI_COLS;
            if (c == 0 && slot > 0 && r > 0 && (r - 1) < UI_ROWS && slot == n) { // after the last tile of a row
                r--;
                c = UI_COLS;
            }
            if (r >= 0 && r < UI_ROWS) {
                int x = GRID_X + c * TILE_W - 5, y = GRID_Y + r * TILE_H + 6;
                rect(x, y, 10, TILE_H - 12, RED, 255);
                rect(x + 3, y + 3, 4, TILE_H - 18, YELLOW, 255);
                drawn_over(x, y, 10, TILE_H - 12);
            }
        }
        if (g_drag_back) {
            oval_button(BACK_X, SET_Y, BACK_W, SET_H, GREEN, "< back", 30, 1);
            drawn_over(BACK_X - 8, SET_Y - 8, BACK_W + 16, SET_H + 16);
        }
        // The picture only, small, up and to the left of the finger: what's under the finger (the red
        // bar, the highlighted tile) stays in view.
        const UiGame* d = &games[g_drag_index];
        int iw = UI_THUMB_W * 7 / 10, ih = UI_THUMB_H * 7 / 10;
        int x = g_drag_x - iw - 14, y = g_drag_y - ih - 14;
        rect(x - 5, y - 5, iw + 10, ih + 10, YELLOW, 255);
        picture(d, g_drag_index, x, y, iw, ih);
        ftext_fit(x + iw / 2, y + ih + 4, d->title, 24, YELLOW, 0x000000, 2, iw + 60, 1);
        drawn_over(x - 50, y - 10, iw + 100, ih + 50);
    } else if (n) {
        // The highlight jumps straight to the chosen tile (it used to slide there, carrying the old picture).
        int sx = GRID_X + (sel % UI_COLS) * (TILE_W + GAP);
        int sy = GRID_Y + (sel / UI_COLS - g_top_row) * (TILE_H + GAP);
        if (launch > 0.0f) {
            // Launch: the chosen picture pops up (fast ease-out over the first third) and flashes white.
            float pop = fminf(launch * 3.0f, 1.0f);
            float zoom = 1.0f + 0.12f * (1.0f - (1.0f - pop) * (1.0f - pop));
            float flash = fmaxf(0.0f, 1.0f - launch * 4.0f);
            tile(&games[sel], sel, sx, sy, 1, t, zoom, flash);
            drawn_over(sx - 40, sy - 30, TILE_W + 80, TILE_H + 60); // with room for its zoom
            info_line(&games[sel]);
            drawn_over(0, FOOT_Y - 10, 640, 54);
        } else {
            if (g_sel_drawn != sel) {
                tile(&games[sel], sel, sx, sy, 1, t, 1.0f, 0.0f);
                sel_rect(sx, sy, TILE_W, TILE_H);
                info_line(&games[sel]);
                sel_rect(0, FOOT_Y - 10, 640, 54);
                g_sel_drawn = sel;
            }
            // The pulsing top edge: green to white, drawn opaque so it needs nothing restored under it.
            int w = (int)(127.5f * (0.5f + 0.5f * sinf(t * 6.0f)));
            uint32_t edge = (uint32_t)(((GREEN >> 16 & 255) * (255 - w) + 255 * w) / 255) << 16 |
                            (uint32_t)(((GREEN >> 8 & 255) * (255 - w) + 255 * w) / 255) << 8 |
                            (uint32_t)(((GREEN & 255) * (255 - w) + 255 * w) / 255);
            rect(sx + 2, sy + 2, TILE_W - 4, 4, edge, 255);
            dirty_add(sx + 2, sy + 2, TILE_W - 4, 4);
        }
    }
    {
        static int part;
        if (g_dirty_all) {
            for (int i = 0; i < 3; i++) theme_animate(t, i);
        } else {
            theme_animate(t, part);
            part = (part + 1) % 3;
        }
    }
    // equalizer bars
    for (int i = 0; i < 6; i++) {
        float hgt = 4.0f + level * 30.0f * (0.55f + 0.45f * sinf(t * (6.0f + (float)i * 1.7f) + (float)i));
        rect(24 + i * 9, UI_H - 14 - (int)hgt, 6, (int)hgt, BRIGHT[i], 255);
    }
    drawn_over(20, UI_H - 54, 60, 44);
}

// ---------------------------------------------------------------- loading screen, borders

void ui_draw_progress(uint8_t* canvas, int done, int total, const char* name, float t) {
    C = canvas;
    frame_begin(KIND_OTHER, 0);
    background();
    // A row of hopping shapes so the screen visibly keeps going.
    const uint32_t hop[4] = { YELLOW, BLUE, PURPLE, GREEN };
    for (int i = 0; i < 8; i++) {
        float bounce = fabsf(sinf(t * 4.0f + (float)i * 0.6f)) * 22.0f;
        shape(i % 4, (float)UI_W / 2 - 245.0f + (float)i * 70.0f, (float)BOX_Y + 430.0f - bounce, 18.0f, hop[i % 4]);
    }
    header(total);
    frame(BOX_X, BOX_Y, BOX_W, BOX_H, 5, RED);
    arrow_heading(UI_W / 2, BOX_Y + 40, "Taking game pictures", 54);
    ftext_fit(UI_W / 2, BOX_Y + 140, name, 50, LINK_BLUE, NO_LINE, 3, BOX_W - 80, 1);
    char count[64];
    snprintf(count, sizeof(count), "%d of %d  (only the first time a game is added)", done + 1, total);
    ftext_c(UI_W / 2, BOX_Y + 210, count, 32, GREEN, NO_LINE, 2);
    int bx = 120, bw = UI_W - 240, by = BOX_Y + 300;
    frame(bx, by, bw, 50, 5, YELLOW);
    int fw = total ? (bw - 20) * done / total : 0;
    for (int i = 0; i < NBRIGHT; i++) {
        int x0 = bx + 10 + (bw - 20) * i / NBRIGHT, x1 = bx + 10 + (bw - 20) * (i + 1) / NBRIGHT;
        if (x0 >= bx + 10 + fw) break;
        if (x1 > bx + 10 + fw) x1 = bx + 10 + fw;
        rect(x0, by + 10, x1 - x0, 30, BRIGHT[i], 255);
    }
}

void ui_draw_loading(uint8_t* canvas, const char* name) {
    C = canvas;
    frame_begin(KIND_OTHER, 0);
    background();
    ftext_c(UI_W / 2, UI_H / 2 - 100, "Loading...", 80, RED, YELLOW, 4);
    ftext_fit(UI_W / 2, UI_H / 2 + 20, name, 44, LINK_BLUE, NO_LINE, 3, UI_W - 120, 1);
}

void ui_draw_border(uint8_t* canvas, int sx, int sy, int sw, int sh) {
    C = canvas;
    frame_begin(KIND_OTHER, 0);
    background();
    // Thick red frame around the game.
    int f = 6;
    rect(sx - f, sy, f, sh, RED, 255);
    rect(sx + sw, sy, f, sh, RED, 255);
    int bar = sx - f; // width of each side bar
    if (bar < 8) return;

    // Left: shapes, then the title stacked letter by letter.
    const uint32_t cols[4] = { YELLOW, BLUE, PURPLE, GREEN };
    float r = bar >= 150 ? 22.0f : (float)bar * 0.15f;
    for (int i = 0; i < 4; i++)
        shape(i, (float)bar / 2 + (i % 2 ? r * 1.3f : -r * 1.3f), 34.0f + (float)(i / 2) * r * 2.7f, r, cols[i]);
    const char* words[] = { "FLASH", "EMU" };
    // Capitals with their outline are about 0.8 px tall; a step of 0.85 px keeps them apart, and the
    // whole column (8 letters) must fit under the shapes in 720 lines.
    int top = (int)(34.0f + r * 4.0f);
    int px = (UI_H - top - 30) * 100 / (8 * 85 + 40);
    if (px > 64) px = 64;
    if (px > bar * 8 / 10) px = bar * 8 / 10;
    int y = top - px * 25 / 100; // the letter cell starts above the capital
    for (int w = 0; w < 2; w++) {
        for (const char* p = words[w]; *p; p++) {
            char ch[2] = { *p, 0 };
            // In the same style as the name on the menu.
            if (g_theme == UI_THEME_FLASH) ftext_c(bar / 2, y, ch, px, w ? RED : 0xffffff, NO_LINE, 2);
            else if (g_theme == UI_THEME_HALLOWEEN) ftext_c(bar / 2, y, ch, px, ORANGE, 0x1a0a2e, 3);
            else ftext_c(bar / 2, y, ch, px, RED, YELLOW, 3);
            y += px * 85 / 100;
        }
        y += px * 40 / 100;
    }

    // Right: the controls, a card for each, with a picture of the button, as big as the bar allows.
    int m = bar >= 120 ? 10 : 3;
    int rx = sx + sw + f + m, pw = UI_W - rx - m;
    if (pw < 16) return;
    float k = pw >= 120 ? 1.0f : (float)pw / 120.0f; // of the full-size pictures
    const uint32_t accent[6] = { BLUE, RED, PURPLE, GREEN, ORANGE, YELLOW };
    const char* what[6] = { "mouse", "clicks", "arrows", "space", "enter", "menu" };
    int ch = 108, cy = 16;
    int lpx = (int)(30.0f * k);
    int cr = pw / 4 < 14 ? pw / 4 : 14;
    for (int i = 0; i < 6; i++) {
        uint32_t c = accent[i];
        rrect(rx + 3, cy + 4, pw, ch, cr, 0x000000);
        rrect(rx, cy, pw, ch, cr, c);
        rrect(rx + 3, cy + 3, pw - 6, ch - 6, cr > 3 ? cr - 3 : 0, darker(c, 30));
        float bx = (float)rx + (float)pw / 2, by = (float)cy + 40.0f;
        switch (i) {
        case 0: // left stick
            ellipse(bx, by, 25 * k, 25 * k, 0x101010, 255);
            ellipse(bx, by, 22 * k, 22 * k, 0x505050, 255);
            ellipse(bx, by, 15 * k, 15 * k, 0xd8d8d8, 255);
            break;
        case 1: { // L and R
            int bw = (int)(44.0f * k), bh = (int)(36.0f * k), gap = (int)(3.0f * k) + 1;
            int br = (int)(9.0f * k), fpx = (int)(32.0f * k);
            rrect((int)bx - bw - gap, (int)by - bh / 2, bw, bh, br, 0xe8e8e8);
            rrect((int)bx + gap, (int)by - bh / 2, bw, bh, br, 0xe8e8e8);
            ftext_c((int)bx - bw / 2 - gap, (int)by - fpx * 11 / 16, "L", fpx, 0x202020, NO_LINE, 0);
            ftext_c((int)bx + bw / 2 + gap, (int)by - fpx * 11 / 16, "R", fpx, 0x202020, NO_LINE, 0);
            break;
        }
        case 2: { // d-pad
            int a = (int)(26.0f * k), b = (int)(9.0f * k);
            rrect((int)bx - b, (int)by - a, b * 2, a * 2, (int)(4.0f * k), 0xe8e8e8);
            rrect((int)bx - a, (int)by - b, a * 2, b * 2, (int)(4.0f * k), 0xe8e8e8);
            ellipse(bx, by, 5 * k, 5 * k, 0x707070, 255);
            break;
        }
        default: { // a round button
            const char* face = i == 3 ? "A" : i == 4 ? "+" : "-";
            int fpx = (int)(38.0f * k);
            ellipse(bx, by, 25 * k, 25 * k, 0xe8e8e8, 255);
            ftext_c((int)bx, (int)by - fpx * 2 / 3, face, fpx, 0x202020, NO_LINE, 0);
            break;
        }
        }
        if (lpx >= 9) ftext_c((int)bx, cy + 68 + (30 - lpx) / 2, what[i], lpx, 0xffffff, NO_LINE, lpx >= 20 ? 1 : 0);
        cy += ch + 8;
    }
}
// ---------------------------------------------------------------- settings / credits panels

// The menu behind the settings and credits pages, darkened: made once per menu picture (g_base), not
// each time a page changes (darkening the whole screen was slow on the console).
static uint8_t* g_dim;
static uint32_t g_dim_key;

// Puts the darkened menu on the canvas; 0 if there's no menu picture yet.
static int dim_background(void) {
    if (!g_base) return 0;
    if (!g_dim) g_dim = (uint8_t*)malloc((size_t)UI_W * UI_H * 4);
    if (g_dim && g_dim_key == g_base_key) {
        memcpy(C, g_dim, (size_t)UI_W * UI_H * 4);
        return 1;
    }
    memcpy(C, g_base, (size_t)UI_W * UI_H * 4);
    dim_all();
    if (g_dim) {
        memcpy(g_dim, C, (size_t)UI_W * UI_H * 4);
        g_dim_key = g_base_key;
    }
    return 1;
}

// Copies rows of the rect (x, y, w, h) from one canvas-sized picture to another.
static void copy_rect(uint8_t* to, const uint8_t* from, int x, int y, int w, int h) {
    for (int r = y; r < y + h; r++)
        memcpy(to + ((size_t)r * UI_W + x) * 4, from + ((size_t)r * UI_W + x) * 4, (size_t)w * 4);
}

// A black panel inside a thick red border, and an arrow heading (over the darkened menu).
static void panel(int x, int y, int w, int h, const char* title) {
    rect(x, y, w, h, PAGE_BG, 255);
    frame(x, y, w, h, 6, RED);
    frame(x + 10, y + 10, w - 20, h - 20, 3, YELLOW);
    arrow_heading(x + w / 2, y + 16, title, 50);
}

#define SP_X 200
#define SP_Y 40
#define SP_W 880
#define SP_ROW_Y (SP_Y + 100)
#define SP_ROW_H 66

int ui_settings_row_at(int n, int x, int y) {
    if (x < SP_X + 30 || x >= SP_X + SP_W - 30 || y < SP_ROW_Y) return -1;
    int r = (y - SP_ROW_Y) / SP_ROW_H;
    return r < n ? r : -1;
}

// The settings and credits pages are drawn once into g_over and copied while nothing on them changes
// (dimming the whole screen and drawing the panel every frame made the menu stutter); only the glow on
// the chosen row is drawn each frame. The menu behind stays as it was when the page was drawn.
static uint8_t* g_over;
static uint32_t g_over_key;
static void draw_settings_page(const char* const* labels, const char* const* values, int n, int sel);

static uint32_t hash_str(uint32_t h, const char* s) {
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h * 16777619u;
}

// 1 = the cached page was copied to the canvas; 0 = draw it, then call over_save(key).
static int over_load(uint32_t key, int kind) {
    if (!g_over) g_over = (uint8_t*)malloc((size_t)UI_W * UI_H * 4);
    if (!g_over || g_over_key != key) return 0;
    memcpy(C, g_over, (size_t)UI_W * UI_H * 4);
    frame_begin(kind, key); // the canvas now holds the cached page
    g_dirty_all = 1;
    return 1;
}

static void over_save(uint32_t key) {
    if (!g_over) return;
    memcpy(g_over, C, (size_t)UI_W * UI_H * 4);
    g_over_key = key;
}

// Each settings row's finished picture, plain and chosen, so moving between rows is a copy.
#define ROW_BYTES ((size_t)(SP_W - 60) * SP_ROW_H * 4)
static uint8_t* g_row_px[UI_SETTINGS_MAX][2];
static uint32_t g_row_key[UI_SETTINGS_MAX][2];
static uint32_t g_row_shown[UI_SETTINGS_MAX]; // what each row on the page looks like now
static int g_rows_on_page;

static void draw_row(const char* const* labels, const char* const* values, int i, int sel) {
    const uint32_t rows[] = { BLUE, PURPLE, RED, GREEN, ORANGE };
    int y = SP_ROW_Y + i * SP_ROW_H;
    uint32_t c = rows[i % 5];
    rect(SP_X + 30, y - 6, SP_W - 60, SP_ROW_H, PAGE_BG, 255);
    if (i == sel) frame(SP_X + 30, y - 4, SP_W - 60, SP_ROW_H - 4, 4, GREEN);
    slant_button(SP_X + 44, y + 2, SP_W - 88, SP_ROW_H - 16, i == sel ? c : darker(c, 70), i % 2);
    ftext(SP_X + 70, y - 2, labels[i], 38, YELLOW, 0x000000, 0);
    if (values[i][0]) {
        char v[48];
        snprintf(v, sizeof(v), "< %s >", values[i]);
        ftext(SP_X + SP_W - 70 - ftext_w(v, 38), y - 2, v, 38, WHITE, 0x000000, 0);
    }
}

// Puts row i on the canvas if it looks different now; 1 if it did.
static int row_paint(const char* const* labels, const char* const* values, int i, int sel) {
    int st = i == sel, x = SP_X + 30, y = SP_ROW_Y + i * SP_ROW_H - 6, w = SP_W - 60;
    uint32_t key = (hash_str(hash_str(2166136261u, labels[i]), values[i]) << 1) | (uint32_t)st | 0x80000000u;
    if (g_row_shown[i] == key) return 0;
    g_row_shown[i] = key;
    if (!g_row_px[i][st]) g_row_px[i][st] = (uint8_t*)malloc(ROW_BYTES);
    uint8_t* px = g_row_px[i][st];
    if (px && g_row_key[i][st] == key) {
        for (int r = 0; r < SP_ROW_H; r++)
            memcpy(C + ((size_t)(y + r) * UI_W + x) * 4, px + (size_t)r * w * 4, (size_t)w * 4);
        return 1;
    }
    draw_row(labels, values, i, sel);
    if (px) {
        for (int r = 0; r < SP_ROW_H; r++)
            memcpy(px + (size_t)r * w * 4, C + ((size_t)(y + r) * UI_W + x) * 4, (size_t)w * 4);
        g_row_key[i][st] = key;
    }
    return 1;
}

void ui_draw_settings(uint8_t* canvas, const char* const* labels, const char* const* values, int n, int sel, float t) {
    C = canvas;
    uint32_t key = g_base_key * 31u + 1u + (uint32_t)n * 7u + (uint32_t)sel * 131u;
    for (int i = 0; i < n; i++) key = hash_str(hash_str(key, labels[i]), values[i]);
    if (!key) key = 1;
    int showing_page = g_kind == KIND_SETTINGS && g_kind_canvas == C && g_over && g_rows_on_page == n;
    if (g_over && g_over_key == key && frame_begin(KIND_SETTINGS, key)) {
        restore_prev(g_over); // just the glow strip
    } else if (showing_page && g_over_key != key) {
        // Another row chosen or a value changed: only the rows that look different are put back and
        // uploaded (redrawing the panel on each press lagged on the console).
        frame_begin(KIND_SETTINGS, key);
        g_dirty_all = 0;
        for (int i = 0; i < n; i++) {
            if (!row_paint(labels, values, i, sel)) continue;
            int y = SP_ROW_Y + i * SP_ROW_H - 6;
            copy_rect(g_over, C, SP_X + 30, y, SP_W - 60, SP_ROW_H);
            dirty_add(SP_X + 30, y, SP_W - 60, SP_ROW_H);
        }
        g_over_key = key;
    } else if (!over_load(key, KIND_SETTINGS)) {
        frame_begin(KIND_OTHER, 0);
        dim_background(); // the menu behind, without its selection
        draw_settings_page(labels, values, n, sel);
        over_save(key);
        frame_begin(KIND_SETTINGS, key);
        g_dirty_all = 1;
    }
    if (sel >= 0 && sel < n) {
        int glow = (int)(180.0f + 75.0f * (0.5f + 0.5f * sinf(t * 6.0f)));
        rect(SP_X + 30, SP_ROW_Y + sel * SP_ROW_H - 4, SP_W - 60, 4, WHITE, 255 - glow);
        drawn_over(SP_X + 30, SP_ROW_Y + sel * SP_ROW_H - 4, SP_W - 60, 4);
    }
}

static void draw_settings_page(const char* const* labels, const char* const* values, int n, int sel) {
    int h = 100 + n * SP_ROW_H + 70;
    panel(SP_X, SP_Y, SP_W, h, "Settings");
    for (int i = 0; i < n; i++) {
        g_row_shown[i] = 0; // the page was just cleared
        row_paint(labels, values, i, sel);
    }
    g_rows_on_page = n;
    ftext_c(SP_X + SP_W / 2, SP_Y + h - 62, "up/down choose   left/right or A change   B back", 30, LINK_BLUE, NO_LINE, 2);
}

void ui_draw_credits(uint8_t* canvas, float t) {
    C = canvas;
    (void)t;
    uint32_t key = g_base_key * 31u + 2u;
    if (!key) key = 2;
    if (g_over && g_over_key == key && frame_begin(KIND_CREDITS, key)) return; // nothing changes
    if (over_load(key, KIND_CREDITS)) return;
    frame_begin(KIND_OTHER, 0);
    dim_background();
    panel(SP_X, SP_Y, SP_W, 620, "Credits");
    const struct { const char* line; uint32_t color; int px; } lines[] = {
        { "Flashplayer Emu", RED, 54 },
        { "Port by Charliebear31", YELLOW, 40 },
        { "", 0, 10 },
        { "Menu music", GREEN, 32 },
        { "\"Slimers\" by (T-T)b", WHITE, 28 },
        { "\"The World's Hardest Game\" by Stephen Critoph", WHITE, 28 },
        { "\"Not As It Seems\" by Kevin MacLeod (incompetech.com), CC BY", WHITE, 28 },
        { "Flash engine", LINK_BLUE, 32 },
        { "Ruffle - ruffle.rs (MIT / Apache-2.0)", WHITE, 28 },
        { "", 0, 10 },
        { "Also using", PURPLE, 32 },
        { "wasm2c (WABT), devkitPro / wut, minimp3,", WHITE, 28 },
        { "CafeGLSL, Chewy font (Apache-2.0)", WHITE, 28 },
    };
    // A line of size px needs about 1.2 px (letters with descenders and outline): 140..~540.
    int y = SP_Y + 96;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        const char* s = lines[i].line;
        if (s[0]) ftext_c(SP_X + SP_W / 2, y, s, lines[i].px, lines[i].color, i == 0 ? YELLOW : NO_LINE, 2);
        y += s[0] ? lines[i].px * 6 / 5 : lines[i].px;
    }
    ftext_c(SP_X + SP_W / 2, SP_Y + 620 - 62, "A or B back", 30, LINK_BLUE, NO_LINE, 2);
    over_save(key);
    frame_begin(KIND_CREDITS, key);
    g_dirty_all = 1;
}

void ui_set_theme(int theme) {
    if (theme < 0 || theme >= UI_THEME_COUNT || theme == g_theme) return;
    g_theme = theme;
    T = &THEMES[theme];
    // Everything cached was drawn in the old colours.
    g_header_games = -1;
    g_base_key = 0;
    g_dim_key = 0;
    g_over_key = 0;
    memset(g_row_key, 0, sizeof(g_row_key));
    memset(g_row_shown, 0, sizeof(g_row_shown));
    g_kind = KIND_OTHER;
}

void ui_cursor_pixels(uint8_t* rgba) {
    static const char* const ART[] = {
        "kk..............",
        "kwk.............",
        "kwwk............",
        "kwwwk...........",
        "kwwwwk..........",
        "kwwwwwk.........",
        "kwwwwwwk........",
        "kwwwwwwwk.......",
        "kwwwwwwwwk......",
        "kwwwwwwwwwk.....",
        "kwwwwwwwwwwk....",
        "kwwwwwwwwwwwk...",
        "kwwwwwwwkkkkkk..",
        "kwwwkwwwk.......",
        "kwwk.kwwwk......",
        "kwk..kwwwk......",
        "kk....kwwwk.....",
        "......kwwwk.....",
        ".......kkk......",
    };
    memset(rgba, 0, UI_CURSOR_W * UI_CURSOR_H * 4);
    const int s = 3, scale_den = 2; // 1.5x, so it's easy to see on the TV
    for (int y = 0; y < UI_CURSOR_H; y++)
        for (int x = 0; x < UI_CURSOR_W; x++) {
            int ay = y * scale_den / s, ax = x * scale_den / s;
            if (ay >= (int)(sizeof(ART) / sizeof(ART[0]))) continue;
            size_t len = strlen(ART[ay]);
            char p = (size_t)ax < len ? ART[ay][ax] : '.';
            if (p == '.') continue;
            uint8_t v = p == 'w' ? 255 : 0;
            uint8_t* o = rgba + (y * UI_CURSOR_W + x) * 4;
            o[0] = o[1] = o[2] = v;
            o[3] = 255;
        }
}

// ---------------------------------------------------------------- on-screen keyboard
// 13 columns, 4 rows; cells are 58 x 56.
typedef struct { const char* label; uint32_t code; uint8_t row, col, span; } OskKey;
static const OskKey OSK[UI_OSK_KEYS] = {
    { "esc", RW_KEY_ESCAPE, 0, 0, 1 }, { "1", '1', 0, 1, 1 }, { "2", '2', 0, 2, 1 }, { "3", '3', 0, 3, 1 },
    { "4", '4', 0, 4, 1 }, { "5", '5', 0, 5, 1 }, { "6", '6', 0, 6, 1 }, { "7", '7', 0, 7, 1 }, { "8", '8', 0, 8, 1 },
    { "9", '9', 0, 9, 1 }, { "0", '0', 0, 10, 1 }, { "back", RW_KEY_BACKSPACE, 0, 11, 2 },
    { "tab", RW_KEY_TAB, 1, 0, 1 }, { "Q", 'q', 1, 1, 1 }, { "W", 'w', 1, 2, 1 }, { "E", 'e', 1, 3, 1 }, { "R", 'r', 1, 4, 1 },
    { "T", 't', 1, 5, 1 }, { "Y", 'y', 1, 6, 1 }, { "U", 'u', 1, 7, 1 }, { "I", 'i', 1, 8, 1 }, { "O", 'o', 1, 9, 1 },
    { "P", 'p', 1, 10, 1 }, { "enter", RW_KEY_ENTER, 1, 11, 2 },
    { "shift", RW_KEY_SHIFT, 2, 0, 2 }, { "A", 'a', 2, 2, 1 }, { "S", 's', 2, 3, 1 }, { "D", 'd', 2, 4, 1 }, { "F", 'f', 2, 5, 1 },
    { "G", 'g', 2, 6, 1 }, { "H", 'h', 2, 7, 1 }, { "J", 'j', 2, 8, 1 }, { "K", 'k', 2, 9, 1 }, { "L", 'l', 2, 10, 1 },
    { "^", RW_KEY_UP, 2, 11, 1 }, { "X", 0, 2, 12, 1 },
    { "ctrl", RW_KEY_CONTROL, 3, 0, 2 }, { "Z", 'z', 3, 2, 1 }, { "X", 'x', 3, 3, 1 }, { "C", 'c', 3, 4, 1 }, { "V", 'v', 3, 5, 1 },
    { "B", 'b', 3, 6, 1 }, { "N", 'n', 3, 7, 1 }, { "M", 'm', 3, 8, 1 }, { "spc", ' ', 3, 9, 1 },
    { "<", RW_KEY_LEFT, 3, 10, 1 }, { "v", RW_KEY_DOWN, 3, 11, 1 }, { ">", RW_KEY_RIGHT, 3, 12, 1 }, { "", 0xffffffffu, 2, 1, 0 },
};

uint32_t ui_osk_code(int key) {
    return key >= 0 && key < UI_OSK_KEYS ? OSK[key].code : 0xffffffffu;
}

int ui_osk_key_at(int x, int y) {
    x -= UI_OSK_X + 7;
    y -= UI_OSK_Y + 8;
    if (x < 0 || y < 0) return -1;
    int col = x / 58, row = y / 56;
    for (int i = 0; i < UI_OSK_KEYS; i++)
        if (OSK[i].row == row && col >= OSK[i].col && col < OSK[i].col + OSK[i].span) return i;
    return -1;
}

void ui_draw_osk(uint8_t* canvas, const uint8_t* held) {
    C = canvas;
    rrect(0, 0, UI_OSK_W, UI_OSK_H, 12, RED);
    rrect(3, 3, UI_OSK_W - 6, UI_OSK_H - 6, 10, PAGE_BG);
    for (int i = 0; i < UI_OSK_KEYS; i++) {
        const OskKey* k = &OSK[i];
        if (!k->span) continue;
        int x = 7 + k->col * 58, y = 8 + k->row * 56, w = k->span * 58 - 4;
        int special = k->span > 1 || !k->code || k->code > 255;
        uint32_t c = held[i] ? YELLOW : !k->code ? RED : special ? darker(BLUE, 75) : darker(BLUE, 45);
        rrect(x, y, w, 52, 8, c);
        int px = k->label[1] ? 22 : 30;
        ftext_c(x + w / 2, y + (k->label[1] ? 10 : 4), k->label, px, held[i] ? 0x000000 : WHITE, NO_LINE, 0);
    }
}

void ui_draw_osk_tab(uint8_t* canvas, int open) {
    C = canvas;
    int y = UI_OSK_TAB_CANVAS_Y;
    rect(0, y, UI_OSK_TAB_W, UI_OSK_TAB_H, 0x000000, 255);
    rrect(0, y, UI_OSK_TAB_W - 2, UI_OSK_TAB_H - 2, 8, open ? YELLOW : RED);
    rrect(2, y + 2, UI_OSK_TAB_W - 6, UI_OSK_TAB_H - 6, 6, darker(BLUE, 45));
    // A little keyboard.
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 4; k++) {
            if (r == 2 && (k == 0 || k == 3)) continue;
            rect(7 + k * 7, y + 13 + r * 8, r == 2 ? 7 : 5, 5, WHITE, 255);
        }
}
