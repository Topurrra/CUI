#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* The log view over a big log: the build time (cui_begin_frame to cui_end_frame) of the frames
 * around its first appearance, a filter typed into it and a level turned off.
 *
 *     cui_bench_log [--lines N]     (default 1,100,000 lines, about 100 MB)
 */
#include <cui/cui.h>
#include <cui/cui_render.h>
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

static cui_log_buffer g_log;
static void build(cui_ctx *ui, void *u) { (void)u; cui_log_view(ui, "log", &g_log, .height = -1); }

static double g_worst;
static int g_frames;
static void frame(cui_ctx *ui, double *max_ms) {
    double t0 = now_ms();
    cui_begin_frame(ui, 1.0f / 60.0f);
    build(ui, NULL);
    cui_end_frame(ui);
    double d = now_ms() - t0;
    if (max_ms && d > *max_ms) *max_ms = d;
    if (d > g_worst) g_worst = d;
    g_frames++;
}

static void phase(cui_ctx *ui, const char *name, int max_frames) {
    double first = 0, worst = 0, total = 0;
    int n = 0;
    for (; n < max_frames; n++) {
        double t0 = now_ms();
        cui_begin_frame(ui, 1.0f / 60.0f);
        build(ui, NULL);
        cui_end_frame(ui);
        double d = now_ms() - t0;
        if (n == 0) first = d;
        if (d > worst) worst = d;
        total += d;
        if (n > 0 && !cui_needs_redraw(ui)) { n++; break; }
    }
    printf("%-26s first %7.2f ms  worst %7.2f ms  frames until idle %3d  total %8.2f ms\n", name, first, worst, n, total);
}

int main(int argc, char **argv) {
    int lines = 1100000;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--lines")) lines = atoi(argv[i + 1]);
    size_t tcap = (size_t)lines * 96 + 1024;
    char *text = malloc(tcap);
    cui_log_line *recs = malloc(sizeof(cui_log_line) * (size_t)(lines + 16));
    cui_log_init(&g_log, recs, lines + 16, text, tcap);
    for (int i = 0; i < lines; i++)
        cui_log_append(&g_log, (cui_log_level)(i % 5 == 4 ? 0 : 1 + i % 4), "%08d worker %d finished the job with status %s in %d ms", i, i % 16, i % 7 ? "ok" : "slow", i % 90);
    printf("%d lines, %.1f MB of text\n", g_log.count, g_log.text_end / 1048576.0);
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 1180, 780, 1);
    cui_set_reduced_motion(ui, true);
    phase(ui, "first appearance", 400);
    /* a filter: click the search field and type */
    cui_input_mouse_move(ui, 700, 22); frame(ui, NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, NULL);
    cui_input_text(ui, "slow");
    phase(ui, "filter \"slow\"", 400);
    cui_input_key(ui, CUI_KEY_A, true, CUI_MOD_PRIMARY); cui_input_key(ui, CUI_KEY_A, false, CUI_MOD_PRIMARY);
    frame(ui, NULL);
    cui_input_key(ui, CUI_KEY_BACKSPACE, true, 0); cui_input_key(ui, CUI_KEY_BACKSPACE, false, 0);
    phase(ui, "filter cleared", 400);
    /* a level: the first toggle (Trace) */
    cui_input_mouse_move(ui, 30, 22); frame(ui, NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    phase(ui, "Trace turned off", 400);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    return 0;
}
