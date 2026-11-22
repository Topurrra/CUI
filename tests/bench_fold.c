#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Measures the code editor's folding and minimap (cui_code_editor_opts.folding, .minimap) over a generated C-like text
 * on a document (as jneditor edits files): frames while idle, scrolling, typing near the top and in the middle, Enter,
 * with both off, both on, and both on with every block folded; fold all and open all themselves. Headless.
 *
 *     cui_bench_fold [MB ...]      default: 1 10
 *     cui_bench_fold --wrap [MB]   the same, wrapped
 */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
#endif

typedef struct { cui_text_doc *doc; bool fold, mm, wrap; cui_code_editor_result r; } bench;

static void build(cui_ctx *ui, void *u) {
    bench *b = u;
    b->r = cui_code_editor(ui, "ed", NULL, 0, .doc = b->doc, .lang = "c", .height = -1, .wrap = b->wrap, .folding = b->fold, .minimap = b->mm);
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

typedef struct { double p50, p95, max; } stats;
/* n frames; before each: a wheel step (scroll), a typed character (type), or Enter (enter) */
static stats frames(cui_ctx *ui, bench *b, int n, int what) {
    double t[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        if (what == 1) cui_input_scroll(ui, 0, -3);
        if (what == 2) cui_input_text(ui, "a");
        if (what == 3) { cui_input_key(ui, CUI_KEY_ENTER, true, 0); cui_input_key(ui, CUI_KEY_ENTER, false, 0); }
        double t0 = now();
        cui_begin_frame(ui, 1.0f / 60.0f);
        build(ui, b);
        cui_end_frame(ui);
        t[i] = (now() - t0) * 1000;
    }
    qsort(t, (size_t)n, sizeof t[0], cmpd);
    stats s = { t[n / 2], t[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], t[n - 1] };
    return s;
}

/* C-like functions: a header, nested blocks, a closing brace */
static char *gen(size_t size, int *len) {
    char *buf = malloc(size + 4096);
    size_t n = 0;
    for (int f = 0; n + 2048 < size; f++) {
        n += (size_t)sprintf(buf + n, "/* function %d: does a thing with its argument */\nstatic int fn_%d(int x, const char *name) {\n", f, f);
        for (int k = 0; k < 4; k++) {
            n += (size_t)sprintf(buf + n, "    if (x > %d) {\n        x -= %d; // step %d\n        printf(\"%%s %%d\\n\", name, x);\n    }\n", k * 3, k + 1, k);
        }
        n += (size_t)sprintf(buf + n, "    return x * %d;\n}\n\n", f % 97);
    }
    buf[n] = 0;
    *len = (int)n;
    return buf;
}

static void line(const char *what, stats s) { printf("  %-34s p50 %6.3f  p95 %6.3f  max %6.3f ms\n", what, s.p50, s.p95, s.max); }

static void one(size_t mb, bool wrap) {
    int len;
    char *text = gen(mb * 1024 * 1024, &len);
    static const struct { bool fold, mm, all; const char *name; } modes[] = {
        { false, false, false, "off" }, { true, true, false, "folding + minimap" }, { true, true, true, "folding + minimap, all folded" } };
    printf("%zu MB%s, %d bytes\n", mb, wrap ? ", wrapped" : "", len);
    for (int m = 0; m < 3; m++) {
        bench b;
        memset(&b, 0, sizeof b);
        b.doc = cui_text_doc_new(text, len);
        b.fold = modes[m].fold; b.mm = modes[m].mm; b.wrap = wrap;
        cui_sw_renderer *sw = cui_sw_create();
        cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
        cui_set_viewport(ui, 1200, 800, 1);
        cui_set_reduced_motion(ui, true);
        double t0 = now();
        frames(ui, &b, 1, 0);
        double first = (now() - t0) * 1000;
        frames(ui, &b, 3, 0);
        printf(" %s: first frame %.2f ms\n", modes[m].name, first);
        if (modes[m].all) {
            cui_code_editor_fold(ui, "ed", CUI_FOLD_ALL, 0);
            stats s = frames(ui, &b, 1, 0);
            printf("  fold all: %.2f ms, %d folds, %d lines hidden\n", s.max, b.r.fold_count, b.r.hidden_lines);
        }
        cui_input_mouse_move(ui, 600, 400);
        line("idle", frames(ui, &b, 30, 0));
        line("scroll", frames(ui, &b, 120, 1));
        int nl = b.r.line_count;
        cui_code_editor_goto(ui, "ed", 3, 5);
        frames(ui, &b, 3, 0);
        line("type near the top", frames(ui, &b, 60, 2));
        cui_code_editor_goto(ui, "ed", nl / 2, 5);
        frames(ui, &b, 3, 0);
        line("type in the middle", frames(ui, &b, 60, 2));
        line("Enter in the middle", frames(ui, &b, 30, 3));
        cui_code_editor_goto(ui, "ed", nl / 3, 1);
        frames(ui, &b, 3, 0);
        line("scroll from a third down", frames(ui, &b, 120, 1));
        if (modes[m].all) {
            cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN_ALL, 0);
            stats s = frames(ui, &b, 1, 0);
            printf("  open all: %.2f ms\n", s.max);
        }
        cui_destroy(ui);
        cui_sw_destroy(sw);
        cui_text_doc_free(b.doc);
    }
    free(text);
}

int main(int argc, char **argv) {
    bool wrap = argc > 1 && !strcmp(argv[1], "--wrap");
    if (wrap) { argc--; argv++; }
    size_t sizes[8] = { 1, 10 };
    int ns = 2;
    if (argc > 1) { ns = 0; for (int i = 1; i < argc && ns < 8; i++) sizes[ns++] = (size_t)atoi(argv[i]); }
    for (int i = 0; i < ns; i++) one(sizes[i], wrap);
    return 0;
}
