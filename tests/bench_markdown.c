#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* The markdown view over big generated notes, the way a reader app shows one: a scroll area with
 * the note in a column. One note per run, so the peak memory is the note's own.
 *
 *     cui_bench_markdown [--mb 0.5] [--variant lists|quotes|tables|mixed] [--scroll 300] [--width 1180]
 *                        [--no-pace] [--revision]
 *
 * Prints, for that note:
 *   first     time from the first frame to the end of the first frame that shows the note
 *   parse     frames (paced at 60 a second unless --no-pace) until the view stops asking for frames,
 *             with the mean and worst CPU of a frame meanwhile (cui_begin_frame to cui_end_frame)
 *   memory    the process's peak private memory over the run, less what it used before the note was
 *             made, and that as a multiple of the note's size (the note itself counts once)
 *   scroll    a wheel notch a frame for --scroll frames: median, 95th percentile and worst frame
 *   jumps     the frame after jumping to the middle and to the end, and the frames until it settles
 * --revision passes .revision (the app promises the text did not change while it stays the same). */
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
static double mem_now_mb(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return (double)m.PrivateUsage / 1048576.0;
}
static double mem_peak_mb(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return (double)m.PeakPagefileUsage / 1048576.0;
}
static void sleep_ms(double ms) { if (ms >= 1) Sleep((DWORD)ms); }
#else
#include <sys/resource.h>
#include <unistd.h>
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec * 1e-6; }
static double mem_now_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return (double)rss * 4096.0 / 1048576.0;
}
static double mem_peak_mb(void) { struct rusage r; getrusage(RUSAGE_SELF, &r); return (double)r.ru_maxrss / 1024.0; }
static void sleep_ms(double ms) { if (ms >= 1) usleep((useconds_t)(ms * 1000)); }
#endif

/* ---- notes ---- */

typedef struct { char *s; size_t n, cap; bool count; } buf;   /* count: only measure */
static void put(buf *b, const char *s) {
    size_t l = strlen(s);
    if (b->count) { b->n += l; return; }
    if (b->n + l + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 1 << 16;
        while (nc < b->n + l + 1) nc *= 2;
        char *ns = realloc(b->s, nc);
        if (!ns) { fprintf(stderr, "out of memory\n"); exit(1); }
        b->s = ns; b->cap = nc;
    }
    memcpy(b->s + b->n, s, l + 1);
    b->n += l;
}
static unsigned rng = 0x9E3779B9u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static const char *const words[] = { "the", "view", "parses", "each", "block", "once", "and", "lays", "out", "only", "what", "is",
                                     "near", "the", "window", "while", "a", "worker", "reads", "rest", "of", "note", "heights",
                                     "come", "back", "from", "layout", "scroll", "stays", "put", "when", "they", "change" };
static void sentence(buf *b, int nwords) {
    char w[96];
    for (int k = 0; k < nwords; k++) {
        const char *x = words[rnd() % (sizeof words / sizeof words[0])];
        unsigned r = rnd() % 40;
        if (r == 0) snprintf(w, sizeof w, "**%s**", x);
        else if (r == 1) snprintf(w, sizeof w, "*%s*", x);
        else if (r == 2) snprintf(w, sizeof w, "`%s`", x);
        else if (r == 3) snprintf(w, sizeof w, "[%s](https://example.org/%s)", x, x);
        else snprintf(w, sizeof w, "%s", x);
        if (k) put(b, " ");
        put(b, w);
    }
}
static void section(buf *b, int *sec, int level) {
    char h[64];
    snprintf(h, sizeof h, "%.*s Section %d\n\n", level, "######", ++*sec);
    put(b, h);
}
static void list(buf *b) {
    int items = 4 + (int)(rnd() % 6);
    for (int i = 0; i < items; i++) {
        put(b, rnd() % 5 == 0 ? "- [ ] " : "- ");
        sentence(b, 6 + (int)(rnd() % 14));
        put(b, "\n");
        if (rnd() % 3 == 0) {
            for (int j = 0; j < 2; j++) { put(b, "  - "); sentence(b, 4 + (int)(rnd() % 8)); put(b, "\n"); }
            if (rnd() % 2) { put(b, "    1. "); sentence(b, 5); put(b, "\n"); }
        }
    }
    put(b, "\n");
}
static void quote(buf *b) {
    int lines = 2 + (int)(rnd() % 4);
    if (rnd() % 6 == 0) put(b, "> [!note] A callout\n");
    for (int i = 0; i < lines; i++) { put(b, i == 2 && rnd() % 2 ? "> > " : "> "); sentence(b, 8 + (int)(rnd() % 12)); put(b, "\n"); }
    put(b, "\n");
}
static void table(buf *b) {
    put(b, "| Name | Value | Notes |\n|:-----|------:|-------|\n");
    int rows = 6 + (int)(rnd() % 14);
    char r[64];
    for (int i = 0; i < rows; i++) {
        snprintf(r, sizeof r, "| row %d | %u | ", i, rnd() % 10000);
        put(b, r);
        sentence(b, 3 + (int)(rnd() % 6));
        put(b, " |\n");
    }
    put(b, "\n");
}
static void para(buf *b) { sentence(b, 30 + (int)(rnd() % 50)); put(b, "\n\n"); }
static void code(buf *b) {
    put(b, "```c\n");
    int lines = 3 + (int)(rnd() % 10);
    char l[96];
    for (int i = 0; i < lines; i++) { snprintf(l, sizeof l, "    int v%d = f(%u) + g(%d); /* note */\n", i, rnd() % 1000, i); put(b, l); }
    put(b, "```\n\n");
}

enum { V_LISTS, V_QUOTES, V_TABLES, V_MIXED };
static const char *const vnames[] = { "lists", "quotes", "tables", "mixed" };

static void note_into(buf *b, int variant, size_t bytes);
/* Made twice from the same seed, measured then written, so the note takes exactly its size (the peak
 * memory then holds it once). */
static char *make_note(int variant, size_t bytes, size_t *len) {
    buf m = { NULL, 0, 0, true };
    unsigned seed = rng;
    note_into(&m, variant, bytes);
    rng = seed;
    buf b = { malloc(m.n + 1), 0, m.n + 1, false };
    if (!b.s) { fprintf(stderr, "out of memory\n"); exit(1); }
    note_into(&b, variant, bytes);
    *len = b.n;
    return b.s;
}
static void note_into(buf *bp, int variant, size_t bytes) {
    buf b = *bp;
    int sec = 0;
    put(&b, "# A big note\n\n");
    while (b.n < bytes) {
        if (variant != V_MIXED || rnd() % 4 == 0) section(&b, &sec, 2 + (int)(rnd() % 2));
        switch (variant) {
        case V_LISTS: list(&b); break;
        case V_QUOTES: quote(&b); break;
        case V_TABLES: para(&b); table(&b); break;
        default:
            switch (rnd() % 6) { case 0: list(&b); break; case 1: quote(&b); break; case 2: table(&b); break; case 3: code(&b); break; default: para(&b); break; }
        }
    }
    *bp = b;
}

/* ---- the reader ---- */

typedef struct { const char *text; int len; bool revision; float h; } reader;
static void build(cui_ctx *ui, reader *r) {
    cui_scroll(ui, .id = "read", .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .width = CUI_FILL, .align = CUI_ALIGN_CENTER, .pad_top = 44, .pad_bottom = 120, .pad_x = 32) {
            cui_column(ui, .width = CUI_FILL, .max_width = 760) {
#if defined(BENCH_BASELINE)   /* the library before .revision */
                cui_markdown_result res = cui_markdown(ui, "md", r->text, .length = r->len);
#else
                cui_markdown_result res = cui_markdown(ui, "md", r->text, .length = r->len, .revision = r->revision ? 1 : 0);
#endif
                r->h = res.height;
            }
        }
    }
}
static double frame(cui_ctx *ui, reader *r) {
    double t0 = now_ms();
    cui_begin_frame(ui, 1.0f / 60.0f);
    build(ui, r);
    cui_end_frame(ui);
    return now_ms() - t0;
}

static cui_sw_renderer *g_sw;
static const char *g_png;
/* --png: the frame just built, drawn whole, to <prefix>-<name>.png */
static void shot(cui_ctx *ui, const char *name) {
    if (!g_png) return;
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * (size_t)dd->height_px * 4);
    if (!px) return;
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    char path[512];
    snprintf(path, sizeof path, "%s-%s.png", g_png, name);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double pct(double *v, int n, double p) { if (n <= 0) return 0; qsort(v, (size_t)n, sizeof *v, cmp_d); int i = (int)(p * (n - 1) + 0.5); return v[i]; }

/* frames until the view stops asking for more (at most max), paced at 60 a second */
static int settle(cui_ctx *ui, reader *r, int max, bool pace, double *mean, double *worst, double *wall) {
    double t0 = now_ms(), sum = 0;
    int n = 0;
    *worst = 0;
    for (; n < max; n++) {
        double s = now_ms();
        double d = frame(ui, r);
        sum += d;
        if (d > *worst) *worst = d;
        float wake = cui_next_wakeup(ui);   /* a worker still busy asks for a frame shortly */
        if (!cui_needs_redraw(ui) && !(wake >= 0 && wake < 0.02f)) { n++; break; }
        if (pace) sleep_ms(16.6 - (now_ms() - s));
    }
    *mean = n ? sum / n : 0;
    *wall = now_ms() - t0;
    return n;
}

int main(int argc, char **argv) {
    double mb = 0.5;
    int variant = V_MIXED, scroll = 300, width = 1180;
    bool pace = true, revision = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = atof(argv[++i]);
        else if (!strcmp(argv[i], "--variant") && i + 1 < argc) {
            const char *v = argv[++i];
            for (int k = 0; k < 4; k++) if (!strcmp(v, vnames[k])) variant = k;
        } else if (!strcmp(argv[i], "--scroll") && i + 1 < argc) scroll = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-pace")) pace = false;
        else if (!strcmp(argv[i], "--revision")) revision = true;
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) g_png = argv[++i];
    }
    cui_sw_renderer *sw = cui_sw_create();
    g_sw = sw;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, (float)width, 780, 1);
    cui_set_reduced_motion(ui, true);
    cui_input_mouse_move(ui, (float)width * 0.5f, 400);   /* the wheel goes to the scroll area under the pointer */
    double base = mem_now_mb();
    size_t len;
    char *note = make_note(variant, (size_t)(mb * 1048576.0), &len);
    double text_mb = (double)len / 1048576.0;
    reader r = { note, (int)len, revision, 0 };

    /* first frame that shows the note */
    double t0 = now_ms(), first = -1, f1 = 0;
    int nfirst = 0;
    for (; nfirst < 2000; nfirst++) {
        double d = frame(ui, &r);
        if (nfirst == 0) f1 = d;
        if (r.h > 1 || cui_get_rect(ui, cui_make_id(ui, "md")).h > 1) { first = now_ms() - t0; nfirst++; break; }
    }
    double pmean, pworst, pwall;
    int pframes = settle(ui, &r, 5000, pace, &pmean, &pworst, &pwall);
    shot(ui, "start");
    printf("%-6s %5.1f MB  first %8.1f ms (%d frames, frame 1 %.1f ms)  parse %4d frames %8.1f ms, frame mean %6.2f worst %7.2f ms\n",
           vnames[variant], text_mb, first, nfirst, f1, pframes, pwall, pmean, pworst);
    fflush(stdout);

    /* scrolling: a wheel notch every frame */
    double *v = malloc(sizeof(double) * (size_t)(scroll + 1));
    int nv = 0;
    for (int f = 0; f < scroll; f++) {
        cui_input_scroll(ui, 0, -1);
        v[nv++] = frame(ui, &r);
    }
    cui_scroll_info si = cui_scroll_get(ui, "read");
    double med = pct(v, nv, 0.5), p95 = pct(v, nv, 0.95), mx = nv ? v[nv - 1] : 0;
    printf("       scroll %d notches: median %6.2f  p95 %6.2f  max %7.2f ms  (at %.0f of %.0f pt)\n", nv, med, p95, mx, si.y, si.max_y);
    /* jumps */
    const char *names[] = { "middle", "end", "top" };
    float to[] = { 0.5f, 1.0f, 0.0f };
    for (int k = 0; k < 3; k++) {
        si = cui_scroll_get(ui, "read");
        cui_scroll_set(ui, "read", to[k] >= 1.0f ? CUI_SCROLL_BOTTOM : to[k] * si.max_y, true);
        double jf = frame(ui, &r), jm, jw, jwall;
        int jn = settle(ui, &r, 600, false, &jm, &jw, &jwall);
        shot(ui, names[k]);
        si = cui_scroll_get(ui, "read");
        printf("       jump to the %-6s first frame %7.2f ms, then %3d frames to settle, worst %7.2f ms  (at %.0f of %.0f pt)\n", names[k], jf, jn,
               jw, si.y, si.max_y);
    }
    double peak = mem_peak_mb();
    size_t aused = 0, acap = 0, apeak = 0;
    cui_frame_memory(ui, &aused, &acap, &apeak);
    double now = mem_now_mb();
    printf("       memory peak %8.1f MB over the start (%.2fx the note's %.1f MB), at the end %.1f MB (%.2fx), frame arena peak %.1f MB\n",
           peak - base, (peak - base) / text_mb, text_mb, now - base, (now - base) / text_mb, (double)apeak / 1048576.0);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    free(note);
    free(v);
    return 0;
}
