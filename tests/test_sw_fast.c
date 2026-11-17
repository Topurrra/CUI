/* The software renderer's fast paths against the per pixel float formulas it used before them:
 * glyphs (at their size and scaled), rounded rectangles without a border, translucent fills,
 * pictures with translucent texels, blended in 16 bit fixed point (SSE2 where there is SSE2).
 * Each case is a hand made draw list over a busy picture, drawn whole and incrementally, on one
 * thread and on four; the reference draws the same quads pixel by pixel with the old arithmetic.
 * Then the parts of a frame hidden under an opaque rectangle or an opaque picture, which the
 * renderer skips, must change nothing: the frame is compared byte for byte with the same frame
 * without the hidden things. Max channel difference, mean and the channels that differ are printed. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define W 640
#define H 400

/* ---- a hand made draw list ---- */

typedef struct { cui_vertex v[4096]; uint32_t ix[6144]; cui_draw_cmd cmd[64]; int nv, ni, nc; } list;
static void list_cmd(list *l, cui_texture t, cui_rect clip) {
    cui_draw_cmd *c = &l->cmd[l->nc++];
    c->clip = clip; c->texture = t; c->index_offset = (uint32_t)l->ni; c->index_count = 0;
}
/* a quad as cui__sdf_quad makes one: x0, y0 to x1, y1 in pixels, the shape's half size, radius, border */
static void list_quad(list *l, int mode, float x0, float y0, float x1, float y1, float hw, float hh, float r, float bw,
                      cui_color c, cui_color bc, float u0, float v0, float u1, float v1) {
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    const float px[4] = { x0, x1, x1, x0 }, py[4] = { y0, y0, y1, y1 }, pu[4] = { u0, u1, u1, u0 }, pv[4] = { v0, v0, v1, v1 };
    uint32_t base = (uint32_t)l->nv;
    for (int i = 0; i < 4; i++) {
        cui_vertex *v = &l->v[l->nv++];
        memset(v, 0, sizeof *v);
        v->x = px[i]; v->y = py[i]; v->u = pu[i]; v->v = pv[i];
        v->lx = px[i] - cx; v->ly = py[i] - cy; v->hw = hw; v->hh = hh;
        v->r_tl = v->r_tr = v->r_br = v->r_bl = r;
        v->border = bw; v->mode = (float)mode; v->color = c; v->border_color = bc;
    }
    const uint32_t q[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
    memcpy(&l->ix[l->ni], q, sizeof q);
    l->ni += 6;
    l->cmd[l->nc - 1].index_count += 6;
}
static cui_draw_data list_data(const list *l, cui_texture atlas) {
    cui_draw_data d;
    memset(&d, 0, sizeof d);
    d.vertices = l->v; d.vertex_count = l->nv; d.indices = l->ix; d.index_count = l->ni;
    d.commands = l->cmd; d.command_count = l->nc; d.width_px = W; d.height_px = H;
    d.clear_color = cui__rgba(20, 22, 26, 255); d.font_atlas = atlas;
    return d;
}

/* ---- the reference: the old per pixel arithmetic ---- */

typedef struct { int w, h, fmt; const uint8_t *data; } ref_tex;
static float ref_sat(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static uint8_t ref_ch(float s, uint8_t d, float ia) { return (uint8_t)(ref_sat(s + (d / 255.0f) * ia) * 255.0f + 0.5f); }
static void ref_blend(uint8_t *p, const float s[4]) {   /* s premultiplied, RGBA order (the test draws RGBA) */
    if (s[3] <= 0.0005f) return;
    if (s[3] >= 0.9995f) { for (int k = 0; k < 3; k++) p[k] = (uint8_t)(ref_sat(s[k]) * 255.0f + 0.5f); p[3] = 255; return; }
    float ia = 1.0f - s[3];
    for (int k = 0; k < 4; k++) p[k] = ref_ch(s[k], p[k], ia);
}
static float ref_sd(float px, float py, float hw, float hh, float r) {
    float m = hw < hh ? hw : hh;
    if (r > m) r = m;
    float qx = fabsf(px) - hw + r, qy = fabsf(py) - hh + r;
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, in = qx > qy ? qx : qy;
    if (in > 0) in = 0;
    return in + sqrtf(ox * ox + oy * oy) - r;
}
static float ref_r8(const ref_tex *t, float u, float v) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = CUI_CLAMP(x0, 0, t->w - 1); y0 = CUI_CLAMP(y0, 0, t->h - 1); x1 = CUI_CLAMP(x1, 0, t->w - 1); y1 = CUI_CLAMP(y1, 0, t->h - 1);
    const uint8_t *d = t->data;
    float a = d[y0 * t->w + x0], b = d[y0 * t->w + x1], c = d[y1 * t->w + x0], e = d[y1 * t->w + x1];
    return ((a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + e * fx) * fy) / 255.0f;
}
static void ref_rgba(const ref_tex *t, float u, float v, float o[4]) {
    float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f;
    float rx = floorf(x + 0.5f), ry = floorf(y + 0.5f);
    if (fabsf(x - rx) <= 1.0f / 512 && fabsf(y - ry) <= 1.0f / 512) {   /* on a texel center: that texel */
        int ix = CUI_CLAMP((int)rx, 0, t->w - 1), iy = CUI_CLAMP((int)ry, 0, t->h - 1);
        const uint8_t *p = t->data + ((size_t)iy * (size_t)t->w + (size_t)ix) * 4;
        float a = p[3] / 255.0f;
        o[0] = p[0] / 255.0f * a; o[1] = p[1] / 255.0f * a; o[2] = p[2] / 255.0f * a; o[3] = a;
        return;
    }
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - (float)x0, fy = y - (float)y0;
    int xs[2] = { CUI_CLAMP(x0, 0, t->w - 1), CUI_CLAMP(x0 + 1, 0, t->w - 1) }, ys[2] = { CUI_CLAMP(y0, 0, t->h - 1), CUI_CLAMP(y0 + 1, 0, t->h - 1) };
    float wt[4] = { (1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy };
    o[0] = o[1] = o[2] = o[3] = 0;
    for (int k = 0; k < 4; k++) {
        const uint8_t *p = t->data + ((size_t)ys[k >> 1] * (size_t)t->w + (size_t)xs[k & 1]) * 4;
        float a = p[3] / 255.0f;
        o[0] += wt[k] * p[0] / 255.0f * a; o[1] += wt[k] * p[1] / 255.0f * a; o[2] += wt[k] * p[2] / 255.0f * a; o[3] += wt[k] * a;
    }
}
/* every quad of the list over px, the way the old renderer drew it pixel by pixel */
static void ref_draw(uint8_t *px, const list *l, const ref_tex *texs, int ntex) {
    for (int ci = 0; ci < l->nc; ci++) {
        const cui_draw_cmd *cmd = &l->cmd[ci];
        const ref_tex *t = (int)cmd->texture >= 1 && (int)cmd->texture <= ntex ? &texs[cmd->texture - 1] : NULL;
        int cx0 = (int)floorf(cmd->clip.x), cy0 = (int)floorf(cmd->clip.y), cx1 = (int)ceilf(cmd->clip.x + cmd->clip.w), cy1 = (int)ceilf(cmd->clip.y + cmd->clip.h);
        for (uint32_t k = 0; k < cmd->index_count; k += 6) {
            const cui_vertex *v0 = &l->v[l->ix[cmd->index_offset + k]], *v1 = v0 + 1, *v3 = v0 + 3;
            int mode = (int)(v0->mode + 0.5f);
            float qx0 = v0->x, qy0 = v0->y, qx1 = v1->x, qy1 = v3->y, qw = qx1 - qx0, qh = qy1 - qy0;
            float du = (v1->u - v0->u) / qw, dv = (v3->v - v0->v) / qh;
            bool exact = mode == CUI_SHADE_TEXT && t && fabsf(du * (float)t->w - 1.0f) < 0.001f && fabsf(dv * (float)t->h - 1.0f) < 0.001f;
            int x0 = CUI_MAX(CUI_MAX(cx0, 0), (int)ceilf(qx0 - 0.5f)), x1 = CUI_MIN(CUI_MIN(cx1, W), (int)ceilf(qx1 - 0.5f));
            int y0 = CUI_MAX(CUI_MAX(cy0, 0), (int)ceilf(qy0 - 0.5f)), y1 = CUI_MIN(CUI_MIN(cy1, H), (int)ceilf(qy1 - 0.5f));
            float cr = v0->color.r / 255.0f, cg = v0->color.g / 255.0f, cb = v0->color.b / 255.0f, ca = v0->color.a / 255.0f;
            float bcr = v0->border_color.r / 255.0f, bcg = v0->border_color.g / 255.0f, bcb = v0->border_color.b / 255.0f, bca = v0->border_color.a / 255.0f;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    float pxc = (float)x + 0.5f, pyc = (float)y + 0.5f;
                    float lx = v0->lx + (pxc - qx0), ly = v0->ly + (pyc - qy0);
                    float o[4];
                    if (mode == CUI_SHADE_SOLID) {
                        o[0] = cr * ca; o[1] = cg * ca; o[2] = cb * ca; o[3] = ca;
                    } else if (mode == CUI_SHADE_TEXT) {
                        float u = v0->u + (pxc - qx0) * du, vv = v0->v + (pyc - qy0) * dv, cov;
                        if (exact) {
                            int txi = (int)(u * (float)t->w), tyi = (int)(vv * (float)t->h);
                            if (txi < 0 || txi >= t->w || tyi < 0 || tyi >= t->h) continue;
                            uint8_t tt = t->data[(size_t)tyi * (size_t)t->w + (size_t)txi];
                            if (!tt) continue;
                            cov = tt / 255.0f;
                        } else cov = ref_r8(t, u, vv);
                        float al = ca * cov;
                        o[0] = cr * al; o[1] = cg * al; o[2] = cb * al; o[3] = al;
                    } else if (mode == CUI_SHADE_RRECT) {
                        float d = ref_sd(lx, ly, v0->hw, v0->hh, v0->r_tl), outer = ref_sat(0.5f - d);
                        if (outer <= 0) continue;
                        float bw = v0->border, inner = bw > 0 ? ref_sat(0.5f - (d + bw)) : outer;
                        float fa = ca * inner, ba = bca * (outer - inner);
                        o[0] = cr * fa + bcr * ba; o[1] = cg * fa + bcg * ba; o[2] = cb * fa + bcb * ba; o[3] = fa + ba;
                    } else {   /* image */
                        float d = ref_sd(lx, ly, v0->hw, v0->hh, v0->r_tl), cov = ref_sat(0.5f - d);
                        if (cov <= 0) continue;
                        float tc[4];
                        ref_rgba(t, v0->u + (pxc - qx0) * du, v0->v + (pyc - qy0) * dv, tc);
                        float kk = ca * cov;
                        o[0] = tc[0] * cr * kk; o[1] = tc[1] * cg * kk; o[2] = tc[2] * cb * kk; o[3] = tc[3] * kk;
                    }
                    ref_blend(px + ((size_t)y * W + (size_t)x) * 4, o);
                }
        }
    }
}

/* ---- comparing ---- */

typedef struct { int max; double mean; long long n; } diff;
static diff compare(const uint8_t *a, const uint8_t *b) {
    diff d = { 0, 0, 0 };
    double sum = 0;
    for (size_t i = 0; i < (size_t)W * H * 4; i++) {
        int e = abs((int)a[i] - (int)b[i]);
        if (e > d.max) d.max = e;
        if (e) d.n++;
        sum += e;
    }
    d.mean = sum / ((double)W * H * 4);
    return d;
}

/* ---- textures and scenes ---- */

static uint8_t g_bg[W * H * 4], g_glyphs[256 * 64], g_pic[96 * 64 * 4];
static cui_texture g_tbg, g_tgl, g_tpic, g_tpic2;
static ref_tex g_ref[8];

static void make_textures(cui_sw_renderer *sw) {
    cui_renderer_iface rf = cui_sw_iface(sw);
    unsigned s = 12345;
    for (int i = 0; i < W * H; i++) {   /* a busy opaque picture: gradients and noise */
        s = s * 1103515245u + 12345u;
        int x = i % W, y = i / W;
        g_bg[i * 4 + 0] = (uint8_t)(x * 255 / W); g_bg[i * 4 + 1] = (uint8_t)(y * 255 / H);
        g_bg[i * 4 + 2] = (uint8_t)(s >> 24); g_bg[i * 4 + 3] = 255;
    }
    for (int i = 0; i < 256 * 64; i++) {   /* glyph-like coverage: runs of 0, partial and full */
        s = s * 1103515245u + 12345u;
        int x = i % 256, y = i / 256;
        int v = ((x / 3 + y / 2) % 5 == 0) ? 0 : (int)(s >> 24);
        if ((x + y) % 7 == 0) v = 255;
        g_glyphs[i] = (uint8_t)v;
    }
    for (int i = 0; i < 96 * 64; i++) {   /* a picture with opaque runs and translucent texels */
        s = s * 1103515245u + 12345u;
        g_pic[i * 4 + 0] = (uint8_t)(s >> 24); g_pic[i * 4 + 1] = (uint8_t)(s >> 16); g_pic[i * 4 + 2] = (uint8_t)(i * 3);
        g_pic[i * 4 + 3] = (uint8_t)((i % 96) % 9 == 4 ? (s >> 8) & 255 : 255);
    }
    g_tbg = rf.texture_create(rf.user, W, H, CUI_TEXTURE_RGBA8, g_bg);
    g_tgl = rf.texture_create(rf.user, 256, 64, CUI_TEXTURE_R8, g_glyphs);
    g_tpic = rf.texture_create(rf.user, 96, 64, CUI_TEXTURE_RGBA8, g_pic);
    g_ref[g_tbg - 1] = (ref_tex){ W, H, 4, g_bg };
    g_ref[g_tgl - 1] = (ref_tex){ 256, 64, 1, g_glyphs };
    g_ref[g_tpic - 1] = (ref_tex){ 96, 64, 4, g_pic };
    (void)g_tpic2;
}

static const cui_rect g_full = { 0, 0, W, H };
static void add_bg(list *l) {
    list_cmd(l, g_tbg, g_full);
    list_quad(l, CUI_SHADE_IMAGE, 0, 0, W, H, W * 0.5f, H * 0.5f, 0, 0, cui__rgba(255, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
}

/* glyphs at their size (whole texel steps, half pixel offsets as text lands) and scaled, several colors and alphas */
static void scene_text(list *l, float scale) {
    add_bg(l);
    list_cmd(l, g_tgl, (cui_rect){ 10, 10, W - 20, H - 20 });
    static const cui_color cols[4] = { { 235, 235, 240, 255 }, { 120, 170, 255, 255 }, { 250, 250, 250, 128 }, { 10, 10, 10, 200 } };
    for (int i = 0; i < 40; i++) {
        int gw = 6 + i % 9, gh = 10 + i % 7, tx = (i * 23) % (256 - gw), ty = (i * 11) % (64 - gh);
        float x = 20.0f + (float)(i % 8) * 74.0f, y = 20.0f + (float)(i / 8) * 70.0f;
        float w = (float)gw * scale, h = (float)gh * scale;
        list_quad(l, CUI_SHADE_TEXT, x, y, x + w, y + h, w * 0.5f, h * 0.5f, 0, 0, cols[i % 4], cui__rgba(0, 0, 0, 0),
                  (float)tx / 256.0f, (float)ty / 64.0f, (float)(tx + gw) / 256.0f, (float)(ty + gh) / 64.0f);
    }
}
/* rounded rectangles without a border, opaque and translucent, sizes from a knob to a panel, fractional edges */
static void scene_rrect(list *l) {
    add_bg(l);
    list_cmd(l, 0, g_full);
    static const struct { float x, y, w, h, r; cui_color c; } rs[] = {
        { 20, 20, 300, 200, 14, { 44, 44, 50, 240 } }, { 340.5f, 30.25f, 16, 16, 8, { 250, 250, 250, 255 } },
        { 380, 30, 36, 20, 10, { 60, 120, 240, 255 } }, { 430.3f, 40.7f, 180.4f, 90.2f, 0, { 200, 40, 40, 100 } },
        { 30, 240, 560, 120, 30, { 0, 0, 0, 61 } }, { 600, 300, 30, 80, 40, { 255, 255, 255, 30 } },
    };
    for (size_t i = 0; i < sizeof rs / sizeof rs[0]; i++) {
        float hw = rs[i].w * 0.5f, hh = rs[i].h * 0.5f;
        list_quad(l, CUI_SHADE_RRECT, rs[i].x - 1, rs[i].y - 1, rs[i].x + rs[i].w + 1, rs[i].y + rs[i].h + 1, hw, hh, rs[i].r, 0,
                  rs[i].c, cui__rgba(0, 0, 0, 0), 0, 0, 0, 0);
    }
    /* a translucent solid and a bordered panel (the bordered one keeps the float path) */
    list_quad(l, CUI_SHADE_SOLID, 100, 100, 260.5f, 330, 80.25f, 115, 0, 0, cui__rgba(255, 200, 0, 90), cui__rgba(0, 0, 0, 0), 0, 0, 0, 0);
    list_quad(l, CUI_SHADE_RRECT, 300, 150, 520, 370, 109, 109, 14, 1, cui__rgba(44, 44, 50, 230), cui__rgba(90, 90, 100, 230), 0, 0, 0, 0);
}
/* a picture with translucent texels at its size (runs copied, the rest blended), tinted, and scaled */
static void scene_picture(list *l) {
    add_bg(l);
    list_cmd(l, g_tpic, g_full);
    list_quad(l, CUI_SHADE_IMAGE, 40, 40, 136, 104, 48, 32, 0, 0, cui__rgba(255, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
    list_quad(l, CUI_SHADE_IMAGE, 200, 40, 296, 104, 48, 32, 8, 0, cui__rgba(255, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
    list_quad(l, CUI_SHADE_IMAGE, 340, 40, 436, 104, 48, 32, 0, 0, cui__rgba(255, 128, 60, 200), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
    list_quad(l, CUI_SHADE_IMAGE, 40, 150, 290.5f, 330.5f, 125.25f, 90.25f, 12, 0, cui__rgba(255, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
}

static diff run(cui_sw_renderer *sw, void (*scene)(list *), float scale_arg, int threads, bool incremental, list *l) {
    static uint8_t got[W * H * 4], want[W * H * 4];
    memset(l, 0, sizeof *l);
    if (scene == (void (*)(list *))0) scene_text(l, scale_arg); else scene(l);
    cui_draw_data dd = list_data(l, g_tgl);
    cui_sw_set_threads(sw, threads);
    cui_sw_set_incremental(sw, incremental);
    if (incremental) {   /* a different frame first, then this one drawn over it */
        list other;
        memset(&other, 0, sizeof other);
        add_bg(&other);
        cui_draw_data od = list_data(&other, g_tgl);
        cui_sw_render(sw, &od, got, W, H, W * 4, CUI_SW_RGBA);
    }
    cui_sw_render(sw, &dd, got, W, H, W * 4, CUI_SW_RGBA);
    for (int i = 0; i < W * H; i++) { want[i * 4] = 20; want[i * 4 + 1] = 22; want[i * 4 + 2] = 26; want[i * 4 + 3] = 255; }
    ref_draw(want, l, g_ref, 8);
    return compare(got, want);
}

/* ---- the parts hidden under opaque things ---- */

static int hidden_case(cui_sw_renderer *sw, int occluder, bool with_hidden, uint8_t *out) {
    static list l;
    memset(&l, 0, sizeof l);
    add_bg(&l);   /* itself hidden under the occluders below where they cover */
    list_cmd(&l, g_tgl, g_full);
    if (with_hidden)
        for (int i = 0; i < 30; i++)
            list_quad(&l, CUI_SHADE_TEXT, 30.0f + (float)i * 19, 60, 42.0f + (float)i * 19, 76, 6, 8, 0, 0, cui__rgba(255, 255, 255, 255),
                      cui__rgba(0, 0, 0, 0), (float)(i * 7) / 256.0f, 0, (float)(i * 7 + 12) / 256.0f, 16.0f / 64.0f);
    if (occluder == 0) {
        list_cmd(&l, 0, g_full);
        list_quad(&l, CUI_SHADE_RRECT, 9, 9, 631, 391, 311, 191, 16, 0, cui__rgba(30, 30, 34, 255), cui__rgba(0, 0, 0, 0), 0, 0, 0, 0);
    } else if (occluder == 1) {
        list_cmd(&l, 0, g_full);
        list_quad(&l, CUI_SHADE_SOLID, 0, 0, 640, 400, 320, 200, 0, 0, cui__rgba(30, 30, 34, 255), cui__rgba(0, 0, 0, 0), 0, 0, 0, 0);
    } else {   /* an opaque picture (the background texture again), tinted opaque, with rounded corners */
        list_cmd(&l, g_tbg, g_full);
        list_quad(&l, CUI_SHADE_IMAGE, 4, 4, 636, 396, 316, 196, 10, 0, cui__rgba(200, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
    }
    /* something drawn after, over both */
    list_cmd(&l, g_tgl, g_full);
    list_quad(&l, CUI_SHADE_TEXT, 100, 200, 112, 216, 6, 8, 0, 0, cui__rgba(255, 220, 0, 255), cui__rgba(0, 0, 0, 0), 0, 0, 12.0f / 256.0f, 16.0f / 64.0f);
    cui_draw_data dd = list_data(&l, g_tgl);
    cui_sw_set_threads(sw, 1);
    cui_sw_set_incremental(sw, false);
    cui_sw_render(sw, &dd, out, W, H, W * 4, CUI_SW_BGRA);
    return 0;
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    make_textures(sw);
    static list l;
    static const char *names[] = { "glyphs at their size", "glyphs scaled 1.37", "rounded rectangles, fills", "pictures" };
    for (int c = 0; c < 4; c++)
        for (int th = 1; th <= 4; th += 3)
            for (int inc = 0; inc < 2; inc++) {
                diff d;
                if (c == 0) d = run(sw, NULL, 1.0f, th, inc, &l);
                else if (c == 1) d = run(sw, NULL, 1.37f, th, inc, &l);
                else d = run(sw, c == 2 ? scene_rrect : scene_picture, 0, th, inc, &l);
                printf("%s, %d thread(s)%s: max channel difference %d, mean %.5f, %lld channels differ\n", names[c], th,
                       inc ? ", incremental" : "", d.max, d.mean, d.n);
                CHECK(d.max <= 1);
                CHECK(d.mean <= 0.001);
            }
    /* a frame big enough for the worker threads (from 3,000,000 pixels): the same bytes on 4 threads as on one */
    {
        enum { BW = 2000, BH = 1600 };
        static list big;
        memset(&big, 0, sizeof big);
        list_cmd(&big, g_tbg, (cui_rect){ 0, 0, BW, BH });
        list_quad(&big, CUI_SHADE_IMAGE, 0, 0, BW, BH, BW * 0.5f, BH * 0.5f, 0, 0, cui__rgba(255, 255, 255, 255), cui__rgba(0, 0, 0, 0), 0, 0, 1, 1);
        list_cmd(&big, g_tgl, (cui_rect){ 0, 0, BW, BH });
        for (int i = 0; i < 600; i++) {
            float x = (float)(i % 30) * 66.0f + 3.5f, y = (float)(i / 30) * 79.0f + 2.0f;
            list_quad(&big, i % 3 ? CUI_SHADE_TEXT : CUI_SHADE_RRECT, x, y, x + 40, y + 30, 20, 15, i % 3 ? 0 : 6, 0,
                      cui__rgba(230, (uint8_t)(i * 7), 90, (uint8_t)(120 + i % 136)), cui__rgba(0, 0, 0, 0), 0.1f, 0.1f, 0.1f + 40.0f / 256.0f, 0.1f + 30.0f / 64.0f);
        }
        cui_draw_data dd = list_data(&big, g_tgl);
        dd.width_px = BW; dd.height_px = BH;
        uint8_t *one = malloc((size_t)BW * BH * 4), *four = malloc((size_t)BW * BH * 4);
        cui_sw_set_incremental(sw, false);
        cui_sw_set_threads(sw, 1);
        cui_sw_render(sw, &dd, one, BW, BH, BW * 4, CUI_SW_BGRA);
        cui_sw_set_threads(sw, 4);
        cui_sw_render(sw, &dd, four, BW, BH, BW * 4, CUI_SW_BGRA);
        bool same = memcmp(one, four, (size_t)BW * BH * 4) == 0;
        printf("%dx%d on 4 threads against one: %s\n", BW, BH, same ? "the same bytes" : "DIFFERENT");
        CHECK(same);
        free(one); free(four);
    }
    /* hidden parts: the same bytes with and without them */
    static uint8_t a[W * H * 4], b[W * H * 4];
    static const char *occ[] = { "an opaque rounded rectangle", "an opaque solid", "an opaque picture" };
    for (int o = 0; o < 3; o++) {
        hidden_case(sw, o, true, a);
        hidden_case(sw, o, false, b);
        diff d = compare(a, b);
        printf("under %s: %lld channels differ with and without what it hides\n", occ[o], d.n);
        CHECK(d.n == 0);
    }
    /* a picture that is not opaque hides nothing: an update that brings a translucent texel ends it */
    {
        cui_renderer_iface rf = cui_sw_iface(sw);
        uint8_t texel[4] = { 0, 0, 0, 0 };
        rf.texture_update(rf.user, g_tbg, 300, 60, 1, 1, texel, 4);
        hidden_case(sw, 2, true, a);
        hidden_case(sw, 2, false, b);
        diff d = compare(a, b);
        printf("under a picture with one clear texel: %lld channels differ (the glyph under it shows)\n", d.n);
        CHECK(d.n > 0);
    }
    cui_sw_destroy(sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
