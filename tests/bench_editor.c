#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Measures the code editor over big generated texts: the first frame, idle
 * frames, scrolling in the middle and at the end, typing near the top and
 * near the end, and memory. Headless, like the tests.
 *
 *     cui_bench_editor [MB ...]          default: 1 10 100
 *     cui_bench_editor --quick           1 and 10 MB only
 */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static double mem_mb(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return (double)m.PrivateUsage / (1024.0 * 1024.0);
}
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static double mem_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return (double)rss * 4096.0 / (1024.0 * 1024.0);
}
#endif

typedef struct {
    char *buf; size_t cap;
    const char *lang;
    bool wrap, plain, big;
    cui_code_editor_result r;
} bench_ed;

static void build(cui_ctx *ui, void *u) {
    bench_ed *b = u;
#if defined(BENCH_BASELINE)   /* the library before .plain and .revision */
    b->r = cui_code_editor(ui, "ed", b->buf, b->cap, .lang = b->plain ? "text" : b->lang, .wrap = b->wrap, .height = -1);
#else
    b->r = cui_code_editor(ui, "ed", b->buf, b->cap, .lang = b->lang, .wrap = b->wrap, .height = -1,
                           .plain = b->plain, .revision = b->big ? 1 : 0);
#endif
}

static double frames(cui_ctx *ui, bench_ed *b, int n, int scroll) {
    double worst = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (scroll) cui_input_scroll(ui, 0, (float)-scroll);
        double t0 = now();
        cui_begin_frame(ui, 1.0f / 60.0f);
        build(ui, b);
        cui_end_frame(ui);
        double t = (now() - t0) * 1000;
        total += t;
        if (t > worst) worst = t;
    }
    (void)worst;
    return total / n;
}

/* Text: short lines like a log, or long lines (about 3000 bytes) like minified code. Lines
 * hold words, brackets, quotes and comment markers, and every 40th line in markdown is a fence. */
static size_t gen(char *buf, size_t size, bool long_lines) {
    static const char *const words[] = { "INFO", "worker", "request", "(id=42)", "{ok}", "\"path\"", "-> done;", "//", "x[3]",
                                         "# heading", "`code`", "*em*", "value=0x1F", "elapsed", "ms", "retry" };
    size_t n = 0;
    unsigned seed = 12345;
    int line = 0;
    while (n + 4000 < size) {
        int target = long_lines ? 2500 + (int)(seed % 1000) : 40 + (int)(seed % 80);
        int start = (int)n;
        if (line % 40 == 39) { memcpy(buf + n, "```", 3); n += 3; }
        else {
            n += (size_t)sprintf(buf + n, "%08d ", line);
            while ((int)n - start < target) {
                seed = seed * 1103515245u + 12345u;
                const char *w = words[(seed >> 16) % 16];
                size_t wl = strlen(w);
                memcpy(buf + n, w, wl); n += wl;
                buf[n++] = ' ';
            }
        }
        buf[n++] = '\n';
        line++;
        seed = seed * 1103515245u + 12345u;
    }
    buf[n] = 0;
    return n;
}

static void one(size_t mb, bool long_lines, const char *lang, bool wrap, bool plain, bool big) {
    size_t size = mb * 1024 * 1024;
    bench_ed b;
    memset(&b, 0, sizeof b);
    b.cap = size + 1024 * 1024;
    b.buf = malloc(b.cap);
    size_t len = gen(b.buf, size, long_lines);
    b.lang = lang; b.wrap = wrap; b.plain = plain; b.big = big;
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1200, 800, 1);
    cui_set_reduced_motion(ui, true);
    double m0 = mem_mb();
    double open = frames(ui, &b, 1, 0);
    frames(ui, &b, 2, 0);
    double idle = frames(ui, &b, 5, 0);
    double mem = mem_mb() - m0;
    cui_input_mouse_move(ui, 600, 400);
    int nl = b.r.line_count;
    cui_code_editor_goto(ui, "ed", nl / 2, 1);
    frames(ui, &b, 2, 0);
    double mid = frames(ui, &b, 5, 3);
    cui_code_editor_goto(ui, "ed", nl, 1);
    frames(ui, &b, 2, 0);
    double end = frames(ui, &b, 5, -3);
    /* typing: a character each frame, near the top and near the end */
    cui_code_editor_goto(ui, "ed", 3, 5);
    frames(ui, &b, 2, 0);
    double ttop = 0, tend = 0;
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, "a"); ttop += frames(ui, &b, 1, 0); }
    cui_code_editor_goto(ui, "ed", nl - 3, 5);
    frames(ui, &b, 2, 0);
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, "b"); tend += frames(ui, &b, 1, 0); }
    double mem2 = mem_mb() - m0;
    /* Enter near the top: a line is added, every line below moves */
    cui_code_editor_goto(ui, "ed", 3, 5);
    frames(ui, &b, 2, 0);
    double enter = 0;
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_ENTER, true, 0); cui_input_key(ui, CUI_KEY_ENTER, false, 0); enter += frames(ui, &b, 1, 0); }
    double mem3 = mem_mb() - m0;
    printf("%4zu MB %-5s %-8s %-4s %-5s %-3s %8d lines | open %8.1f  idle %7.2f  mid %7.2f  end %7.2f  type top %7.2f  end %7.2f  enter %7.2f ms | mem %6.0f %6.0f %6.0f MB\n",
           mb, long_lines ? "long" : "short", plain ? "plain" : lang, wrap ? "wrap" : "", big ? "rev" : "", "", nl,
           open, idle, mid, end, ttop / 5, tend / 5, enter / 5, mem, mem2, mem3);
    fflush(stdout);
    (void)len;
    cui_destroy(ui);
    cui_sw_destroy(sw);
    free(b.buf);
}

int main(int argc, char **argv) {
    size_t sizes[8] = { 1, 10, 100 };
    int ns = 3;
    bool bigs[2] = { true, false };   /* with .revision first: the way an app opens a big file */
    int nb = 2;
#if defined(BENCH_BASELINE)
    nb = 1;
#endif
    if (argc > 1 && !strcmp(argv[1], "--quick")) ns = 2;
    else if (argc > 1) { ns = 0; for (int i = 1; i < argc && ns < 8; i++) sizes[ns++] = (size_t)atoi(argv[i]); }
    static const char *const langs[] = { "text", "c", "markdown" };
    for (int s = 0; s < ns; s++)
        for (int ll = 0; ll < 2; ll++)
            for (int w = 0; w < 2; w++)
                for (int k = 0; k < nb; k++) {
                    for (int l = 0; l < 3; l++) one(sizes[s], ll, langs[l], w, false, bigs[k]);
                    one(sizes[s], ll, "text", w, true, bigs[k]);
                }
    return 0;
}
