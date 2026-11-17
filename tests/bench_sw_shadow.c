/* What a popover costs the software renderer: one floating shadow, a full-window picture under it,
 * and a real popover opened and closed with its animation, at 1200x800, 1900x1000 and 3800x2000.
 * Prints milliseconds; checks nothing. Usage: cui_bench_sw_shadow [--threads N] [--reps N] */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

static cui_sw_renderer *g_sw;
static cui_texture g_pic;
static int g_what;   /* bits: 1 picture, 2 shadow, 4 popover body */
static float g_pw = 360, g_ph = 440;

static void scene(cui_ctx *ui) {
    cui_rect full = { 0, 0, ui->vw, ui->vh };
    cui_draw_rect(ui, full, CUI_CORNERS(0), cui__rgba(30, 30, 34, 255));
    if (g_what & 1) cui_draw_image(ui, g_pic, full, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
    cui_rect pr = { full.w - g_pw - 40, 60, g_pw, g_ph };
    if (g_what & 2) cui__draw_elevation(ui, pr, CUI_CORNERS(10), CUI_ELEVATION_FLOATING, 1);
    if (g_what & 4) cui_draw_rect(ui, pr, CUI_CORNERS(10), cui__rgba(44, 44, 50, 255));
}

static double render_ms(cui_ctx *ui, uint8_t *px, int reps) {
    cui_begin_frame(ui, 1.0f / 60);
    scene(ui);
    cui_end_frame(ui);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);   /* warm */
    double best = 1e30;
    for (int i = 0; i < reps; i++) {
        double t0 = now_ms();
        cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);
        double t = now_ms() - t0;
        if (t < best) best = t;
    }
    return best;
}

/* a real popover: open it, render until it settles, close it, render until it settles (incremental, as an app) */
static bool g_open_req, g_close_req, g_pop_pic = true;
static void pop_frame(cui_ctx *ui) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        if (g_pop_pic) cui_draw_image(ui, g_pic, (cui_rect){ 0, 0, ui->vw, ui->vh }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0),
                                      cui__rgba(255, 255, 255, 255));
        if (g_open_req) { cui_popover_open(ui, "p"); g_open_req = false; }
        if (g_close_req) { cui_popover_close(ui, "p"); g_close_req = false; }
        if (cui_popover_at_begin(ui, "p", ui->vw - 60, 50, CUI_PLACE_BOTTOM_END, &(cui_popover_opts){ .width = 330, .pad = 14 })) {
            /* what a browser's shield popover holds: a title, a big number, lines, switches, buttons */
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
                cui_icon(ui, CUI_ICON_SHIELD, 18, CUI_C(ui, ACCENT));
                cui_text(ui, "news.example", .font = CUI_FONT_MEDIUM, .size = 14, .width = CUI_FILL, .nowrap = true);
            }
            cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_END) {
                cui_text(ui, "12", .font = CUI_FONT_MEDIUM, .size = 34);
                cui_text(ui, "trackers and ads blocked", .color = CUI_COLOR_TEXT_SECONDARY, .size = 13);
            }
            cui_text(ui, "on this page, including requests made after it loaded", .color = CUI_COLOR_TEXT_MUTED, .size = 11.5f);
            for (int i = 0; i < 3; i++) cui_text(ui, "3 scripts, 1 image, 1 frame: about 240 KB not loaded", .size = 12.5f);
            static bool on[3] = { true, false, true };
            cui_switch(ui, "Protection on this site", &on[0]);
            cui_switch(ui, "Block third-party cookies", &on[1]);
            cui_switch(ui, "Clean tracking parameters", &on[2]);
            cui_row(ui, .width = CUI_FILL, .gap = 8) {
                cui_button(ui, "Settings", .variant = CUI_BUTTON_SECONDARY);
                cui_button(ui, "Privacy and speed", .variant = CUI_BUTTON_GHOST);
            }
            cui_popover_end(ui);
        }
    }
}
static void pop_cycle(cui_ctx *ui, uint8_t *px, int *frames, double *ms, bool pic, bool shadow) {
    *frames = 0; *ms = 0;
    g_pop_pic = pic;
    for (int i = 0; i < 2; i++) ui->theme.shadow_floating[i].alpha = ui->target_theme.shadow_floating[i].alpha = shadow ? (i ? 0.24f : 0.40f) : 0;
    for (int phase = 0; phase < 2; phase++) {
        if (phase == 0) g_open_req = true; else g_close_req = true;
        for (int i = 0; i < 120; i++) {
            cui_begin_frame(ui, 1.0f / 60);
            pop_frame(ui);
            cui_end_frame(ui);
            const cui_draw_data *dd = cui_get_draw_data(ui);
            double t0 = now_ms();
            cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);
            *ms += now_ms() - t0;
            (*frames)++;
            if (i > 2 && !ui->animating) break;
        }
    }
}

int main(int argc, char **argv) {
    int threads = 0, reps = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    }
    g_sw = cui_sw_create();
    if (threads > 0) cui_sw_set_threads(g_sw, threads);
    static const struct { int w, h; float s; } sizes[3] = { { 1200, 800, 1 }, { 1900, 1000, 1 }, { 1900, 1000, 2 } };
    printf("full frames, best of %d, ms (threads %s)\n", reps, threads > 0 ? argv[2] : "default");
    printf("%-11s %8s %8s %8s %8s %10s | %s\n", "pixels", "bg", "+shadow", "shadow", "+picture", "+pic+shad", "popover open+close: frames, ms; without shadow; without picture");
    for (int k = 0; k < 3; k++) {
        cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
        cui_set_viewport(ui, (float)sizes[k].w, (float)sizes[k].h, sizes[k].s);
        cui_set_reduced_motion(ui, false);
        int pw = (int)(sizes[k].w * sizes[k].s), ph = (int)(sizes[k].h * sizes[k].s);
        uint8_t *pic = malloc((size_t)pw * ph * 4), *px = malloc((size_t)pw * ph * 4);
        for (size_t i = 0; i < (size_t)pw * ph * 4; i++) pic[i] = (uint8_t)((i * 2654435761u) >> 13);
        for (size_t i = 3; i < (size_t)pw * ph * 4; i += 4) pic[i] = 255;
        g_pic = cui_image_create(ui, pw, ph, pic);
        cui_sw_set_incremental(g_sw, false);
        g_what = 0; double bg = render_ms(ui, px, reps);
        g_what = 2 | 4; double sh = render_ms(ui, px, reps);
        g_what = 4; double body = render_ms(ui, px, reps);
        g_what = 1; double pic1 = render_ms(ui, px, reps);
        g_what = 1 | 2 | 4; double all = render_ms(ui, px, reps);
        cui_sw_set_incremental(g_sw, true);
        int fr; double ms;
        double ms_ns, ms_np;
        pop_cycle(ui, px, &fr, &ms, true, true);   /* warm: glyphs */
        pop_cycle(ui, px, &fr, &ms, true, true);
        pop_cycle(ui, px, &fr, &ms_ns, true, false);
        pop_cycle(ui, px, &fr, &ms_np, false, true);
        char name[32];
        snprintf(name, sizeof name, "%dx%d", pw, ph);
        printf("%-11s %8.2f %8.2f %8.2f %8.2f %10.2f | %d, %.1f; %.1f; %.1f\n", name, bg, sh, sh - body, pic1, all, fr, ms, ms_ns, ms_np);
        cui_image_destroy(ui, g_pic);
        cui_destroy(ui);
        free(pic); free(px);
        (void)bg;
    }
    cui_sw_destroy(g_sw);
    return 0;
}
