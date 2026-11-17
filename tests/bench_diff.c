#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Measures the diff viewer over big generated texts (C-like lines of about 100 bytes, a change every 50
 * lines, block comments now and then): preparing the diff (what a worker does), the frame the diff arrives
 * in, idle frames, scrolling, next change, a change of mode, and for comparison the old path where the view is
 * given the texts. Headless, Release build, like cui_bench_editor.
 *
 *     cui_bench_diff [MB ...]        default: 10 100
 *     cui_bench_diff --texts [MB]    also the view diffing the texts itself, on the UI thread (default 10 MB)
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

/* C-like lines of about 100 bytes; every 40th line opens a three line block comment */
static size_t gen(char *buf, size_t size, unsigned seed) {
    static const char *const words[] = { "int", "value", "(id=42)", "{", "}", "\"path\"", "return", "x[3]", "retry", "elapsed", "worker",
                                         "static", "const", "char *", "= 0;", "if", "for", "0x1F", "// note", "request" };
    size_t n = 0;
    int line = 0;
    while (n + 400 < size) {
        if (line % 40 == 17) { n += (size_t)sprintf(buf + n, "/* block %08d\n * still in the comment return 1;\n */\n", line); line += 3; continue; }
        size_t start = n;
        n += (size_t)sprintf(buf + n, "    n%08d ", line);
        while (n - start < 94) {
            seed = seed * 1103515245u + 12345u;
            const char *w = words[(seed >> 16) % 20];
            size_t wl = strlen(w);
            memcpy(buf + n, w, wl);
            n += wl;
            buf[n++] = ' ';
        }
        buf[n++] = '\n';
        line++;
    }
    buf[n] = 0;
    return n;
}

/* every 50th line edited, every 400th removed, a line added every 300 */
static size_t mutate(const char *a, char *out) {
    size_t n = 0;
    int line = 0;
    for (const char *p = a; *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) + 1 : strlen(p);
        if (line % 400 == 399) { /* removed */ }
        else if (line % 50 == 7) { n += (size_t)sprintf(out + n, "%.*s changed %d\n", (int)(len - 1), p, line); }
        else { memcpy(out + n, p, len); n += len; }
        if (line % 300 == 150) n += (size_t)sprintf(out + n, "an added line %d\n", line);
        p += len;
        line++;
    }
    out[n] = 0;
    return n;
}

typedef struct {
    const char *id;
    const char *a, *b;
    const cui_diff_prepared *prep;
    int revision;
    int current;
    cui_diff_mode mode;
    cui_diff_result r;
} view;

static void build(cui_ctx *ui, void *u) {
    view *v = u;
    v->r = cui_diff_view(ui, v->id ? v->id : "diff", v->a, v->b, .prepared = v->prep, .revision = v->revision, .mode = v->mode, .current = &v->current,
                         .height = -1, .language = "c", .old_title = "a.c", .new_title = "b.c");
}

static double frame(cui_ctx *ui, view *v, double *worst) {
    double t0 = now();
    cui_begin_frame(ui, 1.0f / 60.0f);
    build(ui, v);
    cui_end_frame(ui);
    double ms = (now() - t0) * 1000;
    if (ms > *worst) *worst = ms;
    return ms;
}

/* the frames after the diff is in: idle, scrolling, next change, the other mode */
static void steps(cui_ctx *ui, view *v, double first, const char *label) {
    double w = 0, second = frame(ui, v, &w);
    for (int i = 0; i < 3; i++) frame(ui, v, &w);
    double idle = 0, idle_w = 0;
    for (int i = 0; i < 30; i++) idle += frame(ui, v, &idle_w);
    cui_input_mouse_move(ui, 700, 400);
    double scroll = 0, scroll_w = 0;
    for (int i = 0; i < 40; i++) { cui_input_scroll(ui, 0, -3); scroll += frame(ui, v, &scroll_w); }
    double nav = 0, nav_w = 0;
    for (int i = 0; i < 30; i++) { v->current = (v->current + 1) % (v->r.changes ? v->r.changes : 1); nav += frame(ui, v, &nav_w); }
    v->current = v->r.changes / 2;
    double jump_w = 0, jump = frame(ui, v, &jump_w);
    frame(ui, v, &w);
    v->mode = CUI_DIFF_INLINE;
    double mode_w = 0, mode = frame(ui, v, &mode_w);
    double inl = 0, inl_w = 0;
    for (int i = 0; i < 20; i++) { cui_input_scroll(ui, 0, -3); inl += frame(ui, v, &inl_w); }
    v->mode = CUI_DIFF_SPLIT;
    frame(ui, v, &w);
    printf("  %-22s first frame %7.2f ms (second %.2f) | idle %.2f (worst %.2f) | scroll %.2f (worst %.2f) | next change %.2f (worst %.2f) | "
           "jump to the middle %.2f | to inline %.2f, inline scroll %.2f (worst %.2f)\n",
           label, first, second, idle / 30, idle_w, scroll / 40, scroll_w, nav / 30, nav_w, jump, mode, inl / 20, inl_w);
}

static void one(size_t mb, bool texts) {
    size_t size = mb * 1024 * 1024;
    char *a = malloc(size + 1), *b = malloc(size * 2 + 1);
    size_t al = gen(a, size, 7), bl = mutate(a, b);
    double base = mem_mb();
    printf("%zu MB (%zu / %zu bytes)\n", mb, al, bl);
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1400, 800, 1);
    cui_set_reduced_motion(ui, true);

    /* what the worker does */
    double t0 = now();
    cui_diff_prepared *plain = cui_diff_prepare(a, al, b, bl);
    double t_plain = (now() - t0) * 1000;
    cui_diff_prepared_free(plain);
    t0 = now();
    cui_diff_prepared *p = cui_diff_prepare(a, al, b, bl, .language = "c");
    double t_prep = (now() - t0) * 1000;
    cui_diff_stats st;
    cui_diff_prepared_stats(p, &st);
    printf("  prepare (worker): %.1f ms without a language, %.1f ms with .language = \"c\" | %d changes, +%d -%d%s | memory +%.0f MB\n",
           t_plain, t_prep, st.hunks, st.added, st.removed, st.approximate ? ", approximate" : "", mem_mb() - base);

    view v;
    memset(&v, 0, sizeof v);
    v.prep = p;
    v.current = -1;
    double w = 0, first = frame(ui, &v, &w);   /* the very first frame: the glyphs of the text are not in the atlas yet */
    cui_destroy(ui);
    cui_sw_destroy(sw);
    sw = cui_sw_create();
    ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1400, 800, 1);
    cui_set_reduced_motion(ui, true);
    {   /* an app has drawn code before: a small diff of other text first warms the atlas */
        view tiny;
        memset(&tiny, 0, sizeof tiny);
        tiny.id = "tiny";
        tiny.a = "int main(void) {\n    return 0;\n}\n";
        tiny.b = "int main(void) {\n    return 1; /* x */\n}\n";
        tiny.current = -1;
        for (int i = 0; i < 3; i++) frame(ui, &tiny, &w);
    }
    double cold = first;
    v.current = -1;
    w = 0;
    first = frame(ui, &v, &w);
    printf("  arrival frame, glyphs cold: %.2f ms\n", cold);
    steps(ui, &v, first, "prepared diff");
    printf("  memory with the view: +%.0f MB\n", mem_mb() - base);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_diff_prepared_free(p);

    if (texts) {   /* the view given the texts: it diffs on the UI thread in the first frame */
        for (int rev = 0; rev < 2; rev++) {
            sw = cui_sw_create();
            ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
            cui_set_viewport(ui, 1400, 800, 1);
            cui_set_reduced_motion(ui, true);
            memset(&v, 0, sizeof v);
            v.a = a; v.b = b; v.current = -1;
            v.revision = rev ? 1 : 0;
            w = 0;
            first = frame(ui, &v, &w);
            steps(ui, &v, first, rev ? "texts with .revision" : "texts, no .revision");
            cui_destroy(ui);
            cui_sw_destroy(sw);
        }
    }
    free(a);
    free(b);
}

int main(int argc, char **argv) {
    size_t sizes[8] = { 10, 100 };
    int ns = 2;
    bool texts = false;
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "--texts")) { texts = true; first = 2; sizes[0] = 10; ns = 1; }
    if (argc > first) { ns = 0; for (int i = first; i < argc && ns < 8; i++) sizes[ns++] = (size_t)atoi(argv[i]); }
    for (int s = 0; s < ns; s++) one(sizes[s], texts);
    return 0;
}
