#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* The cell grid over a table of 10,000,000 rows by 2,000 columns, full screen: frame times while idle, scrolling
 * down, scrolling sideways, jumping to far cells, moving the cursor by key, and dragging the scroll bar, with
 * the count of text draws and vertices a frame, and the memory the grid adds.
 *
 *     cui_bench_cellgrid [--rows 10000000] [--cols 2000] [--size 1920x1080] [--scale 1] [--frames 300] [--raster]
 *
 * A frame is cui_begin_frame to cui_end_frame: the callback's text, layout and the draw list. --raster adds the
 * software renderer's time (cui_sw_render into a pixel buffer), as the GL renderer's own cost is the GPU's.
 * Headless, release build. The budget is a median under 2 ms for a full screen of cells. */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
static double mem_mb(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return (double)m.PrivateUsage / 1048576.0;
}
#else
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6; }
static double mem_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return (double)rss * 4096.0 / 1048576.0;
}
#endif

typedef struct {
    int rows, cols;
    float *widths;
    unsigned char *right;
    cui_cellgrid_sel sel;
    cui_table_sort sort;
    long calls;
} app;

static const char *cell(void *ctx, int row, int col, char *buf, size_t cap) {
    app *a = ctx;
    a->calls++;
    if (row < 0) { snprintf(buf, cap, "Column %d", col + 1); return buf; }
    if (col < 0) { snprintf(buf, cap, "%d", row + 1); return buf; }
    switch (col % 4) {
    case 0: snprintf(buf, cap, "Customer %d-%d", row, col); break;
    case 1: snprintf(buf, cap, "%d", (row * 7 + col * 13) % 100000); break;
    case 2: snprintf(buf, cap, "%.2f", (double)((row * 31 + col) % 100000) / 100.0); break;
    default: snprintf(buf, cap, "2026-%02d-%02d", row % 12 + 1, col % 28 + 1); break;
    }
    return buf;
}

static void build(cui_ctx *ui, app *a) {
    cui_cellgrid(ui, "grid", a->rows, a->cols, .cell = cell, .ctx = a, .col_widths = a->widths, .header = true, .row_numbers = true,
                 .right_align = a->right, .sort = &a->sort, .sel = &a->sel, .striped = true, .grid_lines = true);
}

typedef struct { double *t; int n; } samples;
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void report(const char *name, samples *s, long calls, int vtx, int cmds) {
    qsort(s->t, (size_t)s->n, sizeof(double), cmp_d);
    printf("  %-34s median %6.3f ms  p95 %6.3f  worst %6.3f   %5ld cells  %6d vertices  %3d draw commands\n", name, s->t[s->n / 2],
           s->t[(int)((double)s->n * 0.95)], s->t[s->n - 1], calls, vtx, cmds);
}

typedef struct { cui_ctx *ui; cui_sw_renderer *sw; app *a; unsigned char *px; bool raster; } rig;

static double one_frame(rig *r) {
    double t0 = now_ms();
    cui_begin_frame(r->ui, 1.0f / 60.0f);
    build(r->ui, r->a);
    cui_end_frame(r->ui);
    double t1 = now_ms();
    if (r->raster) {
        const cui_draw_data *dd = cui_get_draw_data(r->ui);
        cui_sw_render(r->sw, dd, r->px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_BGRA);
        return now_ms() - t0;
    }
    return t1 - t0;
}

/* mode: 0 idle, 1 wheel down, 2 shift+wheel, 3 cursor jumps, 4 Down key, 5 scroll bar drag */
static void phase(rig *r, const char *name, int mode, int n) {
    samples s = { malloc(sizeof(double) * (size_t)n), n };
    unsigned seed = 4242;
    float W = 0, H = 0;
    cui_rect gr = cui_get_rect(r->ui, cui_make_id(r->ui, "grid"));
    W = gr.w; H = gr.h;
    cui_input_mouse_move(r->ui, gr.x + W * 0.5f, gr.y + H * 0.5f);
    if (mode == 2) cui_input_key(r->ui, CUI_KEY_NONE, true, CUI_MOD_SHIFT);
    if (mode == 5) { cui_input_mouse_move(r->ui, gr.x + W - 5, gr.y + 40); one_frame(r); cui_input_mouse_button(r->ui, CUI_MOUSE_LEFT, true); one_frame(r); }
    r->a->calls = 0;
    for (int i = 0; i < n; i++) {
        if (mode == 1) cui_input_scroll(r->ui, 0, -0.6f);
        if (mode == 2) cui_input_scroll(r->ui, 0, -0.6f);
        if (mode == 3) {
            seed = seed * 1664525u + 1013904223u;
            int row = (int)(seed >> 8) % r->a->rows;
            seed = seed * 1664525u + 1013904223u;
            int col = (int)(seed >> 8) % r->a->cols;
            r->a->sel = (cui_cellgrid_sel){ row, col, row, col };
        }
        if (mode == 4) { cui_input_key(r->ui, CUI_KEY_DOWN, true, 0); }
        if (mode == 5) cui_input_mouse_move(r->ui, gr.x + W - 5, gr.y + 40 + (H - 80) * (float)i / (float)n);
        s.t[i] = one_frame(r);
        if (mode == 4) cui_input_key(r->ui, CUI_KEY_DOWN, false, 0);
    }
    if (mode == 2) cui_input_key(r->ui, CUI_KEY_NONE, false, 0);
    if (mode == 5) { cui_input_mouse_button(r->ui, CUI_MOUSE_LEFT, false); one_frame(r); }
    const cui_draw_data *dd = cui_get_draw_data(r->ui);
    report(name, &s, r->a->calls / n, dd->vertex_count, dd->command_count);
    free(s.t);
}

int main(int argc, char **argv) {
    int rows = 10000000, cols = 2000, W = 1920, H = 1080, frames = 300;
    float scale = 1.0f;
    bool raster = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rows") && i + 1 < argc) rows = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cols") && i + 1 < argc) cols = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = (float)atof(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &W, &H) != 2) return 2; }
        else if (!strcmp(argv[i], "--raster")) raster = true;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    app a;
    memset(&a, 0, sizeof a);
    a.rows = rows; a.cols = cols;
    a.widths = malloc(sizeof(float) * (size_t)cols);
    a.right = malloc((size_t)cols);
    for (int k = 0; k < cols; k++) { a.widths[k] = 80.0f + (float)((k * 37) % 90); a.right[k] = (k % 4 == 1 || k % 4 == 2) ? 1 : 0; }
    a.sort.column = -1;
    a.sel = (cui_cellgrid_sel){ 0, 0, 0, 0 };
    double m0 = mem_mb();
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, (float)W / scale, (float)H / scale, scale);
    cui_set_reduced_motion(ui, true);
    rig r = { ui, sw, &a, malloc((size_t)W * (size_t)H * 4), raster };
    printf("cell grid: %d rows x %d columns, %dx%d px, scale %.2f%s\n", rows, cols, W, H, (double)scale, raster ? ", with the software raster" : "");
    double t0 = now_ms();
    one_frame(&r);
    printf("  first frame %.2f ms\n", now_ms() - t0);
    for (int i = 0; i < 5; i++) one_frame(&r);
    phase(&r, "idle (same cells again)", 0, frames);
    phase(&r, "wheel down (0.6 notch a frame)", 1, frames);
    phase(&r, "Shift+wheel sideways", 2, frames);
    phase(&r, "cursor jumps to a random far cell", 3, frames);
    phase(&r, "Down key held", 4, frames);
    phase(&r, "dragging the scroll bar", 5, frames);
    printf("  memory added: %.1f MB (the frame arena, the column edges for %d columns, the renderer's buffers)\n", mem_mb() - m0, cols);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    free(r.px); free(a.widths); free(a.right);
    return 0;
}
