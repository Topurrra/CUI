/* Tests for the VT screen's characters by Unicode 17 (CUI_VT_FEATURE_CLUSTERS) and the view's fitted symbols and contrast floor:
 * every case of Unicode's GraphemeBreakTest.txt, every fully-qualified emoji of emoji-test.txt (one cluster, 2 cells, kept
 * whole on the screen), the emoji and text style variation sequences, the East Asian Width wide ranges, the screen's clusters
 * (marks, VS16 and VS15 widening and narrowing at the right edge, ZWJ, flags, Hangul, Indic conjuncts, split writes, a moved
 * cursor, the table filling up and being collected, the feature turned off, saving and loading), DECRQM 2027, nothing changed
 * without the feature, and in the view: emoji drawn in their cells, .fit_symbols and .adjust_colors. Prints the parse speed
 * with the feature on and off. With a folder argument it writes a test page (vt-cells-dark.png, -light.png). Runs headless
 * with the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test_vt_width_data.h"

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b))) { g_failed++; printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static void W1(cui_vt *v, const char *s) { for (; *s; s++) cui_vt_write(v, s, 1); }
static cui_vt *mkvt(int cols, int rows, int sb) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, CUI_VT_FEATURE_CLUSTERS); return v; }
static int CX(cui_vt *v) { int x, y; cui_vt_cursor(v, &x, &y); return x; }
static char g_t[512];
static const char *CT(cui_vt *v, int x, int y) { cui_vt_cell_text(v, x, y, g_t, sizeof g_t); return g_t; }
static const char *LT(cui_vt *v, int row) { cui_vt_line_text(v, row, g_t, sizeof g_t); return g_t; }

static int utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* GraphemeBreakTest.txt: the clusters cui_vt_grapheme_next finds start exactly where Unicode says */
static void test_break_cases(void) {
    const uint32_t *p = gbt_cases;
    int bad = 0;
    for (int t = 0; t < GBT_COUNT; t++) {
        char s[256]; int len = 0, starts[64], ns = 0;
        for (; *p != 0xFFFFFFFFu; p++) {
            if (*p & 0x80000000u) starts[ns++] = len;
            len += utf8(*p & 0x7FFFFFFFu, s + len);
        }
        p++;
        int got[64], ng = 0;
        for (int i = 0; i < len && ng < 64;) {
            got[ng++] = i;
            int k = cui_vt_grapheme_next(s + i, len - i, NULL);
            if (k <= 0) break;
            i += k;
        }
        bool ok = ng == ns;
        for (int i = 0; ok && i < ng; i++) ok = got[i] == starts[i];
        if (!ok && bad++ < 5) printf("  grapheme case %d differs\n", t);
    }
    CHECK_EQ(bad, 0);
    printf("  %d grapheme break cases\n", GBT_COUNT);
}

/* emoji-test.txt: each fully-qualified emoji is one cluster of 2 cells, as text and on the screen */
static void test_emoji(void) {
    int bad_len = 0, bad_w = 0, bad_scr = 0;
    cui_vt *v = mkvt(40, 4, 0);
    for (int i = 0; i < EMOJI_FULL_COUNT; i++) {
        const char *e = emoji_full[i];
        int n = (int)strlen(e), w;
        int k = cui_vt_grapheme_next(e, n, &w);
        if (k != n && bad_len++ < 5) printf("  emoji %d: %d of %d bytes\n", i, k, n);
        if (w != 2 && bad_w++ < 5) printf("  emoji %d: %d cells\n", i, w);
        W(v, "\x1b[H\x1b[2J");
        W(v, e);
        W(v, "x");
        bool ok = CX(v) == 3 && cui_vt_cell_width(v, 0, 0) == 2 && !strcmp(CT(v, 0, 0), e) && !strcmp(CT(v, 2, 0), "x");
        if (!ok && bad_scr++ < 5) printf("  emoji %d on the screen: cursor %d\n", i, CX(v));
    }
    cui_vt_destroy(v);
    CHECK_EQ(bad_len, 0);
    CHECK_EQ(bad_w, 0);
    CHECK_EQ(bad_scr, 0);
    printf("  %d fully-qualified emoji\n", EMOJI_FULL_COUNT);
}

/* the variation sequences: FE0F makes 2 cells, FE0E 1 for an emoji by default (else the base's own width) */
static void test_vs(void) {
    int bad = 0;
    for (int i = 0; i < VS_BASES_COUNT; i++) {
        char b[16]; int n = utf8(vs_bases[i], b), w0, w1, w2;
        cui_vt_grapheme_next(b, n, &w0);
        memcpy(b + n, "\xEF\xB8\x8F", 3);
        cui_vt_grapheme_next(b, n + 3, &w1);
        memcpy(b + n, "\xEF\xB8\x8E", 3);
        cui_vt_grapheme_next(b, n + 3, &w2);
        bool pres_wide = w0 == 2 && vs_bases[i] >= 0x80;           /* the emoji-by-default ones are wide alone */
        if ((w1 != 2 || (pres_wide ? w2 != 1 && w2 != 2 : w2 != w0)) && bad++ < 5) printf("  VS base %X: %d %d %d\n", vs_bases[i], w0, w1, w2);
    }
    CHECK_EQ(bad, 0);
    /* a few by name */
    CHECK_EQ(cui_vt_text_width("\xE2\x9D\xA4", -1), 1);              /* U+2764 heart: text */
    CHECK_EQ(cui_vt_text_width("\xE2\x9D\xA4\xEF\xB8\x8F", -1), 2);  /* with VS16: the red heart */
    CHECK_EQ(cui_vt_text_width("\xE2\x8C\x9A", -1), 2);              /* U+231A watch: emoji by default */
    CHECK_EQ(cui_vt_text_width("\xE2\x8C\x9A\xEF\xB8\x8E", -1), 1);  /* with VS15: text */
    CHECK_EQ(cui_vt_text_width("1\xEF\xB8\x8F\xE2\x83\xA3", -1), 2); /* keycap one */
    CHECK_EQ(cui_vt_text_width("a\xEF\xB8\x8F", -1), 1);             /* a letter has no emoji style */
}

/* East Asian Width W and F are 2 cells; the few marks among them have nothing to show alone */
static void test_eaw(void) {
    int bad = 0, zero = 0;
    for (int i = 0; i < EAW_WIDE_COUNT; i++) {
        uint32_t probe[3] = { eaw_wide[i][0], eaw_wide[i][1], (eaw_wide[i][0] + eaw_wide[i][1]) / 2 };
        for (int k = 0; k < 3; k++) {
            char b[8]; int n = utf8(probe[k], b), w;
            cui_vt_grapheme_next(b, n, &w);
            if (w == 0) zero++;
            else if (w != 2 && bad++ < 5) printf("  U+%X: %d cells\n", probe[k], w);
        }
    }
    CHECK_EQ(bad, 0);
    CHECK(zero < 20);
    CHECK_EQ(cui_vt_text_width("\xE4\xB8\xAD\xE6\x96\x87", -1), 4);  /* two CJK characters */
    CHECK_EQ(cui_vt_text_width("\xE2\x94\x80\xE2\x95\xAD", -1), 2);  /* box drawing: narrow */
    CHECK_EQ(cui_vt_text_width("\xE2\x9A\xA1\xE2\x9C\x85\xE2\xAD\x90", -1), 6);   /* BMP emoji: 26A1 2705 2B50 */
    CHECK_EQ(cui_vt_text_width("\xF0\x9F\x9A\x80", -1), 2);          /* U+1F680 rocket */
    CHECK_EQ(cui_vt_text_width("\xF0\x9F\x8C\xA1", -1), 1);          /* U+1F321 thermometer: text by default */
    CHECK_EQ(cui_vt_text_width("\xC2\xB1\xCE\xB1", -1), 2);          /* ambiguous: narrow */
}

static void test_screen(void) {
    cui_vt *v = mkvt(20, 4, 100);
    char t[128];
    W(v, "e\xCC\x81x");                                               /* e + combining acute */
    CHECK_EQ(CX(v), 2);
    CHECK_STR(CT(v, 0, 0), "e\xCC\x81");
    CHECK_STR(LT(v, 0), "e\xCC\x81x");
    cui_vt_cell c;
    cui_vt_cell_at(v, 0, 0, &c); CHECK_EQ(c.ch, 'e');
    W(v, "\r\n\xE2\x9D\xA4\xEF\xB8\x8F!");                            /* red heart: widened to 2 */
    CHECK_EQ(CX(v), 3); CHECK_EQ(cui_vt_cell_width(v, 0, 1), 2); CHECK_STR(CT(v, 2, 1), "!");
    W(v, "\r\n\xE2\x8C\x9A\xEF\xB8\x8E!");                            /* watch with VS15: narrowed to 1 */
    CHECK_EQ(CX(v), 2); CHECK_EQ(cui_vt_cell_width(v, 0, 2), 1); CHECK_STR(CT(v, 1, 2), "!");
    W(v, "\x1b[2J\x1b[H");
    const char *fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";   /* man ZWJ woman ZWJ girl */
    W(v, fam); CHECK_EQ(CX(v), 2); CHECK_STR(CT(v, 0, 0), fam);
    W(v, "\x1b[2J\x1b[H\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7\xF0\x9F\x87\xAB");   /* US GB and a lone F */
    CHECK_EQ(CX(v), 6); CHECK_STR(CT(v, 0, 0), "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8"); CHECK_STR(CT(v, 2, 0), "\xF0\x9F\x87\xAC\xF0\x9F\x87\xA7");
    CHECK_STR(CT(v, 4, 0), "\xF0\x9F\x87\xAB");
    W(v, "\x1b[2J\x1b[H\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD.");           /* thumbs up, medium skin tone */
    CHECK_EQ(CX(v), 3); CHECK_STR(CT(v, 0, 0), "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");
    W(v, "\x1b[2J\x1b[H\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8.");       /* Hangul jamo L V T: one syllable, 2 cells */
    CHECK_EQ(CX(v), 3);
    W(v, "\x1b[2J\x1b[H\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7.");       /* Devanagari ka virama ssa: one conjunct */
    CHECK_EQ(CX(v), 2); CHECK_STR(CT(v, 0, 0), "\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7");
    /* byte by byte, the same */
    W(v, "\x1b[2J\x1b[H"); W1(v, fam); W1(v, "e\xCC\x81\xCC\x82");
    CHECK_EQ(CX(v), 3); CHECK_STR(CT(v, 0, 0), fam); CHECK_STR(CT(v, 2, 0), "e\xCC\x81\xCC\x82");
    /* a moved cursor: the mark has nothing to join and is dropped, as before */
    W(v, "\x1b[2J\x1b[Ha\x1b[5G\xCC\x81");
    CHECK_STR(CT(v, 0, 0), "a"); CHECK_EQ(CX(v), 4);
    /* an SGR between a character and its mark keeps them together */
    W(v, "\x1b[2J\x1b[Ha\x1b[31m\xCC\x81");
    CHECK_STR(CT(v, 0, 0), "a\xCC\x81");
    /* VS16 in the last column: no room to widen, the cluster stays one cell; VS15 on a wide one at the edge */
    W(v, "\x1b[2J\x1b[1;20H\xE2\x9D\xA4\xEF\xB8\x8F");
    CHECK_STR(CT(v, 19, 0), "\xE2\x9D\xA4\xEF\xB8\x8F"); CHECK_EQ(cui_vt_cell_width(v, 19, 0), 1);
    W(v, "\x1b[2J\x1b[2;19H\xE2\x8C\x9A");
    CHECK_EQ(cui_vt_cell_width(v, 18, 1), 2);
    W(v, "\xEF\xB8\x8E" "z");
    CHECK_EQ(cui_vt_cell_width(v, 18, 1), 1); CHECK_STR(CT(v, 19, 1), "z");
    /* a long run of marks: 16 code points kept, the rest dropped, one cell */
    W(v, "\x1b[2J\x1b[H");
    char z[256]; int zn = 0; z[zn++] = 'q';
    for (int i = 0; i < 40; i++) { z[zn++] = (char)0xCC; z[zn++] = (char)(0x80 + (i % 16)); }
    z[zn] = 0;
    W(v, z); W(v, "!");
    CHECK_EQ(CX(v), 2); CHECK_EQ((int)strlen(CT(v, 0, 0)), 1 + 15 * 2);
    /* selection text and the whole line carry clusters */
    W(v, "\x1b[2J\x1b[H"); W(v, fam); W(v, " ok");
    snprintf(t, sizeof t, "%s ok", fam);
    CHECK_STR(LT(v, 0), t);
    cui_vt_destroy(v);
}

/* the table fills with clusters that scrolled away: they are collected and the ones still shown stay right */
static void test_collect(void) {
    cui_vt *v = mkvt(80, 24, 200);
    size_t m0 = cui_vt_memory(v);
    char b[16];
    double t0 = now_ms();
    for (int i = 0; i < 90000; i++) {
        uint32_t base = 0x4E00 + (uint32_t)(i % 20000), mark = 0x300 + (uint32_t)(i / 20000);
        int n = utf8(base, b); n += utf8(mark, b + n);
        cui_vt_write(v, b, (size_t)n);
        if (i % 40 == 39) W(v, "\r\n");
    }
    double t1 = now_ms();
    /* the last row written holds clusters 89960.. 89999 */
    for (int k = 0; k < 40; k += 13) {
        int i = 89960 + k;
        uint32_t base = 0x4E00 + (uint32_t)(i % 20000), mark = 0x300 + (uint32_t)(i / 20000);
        int n = utf8(base, b); n += utf8(mark, b + n); b[n] = 0;
        CHECK_STR(CT(v, k * 2, 22), b);
    }
    size_t m1 = cui_vt_memory(v);
    CHECK(m1 - m0 < 8u * 1024u * 1024u);
    printf("  90000 distinct clusters through a 200-line scrollback: %.1f ms, %zu KB more memory\n", t1 - t0, (m1 - m0) / 1024);
    /* off: every cluster becomes its first character */
    cui_vt_set_features(v, 0);
    int n = utf8(0x4E00 + (uint32_t)(89960 % 20000), b); b[n] = 0;
    CHECK_STR(CT(v, 0, 22), b);
    CHECK(cui_vt_memory(v) <= m0 + 64);
    cui_vt_destroy(v);
}

/* without the feature: marks and selectors dropped, the old widths, nothing allocated */
static void test_off(void) {
    cui_vt *v = cui_vt_create(20, 4, 10);
    size_t m0 = cui_vt_memory(v);
    W(v, "e\xCC\x81x\xE2\x9D\xA4\xEF\xB8\x8F!");
    CHECK_STR(LT(v, 0), "ex\xE2\x9D\xA4!");
    CHECK_EQ(CX(v), 4);
    CHECK_EQ(cui_vt_memory(v), m0);
    cui_vt_set_features(v, CUI_VT_FEATURE_QUERIES);
    W(v, "\x1b[?2027$p");
    char r[64]; size_t n = cui_vt_reply(v, r, sizeof r); r[n] = 0;
    CHECK_STR(r, "\x1b[?2027;0$y");
    cui_vt_set_features(v, CUI_VT_FEATURE_QUERIES | CUI_VT_FEATURE_CLUSTERS);
    W(v, "\x1b[?2027$p");
    n = cui_vt_reply(v, r, sizeof r); r[n] = 0;
    CHECK_STR(r, "\x1b[?2027;3$y");
    cui_vt_destroy(v);
}

/* saved and loaded: the clusters come back whole */
static void test_save(void) {
    cui_vt *v = mkvt(30, 4, 50);
    const char *fam = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";
    W(v, "a\xCC\x81 "); W(v, fam); W(v, " \xE2\x9D\xA4\xEF\xB8\x8F end");
    char want[128]; cui_vt_line_text(v, 0, want, sizeof want);
    cui_vt_saved s = { 0 };
    CHECK(cui_vt_save(v, 100, &s) > 0);
    cui_vt *w = mkvt(30, 4, 50);
    CHECK(cui_vt_load(w, s.data, s.len));
    CHECK_STR(LT(w, 0), want);
    CHECK_EQ(cui_vt_cell_width(w, 2, 0), 2);                         /* "a" + mark, a space, the family */
    cui_vt_saved_free(&s);
    cui_vt_destroy(v); cui_vt_destroy(w);
}

/* parse speed: plain text and CJK with the feature off and on, emoji-heavy text on */
static void test_speed(void) {
    static char buf[1 << 20];
    const char *kinds[3] = { "ascii", "cjk", "emoji" };
    for (int kind = 0; kind < 3; kind++) {
        size_t n = 0;
        while (n + 64 < sizeof buf) {
            if (kind == 0) { memcpy(buf + n, "the quick brown fox jumps over 13 lazy dogs; ", 45); n += 45; }
            else if (kind == 1) { memcpy(buf + n, "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97 abc ", 14); n += 14; }
            else { memcpy(buf + n, "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD ok \xE2\x9D\xA4\xEF\xB8\x8F ", 18); n += 18; }
            if ((n % 200) < 18) buf[n++] = '\n';
        }
        double best[2] = { 1e9, 1e9 };
        for (int on = 0; on < 2; on++) {
            for (int rep = 0; rep < 5; rep++) {
                cui_vt *v = cui_vt_create(120, 40, 1000);
                if (on) cui_vt_set_features(v, CUI_VT_FEATURE_CLUSTERS);
                double t0 = now_ms();
                cui_vt_write(v, buf, n);
                double dt = now_ms() - t0;
                if (dt < best[on]) best[on] = dt;
                cui_vt_destroy(v);
            }
        }
        printf("  parse %s: %.0f MB/s off, %.0f MB/s with clusters\n", kinds[kind], (double)n / 1e3 / best[0], (double)n / 1e3 / best[1]);
    }
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
static uint32_t g_px[640 * 400];
typedef struct { cui_vt *vt; cui_vt_look *look; bool fit; int adjust; } scene;
static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { cui_vt_view(ui, "t", s->vt, .look = s->look, .fit_symbols = s->fit, .adjust_colors = s->adjust, .size = 16); }
    cui_end_frame(ui);
}
static void render(cui_ctx *ui) { cui_sw_render(g_sw, cui_get_draw_data(ui), g_px, 640, 400, 640 * 4, CUI_SW_RGBA); }
static cui_ctx *mkui(bool dark) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}
/* pixels in x0..x1, y0..y1 that differ from the background (the pixel at the given spot) by more than d */
static int ink(int x0, int y0, int x1, int y1, uint32_t bg, int d) {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint32_t p = g_px[y * 640 + x];
        int dr = (int)(p & 255) - (int)(bg & 255), dg = (int)((p >> 8) & 255) - (int)((bg >> 8) & 255), db = (int)((p >> 16) & 255) - (int)((bg >> 16) & 255);
        if (abs(dr) + abs(dg) + abs(db) > d) n++;
    }
    return n;
}
static int colorful(int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        uint32_t p = g_px[y * 640 + x];
        int r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255, mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        if (mx - mn > 60) n++;
    }
    return n;
}

/* the light theme's invisible "+": ANSI 7 the same as the background */
static void test_adjust(void) {
    cui_ctx *ui = mkui(false);
    cui_vt_look look; memset(&look, 0, sizeof look);
    look.bg = CUI_RGB(0xfafafa); look.fg = CUI_RGB(0x383a42);
    look.ansi[7] = CUI_RGB(0xfafafa);
    look.pad_x = 0; look.pad_y = 0;
    const char *rows[3] = { "\x1b[37m++++", "\x1b[38;2;250;250;250m++++", "\x1b[37;47m++++" };   /* the scheme's white, a true color, the same asked for both */
    int got[3][3];
    for (int mode = 0; mode < 3; mode++) for (int row = 0; row < 3; row++) {
        scene s = { mkvt(40, 10, 0), &look, false, mode };
        W(s.vt, "\x1b[?25l"); W(s.vt, rows[row]);                   /* no cursor outline */
        for (int i = 0; i < 3; i++) build(ui, &s);
        render(ui);
        got[mode][row] = ink(0, 0, 80, 60, g_px[399 * 640 + 320], 60);
        cui_vt_destroy(s.vt);
    }
    CHECK_EQ(got[0][0], 0);                 /* never: invisible, as before */
    CHECK(got[1][0] > 20);                  /* indexed: the scheme's white is moved off the background */
    CHECK_EQ(got[1][1], 0);                 /* ... a true color is not */
    CHECK(got[2][1] > 20);                  /* always: it is too */
    CHECK_EQ(got[2][2], 0);                 /* the same color asked for both stays hidden */
    printf("  contrast floor: %d ink pixels never, %d indexed, %d always\n", got[0][0], got[1][0], got[2][0]);
    cui_destroy(ui);
}

/* a symbol wider than its cell: at the left edge it runs into the next cell; fitted it stays in its own, centred */
static void test_fit(void) {
    cui_ctx *ui = mkui(true);
    cui_font f = cui_font_load_file(ui, "C:/Windows/Fonts/seguisym.ttf");
    if (f < 0) { printf("  fit: no Segoe UI Symbol here, skipped\n"); cui_destroy(ui); return; }
    cui_font_add_fallback(ui, CUI_FONT_MONO, f);
    cui_vt_look look; memset(&look, 0, sizeof look);
    look.bg = CUI_RGB(0x000000); look.fg = CUI_RGB(0xffffff);
    scene s = { mkvt(40, 4, 0), &look, false, 0 };
    W(s.vt, "\x1b[?25l\xE2\x99\x94");                                 /* U+2654 white chess king: Segoe UI Symbol only (no cursor) */
    float cw = 0;
    int spill[2], left[2], right[2];
    for (int fit = 0; fit < 2; fit++) {
        s.fit = fit != 0;
        for (int i = 0; i < 3; i++) build(ui, &s);
        render(ui);
        cw = cui_measure_text(ui, CUI_FONT_MONO, 16, "0", 1).width;
        int x0 = 0, x1 = 0, first = -1, last = -1;
        for (int x = 0; x < 120; x++) for (int y = 0; y < 30; y++) if ((g_px[y * 640 + x] & 255) > 100) { if (first < 0) first = x; last = x; }
        x0 = first; x1 = last;
        spill[fit] = x1 >= (int)cw ? x1 - (int)cw + 1 : 0;            /* the look has no padding: cell 0 is x 0..cw */
        left[fit] = x0; right[fit] = (int)cw - 1 - x1;
    }
    CHECK(spill[0] > 0);
    CHECK_EQ(spill[1], 0);
    CHECK(abs(left[1] - right[1]) <= 2);
    printf("  fit: unfitted ink runs %d px into the next cell, fitted margins %d/%d px\n", spill[0], left[1], right[1]);
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* an emoji cluster drawn by the emoji font fills its two cells */
static void test_emoji_view(void) {
    cui_ctx *ui = mkui(true);
    if (!cui_emoji_font_load_system(ui)) { printf("  emoji view: no system emoji font, skipped\n"); cui_destroy(ui); return; }
    cui_vt_look look; memset(&look, 0, sizeof look);
    look.bg = CUI_RGB(0x000000); look.fg = CUI_RGB(0xc8c8c8);
    /* a thumbs up with a skin tone (the font has it whole), then a ZWJ family the font may lack (its pieces, fitted) */
    const char *what[2] = { "\x1b[?25l\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD" "x", "\x1b[?25l\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7" "x" };
    for (int t = 0; t < 2; t++) {
        scene s = { mkvt(40, 4, 0), &look, false, 0 };
        W(s.vt, what[t]);
        for (int i = 0; i < 4; i++) build(ui, &s);
        render(ui);
        if (getenv("CUI_TEST_DUMP")) { char pth[512]; snprintf(pth, sizeof pth, "%s/vt-emoji-view-%d.png", getenv("CUI_TEST_DUMP"), t); cui_write_png(pth, g_px, 640, 400, 640 * 4); }
        float cw = cui_measure_text(ui, CUI_FONT_MONO, 16, "0", 1).width;
        int in_cells = colorful(0, 0, (int)(2 * cw) + 1, 30), beyond = colorful((int)(2 * cw) + 1, 0, 200, 30);
        CHECK(in_cells > (t ? 10 : 40));                               /* Windows 11's family is three glyphs the font places: its pieces, fitted */
        CHECK_EQ(beyond, 0);
        printf("  emoji view %d: %d colour pixels in its 2 cells, %d beyond\n", t, in_cells, beyond);
        cui_vt_destroy(s.vt);
    }
#if defined(_WIN32)
    /* Segoe UI Emoji names its ligatures through ccmp; its other lookups are steps of a contextual one */
    CHECK(cui_emoji_supported(ui, "\xF0\x9F\xA7\x91\xE2\x80\x8D\xF0\x9F\x92\xBB", -1));       /* technologist */
    CHECK(cui_emoji_supported(ui, "\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88", -1));   /* rainbow flag */
#endif
    cui_destroy(ui);
}

/* a frame (build and software render) of a screen full of coloured text, with the options off and on */
static void test_frame_cost(void) {
    cui_ctx *ui = mkui(true);
    double best[2] = { 1e9, 1e9 };
    for (int on = 0; on < 2; on++) {
        scene s = { cui_vt_create(80, 24, 0), NULL, on != 0, on ? CUI_VT_ADJUST_INDEXED : 0 };
        if (on) cui_vt_set_features(s.vt, CUI_VT_FEATURE_CLUSTERS);
        char b[256];
        for (int i = 0; i < 24; i++) {
            snprintf(b, sizeof b, "%08d \x1b[3%dm the quick brown fox \x1b[1;3%dm jumps over \x1b[0m the lazy dog \xE2\x86\x92 0123456789 abc\r\n", i, 1 + i % 7, 1 + (i / 7) % 7);
            W(s.vt, b);
        }
        for (int f = 0; f < 3; f++) { build(ui, &s); render(ui); }
        for (int f = 0; f < 40; f++) {
            double t0 = now_ms();
            build(ui, &s); render(ui);
            double dt = now_ms() - t0;
            if (dt < best[on]) best[on] = dt;
        }
        cui_vt_destroy(s.vt);
    }
    printf("  frame of 80x24 coloured text: %.3f ms as before, %.3f ms with clusters, fit and contrast\n", best[0], best[1]);
    cui_destroy(ui);
}

/* the test page: emoji, ZWJ, flags, VS15/VS16, CJK, box drawing and symbols, with the cells they take */
static void shots(const char *dir) {
    for (int dark = 0; dark < 2; dark++) {
        cui_ctx *ui = mkui(dark != 0);
        cui_emoji_font_load_system(ui);
        cui_font f = cui_font_load_file(ui, "C:/Windows/Fonts/seguisym.ttf");
        if (f >= 0) cui_font_add_fallback(ui, CUI_FONT_MONO, f);
        cui_font j = cui_font_load_file(ui, "C:/Windows/Fonts/msyh.ttc");
        if (j >= 0) cui_font_add_fallback(ui, CUI_FONT_MONO, j);
        scene s = { mkvt(60, 16, 0), NULL, true, CUI_VT_ADJUST_INDEXED };
        W(s.vt, "emoji    |\xF0\x9F\x9A\x80|\xE2\x9A\xA1|\xE2\x9C\x85|\xE2\xAD\x90|\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD|\r\n"
                "ZWJ      |\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7|\xF0\x9F\x8F\xB3\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x8C\x88|\r\n"
                "flags    |\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8|\xF0\x9F\x87\xAC\xF0\x9F\x87\xAA|\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5|\r\n"
                "VS16     |\xE2\x9D\xA4|\xE2\x9D\xA4\xEF\xB8\x8F|\xE2\x98\xBA|\xE2\x98\xBA\xEF\xB8\x8F|1\xEF\xB8\x8F\xE2\x83\xA3|\r\n"
                "VS15     |\xE2\x8C\x9A|\xE2\x8C\x9A\xEF\xB8\x8E|\r\n"
                "CJK      |\xE4\xB8\xAD\xE6\x96\x87|\xE3\x81\x8B\xE3\x81\xAA|\xED\x95\x9C\xEA\xB8\x80|\r\n"
                "marks    |e\xCC\x81|a\xCC\x8A|n\xCC\x83|\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7|\r\n"
                "box      |\xE2\x94\x8C\xE2\x94\x80\xE2\x94\xAC\xE2\x94\x80\xE2\x94\x90|\xE2\x96\x88\xE2\x96\x93\xE2\x96\x92|\r\n"
                "symbols  |\xE2\x86\x92|\xE2\x87\x92|\xE2\x88\x9E|\xE2\x99\x94|\xE2\x96\xB6|\xE2\x9C\x93|\xE2\x98\x85|\r\n"
                "contrast |\x1b[37m+ white\x1b[0m|\x1b[97m+ bright\x1b[0m|\x1b[30m+ black\x1b[0m|\r\n");
        for (int i = 0; i < 4; i++) build(ui, &s);
        render(ui);
        char path[512];
        snprintf(path, sizeof path, "%s/vt-cells-%s.png", dir, dark ? "dark" : "light");
        cui_write_png(path, g_px, 640, 400, 640 * 4);
        printf("  wrote %s\n", path);
        cui_vt_destroy(s.vt);
        cui_destroy(ui);
    }
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 1) { shots(argv[1]); cui_sw_destroy(g_sw); return 0; }
    test_break_cases();
    test_emoji();
    test_vs();
    test_eaw();
    test_screen();
    test_collect();
    test_off();
    test_save();
    test_adjust();
    test_fit();
    test_emoji_view();
    test_frame_cost();
    test_speed();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
