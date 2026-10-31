// GX2 implementation of the gfx_* layer (see gfx_gl.c for the PC twin).
// Ruffle draws into an offscreen "screen" target with a D24S8 buffer (masks need stencil, which the
// WHB TV/DRC buffers lack); gfx_gx2_present() then scales that texture onto the TV and the GamePad.
// Every surface is LINEAR_ALIGNED RGBA8 (bytes R,G,B,A) so the CPU can update and read it directly.
#include "../rw_core.h"
#include "gfx_gx2.h"
#include <coreinit/cache.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/display.h>
#include <gx2/registers.h>
#include <gx2/swap.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <gx2/state.h>
#include <gx2/surface.h>
#include <gx2/texture.h>
#include <gx2/utils.h>
#include <whb/gfx.h>
#include <cafeglsl/CafeGLSLCompiler.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const uint8_t flashplayer_gsh[];
extern const uint8_t filter_gsh[];

typedef struct {
    RwVertex* verts;
    uint32_t* idx;
    uint32_t nidx;
    int used;
} Mesh;

typedef struct {
    GX2Texture tex;
    GX2ColorBuffer cb;
    GX2DepthBuffer db;
    int has_target;
    int used;
    uint32_t drawn_epoch; // when a draw last read it (see gfx_tex_update)
} Tex;

static Mesh* g_meshes;
static uint32_t g_nmeshes;
static Tex* g_texs;
static uint32_t g_ntexs;
static Tex g_screen;
static Tex g_game_screen; // what games draw into (tiled)
static Tex* g_shown = &g_screen; // whichever of the two has the latest picture

static WHBGfxShaderGroup g_shader;
static uint32_t u_view, u_world0, u_world1, u_uv0, u_uv1, u_mult, u_add, u_params, s_tex;
static GX2Sampler g_samplers[4]; // [smooth][repeat]
static GX2Sampler g_ramp_sampler;

// GPU memory freed only after the GPU is done with the frame that may still use it. The GPU draws a
// frame while the CPU runs the game's next one, so memory freed during frame N is released once the
// GPU has finished frame N (at the end of frame N + 1).
static void** g_pending;
static uint32_t g_npending, g_cappending;
static void** g_prev_pending; // freed during the frame the GPU may still be drawing
static uint32_t g_prev_npending, g_prev_cap;
static OSTime g_prev_ts;      // that frame's last command

static void defer_free(void* p) {
    if (!p) return;
    if (g_npending == g_cappending) {
        g_cappending = g_cappending ? g_cappending * 2 : 1024;
        g_pending = (void**)realloc(g_pending, sizeof(void*) * g_cappending);
    }
    g_pending[g_npending++] = p;
}

// Our own GPU context state for everything drawn off-screen (game frames, Stage3D, the screen texture).
// It used to borrow WHB's TV state, but a context state remembers the last colour buffer set while it is
// active: on a real Wii U WHBGfxBeginRenderTV (which only restores that state) then drew the TV picture
// into our texture instead of the TV, so the TV stayed black once a game had drawn. Cemu didn't show it.
static GX2ContextState* g_ctx;

// Set once the system asks the app to exit (HOME / power): GX2 is being taken away then, and waiting on
// it (GX2DrawDone) crashed on the console. After that nothing waits on, reads from or draws with the GPU.
static int g_gpu_lost;

void gfx_gx2_gpu_lost(void) {
    g_gpu_lost = 1;
}

// Epochs say whether the GPU may still read a texture: draws queued since the last present or sync are in
// the current one, the frame the GPU may still be drawing in the previous one (0 once it's surely done).
static uint32_t g_epoch = 1, g_prev_epoch;
static int g_c3d_drew; // Stage3D draws don't mark their textures: this frame's Stage3D use means a full wait

static void gpu_sync(void) {
    if (!g_gpu_lost) GX2DrawDone();
    g_epoch++;
    g_prev_epoch = 0;
    g_c3d_drew = 0;
}

// Waits until the GPU has drawn the last frame's picture onto the TV and GamePad (it no longer reads the
// screen texture). Not the frame's end: that includes the swap, which waits for the TV's refresh (the
// menu sat ~8 ms a frame in that wait before writing its picture).
static OSTime g_blit_ts;

static void gpu_wait_last_frame(void) {
    if (!g_gpu_lost && g_blit_ts) GX2WaitTimeStamp(g_blit_ts);
}

// GPU memory in use (pictures, shapes, buffers), for the log and the memory limit: block sizes are kept
// in an open-addressing table by address (the heap's own size query gave nonsense).
static uint64_t g_gpu_bytes;
static uint32_t g_gpu_blocks;
static uintptr_t* g_blk_ptr;
static uint32_t* g_blk_size;
static uint32_t g_blk_cap;  // power of two
static uint32_t g_blk_used; // slots no longer empty (live + freed), kept under half the table
#define BLK_DEAD ((uintptr_t)1)

static uint32_t blk_hash(uintptr_t p) {
    return (uint32_t)((p >> 6) * 2654435761u);
}

static void blk_put(uintptr_t p, uint32_t size);

static void blk_grow(void) {
    uint32_t old_cap = g_blk_cap;
    uintptr_t* old_ptr = g_blk_ptr;
    uint32_t* old_size = g_blk_size;
    // Mostly freed slots: rebuild at the same size; otherwise twice as big.
    uint32_t cap = !old_cap ? 8192 : g_gpu_blocks * 4 < old_cap ? old_cap : old_cap * 2;
    uintptr_t* np = (uintptr_t*)calloc(cap, sizeof(uintptr_t));
    uint32_t* ns = (uint32_t*)calloc(cap, sizeof(uint32_t));
    if (!np || !ns) {
        free(np);
        free(ns);
        return;
    }
    g_blk_ptr = np;
    g_blk_size = ns;
    g_blk_cap = cap;
    g_blk_used = 0;
    for (uint32_t i = 0; i < old_cap; i++)
        if (old_ptr[i] > BLK_DEAD) blk_put(old_ptr[i], old_size[i]);
    free(old_ptr);
    free(old_size);
}

static void blk_put(uintptr_t p, uint32_t size) {
    if ((g_blk_used + 1) * 2 > g_blk_cap) blk_grow();
    if ((g_blk_used + 1) * 2 > g_blk_cap) return; // couldn't grow: not counted
    for (uint32_t i = blk_hash(p) & (g_blk_cap - 1);; i = (i + 1) & (g_blk_cap - 1)) {
        if (g_blk_ptr[i] <= BLK_DEAD) {
            if (!g_blk_ptr[i]) g_blk_used++;
            g_blk_ptr[i] = p;
            g_blk_size[i] = size;
            return;
        }
    }
}

static uint32_t blk_take(uintptr_t p) {
    if (!g_blk_cap) return 0;
    for (uint32_t i = blk_hash(p) & (g_blk_cap - 1), n = 0; n < g_blk_cap; i = (i + 1) & (g_blk_cap - 1), n++) {
        if (g_blk_ptr[i] == p) {
            g_blk_ptr[i] = BLK_DEAD;
            return g_blk_size[i];
        }
        if (!g_blk_ptr[i]) return 0;
    }
    return 0;
}

// GPU memory shares the block reserved for games' memory (main_wiiu.c): a game's memory grows down from the
// top of it, big GPU blocks fill it from the bottom, so a small game leaves the GPU all the rest. With a fixed
// split, the console ran out of GPU memory at 117 MB in a game using 55 MB of a 624 MB block.
// Small blocks (shapes, small pictures) come from the heap first, so they don't leave holes among pictures.
// Free ranges are kept outside the memory itself, sorted by address, and merged when freed.
#define POOL_FROM     (64u * 1024)        // blocks this big or bigger start in the shared block
#define POOL_ROUND    64u
#define POOL_WASM_ROOM ((uintptr_t)64 << 20) // always left for the game's memory to grow into
#define POOL_MAX_FREE 16384
#define SMALL_HEAP_MAX (64u << 20)        // small blocks past this use the shared block too: malloc needs the heap
typedef struct { uintptr_t start, end; } PoolRange;
static PoolRange g_pool_free[POOL_MAX_FREE];
static uint32_t g_pool_nfree;
static uintptr_t g_pool_brk;
static uint32_t g_small_heap_bytes;

void rw_res_range(uint8_t** base, uint8_t** end, uint8_t** low);

uintptr_t gfx_pool_top(void) {
    return g_pool_brk;
}

static void pool_add_free(uintptr_t start, uintptr_t end) {
    if (start >= end) return;
    uint32_t lo = 0, hi = g_pool_nfree;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (g_pool_free[mid].start < start) lo = mid + 1;
        else hi = mid;
    }
    int with_prev = lo > 0 && g_pool_free[lo - 1].end == start;
    int with_next = lo < g_pool_nfree && g_pool_free[lo].start == end;
    if (with_prev && with_next) {
        g_pool_free[lo - 1].end = g_pool_free[lo].end;
        memmove(&g_pool_free[lo], &g_pool_free[lo + 1], (g_pool_nfree - lo - 1) * sizeof(PoolRange));
        g_pool_nfree--;
    } else if (with_prev) {
        g_pool_free[lo - 1].end = end;
    } else if (with_next) {
        g_pool_free[lo].start = start;
    } else {
        if (g_pool_nfree >= POOL_MAX_FREE) return; // (lost until the holes around it merge away: harmless)
        memmove(&g_pool_free[lo + 1], &g_pool_free[lo], (g_pool_nfree - lo) * sizeof(PoolRange));
        g_pool_free[lo].start = start;
        g_pool_free[lo].end = end;
        g_pool_nfree++;
    }
    if (g_pool_nfree && g_pool_free[g_pool_nfree - 1].end == g_pool_brk) {
        g_pool_brk = g_pool_free[g_pool_nfree - 1].start;
        g_pool_nfree--;
    }
}

static void* pool_alloc(uint32_t size, uint32_t align) {
    uint8_t* base;
    uint8_t* end;
    uint8_t* low;
    rw_res_range(&base, &end, &low);
    if (!base) return NULL;
    if (!g_pool_brk) g_pool_brk = (uintptr_t)base;
    uintptr_t n = (size + POOL_ROUND - 1) & ~(uintptr_t)(POOL_ROUND - 1);
    uintptr_t mask = (uintptr_t)(align ? align : 1) - 1;
    for (uint32_t i = 0; i < g_pool_nfree; i++) {
        uintptr_t a = (g_pool_free[i].start + mask) & ~mask;
        if (a + n > g_pool_free[i].end) continue;
        uintptr_t s = g_pool_free[i].start, e = g_pool_free[i].end;
        memmove(&g_pool_free[i], &g_pool_free[i + 1], (g_pool_nfree - i - 1) * sizeof(PoolRange));
        g_pool_nfree--;
        pool_add_free(s, a);
        pool_add_free(a + n, e);
        return (void*)a;
    }
    uintptr_t a = (g_pool_brk + mask) & ~mask;
    if (a + n + POOL_WASM_ROOM > (uintptr_t)low) return NULL;
    uintptr_t old = g_pool_brk;
    g_pool_brk = a + n;
    pool_add_free(old, a);
    return (void*)a;
}

static int in_pool(void* p) {
    uint8_t* base;
    uint8_t* end;
    uint8_t* low;
    rw_res_range(&base, &end, &low);
    return base && (uint8_t*)p >= base && (uint8_t*)p < end;
}

static void gpu_free_now(void* p) {
    if (!p) return;
    uint32_t n = blk_take((uintptr_t)p);
    g_gpu_bytes -= n < g_gpu_bytes ? n : g_gpu_bytes;
    if (g_gpu_blocks) g_gpu_blocks--;
    if (in_pool(p)) {
        uintptr_t r = (n + POOL_ROUND - 1) & ~(uintptr_t)(POOL_ROUND - 1);
        if (n) pool_add_free((uintptr_t)p, (uintptr_t)p + r);
        return;
    }
    g_small_heap_bytes -= n < g_small_heap_bytes ? n : g_small_heap_bytes;
    MEMFreeToDefaultHeap(p);
}

// Shape buffers (`tail`) come from the top of the heap, pictures from the bottom: a game building
// thousands of shapes a second otherwise left holes between its pictures, and a big picture found no
// room with enough memory free in total.
// (Cemu refuses top-of-heap requests, so those fall back to the bottom; once refused, not tried again.)
static void* raw_gpu_alloc(uint32_t size, uint32_t align, int tail) {
    static int tail_refused;
    if (tail && !tail_refused) {
        void* p = MEMAllocFromDefaultHeapEx(size, -(int)align);
        if (p) return p;
        tail_refused = 1;
    }
    return MEMAllocFromDefaultHeapEx(size, (int)align);
}

// Releases everything waiting for the GPU to finish with it, after waiting for the GPU.
static void release_pending_now(void) {
    gpu_sync();
    for (uint32_t i = 0; i < g_prev_npending; i++) gpu_free_now(g_prev_pending[i]);
    g_prev_npending = 0;
    for (uint32_t i = 0; i < g_npending; i++) gpu_free_now(g_pending[i]);
    g_npending = 0;
}

static int g_heap_next;

static void* gpu_alloc_ex(uint32_t size, uint32_t align, int tail) {
    if (!size) size = 4;
    // Things that stay for the whole session stay out of the shared block: one left high up in it stopped a game's
    // memory growing past there (252 MB of it was held by leftovers, and Epic Battle Fantasy 5 ran out).
    int pool_first = !g_heap_next && (size >= POOL_FROM || g_small_heap_bytes >= SMALL_HEAP_MAX);
    void* p = pool_first ? pool_alloc(size, align) : raw_gpu_alloc(size, align, tail);
    if (!p) p = pool_first ? raw_gpu_alloc(size, align, tail) : pool_alloc(size, align);
    if (!p && !g_gpu_lost) {
        // Out of memory: what's only waiting for the GPU may be enough. Then try once more.
        release_pending_now();
        p = pool_alloc(size, align);
        if (!p) p = raw_gpu_alloc(size, align, tail);
    }
    if (p && !in_pool(p)) g_small_heap_bytes += size;
    if (!p) {
        static uint32_t reported;
        if (reported++ < 5) {
            char m[96];
            snprintf(m, sizeof(m), "out of GPU memory (%u MB in %u blocks in use, %u KB asked)",
                     (unsigned)(g_gpu_bytes >> 20), (unsigned)g_gpu_blocks, (unsigned)(size >> 10));
            plat_log(0, m);
        }
        return NULL;
    }
    blk_put((uintptr_t)p, size);
    g_gpu_bytes += size;
    g_gpu_blocks++;
    return p;
}

static void* gpu_alloc(uint32_t size, uint32_t align) {
    return gpu_alloc_ex(size, align, 0);
}

void gfx_gx2_memory(uint32_t* mb, uint32_t* blocks) {
    *mb = (uint32_t)(g_gpu_bytes >> 20);
    *blocks = g_gpu_blocks;
}

static uint32_t var_offset(int vertex, const char* name) {
    GX2UniformVar* v = vertex ? GX2GetVertexUniformVar(g_shader.vertexShader, name)
                              : GX2GetPixelUniformVar(g_shader.pixelShader, name);
    if (!v) {
        char msg[96];
        snprintf(msg, sizeof(msg), "shader uniform %s missing", name);
        plat_log(1, msg);
        return 0xffffffff;
    }
    return v->offset;
}

// Render targets only the GPU uses can be tiled: it draws to the linear layout (which the CPU needs) far slower.
static int g_tile_next;
static int g_float_next; // the next surface holds 32-bit float RGBA

static void set_tex_surface(Tex* t, uint32_t w, uint32_t h) {
    memset(&t->tex, 0, sizeof(t->tex));
    t->tex.surface.width = w;
    t->tex.surface.height = h;
    t->tex.surface.depth = 1;
    t->tex.surface.mipLevels = 1;
    t->tex.surface.format = g_float_next ? GX2_SURFACE_FORMAT_FLOAT_R32_G32_B32_A32 : GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8;
    t->tex.surface.aa = GX2_AA_MODE1X;
    t->tex.surface.use = GX2_SURFACE_USE_TEXTURE_COLOR_BUFFER_TV;
    t->tex.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    t->tex.surface.tileMode = g_tile_next ? GX2_TILE_MODE_DEFAULT : GX2_TILE_MODE_LINEAR_ALIGNED;
    t->tex.viewNumSlices = 1;
    t->tex.compMap = GX2_COMP_MAP(GX2_SQ_SEL_R, GX2_SQ_SEL_G, GX2_SQ_SEL_B, GX2_SQ_SEL_A);
    GX2CalcSurfaceSizeAndAlignment(&t->tex.surface);
    GX2InitTextureRegs(&t->tex);
    t->tex.surface.image = gpu_alloc(t->tex.surface.imageSize, t->tex.surface.alignment);
    if (!t->tex.surface.image) return; // (flushing a NULL image crashed the console: DSI at address 0)
    memset(t->tex.surface.image, 0, t->tex.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, t->tex.surface.image, t->tex.surface.imageSize);
}

static void make_target(Tex* t) {
    if (t->has_target) return;
    memset(&t->cb, 0, sizeof(t->cb));
    t->cb.surface = t->tex.surface;
    t->cb.viewNumSlices = 1;
    GX2InitColorBufferRegs(&t->cb);

    memset(&t->db, 0, sizeof(t->db));
    t->db.surface.width = t->tex.surface.width;
    t->db.surface.height = t->tex.surface.height;
    t->db.surface.depth = 1;
    t->db.surface.mipLevels = 1;
    t->db.surface.format = GX2_SURFACE_FORMAT_FLOAT_D24_S8;
    t->db.surface.aa = GX2_AA_MODE1X;
    t->db.surface.use = GX2_SURFACE_USE_DEPTH_BUFFER;
    t->db.surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    t->db.surface.tileMode = GX2_TILE_MODE_DEFAULT;
    t->db.viewNumSlices = 1;
    t->db.depthClear = 1.0f;
    GX2CalcSurfaceSizeAndAlignment(&t->db.surface);
    GX2InitDepthBufferRegs(&t->db);
    t->db.surface.image = gpu_alloc(t->db.surface.imageSize, t->db.surface.alignment);
    t->has_target = 1;
}

static void free_tex(Tex* t) {
    defer_free(t->tex.surface.image);
    if (t->has_target) defer_free(t->db.surface.image);
    memset(t, 0, sizeof(*t));
}

int gfx_gx2_init(uint32_t width, uint32_t height) {
    g_ctx = (GX2ContextState*)MEMAllocFromDefaultHeapEx(sizeof(GX2ContextState), GX2_CONTEXT_STATE_ALIGNMENT);
    if (!g_ctx) {
        plat_log(0, "no memory for the GPU context state");
        return -1;
    }
    GX2SetupContextStateEx(g_ctx, TRUE);
    if (!WHBGfxLoadGFDShaderGroup(&g_shader, 0, flashplayer_gsh)) {
        plat_log(0, "could not load flashplayer.gsh");
        return -1;
    }
    WHBGfxInitShaderAttribute(&g_shader, "pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&g_shader, "color", 0, 8, GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
    WHBGfxInitFetchShader(&g_shader);
    u_view = var_offset(1, "view");
    u_world0 = var_offset(1, "world0");
    u_world1 = var_offset(1, "world1");
    u_uv0 = var_offset(1, "uv0");
    u_uv1 = var_offset(1, "uv1");
    u_mult = var_offset(0, "mult");
    u_add = var_offset(0, "add");
    u_params = var_offset(0, "params");
    s_tex = 0;
    for (uint32_t i = 0; i < g_shader.pixelShader->samplerVarCount; i++)
        if (!strcmp(g_shader.pixelShader->samplerVars[i].name, "tex")) s_tex = g_shader.pixelShader->samplerVars[i].location;

    for (int i = 0; i < 4; i++) {
        GX2InitSampler(&g_samplers[i], (i & 2) ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP,
                       (i & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
    }
    GX2InitSampler(&g_ramp_sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);

    set_tex_surface(&g_screen, width, height); // what the menu draws, and where the game's picture is read back
    g_screen.used = 1;
    g_tile_next = 1;
    set_tex_surface(&g_game_screen, width, height);
    g_tile_next = 0;
    make_target(&g_game_screen);
    g_game_screen.used = 1;
    return 0;
}

// ---------------------------------------------------------------- resources

uint32_t gfx_mesh_create(const RwVertex* v, uint32_t nverts, const uint32_t* idx, uint32_t nidx) {
    uint32_t id = 1;
    while (id < g_nmeshes && g_meshes[id].used) id++;
    if (id >= g_nmeshes) {
        uint32_t n = g_nmeshes ? g_nmeshes * 2 : 4096;
        Mesh* grown = (Mesh*)realloc(g_meshes, n * sizeof(Mesh));
        if (!grown) return 0; // (mesh 0 draws nothing)
        g_meshes = grown;
        memset(g_meshes + g_nmeshes, 0, (n - g_nmeshes) * sizeof(Mesh));
        g_nmeshes = n;
    }
    Mesh* m = &g_meshes[id];
    m->used = 1;
    m->nidx = nidx;
    m->verts = (RwVertex*)gpu_alloc_ex(nverts * sizeof(RwVertex), GX2_VERTEX_BUFFER_ALIGNMENT, 1);
    m->idx = (uint32_t*)gpu_alloc_ex(nidx * 4, GX2_INDEX_BUFFER_ALIGNMENT, 1);
    if (!m->verts || !m->idx) {
        m->nidx = 0;
        return id;
    }
    memcpy(m->verts, v, nverts * sizeof(RwVertex));
    memcpy(m->idx, idx, nidx * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, m->verts, nverts * sizeof(RwVertex));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, m->idx, nidx * 4);
    return id;
}

void gfx_mesh_free(uint32_t id) {
    if (id >= g_nmeshes || !g_meshes[id].used) return;
    defer_free(g_meshes[id].verts);
    defer_free(g_meshes[id].idx);
    memset(&g_meshes[id], 0, sizeof(Mesh));
}

static void write_pixels(Tex* t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    uint8_t* img = (uint8_t*)t->tex.surface.image;
    if (!img) return;
    uint32_t pitch = t->tex.surface.pitch * 4;
    for (uint32_t row = 0; row < h; row++)
        memcpy(img + (size_t)(y + row) * pitch + x * 4, rgba + (size_t)row * stride, (size_t)w * 4);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, img, t->tex.surface.imageSize);
}

uint32_t gfx_tex_create(uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    uint32_t id = 1;
    while (id < g_ntexs && g_texs[id].used) id++;
    if (id >= g_ntexs) {
        uint32_t n = g_ntexs ? g_ntexs * 2 : 1024;
        Tex* grown = (Tex*)realloc(g_texs, n * sizeof(Tex));
        if (!grown) return 0; // (texture 0 draws nothing)
        g_texs = grown;
        memset(g_texs + g_ntexs, 0, (n - g_ntexs) * sizeof(Tex));
        g_ntexs = n;
    }
    Tex* t = &g_texs[id];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    set_tex_surface(t, w ? w : 1, h ? h : 1);
    if (rgba) write_pixels(t, 0, 0, w, h, rgba, stride);
    return id;
}

void gfx_tex_update(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t* rgba, uint32_t stride) {
    if (id >= g_ntexs || !g_texs[id].used) return;
    // The GPU may still be reading this texture for an earlier draw: wait only as long as that needs. A full
    // wait here (which includes the TV's refresh) cost games that update a bitmap every frame a whole frame.
    Tex* t = &g_texs[id];
    if (t->drawn_epoch == g_epoch || g_c3d_drew) gpu_sync();
    else if (t->drawn_epoch && t->drawn_epoch == g_prev_epoch) gpu_wait_last_frame();
    write_pixels(t, x, y, w, h, rgba, stride);
}

void gfx_tex_free(uint32_t id) {
    if (id >= g_ntexs || !g_texs[id].used) return;
    free_tex(&g_texs[id]);
}

static void copy_game_screen(void);

// The game's picture, for reading back on the CPU (screenshots, game pictures for the menu, BitmapData of the stage).
static void screen_to_linear(void) {
    if (g_shown != &g_game_screen || g_gpu_lost) return;
    copy_game_screen();
}

static void read_pixels(Tex* t, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out) {
    gpu_sync();
    uint8_t* img = (uint8_t*)t->tex.surface.image;
    if (!img || g_gpu_lost) {
        memset(out, 0, (size_t)w * h * 4);
        return;
    }
    DCInvalidateRange(img, t->tex.surface.imageSize);
    uint32_t pitch = t->tex.surface.pitch * 4;
    for (uint32_t row = 0; row < h; row++)
        memcpy(out + (size_t)row * w * 4, img + (size_t)(y + row) * pitch + x * 4, (size_t)w * 4);
}

void gfx_tex_read(uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out) {
    if (id >= g_ntexs || !g_texs[id].used) {
        memset(out, 0, (size_t)w * h * 4);
        return;
    }
    read_pixels(&g_texs[id], x, y, w, h, out);
}

// ---------------------------------------------------------------- Stage3D
static void end_target(Tex* t);

// GLSL from context3d.rs (no Y flip, constants as plain uniform arrays) is compiled on the console with
// CafeGLSL in register mode. Flash's vertex/index data is little-endian, read as is (no endian swap).

typedef struct {
    uint8_t* mem;
    uint32_t size;
    uint32_t drawn_frame; // last frame a draw used it (then an upload copies it, see gfx_c3d_buf_upload)
    int vertex;           // vertex data is stored as big-endian 32-bit words (read with the default swap)
    int used;
} C3dBuf;

// Vertex data: Flash's little-endian 32-bit values are stored big-endian, so the attributes can be read
// with GX2's default endian swap like all other data (Cemu didn't draw them with ENDIAN_SWAP_NONE).
// 4-byte color attributes come out right too: the default swap undoes the word swap for them.
static void copy_words_swapped(uint8_t* dst, const uint8_t* src, uint32_t len) {
    uint32_t i = 0;
    for (; i + 4 <= len; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
    }
    for (; i < len; i++) dst[i] = src[i];
}

// Index data: little-endian u16 -> big-endian (drawn as GX2_INDEX_TYPE_U16).
static void copy_halves_swapped(uint8_t* dst, const uint8_t* src, uint32_t len) {
    uint32_t i = 0;
    for (; i + 2 <= len; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
    }
    for (; i < len; i++) dst[i] = src[i];
}

static void buf_copy_in(const C3dBuf* b, uint8_t* dst, const uint8_t* src, uint32_t len) {
    if (b->vertex) copy_words_swapped(dst, src, len);
    else copy_halves_swapped(dst, src, len);
}

typedef struct CompileJob CompileJob;
typedef struct {
    GX2VertexShader* vs;
    GX2PixelShader* ps;
    uint32_t vc_off, fc_off; // uniform register offsets of the constant arrays (~0 if unused)
    int attr_loc[8];         // shader attribute location for va0..va7 (-1 if unused)
    int samp_loc[8];         // sampler location for fs0..fs7 (-1 if unused)
    CompileJob* job;         // while its shaders compile (see prog_ready)
    uint64_t queued_ns;
    int ready, warned, waited;
    int used;
} C3dProg;

#define FETCH_CACHE 32
typedef struct {
    uint32_t key[9]; // program, then format << 16 | offset for each attribute
    GX2FetchShader fs;
    void* mem;
    uint32_t last_use;
} FetchEntry;

static C3dBuf* g_c3d_bufs;
static uint32_t g_c3d_nbufs;
static C3dProg* g_c3d_progs;
static uint32_t g_c3d_nprogs;
static FetchEntry g_fetch[FETCH_CACHE];
static uint32_t g_frame_no, g_fetch_clock;
static int g_glsl_ready;
static GX2Sampler g_c3d_samplers[8]; // [wrap 0..3][linear]

uint32_t gfx_c3d_flags(void) { return 2; } // no Y flip (GX2 clip +1 is the top row), plain uniforms

uint32_t gfx_c3d_buf_create(uint32_t size, int vertex) {
    uint32_t id = 1;
    while (id < g_c3d_nbufs && g_c3d_bufs[id].used) id++;
    if (id >= g_c3d_nbufs) {
        uint32_t n = g_c3d_nbufs ? g_c3d_nbufs * 2 : 256;
        g_c3d_bufs = (C3dBuf*)realloc(g_c3d_bufs, n * sizeof(C3dBuf));
        memset(g_c3d_bufs + g_c3d_nbufs, 0, (n - g_c3d_nbufs) * sizeof(C3dBuf));
        g_c3d_nbufs = n;
    }
    C3dBuf* b = &g_c3d_bufs[id];
    memset(b, 0, sizeof(*b));
    b->mem = (uint8_t*)gpu_alloc(size, 256);
    if (b->mem) memset(b->mem, 0, size);
    b->size = size;
    b->drawn_frame = ~0u;
    b->vertex = vertex;
    b->used = 1;
    return id;
}

void gfx_c3d_buf_upload(uint32_t id, uint32_t offset, const uint8_t* data, uint32_t len) {
    if (id >= g_c3d_nbufs || !g_c3d_bufs[id].used) return;
    C3dBuf* b = &g_c3d_bufs[id];
    if (!b->mem || offset + len > b->size) return;
    if (b->drawn_frame != ~0u && b->drawn_frame + 1 >= g_frame_no) {
        // A draw this frame or the last (which the GPU may still be drawing) may read the old contents:
        // write into a fresh copy instead of waiting.
        uint8_t* fresh = (uint8_t*)gpu_alloc(b->size, 256);
        if (!fresh) return;
        memcpy(fresh, b->mem, b->size);
        defer_free(b->mem);
        b->mem = fresh;
        b->drawn_frame = ~0u;
        buf_copy_in(b, b->mem + offset, data, len);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, b->mem, b->size);
        return;
    }
    buf_copy_in(b, b->mem + offset, data, len);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, b->mem + offset, len);
}

void gfx_c3d_buf_free(uint32_t id) {
    if (id >= g_c3d_nbufs || !g_c3d_bufs[id].used) return;
    defer_free(g_c3d_bufs[id].mem);
    memset(&g_c3d_bufs[id], 0, sizeof(C3dBuf));
}

// Number after "<prefix>" in a shader variable name, or -1.
static int name_number(const char* name, const char* prefix) {
    const char* p = name ? strstr(name, prefix) : NULL;
    if (!p) return -1;
    p += strlen(prefix);
    if (*p < '0' || *p > '9') return -1;
    return atoi(p);
}

// Every shader pair is compiled once and kept for the rest of the run (compiling takes ~10 ms or more,
// and a game started again or a second Program3D with the same code reuses it). Games use a handful of
// distinct shaders, so keeping them costs little.
typedef struct {
    char* src; // vs, '\0', fs
    size_t len;
    GX2VertexShader* v;
    GX2PixelShader* p;
} CompiledPair;
static CompiledPair* g_compiled;
static uint32_t g_ncompiled, g_compiled_cap;

// Runs on the compile thread only (g_compiled belongs to it). Writes a line for the main thread to log.
static int compiled_shaders(const char* vs, const char* fs, GX2VertexShader** v_out, GX2PixelShader** p_out,
                            char* msg, size_t msg_size) {
    size_t vs_len = strlen(vs), fs_len = strlen(fs), len = vs_len + 1 + fs_len;
    for (uint32_t i = 0; i < g_ncompiled; i++) {
        CompiledPair* c = &g_compiled[i];
        if (c->len == len && !memcmp(c->src, vs, vs_len + 1) && !memcmp(c->src + vs_len + 1, fs, fs_len)) {
            *v_out = c->v;
            *p_out = c->p;
            msg[0] = 0;
            return 1;
        }
    }
    static char log[4096];
    GX2VertexShader* v = NULL;
    GX2PixelShader* p = NULL;
    // In Cemu a compile sometimes came out as an empty program (16-byte vertex shader with no attributes or
    // uniforms), then right again with the same input a moment later; large shaders could also hang there
    // or not, run to run (the PC build of the same compiler takes 0.1 s).
    // So a result that lacks what the source declares is retried after a short wait.
    uint64_t t0 = plat_time_ns();
    for (int attempt = 0;; attempt++) {
        GX2ShaderMode mode = CompileShaderPair(vs, fs, GLSL_COMPILE_REGISTER, &v, &p, log, sizeof(log), GLSL_COMPILER_FLAG_NONE);
        if (mode == GLSL_SHADER_MODE_ERROR || !v || !p) {
            snprintf(msg, msg_size, "Stage3D: shader compile failed: %.400s", log);
            FreeShaders(v, p);
            return 0;
        }
        // (An 8-byte pixel shader is fine: passing an interpolated input straight out is one export.)
        int complete = v->size > 16 && !(strstr(vs, "_p2vs_location") && !v->attribVarCount) &&
                       !(strstr(vs, "binding_0_vs") && !v->uniformVarCount) &&
                       !(strstr(fs, "binding_1_fs") && !p->uniformVarCount) &&
                       !(strstr(fs, "sampler") && !p->samplerVarCount);
        if (complete) {
            snprintf(msg, msg_size, "Stage3D: shaders compiled in %u ms (%u + %u bytes of GLSL, %d retries)",
                     (unsigned)((plat_time_ns() - t0) / 1000000), (unsigned)vs_len, (unsigned)fs_len, attempt);
            break;
        }
        if (attempt == 100) { // ~3 s: keep it, a broken shader beats none
            snprintf(msg, msg_size, "Stage3D: shader still incomplete after retries (%u + %u bytes of GLSL)",
                     (unsigned)vs_len, (unsigned)fs_len);
            break;
        }
        FreeShaders(v, p);
        v = NULL;
        p = NULL;
        OSSleepTicks(OSMillisecondsToTicks(30));
    }
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, v->program, v->size);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, p->program, p->size);
    if (g_ncompiled == g_compiled_cap) {
        uint32_t n = g_compiled_cap ? g_compiled_cap * 2 : 32;
        CompiledPair* grown = (CompiledPair*)realloc(g_compiled, n * sizeof(CompiledPair));
        if (!grown) goto done;
        g_compiled = grown;
        g_compiled_cap = n;
    }
    {
        char* src = (char*)malloc(len);
        if (!src) goto done;
        memcpy(src, vs, vs_len + 1);
        memcpy(src + vs_len + 1, fs, fs_len);
        g_compiled[g_ncompiled++] = (CompiledPair){ src, len, v, p };
    }
done:
    // (If it could not be kept, the pair leaks: still correct, just not shared.)
    *v_out = v;
    *p_out = p;
    return 1;
}

// Shaders compile on their own thread (core 2): a big shader can take a while (or, in Cemu, hang), and the
// game, menu and music keep running meanwhile. A program draws nothing until its job is done.
struct CompileJob {
    char* vs;
    char* fs;
    GX2VertexShader* v;
    GX2PixelShader* p;
    char msg[480];       // line for the main thread to log (plat_log isn't used from the compile thread)
    volatile int done;   // 0 pending, 1 compiled, -1 failed; set last by the compile thread
    volatile int refs;   // compile thread + program slot; whoever drops the last one frees the job
};

static OSMessageQueue g_compile_queue;
static OSMessage g_compile_msgs[256];
static OSThread g_compile_thread __attribute__((aligned(16)));
#define COMPILE_STACK (8 * 1024 * 1024) // Mesa recurses over deeply nested shader code

static void job_release(CompileJob* j) {
    if (__sync_sub_and_fetch(&j->refs, 1) == 0) {
        free(j->vs);
        free(j->fs);
        free(j);
    }
}

static int compile_thread_main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    InitGLSLCompiler();
    for (;;) {
        OSMessage m;
        OSReceiveMessage(&g_compile_queue, &m, OS_MESSAGE_FLAGS_BLOCKING);
        CompileJob* j = (CompileJob*)m.args[0];
        int ok = compiled_shaders(j->vs, j->fs, &j->v, &j->p, j->msg, sizeof(j->msg));
        __sync_synchronize(); // results before the flag
        j->done = ok ? 1 : -1;
        job_release(j);
    }
    return 0;
}

static int start_compile_thread(void) {
    OSInitMessageQueue(&g_compile_queue, g_compile_msgs, 256);
    uint8_t* stack = (uint8_t*)MEMAllocFromDefaultHeapEx(COMPILE_STACK, 16);
    if (!stack) return 0;
    if (!OSCreateThread(&g_compile_thread, compile_thread_main, 0, NULL, stack + COMPILE_STACK, COMPILE_STACK, 20,
                        OS_THREAD_ATTRIB_AFFINITY_CPU2 | OS_THREAD_ATTRIB_DETACHED))
        return 0;
    OSSetThreadName(&g_compile_thread, "Stage3D shader compiler");
    OSResumeThread(&g_compile_thread);
    return 1;
}

// Fills a program from its finished job (main thread). Returns whether the program can draw.
static int prog_ready(C3dProg* pr) {
    if (pr->ready) return 1;
    CompileJob* j = pr->job;
    if (!j) return 0;
    if (!j->done && !pr->waited) {
        // The first draw waits a little: games that draw once (a still frame) would otherwise lose it.
        pr->waited = 1;
        uint64_t until = plat_time_ns() + 2000000000ull;
        while (!j->done && plat_time_ns() < until) OSSleepTicks(OSMillisecondsToTicks(1));
    }
    if (!j->done) {
        if (!pr->warned && plat_time_ns() - pr->queued_ns > 10000000000ull) {
            pr->warned = 1;
            plat_log(1, "Stage3D: a shader has been compiling for over 10 s (it draws nothing until it's done)");
        }
        return 0;
    }
    __sync_synchronize();
    if (j->msg[0]) plat_log(j->done > 0 ? 2 : 0, j->msg);
    if (j->done > 0) {
        GX2VertexShader* v = j->v;
        GX2PixelShader* p = j->p;
        pr->vs = v;
        pr->ps = p;
        // CafeGLSL lists the array and each element ("name[3]"); the array starts at the lowest offset.
        for (uint32_t i = 0; i < v->uniformVarCount; i++)
            if (strstr(v->uniformVars[i].name, "binding_0_vs") && v->uniformVars[i].offset < pr->vc_off)
                pr->vc_off = v->uniformVars[i].offset;
        for (uint32_t i = 0; i < p->uniformVarCount; i++)
            if (strstr(p->uniformVars[i].name, "binding_1_fs") && p->uniformVars[i].offset < pr->fc_off)
                pr->fc_off = p->uniformVars[i].offset;
        for (uint32_t i = 0; i < v->attribVarCount; i++) {
            int n = name_number(v->attribVars[i].name, "location");
            if (n >= 0 && n < 8) pr->attr_loc[n] = (int)v->attribVars[i].location;
        }
        for (uint32_t i = 0; i < p->samplerVarCount; i++) {
            int n = name_number(p->samplerVars[i].name, "binding_");
            if (n >= 10 && n < 18) pr->samp_loc[n - 10] = (int)p->samplerVars[i].location;
        }
        pr->ready = 1;
    }
    pr->job = NULL;
    job_release(j);
    return pr->ready;
}

uint32_t gfx_c3d_program(const char* vs, const char* fs) {
    static int compile_thread_ok;
    if (!g_glsl_ready) {
        g_glsl_ready = 1;
        compile_thread_ok = start_compile_thread();
        if (!compile_thread_ok) plat_log(0, "Stage3D: couldn't start the shader compile thread");
        for (int i = 0; i < 8; i++) {
            int wrap = i >> 1;
            GX2TexClampMode u = (wrap == 1 || wrap == 3) ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP;
            GX2TexClampMode v = (wrap == 1 || wrap == 2) ? GX2_TEX_CLAMP_MODE_WRAP : GX2_TEX_CLAMP_MODE_CLAMP;
            GX2InitSampler(&g_c3d_samplers[i], u, (i & 1) ? GX2_TEX_XY_FILTER_MODE_LINEAR : GX2_TEX_XY_FILTER_MODE_POINT);
            GX2InitSamplerClamping(&g_c3d_samplers[i], u, v, GX2_TEX_CLAMP_MODE_CLAMP);
        }
    }
    if (!compile_thread_ok) return 0;
    CompileJob* j = (CompileJob*)calloc(1, sizeof(CompileJob));
    if (!j) return 0;
    j->vs = strdup(vs);
    j->fs = strdup(fs);
    j->refs = 2;
    if (!j->vs || !j->fs) {
        free(j->vs);
        free(j->fs);
        free(j);
        return 0;
    }
    OSMessage m = { 0 };
    m.args[0] = (uint32_t)j;
    if (!OSSendMessage(&g_compile_queue, &m, OS_MESSAGE_FLAGS_NONE)) { // queue full
        free(j->vs);
        free(j->fs);
        free(j);
        return 0;
    }

    uint32_t id = 1;
    while (id < g_c3d_nprogs && g_c3d_progs[id].used) id++;
    if (id >= g_c3d_nprogs) {
        uint32_t n = g_c3d_nprogs ? g_c3d_nprogs * 2 : 64;
        g_c3d_progs = (C3dProg*)realloc(g_c3d_progs, n * sizeof(C3dProg));
        memset(g_c3d_progs + g_c3d_nprogs, 0, (n - g_c3d_nprogs) * sizeof(C3dProg));
        g_c3d_nprogs = n;
    }
    C3dProg* pr = &g_c3d_progs[id];
    memset(pr, 0, sizeof(*pr));
    pr->used = 1;
    pr->job = j;
    pr->queued_ns = plat_time_ns();
    pr->vc_off = pr->fc_off = ~0u;
    for (int i = 0; i < 8; i++) pr->attr_loc[i] = pr->samp_loc[i] = -1;
    return id;
}

void gfx_c3d_program_free(uint32_t id) {
    if (id >= g_c3d_nprogs || !g_c3d_progs[id].used) return;
    if (g_c3d_progs[id].job) job_release(g_c3d_progs[id].job); // the compile thread drops its own reference
    // The GPU may still use its fetch shaders this frame; programs are freed rarely, so wait.
    // (The shaders themselves stay in the compiled_shaders cache.)
    gpu_sync();
    for (int i = 0; i < FETCH_CACHE; i++)
        if (g_fetch[i].mem && g_fetch[i].key[0] == id) {
            gpu_free_now(g_fetch[i].mem);
            memset(&g_fetch[i], 0, sizeof(FetchEntry));
        }
    memset(&g_c3d_progs[id], 0, sizeof(C3dProg));
}

static GX2FetchShader* fetch_shader(uint32_t prog_id, const C3dProg* pr, const uint32_t* attrs) {
    static const GX2AttribFormat formats[] = { GX2_ATTRIB_FORMAT_FLOAT_32, GX2_ATTRIB_FORMAT_FLOAT_32_32,
                                               GX2_ATTRIB_FORMAT_FLOAT_32_32_32, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32,
                                               GX2_ATTRIB_FORMAT_UNORM_8_8_8_8 };
    static const uint32_t masks[] = {
        GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_0, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
        GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_0, GX2_SQ_SEL_1),
        GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_1),
        GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
        GX2_SEL_MASK(GX2_SQ_SEL_X, GX2_SQ_SEL_Y, GX2_SQ_SEL_Z, GX2_SQ_SEL_W),
    };
    uint32_t key[9] = { prog_id };
    for (int i = 0; i < 8; i++) {
        const uint32_t* a = &attrs[i * 4];
        key[i + 1] = (a[0] && pr->attr_loc[i] >= 0) ? ((a[2] + 1) << 24 | a[1]) : 0;
    }
    g_fetch_clock++;
    int victim = 0;
    for (int i = 0; i < FETCH_CACHE; i++) {
        if (g_fetch[i].mem && !memcmp(g_fetch[i].key, key, sizeof(key))) {
            g_fetch[i].last_use = g_fetch_clock;
            return &g_fetch[i].fs;
        }
        if (g_fetch[i].last_use < g_fetch[victim].last_use) victim = i;
    }
    GX2AttribStream streams[8];
    uint32_t count = 0;
    for (int i = 0; i < 8; i++) {
        if (!key[i + 1]) continue;
        const uint32_t* a = &attrs[i * 4];
        uint32_t f = a[2] > 4 ? 3 : a[2];
        GX2AttribStream* s = &streams[count++];
        memset(s, 0, sizeof(*s));
        s->location = (uint32_t)pr->attr_loc[i];
        s->buffer = (uint32_t)i;
        s->offset = a[1];
        s->format = formats[f];
        s->type = GX2_ATTRIB_INDEX_PER_VERTEX;
        s->mask = masks[f];
        s->endianSwap = GX2_ENDIAN_SWAP_DEFAULT; // vertex buffers are stored big-endian (copy_words_swapped)
    }
    FetchEntry* e = &g_fetch[victim];
    if (e->mem) {
        gpu_sync(); // the evicted fetch shader may still be in flight
        gpu_free_now(e->mem);
    }
    memset(e, 0, sizeof(*e));
    uint32_t size = GX2CalcFetchShaderSizeEx(count, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    e->mem = gpu_alloc(size, 256); // shader program alignment
    if (!e->mem) return NULL;
    GX2InitFetchShaderEx(&e->fs, (uint8_t*)e->mem, count, streams, GX2_FETCH_SHADER_TESSELLATION_NONE, GX2_TESSELLATION_MODE_DISCRETE);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_SHADER, e->mem, size);
    memcpy(e->key, key, sizeof(key));
    e->last_use = g_fetch_clock;
    return &e->fs;
}

static Tex* c3d_target(uint32_t id) {
    if (id >= g_ntexs || !g_texs[id].used) return NULL;
    make_target(&g_texs[id]);
    return &g_texs[id];
}

void gfx_c3d_clear(uint32_t target, const float rgba[4], float depth, uint32_t stencil, uint32_t mask) {
    Tex* t = c3d_target(target);
    if (!t) return;
    if (mask & 1) GX2ClearColor(&t->cb, rgba[0], rgba[1], rgba[2], rgba[3]);
    GX2ClearFlags flags = (GX2ClearFlags)(((mask & 2) ? GX2_CLEAR_FLAGS_DEPTH : 0) | ((mask & 4) ? GX2_CLEAR_FLAGS_STENCIL : 0));
    if (flags) GX2ClearDepthStencilEx(&t->db, depth, (uint8_t)stencil, flags);
    GX2SetContextState(g_ctx); // clears change GPU state
    end_target(t);
}

static GX2BlendMode c3d_blend(uint32_t f, int alpha) {
    static const GX2BlendMode color[] = { GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_SRC_COLOR,
                                          GX2_BLEND_MODE_INV_SRC_COLOR, GX2_BLEND_MODE_SRC_ALPHA,
                                          GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                          GX2_BLEND_MODE_INV_DST_ALPHA, GX2_BLEND_MODE_DST_COLOR,
                                          GX2_BLEND_MODE_INV_DST_COLOR };
    static const GX2BlendMode alpha_f[] = { GX2_BLEND_MODE_ZERO, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_SRC_ALPHA,
                                            GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_SRC_ALPHA,
                                            GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                            GX2_BLEND_MODE_INV_DST_ALPHA, GX2_BLEND_MODE_DST_ALPHA,
                                            GX2_BLEND_MODE_INV_DST_ALPHA };
    if (f > 9) f = 1;
    return alpha ? alpha_f[f] : color[f];
}

void gfx_c3d_draw(const uint32_t* w, uint32_t n) {
    g_c3d_drew = 1;
    enum { ATTRS = 27, TEXS = ATTRS + 32, CONSTS = TEXS + 32, VC = 128 * 4, FC = 64 * 4 };
    if (n < CONSTS + VC + FC) return;
    Tex* t = c3d_target(w[0]);
    uint32_t prog = w[4], ib = w[5];
    if (!t || !prog || prog >= g_c3d_nprogs || !g_c3d_progs[prog].used || !prog_ready(&g_c3d_progs[prog])) return;
    if (ib >= g_c3d_nbufs || !g_c3d_bufs[ib].used || !g_c3d_bufs[ib].mem) return;
    C3dProg* pr = &g_c3d_progs[prog];
    GX2FetchShader* fetch = fetch_shader(prog, pr, &w[ATTRS]);
    if (!fetch) return;

    GX2SetContextState(g_ctx);
    GX2SetColorBuffer(&t->cb, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&t->db);
    GX2SetViewport(0, 0, (float)t->tex.surface.width, (float)t->tex.surface.height, 0.0f, 1.0f);
    if (w[14]) GX2SetScissor(w[15], w[16], w[17], w[18]);
    else GX2SetScissor(0, 0, t->tex.surface.width, t->tex.surface.height);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(fetch);
    GX2SetVertexShader(pr->vs);
    GX2SetPixelShader(pr->ps);
    if (pr->vc_off != ~0u) GX2SetVertexUniformReg(pr->vc_off, VC, &w[CONSTS]);
    if (pr->fc_off != ~0u) GX2SetPixelUniformReg(pr->fc_off, FC, &w[CONSTS + VC]);

    for (uint32_t i = 0; i < 8; i++) {
        const uint32_t* a = &w[ATTRS + i * 4];
        if (!a[0] || a[0] >= g_c3d_nbufs || !g_c3d_bufs[a[0]].used || pr->attr_loc[i] < 0) continue;
        C3dBuf* b = &g_c3d_bufs[a[0]];
        b->drawn_frame = g_frame_no;
        GX2SetAttribBuffer(i, b->size, a[3], b->mem);
    }
    for (int i = 0; i < 8; i++) {
        const uint32_t* tx = &w[TEXS + i * 4];
        // (tx[1] marks a cube texture: a 2D strip of 6 faces that the GLSL samples itself, see context3d.rs)
        if (pr->samp_loc[i] < 0 || !tx[0] || tx[0] >= g_ntexs || !g_texs[tx[0]].used) continue;
        GX2SetPixelTexture(&g_texs[tx[0]].tex, (uint32_t)pr->samp_loc[i]);
        GX2SetPixelSampler(&g_c3d_samplers[(tx[2] & 3) << 1 | (tx[3] & 1)], (uint32_t)pr->samp_loc[i]);
    }

    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xff, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, c3d_blend(w[8], 0), c3d_blend(w[9], 0), GX2_BLEND_COMBINE_MODE_ADD,
                       TRUE, c3d_blend(w[8], 1), c3d_blend(w[9], 1), GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetTargetChannelMasks((GX2ChannelMask)(w[13] & 0xf), 0, 0, 0, 0, 0, 0, 0);

    if (w[3]) {
        // Stencil actions apply to the chosen faces (w[19]: 1 back, 2 front, 3 both); others keep the stencil.
        int front = w[19] == 2 || w[19] == 3, back = w[19] == 1 || w[19] == 3;
        GX2CompareFunction func = (GX2CompareFunction)(w[20] & 7);
        GX2StencilFunction pass = (GX2StencilFunction)(w[21] & 7), zfail = (GX2StencilFunction)(w[22] & 7),
                           fail = (GX2StencilFunction)(w[23] & 7);
        GX2SetDepthStencilControl(TRUE, w[10] ? TRUE : FALSE, (GX2CompareFunction)(w[11] & 7), w[19] != 0, TRUE,
                                  front ? func : GX2_COMPARE_FUNC_ALWAYS, front ? pass : GX2_STENCIL_FUNCTION_KEEP,
                                  front ? zfail : GX2_STENCIL_FUNCTION_KEEP, front ? fail : GX2_STENCIL_FUNCTION_KEEP,
                                  back ? func : GX2_COMPARE_FUNC_ALWAYS, back ? pass : GX2_STENCIL_FUNCTION_KEEP,
                                  back ? zfail : GX2_STENCIL_FUNCTION_KEEP, back ? fail : GX2_STENCIL_FUNCTION_KEEP);
        GX2SetStencilMask((uint8_t)w[25], (uint8_t)w[26], (uint8_t)w[24], (uint8_t)w[25], (uint8_t)w[26], (uint8_t)w[24]);
    } else {
        GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                                  GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                                  GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                                  GX2_STENCIL_FUNCTION_KEEP);
    }
    // Flash's front faces are clockwise; with no Y flip that holds in GX2's clip space too.
    // (GX2SetCullOnlyControl with a cull bit set made Cemu drop every triangle; the full call works.)
    GX2SetPolygonControl(GX2_FRONT_FACE_CW, w[12] == 2 || w[12] == 3, w[12] == 1 || w[12] == 3, FALSE,
                         GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE, FALSE, FALSE, FALSE);
    // Stage3D clip-space depth is 0..1 (Direct3D style).
    GX2SetRasterizerClipControlEx(TRUE, TRUE, TRUE);

    C3dBuf* ibuf = &g_c3d_bufs[ib];
    ibuf->drawn_frame = g_frame_no;
    GX2DrawIndexedEx(GX2_PRIMITIVE_MODE_TRIANGLES, w[7], GX2_INDEX_TYPE_U16, ibuf->mem + w[6] * 2, 0, 1);

    // Back to what the 2D renderer and the TV/GamePad present expect (they share this context state).
    GX2SetRasterizerClipControlEx(TRUE, TRUE, FALSE);
    GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
                         FALSE, FALSE, FALSE);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS,
                              GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                              GX2_COMPARE_FUNC_ALWAYS, GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP,
                              GX2_STENCIL_FUNCTION_KEEP);
    end_target(t);
}

void gfx_reset(void) {
    for (uint32_t i = 0; i < g_nmeshes; i++) gfx_mesh_free(i);
    for (uint32_t i = 0; i < g_ntexs; i++) gfx_tex_free(i);
    for (uint32_t i = 0; i < g_c3d_nbufs; i++) gfx_c3d_buf_free(i);
    for (uint32_t i = 0; i < g_c3d_nprogs; i++) gfx_c3d_program_free(i);
}

void gfx_gx2_read_screen_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* rgba) {
    screen_to_linear();
    read_pixels(&g_screen, x, y, w, h, rgba);
}

void gfx_tex_update_screen(const uint8_t* rgba) {
    gpu_sync();
    g_shown = &g_screen;
    write_pixels(&g_screen, 0, 0, g_screen.tex.surface.width, g_screen.tex.surface.height, rgba,
                 g_screen.tex.surface.width * 4);
}

void gfx_tex_update_screen_rects(const uint8_t* rgba, const int (*rects)[4], int n) {
    g_shown = &g_screen;
    uint32_t sw = g_screen.tex.surface.width, sh = g_screen.tex.surface.height;
    uint8_t* img = (uint8_t*)g_screen.tex.surface.image;
    if (!img || !n) return;
    // No wait for the GPU: the worst case is that a changed area shows one frame early, and waiting cost
    // the menu ~5 ms a frame on the console.
    uint32_t pitch = g_screen.tex.surface.pitch * 4;
    for (int i = 0; i < n; i++) {
        uint32_t x = (uint32_t)rects[i][0], y = (uint32_t)rects[i][1], w = (uint32_t)rects[i][2], h = (uint32_t)rects[i][3];
        if (x >= sw || y >= sh) continue;
        if (x + w > sw) w = sw - x;
        if (y + h > sh) h = sh - y;
        // Only the changed bytes of each line leave the CPU cache: flushing whole lines (5 KB each) was
        // ~3.5 MB a frame and took ~9 ms on the console.
        for (uint32_t row = y; row < y + h; row++) {
            uint8_t* dst = img + (size_t)row * pitch + x * 4;
            memcpy(dst, rgba + ((size_t)row * sw + x) * 4, (size_t)w * 4);
            DCFlushRange(dst, w * 4);
        }
        // Tell the GPU, and Cemu (which watches CPU_TEXTURE invalidates; a GPU-only one flickered there).
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, img + (size_t)y * pitch + x * 4, w * 4);
    }
}

void gfx_gx2_read_screen(uint8_t* rgba) {
    screen_to_linear();
    read_pixels(&g_screen, 0, 0, g_screen.tex.surface.width, g_screen.tex.surface.height, rgba);
}

// ---------------------------------------------------------------- drawing

static float f(uint32_t w) {
    float v;
    memcpy(&v, &w, 4);
    return v;
}

static void set_mask(uint32_t state, uint32_t depth) {
    GX2CompareFunction func = GX2_COMPARE_FUNC_EQUAL;
    GX2StencilFunction pass = GX2_STENCIL_FUNCTION_KEEP;
    int test = 1, color = 1;
    uint8_t ref = (uint8_t)depth;
    switch (state) {
    case 0: test = 0; break;
    case 1: ref = (uint8_t)(depth - 1); pass = GX2_STENCIL_FUNCTION_INCR_CLAMP; color = 0; break;
    case 2: break;
    default: pass = GX2_STENCIL_FUNCTION_DECR_CLAMP; color = 0; break;
    }
    GX2SetDepthStencilControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS, test, test, func, pass,
                              GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP, func, pass,
                              GX2_STENCIL_FUNCTION_KEEP, GX2_STENCIL_FUNCTION_KEEP);
    GX2SetStencilMask(0xff, 0xff, ref, 0xff, 0xff, ref);
    GX2SetTargetChannelMasks(color ? GX2_CHANNEL_MASK_RGBA : (GX2ChannelMask)0, 0, 0, 0, 0, 0, 0, 0);
}

static void set_blend(uint32_t mode) {
    GX2BlendCombineMode eq = GX2_BLEND_COMBINE_MODE_ADD;
    GX2BlendMode src = GX2_BLEND_MODE_ONE, dst = GX2_BLEND_MODE_INV_SRC_ALPHA;
    GX2BlendMode asrc = GX2_BLEND_MODE_ONE, adst = GX2_BLEND_MODE_INV_SRC_ALPHA;
    switch (mode) {
    case 3: src = GX2_BLEND_MODE_DST_COLOR; break;                                   // multiply
    case 4: dst = GX2_BLEND_MODE_INV_SRC_COLOR; break;                               // screen
    case 5: eq = GX2_BLEND_COMBINE_MODE_MAX; dst = GX2_BLEND_MODE_ONE; break;        // lighten
    case 6: eq = GX2_BLEND_COMBINE_MODE_MIN; dst = GX2_BLEND_MODE_ONE; break;        // darken
    case 8: dst = GX2_BLEND_MODE_ONE; break;                                         // add
    case 9: eq = GX2_BLEND_COMBINE_MODE_REV_SUB; dst = GX2_BLEND_MODE_ONE; break;    // subtract
    case 10: src = GX2_BLEND_MODE_INV_DST_COLOR; dst = GX2_BLEND_MODE_ZERO; break;   // invert
    case 11: src = GX2_BLEND_MODE_ZERO; dst = adst = GX2_BLEND_MODE_SRC_ALPHA; asrc = src; break; // alpha
    case 12: src = GX2_BLEND_MODE_ZERO; dst = GX2_BLEND_MODE_INV_SRC_ALPHA; asrc = src; break; // erase
    case 255:                                                                        // opaque copy (Stage3D)
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD,
                           TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_ZERO, GX2_BLEND_COMBINE_MODE_ADD);
        return;
    default: break;
    }
    GX2SetBlendControl(GX2_RENDER_TARGET_0, src, dst, eq, TRUE, asrc, adst, GX2_BLEND_COMBINE_MODE_ADD);
}

static void begin_target(Tex* t) {
    GX2SetContextState(g_ctx);
    GX2SetColorBuffer(&t->cb, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&t->db);
    uint32_t w = t->tex.surface.width, h = t->tex.surface.height;
    GX2SetViewport(0, 0, (float)w, (float)h, 0.0f, 1.0f);
    GX2SetScissor(0, 0, w, h);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&g_shader.fetchShader);
    GX2SetVertexShader(g_shader.vertexShader);
    GX2SetPixelShader(g_shader.pixelShader);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0xff, FALSE, TRUE);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    // Flash y grows downwards and row 0 is the top of every target: y 0 -> clip +1.
    float view[4] = { 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f };
    GX2SetVertexUniformReg(u_view, 4, view);
    set_blend(0);
    set_mask(0, 0);
}

static void end_target(Tex* t) {
    // Make the result visible to later texture reads of this target.
    GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, t->tex.surface.image, t->tex.surface.imageSize);
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, t->tex.surface.image, t->tex.surface.imageSize);
}

static void draw_mesh(const Mesh* m, uint32_t first, uint32_t count, GX2PrimitiveMode mode) {
    if (!m->verts || !count) return;
    GX2SetAttribBuffer(0, 0x7fffffff, sizeof(RwVertex), m->verts);
    GX2DrawIndexedEx(mode, count, GX2_INDEX_TYPE_U32, m->idx + first, 0, 1);
}

void gfx_frame(uint32_t target, const uint32_t* c, uint32_t n) {
    Tex* t = &g_game_screen;
    if (!target) g_shown = t;
    if (target) {
        if (target >= g_ntexs || !g_texs[target].used) return;
        t = &g_texs[target];
        make_target(t);
        // The GPU keeps drawn pixels in a cache and writes them out later: a picture updated right after being
        // drawn into, without waiting, came out with black blocks on the console (Moto X3M's backgrounds).
        t->drawn_epoch = g_epoch;
    }
    begin_target(t);
    uint32_t i = 0;
    while (i < n) {
        uint32_t op = c[i++];
        if (op == RW_OP_CLEAR) {
            GX2ClearColor(&t->cb, f(c[i]), f(c[i + 1]), f(c[i + 2]), f(c[i + 3]));
            GX2ClearDepthStencilEx(&t->db, 1.0f, 0, GX2_CLEAR_FLAGS_BOTH);
            begin_target(t); // clears reset the context state
            i += 4;
        } else if (op == RW_OP_MASK) {
            set_mask(c[i], c[i + 1]);
            i += 2;
        } else if (op == RW_OP_BLEND) {
            set_blend(c[i]);
            i += 1;
        } else if (op == RW_OP_DRAW) {
            uint32_t mesh = c[i], first = c[i + 1], count = c[i + 2], primitive = c[i + 3];
            const uint32_t* w = &c[i + 4];
            float world0[4] = { f(w[0]), f(w[1]), f(w[2]), f(w[3]) };
            float world1[4] = { f(w[4]), f(w[5]), 0, 0 };
            GX2SetVertexUniformReg(u_world0, 4, world0);
            GX2SetVertexUniformReg(u_world1, 4, world1);
            GX2SetPixelUniformReg(u_mult, 4, &w[6]);
            GX2SetPixelUniformReg(u_add, 4, &w[10]);
            i += 4 + 14;
            uint32_t kind = c[i++];
            float params[4] = { (float)kind, 0, 0, 0 };
            if (kind == RW_KIND_BITMAP || kind == RW_KIND_GRADIENT || kind == RW_KIND_RAW) {
                uint32_t tex = c[i];
                const uint32_t* uv;
                Tex* tx = (tex < g_ntexs && g_texs[tex].used) ? &g_texs[tex] : NULL;
                if (kind != RW_KIND_GRADIENT) {
                    uint32_t flags = c[i + 1];
                    if (tx) GX2SetPixelSampler(&g_samplers[flags & 3], s_tex);
                    uv = &c[i + 2];
                    i += 2 + 6;
                } else {
                    params[1] = (float)c[i + 1];
                    params[2] = (float)c[i + 2];
                    params[3] = f(c[i + 3]);
                    GX2SetPixelSampler(&g_ramp_sampler, s_tex);
                    uv = &c[i + 4];
                    i += 4 + 6;
                }
                if (!tx) continue;
                tx->drawn_epoch = g_epoch;
                GX2SetPixelTexture(&tx->tex, s_tex);
                GX2SetVertexUniformReg(u_uv0, 4, uv);
                float uv1[4] = { f(uv[4]), f(uv[5]), 0, 0 };
                GX2SetVertexUniformReg(u_uv1, 4, uv1);
            }
            GX2SetPixelUniformReg(u_params, 4, params);
            if (mesh >= g_nmeshes || !g_meshes[mesh].used) continue;
            GX2PrimitiveMode mode = primitive == RW_PRIM_LINES ? GX2_PRIMITIVE_MODE_LINES
                                  : primitive == RW_PRIM_LINE_LOOP ? GX2_PRIMITIVE_MODE_LINE_LOOP
                                  : GX2_PRIMITIVE_MODE_TRIANGLES;
            draw_mesh(&g_meshes[mesh], first, count, mode);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "bad render op %u", (unsigned)op);
            plat_log(0, msg);
            break;
        }
    }
    end_target(t);
}

// ---------------------------------------------------------------- present

// Unit quad used to draw the screen texture onto the TV / GamePad (and for filter passes).
static Mesh g_quad;

static void make_quad(void) {
    if (g_quad.verts) return;
    static const RwVertex v[4] = { { 0, 0, 0xffffffff }, { 1, 0, 0xffffffff }, { 1, 1, 0xffffffff }, { 0, 1, 0xffffffff } };
    static const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    g_quad.verts = (RwVertex*)gpu_alloc(sizeof(v), GX2_VERTEX_BUFFER_ALIGNMENT);
    g_quad.idx = (uint32_t*)gpu_alloc(sizeof(idx), GX2_INDEX_BUFFER_ALIGNMENT);
    if (!g_quad.verts || !g_quad.idx) return;
    memcpy(g_quad.verts, v, sizeof(v));
    memcpy(g_quad.idx, idx, sizeof(idx));
    g_quad.nidx = 6;
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, g_quad.verts, sizeof(v));
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, g_quad.idx, sizeof(idx));
}

// ---------------------------------------------------------------- filters on the GPU
// Blur, glow and drop shadow as GPU passes (shaders/filter.frag) on the textures Ruffle already drew,
// instead of reading them back and filtering on the CPU (a game with glows spent ~90% of its drawing
// time there). Scratch textures only grow; each use clears the part it needs, so samples past the
// filtered area read as transparent, like the CPU version's edges.

static WHBGfxShaderGroup g_fshader;
static int g_filters_ok;
static uint32_t uf_view, uf_world0, uf_world1, uf_uv0, uf_uv1, uf_params, uf_step, uf_color, uf_flags, sf_src, sf_blurred;
static uint32_t uf_m[5];
static GX2Sampler g_border_sampler; // point sampling, transparent outside
static Tex g_scratch[3];

static uint32_t fvar(int vertex, const char* name) {
    GX2UniformVar* v = vertex ? GX2GetVertexUniformVar(g_fshader.vertexShader, name)
                              : GX2GetPixelUniformVar(g_fshader.pixelShader, name);
    if (!v) g_filters_ok = 0;
    return v ? v->offset : 0;
}

static void init_filters(void) {
    if (!WHBGfxLoadGFDShaderGroup(&g_fshader, 0, filter_gsh)) {
        plat_log(1, "could not load filter.gsh: filters run on the CPU");
        return;
    }
    WHBGfxInitShaderAttribute(&g_fshader, "pos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32);
    WHBGfxInitShaderAttribute(&g_fshader, "color", 0, 8, GX2_ATTRIB_FORMAT_UNORM_8_8_8_8);
    WHBGfxInitFetchShader(&g_fshader);
    g_filters_ok = 1;
    uf_view = fvar(1, "view");
    uf_world0 = fvar(1, "world0");
    uf_world1 = fvar(1, "world1");
    uf_uv0 = fvar(1, "uv0");
    uf_uv1 = fvar(1, "uv1");
    uf_params = fvar(0, "fparams");
    uf_step = fvar(0, "fstep");
    uf_color = fvar(0, "fcolor");
    uf_flags = fvar(0, "fflags");
    for (int i = 0; i < 5; i++) {
        char name[8];
        snprintf(name, sizeof(name), "fm%d", i);
        uf_m[i] = fvar(0, name);
    }
    sf_src = sf_blurred = ~0u;
    for (uint32_t i = 0; i < g_fshader.pixelShader->samplerVarCount; i++) {
        if (!strcmp(g_fshader.pixelShader->samplerVars[i].name, "src")) sf_src = g_fshader.pixelShader->samplerVars[i].location;
        if (!strcmp(g_fshader.pixelShader->samplerVars[i].name, "blurred"))
            sf_blurred = g_fshader.pixelShader->samplerVars[i].location;
    }
    if (sf_src == ~0u || sf_blurred == ~0u) g_filters_ok = 0;
    if (!g_filters_ok) plat_log(1, "filter.gsh is missing uniforms: filters run on the CPU");
    GX2InitSampler(&g_border_sampler, GX2_TEX_CLAMP_MODE_CLAMP_BORDER, GX2_TEX_XY_FILTER_MODE_POINT);
    GX2InitSamplerBorderType(&g_border_sampler, GX2_TEX_BORDER_TYPE_TRANSPARENT_BLACK);
}

// Makes the three scratch textures at least w x h, all the same size (a glow samples two of them with the
// same coordinates). Returns 0 if out of memory.
static int scratch_size(uint32_t w, uint32_t h) {
    uint32_t cw = g_scratch[0].tex.surface.width, ch = g_scratch[0].tex.surface.height;
    if (g_scratch[0].tex.surface.image && g_scratch[1].tex.surface.image && g_scratch[2].tex.surface.image && cw >= w &&
        ch >= h)
        return 1;
    uint32_t nw = cw > w ? cw : (w + 63) & ~63u, nh = ch > h ? ch : (h + 63) & ~63u;
    g_heap_next = 1;
    for (int i = 0; i < 3; i++) {
        Tex* t = &g_scratch[i];
        if (t->tex.surface.image) free_tex(t);
        g_tile_next = 1;
        set_tex_surface(t, nw, nh);
        g_tile_next = 0;
        if (!t->tex.surface.image) {
            g_heap_next = 0;
            return 0;
        }
        make_target(t);
        if (!t->db.surface.image) {
            g_heap_next = 0;
            return 0;
        }
        t->used = 1;
    }
    g_heap_next = 0;
    return 1;
}

static int g_filter_state_set; // loading the whole GPU state for every pass was slow: once per filter

static uint32_t g_pass_mask[2]; // the stencil test for a pass (only blended layers use one)

static void filter_target(Tex* t) {
    if (!g_filter_state_set) {
        GX2SetContextState(g_ctx);
        g_filter_state_set = 1;
    }
    GX2SetColorBuffer(&t->cb, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&t->db);
    uint32_t w = t->tex.surface.width, h = t->tex.surface.height;
    GX2SetViewport(0, 0, (float)w, (float)h, 0.0f, 1.0f);
    GX2SetScissor(0, 0, w, h);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&g_fshader.fetchShader);
    GX2SetVertexShader(g_fshader.vertexShader);
    GX2SetPixelShader(g_fshader.pixelShader);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE); // no blending: results replace what's there
    GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
                         FALSE, FALSE, FALSE);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    set_mask(g_pass_mask[0], g_pass_mask[1]);
    float view[4] = { 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f };
    GX2SetVertexUniformReg(uf_view, 4, view);
}

// The colour matrix for the next pass (20 floats by rows, offsets 0..255), or NULL.
static const float* g_pass_matrix;
static const float* g_pass_shadow; // a bevel's shadow colour (4 floats), or NULL

// One pass: texels (sx, sy, sw, sh) of `src` drawn to (dx, dy, sw, sh) of `dst` with filter mode `mode`.
static void filter_pass(Tex* dst, const GX2Texture* src, const GX2Texture* blurred, float sx, float sy, float sw, float sh,
                        float dx, float dy, const float params[4], const float step[4], const float color[4],
                        const float flags[4]) {
    filter_target(dst);
    float tw = (float)src->surface.width, th = (float)src->surface.height;
    float world0[4] = { sw, 0, 0, sh }, world1[4] = { dx, dy, 0, 0 };
    float uv0[4] = { sw / tw, 0, 0, sh / th }, uv1[4] = { sx / tw, sy / th, 0, 0 };
    GX2SetVertexUniformReg(uf_world0, 4, world0);
    GX2SetVertexUniformReg(uf_world1, 4, world1);
    GX2SetVertexUniformReg(uf_uv0, 4, uv0);
    GX2SetVertexUniformReg(uf_uv1, 4, uv1);
    GX2SetPixelUniformReg(uf_params, 4, params);
    GX2SetPixelUniformReg(uf_step, 4, step);
    GX2SetPixelUniformReg(uf_color, 4, color);
    GX2SetPixelUniformReg(uf_flags, 4, flags);
    if (g_pass_matrix) {
        const float* m = g_pass_matrix;
        for (int r = 0; r < 4; r++) GX2SetPixelUniformReg(uf_m[r], 4, &m[r * 5]);
        float offs[4] = { m[4] / 255.0f, m[9] / 255.0f, m[14] / 255.0f, m[19] / 255.0f };
        GX2SetPixelUniformReg(uf_m[4], 4, offs);
    }
    if (g_pass_shadow) GX2SetPixelUniformReg(uf_m[0], 4, g_pass_shadow);
    GX2SetPixelTexture(src, sf_src);
    GX2SetPixelSampler(&g_border_sampler, sf_src);
    GX2SetPixelTexture(blurred ? blurred : src, sf_blurred);
    GX2SetPixelSampler(&g_border_sampler, sf_blurred);
    draw_mesh(&g_quad, 0, 6, GX2_PRIMITIVE_MODE_TRIANGLES);
    end_target(dst);
}

// Makes the top-left w x h of a scratch texture, plus the margin later passes may read past it,
// transparent (it holds the last filter's pixels). A pass over just that area: clearing the whole
// texture for every pass, however small the filter, was the slowest part.
static uint32_t g_clear_margin;

static void clear_scratch(Tex* t, const GX2Texture* any, uint32_t w, uint32_t h) {
    static const float mode[4] = { -1.0f, 0, 0, 0 };
    static const float zero[4] = { 0, 0, 0, 0 };
    float cw = (float)(w + g_clear_margin), ch = (float)(h + g_clear_margin);
    if (cw > (float)t->tex.surface.width) cw = (float)t->tex.surface.width;
    if (ch > (float)t->tex.surface.height) ch = (float)t->tex.surface.height;
    filter_pass(t, any, NULL, 0, 0, cw, ch, 0, 0, mode, zero, zero, zero);
}

// Box blur passes over the top-left w x h of scratch a (the result ends up in a; b is the other buffer).
static void blur_passes(Tex* a, Tex* b, uint32_t w, uint32_t h, float bx, float by, int passes) {
    static const float zero[4] = { 0, 0, 0, 0 };
    for (int p = 0; p < passes; p++) {
        for (int dir = 0; dir < 2; dir++) {
            float width = dir ? by : bx;
            if (width <= 1.0f) continue;
            float r = (width - 1.0f) / 2.0f;
            float taps = floorf(r);
            float params[4] = { 1.0f, taps, r - taps, 1.0f / (2.0f * r + 1.0f) };
            float step[4] = { dir ? 0.0f : 1.0f / (float)a->tex.surface.width, dir ? 1.0f / (float)a->tex.surface.height : 0.0f,
                              0, 0 };
            clear_scratch(b, &a->tex, w, h);
            filter_pass(b, &a->tex, NULL, 0, 0, (float)w, (float)h, 0, 0, params, step, zero, zero);
            Tex tmp = *a;
            *a = *b;
            *b = tmp;
        }
    }
}

// kind 1: blur (p: blur x, blur y, passes); 2: glow / drop shadow (p: blur x, blur y, passes, strength,
// offset x, offset y, premultiplied r, g, b, a, inner, knockout, hide object). Filters texels
// (sx, sy, sw, sh) of texture `src` into texture `dst` at (dx, dy). Returns 0 if it couldn't (the caller
// then filters on the CPU).
static int g_filters_on_cpu; // test scripts: "cpufilters" compares against the CPU versions

void gfx_gx2_filters_on_cpu(int on) {
    g_filters_on_cpu = on;
}

void gfx_tex_copy(uint32_t dst, uint32_t src) {
    static const float zero[4] = { 0, 0, 0, 0 };
    static int tried;
    if (!tried) {
        tried = 1;
        if (!g_filters_ok) init_filters();
    }
    if (!g_filters_ok || g_gpu_lost || dst >= g_ntexs || !g_texs[dst].used) return;
    Tex* s = &g_game_screen;
    if (src) {
        if (src >= g_ntexs || !g_texs[src].used) return;
        s = &g_texs[src];
    }
    Tex* d = &g_texs[dst];
    make_target(d);
    make_quad();
    g_filter_state_set = 0;
    uint32_t w = d->tex.surface.width, h = d->tex.surface.height;
    d->drawn_epoch = g_epoch;
    s->drawn_epoch = g_epoch;
    filter_pass(d, &s->tex, NULL, 0, 0, (float)w, (float)h, 0, 0, zero, zero, zero, zero);
}

uint32_t gfx_tex_create_float(uint32_t w, uint32_t h, const float* rgba) {
    g_float_next = 1;
    uint32_t id = gfx_tex_create(w, h, NULL, 0);
    g_float_next = 0;
    if (!id) return 0;
    Tex* t = &g_texs[id];
    uint8_t* img = (uint8_t*)t->tex.surface.image;
    if (rgba && img) {
        uint32_t pitch = t->tex.surface.pitch * 16;
        for (uint32_t y = 0; y < h; y++) memcpy(img + (size_t)y * pitch, rgba + (size_t)y * w * 4, (size_t)w * 16);
        GX2Invalidate(GX2_INVALIDATE_MODE_CPU_TEXTURE, img, t->tex.surface.imageSize);
    }
    return id;
}

void gfx_tex_read_float(uint32_t id, uint32_t w, uint32_t h, float* out) {
    memset(out, 0, (size_t)w * h * 16);
    if (id >= g_ntexs || !g_texs[id].used || g_gpu_lost) return;
    gpu_sync();
    Tex* t = &g_texs[id];
    uint8_t* img = (uint8_t*)t->tex.surface.image;
    if (!img) return;
    DCInvalidateRange(img, t->tex.surface.imageSize);
    uint32_t pitch = t->tex.surface.pitch * 16;
    for (uint32_t y = 0; y < h; y++) memcpy(out + (size_t)y * w * 4, img + (size_t)y * pitch, (size_t)w * 16);
}

static uint32_t pb_uniform(GX2PixelShader* p, const char* name) {
    uint32_t off = ~0u;
    for (uint32_t i = 0; i < p->uniformVarCount; i++)
        if (strstr(p->uniformVars[i].name, name) && p->uniformVars[i].offset < off) off = p->uniformVars[i].offset;
    return off;
}

int gfx_pb_run(uint32_t program, uint32_t target, const uint32_t* textures, const float* floats, uint32_t nfloats,
               const int32_t* ints, uint32_t nints, int zero_outside, uint32_t linear_mask) {
    if (g_gpu_lost || !program || program >= g_c3d_nprogs || !g_c3d_progs[program].used) return 0;
    C3dProg* pr = &g_c3d_progs[program];
    // A shader is usually run once: wait for it to compile rather than lose the result.
    uint64_t until = plat_time_ns() + 15000000000ull;
    while (!prog_ready(pr) && pr->job && plat_time_ns() < until) OSSleepTicks(OSMillisecondsToTicks(2));
    if (!pr->ready) return 0;
    Tex* t = &g_game_screen;
    if (target) {
        if (target >= g_ntexs || !g_texs[target].used) return 0;
        t = &g_texs[target];
        make_target(t);
    } else {
        g_shown = t;
    }
    make_quad();
    t->drawn_epoch = g_epoch;
    GX2SetContextState(g_ctx);
    GX2SetColorBuffer(&t->cb, GX2_RENDER_TARGET_0);
    GX2SetDepthBuffer(&t->db);
    uint32_t w = t->tex.surface.width, h = t->tex.surface.height;
    GX2SetViewport(0, 0, (float)w, (float)h, 0.0f, 1.0f);
    GX2SetScissor(0, 0, w, h);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&g_shader.fetchShader); // position first, the same as the program's vertex shader
    GX2SetVertexShader(pr->vs);
    GX2SetPixelShader(pr->ps);
    GX2PixelShader* p = pr->ps;
    uint32_t off = pb_uniform(p, "binding_3_fs");
    if (off != ~0u && nfloats) GX2SetPixelUniformReg(off, nfloats, floats);
    off = pb_uniform(p, "binding_4_fs");
    if (off != ~0u && nints) GX2SetPixelUniformReg(off, nints, ints);
    off = pb_uniform(p, "binding_5_fs");
    if (off != ~0u) {
        const float z = zero_outside ? 1.0f : 0.0f;
        const float mode[4] = { z, z, z, z };
        GX2SetPixelUniformReg(off, 4, mode);
    }
    for (uint32_t i = 0; i < p->samplerVarCount; i++) {
        int n = name_number(p->samplerVars[i].name, "binding_") - 6;
        if (n < 0 || n >= 8 || !textures[n] || textures[n] >= g_ntexs || !g_texs[textures[n]].used) continue;
        g_texs[textures[n]].drawn_epoch = g_epoch;
        uint32_t loc = p->samplerVars[i].location;
        GX2SetPixelTexture(&g_texs[textures[n]].tex, loc);
        // clamp (wrap mode 0), filtering by what the shader asks for
        GX2SetPixelSampler(&g_c3d_samplers[(linear_mask >> n & 1) ? 1 : 0], loc);
    }
    // A shader job's result replaces what's in its target; a filter's or blend's goes over it.
    GX2SetColorControl(GX2_LOGIC_OP_COPY, zero_outside ? 0xff : 0x00, FALSE, TRUE);
    GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD,
                       TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, FALSE, FALSE);
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    draw_mesh(&g_quad, 0, 6, GX2_PRIMITIVE_MODE_TRIANGLES);
    end_target(t);
    return 1;
}

int gfx_blend_layer(uint32_t target, uint32_t layer, uint32_t mode, uint32_t mask_state, uint32_t mask_depth) {
    static const float zero[4] = { 0, 0, 0, 0 };
    static int tried;
    if (!tried) { // (done when first needed, like the filters)
        tried = 1;
        if (!g_filters_ok) init_filters();
    }
    if (!g_filters_ok || g_gpu_lost || layer >= g_ntexs || !g_texs[layer].used) return 0;
    Tex* dst = &g_game_screen;
    if (target) {
        if (target >= g_ntexs || !g_texs[target].used) return 0;
        dst = &g_texs[target];
        make_target(dst);
    }
    uint32_t w = dst->tex.surface.width, h = dst->tex.surface.height;
    if (!scratch_size(w, h)) return 0;
    make_quad();
    g_filter_state_set = 0;
    // A copy of what's behind: a target can't be read while being drawn into.
    const float copy[4] = { 0, 0, 0, 0 };
    filter_pass(&g_scratch[0], &dst->tex, NULL, 0, 0, (float)w, (float)h, 0, 0, copy, zero, zero, zero);
    // The layer too: the pass reads both with one set of coordinates, so they must be the same size.
    filter_pass(&g_scratch[1], &g_texs[layer].tex, NULL, 0, 0, (float)w, (float)h, 0, 0, copy, zero, zero, zero);
    const float params[4] = { 5.0f, (float)mode, 0, 0 };
    g_pass_mask[0] = mask_state;
    g_pass_mask[1] = mask_depth;
    filter_pass(dst, &g_scratch[1].tex, &g_scratch[0].tex, 0, 0, (float)w, (float)h, 0, 0, params, zero, zero, zero);
    g_pass_mask[0] = g_pass_mask[1] = 0;
    return 1;
}

int gfx_filter(uint32_t kind, uint32_t src, uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh, uint32_t dst, int32_t dx,
               int32_t dy, const float* p, uint32_t np) {
    static int inited;
    if (g_filters_on_cpu) return 0;
    if (!inited) {
        inited = 1;
        init_filters();
    }
    if (!g_filters_ok || g_gpu_lost || (kind == 1 && np < 3) || (kind == 2 && np < 13) || (kind == 3 && np < 20) ||
        (kind == 4 && np < 17) || kind < 1 || kind > 4)
        return 0;
    if (src >= g_ntexs || !g_texs[src].used || dst >= g_ntexs || !g_texs[dst].used || !sw || !sh) return 0;
    if (sw > 4096 || sh > 4096) return 0;
    make_quad();
    g_filter_state_set = 0;
    // How far past the filtered area a pass can read: the blur's reach and a shadow or bevel's offset.
    g_clear_margin = 2;
    if (kind != 3) {
        float reach = (p[0] > p[1] ? p[0] : p[1]) / 2.0f + 2.0f;
        if (kind != 1) reach += fabsf(p[4]) + fabsf(p[5]);
        g_clear_margin = (uint32_t)reach + 2;
    }
    Tex* s = &g_texs[src];
    Tex* d = &g_texs[dst];
    s->drawn_epoch = g_epoch;
    d->drawn_epoch = g_epoch;
    make_target(d);
    if (!d->db.surface.image || !scratch_size(sw + 2, sh + 2)) return 0;
    Tex* a = &g_scratch[0];
    Tex* b = &g_scratch[1];
    Tex* c = kind == 2 || kind == 4 ? &g_scratch[2] : NULL;
    static const float zero[4] = { 0, 0, 0, 0 };
    static const float copy_params[4] = { 0, 0, 0, 0 };
    if (kind == 3) {
        // Colour matrix: the source into scratch a (the destination may be the source), then one pass.
        // p: the 4 x 5 matrix by rows, the offsets in 0..255 like Flash's.
        clear_scratch(a, &s->tex, sw, sh);
        filter_pass(a, &s->tex, NULL, (float)sx, (float)sy, (float)sw, (float)sh, 0, 0, copy_params, zero, zero, zero);
        float params[4] = { 3.0f, 0, 0, 0 };
        g_pass_matrix = p;
        filter_pass(d, &a->tex, NULL, 0, 0, (float)sw, (float)sh, (float)dx, (float)dy, params, zero, zero, zero);
        g_pass_matrix = NULL;
        return 1;
    }
    int passes = (int)p[2];
    if (passes < 1) passes = 1;
    // The source into scratch a (and c for glows: the object itself, as the destination may be the source).
    clear_scratch(a, &s->tex, sw, sh);
    filter_pass(a, &s->tex, NULL, (float)sx, (float)sy, (float)sw, (float)sh, 0, 0, copy_params, zero, zero, zero);
    if (c) {
        clear_scratch(c, &s->tex, sw, sh);
        filter_pass(c, &s->tex, NULL, (float)sx, (float)sy, (float)sw, (float)sh, 0, 0, copy_params, zero, zero, zero);
    }
    blur_passes(a, b, sw, sh, p[0], p[1], passes);
    if (kind == 1) {
        filter_pass(d, &a->tex, NULL, 0, 0, (float)sw, (float)sh, (float)dx, (float)dy, copy_params, zero, zero, zero);
    } else if (kind == 4) {
        // Bevel. p: blur x, blur y, passes, strength, offset x, y, highlight r, g, b, a, shadow r, g, b, a
        // (premultiplied), inner, knockout, on top.
        float params[4] = { 4.0f, 0, 0, 0 };
        float step[4] = { 0, 0, p[4] / (float)a->tex.surface.width, p[5] / (float)a->tex.surface.height };
        float color[4] = { p[6], p[7], p[8], p[9] };
        float flags[4] = { p[3], p[14], p[15], p[16] };
        g_pass_shadow = &p[10];
        filter_pass(d, &c->tex, &a->tex, 0, 0, (float)sw, (float)sh, (float)dx, (float)dy, params, step, color, flags);
        g_pass_shadow = NULL;
    } else {
        float params[4] = { 2.0f, 0, 0, 0 };
        float step[4] = { 0, 0, p[4] / (float)a->tex.surface.width, p[5] / (float)a->tex.surface.height };
        float color[4] = { p[6], p[7], p[8], p[9] };
        float flags[4] = { p[3], p[10], p[11], p[12] };
        // The original (c) and the blurred copy (a) have the same size, so their uvs match.
        filter_pass(d, &c->tex, &a->tex, 0, 0, (float)sw, (float)sh, (float)dx, (float)dy, params, step, color, flags);
    }
    return 1;
}

// Optional art shown around a game that doesn't fill the screen (ui_draw_border), and the stage
// rectangle (in screen-target pixels) that is drawn from the game over it.
static Tex g_border;
static int g_border_on;
static float g_stage[4];
static float g_fade = 1.0f;

void gfx_gx2_set_fade(float brightness) {
    g_fade = brightness < 0 ? 0 : brightness > 1 ? 1 : brightness;
}

void gfx_gx2_set_border(const uint8_t* rgba, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!rgba) {
        g_border_on = 0;
        return;
    }
    uint32_t sw = g_screen.tex.surface.width, sh = g_screen.tex.surface.height;
    // A new picture for each game: on the console the last game's art stayed up, cut off by a wider game.
    if (g_border.tex.surface.image) free_tex(&g_border);
    g_heap_next = 1;
    set_tex_surface(&g_border, sw, sh);
    g_heap_next = 0;
    gpu_sync();
    write_pixels(&g_border, 0, 0, sw, sh, rgba, sw * 4);
    // Another game's art was seen on the console, cut off by this game's wider picture: drop any cached copy.
    GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, g_border.tex.surface.image, g_border.tex.surface.imageSize);
    g_stage[0] = (float)x; g_stage[1] = (float)y; g_stage[2] = (float)w; g_stage[3] = (float)h;
    g_border_on = 1;
}

// Small picture drawn over the game's top-left corner (the FPS counter), in screen-texture pixels.
static Tex g_hud;
static int g_hud_on;

void gfx_gx2_set_hud(const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (!rgba) {
        g_hud_on = 0;
        return;
    }
    g_heap_next = 1;
    if (!g_hud.tex.surface.image) set_tex_surface(&g_hud, w, h);
    g_heap_next = 0;
    if (g_hud.tex.surface.width != w || g_hud.tex.surface.height != h) return;
    gpu_wait_last_frame(); // the last frame may still be reading it
    write_pixels(&g_hud, 0, 0, w, h, rgba, w * 4);
    g_hud_on = 1;
}

// The mouse arrow for the left stick, with transparent corners, over everything.
static Tex g_cursor;
static int g_cursor_on;
static float g_cursor_xy[2];

void gfx_gx2_set_cursor(const uint8_t* rgba, uint32_t w, uint32_t h) {
    if (g_cursor.tex.surface.image) return; // (always the same picture)
    g_heap_next = 1;
    set_tex_surface(&g_cursor, w, h);
    g_heap_next = 0;
    write_pixels(&g_cursor, 0, 0, w, h, rgba, w * 4);
}

void gfx_gx2_set_cursor_pos(int on, float x, float y) {
    g_cursor_on = on;
    g_cursor_xy[0] = x;
    g_cursor_xy[1] = y;
}

static Tex g_panel[2];
static int g_panel_on[2];
static float g_panel_xy[2][2];

void gfx_gx2_set_panel(int slot, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t stride, float x, float y) {
    Tex* t = &g_panel[slot];
    g_heap_next = 1;
    if (!t->tex.surface.image) set_tex_surface(t, w, h);
    g_heap_next = 0;
    gpu_wait_last_frame(); // the last frame may still be reading it
    write_pixels(t, 0, 0, w, h, rgba, stride);
    g_panel_xy[slot][0] = x;
    g_panel_xy[slot][1] = y;
}

void gfx_gx2_panel_on(int slot, int on) {
    g_panel_on[slot] = on;
}

// Draws texture rect (sx, sy, sw, sh in texture pixels) to (dx, dy, dw, dh) of a w x h output.
static void blit(GX2Texture* tex, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
                 uint32_t w, uint32_t h) {
    float tw = (float)tex->surface.width, th = (float)tex->surface.height;
    float view[4] = { 2.0f / (float)w, -2.0f / (float)h, -1.0f, 1.0f };
    float world0[4] = { dw, 0, 0, dh }, world1[4] = { dx, dy, 0, 0 };
    float uv0[4] = { sw / tw, 0, 0, sh / th }, uv1[4] = { sx / tw, sy / th, 0, 0 };
    // g_fade darkens the whole picture (menu transitions); the screen texture's alpha is ignored.
    float mult[4] = { g_fade, g_fade, g_fade, 1 }, add[4] = { 0, 0, 0, 0 }, params[4] = { 1, 0, 0, 0 };
    GX2SetVertexUniformReg(u_view, 4, view);
    GX2SetVertexUniformReg(u_world0, 4, world0);
    GX2SetVertexUniformReg(u_world1, 4, world1);
    GX2SetVertexUniformReg(u_uv0, 4, uv0);
    GX2SetVertexUniformReg(u_uv1, 4, uv1);
    GX2SetPixelUniformReg(u_mult, 4, mult);
    GX2SetPixelUniformReg(u_add, 4, add);
    GX2SetPixelUniformReg(u_params, 4, params);
    GX2SetPixelTexture(tex, s_tex);
    GX2SetPixelSampler(&g_samplers[1], s_tex);
    draw_mesh(&g_quad, 0, 6, GX2_PRIMITIVE_MODE_TRIANGLES);
}

static void draw_screen_to(uint32_t w, uint32_t h, int drc) {
    make_quad();
    WHBGfxClearColor(0, 0, 0, 1);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_REGISTER);
    GX2SetFetchShader(&g_shader.fetchShader);
    GX2SetVertexShader(g_shader.vertexShader);
    GX2SetPixelShader(g_shader.pixelShader);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, 0, 0, 0, 0, 0, 0, 0);
    // The system keyboard (g_overlay) draws with the same context state and leaves its settings behind.
    GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    GX2SetPolygonControl(GX2_FRONT_FACE_CCW, FALSE, FALSE, FALSE, GX2_POLYGON_MODE_TRIANGLE, GX2_POLYGON_MODE_TRIANGLE,
                         FALSE, FALSE, FALSE);
    // Fit the screen texture into w x h keeping its aspect.
    float sw = (float)g_screen.tex.surface.width, sh = (float)g_screen.tex.surface.height;
    float scale = (float)w / sw < (float)h / sh ? (float)w / sw : (float)h / sh;
    float dw = sw * scale, dh = sh * scale;
    float ox = ((float)w - dw) / 2, oy = ((float)h - dh) / 2;
    if (g_border_on) {
        blit(&g_border.tex, 0, 0, sw, sh, ox, oy, dw, dh, w, h);
        blit(&g_shown->tex, g_stage[0], g_stage[1], g_stage[2], g_stage[3], ox + g_stage[0] * scale,
             oy + g_stage[1] * scale, g_stage[2] * scale, g_stage[3] * scale, w, h);
    } else {
        blit(&g_shown->tex, 0, 0, sw, sh, ox, oy, dw, dh, w, h);
    }
    if (g_hud_on) {
        float hw = (float)g_hud.tex.surface.width, hh = (float)g_hud.tex.surface.height;
        blit(&g_hud.tex, 0, 0, hw, hh, ox + 8 * scale, oy + 8 * scale, hw * scale, hh * scale, w, h);
    }
    // The on-screen keyboard and its tab are touched, so they're only on the GamePad.
    for (int i = 0; drc && i < 2; i++) {
        if (!g_panel_on[i] || !g_panel[i].tex.surface.image) continue;
        float pw = (float)g_panel[i].tex.surface.width, ph = (float)g_panel[i].tex.surface.height;
        blit(&g_panel[i].tex, 0, 0, pw, ph, ox + g_panel_xy[i][0] * scale, oy + g_panel_xy[i][1] * scale, pw * scale, ph * scale, w, h);
    }
    if (g_cursor_on && g_cursor.tex.surface.image) {
        float cw = (float)g_cursor.tex.surface.width, ch = (float)g_cursor.tex.surface.height;
        GX2SetAlphaTest(TRUE, GX2_COMPARE_FUNC_GREATER, 0.5f);
        blit(&g_cursor.tex, 0, 0, cw, ch, ox + g_cursor_xy[0] * scale, oy + g_cursor_xy[1] * scale, cw * scale, ch * scale,
             w, h);
        GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0.0f);
    }
}

// Draws the game's tiled picture into the linear one, which the CPU can read. (GX2CopySurface left it
// unchanged in Cemu.)
static void copy_game_screen(void) {
    uint32_t w = g_screen.tex.surface.width, h = g_screen.tex.surface.height;
    float fade = g_fade;
    gpu_sync();
    make_target(&g_screen);
    make_quad();
    begin_target(&g_screen);
    GX2SetColorControl(GX2_LOGIC_OP_COPY, 0x00, FALSE, TRUE);
    GX2SetDepthOnlyControl(FALSE, FALSE, GX2_COMPARE_FUNC_ALWAYS);
    g_fade = 1.0f;
    blit(&g_game_screen.tex, 0, 0, (float)w, (float)h, 0, 0, (float)w, (float)h, w, h);
    g_fade = fade;
    end_target(&g_screen);
}

static void (*g_overlay)(int drc);

void gfx_gx2_set_overlay(void (*draw)(int drc)) {
    g_overlay = draw;
}

void gfx_gx2_present(void) {
    if (g_gpu_lost) return;
    WHBGfxBeginRender();
    WHBGfxBeginRenderTV();
    GX2ColorBuffer* tv = WHBGfxGetTVColourBuffer();
    draw_screen_to(tv->surface.width, tv->surface.height, 0);
    if (g_overlay) g_overlay(0);
    WHBGfxFinishRenderTV();
    WHBGfxBeginRenderDRC();
    GX2ColorBuffer* drc = WHBGfxGetDRCColourBuffer();
    draw_screen_to(drc->surface.width, drc->surface.height, 1);
    if (g_overlay) g_overlay(1);
    WHBGfxFinishRenderDRC();
    GX2Flush(); // the pictures so far as their own batch: gpu_wait_last_frame waits for just these
    g_blit_ts = GX2GetLastSubmittedTimeStamp();
    // WHBGfxFinishRender without its GX2DrawDone: that (and a second wait here) made the CPU sit idle
    // while the GPU drew each frame, so a frame cost the game's time plus the GPU's, and anything over a
    // few ms of game logic missed the TV's refresh (Zuma at 42 fps with 7 ms of logic). Now the GPU draws
    // this frame while the CPU runs the next; WHBGfxBeginRender still waits for the last frame's flip.
    GX2SwapScanBuffers();
    GX2Flush();
    GX2SetTVEnable(TRUE);
    GX2SetDRCEnable(TRUE);
    OSTime ts = GX2GetLastSubmittedTimeStamp();
    // The frame before this one is done: release what was freed while it was drawn.
    if (g_prev_ts && !g_gpu_lost) GX2WaitTimeStamp(g_prev_ts);
    for (uint32_t i = 0; i < g_prev_npending; i++) gpu_free_now(g_prev_pending[i]);
    g_prev_npending = 0;
    void** swap = g_prev_pending;
    uint32_t swap_cap = g_prev_cap;
    g_prev_pending = g_pending;
    g_prev_cap = g_cappending;
    g_prev_npending = g_npending;
    g_pending = swap;
    g_cappending = swap_cap;
    g_npending = 0;
    g_prev_ts = ts;
    g_frame_no++;
    g_prev_epoch = g_epoch;
    g_epoch++;
    g_c3d_drew = 0;
}

void gfx_gx2_screen_size(uint32_t* w, uint32_t* h) {
    *w = g_screen.tex.surface.width;
    *h = g_screen.tex.surface.height;
}
