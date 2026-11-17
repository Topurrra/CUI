/* A browser-like window through cui_app, timed per step: what a popover opening and closing and a
 * long list scrolling cost the whole process (all threads), with either renderer.
 *
 *     cui_bench_sw_app [--renderer software|opengl] [--size WxH] [--scale S] [--cycles N] [--steps N]
 *                      [--threads N] [--no-picture]
 *
 * The window: a toolbar, a page area (a full-size picture stands in for the page while a popover is
 * open, as a browser shows a snapshot of its native page), and a side panel with a virtual list of
 * 600 rows (icon, title, time). Then:
 *   popover  N cycles (default 6): open, wait until it settles, close, wait until it settles
 *   scroll   N wheel steps (default 240) over the list, half down then half up, each waited out
 * For each section it prints frames, wall time, the process CPU (every thread), the UI thread's CPU,
 * and the sums of the frame phases from the frame probe: build (input to the draw list), raster
 * (the renderer) and present (the blit or the swap). Prints; checks nothing. Windows reads the CPU
 * of the process and the thread; elsewhere those columns are 0. */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include "../src/cui_internal.h"
#include "../src/platform/cui_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
static double ft_ms(FILETIME a, FILETIME b) {
    ULARGE_INTEGER x, y;
    x.LowPart = a.dwLowDateTime; x.HighPart = a.dwHighDateTime;
    y.LowPart = b.dwLowDateTime; y.HighPart = b.dwHighDateTime;
    return (double)(x.QuadPart + y.QuadPart) / 10000.0;
}
static double proc_cpu_ms(void) { FILETIME c, e, k, u; return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? ft_ms(k, u) : 0; }
static double thread_cpu_ms(void) { FILETIME c, e, k, u; return GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u) ? ft_ms(k, u) : 0; }
static void put_env(const char *kv) { _putenv(kv); }
#else
#include <time.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6; }
static double proc_cpu_ms(void) { return 0; }
static double thread_cpu_ms(void) { return 0; }
static void put_env(const char *kv) { putenv((char *)kv); }
#endif

#define ROWS 600

/* ---- the frame probe: phase sums ---- */
static struct {
    double mark[CUI_PROBE_WAKE + 1];
    double build, raster, present;
} P;
static void probe(void *user, cui_probe_mark m) {
    (void)user;
    double t = now_ms();
    P.mark[m] = t;
    if (m == CUI_PROBE_PRESENT) P.raster += t - P.mark[CUI_PROBE_RENDER];
    else if (m == CUI_PROBE_DONE) P.present += t - P.mark[CUI_PROBE_PRESENT];
    else if (m == CUI_PROBE_PAINT_END && P.mark[CUI_PROBE_BEGIN] > 0) P.build += t - P.mark[CUI_PROBE_BEGIN];
}

/* ---- the window ---- */
static struct {
    cui_texture pic;
    int pic_w, pic_h;
    bool use_picture;
    bool want_open;
} W;

static const char *const words[] = { "news", "weather", "the", "renderer", "draws", "only", "what", "changed", "page", "about",
                                     "privacy", "tracker", "list", "home", "docs", "search", "results", "for", "browser" };

static void row(cui_ctx *ui, int i) {
    char title[96], when[16];
    unsigned s = (unsigned)i * 2654435761u;
    int n = 0;
    for (int k = 0; k < 3 + (int)(s % 5); k++) {
        s = s * 1103515245u + 12345u;
        n += snprintf(title + n, sizeof title - (size_t)n, "%s%s", k ? " " : "", words[(s >> 8) % (sizeof words / sizeof *words)]);
        if (n >= (int)sizeof title - 16) break;
    }
    snprintf(when, sizeof when, "%02d:%02d", (int)(s >> 4) % 24, (int)(s >> 12) % 60);
    cui_push_id_int(ui, i);
    cui_row(ui, .width = CUI_FILL, .height = CUI_PX(34), .pad_left = 12, .pad_right = 12, .gap = 8, .align = CUI_ALIGN_CENTER) {
        cui_icon(ui, CUI_ICON_GLOBE, 14, CUI_C(ui, TEXT_MUTED));
        cui_text(ui, title, .width = CUI_FILL, .nowrap = true, .size = 13);
        cui_text(ui, when, .color = CUI_COLOR_TEXT_MUTED, .size = 12);
    }
    cui_pop_id(ui);
}

static void paint_pic(cui_ctx *ui, cui_rect r, void *user) {
    (void)user;
    cui_draw_image(ui, W.pic, r, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), (cui_color){ 255, 255, 255, 255 });
}

static void frame(cui_ctx *ui) {
    bool open = cui_popover_is_open(ui, "pop");
    if (W.want_open != open) { if (W.want_open) cui_popover_open(ui, "pop"); else cui_popover_close(ui, "pop"); }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        cui_row(ui, .width = CUI_FILL, .height = CUI_PX(44), .pad = 6, .gap = 6, .align = CUI_ALIGN_CENTER, .bg = CUI_COLOR_SURFACE) {
            cui_icon_button(ui, CUI_ICON_ARROW_LEFT, .tooltip = "Back");
            cui_icon_button(ui, CUI_ICON_ARROW_RIGHT, .tooltip = "Forward");
            cui_text(ui, "https://news.example/a/long/address/of/a/page.html", .width = CUI_FILL, .nowrap = true);
            cui_popover(ui, "pop", .placement = CUI_PLACE_BOTTOM_END, .width = 330, .pad = 14) {
                cui_text(ui, "news.example", .font = CUI_FONT_MEDIUM, .size = 15);
                cui_text(ui, "12 trackers and ads blocked on this page", .color = CUI_COLOR_TEXT_SECONDARY);
                for (int i = 0; i < 6; i++) cui_switch(ui, cui_frame_strndup(ui, i % 2 ? "Block trackers and ads" : "Clean tracking parameters", -1), &(bool){ i % 2 == 0 });
                cui_button(ui, "Settings", .variant = CUI_BUTTON_SECONDARY);
            }
            cui_icon_button(ui, CUI_ICON_SHIELD, .tooltip = "Shields");
            cui_icon_button(ui, CUI_ICON_ELLIPSIS, .tooltip = "Menu");
        }
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
            if (open && W.use_picture) cui_canvas(ui, "page", CUI_FILL, CUI_FILL, paint_pic, NULL, 0);
            else cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {}
            cui_column(ui, .width = CUI_PX(360), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE, .pad = 6) {
                int first, last;
                cui_virtual_list(ui, "rows", ROWS, 34, CUI_FILL, &first, &last) {
                    for (int i = first; i < last; i++) row(ui, i);
                }
            }
        }
    }
}

/* ---- the run ---- */
typedef struct { const char *name; int frames; double wall, cpu, ui_cpu, build, raster, present; } section;
static void section_begin(section *s, const char *name) {
    memset(s, 0, sizeof *s);
    s->name = name;
    memset(&P, 0, sizeof P);
    s->wall = now_ms(); s->cpu = proc_cpu_ms(); s->ui_cpu = thread_cpu_ms();
}
static void section_end(section *s) {
    s->wall = now_ms() - s->wall; s->cpu = proc_cpu_ms() - s->cpu; s->ui_cpu = thread_cpu_ms() - s->ui_cpu;
    s->build = P.build; s->raster = P.raster; s->present = P.present;
}
static void section_print(const section *s, int actions) {
    printf("%-8s %6d %9.0f %9.0f %9.0f %9.1f %9.1f %9.1f %9.1f\n", s->name, s->frames, s->wall, s->cpu, s->ui_cpu,
           s->build, s->raster, s->present, actions ? s->cpu / actions : 0);
}

int main(int argc, char **argv) {
    const char *renderer = "software";
    int w = 1200, h = 800, cycles = 6, steps = 240, threads = 0;
    float scale = 0;
    W.use_picture = true;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--renderer") && i + 1 < argc) renderer = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &w, &h);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) cycles = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--steps") && i + 1 < argc) steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-picture")) W.use_picture = false;
    }
    bool sw = strcmp(renderer, "opengl") != 0;
    static char env_r[64], env_s[64];
    snprintf(env_r, sizeof env_r, "CUI_RENDERER=%s", sw ? "software" : "opengl");
    put_env(env_r);
    if (scale > 0) { snprintf(env_s, sizeof env_s, "CUI_SCALE=%g", (double)scale); put_env(env_s); }
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "cui_bench_sw_app", .width = w, .height = h, .theme = CUI_THEME_DARK,
                                                    .renderer = sw ? CUI_RENDERER_SOFTWARE : CUI_RENDERER_OPENGL });
    if (!app) { fprintf(stderr, "no window\n"); return 1; }
    cui_ctx *ui = cui_app_ui(app);
    cui_set_reduced_motion(ui, false);
    if (threads > 0 && app->sw) cui_sw_set_threads(app->sw, threads);
    cui_set_probe(probe, NULL);

    /* states: 0 warm, 1 popover, 2 scroll, 3 done */
    int state = 0, action = 0, since = 0, frames = 0;
    section sec[2];
    bool waiting = false;
    while (cui_app_frame(app)) {
        ui = cui_app_ui(app);
        if (!W.pic) {   /* the stand-in for the page: noise-free, like a page (flat areas and text-like stripes) */
            float s = cui_dpi_scale(ui);
            cui_vec2 sz = cui_app_size(app);
            W.pic_w = (int)((sz.x - 360) * s); W.pic_h = (int)((sz.y - 44) * s);
            if (W.pic_w < 1) W.pic_w = 1;
            if (W.pic_h < 1) W.pic_h = 1;
            uint8_t *px = malloc((size_t)W.pic_w * (size_t)W.pic_h * 4);
            for (int y = 0; y < W.pic_h; y++)
                for (int x = 0; x < W.pic_w; x++) {
                    uint8_t *p = px + ((size_t)y * (size_t)W.pic_w + (size_t)x) * 4;
                    bool ink = (y / 6) % 4 == 1 && (x / 9) % 7 != 3 && x % 300 < 260;
                    uint8_t v = ink ? 40 : (uint8_t)(235 + (y * 7 / (W.pic_h + 1)));
                    p[0] = v; p[1] = v; p[2] = (uint8_t)(ink ? 60 : v); p[3] = 255;
                }
            W.pic = cui_image_create(ui, W.pic_w, W.pic_h, px);
            free(px);
        }
        frames++;
        if (state > 0 && state < 3) sec[state - 1].frames++;
        bool settled = since >= 3 && !ui->animating;
        since++;
        if (state == 0 && frames >= 30) {
            state = 1; action = 0; since = 0; waiting = false;
            section_begin(&sec[0], "popover");
        }
        if (state == 1 && (!waiting || settled)) {
            if (action == 2 * cycles) { section_end(&sec[0]); state = 2; action = 0; waiting = false; section_begin(&sec[1], "scroll"); }
            else { W.want_open = action % 2 == 0; action++; since = 0; waiting = true; }
        }
        if (state == 2) {
            cui_vec2 sz = cui_app_size(app);
            cui_input_mouse_move(ui, sz.x - 180, sz.y * 0.5f);
            if (!waiting || settled) {
                if (action == steps) { section_end(&sec[1]); state = 3; }
                else { cui_input_scroll(ui, 0, action < steps / 2 ? -3.0f : 3.0f); action++; since = 0; waiting = true; }
            }
        }
        frame(ui);
        if (state == 3) break;
        if (state < 3) cui_request_redraw(ui);
    }
    cui_set_probe(NULL, NULL);
    cui_vec2 sz = cui_app_size(app);
    printf("%s, %.0fx%.0f pt at scale %.2g%s, %d popover cycles, %d wheel steps (ms)\n", sw ? "software" : "opengl", sz.x, sz.y,
           (double)cui_dpi_scale(ui), W.use_picture ? "" : ", no picture", cycles, steps);
    printf("%-8s %6s %9s %9s %9s %9s %9s %9s %9s\n", "section", "frames", "wall", "cpu", "ui cpu", "build", "raster", "present", "cpu/act");
    section_print(&sec[0], cycles);
    section_print(&sec[1], steps);
    cui_image_destroy(ui, W.pic);
    cui_app_destroy(app);
    return 0;
}
