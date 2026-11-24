/* Tests for CUI_VT_FEATURE_REFLOW: a change of width joins soft-wrapped rows and wraps them again, on the main screen and in
 * the scrollback; round trips, wide characters at the edge, attributes, the cursor, an empty line against a soft wrap, the
 * alternate screen, a drag the program hears of only at its end (cui_vt_resize_sent), picture rows, the view (selection,
 * looking back), the option off, and the time a reflow of a full scrollback takes. */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include "../src/cui_internal.h"

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
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b))) { g_failed++; printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

#define RF CUI_VT_FEATURE_REFLOW

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static const char *row(const cui_vt *v, int y) { static char b[8][1100]; static int i; i = (i + 1) & 7; cui_vt_line_text(v, y, b[i], sizeof b[i]); return b[i]; }
static cui_vt *mk(int cols, int rows, int sb, unsigned f) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, f); return v; }
static void curs(const cui_vt *v, int x, int y, int line) {
    int cx, cy; cui_vt_cursor(v, &cx, &cy);
    g_checks++;
    if (cx != x || cy != y) { g_failed++; printf("  FAIL %s:%d: cursor %d,%d, expected %d,%d\n", __FILE__, line, cx, cy, x, y); }
}
#define CURSOR(v, x, y) curs((v), (x), (y), __LINE__)

/* ---- wrap and unwrap ---- */

static void test_round_trip(void) {
    cui_vt *v = mk(20, 6, 100, RF);
    W(v, "AAAAAAAAAABBBBBBBBBBCCCCC\r\nshort\r\n$ ");
    CHECK_STR(row(v, 0), "AAAAAAAAAABBBBBBBBBB"); CHECK_STR(row(v, 1), "CCCCC"); CHECK_STR(row(v, 3), "$ "); CURSOR(v, 2, 3);
    cui_vt_resize(v, 10, 6);                                   /* narrower: the long line takes three rows */
    CHECK_STR(row(v, 0), "AAAAAAAAAA"); CHECK_STR(row(v, 1), "BBBBBBBBBB"); CHECK_STR(row(v, 2), "CCCCC");
    CHECK_STR(row(v, 3), "short"); CHECK_STR(row(v, 4), "$ "); CURSOR(v, 2, 4);
    CHECK_EQ(cui_vt_scrollback_count(v), 0);
    cui_vt_resize(v, 20, 6);                                   /* and back: exactly as it was */
    CHECK_STR(row(v, 0), "AAAAAAAAAABBBBBBBBBB"); CHECK_STR(row(v, 1), "CCCCC"); CHECK_STR(row(v, 2), "short"); CURSOR(v, 2, 3);
    W(v, "x");                                                 /* typing goes on where the prompt is */
    CHECK_STR(row(v, 3), "$ x");
    cui_vt_resize(v, 30, 6);                                   /* wider: one row, the top stays */
    CHECK_STR(row(v, 0), "AAAAAAAAAABBBBBBBBBBCCCCC"); CHECK_STR(row(v, 1), "short"); CURSOR(v, 3, 2);
    CHECK_EQ(cui_vt_reflows(v), 3);
    /* narrower than the screen holds: the top rows leave into the scrollback, the cursor stays */
    cui_vt_resize(v, 5, 6);                                    /* 5 rows of A..C, short, "$ x": 7 rows */
    CHECK_EQ(cui_vt_scrollback_count(v), 1);
    CHECK_STR(row(v, -1), "AAAAA"); CHECK_STR(row(v, 0), "AAAAA"); CHECK_STR(row(v, 3), "CCCCC"); CHECK_STR(row(v, 4), "short");
    CHECK_STR(row(v, 5), "$ x"); CURSOR(v, 3, 5);
    cui_vt_resize(v, 20, 7);
    /* the program heard of every size (no cui_vt_resize_sent): the row that left stays in the scrollback, cut from its line
     * there as the console host has it (the screen's first row starts a line); the screen holds the rest */
    CHECK_EQ(cui_vt_scrollback_count(v), 1);
    CHECK_STR(row(v, -1), "AAAAA"); CHECK_STR(row(v, 0), "AAAAABBBBBBBBBBCCCCC"); CHECK_STR(row(v, 1), "short"); CHECK_STR(row(v, 2), "$ x");
    CURSOR(v, 3, 2);
    /* once that part scrolls into the scrollback too, the line is whole again at the next reflow */
    W(v, "\r\n\r\n\r\n\r\n\r\n\r\n\r\n");
    cui_vt_resize(v, 25, 7);
    CHECK_STR(row(v, -3), "AAAAAAAAAABBBBBBBBBBCCCCC"); CHECK_STR(row(v, -2), "short"); CHECK_STR(row(v, -1), "$ x");
    cui_vt_destroy(v);
}

static void test_scrollback(void) {
    cui_vt *v = mk(40, 5, 1000, RF);
    char b[128];
    for (int i = 0; i < 20; i++) {                             /* 60 characters a line: two rows each */
        int n = snprintf(b, sizeof b, "%02d:", i);
        while (n < 60) { b[n] = (char)('a' + (n + i) % 26); n++; }
        b[n] = 0; W(v, b); W(v, "\r\n");
    }
    W(v, "end");
    int sb = cui_vt_scrollback_count(v);
    CHECK_EQ(sb, 41 - 5);                                      /* 40 rows of lines and "end": the screen keeps 5 */
    cui_vt_resize(v, 60, 5);                                   /* one row a line */
    CHECK_EQ(cui_vt_scrollback_count(v), 18);                  /* lines 0..17; 18, 19 and "end" on the screen */
    for (int k = 1; k <= cui_vt_scrollback_count(v); k++) CHECK_EQ(strlen(row(v, -k)), 60);
    CHECK(!strncmp(row(v, -1), "17:", 3));
    CHECK(!strncmp(row(v, -18), "00:", 3));
    CHECK(!strncmp(row(v, 0), "18:", 3)); CHECK(!strncmp(row(v, 1), "19:", 3)); CHECK_STR(row(v, 2), "end"); CURSOR(v, 3, 2);
    cui_vt_resize(v, 30, 5);                                   /* two rows a line again, nothing lost */
    CHECK(!strncmp(row(v, -cui_vt_scrollback_count(v)), "00:", 3));
    CHECK_EQ(strlen(row(v, -cui_vt_scrollback_count(v))), 30);
    cui_vt_resize(v, 200, 5);
    CHECK(!strncmp(row(v, -cui_vt_scrollback_count(v)), "00:", 3));
    CHECK_EQ(strlen(row(v, -cui_vt_scrollback_count(v))), 60);
    cui_vt_destroy(v);
}

static void test_wide(void) {
    cui_vt *v = mk(5, 4, 100, RF);
    W(v, "abcd\xe4\xbd\xa0" "e");                              /* the wide one does not fit in the last cell: padding there */
    CHECK_STR(row(v, 0), "abcd"); CHECK_STR(row(v, 1), "\xe4\xbd\xa0" "e");
    cui_vt_resize(v, 6, 4);                                    /* it fits now, and the padding is gone */
    CHECK_STR(row(v, 0), "abcd\xe4\xbd\xa0"); CHECK_STR(row(v, 1), "e");
    cui_vt_cell c;
    CHECK(cui_vt_cell_at(v, 4, 0, &c)); CHECK_EQ(c.ch, 0x4F60);
    cui_vt_resize(v, 7, 4);
    CHECK_STR(row(v, 0), "abcd\xe4\xbd\xa0" "e"); CHECK_STR(row(v, 1), "");
    cui_vt_resize(v, 5, 4);                                    /* back: never split across rows */
    CHECK_STR(row(v, 0), "abcd"); CHECK_STR(row(v, 1), "\xe4\xbd\xa0" "e");
    CHECK(cui_vt_cell_at(v, 4, 0, &c)); CHECK_EQ(c.ch, 0);
    CHECK(cui_vt_cell_at(v, 0, 1, &c)); CHECK_EQ(c.ch, 0x4F60);
    cui_vt_resize(v, 3, 4);
    CHECK_STR(row(v, 0), "abc"); CHECK_STR(row(v, 1), "d\xe4\xbd\xa0"); CHECK_STR(row(v, 2), "e");
    cui_vt_resize(v, 9, 4);
    CHECK_STR(row(v, 0), "abcd\xe4\xbd\xa0" "e");
    cui_vt_destroy(v);
    v = mk(4, 4, 100, RF);
    W(v, "\xe4\xbd\xa0\xe4\xbd\xa0\xe4\xbd\xa0");              /* exactly two rows */
    cui_vt_resize(v, 3, 4);
    CHECK_STR(row(v, 0), "\xe4\xbd\xa0"); CHECK_STR(row(v, 1), "\xe4\xbd\xa0"); CHECK_STR(row(v, 2), "\xe4\xbd\xa0");
    cui_vt_resize(v, 6, 4);
    CHECK_STR(row(v, 0), "\xe4\xbd\xa0\xe4\xbd\xa0\xe4\xbd\xa0"); CHECK_STR(row(v, 1), "");
    cui_vt_destroy(v);
}

static void test_attributes(void) {
    cui_vt *v = mk(10, 4, 100, RF);
    W(v, "\x1b[1;31mRRRRR\x1b[0m\x1b[42mGGGGGGGG\x1b[0m\x1b[38;2;1;2;3mT\x1b[0m");
    CHECK_STR(row(v, 0), "RRRRRGGGGG"); CHECK_STR(row(v, 1), "GGGT");
    cui_vt_resize(v, 20, 4);
    CHECK_STR(row(v, 0), "RRRRRGGGGGGGGT");
    cui_vt_cell c;
    cui_vt_cell_at(v, 0, 0, &c); CHECK_EQ(c.fg, 1); CHECK(c.attrs & CUI_VT_BOLD); CHECK_EQ(c.bg, -1);
    cui_vt_cell_at(v, 12, 0, &c); CHECK_EQ(c.ch, 'G'); CHECK_EQ(c.bg, 2); CHECK_EQ(c.fg, -1); CHECK(!(c.attrs & CUI_VT_BOLD));
    cui_vt_cell_at(v, 13, 0, &c); CHECK_EQ(c.ch, 'T'); CHECK_EQ(c.fg, 0x1000000 | 0x010203);
    cui_vt_resize(v, 4, 4);
    cui_vt_cell_at(v, 0, 3, &c); CHECK_EQ(c.ch, 'G'); CHECK_EQ(c.bg, 2);
    cui_vt_cell_at(v, 1, 3, &c); CHECK_EQ(c.ch, 'T'); CHECK_EQ(c.fg, 0x1000000 | 0x010203);
    cui_vt_cell_at(v, 0, 1, &c); CHECK_EQ(c.ch, 'R'); CHECK_EQ(c.fg, 1);
    cui_vt_destroy(v);
}

static void test_cursor(void) {
    cui_vt *v = mk(10, 5, 100, RF);
    W(v, "0123456789abcdef\x1b[2;4H");                         /* the cursor on 'd', offset 13 of the line */
    cui_vt_resize(v, 4, 5);
    CURSOR(v, 1, 3);
    W(v, "X");
    cui_vt_resize(v, 10, 5);
    CHECK_STR(row(v, 1), "abcXef"); CURSOR(v, 4, 1);
    cui_vt_destroy(v);
    /* past the last column (the next character wraps): it stays past its cell */
    v = mk(5, 4, 100, RF);
    W(v, "abcde");
    cui_vt_resize(v, 10, 4);
    CURSOR(v, 5, 0);
    W(v, "f");
    CHECK_STR(row(v, 0), "abcdef");
    cui_vt_destroy(v);
    v = mk(10, 4, 100, RF);
    W(v, "abcdefghij");
    cui_vt_resize(v, 5, 4);
    CHECK_STR(row(v, 1), "fghij"); CURSOR(v, 4, 1);
    W(v, "k");
    CHECK_STR(row(v, 2), "k"); CURSOR(v, 1, 2);
    cui_vt_destroy(v);
    /* a cursor right of the text keeps its column, and the blank rows below it do not count */
    v = mk(20, 6, 100, RF);
    W(v, "PS C:\\> \x1b[15G");
    cui_vt_resize(v, 10, 6);
    CURSOR(v, 4, 1);
    CHECK_STR(row(v, 0), "PS C:\\> ");
    cui_vt_resize(v, 20, 6);
    CURSOR(v, 14, 0);
    cui_vt_destroy(v);
}

static void test_empty_line_and_soft_wrap(void) {
    cui_vt *v = mk(5, 6, 100, RF);
    W(v, "abcde\r\n\r\nxy\r\nabcdefg");                       /* a full row and a line end is not a soft wrap; the last line is */
    cui_vt_resize(v, 10, 6);
    CHECK_STR(row(v, 0), "abcde"); CHECK_STR(row(v, 1), ""); CHECK_STR(row(v, 2), "xy"); CHECK_STR(row(v, 3), "abcdefg");
    cui_vt_resize(v, 3, 6);                                    /* 7 rows: the top one leaves */
    CHECK_STR(row(v, -1), "abc");
    CHECK_STR(row(v, 0), "de"); CHECK_STR(row(v, 1), ""); CHECK_STR(row(v, 2), "xy");
    CHECK_STR(row(v, 3), "abc"); CHECK_STR(row(v, 4), "def"); CHECK_STR(row(v, 5), "g"); CURSOR(v, 1, 5);
    cui_vt_resize(v, 10, 6);
    CHECK_STR(row(v, -1), "abc"); CHECK_STR(row(v, 0), "de"); CHECK_STR(row(v, 1), ""); CHECK_STR(row(v, 2), "xy"); CHECK_STR(row(v, 3), "abcdefg");
    cui_vt_destroy(v);
    /* spaces at a line's end go, inside the line they stay */
    v = mk(10, 4, 100, RF);
    W(v, "a b       \r\nnext");
    cui_vt_resize(v, 4, 4);
    CHECK_STR(row(v, 0), "a b"); CHECK_STR(row(v, 1), "next");
    cui_vt_destroy(v);
}

static void test_alt_screen(void) {
    for (int told = 0; told < 2; told++) {
        cui_vt *v = mk(10, 4, 100, RF);
        if (told) cui_vt_resize_sent(v);
        W(v, "0123456789abc\r\n$ ");
        W(v, "\x1b[?1049h\x1b[H" "ALT SCREEN");
        cui_vt_resize(v, 5, 4);
        CHECK_STR(row(v, 0), "ALT S");                          /* the alternate screen is cut, not reflowed */
        if (told) cui_vt_resize_sent(v);                       /* waits: the host reflows the main screen when it is left */
        cui_vt_resize(v, 10, 4);
        CHECK_STR(row(v, 0), "ALT S");
        W(v, "\x1b[?1049l");
        CHECK_STR(row(v, 0), "0123456789"); CHECK_STR(row(v, 1), "abc"); CHECK_STR(row(v, 2), "$ "); CURSOR(v, 2, 2);
        CHECK_EQ(cui_vt_scrollback_count(v), 0);
        W(v, "\x1b[?1049h");                                   /* leave it at another width */
        cui_vt_resize(v, 5, 4);
        W(v, "\x1b[?1049l");
        CHECK_STR(row(v, 0), "01234"); CHECK_STR(row(v, 2), "abc"); CURSOR(v, 2, 3);
        cui_vt_destroy(v);
    }
}

/* a drag the program hears of only at its end: what a narrow width pushed out comes back */
static void test_drag(void) {
    cui_vt *v = mk(20, 4, 100, RF);
    cui_vt_resize_sent(v);
    W(v, "AAAAAAAAAABBBBBBBBBB\r\nCCCCCCCCCCDDDDDDDDDD\r\n$ ");
    cui_vt_resize(v, 10, 4);                                   /* 5 rows: one leaves */
    CHECK_EQ(cui_vt_scrollback_count(v), 1); CHECK_STR(row(v, -1), "AAAAAAAAAA");
    cui_vt_resize(v, 5, 4);
    CHECK_EQ(cui_vt_scrollback_count(v), 5);
    cui_vt_resize(v, 20, 4);                                   /* the host still has it all: so has the screen */
    CHECK_EQ(cui_vt_scrollback_count(v), 0);
    CHECK_STR(row(v, 0), "AAAAAAAAAABBBBBBBBBB"); CHECK_STR(row(v, 1), "CCCCCCCCCCDDDDDDDDDD"); CHECK_STR(row(v, 2), "$ "); CURSOR(v, 2, 2);
    cui_vt_resize(v, 10, 4);
    cui_vt_resize_sent(v);                                     /* the host heard of 10: its first row is gone for it too */
    cui_vt_resize(v, 20, 4);
    CHECK_EQ(cui_vt_scrollback_count(v), 1);
    CHECK_STR(row(v, -1), "AAAAAAAAAA"); CHECK_STR(row(v, 0), "BBBBBBBBBB"); CHECK_STR(row(v, 1), "CCCCCCCCCCDDDDDDDDDD");
    cui_vt_resize(v, 10, 4);                                   /* output starts from the screen as it is */
    W(v, "x");
    cui_vt_resize(v, 20, 4);
    CHECK_STR(row(v, 0), "BBBBBBBBBB"); CHECK_STR(row(v, 1), "CCCCCCCCCCDDDDDDDDDD"); CHECK_STR(row(v, 2), "$ x");
    cui_vt_destroy(v);
}

static void test_off_by_default(void) {
    cui_vt *v = cui_vt_create(10, 4, 100);
    W(v, "0123456789abcde");
    cui_vt_resize(v, 5, 4);
    cui_vt_resize(v, 10, 4);
    CHECK_STR(row(v, 0), "01234"); CHECK_STR(row(v, 1), "abcde");      /* cut, as always */
    CHECK_EQ(cui_vt_reflows(v), 0);
    cui_vt_destroy(v);
    v = mk(10, 4, 100, RF);                                    /* and off again: the feature goes */
    cui_vt_set_features(v, 0);
    W(v, "0123456789abcde");
    cui_vt_resize(v, 5, 4);
    cui_vt_resize(v, 10, 4);
    CHECK_STR(row(v, 0), "01234");
    cui_vt_destroy(v);
}

/* a row holding a sixel picture keeps its row and its picture through a reflow of the scrollback */
static void test_picture_rows(void) {
    cui_vt *v = mk(20, 4, 100, RF | CUI_VT_FEATURE_SIXEL);
    W(v, "\x1bP9;1;0q\"1;1;20;18#0;2;100;0;0#0!20~-!20~-!20~\x1b\\");   /* 20 x 18 red: 2 cells of one row */
    CHECK_EQ(cui__vt_pic_count(v), 1);
    W(v, "\r\n0123456789012345678901234\r\n");
    CHECK_EQ(cui__vt_pic_count(v), 1);
    for (int i = 0; i < 4; i++) W(v, "....................more\r\n");
    CHECK_EQ(cui__vt_pic_count(v), 1);                         /* (an empty first scrollback line is kept when the next one comes) */
    int at = 0;
    for (int r = -1; r >= -cui_vt_scrollback_count(v); r--) if (cui__vt_pic_px(v, r, 0, 0)) { at = r; break; }
    CHECK(at < 0);
    int n0 = cui__vt_pic_count(v);
    CHECK_EQ(n0, 1);
    cui_vt_resize(v, 10, 4);
    int at2 = 0;
    for (int r = -1; r >= -cui_vt_scrollback_count(v); r--) if (cui__vt_pic_px(v, r, 0, 0)) { at2 = r; break; }
    CHECK(at2 < 0);
    CHECK_EQ(cui__vt_pic_count(v), 1);
    CHECK_EQ(cui__vt_pic_px(v, at2, 5, 5), 0xFF0000FFu);
    CHECK_STR(row(v, at2 + 1), "0123456789");                 /* the text after it reflowed */
    CHECK_STR(row(v, at2 + 3), "01234");
    cui_vt_resize(v, 20, 4);
    int at3 = 0;
    for (int r = -1; r >= -cui_vt_scrollback_count(v); r--) if (cui__vt_pic_px(v, r, 0, 0)) { at3 = r; break; }
    CHECK(at3 < 0); CHECK_EQ(cui__vt_pic_count(v), 1);
    CHECK_STR(row(v, at3 + 1), "01234567890123456789"); CHECK_STR(row(v, at3 + 2), "01234");
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;

static cui_vt_result build(cui_ctx *ui, cui_vt *vt, float w) {
    cui_vt_result r;
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { r = cui_vt_view(ui, "t", vt, .width = CUI_PX(w)); }
    cui_end_frame(ui);
    return r;
}

static void test_view(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 800, 300, 1);
    cui_set_reduced_motion(ui, true);
    cui_vt *vt = mk(80, 10, 1000, RF);
    for (int i = 0; i < 3; i++) build(ui, vt, 700);
    int c0, r0; cui_vt_size(vt, &c0, &r0);
    char b[300];
    for (int i = 0; i < 60; i++) {                             /* lines of 1.5 times the width */
        int n = snprintf(b, sizeof b, "L%02d ", i);
        while (n < c0 * 3 / 2) { b[n] = (char)('a' + n % 26); n++; }
        memcpy(b + n, "\r\n", 3);
        W(vt, b);
    }
    build(ui, vt, 700);
    cui_input_scroll(ui, 0, 10);                               /* look back: 30 rows */
    build(ui, vt, 700); build(ui, vt, 700);
    /* the text at the view's top before and after a reflow: same line */
    unsigned rf0 = cui_vt_reflows(vt);
    cui_vt_result r1 = build(ui, vt, 400), r2 = build(ui, vt, 400);      /* (the view sees its new size a frame later) */
    CHECK(r1.resized || r2.resized);
    CHECK_EQ(cui_vt_reflows(vt), rf0 + 1);
    int c1; cui_vt_size(vt, &c1, NULL);
    CHECK(c1 < c0);
    /* the lines are whole in the scrollback after the reflow (each starts with L and its number) */
    int starts = 0;
    for (int k = 1; k <= cui_vt_scrollback_count(vt); k++) if (row(vt, -k)[0] == 'L') starts++;
    CHECK(starts >= 50);
    cui_vt_destroy(vt);
    cui_destroy(ui);
}

/* ---- speed ---- */

static double time_reflow(cui_vt *v, int cols, int rows) {
    double t0 = now_ms();
    cui_vt_resize(v, cols, rows);
    return now_ms() - t0;
}

static void test_speed(void) {
    /* 5000 lines of a typical log (about 48 cells a line: what the ring holds for 5000 lines), at 200 columns */
    cui_vt *v = mk(200, 50, 5000, RF);
    size_t cap = 8 << 20, n = 0;
    char *buf = (char *)malloc(cap);
    for (int i = 0; i < 5100; i++) n += (size_t)snprintf(buf + n, cap - n, "\x1b[32m%6d\x1b[0m  C:\\Windows\\System32\\drivers %d\r\n", i, i * 7);
    cui_vt_write(v, buf, n);
    int lines = cui_vt_scrollback_count(v);
    double a = time_reflow(v, 120, 50), b = time_reflow(v, 200, 50), c = time_reflow(v, 80, 50), d = time_reflow(v, 200, 50);
    printf("  reflow of %d scrollback lines of a log at 200 columns: to 120 %.2f ms, to 200 %.2f, to 80 %.2f, to 200 %.2f\n", lines, a, b, c, d);
    CHECK(lines >= 4900);
    CHECK_EQ(cui_vt_scrollback_count(v), lines);
    CHECK(a < 100 && b < 100 && c < 100 && d < 100);
    cui_vt_destroy(v);
    v = cui_vt_create(200, 50, 5000);                          /* before: the same resizes cut */
    cui_vt_write(v, buf, n);
    a = time_reflow(v, 120, 50); b = time_reflow(v, 200, 50); c = time_reflow(v, 80, 50); d = time_reflow(v, 200, 50);
    printf("  the same without REFLOW (cut): %.3f, %.3f, %.3f, %.3f ms\n", a, b, c, d);
    cui_vt_destroy(v);
    /* the ring full of rows 200 wide (it keeps 5000 * 48 cells: 1200 such lines), and the screen too */
    v = mk(200, 50, 5000, RF);
    n = 0;
    for (int i = 0; i < 1400; i++) {
        n += (size_t)snprintf(buf + n, cap - n, "\x1b[3%dm", i % 8);
        for (int k = 0; k < 200; k++) buf[n++] = (char)('a' + (i + k) % 26);
    }
    cui_vt_write(v, buf, n);
    lines = cui_vt_scrollback_count(v);
    a = time_reflow(v, 120, 50); b = time_reflow(v, 200, 50); c = time_reflow(v, 80, 50); d = time_reflow(v, 200, 50);
    printf("  reflow of a full ring (%d rows of 200 cells, %d cells): to 120 %.2f ms, to 200 %.2f, to 80 %.2f, to 200 %.2f\n", lines, lines * 200, a, b, c, d);
    CHECK(a < 100 && b < 100 && c < 100 && d < 100);
    /* a drag: 40 steps from 200 to 80 columns and back, as the program hears only of the last */
    cui_vt_resize_sent(v);
    double t0 = now_ms(), worst = 0;
    for (int s = 0; s < 40; s++) { int cols = s < 20 ? 200 - s * 6 : 80 + (s - 20) * 6; double t = time_reflow(v, cols, 50); if (t > worst) worst = t; }
    printf("  a drag of 40 reflows: %.2f ms in all, the slowest %.2f ms\n", now_ms() - t0, worst);
    free(buf);
    cui_vt_destroy(v);
}

int main(void) {
    g_sw = cui_sw_create();
    test_round_trip();
    test_scrollback();
    test_wide();
    test_attributes();
    test_cursor();
    test_empty_line_and_soft_wrap();
    test_alt_screen();
    test_drag();
    test_off_by_default();
    test_picture_rows();
    test_view();
    test_speed();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
