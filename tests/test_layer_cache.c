/* The layer cache (cui_set_layer_cache): a group drawn from its bitmap against the same group drawn
 * the plain way, pixel by pixel.
 *
 *   - At rest, moved by whole pixels, and faded (against the panel drawn opaque and blended over the
 *     background by the opacity: a group fades as a whole), within 2 levels.
 *   - Scaled and moved by a fraction of a pixel (an opening popover): the bitmap is resampled, so text
 *     edges differ a little; the mean and the share of pixels off by more than 16 levels are bounded.
 *   - The frame's own commands, as a renderer that knows nothing of groups draws them (OpenGL), against
 *     the plain drawing.
 *   - The bitmap is drawn once while only the transform changes, again when the content changes, not
 *     for a group too big for the cache, and let go on the first frame without the group.
 *   - A real popover: a group while it animates, none at rest or after it closed and the bitmap freed
 *     then, hit areas the same as without the cache, incremental frames the same as whole frames, also
 *     with threads.
 * Prints the differences; with --shots DIR writes the plain and the cached pictures as PNG. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char *g_shots;
#define W 900
#define H 560

typedef struct { int max; double mean; double over16; } diff;
static diff compare(const uint8_t *a, const uint8_t *b, int w, int h) {
    diff d = { 0, 0, 0 };
    double sum = 0;
    long long over = 0;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        int m = 0;
        for (int c = 0; c < 4; c++) {
            int e = abs((int)a[i * 4 + c] - (int)b[i * 4 + c]);
            sum += e;
            if (e > m) m = e;
        }
        if (m > d.max) d.max = m;
        over += m > 16;
    }
    d.mean = sum / ((double)w * h * 4);
    d.over16 = (double)over / ((double)w * h);
    return d;
}
static void print_diff(const char *what, diff d) { printf("  %-52s max %3d, mean %.4f, over 16: %.5f%%\n", what, d.max, d.mean, d.over16 * 100); }

static void shot(const char *name, const uint8_t *px) {
    if (!g_shots) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", g_shots, name);
    cui_write_png(path, px, W, H, W * 4);
}

/* ---- a panel drawn with groups by hand: every kind of shape a popover has ---- */

static cui_texture g_bg, g_icon;
static int g_content;
static cui_rect g_panel = { 280, 90, 330, 300 };

static void panel(cui_ctx *ui) {
    cui_rect r = g_panel;
    cui__draw_elevation(ui, r, CUI_CORNERS(12), CUI_ELEVATION_FLOATING, 1);
    cui_draw_rect_border(ui, r, CUI_CORNERS(12), cui__rgba(250, 250, 248, 255), cui__rgba(0, 0, 0, 40), 1);
    cui_draw_text(ui, CUI_FONT_MEDIUM, 15, (cui_vec2){ r.x + 16, r.y + 16 }, cui__rgba(20, 20, 24, 255), "news.example", -1);
    cui_draw_text(ui, CUI_FONT_BODY, 13, (cui_vec2){ r.x + 16, r.y + 44 }, cui__rgba(90, 90, 96, 255), "12 trackers and ads blocked on this page", -1);
    for (int i = 0; i < 4; i++) {
        float y = r.y + 80 + (float)i * 34;
        cui_draw_text(ui, CUI_FONT_BODY, 13, (cui_vec2){ r.x + 16, y + 3 }, cui__rgba(30, 30, 34, 255), i % 2 ? "Block trackers and ads" : "Clean tracking parameters", -1);
        cui_draw_rect(ui, (cui_rect){ r.x + r.w - 56, y, 40, 22 }, CUI_CORNERS(11), i % 2 ? cui__rgba(40, 110, 230, 255) : cui__rgba(200, 200, 205, 255));
        cui_draw_circle(ui, (cui_vec2){ r.x + r.w - (i % 2 ? 27 : 45), y + 11 }, 8, cui__rgba(255, 255, 255, 255));
    }
    cui_draw_line(ui, (cui_vec2){ r.x + 16, r.y + 222 }, (cui_vec2){ r.x + r.w - 16, r.y + 222 }, 1, cui__rgba(0, 0, 0, 30));
    cui_draw_image(ui, g_icon, (cui_rect){ r.x + 16, r.y + 236, 32, 32 }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(6), cui__rgba(255, 255, 255, 255));
    cui_draw_rect(ui, (cui_rect){ r.x + 60, r.y + 240, 120, 26 }, CUI_CORNERS(6), g_content ? cui__rgba(220, 60, 50, 255) : cui__rgba(230, 230, 232, 255));
    cui_draw_text(ui, CUI_FONT_BODY, 13, (cui_vec2){ r.x + 72, r.y + 245 }, cui__rgba(20, 20, 24, 255), "Settings", -1);
}

/* the background, then the panel scaled by s around its center, moved by (tx, ty), faded to a: as a
 * group (when the cache is on) or plainly; without the panel when a < 0 */
static const cui_draw_data *frame(cui_ctx *ui, float s, float a, float tx, float ty, cui_id id) {
    cui_begin_frame(ui, 1.0f / 60);
    cui_draw_image(ui, g_bg, (cui_rect){ 0, 0, ui->vw, ui->vh }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
    if (a >= 0) {
        cui_rect r = g_panel;
        cui__push_xform(ui, s, r.x + r.w * 0.5f, r.y + r.h * 0.5f, a, tx, ty);
        bool g = cui__group_begin(ui, id);
        panel(ui);
        if (g) cui__group_end(ui);
        cui__pop_xform(ui);
    }
    cui_end_frame(ui);
    return cui_get_draw_data(ui);
}

static void render(cui_sw_renderer *sw, const cui_draw_data *dd, uint8_t *px) { cui_sw_render(sw, dd, px, W, H, W * 4, CUI_SW_RGBA); }

/* a over b by the opacity t, per channel (what a group faded as a whole gives over b) */
static void fade_ref(uint8_t *out, const uint8_t *a, const uint8_t *b, float t) {
    for (size_t i = 0; i < (size_t)W * H * 4; i++) out[i] = (uint8_t)(a[i] * t + b[i] * (1 - t) + 0.5f);
}

static void test_drawn_groups(void) {
    printf("groups drawn by hand (%dx%d)\n", W, H);
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, W, H, 1);
    cui_sw_set_incremental(sw, false);
    uint8_t *bgpx = malloc((size_t)W * H * 4), icon[32 * 32 * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = bgpx + ((size_t)y * W + x) * 4;
            bool ink = (y / 6) % 4 == 1 && (x / 9) % 7 != 3;
            p[0] = ink ? 40 : (uint8_t)(200 + x * 50 / W); p[1] = ink ? 40 : (uint8_t)(210 + y * 40 / H); p[2] = ink ? 70 : 225; p[3] = 255;
        }
    for (int i = 0; i < 32 * 32; i++) { icon[i * 4] = (uint8_t)(i * 7); icon[i * 4 + 1] = 120; icon[i * 4 + 2] = (uint8_t)(255 - i % 32 * 8); icon[i * 4 + 3] = (uint8_t)(i % 32 < 4 ? 0 : 255 - (i / 32) * 4); }
    g_bg = cui_image_create(ui, W, H, bgpx);
    g_icon = cui_image_create(ui, 32, 32, icon);
    uint8_t *plain = malloc((size_t)W * H * 4), *cached = malloc((size_t)W * H * 4), *bg = malloc((size_t)W * H * 4),
            *opaque = malloc((size_t)W * H * 4), *ref = malloc((size_t)W * H * 4);
    int r0, r1;
    size_t held;
    cui_set_layer_cache(ui, false);
    render(sw, frame(ui, 1, 1, 0, 0, 7), plain);   /* warm: the glyphs go to the atlas */
    render(sw, frame(ui, 1, -1, 0, 0, 7), bg);

    /* at rest and moved by whole pixels: the bitmap is copied pixel for pixel */
    static const struct { float tx, ty; const char *name; } moves[] = { { 0, 0, "at rest" }, { 0, -6, "moved 6 px up" }, { 13, 4, "moved 13, 4" } };
    for (int k = 0; k < 3; k++) {
        cui_set_layer_cache(ui, false);
        render(sw, frame(ui, 1, 1, moves[k].tx, moves[k].ty, 7), plain);
        cui_set_layer_cache(ui, true);
        render(sw, frame(ui, 0.97f, 0.5f, 0, 0, 7), cached);   /* moving first: the bitmap is drawn */
        const cui_draw_data *dd = frame(ui, 1, 1, moves[k].tx, moves[k].ty, 7);
        CHECK(dd->group_count == 1);
        render(sw, dd, cached);
        diff d = compare(plain, cached, W, H);
        char what[96];
        snprintf(what, sizeof what, "%s, from the bitmap / plain", moves[k].name);
        print_diff(what, d);
        CHECK(d.max <= 2);
        if (k == 1) { shot("rest_plain", plain); shot("rest_cached", cached); }
    }

    /* faded as a whole: against the opaque panel blended over the background */
    static const float fades[] = { 0.5f, 0.12f, 0.93f };
    for (int k = 0; k < 3; k++) {
        cui_set_layer_cache(ui, false);
        render(sw, frame(ui, 1, 1, 0, -3, 7), opaque);
        fade_ref(ref, opaque, bg, fades[k]);
        cui_set_layer_cache(ui, true);
        render(sw, frame(ui, 1, fades[k], 0, -3, 7), cached);
        diff d = compare(ref, cached, W, H);
        char what[96];
        snprintf(what, sizeof what, "opacity %.2f, from the bitmap / blended reference", (double)fades[k]);
        print_diff(what, d);
        CHECK(d.max <= 2);
    }

    /* scaled and moved by fractions: the bitmap resampled; unscaled, it goes to the nearest whole pixels */
    static const struct { float s, a, tx, ty; } anim[] = { { 0.96f, 1, 0, -5.7f }, { 0.975f, 1, 0.4f, -2.3f }, { 0.99f, 1, 0, -0.6f }, { 0.97f, 0.6f, 0, -3.1f }, { 1.0f, 1, 0.5f, -0.4f } };
    for (int k = 0; k < 5; k++) {
        cui_set_layer_cache(ui, false);
        if (anim[k].s == 1) render(sw, frame(ui, 1, 1, floorf(anim[k].tx + 0.5f), floorf(anim[k].ty + 0.5f), 7), opaque);
        else render(sw, frame(ui, anim[k].s, 1, anim[k].tx, anim[k].ty, 7), opaque);
        if (anim[k].a < 1) fade_ref(ref, opaque, bg, anim[k].a);
        else memcpy(ref, opaque, (size_t)W * H * 4);
        cui_set_layer_cache(ui, true);
        render(sw, frame(ui, 0.97f, 0.5f, 0, 0, 7), cached);   /* moving first: the bitmap is drawn */
        const cui_draw_data *dd = frame(ui, anim[k].s, anim[k].a, anim[k].tx, anim[k].ty, 7);
        render(sw, dd, cached);
        diff d = compare(ref, cached, W, H);
        char what[96];
        snprintf(what, sizeof what, "scale %.3f, opacity %.1f, moved %.1f, %.1f / plain", (double)anim[k].s, (double)anim[k].a, (double)anim[k].tx, (double)anim[k].ty);
        print_diff(what, d);
        CHECK(d.mean < 0.2);
        CHECK(d.over16 < 0.003);
        if (anim[k].s == 1) CHECK(d.max <= 2);
        if (k == 0) { shot("scaled_plain", ref); shot("scaled_cached", cached); }
        /* the frame's own commands, as a renderer without groups draws them, against the plain drawing */
        cui_draw_data nog = *dd;
        nog.groups = NULL; nog.group_count = 0;
        render(sw, &nog, cached);
        cui_set_layer_cache(ui, false);
        render(sw, frame(ui, anim[k].s, anim[k].a, anim[k].tx, anim[k].ty, 7), plain);
        d = compare(plain, cached, W, H);
        print_diff("  the group's commands drawn without groups / plain", d);
        CHECK(d.max <= 24);
        CHECK(d.mean < 0.05);
    }

    /* drawn once while only the transform changes; again for new content; let go when the group goes */
    cui_set_layer_cache(ui, true);
    render(sw, frame(ui, 0.9f, 0.1f, 0, -6, 9), cached);
    cui_sw_layer_stats(sw, &r0, NULL, NULL);
    for (int f = 1; f <= 10; f++) render(sw, frame(ui, 0.96f + 0.004f * f, 0.1f * f, 0, -6 + 0.6f * f, 9), cached);
    cui_sw_layer_stats(sw, &r1, &held, NULL);
    printf("  10 frames moving: %d bitmaps drawn, %zu bytes held\n", r1 - r0, held);
    CHECK(r1 == r0);
    CHECK(held > 0 && held <= (size_t)(g_panel.w + 120) * (g_panel.h + 140) * 4);
    g_content = 1;
    render(sw, frame(ui, 0.98f, 0.8f, 0, -1.5f, 9), cached);
    render(sw, frame(ui, 0.99f, 0.9f, 0, -1, 9), cached);
    cui_sw_layer_stats(sw, &r1, NULL, NULL);
    printf("  content changed: %d bitmap drawn\n", r1 - r0);
    CHECK(r1 == r0 + 1);
    cui_set_layer_cache(ui, false);
    render(sw, frame(ui, 0.99f, 0.9f, 0, -1, 9), plain);
    diff d = compare(plain, cached, W, H);
    CHECK(d.mean < 0.6);
    g_content = 0;
    cui_set_layer_cache(ui, true);
    render(sw, frame(ui, 1, -1, 0, 0, 9), cached);
    cui_sw_layer_stats(sw, NULL, &held, NULL);
    CHECK(held == 0);

    /* a group too big for the cache is drawn the plain way */
    cui_rect keep = g_panel;
    g_panel = (cui_rect){ -900, -900, 2200, 2200 };
    cui_sw_layer_stats(sw, &r0, NULL, NULL);
    render(sw, frame(ui, 0.97f, 0.7f, 0, -3, 11), cached);
    cui_sw_layer_stats(sw, &r1, &held, NULL);
    CHECK(r1 == r0 && held == 0);
    cui_set_layer_cache(ui, false);
    render(sw, frame(ui, 0.97f, 0.7f, 0, -3, 11), plain);
    d = compare(plain, cached, W, H);
    print_diff("a group too big for the cache / plain", d);
    CHECK(d.max <= 24 && d.mean < 0.05);
    g_panel = keep;

    cui_image_destroy(ui, g_bg);
    cui_image_destroy(ui, g_icon);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    free(bgpx); free(plain); free(cached); free(bg); free(opaque); free(ref);
}

/* ---- a real popover ---- */

static bool g_open_req, g_close_req;
static void pop_frame(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        cui_draw_image(ui, g_bg, (cui_rect){ 0, 0, ui->vw, ui->vh }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
        if (g_open_req) { cui_popover_open(ui, "p"); g_open_req = false; }
        if (g_close_req) { cui_popover_close(ui, "p"); g_close_req = false; }
        if (cui_popover_at_begin(ui, "p", ui->vw - 60, 50, CUI_PLACE_BOTTOM_END, &(cui_popover_opts){ .width = 330, .pad = 14 })) {
            cui_text(ui, "news.example", .font = CUI_FONT_MEDIUM, .size = 15);
            cui_text(ui, "12 trackers and ads blocked on this page", .color = CUI_COLOR_TEXT_SECONDARY);
            static bool on[3] = { true, false, true };
            cui_switch(ui, "Protection on this site", &on[0]);
            cui_switch(ui, "Block third-party cookies", &on[1]);
            cui_switch(ui, "Clean tracking parameters", &on[2]);
            cui_button(ui, "Settings", .variant = CUI_BUTTON_SECONDARY);
            cui_popover_end(ui);
        }
    }
}

/* one context's textures in two renderers (same handles: both start empty and get the same calls) */
static cui_renderer_iface g_tee[2];
static cui_texture tee_create(void *user, int w, int h, cui_texture_format fmt, const void *px) {
    (void)user;
    cui_texture a = g_tee[0].texture_create(g_tee[0].user, w, h, fmt, px), b = g_tee[1].texture_create(g_tee[1].user, w, h, fmt, px);
    CHECK(a == b);
    return a;
}
static void tee_update(void *user, cui_texture t, int x, int y, int w, int h, const void *px, int stride) {
    (void)user;
    for (int k = 0; k < 2; k++) g_tee[k].texture_update(g_tee[k].user, t, x, y, w, h, px, stride);
}
static void tee_destroy(void *user, cui_texture t) {
    (void)user;
    for (int k = 0; k < 2; k++) g_tee[k].texture_destroy(g_tee[k].user, t);
}

typedef struct { int n; cui_hit h[256]; } hits;
static void keep_hits(cui_ctx *ui, hits *o) {
    o->n = CUI_MIN(ui->hit_count, 256);
    memcpy(o->h, ui->hits, sizeof *o->h * (size_t)o->n);
}

static void test_popover(int w, int h, float scale, int threads) {
    printf("a real popover (%dx%d at %.0fx, %d threads)\n", w, h, (double)scale, threads);
    /* two contexts fed the same frames: one with the cache, one without */
    cui_sw_renderer *sw_c = cui_sw_create(), *sw_p = cui_sw_create(), *sw_whole = cui_sw_create();
    cui_ctx *uc = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw_c), .theme = CUI_THEME_DARK });
    cui_ctx *up = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw_p), .theme = CUI_THEME_DARK });
    int pw = (int)(w * scale), ph = (int)(h * scale);
    uint8_t *bgpx = malloc((size_t)pw * ph * 4);
    for (size_t i = 0; i < (size_t)pw * ph; i++) { bgpx[i * 4] = (uint8_t)(i * 13); bgpx[i * 4 + 1] = (uint8_t)(i / pw); bgpx[i * 4 + 2] = 90; bgpx[i * 4 + 3] = 255; }
    cui_ctx *both[2] = { uc, up };
    cui_texture bgs[2];
    for (int k = 0; k < 2; k++) {
        cui_set_viewport(both[k], (float)w, (float)h, scale);
        cui_set_reduced_motion(both[k], false);
        bgs[k] = cui_image_create(both[k], pw, ph, bgpx);
    }
    /* the whole-frame renderer draws uc's frames too: it needs the same textures, in the same order */
    cui_set_layer_cache(uc, true);
    cui_sw_set_incremental(sw_c, true);
    cui_sw_set_incremental(sw_p, false);
    cui_sw_set_threads(sw_c, threads);
    cui_sw_set_threads(sw_p, threads);
    uint8_t *pc = malloc((size_t)pw * ph * 4), *pp = malloc((size_t)pw * ph * 4), *pwh = malloc((size_t)pw * ph * 4);
    int groups_moving = 0, groups_rest = 0, frames = 0, hit_diffs = 0, inc_worst = 0, rest_worst = -1;
    size_t rest_held = 1;
    double scaled_mean = 0;
    int rasters0, phase_frames = 0, phase_rasters, compared = 0;
    cui_sw_layer_stats(sw_c, &rasters0, NULL, NULL);
    phase_rasters = rasters0;
    for (int f = 0; f < 3; f++)   /* springs snap on a context's first frames */
        for (int k = 0; k < 2; k++) {
            g_bg = bgs[k];
            cui_begin_frame(both[k], 1.0f / 60);
            pop_frame(both[k]);
            cui_end_frame(both[k]);
        }
    for (int phase = 0; phase < 3; phase++) {
        for (int i = 0; i < 90; i++) {
            if (i == 0 && phase == 0) g_open_req = true;
            if (i == 0 && phase == 2) g_close_req = true;
            bool req_o = g_open_req, req_c = g_close_req;
            hits hc, hp;
            const cui_draw_data *dd[2];
            for (int k = 0; k < 2; k++) {
                g_open_req = req_o; g_close_req = req_c;
                g_bg = bgs[k];
                cui_begin_frame(both[k], 1.0f / 60);
                pop_frame(both[k]);
                cui_end_frame(both[k]);
                dd[k] = cui_get_draw_data(both[k]);
                keep_hits(both[k], k ? &hp : &hc);
            }
            g_open_req = g_close_req = false;
            frames++;
            bool moving = uc->animating;
            if (dd[0]->group_count) {
                if (phase == 1) groups_rest++;
                else if (moving) groups_moving++;
            }
            if (phase == 1 && i == 2) {   /* at rest: no group, and the bitmap is gone after a frame */
                size_t now;
                cui_sw_layer_stats(sw_c, NULL, &now, NULL);
                rest_held = now;
            }
            hit_diffs += hc.n != hp.n || memcmp(hc.h, hp.h, sizeof *hc.h * (size_t)hc.n) != 0;
            cui_sw_render(sw_c, dd[0], pc, pw, ph, pw * 4, CUI_SW_BGRA);
            cui_sw_render(sw_p, dd[1], pp, pw, ph, pw * 4, CUI_SW_BGRA);
            if (phase == 1) {   /* at rest: from the bitmap, pixel for pixel */
                diff d = compare(pc, pp, pw, ph);
                if (d.max > rest_worst) rest_worst = d.max;
            } else if (dd[0]->group_count && dd[0]->groups[0].opacity > 0.95f) {   /* nearly opaque: as the plain drawing */
                diff d = compare(pc, pp, pw, ph);
                if (d.mean > scaled_mean) scaled_mean = d.mean;
                compared++;
            }
            if (!moving && i > 3 && phase != 1) break;
            if (phase == 1 && i > 3) break;
        }
        int rs;
        cui_sw_layer_stats(sw_c, &rs, NULL, NULL);
        printf("  %s: %d frames, %d bitmaps drawn\n", phase == 0 ? "opening" : phase == 1 ? "at rest" : "closing", frames - phase_frames, rs - phase_rasters);
        phase_frames = frames;
        phase_rasters = rs;
    }
    /* incremental frames against whole frames: the same frames again on a fresh pair */
    int rasters1;
    size_t held, peak;
    cui_sw_layer_stats(sw_c, &rasters1, &held, &peak);
    printf("  %d frames: %d with a moving group, %d at rest; %d bitmaps drawn, %zu bytes at most, %zu held at rest, %zu after\n",
           frames, groups_moving, groups_rest, rasters1 - rasters0, peak, rest_held, held);
    printf("  at rest / plain: max %d; moving with opacity > 0.95 / plain: worst mean %.4f (%d frames); frames with other hit areas: %d\n",
           rest_worst, scaled_mean, compared, hit_diffs);
    CHECK(compared >= 2);
    CHECK(groups_moving >= 4);
    CHECK(groups_rest == 0 && rest_held == 0);
    CHECK(rasters1 - rasters0 >= 1 && rasters1 - rasters0 <= 3);
    CHECK(held == 0);
    CHECK(rest_worst >= 0 && rest_worst <= 2);
    CHECK(scaled_mean < 0.6);
    CHECK(hit_diffs == 0);
    CHECK(uc->draw_data.group_count == 0);
    /* incremental against whole, with the cache: one context, two renderers over the same draw data */
    cui_sw_destroy(sw_whole);
    sw_whole = NULL;
    {
        cui_sw_renderer *inc = cui_sw_create(), *whole = cui_sw_create();
        g_tee[0] = cui_sw_iface(inc);
        g_tee[1] = cui_sw_iface(whole);
        cui_renderer_iface tee;
        memset(&tee, 0, sizeof tee);
        tee.texture_create = tee_create; tee.texture_update = tee_update; tee.texture_destroy = tee_destroy;
        cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = tee, .theme = CUI_THEME_DARK });
        cui_set_viewport(ui, (float)w, (float)h, scale);
        cui_set_reduced_motion(ui, false);
        cui_set_layer_cache(ui, true);
        g_bg = cui_image_create(ui, pw, ph, bgpx);
        cui_sw_set_incremental(inc, true);
        cui_sw_set_incremental(whole, false);
        cui_sw_set_threads(inc, threads);
        cui_sw_set_threads(whole, 1);
        for (int f = 0; f < 3; f++) { cui_begin_frame(ui, 1.0f / 60); pop_frame(ui); cui_end_frame(ui); }
        for (int phase = 0; phase < 2; phase++) {
            if (phase == 0) g_open_req = true; else g_close_req = true;
            for (int i = 0; i < 90; i++) {
                cui_begin_frame(ui, 1.0f / 60);
                pop_frame(ui);
                cui_end_frame(ui);
                const cui_draw_data *dd = cui_get_draw_data(ui);
                cui_sw_render(inc, dd, pc, pw, ph, pw * 4, CUI_SW_BGRA);
                cui_sw_render(whole, dd, pwh, pw, ph, pw * 4, CUI_SW_BGRA);
                diff d = compare(pc, pwh, pw, ph);
                if (d.max > inc_worst) inc_worst = d.max;
                if (i > 3 && !ui->animating) break;
            }
        }
        printf("  incremental frames with %d threads / whole frames on one: max %d\n", threads, inc_worst);
        CHECK(inc_worst == 0);
        cui_image_destroy(ui, g_bg);
        cui_destroy(ui);
        cui_sw_destroy(inc);
        cui_sw_destroy(whole);
    }
    for (int k = 0; k < 2; k++) { cui_image_destroy(both[k], bgs[k]); cui_destroy(both[k]); }
    cui_sw_destroy(sw_c);
    cui_sw_destroy(sw_p);
    free(bgpx); free(pc); free(pp); free(pwh);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--shots") && i + 1 < argc) g_shots = argv[++i];
    test_drawn_groups();
    test_popover(1200, 800, 1, 1);
    test_popover(900, 600, 2, 4);
    test_popover(2400, 1500, 1, 4);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
