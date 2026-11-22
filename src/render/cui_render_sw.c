/* Software rasterizer for CUI draw data.
 *
 * Implements exactly the same shading as the GPU shader (see
 * docs/rendering.md): signed distance rounded rectangles with borders,
 * gaussian shadows, alpha-texture text, images, solid geometry and ordered dither, blended
 * with premultiplied alpha. Big frames are split over worker threads by tile rows.
 *
 * Incremental frames draw only the 32 pixel tiles whose content changed, and move the
 * pixels of tiles whose content only moved up or down (a scroll) instead of drawing them.
 * A part hidden under an opaque rectangle or picture is not drawn. One color over many
 * pixels (text, fills, shadows, edges without a border) is blended in 16 bit fixed point,
 * four pixels at a time with SSE2. */
#include "../cui_internal.h"
#include "cui/cui_render.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

typedef struct cui__sw_tex {
    int w, h;
    cui_texture_format fmt;
    uint8_t *data;
    bool used;
    bool opaque;   /* RGBA8 with every alpha 255 (an update that brings alpha keeps it false): its pictures hide what is under them */
} cui__sw_tex;

/* every alpha of n RGBA texels is 255 */
static bool cui__sw_all_opaque(const uint8_t *p, size_t n) {
    uint8_t a = 255;
    for (size_t i = 0; i < n; i++) a &= p[i * 4 + 3];
    return a == 255;
}

#define CUI__SW_TILE 32

/* One thing the frame draws: an axis aligned quad (two triangles) or a triangle, with what
 * its pixels depend on. h covers everything but the position, so the same thing moved by a
 * scroll hashes the same relative to where it is. */
typedef struct cui__sw_prim {
    uint64_t h;                  /* vertices relative to the first one, texture, kind */
    float ax, ay;                /* the first vertex */
    float bx0, by0, bx1, by1;    /* bounds of the vertices */
    int cx0, cy0, cx1, cy1;      /* the clip in whole pixels, as the raster cuts by it */
    int ua[4], ub[4];            /* pixels it paints in one way throughout (x0, y0, x1, y1), or empty */
    int oa[4], ob[4];            /* with occ: pixels it paints opaque, whatever is under them (A and B of cui__sw_inside) */
    uint32_t cmd, k;             /* command, first index in it */
    uint32_t key;                /* color and shading of the uniform part */
    uint8_t quad, forced, mode;  /* six indices; samples a part of a texture written since last frame; shading */
    uint8_t occ;
    uint8_t layer;               /* a group's bitmap (layer + 1 in r->layers), blended instead of its commands; 0 none */
} cui__sw_prim;

typedef struct cui__sw_pool cui__sw_pool;

/* The layer cache (cui_set_layer_cache): a group drawn once into a bitmap at its rest place, then
 * blended each frame where the group's transform puts it. The bitmap has a clear one pixel border,
 * so a sample at its edge reads nothing outside it. */
#define CUI__SW_LAYERS 4
#define CUI__SW_LAYER_PX (2048 * 2048)           /* one bitmap at most, in pixels (16 MB) */
#define CUI__SW_LAYERS_PX (2 * CUI__SW_LAYER_PX)  /* all of them (32 MB) */
typedef struct cui__sw_layer {
    cui_id id;
    bool used, valid, fresh, seen;  /* valid: the pixels are what hash says; fresh: made this frame */
    bool drawn;                     /* the pixels were drawn on the last frame */
    uint64_t hash, last;            /* of the content in the pixels; of the content last frame */
    int x0, y0, w, h;               /* the bitmap's place in rest pixels, and size */
    cui_sw_format fmt;
    uint8_t *px; size_t cap;
    int *span; int span_cap;        /* per row: the first and one past the last pixel that is not clear */
    float s, dx, dy; int a256;      /* this frame: scale, offset (pixels) and opacity (0..256) */
    /* this frame, scaled: per screen column from tx0 the bitmap column left of its sample and the two weights (low 16
     * bits for that column, high for the next); per screen row from ty0 the bitmap row, its weight and the screen
     * columns [a, b) that read pixels which are not clear */
    int tx0, tnx, ty0, tny, tcap_x, tcap_y;
    int *tcol; int32_t *tw; int *trow;
    uint16_t *tw4;                  /* tw's two weights four times each (one per channel): tcap_x * 4 first ones, then as many second ones */
} cui__sw_layer;

struct cui_sw_renderer {
    cui__sw_tex *tex;
    int tex_count, tex_cap;
    int threads;
    cui__sw_pool *pool;                    /* threads - 1 workers, made on first use */
    /* incremental: repaint only the tiles whose triangles changed since the last frame */
    bool incremental;
    const void *last_px; int last_w, last_h, last_stride; cui_sw_format last_fmt;
    uint64_t *tiles, *prev, *moved; int tiles_cap;
    uint8_t *force;                        /* tiles that sample a part of a texture that changed */
    uint8_t *what;                         /* per tile this frame: 0 as it was, 1 moved, 2 drawn */
    int damage[4];                         /* x0, y0, x1, y1 of the last frame's repaint */
    int last_shift;                        /* pixels the last frame moved by (0: none) */
    float last_drawn, last_moved;          /* shares of the last frame drawn and moved */
    /* this frame's and the last frame's primitives; per tile row, the ones over it */
    cui__sw_prim *prims, *pprims; int nprims, npprims, prims_cap, pprims_cap, prims_big;
    uint32_t *row_start, *row_items; int row_start_cap, row_items_cap;
    uint64_t *vkeys; float *vy; int vcap;  /* last frame's primitives by content and x: y */
    int *jobs; int jobs_cap;               /* parts to draw: x0, y0, x1, y1, tile row */
    int shifts[3];                         /* this frame's move distances (what 1, 3, 4) */
    uint8_t *spare; size_t spare_cap;      /* pixels of tiles moved by the second and third distance */
    struct cui__sw_scratch *scratch;       /* the jobs' tables when one thread draws a frame (made on first use) */
    /* texture parts written since the last frame: triangles sampling them are drawn again
     * (new glyphs go to unused parts, so text already on screen stays put) */
    struct { cui_texture t; int x0, y0, x1, y1; } changed[32];
    int nchanged; bool changed_overflow;
    /* the layer cache: the bitmaps, the renderer that draws a group into one (it borrows these
     * textures), its copy of the group, and per group this frame the bitmap used (-1: drawn plain) */
    cui__sw_layer *layers;
    cui_sw_renderer *sub;
    bool keep_alpha;                       /* the clear keeps the clear color's alpha (the sub renderer's bitmaps) */
    cui_vertex *lvtx; int lvtx_cap;
    cui_draw_cmd *lcmd; int lcmd_cap;
    int *gslot; int gslot_cap;
    int layer_rasters; size_t layer_bytes, layer_peak;
    /* what is under a group's bitmap, kept: the frame's pixels of a tile just before the bitmap is blended,
     * saved the first time and copied back while what is drawn there before the bitmap stays the same (the
     * hash of it, `under`, as the tile hashes count it), instead of drawing it again on every frame of the
     * animation. Per tile: under (0: no bitmap over it this frame; 2: only fills under it, drawn again; else odd),
     * its slot in upx (-1 none), the hash the slot was saved with, whether it was saved, this frame's use (0 none,
     * 1 save, 2 copy back), and whether anything but a fill of the whole tile is drawn before the bitmap. */
    uint64_t *under, *uhash;
    int *uslot;
    uint8_t *uvalid, *umode, *ubusy;
    int nlayer_prims;                      /* this frame's primitives that blend a bitmap */
    uint8_t *upx; int ucount, ucap;        /* the slots, CUI__SW_TILE squared pixels each */
    int utw, uw, uh; cui_sw_format ufmt;   /* the frame they were saved from (another one drops them) */
    size_t ubytes;
    long usaved, urestored, usnapped;      /* tiles saved and copied back, groups snapped, so far (cui__sw_under_stats) */
};

static void cui__sw_note_change(cui_sw_renderer *r, cui_texture t, int x0, int y0, int x1, int y1) {
    for (int i = 0; i < r->nchanged; i++)
        if (r->changed[i].t == t) {
            if (x0 < r->changed[i].x0) r->changed[i].x0 = x0;
            if (y0 < r->changed[i].y0) r->changed[i].y0 = y0;
            if (x1 > r->changed[i].x1) r->changed[i].x1 = x1;
            if (y1 > r->changed[i].y1) r->changed[i].y1 = y1;
            return;
        }
    if (r->nchanged == 32) { r->changed_overflow = true; return; }
    r->changed[r->nchanged].t = t;
    r->changed[r->nchanged].x0 = x0; r->changed[r->nchanged].y0 = y0; r->changed[r->nchanged].x1 = x1; r->changed[r->nchanged].y1 = y1;
    r->nchanged++;
}

static void *cui__sw_xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q && n) { fprintf(stderr, "cui: out of memory\n"); abort(); }
    return q;
}

static cui_texture cui__sw_tex_create(void *user, int w, int h, cui_texture_format fmt, const void *pixels) {
    cui_sw_renderer *r = user;
    int slot = -1;
    for (int i = 0; i < r->tex_count; i++) if (!r->tex[i].used) { slot = i; break; }
    if (slot < 0) {
        if (r->tex_count == r->tex_cap) {
            r->tex_cap = r->tex_cap ? r->tex_cap * 2 : 16;
            r->tex = cui__sw_xrealloc(r->tex, sizeof(cui__sw_tex) * (size_t)r->tex_cap);
        }
        slot = r->tex_count++;
    }
    cui__sw_tex *t = &r->tex[slot];
    if (w < 1 || h < 1) {   /* an empty texture would clamp sample coordinates to -1: make it one blank texel */
        w = h = 1;
        pixels = NULL;
    }
    cui__sw_note_change(r, (cui_texture)(slot + 1), 0, 0, w, h);
    t->w = w; t->h = h; t->fmt = fmt; t->used = true;
    size_t bytes = (size_t)w * (size_t)h * (size_t)fmt;
    t->data = cui__sw_xrealloc(NULL, bytes);
    if (pixels) memcpy(t->data, pixels, bytes);
    else memset(t->data, 0, bytes);
    t->opaque = fmt == CUI_TEXTURE_RGBA8 && pixels && cui__sw_all_opaque(t->data, (size_t)w * (size_t)h);
    return (cui_texture)(slot + 1);
}

static void cui__sw_tex_update(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride) {
    cui_sw_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count || !r->tex[slot].used) return;
    cui__sw_tex *t = &r->tex[slot];
    cui__sw_note_change(r, tex, x, y, x + w, y + h);
    int bpp = (int)t->fmt;
    int cx0 = x < 0 ? 0 : x, cx1 = x + w > t->w ? t->w : x + w;   /* clip to the texture: the rect may hang over an edge */
    if (cx1 <= cx0) return;
    const uint8_t *src = pixels;
    for (int row = 0; row < h; row++) {
        if (y + row < 0 || y + row >= t->h) continue;
        memcpy(t->data + ((size_t)(y + row) * (size_t)t->w + (size_t)cx0) * (size_t)bpp,
               src + (size_t)row * (size_t)stride + (size_t)(cx0 - x) * (size_t)bpp, (size_t)(cx1 - cx0) * (size_t)bpp);
        if (t->opaque && !cui__sw_all_opaque(src + (size_t)row * (size_t)stride + (size_t)(cx0 - x) * (size_t)bpp, (size_t)(cx1 - cx0))) t->opaque = false;
    }
}

static void cui__sw_tex_destroy(void *user, cui_texture tex) {
    cui_sw_renderer *r = user;
    int slot = (int)tex - 1;
    if (slot < 0 || slot >= r->tex_count) return;
    free(r->tex[slot].data);
    r->tex[slot].data = NULL;
    r->tex[slot].used = false;
}

void cui__sw_move_textures(cui_sw_renderer *r, const cui_renderer_iface *to) {
    if (!r || !to || !to->texture_create) return;
    /* free slots are made too, then let go, so the ones after them keep their numbers */
    for (int i = 0; i < r->tex_count; i++) {
        const cui__sw_tex *t = &r->tex[i];
        if (t->used) to->texture_create(to->user, t->w, t->h, t->fmt, t->data);
        else to->texture_create(to->user, 1, 1, CUI_TEXTURE_R8, NULL);
    }
    for (int i = 0; i < r->tex_count; i++)
        if (!r->tex[i].used && to->texture_destroy) to->texture_destroy(to->user, (cui_texture)(i + 1));
}

static int cui__cpu_count(void) {
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    return 1;
#endif
}

static void cui__shadow_lut_init(void);
static void cui__sw_pool_destroy(cui__sw_pool *p);

cui_sw_renderer *cui_sw_create(void) {
    cui_sw_renderer *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    cui__shadow_lut_init();
    int n = cui__cpu_count();
    r->threads = n > 8 ? 8 : (n < 1 ? 1 : n);
    return r;
}

static void cui__sw_layers_free(cui_sw_renderer *r);

void cui_sw_destroy(cui_sw_renderer *r) {
    if (!r) return;
    cui__sw_layers_free(r);
    for (int i = 0; i < r->tex_count; i++) free(r->tex[i].data);
    free(r->tex);
    cui__sw_pool_destroy(r->pool);
    free(r->tiles);
    free(r->prev);
    free(r->moved);
    free(r->force);
    free(r->what);
    free(r->prims);
    free(r->pprims);
    free(r->row_start);
    free(r->row_items);
    free(r->vkeys);
    free(r->vy);
    free(r->jobs);
    free(r->spare);
    free(r->scratch);
    free(r->under); free(r->uhash); free(r->uslot); free(r->uvalid); free(r->umode); free(r->ubusy);
    free(r);
}

void cui_sw_set_threads(cui_sw_renderer *r, int threads) { r->threads = threads < 1 ? 1 : (threads > 32 ? 32 : threads); }
void cui_sw_set_incremental(cui_sw_renderer *r, bool on) { if (r) { r->incremental = on; r->last_px = NULL; } }
void cui__sw_last_stats(const cui_sw_renderer *r, int *shift, float *drawn, float *moved) {
    if (shift) *shift = r ? r->last_shift : 0;
    if (drawn) *drawn = r ? r->last_drawn : 0;
    if (moved) *moved = r ? r->last_moved : 0;
}
/* which blend draws a scaled group's bitmap (cui__sw_test_blend): -1 the fastest there is, 0 cui__sw_blit_row,
 * 1 cui__sw_blit_mixed without AVX2, 2 with it where the processor has it; the same integers each way */
static int cui__sw_blend_level = -1;
void cui__sw_test_blend(int level) { cui__sw_blend_level = level < -1 || level > 2 ? -1 : level; }
void cui__sw_under_stats(const cui_sw_renderer *r, long *saved, long *restored, long *snapped) {
    if (saved) *saved = r ? r->usaved : 0;
    if (restored) *restored = r ? r->urestored : 0;
    if (snapped) *snapped = r ? r->usnapped : 0;
}
void cui_sw_layer_stats(const cui_sw_renderer *r, int *rasters, size_t *bytes, size_t *peak_bytes) {
    if (rasters) *rasters = r ? r->layer_rasters : 0;
    if (bytes) *bytes = r ? r->layer_bytes + r->ubytes : 0;
    if (peak_bytes) *peak_bytes = r ? r->layer_peak : 0;
}
bool cui_sw_last_damage(const cui_sw_renderer *r, int *x, int *y, int *w, int *h) {
    if (!r) return false;
    if (x) *x = r->damage[0];
    if (y) *y = r->damage[1];
    if (w) *w = r->damage[2] - r->damage[0];
    if (h) *h = r->damage[3] - r->damage[1];
    return r->damage[2] > r->damage[0] && r->damage[3] > r->damage[1];
}

cui_renderer_iface cui_sw_iface(cui_sw_renderer *r) {
    cui_renderer_iface i;
    memset(&i, 0, sizeof i);
    i.user = r;
    i.texture_create = cui__sw_tex_create;
    i.texture_update = cui__sw_tex_update;
    i.texture_destroy = cui__sw_tex_destroy;
    return i;
}

/* ---- shading ---- */

static inline float cui__sd_rrect(float px, float py, float hw, float hh, float tl, float tr, float br, float bl) {
    float r = px < 0 ? (py < 0 ? tl : bl) : (py < 0 ? tr : br);
    float m = hw < hh ? hw : hh;
    if (r > m) r = m;
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return inside + sqrtf(ox * ox + oy * oy) - r;
}

static inline float cui__erf(float x) {
    /* Abramowitz-Stegun 7.1.26 */
    float s = x < 0 ? -1.0f : 1.0f;
    x = fabsf(x);
    float t = 1.0f / (1.0f + 0.3275911f * x);
    float y = 1.0f - (((((1.061405429f * t - 1.453152027f) * t) + 1.421413741f) * t - 0.284496736f) * t + 0.254829592f) * t * expf(-x * x);
    return s * y;
}

static inline float cui__sat(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

/* The 8x8 Bayer threshold of the cell at (u, v), from the bits of x ^ y and y: the same
 * arithmetic as the shader, so both renderers light the same cells. */
static inline float cui__sw_bayer(float u, float v) {
    unsigned x = (unsigned)(int)floorf(u) & 7u, y = (unsigned)(int)floorf(v) & 7u, a = x ^ y;
    unsigned m = ((a & 1u) << 5) | ((y & 1u) << 4) | ((a & 2u) << 2) | ((y & 2u) << 1) | ((a & 4u) >> 1) | ((y & 4u) >> 2);
    return ((float)m + 0.5f) / 64.0f;
}

/* Shadow falloff 0.5 * (1 - erf(z)) over the z a shadow uses (-4 to 2.8, outside it the
 * value is 1 or ignored), as a table read with linear interpolation: within 2e-6 of the
 * formula, so at most a rare one-step difference in a pixel, at a fraction of the cost. */
#define CUI__SHADOW_N 1024
#define CUI__SHADOW_Z0 (-4.0f)
#define CUI__SHADOW_Z1 2.8f
static float cui__shadow_lut[CUI__SHADOW_N + 2];
static void cui__shadow_lut_init(void) {
    if (cui__shadow_lut[0] != 0) return;
    for (int i = CUI__SHADOW_N + 1; i >= 0; i--) {   /* entry 0 last: it marks the table done */
        float z = CUI__SHADOW_Z0 + (CUI__SHADOW_Z1 - CUI__SHADOW_Z0) * (float)i / CUI__SHADOW_N;
        cui__shadow_lut[i] = 0.5f * (1.0f - cui__erf(z));
    }
}
static inline float cui__shadow_falloff(float z) {
    if (z <= CUI__SHADOW_Z0) return 1.0f;
    float f = (z - CUI__SHADOW_Z0) * ((float)CUI__SHADOW_N / (CUI__SHADOW_Z1 - CUI__SHADOW_Z0));
    int i = (int)f;
    if (i >= CUI__SHADOW_N) return cui__shadow_lut[CUI__SHADOW_N];
    float t = f - (float)i;
    return cui__shadow_lut[i] + (cui__shadow_lut[i + 1] - cui__shadow_lut[i]) * t;
}

/* The falloff a shadow has along one axis, q being cui__sd_rrect's qx or qy there and r the
 * corner radius: as cui__sd_rrect computes the distance when the other axis is not past its
 * corner (inside + length - r), 0 where the shadow draws nothing. */
#define CUI__SH_COLS 640   /* widest part of a quad a job draws with column tables (jobs are 512 wide) */
static inline float cui__shadow_side(float q, float r, float k) {
    float d = q > 0 ? sqrtf(q * q) - r : (q + 0.0f) - r;
    float z = d / k;
    return z > CUI__SHADOW_Z1 ? 0.0f : cui__shadow_falloff(z);
}

typedef struct { float r, g, b, a; } cui__px;  /* premultiplied */

static float cui__sample_r8(const cui__sw_tex *t, float u, float v) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = CUI_CLAMP(x0, 0, t->w - 1); y0 = CUI_CLAMP(y0, 0, t->h - 1);
    x1 = CUI_CLAMP(x1, 0, t->w - 1); y1 = CUI_CLAMP(y1, 0, t->h - 1);
    const uint8_t *d = t->data;
    float a = d[y0 * t->w + x0], b = d[y0 * t->w + x1], c = d[y1 * t->w + x0], e = d[y1 * t->w + x1];
    return ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + e * fx) * fy) / 255.0f;
}

static cui__px cui__sample_rgba(const cui__sw_tex *t, float u, float v) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int xs[2] = { x0, x0 + 1 }, ys[2] = { y0, y0 + 1 };
    for (int i = 0; i < 2; i++) {
        xs[i] = CUI_CLAMP(xs[i], 0, t->w - 1);
        ys[i] = CUI_CLAMP(ys[i], 0, t->h - 1);
    }
    float w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    cui__px o = { 0, 0, 0, 0 };
    for (int k = 0; k < 4; k++) {
        const uint8_t *p = t->data + ((size_t)ys[k >> 1] * (size_t)t->w + (size_t)xs[k & 1]) * 4;
        float a = p[3] / 255.0f;
        o.r += w[k] * p[0] / 255.0f * a;
        o.g += w[k] * p[1] / 255.0f * a;
        o.b += w[k] * p[2] / 255.0f * a;
        o.a += w[k] * a;
    }
    return o;
}

/* The texel a sample at (u, v) falls on the center of (within 1/512 of a texel: the bilinear
 * mix would be that texel within half a step), or NULL. An image drawn at its own size (a
 * screenshot, an icon at 1:1) samples every pixel this way: one read instead of four. */
static inline const uint8_t *cui__texel_at(const cui__sw_tex *t, float u, float v) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    float rx = floorf(x + 0.5f), ry = floorf(y + 0.5f);
    if (fabsf(x - rx) > 1.0f / 512 || fabsf(y - ry) > 1.0f / 512) return NULL;
    int ix = (int)rx, iy = (int)ry;
    ix = CUI_CLAMP(ix, 0, t->w - 1); iy = CUI_CLAMP(iy, 0, t->h - 1);
    return t->data + ((size_t)iy * (size_t)t->w + (size_t)ix) * 4;
}

/* One color c (4 bytes, in the frame's channel order) over n pixels, each with a 16 bit weight:
 * p = (p * (65536 - a) + c * a + 32768) >> 16 per channel, the same integers as
 * cui__blend_shadow. Weights are at most 65534 (65534 to 65536 all give c: the difference is
 * at most 510 / 65536 of a step) and w NULL means every pixel takes the weight wc. With SSE2
 * (every x86-64) four pixels go at a time: p + floor(((c - p) * a + 32768) / 65536) is the same
 * value, and (c - p) * a is one multiply-add of 16 bit halves of a. */
#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  define CUI__SW_SSE2 1
#endif
#define CUI__W16_MAX 65534u
static void cui__sw_blend_w16(uint8_t *p, int n, const uint16_t *w, uint32_t wc, const uint8_t c[4]) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i zero = _mm_setzero_si128(), half = _mm_set1_epi32(32768);
    __m128i c8 = _mm_cvtsi32_si128((int)((uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16 | (uint32_t)c[3] << 24));
    c8 = _mm_shuffle_epi32(c8, 0);
    const __m128i c16 = _mm_unpacklo_epi8(c8, zero);   /* the color for two pixels, 16 bit */
    uint32_t y0c = wc >> 1;
    const __m128i pc = _mm_set1_epi32((int)(y0c | (wc - y0c) << 16));
    for (; k + 4 <= n; k += 4) {
        __m128i pr;
        if (w) {
            uint64_t w4;
            memcpy(&w4, w + k, 8);
            if (!w4) continue;   /* four weights of 0: the pixels stay */
            __m128i wv = _mm_loadl_epi64((const __m128i *)(const void *)(w + k));
            __m128i y0 = _mm_srli_epi16(wv, 1);
            pr = _mm_unpacklo_epi16(y0, _mm_sub_epi16(wv, y0));   /* (a >> 1, a - (a >> 1)) per pixel: both fit in int16 */
        } else pr = pc;
        uint8_t *q = p + (size_t)k * 4;
        __m128i px = _mm_loadu_si128((const __m128i *)(void *)q);
        __m128i lo = _mm_unpacklo_epi8(px, zero), hi = _mm_unpackhi_epi8(px, zero);
        __m128i dlo = _mm_sub_epi16(c16, lo), dhi = _mm_sub_epi16(c16, hi);
        __m128i m0 = _mm_madd_epi16(_mm_unpacklo_epi16(dlo, dlo), _mm_shuffle_epi32(pr, 0x00));
        __m128i m1 = _mm_madd_epi16(_mm_unpackhi_epi16(dlo, dlo), _mm_shuffle_epi32(pr, 0x55));
        __m128i m2 = _mm_madd_epi16(_mm_unpacklo_epi16(dhi, dhi), _mm_shuffle_epi32(pr, 0xAA));
        __m128i m3 = _mm_madd_epi16(_mm_unpackhi_epi16(dhi, dhi), _mm_shuffle_epi32(pr, 0xFF));
        m0 = _mm_srai_epi32(_mm_add_epi32(m0, half), 16); m1 = _mm_srai_epi32(_mm_add_epi32(m1, half), 16);
        m2 = _mm_srai_epi32(_mm_add_epi32(m2, half), 16); m3 = _mm_srai_epi32(_mm_add_epi32(m3, half), 16);
        __m128i rlo = _mm_add_epi16(_mm_packs_epi32(m0, m1), lo), rhi = _mm_add_epi16(_mm_packs_epi32(m2, m3), hi);
        _mm_storeu_si128((__m128i *)(void *)q, _mm_packus_epi16(rlo, rhi));
    }
#endif
    for (; k < n; k++) {
        uint32_t a = w ? w[k] : wc, ia = 65536u - a;
        uint8_t *q = p + (size_t)k * 4;
        q[0] = (uint8_t)((q[0] * ia + c[0] * a + 32768u) >> 16);
        q[1] = (uint8_t)((q[1] * ia + c[1] * a + 32768u) >> 16);
        q[2] = (uint8_t)((q[2] * ia + c[2] * a + 32768u) >> 16);
        q[3] = (uint8_t)((q[3] * ia + c[3] * a + 32768u) >> 16);
    }
}

/* n pixels of one value, at any alignment */
static inline void cui__sw_fill(uint8_t *p, int n, const uint8_t c[4]) {
    uint32_t v;
    memcpy(&v, c, 4);
    int k = 0;
#if defined(CUI__SW_SSE2)
    __m128i v4 = _mm_set1_epi32((int)v);
    for (; k + 4 <= n; k += 4) _mm_storeu_si128((__m128i *)(void *)(p + (size_t)k * 4), v4);
#endif
    for (; k < n; k++) memcpy(p + (size_t)k * 4, &v, 4);
}

/* Copies the opaque RGBA texels t starts with (at most n) as pixels: the same bytes, red and blue
 * swapped for BGRA. Returns how many: t[that] is the first texel that is not opaque. */
static inline int cui__sw_copy_opaque(uint8_t *q, const uint8_t *t, int n, bool bgra) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i amask = _mm_set1_epi32((int)0xFF000000u), rb = _mm_set1_epi32(0x000000FF), ga = _mm_set1_epi32((int)0xFF00FF00u);
    for (; k + 4 <= n; k += 4) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(t + (size_t)k * 4));
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(v, amask), amask)) != 0xFFFF) break;
        if (bgra) v = _mm_or_si128(_mm_and_si128(v, ga), _mm_or_si128(_mm_and_si128(_mm_srli_epi32(v, 16), rb), _mm_slli_epi32(_mm_and_si128(v, rb), 16)));
        _mm_storeu_si128((__m128i *)(void *)(q + (size_t)k * 4), v);
    }
#endif
    for (; k < n && t[(size_t)k * 4 + 3] == 255; k++) {
        const uint8_t *s = t + (size_t)k * 4;
        uint8_t *d = q + (size_t)k * 4;
        d[0] = s[bgra ? 2 : 0]; d[1] = s[1]; d[2] = s[bgra ? 0 : 2]; d[3] = 255;
    }
    return k;
}

/* The weight cui__blend_shadow gives a shadow of alpha al, 0 where it draws nothing. */
static inline uint16_t cui__shadow_w16(float al) {
    if (al <= 0.0005f) return 0;
    uint32_t a = (uint32_t)(al * 65536.0f + 0.5f);
    return (uint16_t)(a > CUI__W16_MAX ? CUI__W16_MAX : a);
}

/* A shadow's color over a pixel, in 16 bit fixed point: the same blend as cui__blend within
 * one step (it rounds the same value, computed to 1/65536 of a step instead of in float). */
static inline void cui__blend_shadow(uint8_t *p, const uint32_t c8[4], float al) {
    uint32_t a = (uint32_t)(al * 65536.0f + 0.5f), ia = 65536u - a;
    p[0] = (uint8_t)((p[0] * ia + c8[0] * a + 32768u) >> 16);
    p[1] = (uint8_t)((p[1] * ia + c8[1] * a + 32768u) >> 16);
    p[2] = (uint8_t)((p[2] * ia + c8[2] * a + 32768u) >> 16);
    p[3] = (uint8_t)((p[3] * ia + c8[3] * a + 32768u) >> 16);
}

struct cui__sw_wtab { float ca; bool ok; uint16_t w[256]; };
/* a job's scratch tables for cui__raster_quad */
/* a job's scratch tables for cui__raster_quad. A shadow's column tables depend on the quad and the
 * columns only, so the jobs below one another (one thread drawing a frame) reuse them: key is the
 * quad's first vertex (this frame's draw data; cleared each frame) and kx0, kx1 the columns. */
struct cui__sw_scratch {
    float f[2][CUI__SH_COLS], q[2][CUI__SH_COLS];
    uint16_t a[2][CUI__SH_COLS], w[CUI__SH_COLS];
    uint8_t side[CUI__SH_COLS];
    const cui_vertex *key; int kx0, kx1;
    bool a_ok[2]; int mid, cl[2], cr[2];   /* the weights made for each half; the first right side column; per half the end of the left corner columns, the start of the right ones */
    struct cui__sw_wtab wtab;
};

typedef struct {
    const cui_draw_data *dd;
    cui_sw_renderer *r;
    uint8_t *pixels;
    int width, height, stride;
    cui_sw_format fmt;
    int x0, y0, x1, y1;   /* the part this job paints */
    struct cui__sw_wtab *wtab;   /* text: the weight of each coverage for the last alpha (most glyphs share one) */
    struct cui__sw_scratch *sc;
} cui__sw_job;

static inline uint8_t cui__blend_ch(float s, uint8_t d, float ia) {
    return (uint8_t)(cui__sat(s + (d / 255.0f) * ia) * 255.0f + 0.5f);
}

#if defined(_MSC_VER)
#define CUI__HOT_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define CUI__HOT_INLINE inline __attribute__((always_inline))
#else
#define CUI__HOT_INLINE inline
#endif

/* inlined: it runs once per drawn pixel that is not a plain fill */
static CUI__HOT_INLINE void cui__blend(uint8_t *p, cui__px s, bool bgra) {
    if (s.a <= 0.0005f) return;
#if defined(CUI__SW_SSE2)
    /* the four channels at once: the same float operations in the same order, so the same bytes */
    {
        const __m128 zero = _mm_setzero_ps(), k255 = _mm_set1_ps(255.0f);
        __m128 v = bgra ? _mm_setr_ps(s.b, s.g, s.r, s.a) : _mm_setr_ps(s.r, s.g, s.b, s.a);
        bool opaque = s.a >= 0.9995f;
        if (!opaque) {
            int32_t px;
            memcpy(&px, p, 4);
            __m128i zi = _mm_setzero_si128();
            __m128 d = _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(px), zi), zi));
            v = _mm_add_ps(v, _mm_mul_ps(_mm_div_ps(d, k255), _mm_set1_ps(1.0f - s.a)));
        }
        v = _mm_min_ps(_mm_max_ps(v, zero), _mm_set1_ps(1.0f));
        __m128i r = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(v, k255), _mm_set1_ps(0.5f)));
        r = _mm_packs_epi32(r, r);
        uint32_t out = (uint32_t)_mm_cvtsi128_si32(_mm_packus_epi16(r, r));
        if (opaque) out |= 0xFF000000u;
        memcpy(p, &out, 4);
        return;
    }
#endif
    if (s.a >= 0.9995f) {
        int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
        p[ri] = (uint8_t)(cui__sat(s.r) * 255.0f + 0.5f); p[1] = (uint8_t)(cui__sat(s.g) * 255.0f + 0.5f);
        p[bi] = (uint8_t)(cui__sat(s.b) * 255.0f + 0.5f); p[3] = 255;
        return;
    }
    float ia = 1.0f - s.a;
    int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    p[ri] = cui__blend_ch(s.r, p[ri], ia);
    p[1] = cui__blend_ch(s.g, p[1], ia);
    p[bi] = cui__blend_ch(s.b, p[bi], ia);
    p[3] = cui__blend_ch(s.a, p[3], ia);
}


static void cui__raster_tri(const cui__sw_job *job, const cui_vertex *a, const cui_vertex *b, const cui_vertex *c,
                            const cui__sw_tex *tex, int cx0, int cy0, int cx1, int cy1) {
    float area = (b->x - a->x) * (c->y - a->y) - (b->y - a->y) * (c->x - a->x);
    if (fabsf(area) < 1e-8f) return;
    float minx = CUI_MIN(a->x, CUI_MIN(b->x, c->x)), maxx = CUI_MAX(a->x, CUI_MAX(b->x, c->x));
    float miny = CUI_MIN(a->y, CUI_MIN(b->y, c->y)), maxy = CUI_MAX(a->y, CUI_MAX(b->y, c->y));
    int x0 = CUI_MAX(cx0, (int)floorf(minx)), x1 = CUI_MIN(cx1, (int)ceilf(maxx));
    int y0 = CUI_MAX(cy0, (int)floorf(miny)), y1 = CUI_MIN(cy1, (int)ceilf(maxy));
    if (x0 >= x1 || y0 >= y1) return;
    float inv = 1.0f / area;
    /* top-left fill rule: a pixel on an edge shared by two triangles is
     * drawn by exactly one of them, so translucent meshes have no seams */
    float sgn = area > 0 ? 1.0f : -1.0f;
    const cui_vertex *ev[3][2] = { { b, c }, { c, a }, { a, b } };
    bool tl[3];
    for (int e = 0; e < 3; e++) {
        float dx = (ev[e][1]->x - ev[e][0]->x) * sgn, dy = (ev[e][1]->y - ev[e][0]->y) * sgn;
        tl[e] = dy < 0 || (dy == 0 && dx > 0);
    }
    int mode = (int)(a->mode + 0.5f);
    bool bgra = job->fmt == CUI_SW_BGRA;
    /* flat attributes come from the first vertex */
    float hw = a->hw, hh = a->hh, rtl = a->r_tl, rtr = a->r_tr, rbr = a->r_br, rbl = a->r_bl, bw = a->border;
    float bcr = a->border_color.r / 255.0f, bcg = a->border_color.g / 255.0f, bcb = a->border_color.b / 255.0f, bca = a->border_color.a / 255.0f;
    bool flat_color = memcmp(&a->color, &b->color, 4) == 0 && memcmp(&a->color, &c->color, 4) == 0;
    const cui_vertex *tv[3] = { a, b, c };
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        uint8_t *row = job->pixels + (size_t)y * (size_t)job->stride;
        /* horizontal span of the triangle on this row */
        float sx0 = 1e30f, sx1 = -1e30f;
        for (int e = 0; e < 3; e++) {
            const cui_vertex *p0 = tv[e], *p1 = tv[(e + 1) % 3];
            float ya = p0->y, yb = p1->y;
            if ((py < ya && py < yb) || (py > ya && py > yb)) continue;
            float xi = (yb == ya) ? p0->x : p0->x + (p1->x - p0->x) * (py - ya) / (yb - ya);
            if (yb == ya) { sx0 = CUI_MIN(sx0, CUI_MIN(p0->x, p1->x)); sx1 = CUI_MAX(sx1, CUI_MAX(p0->x, p1->x)); continue; }
            sx0 = CUI_MIN(sx0, xi); sx1 = CUI_MAX(sx1, xi);
        }
        if (sx1 < sx0) continue;
        int rx0 = CUI_MAX(x0, (int)floorf(sx0) - 1), rx1 = CUI_MIN(x1, (int)ceilf(sx1) + 1);
        for (int x = rx0; x < rx1; x++) {
            float px = (float)x + 0.5f;
            float e0 = (b->x - px) * (c->y - py) - (b->y - py) * (c->x - px);
            float e1 = (c->x - px) * (a->y - py) - (c->y - py) * (a->x - px);
            float e2 = (a->x - px) * (b->y - py) - (a->y - py) * (b->x - px);
            float w0 = e0 * inv, w1 = e1 * inv, w2 = e2 * inv;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !tl[0]) || (w1 == 0 && !tl[1]) || (w2 == 0 && !tl[2])) continue;
            float cr, cg, cb, ca;
            if (flat_color) {
                cr = a->color.r / 255.0f; cg = a->color.g / 255.0f; cb = a->color.b / 255.0f; ca = a->color.a / 255.0f;
            } else {
                cr = (a->color.r * w0 + b->color.r * w1 + c->color.r * w2) / 255.0f;
                cg = (a->color.g * w0 + b->color.g * w1 + c->color.g * w2) / 255.0f;
                cb = (a->color.b * w0 + b->color.b * w1 + c->color.b * w2) / 255.0f;
                ca = (a->color.a * w0 + b->color.a * w1 + c->color.a * w2) / 255.0f;
            }
            cui__px o;
            if (mode == CUI_SHADE_SOLID) {
                o.r = cr * ca; o.g = cg * ca; o.b = cb * ca; o.a = ca;
            } else if (mode == CUI_SHADE_TEXT) {
                float u = a->u * w0 + b->u * w1 + c->u * w2, v = a->v * w0 + b->v * w1 + c->v * w2;
                float cov = tex && tex->fmt == CUI_TEXTURE_R8 ? cui__sample_r8(tex, u, v) : 1.0f;
                float al = ca * cov;
                o.r = cr * al; o.g = cg * al; o.b = cb * al; o.a = al;
            } else {
                float lx = a->lx * w0 + b->lx * w1 + c->lx * w2, ly = a->ly * w0 + b->ly * w1 + c->ly * w2;
                float d = cui__sd_rrect(lx, ly, hw, hh, rtl, rtr, rbr, rbl);
                if (mode == CUI_SHADE_RRECT) {
                    float outer = cui__sat(0.5f - d);
                    if (outer <= 0) continue;
                    float inner = bw > 0 ? cui__sat(0.5f - (d + bw)) : outer;
                    float fa = ca * inner, ba = bca * (outer - inner);
                    o.r = cr * fa + bcr * ba; o.g = cg * fa + bcg * ba; o.b = cb * fa + bcb * ba; o.a = fa + ba;
                } else if (mode == CUI_SHADE_SHADOW) {
                    float sigma = bw > 0.01f ? bw : 0.01f;
                    float z = d / (sigma * 1.41421356f);
                    if (z > CUI__SHADOW_Z1) continue;
                    float al = ca * cui__shadow_falloff(z);
                    o.r = cr * al; o.g = cg * al; o.b = cb * al; o.a = al;
                } else if (mode == CUI_SHADE_DITHER) {
                    /* color alpha is the density, border the floor, border color the dot */
                    float cov = cui__sat(0.5f - d);
                    if (cov <= 0) continue;
                    float t = cui__sw_bayer(a->u * w0 + b->u * w1 + c->u * w2, a->v * w0 + b->v * w1 + c->v * w2);
                    if (t < bw || t >= ca) continue;
                    float k = bca * cov;
                    o.r = bcr * k; o.g = bcg * k; o.b = bcb * k; o.a = k;
                } else { /* image */
                    float cov = cui__sat(0.5f - d);
                    if (cov <= 0) continue;
                    float u = a->u * w0 + b->u * w1 + c->u * w2, v = a->v * w0 + b->v * w1 + c->v * w2;
                    cui__px t = tex && tex->fmt == CUI_TEXTURE_RGBA8 ? cui__sample_rgba(tex, u, v) : (cui__px){ 1, 1, 1, 1 };
                    float k = ca * cov;
                    o.r = t.r * cr * k; o.g = t.g * cg * k; o.b = t.b * cb * k; o.a = t.a * k;
                }
            }
            cui__blend(row + (size_t)x * 4, o, bgra);
        }
    }
}


/* The quads cui__raster_quad takes: axis aligned, one color across, the same color down each
 * side (a vertical gradient at most), and not dither (the triangle path has the cell lookup). */
static bool cui__sw_quad_ok(const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v2, const cui_vertex *v3) {
    if (v0->y != v1->y || v1->x != v2->x || v2->y != v3->y || v3->x != v0->x) return false;
    if (v0->x >= v1->x || v0->y >= v3->y) return false;
    if (memcmp(&v0->color, &v1->color, 4) != 0 || memcmp(&v2->color, &v3->color, 4) != 0) return false;
    return (int)(v0->mode + 0.5f) != CUI_SHADE_DITHER;
}

static void cui__sw_deep_span(float l0, float q0, float L, int lo, int hi, int *a, int *b);

/* Fast path for axis aligned quads (almost everything CUI draws): no
 * barycentrics, linear local coordinates and early outs for the interior of
 * rounded rectangles. Returns false when the quad does not qualify. */
static bool cui__raster_quad(const cui__sw_job *job, const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v2,
                             const cui_vertex *v3, const cui__sw_tex *tex, int cx0, int cy0, int cx1, int cy1) {
    if (!cui__sw_quad_ok(v0, v1, v2, v3)) return false;
    int mode = (int)(v0->mode + 0.5f);
    float qx0 = v0->x, qy0 = v0->y, qx1 = v1->x, qy1 = v3->y;
    int x0 = CUI_MAX(cx0, (int)ceilf(qx0 - 0.5f)), x1 = CUI_MIN(cx1, (int)ceilf(qx1 - 0.5f));
    int y0 = CUI_MAX(cy0, (int)ceilf(qy0 - 0.5f)), y1 = CUI_MIN(cy1, (int)ceilf(qy1 - 0.5f));
    if (x0 >= x1 || y0 >= y1) return true;
    bool bgra = job->fmt == CUI_SW_BGRA;
    int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    float qh = qy1 - qy0, qw = qx1 - qx0;
    bool gradient = memcmp(&v0->color, &v3->color, 4) != 0;
    float hw = v0->hw, hh = v0->hh, bw = v0->border;
    float rmax = CUI_MAX(CUI_MAX(v0->r_tl, v0->r_tr), CUI_MAX(v0->r_br, v0->r_bl));
    rmax = CUI_MIN(rmax, CUI_MIN(hw, hh));
    float bcr = v0->border_color.r / 255.0f, bcg = v0->border_color.g / 255.0f, bcb = v0->border_color.b / 255.0f, bca = v0->border_color.a / 255.0f;
    float du = (v1->u - v0->u) / qw, dv = (v3->v - v0->v) / qh;
    bool text_exact = mode == CUI_SHADE_TEXT && tex && fabsf(du * (float)tex->w - 1.0f) < 0.001f && fabsf(dv * (float)tex->h - 1.0f) < 0.001f;
    /* A shadow: outside the corners the distance to the shape is the larger of a column's and a
     * row's (cui__sd_rrect: d = max(qx, qy) - r inside, qx - r or qy - r beside a side), and the
     * falloff only drops with distance, so a pixel's falloff is the smaller of its column's and
     * its row's. The columns' are worked out once here (above and below the center: the corner
     * radii may differ), the rows' once per row; only the corners take a square root per pixel.
     * The same arithmetic as the formula, so the same pixels. */
    /* the tables live in the job (they are too big for every call's stack frame) */
    float (*sh_f)[CUI__SH_COLS] = job->sc->f, (*sh_q)[CUI__SH_COLS] = job->sc->q, sh_rad[2][2] = { { 0, 0 }, { 0, 0 } }, sh_k = 1;
    uint8_t *sh_side = job->sc->side;
    uint16_t (*sh_a)[CUI__SH_COLS] = job->sc->a, *sh_w = job->sc->w;   /* one color: the columns' 16 bit weights (cui__shadow_w16), each half made when first needed; a row's */
    struct cui__sw_scratch *sc = job->sc;
    bool *sh_a_ok = sc->a_ok;
    bool sh_cols = mode == CUI_SHADE_SHADOW && x1 - x0 <= CUI__SH_COLS;
    if (sh_cols) {
        float m = hw < hh ? hw : hh;
        const float rr[2][2] = { { v0->r_tl, v0->r_tr }, { v0->r_bl, v0->r_br } };
        for (int half = 0; half < 2; half++)
            for (int sd = 0; sd < 2; sd++) sh_rad[half][sd] = rr[half][sd] > m ? m : rr[half][sd];
        sh_k = (bw > 0.01f ? bw : 0.01f) * 1.41421356f;
        bool cached = sc->key == v0 && sc->kx0 == x0 && sc->kx1 == x1;
        if (!cached) { sc->key = v0; sc->kx0 = x0; sc->kx1 = x1; sc->a_ok[0] = sc->a_ok[1] = false; }
        for (int x = cached ? x1 : x0; x < x1; x++) {
            float lx = v0->lx + (((float)x + 0.5f) - qx0);
            int i = x - x0, sd = lx < 0 ? 0 : 1;
            sh_side[i] = (uint8_t)sd;
            for (int half = 0; half < 2; half++) {
                sh_q[half][i] = fabsf(lx) - hw + sh_rad[half][sd];
                sh_f[half][i] = cui__shadow_side(sh_q[half][i], sh_rad[half][sd], sh_k);
            }
        }
    }
    /* A glyph at its size in one color (almost all text): its texel columns, the weight of each
     * coverage and the color are the same on every row, so they are worked out once and each row
     * is its texel row's weights blended. The same values as the row by row path below. */
    if (mode == CUI_SHADE_TEXT && text_exact && !gradient && tex->fmt == CUI_TEXTURE_R8 && x1 - x0 <= CUI__SH_COLS) {
        int n = x1 - x0;
        int ta = (int)((v0->u + (((float)x0 + 0.5f) - qx0) * du) * (float)tex->w);
        int tb = (int)((v0->u + (((float)(x1 - 1) + 0.5f) - qx0) * du) * (float)tex->w);
        if (tb - ta == n - 1 && ta >= 0 && tb < tex->w) {   /* texel after texel, as the pixels */
            float ca = v0->color.a / 255.0f;
            uint8_t c4[4];
            c4[ri] = (uint8_t)(v0->color.r / 255.0f * 255.0f + 0.5f); c4[1] = (uint8_t)(v0->color.g / 255.0f * 255.0f + 0.5f);
            c4[bi] = (uint8_t)(v0->color.b / 255.0f * 255.0f + 0.5f); c4[3] = 255;
            struct cui__sw_wtab *tab = job->wtab;
            if (!tab->ok || tab->ca != ca) {   /* cui__shadow_w16 of ca times the coverage, 0 for none */
                tab->w[0] = 0;
                for (int t = 1; t < 256; t++) tab->w[t] = cui__shadow_w16(ca * (t / 255.0f));
                tab->ca = ca; tab->ok = true;
            }
            const uint16_t *wt = tab->w;
            for (int y = y0; y < y1; y++) {
                float vv = v0->v + (((float)y + 0.5f) - qy0) * dv;
                int tyi = (int)(vv * (float)tex->h);
                if (tyi < 0 || tyi >= tex->h) continue;
                const uint8_t *trow = tex->data + (size_t)tyi * (size_t)tex->w + ta;
                for (int i = 0; i < n; i++) sh_w[i] = wt[trow[i]];
                cui__sw_blend_w16(job->pixels + (size_t)y * (size_t)job->stride + (size_t)x0 * 4, n, sh_w, 0, c4);
            }
            return true;
        }
    }
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        float ty = (py - qy0) / qh;
        float cr, cg, cb, ca;
        if (gradient) {
            cr = (v0->color.r + (v3->color.r - v0->color.r) * ty) / 255.0f;
            cg = (v0->color.g + (v3->color.g - v0->color.g) * ty) / 255.0f;
            cb = (v0->color.b + (v3->color.b - v0->color.b) * ty) / 255.0f;
            ca = (v0->color.a + (v3->color.a - v0->color.a) * ty) / 255.0f;
        } else {
            cr = v0->color.r / 255.0f; cg = v0->color.g / 255.0f; cb = v0->color.b / 255.0f; ca = v0->color.a / 255.0f;
        }
        bool plain_tint = cr == 1.0f && cg == 1.0f && cb == 1.0f && ca == 1.0f;
        uint8_t opaque[4];
        opaque[ri] = (uint8_t)(cr * 255.0f + 0.5f); opaque[1] = (uint8_t)(cg * 255.0f + 0.5f);
        opaque[bi] = (uint8_t)(cb * 255.0f + 0.5f); opaque[3] = 255;
        float ly = v0->ly + (py - qy0);
        float ay = fabsf(ly);
        uint8_t *row = job->pixels + (size_t)y * (size_t)job->stride;
        if (mode == CUI_SHADE_TEXT && x1 - x0 <= CUI__SH_COLS) {
            /* the row's weights (cui__shadow_w16 of ca times the coverage, 0 where the coverage
             * is 0), then one color blended over the row with them */
            float vv = v0->v + (py - qy0) * dv;
            int tyi = (int)(vv * (float)(tex ? tex->h : 1)), n = x1 - x0;
            if (!tex || tex->fmt != CUI_TEXTURE_R8) {
                cui__sw_blend_w16(row + (size_t)x0 * 4, n, NULL, cui__shadow_w16(ca), opaque);
                continue;
            }
            if (text_exact) {
                if (tyi < 0 || tyi >= tex->h) continue;
                struct cui__sw_wtab *tab = job->wtab;
                if (!tab->ok || tab->ca != ca) {   /* cui__shadow_w16 of ca times the coverage, 0 for none */
                    tab->w[0] = 0;
                    for (int t = 1; t < 256; t++) tab->w[t] = cui__shadow_w16(ca * (t / 255.0f));
                    tab->ca = ca; tab->ok = true;
                }
                const uint16_t *wt = tab->w;
                const uint8_t *trow = tex->data + (size_t)tyi * (size_t)tex->w;
                int ta = (int)((v0->u + (((float)x0 + 0.5f) - qx0) * du) * (float)tex->w);
                int tb = (int)((v0->u + (((float)(x1 - 1) + 0.5f) - qx0) * du) * (float)tex->w);
                if (tb - ta == n - 1 && ta >= 0 && tb < tex->w) {   /* texel after texel, as the pixels */
                    for (int i = 0; i < n; i++) sh_w[i] = wt[trow[ta + i]];
                } else {
                    for (int i = 0; i < n; i++) {
                        int txi = (int)((v0->u + (((float)(x0 + i) + 0.5f) - qx0) * du) * (float)tex->w);
                        uint8_t t = txi >= 0 && txi < tex->w ? trow[txi] : 0;
                        sh_w[i] = wt[t];
                    }
                }
            } else {
                /* bilinear (text scaled by an animation): cui__sample_r8 with the row's part worked out once */
                float sy = vv * (float)tex->h - 0.5f;
                int ya = (int)floorf(sy), yb = ya + 1;
                float fy = sy - (float)ya;
                ya = CUI_CLAMP(ya, 0, tex->h - 1); yb = CUI_CLAMP(yb, 0, tex->h - 1);
                const uint8_t *ra = tex->data + ya * tex->w, *rb = tex->data + yb * tex->w;
                for (int i = 0; i < n; i++) {
                    float sx = (v0->u + (((float)(x0 + i) + 0.5f) - qx0) * du) * (float)tex->w - 0.5f;
                    int xa = (int)floorf(sx), xb = xa + 1;
                    float fx = sx - (float)xa;
                    xa = CUI_CLAMP(xa, 0, tex->w - 1); xb = CUI_CLAMP(xb, 0, tex->w - 1);
                    float a = ra[xa], b = ra[xb], c = rb[xa], e = rb[xb];
                    float cov = ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + e * fx) * fy) / 255.0f;
                    sh_w[i] = cui__shadow_w16(ca * cov);
                }
            }
            cui__sw_blend_w16(row + (size_t)x0 * 4, n, sh_w, 0, opaque);
            continue;
        }
        if (mode == CUI_SHADE_TEXT) {
            float vv = v0->v + (py - qy0) * dv;
            int tyi = (int)(vv * (float)(tex ? tex->h : 1));
            for (int x = x0; x < x1; x++) {
                float px = (float)x + 0.5f;
                float cov;
                if (!tex || tex->fmt != CUI_TEXTURE_R8) cov = 1.0f;
                else if (text_exact) {
                    int txi = (int)((v0->u + (px - qx0) * du) * (float)tex->w);
                    if (txi < 0 || txi >= tex->w || tyi < 0 || tyi >= tex->h) continue;
                    uint8_t t = tex->data[(size_t)tyi * (size_t)tex->w + (size_t)txi];
                    if (!t) continue;
                    cov = t / 255.0f;
                } else cov = cui__sample_r8(tex, v0->u + (px - qx0) * du, vv);
                float al = ca * cov;
                cui__px o = { cr * al, cg * al, cb * al, al };
                cui__blend(row + (size_t)x * 4, o, bgra);
            }
            continue;
        }
        /* The inside of a rounded rectangle (or all of a solid) is one color: find it with the
         * same test the loop below uses (it is one interval, found by walking in from both ends)
         * and do it in one go: fill it when opaque, skip it when clear (a border-only box),
         * blend one color over it otherwise. The loop does the edges. */
        int sx0 = x1, sx1 = x1;
        if (mode == CUI_SHADE_SOLID || mode == CUI_SHADE_RRECT || mode == CUI_SHADE_SHADOW) {
            if (mode == CUI_SHADE_SOLID) { sx0 = x0; sx1 = x1; }
            else {
                /* a rounded rectangle: inside its border; a shadow: four blur widths inside the
                 * shape, where erf is exactly -1 in float and the shadow is the plain color */
                float inset = mode == CUI_SHADE_SHADOW ? 4.0f * 1.41421356f * (bw > 0.01f ? bw : 0.01f) : bw + 0.5f;
                if (ay <= hh - inset) {
                    /* (|lx| <= hw - inset - rmax && ay <= hh - inset) || (|lx| <= hw - inset && ay <= hh - inset - rmax) */
                    int a, b;
                    cui__sw_deep_span(v0->lx, qx0, ay <= hh - inset - rmax ? hw - inset : hw - inset - rmax, x0, x1, &a, &b);
                    if (a < b) { sx0 = a; sx1 = b; }
                }
            }
            if (sx0 < sx1) {
                uint8_t *p = row + (size_t)sx0 * 4;
                int n = sx1 - sx0;
                if (ca >= 0.999f && mode != CUI_SHADE_SHADOW) cui__sw_fill(p, n, opaque);
                else cui__sw_blend_w16(p, n, NULL, cui__shadow_w16(ca), opaque);   /* weight 0 below 0.0005: unchanged */
            }
        }
        /* An image drawn at its own size under a plain tint: inside its corners, each pixel of the
         * row is the next texel (its two ends land on texel centers, and u is linear in x), so
         * the run is copied, blending only texels that are not opaque. */
        if (mode == CUI_SHADE_IMAGE && plain_tint && tex && tex->fmt == CUI_TEXTURE_RGBA8 && ay <= hh - 0.5f) {
            int a, b;
            cui__sw_deep_span(v0->lx, qx0, ay <= hh - 0.5f - rmax ? hw - 0.5f : hw - 0.5f - rmax, x0, x1, &a, &b);
            if (a < b) {
                float vv = v0->v + (py - qy0) * dv;
                const uint8_t *ta = cui__texel_at(tex, v0->u + (((float)a + 0.5f) - qx0) * du, vv);
                const uint8_t *tb = cui__texel_at(tex, v0->u + (((float)(b - 1) + 0.5f) - qx0) * du, vv);
                if (ta && tb && tb - ta == (ptrdiff_t)(b - 1 - a) * 4) {
                    uint8_t *q = row + (size_t)a * 4;
                    for (int k = 0, n = b - a; k < n;) {
                        int e = k + cui__sw_copy_opaque(q + (size_t)k * 4, ta + (size_t)k * 4, n - k, bgra);   /* a run of opaque texels: copied */
                        if (e == n) break;
                        const uint8_t *t = ta + (size_t)e * 4;
                        float al = t[3] / 255.0f;
                        cui__blend(q + (size_t)e * 4, (cui__px){ t[0] / 255.0f * al, t[1] / 255.0f * al, t[2] / 255.0f * al, al }, bgra);
                        k = e + 1;
                    }
                    sx0 = a; sx1 = b;
                }
            }
        }
        if (mode == CUI_SHADE_SHADOW && sh_cols) {
            /* the row's falloff and qy on each side, the columns' from the tables */
            int half = ly < 0 ? 0 : 1;
            float frow[2], qyr[2];
            for (int sd = 0; sd < 2; sd++) {
                qyr[sd] = fabsf(ly) - hh + sh_rad[half][sd];
                frow[sd] = cui__shadow_side(qyr[sd], sh_rad[half][sd], sh_k);
            }
            const float *fc = sh_f[half], *qc = sh_q[half];
            uint32_t c8[4];
            c8[ri] = (uint32_t)(cr * 255.0f + 0.5f); c8[1] = (uint32_t)(cg * 255.0f + 0.5f);
            c8[bi] = (uint32_t)(cb * 255.0f + 0.5f); c8[3] = 255;
            if (!gradient) {
                /* One color: a pixel's 16 bit weight is the smaller of its column's and its row's
                 * (ca * f and the rounding only grow with f, so the smaller weight is the weight of
                 * the smaller falloff); 0 marks one cui__blend_shadow would skip (al <= 0.0005). */
                if (!sh_a_ok[half]) {
                    int n = x1 - x0, mid = 0;
                    for (int i = 0; i < n; i++) sh_a[half][i] = cui__shadow_w16(ca * fc[i]);
                    while (mid < n && sh_side[mid] == 0) mid++;
                    /* the corner columns (qx > 0): the left side's first ones, the right side's last ones */
                    int lend = 0, rstart = mid;
                    while (lend < mid && qc[lend] > 0) lend++;
                    while (rstart < n && !(qc[rstart] > 0)) rstart++;
                    sc->mid = mid; sc->cl[half] = lend; sc->cr[half] = rstart;
                    sh_a_ok[half] = true;
                }
                const uint16_t *ac = sh_a[half];
                uint16_t ar[2] = { cui__shadow_w16(ca * frow[0]), cui__shadow_w16(ca * frow[1]) };
                const uint8_t cb8[4] = { (uint8_t)c8[0], (uint8_t)c8[1], (uint8_t)c8[2], (uint8_t)c8[3] };
                for (int seg = 0; seg < 2; seg++) {
                    int xa = seg ? (sx0 < sx1 ? sx1 : x1) : x0, xb = seg ? x1 : (sx0 < sx1 ? sx0 : x1);
                    if (xa >= xb) continue;
                    for (int sd = 0; sd < 2; sd++) {
                        int lo = sd ? CUI_MAX(xa - x0, sc->mid) : xa - x0, hi = sd ? xb - x0 : CUI_MIN(xb - x0, sc->mid);
                        uint16_t arv = ar[sd];
                        for (int i = lo; i < hi; i++) sh_w[i] = ac[i] < arv ? ac[i] : arv;
                        if (!(qyr[sd] > 0)) continue;
                        /* a corner: the one place the distance needs both */
                        int c0 = sd ? CUI_MAX(lo, sc->cr[half]) : lo, c1 = sd ? hi : CUI_MIN(hi, sc->cl[half]);
                        for (int i = c0; i < c1; i++) {
                            float qx = qc[i];
                            float d = sqrtf(qx * qx + qyr[sd] * qyr[sd]) - sh_rad[half][sd];
                            float z = d / sh_k;
                            sh_w[i] = z > CUI__SHADOW_Z1 ? 0 : cui__shadow_w16(ca * cui__shadow_falloff(z));
                        }
                    }
                    /* a weight of 0 leaves a pixel as it was, so the run is blended whole */
                    cui__sw_blend_w16(row + (size_t)xa * 4, xb - xa, sh_w + (xa - x0), 0, cb8);
                }
                continue;
            }
            for (int x = x0; x < x1; x++) {
                if (x == sx0) { x = sx1 - 1; continue; }   /* filled above */
                int i = x - x0, sd = sh_side[i];
                float qx = qc[i], f;
                if (qx > 0 && qyr[sd] > 0) {   /* a corner: the one place the distance needs both */
                    float d = sqrtf(qx * qx + qyr[sd] * qyr[sd]) - sh_rad[half][sd];
                    float z = d / sh_k;
                    if (z > CUI__SHADOW_Z1) continue;
                    f = cui__shadow_falloff(z);
                } else f = fc[i] < frow[sd] ? fc[i] : frow[sd];
                float al = ca * f;
                if (al <= 0.0005f) continue;
                cui__blend_shadow(row + (size_t)x * 4, c8, al);
            }
            continue;
        }
        if (mode == CUI_SHADE_RRECT && bw <= 0 && x1 - x0 <= CUI__SH_COLS) {
            /* No border: one color, so each pixel is a weight (cui__shadow_w16 of ca times its
             * coverage, the color itself where the loop below copies it) and the row is blended. */
            float inset = 0.5f;
            uint16_t wdeep = ca >= 0.999f ? (uint16_t)CUI__W16_MAX : cui__shadow_w16(ca);
            for (int x = x0; x < x1; x++) {
                int i = x - x0;
                if (x == sx0) { for (; x < sx1; x++) sh_w[x - x0] = 0; x--; continue; }   /* filled above */
                float lx = v0->lx + (((float)x + 0.5f) - qx0), ax = fabsf(lx);
                if ((ax <= hw - inset - rmax && ay <= hh - inset) || (ax <= hw - inset && ay <= hh - inset - rmax)) { sh_w[i] = wdeep; continue; }
                float d = cui__sd_rrect(lx, ly, hw, hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl);
                float outer = cui__sat(0.5f - d);
                sh_w[i] = outer <= 0 ? 0 : cui__shadow_w16(ca * outer);
            }
            if (sx0 < sx1) {
                cui__sw_blend_w16(row + (size_t)x0 * 4, sx0 - x0, sh_w, 0, opaque);
                cui__sw_blend_w16(row + (size_t)sx1 * 4, x1 - sx1, sh_w + (sx1 - x0), 0, opaque);
            } else cui__sw_blend_w16(row + (size_t)x0 * 4, x1 - x0, sh_w, 0, opaque);
            continue;
        }
        /* (a shadow too wide for the tables: one pixel at a time) */
        for (int x = x0; x < x1; x++) {
            if (x == sx0) { x = sx1 - 1; continue; }   /* filled above */
            float px = (float)x + 0.5f;
            float lx = v0->lx + (px - qx0);
            cui__px o;
            if (mode == CUI_SHADE_SOLID) {
                if (ca >= 0.999f) { memcpy(row + (size_t)x * 4, opaque, 4); continue; }
                o.r = cr * ca; o.g = cg * ca; o.b = cb * ca; o.a = ca;
            } else if (mode == CUI_SHADE_RRECT) {
                float ax = fabsf(lx);
                float inset = bw + 0.5f;
                bool deep = (ax <= hw - inset - rmax && ay <= hh - inset) || (ax <= hw - inset && ay <= hh - inset - rmax);
                if (deep) {
                    if (ca >= 0.999f) { memcpy(row + (size_t)x * 4, opaque, 4); continue; }
                    if (ca <= 0.0005f) continue;
                    o.r = cr * ca; o.g = cg * ca; o.b = cb * ca; o.a = ca;
                } else {
                    float d = cui__sd_rrect(lx, ly, hw, hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl);
                    float outer = cui__sat(0.5f - d);
                    if (outer <= 0) continue;
                    float inner = bw > 0 ? cui__sat(0.5f - (d + bw)) : outer;
                    float fa = ca * inner, ba = bca * (outer - inner);
                    o.r = cr * fa + bcr * ba; o.g = cg * fa + bcg * ba; o.b = cb * fa + bcb * ba; o.a = fa + ba;
                }
            } else if (mode == CUI_SHADE_SHADOW) {
                float d = cui__sd_rrect(lx, ly, hw, hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl);
                float sigma = bw > 0.01f ? bw : 0.01f;
                float z = d / (sigma * 1.41421356f);
                if (z > CUI__SHADOW_Z1) continue;
                float al = ca * cui__shadow_falloff(z);
                o.r = cr * al; o.g = cg * al; o.b = cb * al; o.a = al;
            } else {
                /* an image: inside its corners the coverage is 1 without the distance (the test
                 * a rounded rectangle uses, with no border); a sample on a texel's center reads it
                 * alone, and an opaque one under a plain tint is that texel's bytes */
                float ax = fabsf(lx), cov;
                if ((ax <= hw - 0.5f - rmax && ay <= hh - 0.5f) || (ax <= hw - 0.5f && ay <= hh - 0.5f - rmax)) cov = 1.0f;
                else {
                    float d = cui__sd_rrect(lx, ly, hw, hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl);
                    cov = cui__sat(0.5f - d);
                    if (cov <= 0) continue;
                }
                float u = v0->u + (px - qx0) * du, vv = v0->v + (py - qy0) * dv;
                cui__px t = { 1, 1, 1, 1 };
                if (tex && tex->fmt == CUI_TEXTURE_RGBA8) {
                    const uint8_t *tp = cui__texel_at(tex, u, vv);
                    if (tp && tp[3] == 255 && cov >= 1.0f && plain_tint) {
                        uint8_t *q = row + (size_t)x * 4;
                        q[ri] = tp[0]; q[1] = tp[1]; q[bi] = tp[2]; q[3] = 255;
                        continue;
                    }
                    if (tp) {
                        float a = tp[3] / 255.0f;
                        t = (cui__px){ tp[0] / 255.0f * a, tp[1] / 255.0f * a, tp[2] / 255.0f * a, a };
                    } else t = cui__sample_rgba(tex, u, vv);
                }
                float k = ca * cov;
                o.r = t.r * cr * k; o.g = t.g * cg * k; o.b = t.b * cb * k; o.a = t.a * k;
            }
            cui__blend(row + (size_t)x * 4, o, bgra);
        }
    }
    return true;
}

/* ---- worker threads ---- */

/* Made once and kept: starting threads for every frame cost 0.1 to 0.5 ms on Windows. */
struct cui__sw_pool {
    int want, n;                             /* workers asked for and running */
    void (*fn)(void *ctx, int job);
    void *ctx;
    int njobs;
#if defined(_WIN32)
    HANDLE th[32], go, done;
    volatile LONG next, active, quit;
#else
    pthread_t th[32];
    pthread_mutex_t mu;
    pthread_cond_t go_cv, done_cv;
    unsigned gen;
    int next, active, quit;
#endif
};

#if defined(_WIN32)
static DWORD WINAPI cui__sw_worker(LPVOID arg) {
    cui__sw_pool *p = arg;
    for (;;) {
        WaitForSingleObject(p->go, INFINITE);
        if (p->quit) return 0;
        for (LONG j; (j = InterlockedIncrement(&p->next) - 1) < p->njobs;) p->fn(p->ctx, (int)j);
        if (InterlockedDecrement(&p->active) == 0) SetEvent(p->done);
    }
}
#else
static void *cui__sw_worker(void *arg) {
    cui__sw_pool *p = arg;
    unsigned seen = 0;
    pthread_mutex_lock(&p->mu);
    for (;;) {
        while (p->gen == seen && !p->quit) pthread_cond_wait(&p->go_cv, &p->mu);
        if (p->quit) break;
        seen = p->gen;
        for (int j; (j = p->next++) < p->njobs;) {
            pthread_mutex_unlock(&p->mu);
            p->fn(p->ctx, j);
            pthread_mutex_lock(&p->mu);
        }
        if (--p->active == 0) pthread_cond_signal(&p->done_cv);
    }
    pthread_mutex_unlock(&p->mu);
    return NULL;
}
#endif

static cui__sw_pool *cui__sw_pool_create(int n) {
    cui__sw_pool *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->want = n;
#if defined(_WIN32)
    p->go = CreateSemaphoreW(NULL, 0, 64, NULL);
    p->done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (p->go && p->done)
        for (int i = 0; i < n && i < 32; i++)
            if ((p->th[p->n] = CreateThread(NULL, 0, cui__sw_worker, p, 0, NULL)) != NULL) p->n++;
#else
    pthread_mutex_init(&p->mu, NULL);
    pthread_cond_init(&p->go_cv, NULL);
    pthread_cond_init(&p->done_cv, NULL);
    for (int i = 0; i < n && i < 32; i++)
        if (pthread_create(&p->th[p->n], NULL, cui__sw_worker, p) == 0) p->n++;
#endif
    return p;
}

static void cui__sw_pool_destroy(cui__sw_pool *p) {
    if (!p) return;
#if defined(_WIN32)
    p->quit = 1;
    if (p->n) ReleaseSemaphore(p->go, p->n, NULL);
    for (int i = 0; i < p->n; i++) { WaitForSingleObject(p->th[i], INFINITE); CloseHandle(p->th[i]); }
    if (p->go) CloseHandle(p->go);
    if (p->done) CloseHandle(p->done);
#else
    pthread_mutex_lock(&p->mu);
    p->quit = 1;
    pthread_cond_broadcast(&p->go_cv);
    pthread_mutex_unlock(&p->mu);
    for (int i = 0; i < p->n; i++) pthread_join(p->th[i], NULL);
    pthread_cond_destroy(&p->go_cv);
    pthread_cond_destroy(&p->done_cv);
    pthread_mutex_destroy(&p->mu);
#endif
    free(p);
}

/* runs fn(ctx, 0 .. njobs - 1) on the workers and this thread; returns when all are done */
static void cui__sw_pool_run(cui__sw_pool *p, int njobs, void (*fn)(void *, int), void *ctx) {
    int wake = p ? CUI_MIN(p->n, njobs - 1) : 0;
    if (wake <= 0) { for (int j = 0; j < njobs; j++) fn(ctx, j); return; }
    p->fn = fn; p->ctx = ctx; p->njobs = njobs;
#if defined(_WIN32)
    p->next = 0;
    p->active = wake;
    ReleaseSemaphore(p->go, wake, NULL);
    for (LONG j; (j = InterlockedIncrement(&p->next) - 1) < njobs;) fn(ctx, (int)j);
    WaitForSingleObject(p->done, INFINITE);
#else
    pthread_mutex_lock(&p->mu);
    p->next = 0;
    p->active = p->n;
    p->gen++;
    pthread_cond_broadcast(&p->go_cv);
    for (int j; (j = p->next++) < njobs;) {
        pthread_mutex_unlock(&p->mu);
        fn(ctx, j);
        pthread_mutex_lock(&p->mu);
    }
    while (p->active > 0) pthread_cond_wait(&p->done_cv, &p->mu);
    pthread_mutex_unlock(&p->mu);
#endif
}

/* ---- what changed: primitives, tiles, moves ---- */

static inline uint64_t cui__sw_mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}
static uint64_t cui__sw_hash_bytes(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    while (n >= 8) { uint64_t v; memcpy(&v, b, 8); h = cui__sw_mix(h, v); b += 8; n -= 8; }
    while (n--) h = cui__sw_mix(h, *b++);
    return h;
}
static inline uint64_t cui__sw_bits(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline uint64_t cui__sw_fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* A position relative to a point, in double: exact for any two floats of screen size, so two
 * shapes hash alike only when one is the other moved by whole pixels. */
static inline uint64_t cui__sw_rel(float v, double o) { return cui__sw_bits((double)v - o); }

static uint64_t cui__sw_vertex_hash(const cui_vertex *v, float ax, float ay, uint64_t h) {
    h = cui__sw_mix(h, cui__sw_rel(v->x, ax));
    h = cui__sw_mix(h, cui__sw_rel(v->y, ay));
    return cui__sw_hash_bytes(&v->u, sizeof *v - offsetof(cui_vertex, u), h);
}

static inline int cui__sw_iclamp(double v) { return v != v ? 0 : v < -1e9 ? -1000000000 : v > 1e9 ? 1000000000 : (int)v; }

/* the pixels i in [lo, hi) where |l0 + ((i + 0.5) - q0)| <= L, the raster's test for the inside
 * of a rounded rectangle along one axis (one interval: the expression only grows with i) */
static void cui__sw_deep_span(float l0, float q0, float L, int lo, int hi, int *a, int *b) {
    *a = *b = lo;
    if (!(L >= 0) || lo >= hi) return;
    double c = (double)q0 - (double)l0 - 0.5;
    int ia = cui__sw_iclamp(floor(c - L) - 2), ib = cui__sw_iclamp(ceil(c + L) + 3);
    ia = CUI_CLAMP(ia, lo, hi); ib = CUI_CLAMP(ib, lo, hi);
#define CUI__IN(i) (fabsf(l0 + (((float)(i) + 0.5f) - q0)) <= L)
    while (ia < ib && !CUI__IN(ia)) ia++;
    while (ib > ia && !CUI__IN(ib - 1)) ib--;
#undef CUI__IN
    *a = ia; *b = ib;
}

/* The pixels a quad paints alike: all of a solid one, the inside of a rounded rectangle within
 * its border and corners (A: full height between the corners, B: full width), as the raster
 * decides it. Only for one color (no gradient). */
static void cui__sw_inside(const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v3, int mode, float inset, int a[4], int b[4]) {
    int ex0 = cui__sw_iclamp(ceilf(v0->x - 0.5f)), ex1 = cui__sw_iclamp(ceilf(v1->x - 0.5f));
    int ey0 = cui__sw_iclamp(ceilf(v0->y - 0.5f)), ey1 = cui__sw_iclamp(ceilf(v3->y - 0.5f));
    if (mode == CUI_SHADE_SOLID) {
        a[0] = b[0] = ex0; a[1] = b[1] = ey0; a[2] = b[2] = ex1; a[3] = b[3] = ey1;
        return;
    }
    float hw = v0->hw, hh = v0->hh;
    float rmax = CUI_MAX(CUI_MAX(v0->r_tl, v0->r_tr), CUI_MAX(v0->r_br, v0->r_bl));
    rmax = CUI_MIN(rmax, CUI_MIN(hw, hh));
    cui__sw_deep_span(v0->lx, v0->x, hw - inset - rmax, ex0, ex1, &a[0], &a[2]);
    cui__sw_deep_span(v0->ly, v0->y, hh - inset, ey0, ey1, &a[1], &a[3]);
    cui__sw_deep_span(v0->lx, v0->x, hw - inset, ex0, ex1, &b[0], &b[2]);
    cui__sw_deep_span(v0->ly, v0->y, hh - inset - rmax, ey0, ey1, &b[1], &b[3]);
}
static void cui__sw_uniform(cui__sw_prim *p, const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v3) {
    cui__sw_inside(v0, v1, v3, p->mode, v0->border + 0.5f, p->ua, p->ub);
}

static void *cui__sw_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = need + need / 2 + 16;
    p = cui__sw_xrealloc(p, (size_t)c * elem);
    *cap = c;
    return p;
}

/* range of tiles [*a, *b] that [v0, v1] touches; false when none of 0..n-1 */
static bool cui__sw_tile_range(double v0, double v1, int n, int *a, int *b) {
    double t0 = floor(v0 / CUI__SW_TILE), t1 = floor(v1 / CUI__SW_TILE);
    if (!(t0 <= t1) || t1 < 0 || t0 > n - 1) return false;   /* also NaN */
    *a = t0 < 0 ? 0 : (int)t0;
    *b = t1 > n - 1 ? n - 1 : (int)t1;
    return true;
}

/* The tile rows a primitive may paint: its bounds one pixel wider, within its clip. */
static bool cui__sw_prim_rows(const cui__sw_prim *p, double oy, int th, int *a, int *b) {
    double y0 = CUI_MAX((double)p->by0 - 1, (double)p->cy0) - oy, y1 = CUI_MIN((double)p->by1 + 1, (double)p->cy1 - 1) - oy;
    return cui__sw_tile_range(y0, y1, th, a, b);
}
static bool cui__sw_prim_cols(const cui__sw_prim *p, int tw, int *a, int *b) {
    double x0 = CUI_MAX((double)p->bx0 - 1, (double)p->cx0), x1 = CUI_MIN((double)p->bx1 + 1, (double)p->cx1 - 1);
    return cui__sw_tile_range(x0, x1, tw, a, b);
}

static int cui__sw_layer_prim(cui_sw_renderer *r, const cui_draw_data *dd, int gi, int n, int tw, int th);

/* This frame's primitives, and per tile row the ones that may paint it, in drawing order. */
static void cui__sw_build(cui_sw_renderer *r, const cui_draw_data *dd, int tw, int th) {
    int n = 0, gi = 0, ng = dd->groups ? dd->group_count : 0;
    r->nlayer_prims = 0;
    for (int ci = 0; ci < dd->command_count; ci++) {
        /* a group drawn from its bitmap: one primitive instead of its commands */
        while (gi < ng && (r->gslot[gi] < 0 || dd->groups[gi].command_offset < ci)) gi++;
        if (gi < ng && dd->groups[gi].command_offset == ci) {
            int was = n;
            n = cui__sw_layer_prim(r, dd, gi, n, tw, th);
            r->nlayer_prims += n - was;
            ci += dd->groups[gi].command_count - 1;
            gi++;
            continue;
        }
        const cui_draw_cmd *cmd = &dd->commands[ci];
        cui_texture tid = cmd->texture ? cmd->texture : dd->font_atlas;
        int cx0 = cui__sw_iclamp(floorf(cmd->clip.x)), cy0 = cui__sw_iclamp(floorf(cmd->clip.y));
        int cx1 = cui__sw_iclamp(ceilf(cmd->clip.x + cmd->clip.w)), cy1 = cui__sw_iclamp(ceilf(cmd->clip.y + cmd->clip.h));
        if (cx0 >= cx1 || cy0 >= cy1) continue;
        /* a changed part of this command's texture, in texture pixels */
        int tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
        float tw_px = 1, th_px = 1;
        bool tex_changed = false;
        for (int k = 0; k < r->nchanged; k++)
            if (r->changed[k].t == tid) {
                int slot = (int)tid - 1;
                if (slot >= 0 && slot < r->tex_count && r->tex[slot].used) { tw_px = (float)r->tex[slot].w; th_px = (float)r->tex[slot].h; }
                tx0 = r->changed[k].x0; ty0 = r->changed[k].y0; tx1 = r->changed[k].x1; ty1 = r->changed[k].y1;
                tex_changed = true;
                break;
            }
        for (uint32_t k = 0; k + 2 < cmd->index_count;) {
            const uint32_t *ix = dd->indices + cmd->index_offset + k;
            bool quad = k + 5 < cmd->index_count && ix[3] == ix[0] && ix[4] == ix[2] && ix[1] == ix[0] + 1 && ix[2] == ix[0] + 2 && ix[5] == ix[0] + 3;
            const cui_vertex *v[4];
            int nv = quad ? 4 : 3;
            if (quad) for (int i = 0; i < 4; i++) v[i] = &dd->vertices[ix[0] + (uint32_t)i];
            else for (int i = 0; i < 3; i++) v[i] = &dd->vertices[ix[i]];
            if (n == r->prims_cap) r->prims = cui__sw_grow(r->prims, &r->prims_cap, n + 1, sizeof *r->prims);
            cui__sw_prim *p = &r->prims[n];
            p->bx0 = p->bx1 = v[0]->x; p->by0 = p->by1 = v[0]->y;
            for (int i = 1; i < nv; i++) {
                p->bx0 = CUI_MIN(p->bx0, v[i]->x); p->bx1 = CUI_MAX(p->bx1, v[i]->x);
                p->by0 = CUI_MIN(p->by0, v[i]->y); p->by1 = CUI_MAX(p->by1, v[i]->y);
            }
            p->cx0 = cx0; p->cy0 = cy0; p->cx1 = cx1; p->cy1 = cy1;
            p->cmd = (uint32_t)ci; p->k = k;
            p->quad = quad;
            k += quad ? 6 : 3;
            int a, b;
            if (!cui__sw_prim_rows(p, 0, th, &a, &b) || !cui__sw_prim_cols(p, tw, &a, &b)) continue;   /* paints nothing */
            p->ax = v[0]->x; p->ay = v[0]->y;
            p->mode = (uint8_t)(int)(v[0]->mode + 0.5f);
            p->ua[0] = p->ub[0] = 1000000000; p->ua[2] = p->ub[2] = -1000000000;
            p->ua[1] = p->ub[1] = p->ua[3] = p->ub[3] = 0;
            p->key = 0;
            p->occ = 0;
            p->layer = 0;
            uint64_t h = (uint64_t)tid * 0x100000001B3ull + 7;
            if (quad && cui__sw_quad_ok(v[0], v[1], v[2], v[3])) {
                /* what cui__raster_quad reads: the first vertex, the right side and the bottom */
                h = cui__sw_vertex_hash(v[0], p->ax, p->ay, h ^ 0x51);
                h = cui__sw_mix(h, cui__sw_rel(v[1]->x, p->ax));
                h = cui__sw_mix(h, cui__sw_fbits(v[1]->u));
                h = cui__sw_mix(h, cui__sw_rel(v[3]->y, p->ay));
                h = cui__sw_mix(h, cui__sw_fbits(v[3]->v));
                h = cui__sw_hash_bytes(&v[3]->color, 4, h);
                bool flat = memcmp(&v[0]->color, &v[3]->color, 4) == 0;
                if (flat && (p->mode == CUI_SHADE_SOLID || p->mode == CUI_SHADE_RRECT)) {
                    memcpy(&p->key, &v[0]->color, 4);
                    cui__sw_uniform(p, v[0], v[1], v[3]);
                }
                /* what hides the clear and what was drawn before it: an opaque solid or rounded
                 * rectangle inside its border, an opaque picture under an opaque tint inside its corners */
                if (flat && v[0]->color.a == 255 && (p->mode == CUI_SHADE_SOLID || p->mode == CUI_SHADE_RRECT)) {
                    memcpy(p->oa, p->ua, sizeof p->oa); memcpy(p->ob, p->ub, sizeof p->ob);
                    p->occ = 1;
                } else if (flat && v[0]->color.a == 255 && p->mode == CUI_SHADE_IMAGE) {
                    int slot = (int)tid - 1;
                    if (slot >= 0 && slot < r->tex_count && r->tex[slot].used && r->tex[slot].opaque) {
                        cui__sw_inside(v[0], v[1], v[3], CUI_SHADE_IMAGE, 0.5f, p->oa, p->ob);
                        p->occ = 1;
                    }
                }
            } else {
                for (int i = 0; i < nv; i++) h = cui__sw_vertex_hash(v[i], p->ax, p->ay, h);
                h = cui__sw_mix(h, (uint64_t)nv);
            }
            p->h = h;
            p->forced = 0;
            if (tex_changed) {
                float u0 = v[0]->u, u1 = v[0]->u, w0 = v[0]->v, w1 = v[0]->v;
                for (int i = 1; i < nv; i++) {
                    u0 = CUI_MIN(u0, v[i]->u); u1 = CUI_MAX(u1, v[i]->u);
                    w0 = CUI_MIN(w0, v[i]->v); w1 = CUI_MAX(w1, v[i]->v);
                }
                u0 *= tw_px; u1 *= tw_px; w0 *= th_px; w1 *= th_px;
                p->forced = u1 >= (float)tx0 - 1 && u0 <= (float)tx1 + 1 && w1 >= (float)ty0 - 1 && w0 <= (float)ty1 + 1;
            }
            n++;
        }
    }
    r->nprims = n;
    /* after a burst (a big frame) the list comes back down, as the other per frame buffers do */
    if (r->prims_cap <= 4 * n + 4096) r->prims_big = 0;
    else if (++r->prims_big > 600) { r->prims_cap = n + n / 2 + 16; r->prims = cui__sw_xrealloc(r->prims, sizeof *r->prims * (size_t)r->prims_cap); }
    /* rows: count, then place (a counting sort keeps the drawing order) */
    r->row_start = cui__sw_grow(r->row_start, &r->row_start_cap, 2 * (th + 1), sizeof *r->row_start);
    uint32_t *start = r->row_start, *at = r->row_start + th + 1;
    memset(start, 0, sizeof *start * (size_t)(th + 1));
    for (int i = 0; i < n; i++) {
        int a, b;
        if (cui__sw_prim_rows(&r->prims[i], 0, th, &a, &b)) for (int t = a; t <= b; t++) start[t + 1]++;
    }
    for (int t = 0; t < th; t++) start[t + 1] += start[t];
    r->row_items = cui__sw_grow(r->row_items, &r->row_items_cap, (int)start[th] + 1, sizeof *r->row_items);
    memcpy(at, start, sizeof *start * (size_t)(th + 1));
    for (int i = 0; i < n; i++) {
        int a, b;
        if (cui__sw_prim_rows(&r->prims[i], 0, th, &a, &b)) for (int t = a; t <= b; t++) r->row_items[at[t]++] = (uint32_t)i;
    }
}

/* Each tile's hash: what is drawn over it, in order, relative to the tile, with the clip cut to
 * the tile. With oy, over the parts of the frame oy pixels further down (the last frame's
 * primitives give where each tile's pixels would come from after a move). A shape that paints
 * the whole tile alike counts by its color only, so a background larger than the window
 * hashes the same wherever it is. force marks tiles under a primitive whose texture changed. */
static void cui__sw_grid(const cui__sw_prim *ps, int n, uint64_t seed, uint64_t *out, uint8_t *force, uint64_t *under,
                         uint8_t *r_busy, int tw, int th, int width, int height, int oy) {
    for (int i = 0; i < tw * th; i++) out[i] = seed;
    if (force) memset(force, 0, (size_t)tw * (size_t)th);
    if (under) memset(under, 0, sizeof *under * (size_t)tw * (size_t)th);
    if (r_busy) memset(r_busy, 0, (size_t)tw * (size_t)th);
    for (int i = 0; i < n; i++) {
        const cui__sw_prim *p = &ps[i];
        int ta, tb, xa, xb;
        if (!cui__sw_prim_rows(p, oy, th, &ta, &tb) || !cui__sw_prim_cols(p, tw, &xa, &xb)) continue;
        uint64_t hu = cui__sw_mix(0xC0FFEE00ull + p->mode, p->key);
        for (int ty = ta; ty <= tb; ty++) {
            int ry0 = ty * CUI__SW_TILE + oy, ry1 = CUI_MIN(height, (ty + 1) * CUI__SW_TILE) + oy;
            int qy0 = CUI_MAX(p->cy0, ry0), qy1 = CUI_MIN(p->cy1, ry1);
            if (qy0 >= qy1) continue;
            double rel_y = (double)p->ay - ry0;
            for (int tx = xa; tx <= xb; tx++) {
                int rx0 = tx * CUI__SW_TILE, rx1 = CUI_MIN(width, rx0 + CUI__SW_TILE);
                int qx0 = CUI_MAX(p->cx0, rx0), qx1 = CUI_MIN(p->cx1, rx1);
                if (qx0 >= qx1) continue;
                uint64_t clip = ((uint64_t)(uint32_t)(qx0 - rx0) << 48) | ((uint64_t)(uint32_t)(qy0 - ry0) << 32) |
                                ((uint64_t)(uint32_t)(qx1 - rx0) << 16) | (uint64_t)(uint32_t)(qy1 - ry0);
                uint64_t v;
                if ((p->ua[0] <= qx0 && qx1 <= p->ua[2] && p->ua[1] <= qy0 && qy1 <= p->ua[3]) ||
                    (p->ub[0] <= qx0 && qx1 <= p->ub[2] && p->ub[1] <= qy0 && qy1 <= p->ub[3])) v = hu;
                else v = cui__sw_mix(cui__sw_mix(p->h, cui__sw_bits((double)p->ax - rx0)), cui__sw_bits(rel_y));
                if (under && !under[ty * tw + tx]) {
                    /* what is under the first bitmap (odd); 2 when that is only shapes that paint the whole tile alike and the
                     * clear: drawing them again (fills) costs less than copying the saved pixels back */
                    if (p->layer) under[ty * tw + tx] = r_busy && r_busy[ty * tw + tx] ? out[ty * tw + tx] | 1 : 2;
                    else if (v != hu && r_busy) r_busy[ty * tw + tx] = 1;
                }
                out[ty * tw + tx] = cui__sw_mix(out[ty * tw + tx], cui__sw_mix(v, clip));
                if (force && p->forced) force[ty * tw + tx] = 1;
            }
        }
    }
}

/* How far what the last frame drew has moved down, in whole pixels: up to three distances,
 * the most common first (parts of a frame can move by different amounts: lines of text land on
 * whole pixels, so content moved by a fraction moves its lines by one or the other neighbor).
 * The last frame's primitives are found by content and x; ones that appear twice do not count. */
static int cui__sw_vote(cui_sw_renderer *r, int height, int out[3]) {
    int cap = 64;
    while (cap < 2 * r->npprims) cap *= 2;
    if (cap > r->vcap) {
        r->vkeys = cui__sw_xrealloc(r->vkeys, sizeof *r->vkeys * (size_t)cap);
        r->vy = cui__sw_xrealloc(r->vy, sizeof *r->vy * (size_t)cap);
        r->vcap = cap;
    }
    memset(r->vkeys, 0, sizeof *r->vkeys * (size_t)cap);
    unsigned mask = (unsigned)cap - 1;
    for (int i = 0; i < r->npprims; i++) {
        const cui__sw_prim *p = &r->pprims[i];
        uint64_t key = cui__sw_mix(p->h, cui__sw_fbits(p->ax)) | 1;
        unsigned s = (unsigned)(key >> 32) & mask;
        while (r->vkeys[s] && r->vkeys[s] != key) s = (s + 1) & mask;
        if (!r->vkeys[s]) { r->vkeys[s] = key; r->vy[s] = p->ay; }
        else r->vy[s] = 1e30f;   /* twice: not a clue */
    }
    double dys[16]; int counts[16], nd = 0;
    for (int i = 0; i < r->nprims; i++) {
        const cui__sw_prim *p = &r->prims[i];
        uint64_t key = cui__sw_mix(p->h, cui__sw_fbits(p->ax)) | 1;
        unsigned s = (unsigned)(key >> 32) & mask;
        while (r->vkeys[s] && r->vkeys[s] != key) s = (s + 1) & mask;
        if (!r->vkeys[s] || r->vy[s] == 1e30f) continue;
        double d = (double)p->ay - (double)r->vy[s];
        if (d == 0 || d != floor(d) || fabs(d) >= height) continue;
        int j = 0;
        while (j < nd && dys[j] != d) j++;
        if (j == nd) { if (nd == 16) continue; dys[nd] = d; counts[nd++] = 0; }
        counts[j]++;
    }
    int n = 0;
    while (n < 3) {
        int best = -1;
        for (int j = 0; j < nd; j++) if (counts[j] >= 8 && (best < 0 || counts[j] > counts[best])) best = j;
        if (best < 0) break;
        out[n++] = (int)dys[best];
        counts[best] = 0;
    }
    return n;
}

/* ---- the layer cache (cui_set_layer_cache) ----
 * A group (cui_draw_data.groups) is drawn once into a bitmap at its rest place, by a second renderer
 * that borrows this one's textures, then each frame of its animation the bitmap is blended where the
 * group's transform puts it: one primitive instead of the group's shadows, body and text. The bitmap
 * is drawn again when the group's content changes (a hash of its rest vertices and commands, and a
 * texture it samples being written) and let go on the first frame without the group. A group at rest
 * (no scale, whole pixel offset, opaque) whose bitmap is not current is drawn the plain way: that is
 * cheap with incremental frames, and the bitmap is made when it moves again. */

static void cui__sw_layer_release(cui_sw_renderer *r, cui__sw_layer *L) {
    r->layer_bytes -= L->cap;
    free(L->px);
    free(L->span);
    free(L->tcol); free(L->tw); free(L->trow); free(L->tw4);
    memset(L, 0, sizeof *L);
}

/* L's tables for this frame's scaled blend (cui__sw_layer_blit); false when there is no memory for them */
static bool cui__sw_layer_tables(cui__sw_layer *L) {
    float s = L->s, inv = 1.0f / s;
    int xa = (int)floorf(L->dx + s * (float)L->x0), xb = (int)ceilf(L->dx + s * (float)(L->x0 + L->w));
    int ya = (int)floorf(L->dy + s * (float)L->y0), yb = (int)ceilf(L->dy + s * (float)(L->y0 + L->h));
    int nx = CUI_MAX(0, xb - xa), ny = CUI_MAX(0, yb - ya);
    if (nx > L->tcap_x) {
        int *c = realloc(L->tcol, sizeof *c * (size_t)nx);
        if (c) L->tcol = c;
        int32_t *w = realloc(L->tw, sizeof *w * (size_t)nx);
        if (w) L->tw = w;
        free(L->tw4);
        L->tw4 = malloc(sizeof *L->tw4 * 8 * (size_t)nx);   /* (none: the blend reads tw alone) */
        if (!c || !w) return false;
        L->tcap_x = nx;
    }
    if (ny > L->tcap_y) {
        int *t = realloc(L->trow, sizeof *t * 4 * (size_t)ny);
        if (!t) return false;
        L->trow = t; L->tcap_y = ny;
    }
    L->tx0 = xa; L->tnx = nx; L->ty0 = ya; L->tny = ny;
    for (int i = 0; i < nx; i++) {
        float u = (((float)(xa + i) + 0.5f) - L->dx) * inv - (float)L->x0 - 0.5f;
        float fu = floorf(u);
        int iu = (int)fu;
        float f = u - fu;
        if (iu < 0) { iu = 0; f = 0; }
        if (iu > L->w - 2) { iu = L->w - 2; f = 1; }
        int wq = (int)(f * 256.0f + 0.5f);
        L->tcol[i] = iu;
        L->tw[i] = (int32_t)((uint32_t)wq << 16 | (uint32_t)(256 - wq));
        if (L->tw4)
            for (int ch = 0; ch < 4; ch++) {
                L->tw4[(size_t)i * 4 + (size_t)ch] = (uint16_t)(256 - wq);
                L->tw4[(size_t)L->tcap_x * 4 + (size_t)i * 4 + (size_t)ch] = (uint16_t)wq;
            }
    }
    for (int j = 0; j < ny; j++) {
        float v = (((float)(ya + j) + 0.5f) - L->dy) * inv - (float)L->y0 - 0.5f;
        float fv = floorf(v);
        int jv = (int)fv;
        float f = v - fv;
        if (jv < 0) { jv = 0; f = 0; }
        if (jv > L->h - 2) { jv = L->h - 2; f = 1; }
        int wy = (int)(f * 256.0f + 0.5f);
        /* the pixels that are not clear in the rows it reads, then the screen columns that read them (tcol only grows) */
        int sl = L->span[2 * jv], sr = L->span[2 * jv + 1];
        if (wy) {
            int sl2 = L->span[2 * jv + 2], sr2 = L->span[2 * jv + 3];
            if (wy == 256) { sl = sl2; sr = sr2; }
            else if (sl2 < sr2) { if (sl >= sr) { sl = sl2; sr = sr2; } else { sl = CUI_MIN(sl, sl2); sr = CUI_MAX(sr, sr2); } }
        }
        int a = 0, b = 0;
        if (sl < sr) {
            int lo = 0, hi = nx;   /* first i with tcol[i] + 1 >= sl */
            while (lo < hi) { int m = (lo + hi) / 2; if (L->tcol[m] + 1 < sl) lo = m + 1; else hi = m; }
            a = lo;
            lo = a; hi = nx;       /* first i with tcol[i] >= sr */
            while (lo < hi) { int m = (lo + hi) / 2; if (L->tcol[m] < sr) lo = m + 1; else hi = m; }
            b = lo;
        }
        int *t = L->trow + 4 * j;
        t[0] = jv; t[1] = wy; t[2] = xa + a; t[3] = xa + b;
    }
    return true;
}

/* the pixels kept under the bitmaps: forgotten (another frame size), and their memory let go too with free_px */
static void cui__sw_under_drop(cui_sw_renderer *r, bool free_px) {
    if (r->uslot)
        for (int i = 0; i < r->tiles_cap; i++) { r->uslot[i] = -1; r->uvalid[i] = 0; }
    r->ucount = 0;
    if (free_px) { free(r->upx); r->upx = NULL; r->ucap = 0; r->ubytes = 0; }
}

/* everything the cache holds (no group left, or the renderer goes) */
static void cui__sw_layers_free(cui_sw_renderer *r) {
    if (r->layers)
        for (int i = 0; i < CUI__SW_LAYERS; i++) cui__sw_layer_release(r, &r->layers[i]);
    free(r->layers); r->layers = NULL;
    if (r->sub) { r->sub->tex = NULL; r->sub->tex_count = r->sub->tex_cap = 0; cui_sw_destroy(r->sub); r->sub = NULL; }
    free(r->lvtx); r->lvtx = NULL; r->lvtx_cap = 0;
    free(r->lcmd); r->lcmd = NULL; r->lcmd_cap = 0;
    cui__sw_under_drop(r, true);
}

/* what a group's bitmap depends on: its rest vertices relative to the bitmap, indices and commands */
static uint64_t cui__sw_group_hash(const cui_draw_group *g, cui_texture atlas, float ox, float oy) {
    uint64_t h = 0x6C6179657243ull;
    /* four vertices at a time into four hashes: each hash is a chain of multiplies, four chains run side by side */
    uint64_t q[4] = { h, h ^ 1, h ^ 2, h ^ 3 };
    int i = 0;
    for (; i + 4 <= g->rest_vertex_count; i += 4)
        for (int k = 0; k < 4; k++) q[k] = cui__sw_vertex_hash(&g->rest_vertices[i + k], ox, oy, q[k]);
    for (int k = 0; k < 4; k++) h = cui__sw_mix(h, q[k]);
    for (; i < g->rest_vertex_count; i++) h = cui__sw_vertex_hash(&g->rest_vertices[i], ox, oy, h);
    h = cui__sw_hash_bytes(g->rest_indices, sizeof *g->rest_indices * (size_t)g->rest_index_count, h);
    for (int i = 0; i < g->rest_command_count; i++) {
        const cui_draw_cmd *c = &g->rest_commands[i];
        h = cui__sw_mix(h, (uint64_t)(c->texture ? c->texture : atlas));
        h = cui__sw_mix(h, ((uint64_t)c->index_offset << 32) | c->index_count);
        h = cui__sw_mix(h, cui__sw_rel(c->clip.x, ox));
        h = cui__sw_mix(h, cui__sw_rel(c->clip.y, oy));
        h = cui__sw_mix(h, cui__sw_fbits(c->clip.w) << 32 | cui__sw_fbits(c->clip.h));
    }
    return h;
}

/* a part of a texture the group samples was written since the last frame (as cui__sw_build decides it) */
static bool cui__sw_group_tex_changed(const cui_sw_renderer *r, const cui_draw_group *g, cui_texture atlas) {
    if (r->changed_overflow) return true;
    for (int i = 0; i < g->rest_command_count && r->nchanged; i++) {
        const cui_draw_cmd *c = &g->rest_commands[i];
        cui_texture t = c->texture ? c->texture : atlas;
        int k = 0;
        while (k < r->nchanged && r->changed[k].t != t) k++;
        if (k == r->nchanged || !c->index_count) continue;
        int slot = (int)t - 1;
        float tw = 1, th = 1;
        if (slot >= 0 && slot < r->tex_count && r->tex[slot].used) { tw = (float)r->tex[slot].w; th = (float)r->tex[slot].h; }
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (uint32_t j = 0; j < c->index_count; j++) {
            uint32_t vi = g->rest_indices[c->index_offset + j];
            if (vi >= (uint32_t)g->rest_vertex_count) continue;
            const cui_vertex *v = &g->rest_vertices[vi];
            u0 = CUI_MIN(u0, v->u); u1 = CUI_MAX(u1, v->u); v0 = CUI_MIN(v0, v->v); v1 = CUI_MAX(v1, v->v);
        }
        u0 *= tw; u1 *= tw; v0 *= th; v1 *= th;
        if (u1 >= (float)r->changed[k].x0 - 1 && u0 <= (float)r->changed[k].x1 + 1 && v1 >= (float)r->changed[k].y0 - 1 && v0 <= (float)r->changed[k].y1 + 1) return true;
    }
    return false;
}

/* draws group g into L's bitmap (x0, y0, w, h in rest pixels); false when there is no memory for it */
static bool cui__sw_layer_draw(cui_sw_renderer *r, cui__sw_layer *L, const cui_draw_group *g, const cui_draw_data *dd,
                               cui_sw_format fmt, int x0, int y0, int w, int h) {
    size_t need = (size_t)w * (size_t)h * 4 + 64;   /* 16 texels to spare after the last row (cui__sw_blit_mixed_avx2 reads them) */
    if (L->cap < need || L->cap > 2 * need) {
        r->layer_bytes -= L->cap;
        free(L->px);
        L->px = malloc(need);
        L->cap = L->px ? need : 0;
        r->layer_bytes += L->cap;
        if (!L->px) return false;
        if (r->layer_bytes + r->ubytes > r->layer_peak) r->layer_peak = r->layer_bytes + r->ubytes;
    }
    if (h > L->span_cap) {
        int *sp = realloc(L->span, sizeof *L->span * 2 * (size_t)h);
        if (!sp) return false;
        L->span = sp; L->span_cap = h;
    }
    if (!r->sub) {
        r->sub = cui_sw_create();
        if (!r->sub) return false;
        r->sub->threads = 1;
        r->sub->keep_alpha = true;
    }
    /* the group moved to the bitmap's origin (whole pixels: the same samples) */
    int nv = g->rest_vertex_count, nc = g->rest_command_count;
    r->lvtx = cui__sw_grow(r->lvtx, &r->lvtx_cap, nv, sizeof *r->lvtx);
    r->lcmd = cui__sw_grow(r->lcmd, &r->lcmd_cap, nc, sizeof *r->lcmd);
    for (int i = 0; i < nv; i++) { r->lvtx[i] = g->rest_vertices[i]; r->lvtx[i].x -= (float)x0; r->lvtx[i].y -= (float)y0; }
    for (int i = 0; i < nc; i++) { r->lcmd[i] = g->rest_commands[i]; r->lcmd[i].clip.x -= (float)x0; r->lcmd[i].clip.y -= (float)y0; }
    cui_draw_data sd;
    memset(&sd, 0, sizeof sd);   /* clear: transparent */
    sd.vertices = r->lvtx; sd.vertex_count = nv;
    sd.indices = g->rest_indices; sd.index_count = g->rest_index_count;
    sd.commands = r->lcmd; sd.command_count = nc;
    sd.width_px = w; sd.height_px = h;
    sd.font_atlas = dd->font_atlas;
    r->sub->tex = r->tex; r->sub->tex_count = r->tex_count; r->sub->tex_cap = r->tex_cap;
    cui_sw_render(r->sub, &sd, L->px, w, h, w * 4, fmt);
    r->sub->tex = NULL; r->sub->tex_count = r->sub->tex_cap = 0;
    memset(L->px + (size_t)w * (size_t)h * 4, 0, 64);
    /* per row, the pixels that are not clear */
    for (int y = 0; y < h; y++) {
        const uint8_t *row = L->px + (size_t)y * (size_t)w * 4;
        int a = 0, b = w;
        while (a < w && !row[a * 4 + 3]) a++;
        while (b > a && !row[(b - 1) * 4 + 3]) b--;
        L->span[2 * y] = a; L->span[2 * y + 1] = b;
    }
    L->x0 = x0; L->y0 = y0; L->w = w; L->h = h; L->fmt = fmt;
    L->valid = true;
    r->layer_rasters++;
    return true;
}

/* Which groups are drawn from a bitmap this frame (r->gslot), their bitmaps drawn where needed. */
static void cui__sw_layers_prepare(cui_sw_renderer *r, const cui_draw_data *dd, cui_sw_format fmt) {
    int ng = dd->groups ? dd->group_count : 0;
    if (ng > r->gslot_cap) { r->gslot = cui__sw_xrealloc(r->gslot, sizeof *r->gslot * (size_t)ng); r->gslot_cap = ng; }
    if (!ng && !r->layers) return;
    if (!r->layers) r->layers = calloc(CUI__SW_LAYERS, sizeof *r->layers);
    if (!r->layers) { for (int i = 0; i < ng; i++) r->gslot[i] = -1; return; }
    for (int i = 0; i < CUI__SW_LAYERS; i++) r->layers[i].seen = false;
    for (int gi = 0; gi < ng; gi++) {
        const cui_draw_group *g = &dd->groups[gi];
        r->gslot[gi] = -1;
        if (g->rest_vertex_count <= 0 || g->command_count <= 0 || !(g->scale >= 0.5f && g->scale <= 2.0f) ||
            !(g->opacity >= 0) || !(fabsf(g->dx) < 1e6f && fabsf(g->dy) < 1e6f)) continue;
        /* the bitmap: the rest bounds with a clear pixel around them */
        float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
        for (int i = 0; i < g->rest_vertex_count; i++) {
            const cui_vertex *v = &g->rest_vertices[i];
            mnx = CUI_MIN(mnx, v->x); mxx = CUI_MAX(mxx, v->x); mny = CUI_MIN(mny, v->y); mxy = CUI_MAX(mxy, v->y);
        }
        double bx0 = floor(mnx) - 1, by0 = floor(mny) - 1, bx1 = ceil(mxx) + 1, by1 = ceil(mxy) + 1;
        if (!(bx0 > -1e6 && by0 > -1e6 && bx1 < 1e6 && by1 < 1e6) || (bx1 - bx0) * (by1 - by0) > CUI__SW_LAYER_PX) continue;
        int x0 = (int)bx0, y0 = (int)by0, w = (int)(bx1 - bx0), h = (int)(by1 - by0);
        if (w < 2 || h < 2) continue;
        /* its bitmap from the last frame, else a free one */
        int slot = -1, spare = -1;
        for (int i = 0; i < CUI__SW_LAYERS; i++) {
            if (r->layers[i].used && r->layers[i].id == g->id && !r->layers[i].seen) { slot = i; break; }
            if (!r->layers[i].used && spare < 0) spare = i;
        }
        if (slot < 0) {
            if (spare < 0) continue;
            slot = spare;
            memset(&r->layers[slot], 0, sizeof r->layers[slot]);
            r->layers[slot].used = true; r->layers[slot].id = g->id; r->layers[slot].fresh = true;
        }
        cui__sw_layer *L = &r->layers[slot];
        L->seen = true;
        uint64_t hh = cui__sw_mix(cui__sw_group_hash(g, dd->font_atlas, (float)x0, (float)y0) ^ (uint64_t)fmt,
                                  ((uint64_t)(uint32_t)w << 32) | (uint32_t)h);
        if (L->valid && (L->fmt != fmt || L->x0 != x0 || L->y0 != y0 || L->w != w || L->h != h || cui__sw_group_tex_changed(r, g, dd->font_atlas)))
            L->valid = false;
        /* the transform; unscaled (within 1/10000: a spring's end is not always exactly 1), on whole pixels,
         * so it is copied, not resampled */
        float ps = L->s, pdx = L->dx, pdy = L->dy;
        int pa = L->a256;
        L->s = fabsf(g->scale - 1.0f) < 1e-4f ? 1.0f : g->scale;
        L->dx = L->s == 1.0f ? floorf(g->dx + 0.5f) : g->dx;
        L->dy = L->s == 1.0f ? floorf(g->dy + 0.5f) : g->dy;
        if (L->s != 1.0f && (dd->layer_flags & CUI_LAYER_SNAP) && fabsf(g->scale - 1.0f) * 0.5f * (float)CUI_MAX(w, h) <= 0.25f) {
            /* CUI_LAYER_SNAP: scaled by less than a quarter of a pixel at its edges: copied, its center where the scale puts it */
            L->s = 1.0f;
            r->usnapped++;
            L->dx = floorf(g->dx + (g->scale - 1.0f) * ((float)x0 + 0.5f * (float)w) + 0.5f);
            L->dy = floorf(g->dy + (g->scale - 1.0f) * ((float)y0 + 0.5f * (float)h) + 0.5f);
        }
        L->a256 = (int)((g->opacity > 1 ? 1 : g->opacity) * 256.0f + 0.5f);
        /* at rest: placed as on the last frame (new: unscaled and opaque) */
        bool at_rest = L->fresh ? L->s == 1.0f && L->a256 == 256 : L->s == ps && L->dx == pdx && L->dy == pdy && L->a256 == pa;
        bool use = L->valid && hh == L->hash;
        /* moving: drawn into the bitmap, except right after drawing it for content that changed again
         * (content that changes on every frame, a spinner say, then goes plain every other frame) */
        bool drawn = false;
        if (!use && !at_rest && (L->fresh || hh == L->last || !L->drawn) &&
            r->layer_bytes - L->cap + (size_t)w * (size_t)h * 4 <= (size_t)CUI__SW_LAYERS_PX * 4) {
            use = drawn = cui__sw_layer_draw(r, L, g, dd, fmt, x0, y0, w, h);
            if (use) L->hash = hh;
        }
        if (!use) L->valid = false;
        else if (L->s != 1.0f && !cui__sw_layer_tables(L)) use = false;   /* (no memory for the tables: plain) */
        L->drawn = drawn;
        L->last = hh;
        L->fresh = false;
        r->gslot[gi] = use ? slot : -1;
    }
    /* the bitmaps of groups that are gone */
    bool any = false;
    for (int i = 0; i < CUI__SW_LAYERS; i++) {
        if (r->layers[i].used && !r->layers[i].seen) cui__sw_layer_release(r, &r->layers[i]);
        any |= r->layers[i].used;
    }
    if (!any) cui__sw_layers_free(r);
}

/* group gi's primitive: its bitmap where the transform puts it, inside the group's clip */
static int cui__sw_layer_prim(cui_sw_renderer *r, const cui_draw_data *dd, int gi, int n, int tw, int th) {
    const cui_draw_group *g = &dd->groups[gi];
    int slot = r->gslot[gi];
    const cui__sw_layer *L = &r->layers[slot];
    if (n == r->prims_cap) r->prims = cui__sw_grow(r->prims, &r->prims_cap, n + 1, sizeof *r->prims);
    cui__sw_prim *p = &r->prims[n];
    memset(p, 0, sizeof *p);
    p->bx0 = L->s * (float)L->x0 + L->dx; p->bx1 = L->s * (float)(L->x0 + L->w) + L->dx;
    p->by0 = L->s * (float)L->y0 + L->dy; p->by1 = L->s * (float)(L->y0 + L->h) + L->dy;
    p->cx0 = cui__sw_iclamp(floorf(g->clip.x)); p->cy0 = cui__sw_iclamp(floorf(g->clip.y));
    p->cx1 = cui__sw_iclamp(ceilf(g->clip.x + g->clip.w)); p->cy1 = cui__sw_iclamp(ceilf(g->clip.y + g->clip.h));
    p->cmd = (uint32_t)g->command_offset;
    int a, b;
    if (p->cx0 >= p->cx1 || p->cy0 >= p->cy1 || !cui__sw_prim_rows(p, 0, th, &a, &b) || !cui__sw_prim_cols(p, tw, &a, &b)) return n;
    p->ax = p->bx0; p->ay = p->by0;
    p->ua[0] = p->ub[0] = 1000000000; p->ua[2] = p->ub[2] = -1000000000;
    p->h = cui__sw_mix(cui__sw_mix(cui__sw_mix(0x1A7E4ull, L->hash), cui__sw_fbits(L->s) << 16 | (uint64_t)L->a256),
                       ((uint64_t)(uint32_t)L->w << 32) | (uint32_t)L->h);
    p->layer = (uint8_t)(slot + 1);
    return n + 1;
}

/* n pixels of a bitmap row s over d at opacity a256 (0..256), premultiplied: d = c + d * (255 - c.a) / 255 */
static void cui__sw_over_row(uint8_t *d, const uint8_t *s, int n, int a256) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i z = _mm_setzero_si128(), k128 = _mm_set1_epi16(128), k255 = _mm_set1_epi16(255), A = _mm_set1_epi16((short)a256);
    const __m128i amask = _mm_set1_epi32((int)0xFF000000u);
    for (; k + 4 <= n; k += 4) {
        __m128i c8 = _mm_loadu_si128((const __m128i *)(const void *)(s + (size_t)k * 4));
        __m128i am = _mm_and_si128(c8, amask);
        if (_mm_movemask_epi8(_mm_cmpeq_epi32(am, z)) == 0xFFFF) continue;   /* four clear pixels */
        if (a256 == 256 && _mm_movemask_epi8(_mm_cmpeq_epi32(am, amask)) == 0xFFFF) {   /* four opaque ones */
            _mm_storeu_si128((__m128i *)(void *)(d + (size_t)k * 4), c8);
            continue;
        }
        __m128i dp = _mm_loadu_si128((const __m128i *)(const void *)(d + (size_t)k * 4));
        __m128i out[2];
        for (int half = 0; half < 2; half++) {
            __m128i c = half ? _mm_unpackhi_epi8(c8, z) : _mm_unpacklo_epi8(c8, z);
            if (a256 < 256) c = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c, A), k128), 8);
            __m128i al = _mm_shufflehi_epi16(_mm_shufflelo_epi16(c, 0xFF), 0xFF);
            __m128i x = _mm_mullo_epi16(half ? _mm_unpackhi_epi8(dp, z) : _mm_unpacklo_epi8(dp, z), _mm_sub_epi16(k255, al));
            x = _mm_add_epi16(x, k128);
            x = _mm_srli_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), 8);
            out[half] = _mm_add_epi16(c, x);
        }
        _mm_storeu_si128((__m128i *)(void *)(d + (size_t)k * 4), _mm_packus_epi16(out[0], out[1]));
    }
#endif
    for (; k < n; k++) {
        const uint8_t *c = s + (size_t)k * 4;
        uint8_t *q = d + (size_t)k * 4;
        if (!c[3]) continue;
        unsigned cv[4];
        for (int ch = 0; ch < 4; ch++) cv[ch] = a256 < 256 ? (c[ch] * (unsigned)a256 + 128u) >> 8 : c[ch];
        unsigned ia = 255u - cv[3];
        for (int ch = 0; ch < 4; ch++) {
            unsigned x = q[ch] * ia + 128u;
            unsigned v = cv[ch] + ((x + (x >> 8)) >> 8);
            q[ch] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* texels [0, n) of rows ra and rb mixed (wy for rb, 0..256) into v: the first step of the scaled blend */
static void cui__sw_mix_rows(uint8_t *v, const uint8_t *ra, const uint8_t *rb, int n, int wy) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i z = _mm_setzero_si128(), k128 = _mm_set1_epi16(128);
    const __m128i W0 = _mm_set1_epi16((short)(256 - wy)), W1 = _mm_set1_epi16((short)wy);
    for (; k + 4 <= n; k += 4) {
        __m128i a = _mm_loadu_si128((const __m128i *)(const void *)(ra + (size_t)k * 4));
        __m128i b = _mm_loadu_si128((const __m128i *)(const void *)(rb + (size_t)k * 4));
        __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a, z), W0), _mm_mullo_epi16(_mm_unpacklo_epi8(b, z), W1)), k128), 8);
        __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a, z), W0), _mm_mullo_epi16(_mm_unpackhi_epi8(b, z), W1)), k128), 8);
        _mm_storeu_si128((__m128i *)(void *)(v + (size_t)k * 4), _mm_packus_epi16(lo, hi));
    }
#endif
    for (k *= 4; k < n * 4; k++) v[k] = (uint8_t)((ra[k] * (256u - (unsigned)wy) + rb[k] * (unsigned)wy + 128u) >> 8);
}

/* n pixels, each the mix of row t (already mixed vertically; its first texel is column c0) at column
 * col[k] and col[k] + 1 (weights lw[k]: low 16 bits for the first, high for the second, summing to 256;
 * w0x4 and w1x4 hold the same weights four times per pixel), over d at opacity a256. Four pixels at a
 * time: their texels are read with two loads where the four columns follow each other (a scale near 1:
 * most of them). The same integers as cui__sw_blit_row. */
static void cui__sw_blit_mixed(uint8_t *d, const uint8_t *t, int c0, const int *col, const int32_t *lw, const uint16_t *w0x4,
                               const uint16_t *w1x4, int n, int a256) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i z = _mm_setzero_si128(), k128 = _mm_set1_epi16(128), k255 = _mm_set1_epi16(255), A = _mm_set1_epi16((short)a256);
    for (; k + 4 <= n; k += 4) {
        int c = col[k] - c0;
        __m128i p0, p1;
        if (col[k + 3] - c0 == c + 3) {
            p0 = _mm_loadu_si128((const __m128i *)(const void *)(t + (size_t)c * 4));
            p1 = _mm_loadu_si128((const __m128i *)(const void *)(t + (size_t)c * 4 + 4));
        } else {
            uint32_t q[8];
            for (int i = 0; i < 4; i++) {
                const uint8_t *s = t + (size_t)(col[k + i] - c0) * 4;
                memcpy(&q[i], s, 4); memcpy(&q[4 + i], s + 4, 4);
            }
            p0 = _mm_loadu_si128((const __m128i *)(const void *)q);
            p1 = _mm_loadu_si128((const __m128i *)(const void *)(q + 4));
        }
        const __m128i *w0 = (const __m128i *)(const void *)(w0x4 + (size_t)k * 4), *w1 = (const __m128i *)(const void *)(w1x4 + (size_t)k * 4);
        __m128i lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(p0, z), _mm_loadu_si128(w0)),
                                                                _mm_mullo_epi16(_mm_unpacklo_epi8(p1, z), _mm_loadu_si128(w1))), k128), 8);
        __m128i hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(p0, z), _mm_loadu_si128(w0 + 1)),
                                                                _mm_mullo_epi16(_mm_unpackhi_epi8(p1, z), _mm_loadu_si128(w1 + 1))), k128), 8);
        if (a256 < 256) {
            lo = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(lo, A), k128), 8);
            hi = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(hi, A), k128), 8);
        }
        __m128i alo = _mm_shufflehi_epi16(_mm_shufflelo_epi16(lo, 0xFF), 0xFF), ahi = _mm_shufflehi_epi16(_mm_shufflelo_epi16(hi, 0xFF), 0xFF);
        uint8_t *q = d + (size_t)k * 4;
        __m128i dp = _mm_loadu_si128((const __m128i *)(const void *)q);
        __m128i xl = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(dp, z), _mm_sub_epi16(k255, alo)), k128);
        __m128i xh = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(dp, z), _mm_sub_epi16(k255, ahi)), k128);
        xl = _mm_srli_epi16(_mm_add_epi16(xl, _mm_srli_epi16(xl, 8)), 8);
        xh = _mm_srli_epi16(_mm_add_epi16(xh, _mm_srli_epi16(xh, 8)), 8);
        _mm_storeu_si128((__m128i *)(void *)q, _mm_packus_epi16(_mm_add_epi16(lo, xl), _mm_add_epi16(hi, xh)));
    }
#else
    (void)w0x4; (void)w1x4;
#endif
    for (; k < n; k++) {
        const uint8_t *pa = t + (size_t)(col[k] - c0) * 4;
        unsigned w1 = (unsigned)lw[k] >> 16, w0 = (unsigned)lw[k] & 0xFFFFu, cv[4];
        for (int ch = 0; ch < 4; ch++) {
            unsigned v = (pa[ch] * w0 + pa[ch + 4] * w1 + 128u) >> 8;
            cv[ch] = a256 < 256 ? (v * (unsigned)a256 + 128u) >> 8 : v;
        }
        if (!cv[3]) continue;
        uint8_t *q = d + (size_t)k * 4;
        unsigned ia = 255u - cv[3];
        for (int ch = 0; ch < 4; ch++) {
            unsigned x = q[ch] * ia + 128u;
            unsigned v = cv[ch] + ((x + (x >> 8)) >> 8);
            q[ch] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* The same with AVX2, eight pixels at a time, where the processor has it (chosen at run time; GCC and Clang on
 * x86-64). AVX2 unpacks and packs within each 128 bit half, so the weights are loaded as (pixels k, k + 1 | k + 4,
 * k + 5) and (k + 2, k + 3 | k + 6, k + 7), which is how the texels and the frame's pixels unpack. */
#if defined(CUI__SW_SSE2) && (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#  include <immintrin.h>
#  define CUI__SW_AVX2 1
__attribute__((target("avx2"))) static int cui__sw_blit_mixed_avx2(uint8_t *d, const uint8_t *t, int c0, const int *col, const uint16_t *w0x4,
                                                                    const uint16_t *w1x4, int n, int a256) {
    int k = 0;
    const __m256i z = _mm256_setzero_si256(), k128 = _mm256_set1_epi16(128), k255 = _mm256_set1_epi16(255), A = _mm256_set1_epi16((short)a256);
    for (; k + 8 <= n; k += 8) {
        int c = col[k] - c0;
        __m256i p0, p1;
        int span = col[k + 7] - c0 - c;
        if (span == 7) {
            p0 = _mm256_loadu_si256((const __m256i *)(const void *)(t + (size_t)c * 4));
            p1 = _mm256_loadu_si256((const __m256i *)(const void *)(t + (size_t)c * 4 + 4));
        } else if (span >= 0 && span <= 14) {
            /* the texels c .. c + 15 in two registers, and each pixel's (and its neighbor's) picked from them; t has
             * 16 texels to spare after its last one (cui__sw_layer_blit, cui__sw_layer_draw) */
            const __m256i lo8 = _mm256_loadu_si256((const __m256i *)(const void *)(t + (size_t)c * 4));
            const __m256i hi8 = _mm256_loadu_si256((const __m256i *)(const void *)(t + (size_t)c * 4 + 32));
            const __m256i seven = _mm256_set1_epi32(7);
            __m256i ix = _mm256_sub_epi32(_mm256_loadu_si256((const __m256i *)(const void *)(col + k)), _mm256_set1_epi32(c0 + c));
            __m256i ix1 = _mm256_add_epi32(ix, _mm256_set1_epi32(1));
            p0 = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(lo8, ix), _mm256_permutevar8x32_epi32(hi8, ix), _mm256_cmpgt_epi32(ix, seven));
            p1 = _mm256_blendv_epi8(_mm256_permutevar8x32_epi32(lo8, ix1), _mm256_permutevar8x32_epi32(hi8, ix1), _mm256_cmpgt_epi32(ix1, seven));
        } else {
            uint32_t q[16];
            for (int i = 0; i < 8; i++) {
                const uint8_t *s = t + (size_t)(col[k + i] - c0) * 4;
                memcpy(&q[i], s, 4); memcpy(&q[8 + i], s + 4, 4);
            }
            p0 = _mm256_loadu_si256((const __m256i *)(const void *)q);
            p1 = _mm256_loadu_si256((const __m256i *)(const void *)(q + 8));
        }
#define CUI__W2(w, a, b) _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i *)(const void *)((w) + (size_t)(a) * 4))), \
                                                 _mm_loadu_si128((const __m128i *)(const void *)((w) + (size_t)(b) * 4)), 1)
        __m256i w0l = CUI__W2(w0x4, k, k + 4), w0h = CUI__W2(w0x4, k + 2, k + 6), w1l = CUI__W2(w1x4, k, k + 4), w1h = CUI__W2(w1x4, k + 2, k + 6);
#undef CUI__W2
        __m256i lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(p0, z), w0l),
                                                                         _mm256_mullo_epi16(_mm256_unpacklo_epi8(p1, z), w1l)), k128), 8);
        __m256i hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(p0, z), w0h),
                                                                         _mm256_mullo_epi16(_mm256_unpackhi_epi8(p1, z), w1h)), k128), 8);
        if (a256 < 256) {
            lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(lo, A), k128), 8);
            hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_mullo_epi16(hi, A), k128), 8);
        }
        __m256i alo = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(lo, 0xFF), 0xFF), ahi = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(hi, 0xFF), 0xFF);
        uint8_t *q = d + (size_t)k * 4;
        __m256i dp = _mm256_loadu_si256((const __m256i *)(const void *)q);
        __m256i xl = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(dp, z), _mm256_sub_epi16(k255, alo)), k128);
        __m256i xh = _mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(dp, z), _mm256_sub_epi16(k255, ahi)), k128);
        xl = _mm256_srli_epi16(_mm256_add_epi16(xl, _mm256_srli_epi16(xl, 8)), 8);
        xh = _mm256_srli_epi16(_mm256_add_epi16(xh, _mm256_srli_epi16(xh, 8)), 8);
        _mm256_storeu_si256((__m256i *)(void *)q, _mm256_packus_epi16(_mm256_add_epi16(lo, xl), _mm256_add_epi16(hi, xh)));
    }
    return k;
}
__attribute__((target("avx2"))) static void cui__sw_mix_rows_avx2(uint8_t *v, const uint8_t *ra, const uint8_t *rb, int n, int wy, int *done) {
    int k = 0;
    const __m256i z = _mm256_setzero_si256(), k128 = _mm256_set1_epi16(128);
    const __m256i W0 = _mm256_set1_epi16((short)(256 - wy)), W1 = _mm256_set1_epi16((short)wy);
    for (; k + 8 <= n; k += 8) {
        __m256i a = _mm256_loadu_si256((const __m256i *)(const void *)(ra + (size_t)k * 4));
        __m256i b = _mm256_loadu_si256((const __m256i *)(const void *)(rb + (size_t)k * 4));
        __m256i lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpacklo_epi8(a, z), W0), _mm256_mullo_epi16(_mm256_unpacklo_epi8(b, z), W1)), k128), 8);
        __m256i hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(_mm256_unpackhi_epi8(a, z), W0), _mm256_mullo_epi16(_mm256_unpackhi_epi8(b, z), W1)), k128), 8);
        _mm256_storeu_si256((__m256i *)(void *)(v + (size_t)k * 4), _mm256_packus_epi16(lo, hi));
    }
    *done = k;
}
static int cui__sw_avx2_state;   /* 0 not asked yet, 1 has AVX2, -1 not */
static bool cui__sw_has_avx2(void) {
    if (!cui__sw_avx2_state) {
        __builtin_cpu_init();
        cui__sw_avx2_state = __builtin_cpu_supports("avx2") ? 1 : -1;
    }
    return cui__sw_avx2_state > 0;
}
#endif

/* n pixels, each the bilinear mix of the bitmap at column col[k] and col[k] + 1 (weights lw[k]: low 16
 * bits for the first, high for the second, summing to 256) and of rows ra and rb (wy for rb, 0..256),
 * over d at opacity a256: the same integers in both paths */
static void cui__sw_blit_row(uint8_t *d, const uint8_t *ra, const uint8_t *rb, const int *col, const int32_t *lw, int n, int wy, int a256) {
    int k = 0;
#if defined(CUI__SW_SSE2)
    const __m128i z = _mm_setzero_si128(), k128 = _mm_set1_epi16(128), k255 = _mm_set1_epi16(255), A = _mm_set1_epi16((short)a256);
    const __m128i W0 = _mm_set1_epi16((short)(256 - wy)), W1 = _mm_set1_epi16((short)wy), h32 = _mm_set1_epi32(128);
    for (; k + 2 <= n; k += 2) {
        __m128i a = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)(const void *)(ra + (size_t)col[k] * 4)),
                                       _mm_loadl_epi64((const __m128i *)(const void *)(ra + (size_t)col[k + 1] * 4)));
        __m128i lo = _mm_unpacklo_epi8(a, z), hi = _mm_unpackhi_epi8(a, z);
        if (wy) {
            __m128i b = _mm_unpacklo_epi64(_mm_loadl_epi64((const __m128i *)(const void *)(rb + (size_t)col[k] * 4)),
                                           _mm_loadl_epi64((const __m128i *)(const void *)(rb + (size_t)col[k + 1] * 4)));
            lo = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(lo, W0), _mm_mullo_epi16(_mm_unpacklo_epi8(b, z), W1)), k128), 8);
            hi = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(hi, W0), _mm_mullo_epi16(_mm_unpackhi_epi8(b, z), W1)), k128), 8);
        }
        /* (first, second) per channel, then one multiply-add per pixel */
        lo = _mm_unpacklo_epi16(lo, _mm_srli_si128(lo, 8));
        hi = _mm_unpacklo_epi16(hi, _mm_srli_si128(hi, 8));
        __m128i m0 = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(lo, _mm_set1_epi32(lw[k])), h32), 8);
        __m128i m1 = _mm_srli_epi32(_mm_add_epi32(_mm_madd_epi16(hi, _mm_set1_epi32(lw[k + 1])), h32), 8);
        __m128i c = _mm_packs_epi32(m0, m1);
        if (a256 < 256) c = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(c, A), k128), 8);
        __m128i al = _mm_shufflehi_epi16(_mm_shufflelo_epi16(c, 0xFF), 0xFF);
        if (_mm_movemask_epi8(_mm_cmpeq_epi16(al, z)) == 0xFFFF) continue;   /* both clear */
        uint8_t *q = d + (size_t)k * 4;
        __m128i x = _mm_mullo_epi16(_mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)q), z), _mm_sub_epi16(k255, al));
        x = _mm_add_epi16(x, k128);
        x = _mm_srli_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), 8);
        _mm_storel_epi64((__m128i *)(void *)q, _mm_packus_epi16(_mm_add_epi16(c, x), z));
    }
#endif
    for (; k < n; k++) {
        const uint8_t *pa = ra + (size_t)col[k] * 4, *pb = rb + (size_t)col[k] * 4;
        unsigned w1 = (unsigned)lw[k] >> 16, w0 = (unsigned)lw[k] & 0xFFFFu, cv[4];
        for (int ch = 0; ch < 4; ch++) {
            unsigned t0 = pa[ch], t1 = pa[ch + 4];
            if (wy) {
                t0 = (t0 * (256u - (unsigned)wy) + pb[ch] * (unsigned)wy + 128u) >> 8;
                t1 = (t1 * (256u - (unsigned)wy) + pb[ch + 4] * (unsigned)wy + 128u) >> 8;
            }
            unsigned v = (t0 * w0 + t1 * w1 + 128u) >> 8;
            cv[ch] = a256 < 256 ? (v * (unsigned)a256 + 128u) >> 8 : v;
        }
        if (!cv[3]) continue;
        uint8_t *q = d + (size_t)k * 4;
        unsigned ia = 255u - cv[3];
        for (int ch = 0; ch < 4; ch++) {
            unsigned x = q[ch] * ia + 128u;
            unsigned v = cv[ch] + ((x + (x >> 8)) >> 8);
            q[ch] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
}

/* layer L blended over the pixels [cx0, cx1) x [cy0, cy1) of the frame */
static void cui__sw_layer_blit(const cui__sw_job *job, const cui__sw_layer *L, int cx0, int cy0, int cx1, int cy1) {
    if (L->a256 <= 0 || !L->px) return;
    float s = L->s;
    int xa = CUI_MAX(cx0, (int)floorf(L->dx + s * (float)L->x0)), xb = CUI_MIN(cx1, (int)ceilf(L->dx + s * (float)(L->x0 + L->w)));
    int ya = CUI_MAX(cy0, (int)floorf(L->dy + s * (float)L->y0)), yb = CUI_MIN(cy1, (int)ceilf(L->dy + s * (float)(L->y0 + L->h)));
    if (xa >= xb || ya >= yb) return;
    size_t bw = (size_t)L->w * 4;
    if (s == 1.0f && L->dx == floorf(L->dx) && L->dy == floorf(L->dy)) {
        /* whole pixels: each pixel is one texel */
        int ox = (int)L->dx + L->x0, oy = (int)L->dy + L->y0;
        for (int y = ya; y < yb; y++) {
            int ty = y - oy;
            if (ty < 0 || ty >= L->h) continue;
            int a = CUI_MAX(xa, L->span[2 * ty] + ox), b = CUI_MIN(xb, L->span[2 * ty + 1] + ox);
            if (a < b) cui__sw_over_row(job->pixels + (size_t)y * (size_t)job->stride + (size_t)a * 4, L->px + (size_t)ty * bw + (size_t)(a - ox) * 4, b - a, L->a256);
        }
        return;
    }
    /* scaled: the frame's tables (cui__sw_layer_tables) */
    ya = CUI_MAX(ya, L->ty0); yb = CUI_MIN(yb, L->ty0 + L->tny);
    /* the two rows mixed first (once per texel, not per sample), in parts of at most 256 pixels: a part reads at most
     * 256 / 0.5 + 2 texels (scales from 0.5) */
    enum { PART = 256 };
    uint8_t mixed[(2 * PART + 8 + 16) * 4];   /* (16 texels to spare: cui__sw_blit_mixed_avx2 reads them) */
    for (int y = ya; y < yb; y++) {
        const int *t = L->trow + 4 * (y - L->ty0);
        int a = CUI_MAX(xa, t[2]), b = CUI_MIN(xb, t[3]);
        if (a >= b) continue;
        const uint8_t *ra = L->px + (size_t)t[0] * bw;
        if (!L->tw4 || cui__sw_blend_level == 0) {
            int i = a - L->tx0;
            cui__sw_blit_row(job->pixels + (size_t)y * (size_t)job->stride + (size_t)a * 4, ra, ra + bw, L->tcol + i, L->tw + i, b - a, t[1], L->a256);
            continue;
        }
        for (int pa = a; pa < b; pa += PART) {
            int pb = CUI_MIN(b, pa + PART), i = pa - L->tx0;
            const int *col = L->tcol + i;
            const uint8_t *row = ra;
            int c0 = 0;
            if (t[1]) {   /* columns [c0, last + 1] of the two rows */
                c0 = col[0];
                int nc = col[pb - pa - 1] + 2 - c0;
                if (nc > 2 * PART + 8) { cui__sw_blit_row(job->pixels + (size_t)y * (size_t)job->stride + (size_t)pa * 4, ra, ra + bw, col, L->tw + i, pb - pa, t[1], L->a256); continue; }
                int done = 0;
#if defined(CUI__SW_AVX2)
                if (cui__sw_avx2_state > 0 && cui__sw_blend_level != 1) cui__sw_mix_rows_avx2(mixed, ra + (size_t)c0 * 4, ra + bw + (size_t)c0 * 4, nc, t[1], &done);
#endif
                cui__sw_mix_rows(mixed + (size_t)done * 4, ra + (size_t)(c0 + done) * 4, ra + bw + (size_t)(c0 + done) * 4, nc - done, t[1]);
                memset(mixed + (size_t)nc * 4, 0, 64);
                row = mixed;
            }
            uint8_t *dst = job->pixels + (size_t)y * (size_t)job->stride + (size_t)pa * 4;
            const uint16_t *w0x4 = L->tw4 + (size_t)i * 4, *w1x4 = L->tw4 + (size_t)L->tcap_x * 4 + (size_t)i * 4;
            int k = 0;
#if defined(CUI__SW_AVX2)
            if (cui__sw_avx2_state > 0 && cui__sw_blend_level != 1) k = cui__sw_blit_mixed_avx2(dst, row, c0, col, w0x4, w1x4, pb - pa, L->a256);
#endif
            cui__sw_blit_mixed(dst + (size_t)k * 4, row, c0, col + k, L->tw + i + k, w0x4 + (size_t)k * 4, w1x4 + (size_t)k * 4, pb - pa - k, L->a256);
        }
    }
}

typedef struct {
    cui_sw_renderer *r;
    const cui_draw_data *dd;
    uint8_t *pixels;
    int width, height, stride;
    cui_sw_format fmt;
    struct cui__sw_scratch *sc;   /* drawn on one thread: the scratch the jobs share, else NULL */
    const uint8_t *umode;         /* per tile: what is under a bitmap saved (1) or copied back (2) (cui__sw_under_plan), or NULL */
    int tw;
} cui__sw_frame;

/* primitive p drawn inside [x0, x1) x [y0, y1) */
static void cui__sw_draw_prim(cui__sw_job *job, cui_sw_renderer *r, const cui_draw_data *dd, const cui__sw_prim *p, int x0, int y0, int x1, int y1) {
    int cx0 = CUI_MAX(x0, p->cx0), cy0 = CUI_MAX(y0, p->cy0), cx1 = CUI_MIN(x1, p->cx1), cy1 = CUI_MIN(y1, p->cy1);
    if (cx0 >= cx1 || cy0 >= cy1 || p->bx1 + 1 < (float)cx0 || p->bx0 - 1 > (float)cx1) return;
    if (p->layer) { cui__sw_layer_blit(job, &r->layers[p->layer - 1], cx0, cy0, cx1, cy1); return; }
    const cui_draw_cmd *cmd = &dd->commands[p->cmd];
    cui_texture tid = cmd->texture ? cmd->texture : dd->font_atlas;
    int slot = (int)tid - 1;
    const cui__sw_tex *tex = (slot >= 0 && slot < r->tex_count && r->tex[slot].used) ? &r->tex[slot] : NULL;
    const uint32_t *ix = dd->indices + cmd->index_offset + p->k;
    if (p->quad) {
        const cui_vertex *q = &dd->vertices[ix[0]];
        if (cui__raster_quad(job, &q[0], &q[1], &q[2], &q[3], tex, cx0, cy0, cx1, cy1)) return;
        cui__raster_tri(job, &dd->vertices[ix[0]], &dd->vertices[ix[1]], &dd->vertices[ix[2]], tex, cx0, cy0, cx1, cy1);
        ix += 3;
    }
    cui__raster_tri(job, &dd->vertices[ix[0]], &dd->vertices[ix[1]], &dd->vertices[ix[2]], tex, cx0, cy0, cx1, cy1);
}

/* one part of the frame: cleared, then everything over its tile row drawn in order */
static void cui__sw_paint_job(void *ctx, int j) {
    const cui__sw_frame *f = ctx;
    cui_sw_renderer *r = f->r;
    const cui_draw_data *dd = f->dd;
    const int *jb = r->jobs + (size_t)j * 5;
    int x0 = jb[0], y0 = jb[1], x1 = jb[2], y1 = jb[3], row = jb[4];
    cui_color cc = dd->clear_color;
    uint8_t c4[4] = { cc.r, cc.g, cc.b, r->keep_alpha ? cc.a : 255 };
    if (f->fmt == CUI_SW_BGRA) { c4[0] = cc.b; c4[2] = cc.r; }
    /* The last shape that paints all of this part opaque whatever is under it (occ: the inside of an
     * opaque rectangle or of an opaque picture) hides the clear and everything drawn before it:
     * drawing starts there. */
    uint32_t from = r->row_start[row], to = r->row_start[row + 1];
    bool hidden = false;
    for (uint32_t i = to; i-- > from;) {
        const cui__sw_prim *p = &r->prims[r->row_items[i]];
        if (!p->occ || p->cx0 > x0 || p->cy0 > y0 || p->cx1 < x1 || p->cy1 < y1) continue;
        if ((p->oa[0] <= x0 && x1 <= p->oa[2] && p->oa[1] <= y0 && y1 <= p->oa[3]) ||
            (p->ob[0] <= x0 && x1 <= p->ob[2] && p->ob[1] <= y0 && y1 <= p->ob[3])) { from = i; hidden = true; break; }
    }
    /* Tiles under a group's bitmap (cui__sw_under_plan). iL: the first bitmap over this part; it is the first over
     * each of its tiles it touches (one before it there would be over this part too). Copied back tiles get what
     * was saved under it and skip what comes before it; saved ones are copied out when drawing reaches it. */
    uint32_t iL = to;
    uint8_t um[512 / CUI__SW_TILE + 1] = { 0 };
    int tx0 = x0 / CUI__SW_TILE, ntl = (x1 - x0 + CUI__SW_TILE - 1) / CUI__SW_TILE;
    bool any_copy = false, any_save = false;
    if (f->umode && ntl <= (int)sizeof um) {
        bool any = false;
        for (int k = 0; k < ntl; k++) any |= f->umode[row * f->tw + tx0 + k] != 0;
        for (uint32_t i = any ? from : to; i < to; i++) {
            const cui__sw_prim *p = &r->prims[r->row_items[i]];
            if (!p->layer) continue;
            int cx0 = CUI_MAX(x0, p->cx0), cy0 = CUI_MAX(y0, p->cy0), cx1 = CUI_MIN(x1, p->cx1), cy1 = CUI_MIN(y1, p->cy1);
            if (cx0 >= cx1 || cy0 >= cy1 || p->bx1 + 1 < (float)cx0 || p->bx0 - 1 > (float)cx1) continue;
            iL = i;
            break;
        }
        if (iL < to) {
            const cui__sw_prim *p = &r->prims[r->row_items[iL]];
            for (int k = 0; k < ntl; k++) {
                int a = x0 + k * CUI__SW_TILE, b = CUI_MIN(x1, a + CUI__SW_TILE);
                int cx0 = CUI_MAX(a, p->cx0), cy0 = CUI_MAX(y0, p->cy0), cx1 = CUI_MIN(b, p->cx1), cy1 = CUI_MIN(y1, p->cy1);
                bool touches = !(cx0 >= cx1 || cy0 >= cy1 || p->bx1 + 1 < (float)cx0 || p->bx0 - 1 > (float)cx1);
                um[k] = touches ? f->umode[row * f->tw + tx0 + k] : 0;
                any_copy |= um[k] == 2;
                any_save |= um[k] == 1;
            }
        }
    }
    for (int k = 0; k < ntl && (any_copy || any_save); k++) {   /* copied back: what is under the bitmap, as saved */
        if (um[k] != 2) continue;
        int a = x0 + k * CUI__SW_TILE, b = CUI_MIN(x1, a + CUI__SW_TILE), t = row * f->tw + tx0 + k;
        const uint8_t *s = r->upx + (size_t)r->uslot[t] * CUI__SW_TILE * CUI__SW_TILE * 4;
        for (int y = y0; y < y1; y++) memcpy(f->pixels + (size_t)y * (size_t)f->stride + (size_t)a * 4, s + (size_t)(y - y0) * CUI__SW_TILE * 4, (size_t)(b - a) * 4);
    }
    if (from > iL) any_copy = any_save = false;   /* an opaque shape after the bitmap hides all of this part */
    /* the parts of the job drawn before the bitmap: all of it, or the runs of tiles not copied back */
    int runs[2 * (512 / CUI__SW_TILE + 1)], nruns = 0;
    if (!any_copy) { runs[0] = x0; runs[1] = x1; nruns = 1; }
    else
        for (int k = 0; k < ntl;) {
            if (um[k] == 2) { k++; continue; }
            int a = k;
            while (k < ntl && um[k] != 2) k++;
            runs[2 * nruns] = x0 + a * CUI__SW_TILE; runs[2 * nruns + 1] = CUI_MIN(x1, x0 + k * CUI__SW_TILE); nruns++;
        }
    for (int y = y0; y < y1 && !hidden; y++)
        for (int q = 0; q < nruns; q++) cui__sw_fill(f->pixels + (size_t)y * (size_t)f->stride + (size_t)runs[2 * q] * 4, runs[2 * q + 1] - runs[2 * q], c4);
    cui__sw_job job;
    struct cui__sw_scratch own, *sc = f->sc;   /* the frame's when one thread draws it all, else this job's */
    if (!sc) { sc = &own; own.key = NULL; own.wtab.ok = false; }
    job.sc = sc;
    job.wtab = &sc->wtab;
    job.dd = dd; job.r = r; job.pixels = f->pixels;
    job.width = f->width; job.height = f->height; job.stride = f->stride; job.fmt = f->fmt;
    job.x0 = x0; job.y0 = y0; job.x1 = x1; job.y1 = y1;
    for (uint32_t i = from; i < to; i++) {
        if (any_save && i == iL)   /* what is under the bitmap: saved for the next frames */
            for (int k = 0; k < ntl; k++) {
                if (um[k] != 1) continue;
                int a = x0 + k * CUI__SW_TILE, b = CUI_MIN(x1, a + CUI__SW_TILE), t = row * f->tw + tx0 + k;
                uint8_t *s = r->upx + (size_t)r->uslot[t] * CUI__SW_TILE * CUI__SW_TILE * 4;
                for (int y = y0; y < y1; y++) memcpy(s + (size_t)(y - y0) * CUI__SW_TILE * 4, f->pixels + (size_t)y * (size_t)f->stride + (size_t)a * 4, (size_t)(b - a) * 4);
                r->uvalid[t] = 1;
            }
        const cui__sw_prim *p = &r->prims[r->row_items[i]];
        if (any_copy && i < iL)   /* before the bitmap: only where nothing was copied back */
            for (int q = 0; q < nruns; q++) cui__sw_draw_prim(&job, r, dd, p, runs[2 * q], y0, runs[2 * q + 1], y1);
        else
            cui__sw_draw_prim(&job, r, dd, p, x0, y0, x1, y1);
    }
}

/* Moves the pixels of the tiles marked 1 in what from shift pixels higher (lower when
 * negative), in tile columns tx0 to tx1: rows go in the order that reads each source row before
 * it is written. Columns do not touch each other, so threads can take a few each. */
typedef struct { uint8_t *px; int width, height, stride, tw, shift; const uint8_t *what; } cui__sw_mover;
static void cui__sw_move_job(void *ctx, int j) {
    const cui__sw_mover *m = ctx;
    int cols = CUI_MAX(1, 256 / CUI__SW_TILE), tx0 = j * cols, tx1 = CUI_MIN(m->tw, tx0 + cols);
    int y = m->shift > 0 ? m->height - 1 : 0, end = m->shift > 0 ? -1 : m->height, step = m->shift > 0 ? -1 : 1;
    for (; y != end; y += step) {
        const uint8_t *w = m->what + (size_t)(y / CUI__SW_TILE) * (size_t)m->tw;
        uint8_t *dst = m->px + (size_t)y * (size_t)m->stride;
        const uint8_t *src = m->px + (size_t)(y - m->shift) * (size_t)m->stride;
        for (int tx = tx0; tx < tx1;) {
            if (w[tx] != 1) { tx++; continue; }
            int a = tx;
            while (tx < tx1 && w[tx] == 1) tx++;
            size_t x0 = (size_t)a * CUI__SW_TILE, x1 = (size_t)CUI_MIN(m->width, tx * CUI__SW_TILE);
            memcpy(dst + x0 * 4, src + x0 * 4, (x1 - x0) * 4);
        }
    }
}

/* Tiles moved by a second or third distance go through a spare buffer: their sources are read
 * before the main move writes anything (put false), and written after it (put true). */
static void cui__sw_second(cui_sw_renderer *r, uint8_t *px, int stride, int width, int height, int tw, int th, bool put) {
    size_t at = 0, tile_bytes = (size_t)CUI__SW_TILE * CUI__SW_TILE * 4;
    if (!put) {
        size_t n = 0;
        for (int i = 0; i < tw * th; i++) n += r->what[i] >= 3;
        if (n * tile_bytes > r->spare_cap) { r->spare_cap = n * tile_bytes; r->spare = cui__sw_xrealloc(r->spare, r->spare_cap); }
    }
    for (int ty = 0; ty < th; ty++)
        for (int tx = 0; tx < tw; tx++) {
            uint8_t w = r->what[ty * tw + tx];
            if (w < 3) continue;
            int shift = r->shifts[w - 2];
            int x0 = tx * CUI__SW_TILE, x1 = CUI_MIN(width, x0 + CUI__SW_TILE), y0 = ty * CUI__SW_TILE, y1 = CUI_MIN(height, y0 + CUI__SW_TILE);
            size_t row = (size_t)(x1 - x0) * 4;
            for (int y = y0; y < y1; y++, at += row) {
                if (put) memcpy(px + (size_t)y * (size_t)stride + (size_t)x0 * 4, r->spare + at, row);
                else memcpy(r->spare + at, px + (size_t)(y - shift) * (size_t)stride + (size_t)x0 * 4, row);
            }
        }
}

/* A frame goes to the worker threads only when one thread would take several milliseconds over
 * it (a whole big window). Below that they cost more CPU than they save: waking them, and two
 * threads on one core each run slower (a popover opening in a browser window, about 300,000 pixels
 * a frame, took twice the process CPU on 8 threads as on one). */
#define CUI__SW_THREADS_DRAW 3000000.0   /* pixels drawn (a popover at scale 2 is about 1,200,000: 5 ms on one thread) */
#define CUI__SW_THREADS_MOVE 4000000.0   /* pixels moved (a scroll) */
static void cui__sw_threads_run(cui_sw_renderer *r, int njobs, void (*fn)(void *, int), void *ctx) {
    if (r->threads <= 1 || njobs <= 1) { for (int j = 0; j < njobs; j++) fn(ctx, j); return; }
    if (!r->pool || r->pool->want != r->threads - 1) { cui__sw_pool_destroy(r->pool); r->pool = cui__sw_pool_create(r->threads - 1); }
    cui__sw_pool_run(r->pool, njobs, fn, ctx);
}

/* What happens to each tile this frame (r->what): 0 it stays, 1 its pixels move by the main
 * distance (returned), 3 and 4 they move by the second or third (r->shifts), 2 it is drawn. */
static int cui__sw_classify(cui_sw_renderer *r, const cui_draw_data *dd, int tw, int th, int width, int height, bool full, bool *second) {
    int nt = tw * th, shift = 0;
    uint8_t *what = r->what;
    *second = false;
    if (r->incremental) {
        uint64_t seed = cui__sw_hash_bytes(&dd->clear_color, sizeof dd->clear_color, 1469598103934665603ull);
        cui__sw_grid(r->prims, r->nprims, seed, r->tiles, r->force, r->nlayer_prims ? r->under : NULL, r->nlayer_prims ? r->ubusy : NULL,
                     tw, th, width, height, 0);
        if (!full) {
            bool any = false;
            for (int i = 0; i < nt; i++) { what[i] = (r->tiles[i] != r->prev[i] || r->force[i]) ? 2 : 0; any |= what[i] != 0; }
            /* a scroll: a changed tile that holds what the last frame drew some distance away is moved */
            int cand[3], ncand = any && r->npprims ? cui__sw_vote(r, height, cand) : 0;
            for (int c = 0; c < ncand; c++) {
                cui__sw_grid(r->pprims, r->npprims, seed, r->moved, NULL, NULL, NULL, tw, th, width, height, -cand[c]);
                int moves = 0;
                uint8_t mark = c == 0 ? 1 : (uint8_t)(2 + c);
                for (int ty = 0; ty < th; ty++) {
                    int y0 = ty * CUI__SW_TILE - cand[c], y1 = CUI_MIN(height, (ty + 1) * CUI__SW_TILE) - cand[c];
                    if (y0 < 0 || y1 > height) continue;   /* it would come from outside the frame */
                    for (int i = ty * tw; i < (ty + 1) * tw; i++)
                        if (what[i] == 2 && !r->force[i] && r->moved[i] == r->tiles[i]) { what[i] = mark; moves++; }
                }
                if (!moves) continue;
                r->shifts[c] = cand[c];
                if (c == 0) shift = cand[c];
                else *second = true;
            }
        }
    }
    if (full) memset(what, 2, (size_t)nt);
    return shift;
}

/* The tiles drawn this frame that have a group's bitmap over them: what is under it copied back
 * from its slot (2) when it is what was saved there, else saved into one (1). At most
 * CUI__SW_LAYER_PX pixels of slots; past that a tile is drawn as always (0). */
#define CUI__SW_USLOTS (CUI__SW_LAYER_PX / (CUI__SW_TILE * CUI__SW_TILE))
static bool cui__sw_under_plan(cui_sw_renderer *r, int tw, int th, int width, int height, cui_sw_format fmt) {
    int nt = tw * th;
    if (!r->incremental || !r->nlayer_prims) return false;
    if (r->utw != tw || r->uw != width || r->uh != height || r->ufmt != fmt) {
        cui__sw_under_drop(r, false);
        r->utw = tw; r->uw = width; r->uh = height; r->ufmt = fmt;
    }
    /* the slots this frame needs, made at once (growing them one by one copied megabytes per animation) */
    int need = r->ucount;
    for (int t = 0; t < nt; t++) need += r->what[t] == 2 && (r->under[t] & 1) && !r->force[t] && r->uslot[t] < 0;
    if (need > CUI__SW_USLOTS) need = CUI__SW_USLOTS;
    if (need > r->ucap) {
        int cap = CUI_MIN(CUI__SW_USLOTS, need + need / 4 + 8);
        uint8_t *p = realloc(r->upx, (size_t)cap * CUI__SW_TILE * CUI__SW_TILE * 4);
        if (p) {
            r->upx = p; r->ucap = cap;
            r->ubytes = (size_t)cap * CUI__SW_TILE * CUI__SW_TILE * 4;
            if (r->layer_bytes + r->ubytes > r->layer_peak) r->layer_peak = r->layer_bytes + r->ubytes;
        }
    }
    bool any = false;
    for (int t = 0; t < nt; t++) {
        uint8_t m = 0;
        if (r->what[t] == 2 && (r->under[t] & 1) && !r->force[t]) {
            if (r->uslot[t] >= 0 && r->uvalid[t] && r->uhash[t] == r->under[t]) m = 2;
            else {
                if (r->uslot[t] < 0 && r->ucount < r->ucap) r->uslot[t] = r->ucount++;
                if (r->uslot[t] >= 0) { r->uhash[t] = r->under[t]; r->uvalid[t] = 0; m = 1; }
            }
        }
        r->umode[t] = m;
        any |= m != 0;
        if (m == 2) r->urestored++;
        else if (m == 1) r->usaved++;
    }
    return any;
}

/* The tiles to draw as jobs (per tile row, runs of them cut into parts of at most 512 pixels
 * the threads share), the damage rectangle, and the shares drawn and moved. Returns the jobs. */
static int cui__sw_plan(cui_sw_renderer *r, int tw, int th, int width, int height, double *area) {
    const uint8_t *what = r->what;
    r->jobs = cui__sw_grow(r->jobs, &r->jobs_cap, 5 * (tw * th + th), sizeof *r->jobs);
    int njobs = 0, nmoved = 0;
    *area = 0;
    r->damage[0] = width; r->damage[1] = height; r->damage[2] = 0; r->damage[3] = 0;
    for (int ty = 0; ty < th; ty++) {
        int y0 = ty * CUI__SW_TILE, y1 = CUI_MIN(height, y0 + CUI__SW_TILE);
        for (int tx = 0; tx < tw;) {
            if (!what[ty * tw + tx]) { tx++; continue; }
            int a = tx;
            while (tx < tw && what[ty * tw + tx]) tx++;
            r->damage[0] = CUI_MIN(r->damage[0], a * CUI__SW_TILE); r->damage[1] = CUI_MIN(r->damage[1], y0);
            r->damage[2] = CUI_MAX(r->damage[2], CUI_MIN(width, tx * CUI__SW_TILE)); r->damage[3] = CUI_MAX(r->damage[3], y1);
            for (int b = a; b < tx;) {
                if (what[ty * tw + b] != 2) { nmoved++; b++; continue; }
                int c = b;
                while (c < tx && what[ty * tw + c] == 2 && (c - b) * CUI__SW_TILE < 512) c++;
                int *jb = r->jobs + (size_t)njobs++ * 5;
                jb[0] = b * CUI__SW_TILE; jb[1] = y0; jb[2] = CUI_MIN(width, c * CUI__SW_TILE); jb[3] = y1; jb[4] = ty;
                *area += (double)(jb[2] - jb[0]) * (y1 - y0);
                b = c;
            }
        }
    }
    r->last_drawn = (float)(*area / ((double)width * height));
    r->last_moved = (float)nmoved * CUI__SW_TILE * CUI__SW_TILE / ((float)width * height);
    return njobs;
}

void cui_sw_render(cui_sw_renderer *r, const cui_draw_data *dd, void *pixels, int width, int height, int stride, cui_sw_format fmt) {
    if (!r || !dd || !pixels || width <= 0 || height <= 0) return;
#if defined(CUI__SW_AVX2)
    if (dd->groups && dd->group_count) (void)cui__sw_has_avx2();   /* asked here, before any worker thread reads it */
#endif
    int tw = (width + CUI__SW_TILE - 1) / CUI__SW_TILE, th = (height + CUI__SW_TILE - 1) / CUI__SW_TILE, nt = tw * th;
    if (nt > r->tiles_cap) {
        r->tiles_cap = nt;
        r->tiles = cui__sw_xrealloc(r->tiles, sizeof(uint64_t) * (size_t)nt);
        r->prev = cui__sw_xrealloc(r->prev, sizeof(uint64_t) * (size_t)nt);
        r->moved = cui__sw_xrealloc(r->moved, sizeof(uint64_t) * (size_t)nt);
        r->force = cui__sw_xrealloc(r->force, (size_t)nt);
        r->what = cui__sw_xrealloc(r->what, (size_t)nt);
        r->under = cui__sw_xrealloc(r->under, sizeof(uint64_t) * (size_t)nt);
        r->uhash = cui__sw_xrealloc(r->uhash, sizeof(uint64_t) * (size_t)nt);
        r->uslot = cui__sw_xrealloc(r->uslot, sizeof(int) * (size_t)nt);
        r->uvalid = cui__sw_xrealloc(r->uvalid, (size_t)nt);
        r->umode = cui__sw_xrealloc(r->umode, (size_t)nt);
        r->ubusy = cui__sw_xrealloc(r->ubusy, (size_t)nt);
        cui__sw_under_drop(r, false);
        r->last_px = NULL;
    }
    cui__sw_layers_prepare(r, dd, fmt);
    cui__sw_build(r, dd, tw, th);
    bool full = !r->incremental || pixels != r->last_px || width != r->last_w || height != r->last_h || stride != r->last_stride ||
                fmt != r->last_fmt || r->changed_overflow;
    bool second;
    int shift = cui__sw_classify(r, dd, tw, th, width, height, full, &second);
    r->last_px = pixels; r->last_w = width; r->last_h = height; r->last_stride = stride; r->last_fmt = fmt;
    r->nchanged = 0; r->changed_overflow = false;
    r->last_shift = shift;
    /* moves: tiles moved by the other distances keep their source pixels aside first */
    if (second) cui__sw_second(r, pixels, stride, width, height, tw, th, false);
    if (shift) {
        cui__sw_mover m = { pixels, width, height, stride, tw, shift, r->what };
        int cols = CUI_MAX(1, 256 / CUI__SW_TILE), njobs = (tw + cols - 1) / cols, nmove = 0;
        for (int i = 0; i < nt; i++) nmove += r->what[i] == 1;
        if ((double)nmove * CUI__SW_TILE * CUI__SW_TILE >= CUI__SW_THREADS_MOVE) cui__sw_threads_run(r, njobs, cui__sw_move_job, &m);
        else for (int j = 0; j < njobs; j++) cui__sw_move_job(&m, j);
    }
    if (second) cui__sw_second(r, pixels, stride, width, height, tw, th, true);
    /* then drawing */
    double area;
    const uint8_t *umode = cui__sw_under_plan(r, tw, th, width, height, fmt) ? r->umode : NULL;
    int njobs = cui__sw_plan(r, tw, th, width, height, &area);
    cui__sw_frame f = { r, dd, pixels, width, height, stride, fmt, NULL, umode, tw };
    if (area >= CUI__SW_THREADS_DRAW) cui__sw_threads_run(r, njobs, cui__sw_paint_job, &f);
    else {
        if (!r->scratch) { r->scratch = cui__sw_xrealloc(NULL, sizeof *r->scratch); r->scratch->wtab.ok = false; }
        r->scratch->key = NULL;   /* vertices are this frame's */
        f.sc = r->scratch;
        for (int j = 0; j < njobs; j++) cui__sw_paint_job(&f, j);
    }
    if (r->incremental) { uint64_t *t = r->prev; r->prev = r->tiles; r->tiles = t; }
    cui__sw_prim *pp = r->prims; r->prims = r->pprims; r->pprims = pp;
    int pc = r->prims_cap; r->prims_cap = r->pprims_cap; r->pprims_cap = pc;
    r->npprims = r->nprims; r->nprims = 0;
}

/* ---- PNG writer ---- */

/* Screenshots, never a frame: compiled for size even where the renderer is built for speed (GCC). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("Os")
#endif

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
#include "../../third_party/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

bool cui_write_png(const char *path, const void *rgba, int width, int height, int stride) {
    return stbi_write_png(path, width, height, 4, rgba, stride) != 0;
}

void *cui_png_encode(const void *rgba, int width, int height, int stride, int *len) {
    int n = 0;
    unsigned char *p = stbi_write_png_to_mem((const unsigned char *)rgba, stride, width, height, 4, &n);
    if (len) *len = p ? n : 0;
    return p;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
