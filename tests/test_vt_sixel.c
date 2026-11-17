/* Tests for the VT screen's opt-in parts (cui_vt_set_features): the answers to queries with the features on and silence
 * with them off, sixel decoding (colors, repeat, transparency, raster attributes, writes split at every byte), where the
 * cursor ends (as Windows Terminal leaves it), pictures kept with the rows (overwrite, erase, scrollback, the alternate
 * screen, resize, the budget), hostile input, and what the view paints. Runs headless with the software renderer and
 * prints the decode speed. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b))) { g_failed++; printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

#define Q CUI_VT_FEATURE_QUERIES
#define SX CUI_VT_FEATURE_SIXEL
#define RGBA(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xFF000000u)

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static const char *row(const cui_vt *v, int y) { static char b[4][1100]; static int i; i = (i + 1) & 3; cui_vt_line_text(v, y, b[i], sizeof b[i]); return b[i]; }
static const char *reply(cui_vt *v) { static char b[8192]; size_t n = 0, k; while ((k = cui_vt_reply(v, b + n, sizeof b - 1 - n)) > 0) n += k; b[n] = 0; return b; }
static cui_vt *mk(int cols, int rows, int sb, unsigned f) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, f); return v; }
static void cursor(const cui_vt *v, int *x, int *y) { cui_vt_cursor(v, x, y); }
static uint32_t PX(const cui_vt *v, int r, int x, int y) { return cui__vt_pic_px(v, r, x, y); }

/* A sixel picture as the encoder ratatui-image uses writes it (icy_sixel): ESC P 9;P2;0 q "1;1;w;h, RGB colors in percent,
 * then per band of six rows every color used, run-length coded, each followed by $, and - after every band (the last one
 * too when `dash`). idx(x, y) gives a color index, or -1 for no pixel. */
typedef int (*six_idx)(int x, int y);
static size_t six_make(char *out, size_t cap, int w, int h, int p2, bool dash, const uint8_t (*pal)[3], int ncol, six_idx idx) {
    size_t o = (size_t)snprintf(out, cap, "\x1bP9;%d;0q\"1;1;%d;%d", p2, w, h);
    for (int i = 0; i < ncol; i++) o += (size_t)snprintf(out + o, cap - o, "#%d;2;%d;%d;%d", i, pal[i][0] * 100 / 255, pal[i][1] * 100 / 255, pal[i][2] * 100 / 255);
    int bands = (h + 5) / 6;
    for (int band = 0; band < bands; band++) {
        for (int c = 0; c < ncol; c++) {
            bool used = false;
            for (int x = 0; x < w && !used; x++) for (int r = 0; r < 6 && band * 6 + r < h; r++) if (idx(x, band * 6 + r) == c) { used = true; break; }
            if (!used) continue;
            o += (size_t)snprintf(out + o, cap - o, "#%d", c);
            for (int x = 0; x < w;) {
                int bits = 0;
                for (int r = 0; r < 6 && band * 6 + r < h; r++) if (idx(x, band * 6 + r) == c) bits |= 1 << r;
                int run = 1;
                while (x + run < w) {
                    int b2 = 0;
                    for (int r = 0; r < 6 && band * 6 + r < h; r++) if (idx(x + run, band * 6 + r) == c) b2 |= 1 << r;
                    if (b2 != bits) break;
                    run++;
                }
                if (run > 3) o += (size_t)snprintf(out + o, cap - o, "!%d%c", run, 63 + bits);
                else for (int k = 0; k < run; k++) out[o++] = (char)(63 + bits);
                x += run;
            }
            out[o++] = '$';
        }
        if (band < bands - 1 || dash) out[o++] = '-';
    }
    o += (size_t)snprintf(out + o, cap - o, "\x1b\\");
    return o;
}

static const uint8_t g_pal[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };
static int idx_bars(int x, int y) { (void)y; return (x / 10) % 3; }                 /* red, green, blue bars 10 pixels wide */
static int idx_red(int x, int y) { (void)x; (void)y; return 0; }
static int idx_check(int x, int y) { return ((x / 3 + y / 4) & 1) ? -1 : (x + y) % 4; }  /* holes and four colors */

static char g_buf[1 << 22];
static size_t pic(int w, int h, int p2, bool dash, six_idx f) { return six_make(g_buf, sizeof g_buf, w, h, p2, dash, g_pal, 4, f); }
static void put_pic(cui_vt *v, int w, int h, int p2, bool dash, six_idx f) { cui_vt_write(v, g_buf, pic(w, h, p2, dash, f)); }

/* ---- queries ---- */

static void test_queries_off(void) {
    cui_vt *v = cui_vt_create(30, 10, 0);
    size_t mem = cui_vt_memory(v);
    W(v, "\x1b[c"); CHECK_STR(reply(v), "\x1b[?1;0c");                        /* the old answers, unchanged */
    W(v, "\x1b[>c"); CHECK_STR(reply(v), "\x1b[>0;0;0c");
    W(v, "\x1b[=c\x1b[?6n\x1b[?25$p\x1b[4$p\x1b]11;?\x07\x1b]10;?\x1b\\\x1b[?1;1;0S\x1b[?80$p"); CHECK_STR(reply(v), "");
    W(v, "\x1b[14t\x1b[16t"); CHECK_STR(reply(v), "");                        /* no view has drawn: no cell size */
    W(v, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"); CHECK_STR(reply(v), ""); CHECK_STR(row(v, 0), "");
    put_pic(v, 20, 20, 1, true, idx_red);                                     /* a picture is swallowed: no text, no memory */
    CHECK_STR(row(v, 0), ""); CHECK_EQ(cui__vt_pic_count(v), 0); CHECK_EQ(cui_vt_memory(v), mem);
    int x, y; cursor(v, &x, &y); CHECK_EQ(x, 0); CHECK_EQ(y, 0);
    cui_vt_set_features(v, 0); cui_vt_set_colors(v, (cui_color){ 0 }, (cui_color){ 0 }, (cui_color){ 0 });
    CHECK_EQ(cui_vt_memory(v), mem);                                          /* setting nothing allocates nothing */
    cui_vt_destroy(v);
}

static void test_queries_on(void) {
    cui_vt *v = mk(30, 10, 0, Q);
    W(v, "\x1b[c"); CHECK_STR(reply(v), "\x1b[?62;22c");
    W(v, "\x1b[0c"); CHECK_STR(reply(v), "\x1b[?62;22c");
    W(v, "\x1b[>c"); CHECK_STR(reply(v), "\x1b[>1;10;0c");
    W(v, "\x1b[=c"); CHECK_STR(reply(v), "\x1bP!|00000000\x1b\\");
    W(v, "\x1b[3;5H\x1b[?6n\x1b[6n\x1b[5n"); CHECK_STR(reply(v), "\x1b[?3;5;1R\x1b[3;5R\x1b[0n");
    W(v, "\x1b[?25$p\x1b[?25l\x1b[?25$p\x1b[?7$p\x1b[?1$p\x1b[?2004$p\x1b[?2026$p\x1b[?1049$p");
    CHECK_STR(reply(v), "\x1b[?25;1$y\x1b[?25;2$y\x1b[?7;1$y\x1b[?1;2$y\x1b[?2004;2$y\x1b[?2026;2$y\x1b[?1049;2$y");
    W(v, "\x1b[?1049h\x1b[?1049$p\x1b[?1000h\x1b[?1006h\x1b[?1000$p\x1b[?1003$p\x1b[?1006$p\x1b[?1004$p\x1b[?1049l");
    CHECK_STR(reply(v), "\x1b[?1049;1$y\x1b[?1000;1$y\x1b[?1003;2$y\x1b[?1006;1$y\x1b[?1004;2$y");
    W(v, "\x1b[?9999$p\x1b[4$p\x1b[20$p\x1b[?80$p"); CHECK_STR(reply(v), "\x1b[?9999;0$y\x1b[4;0$y\x1b[20;0$y\x1b[?80;0$y");   /* not recognized */
    W(v, "\x1b]11;?\x07\x1b]10;?\x1b\\"); CHECK_STR(reply(v), "");             /* no colors set: no answer */
    cui_vt_set_colors(v, (cui_color){ 0xE0, 0xE1, 0xE2, 255 }, (cui_color){ 0x10, 0x20, 0x30, 255 }, (cui_color){ 0 });
    W(v, "\x1b]11;?\x07"); CHECK_STR(reply(v), "\x1b]11;rgb:1010/2020/3030\x07");          /* ended as the query was */
    W(v, "\x1b]10;?\x1b\\"); CHECK_STR(reply(v), "\x1b]10;rgb:e0e0/e1e1/e2e2\x1b\\");
    W(v, "\x1b]12;?\x07\x1b]4;1;?\x07\x1b]11;rgb:0/0/0\x07"); CHECK_STR(reply(v), "");        /* cursor unset; palette and setting: no answer */
    W(v, "\x1b[14t\x1b[16t\x1b[?2;1;0S"); CHECK_STR(reply(v), "");             /* pixel sizes belong to the sixel feature */
    W(v, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\x"); CHECK_STR(reply(v), ""); CHECK_STR(row(v, 2), "    x");   /* kitty: silence, nothing printed */
    cui_vt_set_features(v, Q | SX);
    W(v, "\x1b[c"); CHECK_STR(reply(v), "\x1b[?62;4;22c");
    W(v, "\x1b[16t\x1b[14t\x1b[18t"); CHECK_STR(reply(v), "\x1b[6;20;10t\x1b[4;200;300t\x1b[8;10;30t");   /* the virtual 10 x 20 cell */
    W(v, "\x1b[?80$p\x1b[?80h\x1b[?80$p\x1b[?80l\x1b[?80$p"); CHECK_STR(reply(v), "\x1b[?80;2$y\x1b[?80;1$y\x1b[?80;2$y");
    W(v, "\x1b[?1;1;0S\x1b[?2;1;0S\x1b[?2;4;0S\x1b[?1;3;16S\x1b[?3;1;0S\x1b[?1;9;0S");
    CHECK_STR(reply(v), "\x1b[?1;0;256S\x1b[?2;0;300;200S\x1b[?2;0;300;200S\x1b[?1;3;256S\x1b[?3;1;0S\x1b[?1;2;0S");
    /* what ratatui-image sends, in its order, and the exactly one answer each it needs */
    W(v, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c\x1b[16t\x1b[5n");
    CHECK_STR(reply(v), "\x1b[?62;4;22c\x1b[6;20;10t\x1b[0n");
    /* a burst past the screen's own 256 bytes of replies arrives whole and in order */
    char big[2048] = "", want[4096] = "";
    for (int i = 0; i < 60; i++) { strcat(big, "\x1b[?25$p\x1b[c"); strcat(want, "\x1b[?25;2$y\x1b[?62;4;22c"); }
    W(v, big);
    char got[4096]; size_t n = 0, k;
    while ((k = cui_vt_reply(v, got + n, 7)) > 0) n += k;                      /* taken a few bytes at a time */
    got[n] = 0;
    CHECK_STR(got, want);
    W(v, "\x1b[5n"); CHECK_STR(reply(v), "\x1b[0n");                           /* and the buffer works as before after it */
    cui_vt_set_features(v, SX);                                                /* sixel alone: the old device attributes */
    W(v, "\x1b[c\x1b[?6n"); CHECK_STR(reply(v), "\x1b[?1;0c");
    cui_vt_destroy(v);
}

/* ---- decoding ---- */

static void test_decode(void) {
    cui_vt *v = mk(40, 10, 0, SX);
    W(v, "\x1bP9;1;0q#1;2;100;0;0#1~~~~\x1b\\");                             /* four columns of six red pixels */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 3, 5), RGBA(255, 0, 0));
    CHECK_EQ(PX(v, 0, 4, 0), 0); CHECK_EQ(PX(v, 0, 0, 6), 0);
    CHECK_STR(row(v, 0), "");                                                  /* no text */
    W(v, "\x1b[2J\x1b[H\x1bP9;1q~\x1b\\");                                     /* no color chosen: register 15, the VT340's 80% gray */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(204, 204, 204));
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#2;1;120;50;100~#3;1;240;50;100~#4;1;0;50;100~#5;2;50;50;50~#6;1;0;100;0~\x1b\\");   /* HLS: 0 is blue */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 1, 0), RGBA(0, 255, 0)); CHECK_EQ(PX(v, 0, 2, 0), RGBA(0, 0, 255));
    CHECK_EQ(PX(v, 0, 3, 0), RGBA(128, 128, 128)); CHECK_EQ(PX(v, 0, 4, 0), RGBA(255, 255, 255));
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#300;2;0;100;0~#44~\x1b\\");                   /* register numbers wrap at 256 (300 is 44) */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(0, 255, 0)); CHECK_EQ(PX(v, 0, 1, 0), RGBA(0, 255, 0));
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#1;2;100;0;0!10~!3@\x1b\\");                   /* repeat: 10 full columns, then 3 of the top pixel */
    CHECK_EQ(PX(v, 0, 9, 5), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 12, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 12, 1), 0); CHECK_EQ(PX(v, 0, 13, 0), 0);
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#1;2;100;0;0!99999~\x1b\\");                   /* a huge repeat stops at the right edge */
    CHECK_EQ(PX(v, 0, 399, 0), RGBA(255, 0, 0));
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#1;2;100;0;0~~$#2;2;0;0;100??~~\x1b\\");       /* $ goes back over the band */
    CHECK_EQ(PX(v, 0, 1, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 2, 0), RGBA(0, 0, 255));
    /* transparency: P2 = 1 leaves what is there; P2 = 0 fills the raster area with color 0 (black) */
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#1;2;100;0;0!8~\x1b\\\x1b[H\x1bP9;1q#2;2;0;0;100!8@\x1b\\");
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(0, 0, 255)); CHECK_EQ(PX(v, 0, 0, 1), RGBA(255, 0, 0));
    W(v, "\x1b[H\x1bP9;0q\"1;1;4;6#2;2;0;0;100@\x1b\\");
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(0, 0, 255)); CHECK_EQ(PX(v, 0, 0, 1), RGBA(0, 0, 0)); CHECK_EQ(PX(v, 0, 3, 5), RGBA(0, 0, 0));
    CHECK_EQ(PX(v, 0, 4, 1), RGBA(255, 0, 0));                                 /* outside the raster area: as it was */
    /* the pixel's height: P1 (0, 1, 5, 6: 2; 2: 5; 3, 4: 3; 7..9: 1), and the raster attributes after it */
    static const int p1h[10] = { 2, 2, 5, 3, 3, 2, 2, 1, 1, 1 };
    for (int p1 = 0; p1 <= 9; p1++) {
        char s[64]; snprintf(s, sizeof s, "\x1b[2J\x1b[H\x1bP%d;1q#1;2;100;0;0@\x1b\\", p1);
        W(v, s);
        CHECK_EQ(PX(v, 0, 0, p1h[p1] - 1), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 0, p1h[p1]), 0);
    }
    W(v, "\x1b[2J\x1b[H\x1bP0;1q\"1;1#1;2;100;0;0@\x1b\\"); CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 0, 1), 0);   /* "1;1: square */
    W(v, "\x1b[2J\x1b[H\x1bP9;1q\"3;1#1;2;100;0;0@\x1b\\"); CHECK_EQ(PX(v, 0, 0, 2), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 0, 3), 0);
    W(v, "\x1b[2J\x1b[H\x1bP9;1q\"5;2#1;2;100;0;0@\x1b\\"); CHECK_EQ(PX(v, 0, 0, 2), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 0, 3), 0);   /* ceil(5 / 2) */
    /* line breaks, spaces and other characters inside the data are ignored */
    W(v, "\x1b[2J\x1b[H\x1bP9;1q#1;2;100;0;0\r\n~ ~\t~\x1b\\"); CHECK_EQ(PX(v, 0, 2, 0), RGBA(255, 0, 0));
    cui_vt_destroy(v);
}

/* the same picture written in two pieces cut at every byte, and in single bytes, gives the same pixels and cursor */
static uint32_t pic_hash(const cui_vt *v, int rows, int w) {
    uint32_t h = 2166136261u;
    for (int r = 0; r < rows; r++) for (int y = 0; y < 20; y++) for (int x = 0; x < w; x++) h = (h ^ PX(v, r, x, y)) * 16777619u;
    return h;
}

static void test_split(void) {
    char s[4096];
    size_t n = six_make(s, sizeof s, 23, 17, 1, true, g_pal, 4, idx_check);
    char *t = s + n;                                                           /* raster attributes, HLS, a repeat and a P2=0 fill too */
    n += (size_t)snprintf(t, sizeof s - n, "\x1bP0;0;0q\"2;1;12;8#7;1;300;40;90!12~-#7!5N\x1b\\after");
    cui_vt *ref = mk(20, 6, 0, SX);
    cui_vt_write(ref, s, n);
    uint32_t want = pic_hash(ref, 6, 200);
    int wx, wy; cursor(ref, &wx, &wy);
    CHECK(want != pic_hash(mk(1, 1, 0, 0), 1, 1));
    int bad = 0;
    for (size_t k = 0; k <= n; k++) {
        cui_vt *v = mk(20, 6, 0, SX);
        cui_vt_write(v, s, k); cui_vt_write(v, s + k, n - k);
        int x, y; cursor(v, &x, &y);
        if (pic_hash(v, 6, 200) != want || x != wx || y != wy || strcmp(row(v, wy), row(ref, wy))) bad++;
        cui_vt_destroy(v);
    }
    CHECK_EQ(bad, 0);
    cui_vt *v = mk(20, 6, 0, SX);
    for (size_t k = 0; k < n; k++) cui_vt_write(v, s + k, 1);
    CHECK_EQ(pic_hash(v, 6, 200), want);
    CHECK_STR(row(v, wy), row(ref, wy));
    cui_vt_destroy(v);
    cui_vt_destroy(ref);
}

/* ---- where the cursor ends ---- */

static void test_cursor(void) {
    int x, y;
    cui_vt *v = mk(30, 10, 100, SX);
    struct { int h; bool dash; int dy; } c[] = {
        { 40, true, 2 }, { 40, false, 1 }, { 20, true, 1 }, { 20, false, 0 }, { 6, true, 0 }, { 6, false, 0 }, { 60, true, 3 }, { 61, true, 3 }, { 59, true, 3 } };
    for (size_t i = 0; i < sizeof c / sizeof *c; i++) {                       /* origin row + (bands * 6) / 20, back in the column it started */
        W(v, "\x1b[2J\x1b[2;3H");
        put_pic(v, 20, c[i].h, 1, c[i].dash, idx_red);
        cursor(v, &x, &y);
        CHECK_EQ(x, 2); CHECK_EQ(y, 1 + c[i].dy);
        CHECK_EQ(PX(v, 1, 20, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 1, 19, 0), 0);   /* drawn from the cursor's cell */
    }
    /* at the bottom: a band that does not fit scrolls first; ending exactly at the bottom scrolls once more for the cursor */
    W(v, "\x1b[2J\x1b[9;3H");
    int sb0 = cui_vt_scrollback_count(v);
    put_pic(v, 20, 60, 1, true, idx_red);
    cursor(v, &x, &y);
    CHECK_EQ(y, 9); CHECK_EQ(x, 2);
    CHECK_EQ(cui_vt_scrollback_count(v) - sb0, 2);
    CHECK_EQ(PX(v, 6, 20, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 8, 20, 19), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 9, 20, 0), 0); CHECK_EQ(PX(v, 5, 20, 19), 0);
    /* taller than the screen: the top goes into the scrollback with its rows */
    W(v, "\x1b[2J\x1b[H");
    sb0 = cui_vt_scrollback_count(v);
    put_pic(v, 20, 300, 1, true, idx_red);                                     /* 15 rows */
    cursor(v, &x, &y);
    CHECK_EQ(y, 9);
    CHECK_EQ(cui_vt_scrollback_count(v) - sb0, 6);
    CHECK_EQ(PX(v, -1, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, -6, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 8, 0, 19), RGBA(255, 0, 0));
    /* sixel display mode (DECSDM, ?80h): from the top left of the page, no scrolling, the cursor stays */
    W(v, "\x1b[2J\x1b[6;6H\x1b[?80h");
    sb0 = cui_vt_scrollback_count(v);
    put_pic(v, 20, 400, 1, true, idx_red);
    cursor(v, &x, &y);
    CHECK_EQ(x, 5); CHECK_EQ(y, 5);
    CHECK_EQ(cui_vt_scrollback_count(v), sb0);
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 9, 0, 19), RGBA(255, 0, 0));
    W(v, "\x1b[?80l");
    /* below the scroll region: nothing is drawn and the cursor stays */
    W(v, "\x1b[2J\x1b[1;5r\x1b[8;4H");
    put_pic(v, 20, 40, 1, true, idx_red);
    cursor(v, &x, &y);
    CHECK_EQ(x, 3); CHECK_EQ(y, 7);
    CHECK_EQ(PX(v, 7, 30, 0), 0);
    /* inside a scroll region: scrolling stays in the region, the cursor ends below the picture */
    W(v, "\x1b[3;6r\x1b[5;1H");
    int before = cui_vt_scrollback_count(v);
    put_pic(v, 10, 40, 1, true, idx_red);
    cursor(v, &x, &y);
    CHECK_EQ(y, 5); CHECK_EQ(x, 0);
    CHECK_EQ(cui_vt_scrollback_count(v), before);                              /* a region away from the top keeps no history */
    CHECK_EQ(PX(v, 3, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 4, 0, 19), RGBA(255, 0, 0));
    W(v, "\x1b[r");
    /* at the last column: the picture is one cell wide */
    W(v, "\x1b[2J\x1b[1;30H");
    put_pic(v, 40, 20, 1, true, idx_red);
    CHECK_EQ(PX(v, 0, 290, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 299, 0), RGBA(255, 0, 0));
    /* text after a picture goes where the cursor is */
    W(v, "\x1b[2J\x1b[H");
    put_pic(v, 30, 40, 1, true, idx_red);
    W(v, "next");
    CHECK_STR(row(v, 2), "next");
    cui_vt_destroy(v);
}

/* ---- storage ---- */

static void test_storage(void) {
    cui_vt *v = mk(20, 8, 100, SX);
    size_t base = cui_vt_memory(v);
    W(v, "\x1b[H"); put_pic(v, 40, 20, 1, false, idx_red);                    /* 4 cells x 1 row */
    CHECK_EQ(cui__vt_pic_count(v), 1);
    CHECK(cui_vt_memory(v) >= base + 4 * 800);
    W(v, "\x1b[1;2HX");                                                        /* text over a cell takes that cell */
    CHECK_EQ(PX(v, 0, 10, 0), 0); CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 25, 10), RGBA(255, 0, 0));
    W(v, "\x1b[2;1Hbelow and around\x1b[1;10Hright");                          /* repainting around it leaves it */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 35, 19), RGBA(255, 0, 0));
    W(v, "\x1b[1;4H\x1b[K");                                                   /* erase to the end of the line */
    CHECK_EQ(PX(v, 0, 25, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 30, 0), 0);
    W(v, "\x1b[1;1H\x1b[1X");                                                  /* erase characters */
    CHECK_EQ(PX(v, 0, 0, 0), 0); CHECK_EQ(PX(v, 0, 20, 0), RGBA(255, 0, 0));
    W(v, "\x1b[1;3H\x1b[1X");                                                  /* the last cell: the slice goes */
    CHECK_EQ(cui__vt_pic_count(v), 0);
    CHECK(cui_vt_memory(v) - base < 8 * 1024);                                 /* back to where it was, but the row table and the decoder */
    /* insert and delete characters move the picture with the text */
    W(v, "\x1b[2J\x1b[H"); put_pic(v, 40, 20, 1, false, idx_bars);            /* red, green, blue, red */
    W(v, "\x1b[1;2H\x1b[1@");                                                  /* one blank at column 1 */
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 10, 0), 0); CHECK_EQ(PX(v, 0, 25, 0), RGBA(0, 0xFF, 0)); CHECK_EQ(PX(v, 0, 35, 0), RGBA(0, 0, 255));
    W(v, "\x1b[1;2H\x1b[1P");                                                  /* and out again */
    CHECK_EQ(PX(v, 0, 10, 0), RGBA(0, 255, 0)); CHECK_EQ(PX(v, 0, 20, 0), RGBA(0, 0, 255)); CHECK_EQ(PX(v, 0, 30, 0), 0);
    /* erase in display, then scrolling into the scrollback, and clearing it */
    W(v, "\x1b[2J"); CHECK_EQ(cui__vt_pic_count(v), 0);
    W(v, "\x1b[H"); put_pic(v, 20, 40, 1, false, idx_red);                    /* rows 0 and 1 */
    W(v, "\x1b[8;1H\n\n\n");
    CHECK_EQ(PX(v, -3, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, -2, 0, 19), RGBA(255, 0, 0)); CHECK_EQ(PX(v, -1, 0, 0), 0);
    CHECK_EQ(cui__vt_pic_count(v), 2);
    W(v, "\x1b[3J"); CHECK_EQ(cui__vt_pic_count(v), 0); CHECK_EQ(PX(v, -3, 0, 0), 0);
    /* the scrollback drops its oldest lines: their pictures go with them */
    cui_vt *s = mk(10, 3, 5, SX);
    W(s, "\x1b[H"); put_pic(s, 10, 20, 1, false, idx_red);
    for (int i = 0; i < 4; i++) W(s, "\x1b[3;1H\n");
    CHECK_EQ(cui__vt_pic_count(s), 1);
    for (int i = 0; i < 10; i++) W(s, "\x1b[3;1H\n");
    CHECK_EQ(cui__vt_pic_count(s), 0);
    cui_vt_destroy(s);
    s = mk(10, 3, 0, SX);                                                      /* no scrollback: a picture leaving the top is gone */
    W(s, "\x1b[H"); put_pic(s, 10, 20, 1, false, idx_red); W(s, "\x1b[3;1H\n\n\n");
    CHECK_EQ(cui__vt_pic_count(s), 0);
    cui_vt_destroy(s);
    /* the alternate screen keeps its own pictures, and they go when it is left */
    W(v, "\x1b[2J\x1b[H"); put_pic(v, 20, 20, 1, false, idx_red);
    W(v, "\x1b[?1049h");
    CHECK_EQ(PX(v, 0, 0, 0), 0);
    W(v, "\x1b[3;1H"); put_pic(v, 20, 20, 1, false, idx_bars);
    CHECK_EQ(PX(v, 2, 15, 0), RGBA(0, 255, 0)); CHECK_EQ(cui__vt_pic_count(v), 2);
    W(v, "\x1b[?1049l");
    CHECK_EQ(cui__vt_pic_count(v), 1); CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 2, 15, 0), 0);
    W(v, "\x1b[?1049h"); CHECK_EQ(PX(v, 2, 15, 0), 0); W(v, "\x1b[?1049l");
    /* resize: rows that stay keep their pictures, cells past the new edge go; nothing leaks or crashes */
    W(v, "\x1b[2J\x1b[H"); put_pic(v, 100, 60, 1, false, idx_bars);           /* 10 cells x 3 rows */
    cui_vt_resize(v, 6, 8);
    CHECK_EQ(PX(v, 0, 55, 0), RGBA(0, 0, 255)); CHECK_EQ(PX(v, 2, 0, 19), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 65, 0), 0);
    cui_vt_resize(v, 40, 2);                                                   /* the cursor's row stays: rows go into the scrollback */
    cui_vt_resize(v, 40, 20);
    cui_vt_resize(v, 2, 1);
    cui_vt_resize(v, 20, 8);
    W(v, "\x1b[2J\x1b[3J");
    CHECK_EQ(cui__vt_pic_count(v), 0);
    /* a reset clears pictures and the display mode */
    W(v, "\x1b[?80h\x1b[H"); put_pic(v, 20, 20, 1, false, idx_red); W(v, "\x1b" "c");
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_set_features(v, Q | SX);
    W(v, "\x1b[?80$p"); CHECK_STR(reply(v), "\x1b[?80;2$y");
    /* switching the feature off frees every picture */
    W(v, "\x1b[H"); put_pic(v, 20, 20, 1, false, idx_red);
    cui_vt_set_features(v, Q);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_set_features(v, 0);
    cui_vt *fresh = cui_vt_create(20, 8, 100);
    W(fresh, "\x1b[?1049h\x1b[?1049l");                                        /* (both screens allocated, as in v) */
    CHECK_EQ(cui_vt_memory(v), cui_vt_memory(fresh));                          /* all of it given back */
    cui_vt_destroy(fresh);
    cui_vt_destroy(v);
}

static void test_budget(void) {
    cui_vt *v = mk(60, 30, 200, SX);
    size_t base = cui_vt_memory(v);
    size_t cap = 300 * 1000;
    cui_vt_set_picture_budget(v, cap);
    size_t peak = 0;
    for (int i = 0; i < 40; i++) {                                             /* pictures at many places, scrolling too */
        char go[32]; snprintf(go, sizeof go, "\x1b[%d;%dH", 1 + (i * 7) % 25, 1 + (i * 13) % 40);
        W(v, go);
        put_pic(v, 200, 100, 1, true, idx_check);
        size_t m = cui_vt_memory(v) - base;
        if (m > peak) peak = m;
    }
    CHECK(peak <= cap + 64 * 1024);                                            /* pixels within the cap; the rest is the decoder's band */
    printf("  budget %zu: peak picture memory %zu bytes\n", cap, peak);
    /* the oldest go first: A then B where only about one fits */
    W(v, "\x1b[2J\x1b[3J\x1b[H");
    cui_vt_set_picture_budget(v, 30000);                                       /* a picture of 5 x 5 cells is about 20 KB */
    put_pic(v, 50, 100, 1, false, idx_red);                                    /* A at the top */
    W(v, "\x1b[20;20H"); put_pic(v, 50, 100, 1, false, idx_bars);             /* B */
    CHECK_EQ(PX(v, 19, 195, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 23, 205, 0), RGBA(0, 255, 0));
    CHECK_EQ(PX(v, 0, 0, 0), 0); CHECK_EQ(PX(v, 2, 0, 0), 0);                  /* A's top rows went first */
    CHECK_EQ(PX(v, 4, 0, 0), RGBA(255, 0, 0));
    CHECK(cui_vt_memory(v) - base <= 30000 + 8 * 30 + 64 * 1024);
    /* a picture larger than the whole budget is cut off at the bottom */
    W(v, "\x1b[2J\x1b[H");
    cui_vt_set_picture_budget(v, 3 * (10 * 800 + 10 + 64));
    put_pic(v, 100, 200, 1, false, idx_red);
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 2, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 3, 0, 0), 0);
    cui_vt_set_picture_budget(v, 0);                                           /* lowering it frees at once */
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);
}

/* ---- hostile input ---- */

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1103515245u + 12345u; return g_rng >> 8; }

static void test_hostile(void) {
    cui_vt *v = mk(80, 24, 1000, Q | SX);
    size_t base = cui_vt_memory(v);
    W(v, "\x1bP9;1q#1;2;100;0;0!2000000000~!0~!~\x1b\\");                      /* repeats far past any width */
    W(v, "\x1bPq\"1;1;2000000000;2000000000~\x1b\\");                          /* absurd raster sizes */
    W(v, "\x1bP0;0q\"99999;1;99999;99999#0~-~-~\x1b\\");                       /* an aspect ratio taller than the screen */
    W(v, "\x1bPq\"1;0;0;0#9999999999999;9;9;9;9;9;9;9;9;9;9;9;9;9~\x1b\\");   /* too many and too big parameters */
    W(v, "\x1bP;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;q~\x1b\\");
    W(v, "\x1bPq#1;1;9999;9999;9999~#2;3;1;1;1~\x1b\\");                       /* HLS and unknown models out of range */
    W(v, "\x1bP1$qm\x1b\\\x1bP+q544e\x1b\\");                                  /* other DCS: swallowed, no answer */
    CHECK_STR(reply(v), "");
    W(v, "\x1bPq~~~~ unterminated");                                           /* everything until ESC is picture data */
    CHECK_STR(row(v, 0), "");
    W(v, "\x1b\\visible");
    int x, y; cursor(v, &x, &y);
    CHECK_STR(row(v, y), "visible");
    W(v, "\x1bPq~~\x18text\x1bPq~~\x1a!");                                     /* CAN and SUB end a picture too */
    cursor(v, &x, &y);
    CHECK(strstr(row(v, y), "text!") != NULL);
    /* 10 MB of junk, with and without DCS starts in it: no crash, memory within the budget */
    char *junk = (char *)malloc(1 << 20);
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < (1 << 20); i++) {
            uint32_t r = rnd();
            junk[i] = (char)(r % 7 == 0 ? "\x1bPq#-$!\"~?;0123456789\x1b\\"[r % 24] : (char)(r >> 3));
        }
        cui_vt_write(v, junk, 1 << 20);
        CHECK(cui_vt_memory(v) <= base + ((size_t)8 << 20) + 512 * 1024);
    }
    free(junk);
    W(v, "\x1b\\\x1b[c"); reply(v);
    W(v, "\x1b" "c\x1b[2J\x1b[3J\x1b[?1049l\x1b[c");
    CHECK_STR(reply(v), "\x1b[?62;4;22c");                                     /* still answering */
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
static int g_tex_live;                       /* textures of rows of picture (RGBA, 20 high, a multiple of 10 wide) alive */
static cui_renderer_iface g_base;
static int g_pic_tex[4096];
static cui_texture cnt_create(void *u, int w, int h, cui_texture_format f, const void *px) {
    cui_texture t = g_base.texture_create(u, w, h, f, px);
    if (h == 20 && w % 10 == 0 && f == CUI_TEXTURE_RGBA8 && t < 4096) { g_pic_tex[t] = 1; g_tex_live++; }
    return t;
}
static void cnt_destroy(void *u, cui_texture t) {
    if (t < 4096 && g_pic_tex[t]) { g_pic_tex[t] = 0; g_tex_live--; }
    g_base.texture_destroy(u, t);
}

static void build(cui_ctx *ui, cui_vt *vt) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { cui_vt_view(ui, "t", vt, .focus = true); }
    cui_end_frame(ui);
}
static uint32_t g_px[640 * 400];
static void render(cui_ctx *ui) { cui_sw_render(g_sw, cui_get_draw_data(ui), g_px, 640, 400, 640 * 4, CUI_SW_RGBA); }
static uint32_t hash_rect(int x0, int y0, int x1, int y1) {
    uint32_t h = 2166136261u;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) h = (h ^ g_px[y * 640 + x]) * 16777619u;
    return h;
}
static uint32_t at(int x, int y) { return g_px[y * 640 + x]; }
static int ch(uint32_t c, int k) { return (int)((c >> (8 * k)) & 255); }
static cui_ctx *ctx(void) {
    g_base = cui_sw_iface(g_sw);
    cui_renderer_iface r = g_base;
    r.texture_create = cnt_create; r.texture_destroy = cnt_destroy;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = r, .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

static void test_paint(void) {
    /* with the features off the frame is the one it always was, and so is a screen with them on and no picture */
    const char *text = "\x1b[2J\x1b[Hplain \x1b[31mred\x1b[0m text\r\n\x1b[4:3mcurly\x1b[0m";
    uint32_t h[3];
    for (int k = 0; k < 3; k++) {
        cui_ctx *ui = ctx();
        cui_vt *v = cui_vt_create(80, 24, 100);
        if (k == 1) cui_vt_set_features(v, Q | SX);
        if (k == 2) put_pic(v, 40, 40, 1, true, idx_red);                     /* features off: swallowed */
        W(v, text);
        for (int i = 0; i < 3; i++) build(ui, v);
        render(ui);
        h[k] = hash_rect(0, 0, 640, 400);
        cui_vt_destroy(v);
        cui_destroy(ui);
    }
    CHECK_EQ(h[1], h[0]); CHECK_EQ(h[2], h[0]);
    /* a picture reaches the screen: red, green, blue bars over 4 rows of cells, scaled to the cell (7.8 x 19 here) */
    cui_ctx *ui = ctx();
    cui_vt *v = mk(80, 24, 100, Q | SX);
    W(v, "\x1b[H");
    put_pic(v, 300, 80, 1, true, idx_bars);
    for (int i = 0; i < 3; i++) build(ui, v);
    render(ui);
    int cx = 10, cy = 6;                                                       /* the view's padding */
    float cw = 640.0f / 80.0f, lh = 19;
    (void)cw;
    uint32_t r0 = at(cx + 3, cy + 10), g0 = at(cx + 11, cy + 10), b0 = at(cx + 19, cy + 30);
    CHECK(ch(r0, 0) > 200 && ch(r0, 1) < 60 && ch(r0, 2) < 60);
    CHECK(ch(g0, 1) > 200 && ch(g0, 0) < 60);
    CHECK(ch(b0, 2) > 200 && ch(b0, 0) < 60);
    CHECK_EQ(g_tex_live, 4);                                                   /* one texture per row of picture on screen */
    uint32_t bgpx = at(cx + 3, cy + (int)(lh * 5) + 5);                        /* below it: the screen's background */
    CHECK(ch(bgpx, 0) < 80 && ch(bgpx, 1) < 80);
    /* text over part of it: those cells show text and background, the rest stays */
    W(v, "\x1b[1;1HAB");
    build(ui, v); render(ui);
    uint32_t over = at(cx + 3, cy + 2);
    CHECK(!(ch(over, 0) > 200 && ch(over, 1) < 60));
    CHECK(ch(at(cx + 19, cy + 30), 2) > 200);
    /* text that was there before a picture is covered by it, as in Windows Terminal (the picture is drawn over the row) */
    W(v, "\x1b[10;1HWWWWWWWWWW\x1b[10;1H");
    put_pic(v, 100, 20, 1, false, idx_red);
    build(ui, v); render(ui);
    bool all_red = true;
    for (int x = cx + 10; x < cx + 70; x++) for                               /* (cell 0 holds the cursor, drawn over the picture) */ (int y = cy + (int)(lh * 9) + 2; y < cy + (int)(lh * 10) - 2; y++) {
        uint32_t c = at(x, y);
        if (!(ch(c, 0) > 200 && ch(c, 1) < 60 && ch(c, 2) < 60)) all_red = false;
    }
    CHECK(all_red);
    CHECK_STR(row(v, 9), "WWWWWWWWWW");                                        /* (the text is still in the cells) */
    W(v, "\x1b[10;1H\x1b[2K");
    /* selection shows over the picture */
    uint32_t plain = hash_rect(cx, cy + 20, cx + 60, cy + 38);
    cui_input_mouse_move(ui, (float)cx + 2, (float)cy + 25); build(ui, v);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, v);
    cui_input_mouse_move(ui, (float)cx + 50, (float)cy + 25); build(ui, v);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, v);
    render(ui);
    CHECK(hash_rect(cx, cy + 20, cx + 60, cy + 38) != plain);
    /* synchronized output: while an update is open the old picture shows; its textures are kept */
    W(v, "\x1b[?2026h\x1b[2J");
    build(ui, v); render(ui);
    CHECK(ch(at(cx + 19, cy + 30), 2) > 200);
    W(v, "\x1b[?2026l");
    build(ui, v); render(ui);
    CHECK(!(ch(at(cx + 19, cy + 30), 2) > 200));
    CHECK_EQ(g_tex_live, 0);                                                   /* erased: its textures are freed */
    /* scrolled away and back: textures follow what is on screen */
    W(v, "\x1b[H"); put_pic(v, 100, 40, 1, true, idx_red);
    build(ui, v);
    CHECK_EQ(g_tex_live, 2);
    for (int i = 0; i < 30; i++) W(v, "\x1b[24;1H\n");
    build(ui, v);
    CHECK_EQ(g_tex_live, 0);
    cui_input_mouse_move(ui, 300, 200);
    for (int i = 0; i < 10; i++) { cui_input_scroll(ui, 0, 10); build(ui, v); }   /* the wheel looks back */
    render(ui);
    CHECK_EQ(g_tex_live, 2);
    for (int i = 0; i < 10; i++) { cui_input_scroll(ui, 0, -10); build(ui, v); }
    CHECK_EQ(g_tex_live, 0);
    /* lifetimes: the screen destroyed first, then the view stops being built: its textures go with its state */
    W(v, "\x1b[H"); put_pic(v, 100, 40, 1, true, idx_red);
    build(ui, v);
    CHECK_EQ(g_tex_live, 2);
    cui_vt_destroy(v);
    for (int i = 0; i < 400; i++) { cui_begin_frame(ui, 1.0f / 60.0f); cui_end_frame(ui); }
    CHECK_EQ(g_tex_live, 0);
    /* a new screen in the same view (maybe at the same address) never shows the old one's textures */
    v = mk(80, 24, 100, SX);
    W(v, "\x1b[H"); put_pic(v, 100, 40, 1, true, idx_red);
    build(ui, v);
    cui_vt_destroy(v);
    v = mk(80, 24, 100, SX);
    W(v, "\x1b[H"); put_pic(v, 100, 40, 1, true, idx_bars);
    build(ui, v); render(ui);
    CHECK(ch(at(cx + 3, cy + 10), 0) > 200 && ch(at(cx + 11, cy + 10), 1) > 200);
    CHECK_EQ(g_tex_live, 2);
    /* and the context destroyed first: every texture is freed with it, the screen frees its pixels alone */
    cui_destroy(ui);
    CHECK_EQ(g_tex_live, 0);
    cui_vt_destroy(v);
}

/* ---- the virtual cell keeps the view's cell shape (CUI_VT_FEATURE_SIXEL_ASPECT) ---- */

static void build_h(cui_ctx *ui, cui_vt *vt, float size, float lh, float h) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { cui_vt_view(ui, "t", vt, .size = size, .line_height = lh, .height = h > 0 ? CUI_PX(h) : CUI_FILL); }
    cui_end_frame(ui);
}
static void build_sized(cui_ctx *ui, cui_vt *vt, float size, float lh) { build_h(ui, vt, size, lh, 0); }

/* columns of row `r` holding picture (its first pixel column), counted with the row's own cell width `cw` */
static int pic_cells(const cui_vt *v, int r, int cw) { int n = 0; for (int c = 0; c < 200; c++) if (PX(v, r, c * cw, 10)) n++; return n; }

static void test_aspect(void) {
    cui_vt *v = mk(80, 24, 100, Q | SX | CUI_VT_FEATURE_SIXEL_ASPECT);
    W(v, "\x1b[16t\x1b[14t"); CHECK_STR(reply(v), "\x1b[6;20;10t\x1b[4;480;800t");   /* no view has drawn: 10 x 20 */
    cui_vt_destroy(v);
    v = mk(80, 24, 100, Q | SX);                                               /* without the flag: always 10 x 20 */
    static const struct { float size, lh, scale; } shapes[] = { { 13, 0, 1 }, { 13, 0, 2 }, { 16, 1.2f, 1 }, { 14, 1.4f, 1.5f }, { 20, 1.0f, 1 } };
    for (size_t i = 0; i < sizeof shapes / sizeof *shapes; i++) {
        cui_ctx *ui = ctx();
        cui_set_viewport(ui, 640 * shapes[i].scale, 400 * shapes[i].scale, shapes[i].scale);
        cui_vt *ref = cui_vt_create(80, 24, 0), *a = mk(80, 24, 100, Q | SX | CUI_VT_FEATURE_SIXEL_ASPECT);
        for (int k = 0; k < 3; k++) { build_sized(ui, ref, shapes[i].size, shapes[i].lh); build_sized(ui, a, shapes[i].size, shapes[i].lh); build_sized(ui, v, shapes[i].size, shapes[i].lh); }
        int ch = 0, cw = 0, cols, rows;
        W(ref, "\x1b[16t"); CHECK_EQ(sscanf(reply(ref), "\x1b[6;%d;%dt", &ch, &cw), 2);   /* the real cell */
        double cwf = (double)cui_measure_text(ui, CUI_FONT_MONO, shapes[i].size, "0", 1).width * shapes[i].scale;   /* the unrounded cell width */
        int want = 0;
        W(a, "\x1b[16t"); CHECK_EQ(sscanf(reply(a), "\x1b[6;20;%dt", &want), 1);
        double ideal = 20.0 * cwf / ch;
        CHECK(want >= 6 && want <= 14 && want >= (int)ideal && want <= (int)ideal + 1);   /* round(20 * width / height) */
        cui_vt_size(a, &cols, &rows);
        char exp[96]; snprintf(exp, sizeof exp, "\x1b[4;%d;%dt", rows * 20, cols * want);
        W(a, "\x1b[14t"); CHECK_STR(reply(a), exp);
        W(v, "\x1b[16t"); CHECK_STR(reply(v), "\x1b[6;20;10t");
        double off = ((cwf / want) / (ch / 20.0) - 1.0) * 100.0;               /* how much wider than tall a square shows */
        CHECK(off > -6 && off < 6);
        printf("  cell %.2fx%d device px (size %.0f, line height %.1f, scale %.1f): W = %d (ideal %.2f), a square shows %+.1f%% wide (at 10: %+.1f%%)\n",
               cwf, ch, (double)shapes[i].size, shapes[i].lh > 0 ? (double)shapes[i].lh : 1.4, (double)shapes[i].scale, want, ideal, off,
               ((cwf / 10.0) / (ch / 20.0) - 1.0) * 100.0);
        (void)cw;
        /* a picture 80 pixels wide covers ceil(80 / W) cells; the cursor is as without the flag (rows are still 20 high) */
        W(a, "\x1b[2J\x1b[2;3H"); put_pic(a, 80, 40, 1, true, idx_red);
        CHECK_EQ(pic_cells(a, 1, want), (80 + want - 1) / want);
        int x, y; cursor(a, &x, &y); CHECK_EQ(x, 2); CHECK_EQ(y, 3);
        cui_vt_destroy(ref); cui_vt_destroy(a);
        cui_destroy(ui);
    }
    cui_vt_destroy(v);
    /* the cell changes between two pictures: an untouched row keeps its W; a new picture in a row scales that row's old
     * cells to the new W in place, so they stay in their columns */
    cui_ctx *ui = ctx();
    v = mk(80, 24, 100, Q | SX | CUI_VT_FEATURE_SIXEL_ASPECT);
    for (int k = 0; k < 3; k++) build_sized(ui, v, 13, 1.0f);                 /* a wide cell */
    W(v, "\x1b[16t"); int w1 = 0; sscanf(reply(v), "\x1b[6;20;%dt", &w1);
    W(v, "\x1b[H"); put_pic(v, 60, 20, 1, false, idx_red);                    /* row 0 */
    W(v, "\x1b[3;1H"); put_pic(v, 60, 20, 1, false, idx_bars);                /* row 2 */
    int cells0 = pic_cells(v, 0, w1), cells2 = pic_cells(v, 2, w1);
    for (int k = 0; k < 3; k++) build_sized(ui, v, 13, 2.0f);                 /* a tall cell */
    W(v, "\x1b[16t"); int w2 = 0; sscanf(reply(v), "\x1b[6;20;%dt", &w2);
    CHECK(w2 < w1);
    CHECK_EQ(pic_cells(v, 2, w1), cells2);
    CHECK_EQ(PX(v, 2, 0, 5), RGBA(255, 0, 0));
    W(v, "\x1b[1;40H"); put_pic(v, 20, 20, 1, false, idx_bars);               /* a W2 picture into row 0 */
    CHECK_EQ(pic_cells(v, 0, w2), cells0 + (20 + w2 - 1) / w2);
    CHECK_EQ(PX(v, 0, 0, 0), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, (cells0 - 1) * w2 + w2 / 2, 0), RGBA(255, 0, 0));
    CHECK_EQ(PX(v, 0, cells0 * w2 + w2 / 2, 0), 0);
    cui_vt_destroy(v);
    /* a picture being read when the cell changes (here without a resize: a one-row view, 8x13 then 8x16 cells) finishes
     * with the W it started with. (A resize stops a picture being read, as before.) A two-row view keeps its grid. */
    v = mk(80, 2, 0, Q | SX | CUI_VT_FEATURE_SIXEL_ASPECT);
    for (int k = 0; k < 3; k++) build_h(ui, v, 13, 1.0f, 46);
    W(v, "\x1b[16t"); int wa = 0; sscanf(reply(v), "\x1b[6;20;%dt", &wa);
    size_t n = pic(60, 20, 1, false, idx_red);
    W(v, "\x1b[H");
    cui_vt_write(v, g_buf, n / 2);
    int c1, r1; cui_vt_size(v, &c1, &r1);
    for (int k = 0; k < 3; k++) build_h(ui, v, 13, 1.2f, 46);
    int c2, r2; cui_vt_size(v, &c2, &r2);
    cui_vt_write(v, g_buf + n / 2, n - n / 2);
    W(v, "\x1b[16t"); int wb = 0; sscanf(reply(v), "\x1b[6;20;%dt", &wb);   /* (asked after the picture: inside it, ESC would end it) */
    CHECK(wa != wb); CHECK_EQ(c1, c2); CHECK_EQ(r1, r2);
    CHECK_EQ(pic_cells(v, 0, wa), (60 + wa - 1) / wa);
    CHECK_EQ(PX(v, 0, 59, 19), RGBA(255, 0, 0)); CHECK_EQ(PX(v, 0, 60, 0), 0);
    cui_vt_destroy(v);
    cui_destroy(ui);
}

/* ---- speed ---- */

static int idx_noise(int x, int y) { return (int)((((uint32_t)x * 2654435761u) ^ ((uint32_t)y * 40503u)) >> 13) % 4; }
static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void test_speed(void) {
    size_t n = pic(800, 480, 1, true, idx_noise);                              /* noisy: little run-length coding */
    char *s = (char *)malloc(n);
    memcpy(s, g_buf, n);
    cui_vt *v = mk(200, 60, 0, SX);
    double best = 1e9;
    for (int k = 0; k < 12; k++) {
        W(v, "\x1b[2J\x1b[H");
        double t0 = now_s();
        for (int r = 0; r < 8; r++) { W(v, "\x1b[H"); cui_vt_write(v, s, n); }
        double dt = now_s() - t0;
        if (dt < best) best = dt;
    }
    printf("  sixel decode: %.1f MB/s (best of 12, 8 x %zu KB of noisy 800x480)\n", (double)n * 8 / best / 1e6, n >> 10);
    size_t n2 = pic(400, 400, 1, true, idx_bars);
    memcpy(s, g_buf, n2 < n ? n2 : n);
    best = 1e9;
    cui_ctx *ui = ctx();
    cui_vt *w = mk(80, 24, 100, SX);
    build(ui, w);
    for (int k = 0; k < 12; k++) {
        W(w, "\x1b[2J\x1b[H");
        build(ui, w);
        double t0 = now_s();
        W(w, "\x1b[H"); cui_vt_write(w, s, n2);
        build(ui, w); render(ui);
        double dt = now_s() - t0;
        if (dt < best) best = dt;
    }
    printf("  one 400x400 picture, first byte to drawn (decode, textures, software render of 640x400): %.2f ms (best of 12)\n", best * 1000);
    cui_vt *off = cui_vt_create(80, 24, 100), *on = mk(80, 24, 100, Q | SX);
    printf("  memory (80x24, 100 lines): features off %zu bytes, on without a picture %zu, with the 400x400 picture %zu\n",
           cui_vt_memory(off), cui_vt_memory(on), cui_vt_memory(w));
    cui_vt_destroy(off); cui_vt_destroy(on);
    cui_vt_destroy(w);
    cui_destroy(ui);
    cui_vt_destroy(v);
    free(s);
}

int main(void) {
    g_sw = cui_sw_create();
    test_queries_off();
    test_queries_on();
    test_decode();
    test_split();
    test_cursor();
    test_storage();
    test_budget();
    test_hostile();
    test_paint();
    test_aspect();
    test_speed();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
