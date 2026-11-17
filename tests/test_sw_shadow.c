/* The software renderer's shadows against the shadow formula drawn pixel by pixel, the way the
 * renderer did it before its faster shadow path: the distance to the rounded rectangle, the erf
 * falloff, the same blend. Shadows of many sizes, radii, blurs, scales, offsets and colors, over
 * a background that varies, drawn whole and with threads; then a shadow animating (incremental
 * frames) against the same shadow drawn whole. Max channel difference and the mean are printed. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

/* ---- the reference: the shadow as the old renderer computed it ---- */

static float ref_sd(float px, float py, float hw, float hh, float tl, float tr, float br, float bl) {
    float r = px < 0 ? (py < 0 ? tl : bl) : (py < 0 ? tr : br);
    float m = hw < hh ? hw : hh;
    if (r > m) r = m;
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return inside + sqrtf(ox * ox + oy * oy) - r;
}
static float ref_sat(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static uint8_t ref_ch(float s, uint8_t d, float ia) { return (uint8_t)(ref_sat(s + (d / 255.0f) * ia) * 255.0f + 0.5f); }

/* one shadow quad over px (RGBA), as the old quad path drew it (erf in double: the old table was within 2e-6 of it) */
static void ref_shadow(uint8_t *px, int w, int h, const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v3, cui_rect clip) {
    float qx0 = v0->x, qy0 = v0->y, qx1 = v1->x, qy1 = v3->y;
    int x0 = (int)ceilf(qx0 - 0.5f), x1 = (int)ceilf(qx1 - 0.5f), y0 = (int)ceilf(qy0 - 0.5f), y1 = (int)ceilf(qy1 - 0.5f);
    int cx0 = (int)floorf(clip.x), cy0 = (int)floorf(clip.y), cx1 = (int)ceilf(clip.x + clip.w), cy1 = (int)ceilf(clip.y + clip.h);
    x0 = CUI_MAX(x0, CUI_MAX(cx0, 0)); y0 = CUI_MAX(y0, CUI_MAX(cy0, 0));
    x1 = CUI_MIN(x1, CUI_MIN(cx1, w)); y1 = CUI_MIN(y1, CUI_MIN(cy1, h));
    float cr = v0->color.r / 255.0f, cg = v0->color.g / 255.0f, cb = v0->color.b / 255.0f, ca = v0->color.a / 255.0f;
    float sigma = v0->border > 0.01f ? v0->border : 0.01f;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float lx = v0->lx + ((float)x + 0.5f - qx0), ly = v0->ly + ((float)y + 0.5f - qy0);
            float d = ref_sd(lx, ly, v0->hw, v0->hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl);
            float z = d / (sigma * 1.41421356f);
            if (z > 2.8f) continue;
            float al = ca * (float)(0.5 * (1.0 - erf((double)z)));
            if (al <= 0.0005f) continue;
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            float s[4] = { cr * al, cg * al, cb * al, al };
            if (al >= 0.9995f) { for (int k = 0; k < 4; k++) p[k] = (uint8_t)(ref_sat(s[k]) * 255.0f + 0.5f); p[3] = 255; continue; }
            for (int k = 0; k < 4; k++) p[k] = ref_ch(s[k], p[k], 1.0f - al);
        }
}

/* ---- scenes ---- */

typedef struct { float x, y, w, h, r0, r1, blur, spread, ox, oy; cui_color c; } shadow_case;
static float g_scale;
static cui_texture g_bg;
static const shadow_case *g_case;
static int g_ncase;
static bool g_with_shadow;

static void scene(cui_ctx *ui) {
    cui_draw_image(ui, g_bg, (cui_rect){ 0, 0, ui->vw, ui->vh }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
    if (!g_with_shadow) return;
    for (int i = 0; i < g_ncase; i++) {
        const shadow_case *c = &g_case[i];
        cui_draw_shadow(ui, (cui_rect){ c->x, c->y, c->w, c->h }, (cui_corners){ c->r0, c->r1, c->r0, c->r1 }, c->blur, c->spread,
                        (cui_vec2){ c->ox, c->oy }, c->c);
    }
}

typedef struct { int max; double mean; long long n_diff; } diff;
static diff compare(const uint8_t *a, const uint8_t *b, int w, int h) {
    diff d = { 0, 0, 0 };
    double sum = 0;
    for (size_t i = 0; i < (size_t)w * h * 4; i++) {
        int e = abs((int)a[i] - (int)b[i]);
        if (e > d.max) d.max = e;
        if (e) d.n_diff++;
        sum += e;
    }
    d.mean = sum / ((double)w * h * 4);
    return d;
}

/* renders the cases with the renderer and with the reference; returns the difference */
static diff run_cases(cui_sw_renderer *sw, const shadow_case *cs, int n, int w, int h, float scale, int threads) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, (float)w / scale, (float)h / scale, scale);
    uint8_t *bgpx = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = bgpx + ((size_t)y * w + x) * 4;
            p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = (uint8_t)(((x / 7) ^ (y / 5)) * 37); p[3] = 255;
        }
    g_bg = cui_image_create(ui, w, h, bgpx);
    g_case = cs; g_ncase = n; g_scale = scale;
    cui_sw_set_threads(sw, threads);
    cui_sw_set_incremental(sw, false);
    uint8_t *got = malloc((size_t)w * h * 4), *want = malloc((size_t)w * h * 4);
    /* the background alone, then the reference shadows over it */
    g_with_shadow = false;
    cui_begin_frame(ui, 1.0f / 60); scene(ui); cui_end_frame(ui);
    cui_sw_render(sw, cui_get_draw_data(ui), want, w, h, w * 4, CUI_SW_RGBA);
    g_with_shadow = true;
    cui_begin_frame(ui, 1.0f / 60); scene(ui); cui_end_frame(ui);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    for (int ci = 0; ci < dd->command_count; ci++) {
        const cui_draw_cmd *cmd = &dd->commands[ci];
        for (uint32_t k = 0; k + 5 < cmd->index_count + 0u; k += 6) {
            const uint32_t *ix = dd->indices + cmd->index_offset + k;
            const cui_vertex *v = &dd->vertices[ix[0]];
            if ((int)(v->mode + 0.5f) == CUI_SHADE_SHADOW) ref_shadow(want, w, h, &v[0], &v[1], &v[3], cmd->clip);
        }
    }
    cui_sw_render(sw, dd, got, w, h, w * 4, CUI_SW_RGBA);
    diff d = compare(got, want, w, h);
    cui_image_destroy(ui, g_bg);
    cui_destroy(ui);
    free(bgpx); free(got); free(want);
    return d;
}

/* An animated shadow (a popover opening: moving, growing, fading) drawn incrementally must equal
 * the same frames drawn whole. */
static float g_t;
static void anim_scene(cui_ctx *ui) {
    cui_draw_rect(ui, (cui_rect){ 0, 0, ui->vw, ui->vh }, CUI_CORNERS(0), cui__rgba(240, 240, 236, 255));
    float s = 0.94f + 0.06f * g_t;
    cui_rect r = { 300 - 160 * s, 60 + (1 - g_t) * 10, 320 * s, 300 * s };
    cui__draw_elevation(ui, r, CUI_CORNERS(10), CUI_ELEVATION_FLOATING, g_t);
    cui_draw_rect(ui, r, CUI_CORNERS(10), cui__rgba(255, 255, 255, (int)(255 * g_t)));
}
static int run_anim(cui_sw_renderer *inc, cui_sw_renderer *whole) {
    int w = 640, h = 480, worst = 0;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(inc), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, (float)w, (float)h, 1);
    uint8_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4);
    cui_sw_set_incremental(inc, true);
    cui_sw_set_incremental(whole, false);
    for (int f = 0; f <= 30; f++) {
        g_t = f < 20 ? (float)f / 20 : 1.0f - (float)(f - 20) / 12;
        cui_begin_frame(ui, 1.0f / 60); anim_scene(ui); cui_end_frame(ui);
        const cui_draw_data *dd = cui_get_draw_data(ui);
        cui_sw_render(inc, dd, a, w, h, w * 4, CUI_SW_BGRA);
        cui_sw_render(whole, dd, b, w, h, w * 4, CUI_SW_BGRA);
        diff d = compare(a, b, w, h);
        if (d.max > worst) worst = d.max;
    }
    cui_destroy(ui);
    free(a); free(b);
    return worst;
}

/* ---- images: the old per-pixel path (distance, bilinear sample, blend) ---- */

typedef struct { int w, h; const uint8_t *px; } ref_tex;
static void ref_sample(const ref_tex *t, float u, float v, float o[4]) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int xs[2] = { CUI_CLAMP(x0, 0, t->w - 1), CUI_CLAMP(x0 + 1, 0, t->w - 1) }, ys[2] = { CUI_CLAMP(y0, 0, t->h - 1), CUI_CLAMP(y0 + 1, 0, t->h - 1) };
    float w[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int k = 0; k < 4; k++) {
        const uint8_t *p = t->px + ((size_t)ys[k >> 1] * (size_t)t->w + (size_t)xs[k & 1]) * 4;
        float a = p[3] / 255.0f;
        o[0] += w[k] * p[0] / 255.0f * a; o[1] += w[k] * p[1] / 255.0f * a; o[2] += w[k] * p[2] / 255.0f * a; o[3] += w[k] * a;
    }
}
static void ref_image(uint8_t *px, int w, int h, const cui_vertex *v0, const cui_vertex *v1, const cui_vertex *v3, const ref_tex *t) {
    float qx0 = v0->x, qy0 = v0->y, qx1 = v1->x, qy1 = v3->y;
    int x0 = CUI_MAX(0, (int)ceilf(qx0 - 0.5f)), x1 = CUI_MIN(w, (int)ceilf(qx1 - 0.5f));
    int y0 = CUI_MAX(0, (int)ceilf(qy0 - 0.5f)), y1 = CUI_MIN(h, (int)ceilf(qy1 - 0.5f));
    float du = (v1->u - v0->u) / (qx1 - qx0), dv = (v3->v - v0->v) / (qy1 - qy0);
    float cr = v0->color.r / 255.0f, cg = v0->color.g / 255.0f, cb = v0->color.b / 255.0f, ca = v0->color.a / 255.0f;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float pxf = (float)x + 0.5f, pyf = (float)y + 0.5f;
            float lx = v0->lx + (pxf - qx0), ly = v0->ly + (pyf - qy0);
            float cov = ref_sat(0.5f - ref_sd(lx, ly, v0->hw, v0->hh, v0->r_tl, v0->r_tr, v0->r_br, v0->r_bl));
            if (cov <= 0) continue;
            float s[4];
            ref_sample(t, v0->u + (pxf - qx0) * du, v0->v + (pyf - qy0) * dv, s);
            float k = ca * cov;
            s[0] *= cr * k; s[1] *= cg * k; s[2] *= cb * k; s[3] *= k;
            if (s[3] <= 0.0005f) continue;
            uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (s[3] >= 0.9995f) { for (int c = 0; c < 3; c++) p[c] = (uint8_t)(ref_sat(s[c]) * 255.0f + 0.5f); p[3] = 255; continue; }
            for (int c = 0; c < 4; c++) p[c] = ref_ch(s[c], p[c], 1.0f - s[3]);
        }
}

static ref_tex g_img;
static int g_img_case;
static void image_scene(cui_ctx *ui, cui_texture t) {
    cui_draw_rect(ui, (cui_rect){ 0, 0, ui->vw, ui->vh }, CUI_CORNERS(0), cui__rgba(30, 90, 160, 255));
    float iw = (float)g_img.w / ui->scale, ih = (float)g_img.h / ui->scale;
    cui_color white = cui__rgba(255, 255, 255, 255);
    if (g_img_case == 0) cui_draw_image(ui, t, (cui_rect){ 20, 10, iw, ih }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), white);
    if (g_img_case == 1) cui_draw_image(ui, t, (cui_rect){ 20, 10, iw, ih }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(14), white);
    if (g_img_case == 2) cui_draw_image(ui, t, (cui_rect){ 20.5f, 10.25f, iw * 1.37f, ih * 0.8f }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(6), white);
    if (g_img_case == 3) cui_draw_image(ui, t, (cui_rect){ 20, 10, iw, ih }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(3), cui__rgba(255, 200, 120, 180));
}
static diff run_image(cui_sw_renderer *sw, int which, bool translucent, float scale) {
    int w = 600, h = 400;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, (float)w / scale, (float)h / scale, scale);
    int tw = 333, th = 257;
    uint8_t *tp = malloc((size_t)tw * th * 4);
    for (int i = 0; i < tw * th; i++) {
        tp[i * 4 + 0] = (uint8_t)(i * 7); tp[i * 4 + 1] = (uint8_t)(i / tw * 3); tp[i * 4 + 2] = (uint8_t)((i % tw) ^ (i / tw));
        tp[i * 4 + 3] = translucent ? (uint8_t)((i % 5) * 60) : 255;
    }
    g_img = (ref_tex){ tw, th, tp };
    g_img_case = which;
    cui_texture t = cui_image_create(ui, tw, th, tp);
    cui_sw_set_incremental(sw, false);
    cui_begin_frame(ui, 1.0f / 60); image_scene(ui, t); cui_end_frame(ui);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    uint8_t *got = malloc((size_t)w * h * 4), *want = malloc((size_t)w * h * 4);
    cui_sw_render(sw, dd, got, w, h, w * 4, CUI_SW_RGBA);
    for (int i = 0; i < w * h; i++) { want[i * 4] = 30; want[i * 4 + 1] = 90; want[i * 4 + 2] = 160; want[i * 4 + 3] = 255; }
    for (int ci = 0; ci < dd->command_count; ci++) {
        const cui_draw_cmd *cmd = &dd->commands[ci];
        for (uint32_t k = 0; k + 5 < cmd->index_count + 0u; k += 6) {
            const cui_vertex *v = &dd->vertices[dd->indices[cmd->index_offset + k]];
            if ((int)(v->mode + 0.5f) == CUI_SHADE_IMAGE) ref_image(want, w, h, &v[0], &v[1], &v[3], &g_img);
        }
    }
    diff d = compare(got, want, w, h);
    cui_image_destroy(ui, t);
    cui_destroy(ui);
    free(tp); free(got); free(want);
    return d;
}

int main(void) {
    static const shadow_case cases[] = {
        /* x, y, w, h, radius (tl/br, tr/bl), blur, spread, offset, color */
        { 40, 30, 340, 420, 10, 10, 32, -4, 0, 12, { 0, 0, 0, 31 } },      /* the light theme's floating shadow */
        { 40, 30, 340, 420, 10, 10, 3, 0, 0, 1, { 0, 0, 0, 15 } },
        { 460, 40, 300, 260, 12, 12, 36, -4, 0, 14, { 0, 0, 0, 102 } },    /* the dark theme's */
        { 460, 40, 300, 260, 12, 12, 3, 0, 0, 1, { 0, 0, 0, 61 } },
        { 430, 360, 200, 120, 0, 0, 12, 0, 0, 3, { 0, 0, 0, 60 } },        /* square corners */
        { 660, 350, 90, 90, 45, 45, 20, 0, 0, 0, { 255, 255, 255, 97 } },  /* a light circle (the glow use) */
        { 80, 500, 400, 30, 15, 2, 6, 2, 1.3f, 2.6f, { 20, 60, 200, 200 } }, /* mixed radii, fractional offset */
        { 520, 520, 12, 40, 20, 0, 64, 0, 0, 0, { 0, 0, 0, 255 } },        /* blur much wider than the shape, radius over half */
        { 700.25f, 480.5f, 70.5f, 50.25f, 6, 3, 1, 0, 0, 0, { 90, 0, 0, 255 } },   /* a sharp one, opaque */
        { -60, 560, 200, 100, 8, 8, 24, 6, 0, 4, { 0, 0, 0, 128 } },       /* off the left edge */
        { 760, -40, 120, 100, 30, 30, 40, -10, 0, 0, { 0, 0, 0, 90 } },    /* off the top right */
    };
    int n = (int)(sizeof cases / sizeof cases[0]);
    cui_sw_renderer *sw = cui_sw_create(), *sw2 = cui_sw_create();
    static const struct { int w, h; float s; int threads; } runs[] = {
        { 800, 640, 1, 1 }, { 800, 640, 1, 4 }, { 1200, 960, 1.5f, 3 }, { 1600, 1280, 2, 4 },
    };
    for (int i = 0; i < (int)(sizeof runs / sizeof runs[0]); i++) {
        diff d = run_cases(sw, cases, n, runs[i].w, runs[i].h, runs[i].s, runs[i].threads);
        printf("shadows %dx%d scale %.1f, %d thread(s): max channel difference %d, mean %.5f, %lld channels differ\n",
               runs[i].w, runs[i].h, runs[i].s, runs[i].threads, d.max, d.mean, d.n_diff);
        CHECK(d.max <= 2);
        CHECK(d.mean <= 0.01);
    }
    static const char *img_names[4] = { "at its size", "at its size, rounded", "scaled, rounded", "at its size, tinted" };
    for (int s = 0; s < 2; s++)
        for (int tr = 0; tr < 2; tr++)
            for (int c = 0; c < 4; c++) {
                diff d = run_image(sw, c, tr != 0, s ? 2.0f : 1.0f);
                printf("image %s, %s, scale %d: max channel difference %d, mean %.5f\n", img_names[c], tr ? "translucent" : "opaque", s + 1, d.max, d.mean);
                CHECK(d.max <= 1);
                if (c == 0 && !tr) CHECK(d.max == 0);   /* an opaque picture at its size is its texels */
            }
    int a = run_anim(sw, sw2);
    printf("animated shadow, incremental against whole frames: max channel difference %d\n", a);
    CHECK(a == 0);
    cui_sw_destroy(sw);
    cui_sw_destroy(sw2);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
