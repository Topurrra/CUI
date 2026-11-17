#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Scrolling a whole window with the software renderer, a frame at a time, with the time of
 * each phase (median, 95th percentile, worst).
 *
 *     cui_bench_scroll [options]
 *       --scene chat|editor|gallery
 *                             a chat of 5,000 messages (default), the code editor over a big
 *                             text, or the gallery's pages, scrolled with the wheel, a new page every 50 frames
 *       --frames N            frames measured (default 600, after 40 to warm up)
 *       --size WxH            window size in points (default 1180x780)
 *       --scale S             display scale (default 1.5)
 *       --step P              points scrolled per frame (default 53)
 *       --threads N           software renderer threads (default: as cui_app)
 *       --full                draw every frame whole (as without incremental drawing)
 *       --jitter              chat: the messages also move by a fraction of a pixel each frame
 *       --check               draw every frame incrementally and, with a second renderer, whole,
 *                             and compare; exits 1 when one differs (a ctest; the first is saved
 *                             as scroll-incremental.png and scroll-whole.png)
 *       --window              a real window through cui_app (CUI_RENDERER=software is set), timed
 *                             with the frame probe; the window draws and shows every frame
 *
 * Headless, a frame is build (cui_begin_frame to cui_end_frame) then raster (cui_sw_render). */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_render.h>
#include "../src/cui_internal.h"
#include "../examples/gallery_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
#else
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6; }
#endif

/* ---- the scenes ---- */

#define CHAT_ITEMS 5000
typedef struct {
    int scene;                 /* 0 chat, 1 editor, 2 gallery */
    gallery g;
    float step, y;             /* chat: points per frame and the offset */
    int frame;
    int dir;
    float off[CHAT_ITEMS + 1]; /* chat: top of each message */
    unsigned seed[CHAT_ITEMS];
    char *text; size_t cap;    /* editor */
} bench_scene;

static const char *const words[] = { "the", "renderer", "draws", "only", "what", "changed", "since", "last", "frame", "and",
    "a", "scroll", "moves", "pixels", "window", "tile", "hash", "compositor", "present", "budget", "of", "four", "milliseconds",
    "message", "with", "code", "blocks", "lines", "list", "virtual", "items", "per", "second" };

static int chat_lines(unsigned s) { return 1 + (int)(s % 5); }
static bool chat_code(unsigned s) { return (s >> 5) % 4 == 0; }
static float chat_height(unsigned s) { return 28.0f + 21.0f * (float)chat_lines(s) + (chat_code(s) ? 24.0f + 18.0f * 4 : 0) + 18.0f; }

static void scene_init(bench_scene *b) {
    if (b->scene == 0) {
        unsigned s = 12345;
        b->off[0] = 0;
        for (int i = 0; i < CHAT_ITEMS; i++) {
            s = s * 1103515245u + 12345u;
            b->seed[i] = s >> 8;
            b->off[i + 1] = b->off[i] + chat_height(b->seed[i]);
        }
    } else if (b->scene == 2) {
        gallery_init(&b->g);
    } else {
        b->cap = 3u << 20;
        b->text = malloc(b->cap);
        size_t n = 0;
        unsigned s = 777;
        for (int line = 0; n + 200 < b->cap - 1 - 200; line++) {
            s = s * 1103515245u + 12345u;
            int indent = (int)((s >> 9) % 4) * 4;
            if (line % 12 == 0) n += (size_t)sprintf(b->text + n, "/* block %d: what the code below does */\n", line / 12);
            else if (line % 12 == 1) n += (size_t)sprintf(b->text + n, "static int step_%d(const char *p, int n) {\n", line);
            else if (line % 12 == 11) n += (size_t)sprintf(b->text + n, "}\n\n");
            else {
                n += (size_t)sprintf(b->text + n, "%*sif (n > %u && p[%d] == '%c') return f(\"%s\", %d) + %s;\n", indent + 4, "", (s >> 3) % 100,
                                     line % 7, 'a' + (int)(s % 26), words[(s >> 4) % 33], line, words[(s >> 12) % 33]);
            }
        }
        b->text[n] = 0;
    }
}

static bool opt_jitter;
static const char *const scene_names[] = { "chat", "editor", "gallery" };

static int chat_find(const bench_scene *b, float y) {
    int lo = 0, hi = CHAT_ITEMS - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (b->off[m] <= y) lo = m; else hi = m - 1; }
    return lo;
}

static void chat_message(cui_ctx *ui, const bench_scene *b, int i) {
    unsigned s = b->seed[i];
    char line[160];
    cui_push_id_int(ui, i);
    cui_column(ui, .width = CUI_FILL, .height = CUI_PX(chat_height(s) - 18), .gap = 3) {
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .height = CUI_PX(25)) {
            cui_box(ui, .width = CUI_PX(20), .height = CUI_PX(20), .radius = 10, .bg = (s & 1) ? CUI_COLOR_ACCENT : CUI_COLOR_SURFACE_MUTED) {}
            cui_text(ui, (s & 1) ? "Claude" : "You", .font = CUI_FONT_MEDIUM, .size = 14);
            snprintf(line, sizeof line, "%d min ago", (int)(s % 59) + 1);
            cui_text(ui, line, .size = 12, .color = CUI_COLOR_TEXT_MUTED);
        }
        unsigned w = s;
        for (int l = 0; l < chat_lines(s); l++) {
            int n = 0;
            for (int k = 0; k < 12 && n < 120; k++) {
                w = w * 1103515245u + 12345u;
                n += snprintf(line + n, sizeof line - (size_t)n, "%s%s", k ? " " : "", words[(w >> 10) % 33]);
            }
            cui_text(ui, line, .size = 14, .nowrap = true);
        }
        if (chat_code(s)) {
            cui_column(ui, .width = CUI_FILL, .pad = 10, .radius = 8, .bg = CUI_COLOR_SURFACE_MUTED, .gap = 2) {
                for (int l = 0; l < 4; l++) {
                    snprintf(line, sizeof line, "for (int i = %d; i < n; i++) total += item[i].%s * %u;", l, words[(s >> l) % 33], s % 97);
                    cui_text(ui, line, .font = CUI_FONT_MONO, .size = 13, .nowrap = true);
                }
            }
        }
    }
    cui_pop_id(ui);
}

static void chat_frame(cui_ctx *ui, bench_scene *b) {
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .width = CUI_PX(240), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE, .pad = 12, .gap = 4) {
            cui_text(ui, "Sessions", .font = CUI_FONT_MEDIUM, .size = 15);
            for (int k = 0; k < 8; k++) {
                char t[48];
                snprintf(t, sizeof t, "Session %d: %s %s", k + 1, words[k * 3 % 33], words[(k * 7 + 2) % 33]);
                cui_push_id_int(ui, k);
                cui_row(ui, .width = CUI_FILL, .pad = 6, .radius = 6, .bg = k == 0 ? CUI_COLOR_SURFACE_MUTED : CUI_TRANSPARENT) {
                    cui_text(ui, t, .size = 13, .nowrap = true);
                }
                cui_pop_id(ui);
            }
        }
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
            cui_row(ui, .width = CUI_FILL, .pad = 12, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Build the UI shell", .font = CUI_FONT_MEDIUM, .size = 15);
                cui_spacer(ui);
                cui_text(ui, "5000 messages", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            }
            cui_separator(ui);
            /* the offset is set directly each frame, as an app sweeping its transcript does */
            cui_scroll_state *st = cui_state(ui, cui_make_id(ui, "scroll"), sizeof *st);
            float vh = st->view_h > 0 ? st->view_h : 600;
            float total = b->off[CHAT_ITEMS] + 48;
            float maxy = total > vh ? total - vh : 0;
            float y = b->y + (b->dir ? -b->step : b->step);
            if (y >= maxy) { y = maxy; b->dir = 1; }
            if (y <= 0) { y = 0; b->dir = 0; }
            b->y = st->y = st->ty = y;
            int first = chat_find(b, y - 24 - 200), last = first;
            while (last < CHAT_ITEMS && b->off[last] < y + vh + 200) last++;
            cui_scroll(ui, .id = "scroll", .width = CUI_FILL, .height = CUI_FILL) {
                cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .pad_x = 28) {
                    cui_column(ui, .width = CUI_FILL, .max_width = 760) {
                        /* --jitter: what is above moves by a fraction of a pixel each frame, as when
                         * an app measures items while it scrolls; lines of text land on whole pixels */
                        cui_gap(ui, 24 + b->off[first] + (opt_jitter ? 0.37f * (float)(b->frame++ % 7) : 0));
                        for (int i = first; i < last; i++) { chat_message(ui, b, i); cui_gap(ui, 18); }
                        cui_gap(ui, b->off[CHAT_ITEMS] - b->off[last] + 24);
                    }
                }
            }
            cui_separator(ui);
            cui_row(ui, .width = CUI_FILL, .pad = 12) {
                cui_box(ui, .width = CUI_FILL, .height = CUI_PX(44), .radius = 12, .border = 1, .bg = CUI_COLOR_SURFACE) {}
            }
        }
    }
}

static void scene_frame(cui_ctx *ui, bench_scene *b) {
    if (b->scene == 0) chat_frame(ui, b);
    else if (b->scene == 2) {
        /* Motion and Audio keep state outside the context, so a second run would not repeat
         * them and --check could not compare */
        do b->g.page = (b->frame++ / 50) % gallery_page_total();
        while (!strcmp(gallery_page_name(b->g.page), "Motion") || !strcmp(gallery_page_name(b->g.page), "Audio"));
        cui_input_mouse_move(ui, 700, 400);
        cui_input_scroll(ui, 0, b->frame % 50 < 30 ? -2.0f : 3.0f);
        gallery_frame(ui, &b->g);
    }
    else {
        cui_input_mouse_move(ui, 300, 300);
        cui_input_scroll(ui, 0, -b->step / 18.0f);   /* wheel notches; the editor turns them into lines */
        cui_code_editor(ui, "ed", b->text, b->cap, .lang = "c", .height = -1);
    }
}

/* ---- statistics ---- */

typedef struct { double *v; int n, cap; } series;
static void series_add(series *s, double x) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; s->v = realloc(s->v, sizeof(double) * (size_t)s->cap); }
    s->v[s->n++] = x;
}
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void series_print(const char *name, series *s) {
    if (!s->n) return;
    double sum = 0;
    for (int i = 0; i < s->n; i++) sum += s->v[i];
    qsort(s->v, (size_t)s->n, sizeof(double), cmp_d);
    printf("  %-14s mean %6.2f  p50 %6.2f  p95 %6.2f  max %6.2f ms\n", name, sum / s->n, s->v[s->n / 2], s->v[(int)(s->n * 0.95)], s->v[s->n - 1]);
}

/* ---- headless ---- */

/* --check: a second renderer gets every texture call too and draws each frame whole */
static cui_renderer_iface tee_a, tee_b;
static cui_texture tee_create(void *u, int w, int h, cui_texture_format f, const void *px) {
    (void)u;
    cui_texture t = tee_a.texture_create(tee_a.user, w, h, f, px);
    if (tee_b.texture_create(tee_b.user, w, h, f, px) != t) { fprintf(stderr, "texture handles differ\n"); exit(1); }
    return t;
}
static void tee_update(void *u, cui_texture t, int x, int y, int w, int h, const void *px, int stride) {
    (void)u;
    tee_a.texture_update(tee_a.user, t, x, y, w, h, px, stride);
    tee_b.texture_update(tee_b.user, t, x, y, w, h, px, stride);
}
static void tee_destroy(void *u, cui_texture t) { (void)u; tee_a.texture_destroy(tee_a.user, t); tee_b.texture_destroy(tee_b.user, t); }

/* runs the frames; with check, compares each with a whole repaint and returns how many differ */
static int run_headless(int scene, int w, int h, float scale, float step, int frames, int threads, bool incremental,
                        bool check, bool print) {
    bench_scene *b = calloc(1, sizeof *b);
    b->scene = scene; b->step = step;
    scene_init(b);
    cui_sw_renderer *sw = cui_sw_create(), *whole = check ? cui_sw_create() : NULL;
    if (threads > 0) cui_sw_set_threads(sw, threads);
    cui_sw_set_incremental(sw, incremental);
    cui_renderer_iface iface = cui_sw_iface(sw);
    if (check) {
        tee_a = iface;
        tee_b = cui_sw_iface(whole);
        iface.texture_create = tee_create; iface.texture_update = tee_update; iface.texture_destroy = tee_destroy;
    }
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = iface, .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, (float)w, (float)h, scale);
    int pw = (int)(w * scale + 0.5f), ph = (int)(h * scale + 0.5f);
    uint8_t *px = malloc((size_t)pw * (size_t)ph * 4), *px_whole = check ? malloc((size_t)pw * (size_t)ph * 4) : NULL;
    series build = { 0 }, raster = { 0 }, total = { 0 }, drawn = { 0 };
    double drawn_share = 0, moved_share = 0;
    int moved_frames = 0, bad = 0;
    int warm = check ? 0 : 40;
    for (int f = 0; f < warm + frames; f++) {
        double t0 = now_ms();
        cui_begin_frame(ui, 1.0f / 60.0f);
        scene_frame(ui, b);
        cui_end_frame(ui);
        double t1 = now_ms();
        const cui_draw_data *dd = cui_get_draw_data(ui);
        cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);
        double t2 = now_ms();
        if (check) {
            size_t bytes = (size_t)dd->width_px * (size_t)dd->height_px * 4;
            cui_sw_render(whole, dd, px_whole, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);
            if (memcmp(px, px_whole, bytes) != 0) {
                if (bad < 10) printf("frame %d: the incremental frame differs from the frame drawn whole\n", f);
                if (!bad) {
                    cui_write_png("scroll-incremental.png", px, dd->width_px, dd->height_px, dd->width_px * 4);
                    cui_write_png("scroll-whole.png", px_whole, dd->width_px, dd->height_px, dd->width_px * 4);
                }
                bad++;
            }
        }
        if (f < warm) continue;
        int dx, dy, dw, dh;
        series_add(&build, t1 - t0);
        series_add(&raster, t2 - t1);
        series_add(&total, t2 - t0);
        series_add(&drawn, cui_sw_last_damage(sw, &dx, &dy, &dw, &dh) ? 100.0 * dw * dh / ((double)dd->width_px * dd->height_px) : 0);
#ifndef BENCH_BASELINE
        int shift; float dr, mv;
        cui__sw_last_stats(sw, &shift, &dr, &mv);
        drawn_share += dr; moved_share += mv; moved_frames += shift != 0;
#endif
    }
    if (print) {
        printf("%s, %dx%d at %.2gx (%dx%d px), %.0f pt per frame, %s, %d frames\n", scene_names[scene], w, h, scale, pw, ph, step,
               incremental ? "incremental" : "drawn whole", frames);
        series_print("build", &build);
        series_print("raster", &raster);
        series_print("frame", &total);
        qsort(drawn.v, (size_t)drawn.n, sizeof(double), cmp_d);
        double sum = 0;
        for (int i = 0; i < drawn.n; i++) sum += drawn.v[i];
        printf("  damage box     mean %5.1f%% of the window; drawn %.1f%%, moved %.1f%%; %d of %d frames moved pixels\n", drawn.n ? sum / drawn.n : 0,
               100.0 * drawn_share / frames, 100.0 * moved_share / frames, moved_frames, frames);
    }
    free(build.v); free(raster.v); free(total.v); free(drawn.v);
    free(px);
    free(px_whole);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_sw_destroy(whole);
    free(b->text);
    free(b);
    return bad;
}

/* ---- in a window ---- */

typedef struct {
    double mark[CUI_PROBE_WAKE + 1];
    bool recording;
    series ph[9], frame, loop;
    double last_done;
} probe_rec;
static probe_rec rec;

static void probe(void *user, cui_probe_mark m) {
    (void)user;
    double t = now_ms();
    rec.mark[m] = t;
    if (m != CUI_PROBE_DONE) return;
    if (rec.recording) {
        static const int order[] = { CUI_PROBE_WAKE, CUI_PROBE_BEGIN, CUI_PROBE_BUILD, CUI_PROBE_LAYOUT, CUI_PROBE_PAINT, CUI_PROBE_PAINT_END,
                                     CUI_PROBE_RENDER, CUI_PROBE_PRESENT, CUI_PROBE_DONE };
        for (int k = 0; k + 1 < 9; k++) series_add(&rec.ph[k], rec.mark[order[k + 1]] - rec.mark[order[k]]);
        series_add(&rec.frame, t - rec.mark[CUI_PROBE_WAKE]);
        if (rec.last_done > 0) series_add(&rec.loop, t - rec.last_done);
    }
    rec.last_done = t;
}

static int run_window(int scene, int w, int h, float step, int frames) {
#if defined(_WIN32)
    _putenv("CUI_RENDERER=software");
#else
    setenv("CUI_RENDERER", "software", 1);
#endif
    bench_scene *b = calloc(1, sizeof *b);
    b->scene = scene; b->step = step;
    scene_init(b);
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "cui_bench_scroll", .width = w, .height = h, .theme = CUI_THEME_DARK,
                                                    .renderer = CUI_RENDERER_SOFTWARE });
    if (!app) return 1;
    cui_set_probe(probe, NULL);
    int f = 0;
    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        scene_frame(ui, b);
        cui_request_redraw(ui);
        if (++f == 40) rec.recording = true;
        if (f == 40 + frames) break;
    }
    cui_set_probe(NULL, NULL);
    cui_vec2 sz = cui_app_size(app);
    printf("%s in a window, %.0fx%.0f pt, software, %.0f pt per frame, %d frames\n", scene_names[scene], sz.x, sz.y, step, rec.frame.n);
    static const char *names[] = { "wake>begin", "begin", "build", "layout", "paint", "paint>render", "raster", "present" };
    for (int k = 0; k < 8; k++) series_print(names[k], &rec.ph[k]);
    series_print("frame", &rec.frame);
    series_print("done to done", &rec.loop);
    cui_app_destroy(app);
    free(b->text);
    free(b);
    return 0;
}

int main(int argc, char **argv) {
    int scene = 0, frames = 600, w = 1180, h = 780, threads = 0;
    float scale = 1.5f, step = 53;
    bool full = false, check = false, window = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scene") && i + 1 < argc) { i++; scene = !strcmp(argv[i], "editor") ? 1 : !strcmp(argv[i], "gallery") ? 2 : 0; }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &w, &h) != 2) return 2; }
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--step") && i + 1 < argc) step = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full")) full = true;
        else if (!strcmp(argv[i], "--jitter")) opt_jitter = true;
        else if (!strcmp(argv[i], "--check")) check = true;
        else if (!strcmp(argv[i], "--window")) window = true;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (frames < 1) frames = 1;
    if (window) return run_window(scene, w, h, step, frames);
    if (check) {
        int bad = run_headless(scene, w, h, scale, step, frames, threads, true, true, false);
        printf("%s, %dx%d at %.2gx, %.0f pt per frame: %d frames compared, %d differ\n", scene_names[scene], w, h, scale, step, frames, bad);
        return bad != 0;
    }
    run_headless(scene, w, h, scale, step, frames, threads, !full, false, true);
    return 0;
}
