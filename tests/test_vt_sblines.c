/* Tests for CUI_VT_FEATURE_SB_LINES: the scrollback keeps its number of lines whatever their width. Off (the default) the
 * ring holds lines * 48 cells as before; on, it starts small, grows for wide lines, keeps exactly `lines` lines of mixed
 * widths through many wraps of the ring, works with REFLOW, shrinks back when the scrollback is erased, and costs about
 * cells x 8 bytes. */
#include <cui/cui.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart * 1000.0 / (double)f.QuadPart; }
#else
static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000.0 + t.tv_nsec / 1e6; }
#endif

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

#define SBL CUI_VT_FEATURE_SB_LINES

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static cui_vt *mk(int cols, int rows, int sb, unsigned f) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, f); return v; }

/* line i: its number, then letters, len(i) cells in all (at least 8) */
static int len_of(int i, int maxw) { return 8 + (int)((unsigned)i * 37u % (unsigned)(maxw - 8)); }
static void put_line(cui_vt *v, int i, int len) {
    char b[1100];
    int n = snprintf(b, sizeof b, "%06d:", i);
    while (n < len) { b[n] = (char)('a' + (n + i) % 26); n++; }
    b[n] = 0;
    W(v, b);
    W(v, "\r\n");
}
static bool line_is(const cui_vt *v, int row, int i, int len) {
    char b[1100], want[16];
    size_t n = cui_vt_line_text(v, row, b, sizeof b);
    snprintf(want, sizeof want, "%06d:", i);
    return (int)n == len && !strncmp(b, want, 7);
}

static void fill(cui_vt *v, int lines, int width) {
    for (int i = 0; i < lines; i++) put_line(v, i, width);
}

/* off: as before, wide lines push the oldest out early */
static void test_off(void) {
    cui_vt *a = mk(120, 5, 100, 0), *b = cui_vt_create(120, 5, 100);
    fill(a, 300, 120);
    CHECK(cui_vt_scrollback_count(a) < 50);                   /* 4800 cells hold 40 such lines */
    CHECK_EQ(cui_vt_memory(a), cui_vt_memory(b));              /* the same ring as a screen without features */
    cui_vt_destroy(a); cui_vt_destroy(b);
}

/* on: exactly the lines asked for, whatever their width */
static void test_on_uniform(void) {
    cui_vt *v = mk(120, 5, 100, SBL);
    fill(v, 300, 120);
    CHECK_EQ(cui_vt_scrollback_count(v), 100);
    /* 300 lines and the cursor's empty row: the screen shows 296..299 and the empty row, the scrollback 196..295 */
    CHECK(line_is(v, -100, 196, 120));
    CHECK(line_is(v, -1, 295, 120));
    CHECK(cui_vt_memory(v) < (size_t)101 * 120 * 8 + 64 * 1024);
    cui_vt_destroy(v);
}

/* mixed widths through many turns of the ring: every line kept whole, oldest first */
static void test_on_mixed(void) {
    int maxw = 260, N = 500, total = 4000;
    cui_vt *v = mk(maxw, 4, N, SBL);
    for (int i = 0; i < total; i++) put_line(v, i, len_of(i, maxw));
    CHECK_EQ(cui_vt_scrollback_count(v), N);
    int bad = 0, first = total - 3 - N;                       /* the screen keeps the last 3 lines and the empty row */
    for (int k = 0; k < N; k++) {
        int i = first + k;
        if (!line_is(v, -N + k, i, len_of(i, maxw)) && bad++ < 3) printf("  line %d (row %d) differs\n", i, -N + k);
    }
    CHECK_EQ(bad, 0);
    CHECK(cui_vt_memory(v) <= (size_t)(N + 1) * maxw * 8 + 64 * 1024);
    cui_vt_destroy(v);
}

/* narrow output stays small: the ring follows what is there */
static void test_memory(void) {
    cui_vt *v = mk(80, 24, 10000, SBL);
    size_t empty = cui_vt_memory(v);
    CHECK(empty < 400 * 1024);                                 /* 256 KB of ring, the line table, the screen */
    fill(v, 10000 + 24, 10);
    CHECK_EQ(cui_vt_scrollback_count(v), 10000);
    CHECK(cui_vt_memory(v) < 1200 * 1024);                     /* 10 cells a line */
    cui_vt_destroy(v);
    v = mk(80, 24, 10000, SBL);
    fill(v, 10000 + 24, 80);
    size_t m80 = cui_vt_memory(v);
    CHECK(m80 > (size_t)10000 * 80 * 8 && m80 < (size_t)10001 * 80 * 8 + 300 * 1024);
    printf("  10,000 lines of 80: %.1f MB\n", (double)m80 / 1048576.0);
    /* a save of its last 5000 rows (jnshell's restore) still carries 5000 rows, into a screen that keeps lines too */
    cui_vt_saved sv = {0};
    CHECK(cui_vt_save(v, 5000, &sv) > 0);
    cui_vt *w = mk(80, 24, 10000, SBL);
    CHECK(cui_vt_load(w, sv.data, sv.len));
    CHECK_EQ(cui_vt_scrollback_count(w), 5000 - 24);
    CHECK(line_is(w, -(5000 - 24), 10000 + 24 + 1 - 5000, 80));    /* lines 0..10023 and the cursor's empty row: the last 5000 rows */
    cui_vt_saved_free(&sv);
    cui_vt_destroy(w);
    cui_vt_destroy(v);
    v = mk(300, 24, 10000, SBL);
    fill(v, 10000 + 24, 300);
    size_t m300 = cui_vt_memory(v);
    CHECK(m300 > (size_t)10000 * 300 * 8 && m300 < (size_t)10001 * 300 * 8 + 400 * 1024);
    CHECK_EQ(cui_vt_scrollback_count(v), 10000);
    printf("  10,000 lines of 300: %.1f MB\n", (double)m300 / 1048576.0);
    /* ESC [ 3 J: the scrollback goes, and the ring goes back to its start */
    W(v, "\x1b[3J");
    CHECK_EQ(cui_vt_scrollback_count(v), 0);
    CHECK(cui_vt_memory(v) < 600 * 1024);
    fill(v, 50, 300);
    CHECK_EQ(cui_vt_scrollback_count(v), 50);                  /* the screen's 24 rows were still there: 50 rows went up */
    cui_vt_destroy(v);
    /* turned on while empty: the legacy ring (10000 x 48 cells, 3.7 MB) gives way to the small one */
    v = cui_vt_create(80, 24, 10000);
    size_t legacy = cui_vt_memory(v);
    cui_vt_set_features(v, SBL);
    CHECK(cui_vt_memory(v) + 3 * 1024 * 1024 < legacy);
    cui_vt_destroy(v);
}

/* with REFLOW: a narrower screen wraps the lines into more rows, still at most `lines` of them; nothing breaks */
static void test_reflow(void) {
    cui_vt *v = mk(200, 5, 100, SBL | CUI_VT_FEATURE_REFLOW);
    fill(v, 150, 200);
    CHECK_EQ(cui_vt_scrollback_count(v), 100);
    cui_vt_resize(v, 50, 5);                                   /* 4 rows a line */
    CHECK_EQ(cui_vt_scrollback_count(v), 100);
    char b[256];
    cui_vt_line_text(v, -1, b, sizeof b);
    CHECK_EQ(strlen(b), 50);
    cui_vt_resize(v, 200, 5);                                  /* joined again: whole lines */
    int n = cui_vt_scrollback_count(v);
    CHECK(n >= 25 && n <= 100);
    cui_vt_line_text(v, -1, b, sizeof b);
    CHECK_EQ(strlen(b), 200);
    cui_vt_destroy(v);
}

/* the option off again keeps what is there and goes on as before */
static void test_toggle(void) {
    cui_vt *v = mk(120, 5, 100, SBL);
    fill(v, 150, 120);
    CHECK_EQ(cui_vt_scrollback_count(v), 100);
    cui_vt_set_features(v, 0);
    fill(v, 10, 120);
    CHECK_EQ(cui_vt_scrollback_count(v), 100);                 /* the grown ring still has room */
    CHECK(line_is(v, -1, 5, 120));
    cui_vt_destroy(v);
}

/* the cost: 40 MB of 88-column lines into a 120 x 30 screen with 5000 lines of scrollback, off and on, interleaved (logged;
 * the check only catches something far off) */
static void test_speed(void) {
    enum { LINES = 400000 };
    char *buf = (char *)malloc((size_t)LINES * 100);
    size_t n = 0;
    for (int i = 0; i < LINES; i++) {
        int k = snprintf(buf + n, 100, "%08d the quick brown fox jumps over the lazy dog 0123456789 abcdefghijklmnopqrstuvwxyz", i);
        n += (size_t)k;
        buf[n++] = '\r'; buf[n++] = '\n';
    }
    double best[2] = { 1e9, 1e9 };
    for (int round = 0; round < 3; round++)
        for (int on = 0; on < 2; on++) {
            cui_vt *v = mk(120, 30, 5000, on ? SBL : 0);
            double t0 = now_ms();
            for (size_t off = 0; off < n; off += 65536) cui_vt_write(v, buf + off, n - off < 65536 ? n - off : 65536);
            double ms = now_ms() - t0;
            if (ms < best[on]) best[on] = ms;
            if (round == 0 && on) CHECK_EQ(cui_vt_scrollback_count(v), 5000);
            cui_vt_destroy(v);
        }
    printf("  %.0f MB of lines: off %.1f MB/s, on %.1f MB/s\n", (double)n / 1048576.0, (double)n / 1048576.0 / (best[0] / 1000.0),
           (double)n / 1048576.0 / (best[1] / 1000.0));
    CHECK(best[1] < best[0] * 1.5);
    free(buf);
}

int main(void) {
    test_off();
    test_on_uniform();
    test_on_mixed();
    test_memory();
    test_reflow();
    test_toggle();
    test_speed();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
