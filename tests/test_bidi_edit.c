/* test_bidi_edit.c - right-to-left text in a CUI_TEXT_SHAPING build with the system fonts (Segoe UI has
 * Hebrew and Arabic): the shaped line's visual order and Arabic joining, the caret, hit and selection
 * helpers that text fields use with cui_input_use_bidi, and a text field drawn both ways.
 * Windows only (the fonts); elsewhere, or without shaping, it passes at once.
 *
 *   test_bidi_edit            the checks
 *   test_bidi_edit --bench    also the layout times: shaped Hebrew/Arabic titles, cold and cached, against Latin */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include "../src/cui_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) || !defined(CUI_TEXT_SHAPING)
int main(void) { printf("test_bidi_edit: skipped (needs CUI_TEXT_SHAPING and the Windows fonts)\n"); return 0; }
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CLOSE(a, b) (fabsf((a) - (b)) < 0.51f)

static cui_sw_renderer *g_sw;

#define HE "\xd7\x90\xd7\x91\xd7\x92"              /* alef bet gimel: 3 letters, 6 bytes */
#define AR_BEH "\xd8\xa8"                           /* beh */
#define AR_SALAM "\xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85"   /* al-salam, with a lam-alef */

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 480, 120, 1);
    cui_set_reduced_motion(ui, true);
    cui_font_use_system_fallback(ui, true);
    cui_input_use_bidi(ui, true);
    return ui;
}

static float caret(cui_ctx *ui, const char *s, int off) { return cui__bidi_caret_x(ui, CUI_FONT_BODY, 14, s, (int)strlen(s), off); }
static int hit(cui_ctx *ui, const char *s, float x) { return cui__bidi_hit(ui, CUI_FONT_BODY, 14, s, (int)strlen(s), x); }
static float wid(cui_ctx *ui, const char *s) { return cui__bidi_width(ui, CUI_FONT_BODY, 14, s, (int)strlen(s)); }

/* Hebrew alone in a left-to-right field: the first letter is at the right, the caret walks to the left */
static void test_hebrew(void) {
    cui_ctx *ui = new_ctx();
    const char *s = HE;
    CHECK(cui__bidi_wanted(ui, s, 6));
    CHECK(!cui__bidi_wanted(ui, "plain latin", 11));
    float w = wid(ui, s);
    CHECK(w > 10);
    CHECK(CLOSE(caret(ui, s, 0), w));          /* before alef: the right edge */
    CHECK(caret(ui, s, 2) < caret(ui, s, 0));
    CHECK(caret(ui, s, 4) < caret(ui, s, 2));
    CHECK(CLOSE(caret(ui, s, 6), 0));          /* after gimel: the left edge */
    CHECK(hit(ui, s, w + 20) == 0);           /* right of the text: its start */
    CHECK(hit(ui, s, -20) == 6);              /* left of it: its end */
    CHECK(hit(ui, s, caret(ui, s, 2) + 0.2f) == 2);
    cui_destroy(ui);
}

/* "abc " then Hebrew: Latin left to right, then the Hebrew run reversed at the right */
static void test_mixed(void) {
    cui_ctx *ui = new_ctx();
    const char *s = "abc " HE;            /* bytes: a0 b1 c2 sp3, alef 4, bet 6, gimel 8, end 10 */
    float w = wid(ui, s);
    CHECK(caret(ui, s, 0) == 0);
    CHECK(caret(ui, s, 1) > 0 && caret(ui, s, 2) > caret(ui, s, 1) && caret(ui, s, 4) > caret(ui, s, 3));
    CHECK(CLOSE(caret(ui, s, 10), caret(ui, s, 4)));   /* the end of the Hebrew run is at its left edge */
    CHECK(caret(ui, s, 6) > caret(ui, s, 8) && caret(ui, s, 6) < w);
    CHECK(hit(ui, s, w + 10) == 4);       /* right of the text: before alef */
    CHECK(hit(ui, s, 1) == 0);
    /* a selection over "c" + space + alef: two pieces, "c " on the left and alef at the far right */
    float sp[16];
    int n = cui__bidi_spans(ui, CUI_FONT_BODY, 14, s, (int)strlen(s), 2, 6, sp, 8);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(CLOSE(sp[0], caret(ui, s, 2)) && CLOSE(sp[1], caret(ui, s, 4)));
        CHECK(CLOSE(sp[3], w) && sp[2] > sp[1]);
    }
    /* everything: one piece from 0 to the width */
    n = cui__bidi_spans(ui, CUI_FONT_BODY, 14, s, (int)strlen(s), 0, 10, sp, 8);
    CHECK(n == 1 && CLOSE(sp[0], 0) && CLOSE(sp[1], w));
    cui_destroy(ui);
}

/* Arabic joins: four behs are narrower than four isolated ones, and the lam-alef is one glyph */
static void test_arabic(void) {
    cui_ctx *ui = new_ctx();
    float one = wid(ui, AR_BEH);
    float four = wid(ui, AR_BEH AR_BEH AR_BEH AR_BEH);
    CHECK(one > 2 && four < 4 * one - 1);
    const char *s = AR_SALAM;                                  /* alef lam seen lam-alef meem */
    int len = (int)strlen(s);
    CHECK(CLOSE(caret(ui, s, 0), wid(ui, s)));
    CHECK(CLOSE(caret(ui, s, len), 0));
    for (int off = 2; off <= len; off += 2) CHECK(caret(ui, s, off) <= caret(ui, s, off - 2) + 0.01f);
    /* between the lam and the alef of the ligature: inside the ligature glyph, not at one of its edges */
    float a = caret(ui, s, 6), b = caret(ui, s, 8), c = caret(ui, s, 10);
    CHECK(a > b && b > c);
    cui_destroy(ui);
}

/* the field: with the option the caret at the start of Hebrew text is at its right end (the IME rect says
 * where it is), without it at the left as before; typing goes in at the caret in both */
static cui_rect g_ime;
static void ime_rect(void *user, cui_rect r) { (void)user; g_ime = r; }

static float field_caret(bool bidi, const char *text, int key) {
    cui_ctx *ui = new_ctx();
    cui_input_use_bidi(ui, bidi);   /* new_ctx turned it on */
    cui_platform_iface pf = { 0 };
    pf.set_text_input_rect = ime_rect;
    cui_set_platform(ui, &pf);
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    float x0 = 0;
    for (int f = 0; f < 6; f++) {
        if (f == 2) { cui_input_key(ui, (cui_key)key, true, 0); }
        if (f == 3) { cui_input_key(ui, (cui_key)key, false, 0); }
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 10) {
            cui_input(ui, "f", buf, sizeof buf, .autofocus = true);
            x0 = ui->nodes[ui->last_node].rect.x;
        }
        cui_end_frame(ui);
    }
    cui_destroy(ui);
    (void)x0;
    return g_ime.x;
}

static void test_field(void) {
    float off_home = field_caret(false, HE, CUI_KEY_HOME), off_end = field_caret(false, HE, CUI_KEY_END);
    float on_home = field_caret(true, HE, CUI_KEY_HOME), on_end = field_caret(true, HE, CUI_KEY_END);
    CHECK(off_home < off_end);    /* the old way: logical order drawn left to right */
    CHECK(on_home > on_end);      /* reading order: the start is at the right */
    CHECK(CLOSE(on_end, off_home));
    /* Latin is the same either way */
    CHECK(field_caret(true, "hello", CUI_KEY_END) == field_caret(false, "hello", CUI_KEY_END));
}

/* Draws one ellipsis-cut line the way a tab strip does into an RGBA buffer; the first and last inked columns. */
static bool g_shots;
static const char *g_shot_dir = ".";
static void ink_cols(const uint8_t *px, int w, int h, int *a, int *b) {
    *a = w; *b = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            if (p[0] < 128) { if (x < *a) *a = x; if (x > *b) *b = x; }
        }
}
static void draw_cut(const char *text, float max_w, const char *shot, int *ink_a, int *ink_b) {
    enum { W = 300, H = 24 };
    static uint8_t px[W * H * 4];
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, W, H, 1);
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_draw_rect(ui, (cui_rect){ 0, 0, W, H }, CUI_CORNERS(0), (cui_color){ 255, 255, 255, 255 });
    cui__draw_text_ellipsis(ui, CUI_FONT_BODY, 14, 10, 4, max_w, (cui_color){ 0, 0, 0, 255 }, text, -1, false);
    cui_end_frame(ui);
    cui_sw_render(g_sw, cui_get_draw_data(ui), px, W, H, W * 4, CUI_SW_RGBA);
    ink_cols(px, W, H, ink_a, ink_b);
    if (g_shots && shot) {
        char path[300];
        snprintf(path, sizeof path, "%s/%s", g_shot_dir, shot);
        cui_write_png(path, px, W, H, W * 4);
    }
    cui_destroy(ui);
}

/* a title that starts in Latin and ends right to left, cut at the right edge: the ink stays inside the box, and the cut
 * is close to the edge (a short title that fits is drawn whole) */
static void test_cut(void) {
    static const char *he = "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d \xd7\x95\xd7\x99\xd7\xa7\xd7\x99\xd7\xa4\xd7\x93\xd7\x99\xd7\x94 \xd7\x94\xd7\x90\xd7\xa0\xd7\xa6\xd7\x99\xd7\xa7\xd7\x9c\xd7\x95\xd7\xa4\xd7\x93\xd7\x99\xd7\x94";
    static const char *ar = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83\xd9\x85";
    const char *src[2] = { he, ar };
    char text[256];
    int bad = 0;
    for (int k = 0; k < 2; k++) {
        snprintf(text, sizeof text, "Wiki: %s", src[k]);
        cui_ctx *wc = new_ctx();
        float full = wid(wc, text);
        cui_destroy(wc);
        for (int mw = 50; mw < (int)full; mw += 2) {
            int a, b;
            char shot[64];
            snprintf(shot, sizeof shot, "cut%d_%d.png", k, mw);
            draw_cut(text, (float)mw, (mw % 40 == 0) ? shot : NULL, &a, &b);
            bool over = b > 10 + mw + 1, under = 10 + mw - b > 22;
            if (over || under) { bad++; printf("  cut %d max_w %d: ink %d..%d (%s)\n", k, mw, a, b, over ? "too wide" : "much too short"); }
        }
    }
    CHECK(bad == 0);
}

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

/* the multi-line field: with the option a line of Hebrew starts at the right (its caret at the line start is at the right end,
 * clicking and selecting follow the glyphs); the first line stays left to right; without it nothing changes */
static float area_caret(bool bidi, const char *text, int key, float *line2_x) {
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 480, 200, 1);
    cui_input_use_bidi(ui, bidi);
    cui_platform_iface pf = { 0 };
    pf.set_text_input_rect = ime_rect;
    cui_set_platform(ui, &pf);
    char buf[128];
    snprintf(buf, sizeof buf, "%s", text);
    for (int f = 0; f < 6; f++) {
        if (f == 2) cui_input_key(ui, (cui_key)key, true, 0);
        if (f == 3) cui_input_key(ui, (cui_key)key, false, 0);
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 10) {
            cui_textarea(ui, "a", buf, sizeof buf, .autofocus = true, .height = 90, .width = CUI_PX(300));
        }
        cui_end_frame(ui);
    }
    if (line2_x) *line2_x = g_ime.y;
    cui_destroy(ui);
    return g_ime.x;
}

static void test_area(void) {
    const char *t = HE;
    float off_home = area_caret(false, t, CUI_KEY_HOME, NULL), off_end = area_caret(false, t, CUI_KEY_END, NULL);
    float on_home = area_caret(true, t, CUI_KEY_HOME, NULL), on_end = area_caret(true, t, CUI_KEY_END, NULL);
    CHECK(off_home < off_end);   /* the old way: logical order drawn left to right */
    CHECK(on_home > on_end);     /* reading order: the start of the line is at the right */
    CHECK(CLOSE(on_end, off_home));
    CHECK(area_caret(true, "hello", CUI_KEY_END, NULL) == area_caret(false, "hello", CUI_KEY_END, NULL));   /* Latin: the same either way */
    if (g_shots) {   /* a picture: Latin, Hebrew and mixed lines, everything selected */
        static uint8_t px[320 * 130 * 4];
        cui_ctx *ui = new_ctx();
        cui_set_viewport(ui, 320, 130, 1);
        char buf[160];
        snprintf(buf, sizeof buf, "abc def\n%s %s\nabc %s xyz", HE, HE, HE);
        for (int f = 0; f < 6; f++) {
            if (f == 2) { cui_input_key(ui, CUI_KEY_A, true, CUI_MOD_CTRL); cui_input_key(ui, CUI_KEY_A, false, CUI_MOD_CTRL); }
            cui_begin_frame(ui, 1.0f / 60.0f);
            cui_column(ui, .pad = 10) { cui_textarea(ui, "a", buf, sizeof buf, .autofocus = true, .height = 100, .width = CUI_PX(300)); }
            cui_end_frame(ui);
        }
        cui_sw_render(g_sw, cui_get_draw_data(ui), px, 320, 130, 320 * 4, CUI_SW_RGBA);
        char path[300];
        snprintf(path, sizeof path, "%s/area.png", g_shot_dir);
        cui_write_png(path, px, 320, 130, 320 * 4);
        cui_destroy(ui);
    }
}

/* ---- text areas as paragraphs (cui_input_use_paragraphs) ---- */
typedef struct { cui_ctx *ui; char buf[2048]; int dir; bool sel_all; } ta_t;

static void ta_frame(ta_t *t) {
    cui_begin_frame(t->ui, 1.0f / 60.0f);
    cui_column(t->ui, .pad = 10) {
        cui_textarea(t->ui, "a", t->buf, sizeof t->buf, .autofocus = true, .height = 110, .width = CUI_PX(300), .direction = (cui_direction)t->dir);
    }
    cui_end_frame(t->ui);
    (void)cui_get_draw_data(t->ui);
}
static ta_t *ta_new(const char *text, bool paras, int dir) {
    ta_t *t = calloc(1, sizeof *t);
    t->ui = new_ctx();
    cui_set_viewport(t->ui, 480, 200, 1);
    cui_input_use_paragraphs(t->ui, paras);
    cui_platform_iface pf = { 0 };
    pf.set_text_input_rect = ime_rect;
    cui_set_platform(t->ui, &pf);
    snprintf(t->buf, sizeof t->buf, "%s", text);
    t->dir = dir;
    for (int f = 0; f < 4; f++) ta_frame(t);
    return t;
}
static void ta_free(ta_t *t) { cui_destroy(t->ui); free(t); }
static void ta_key(ta_t *t, cui_key key, int mods) {
    cui_input_key(t->ui, key, true, mods); ta_frame(t);
    cui_input_key(t->ui, key, false, 0); cui_input_key(t->ui, CUI_KEY_NONE, false, 0); ta_frame(t);   /* modifiers are sampled on key-down only */
}
static void ta_keys(ta_t *t, cui_key key, int mods, int n) { for (int i = 0; i < n; i++) ta_key(t, key, mods); }
static int ta_cur(ta_t *t) { return t->ui->edit ? t->ui->edit->cursor : -1; }
static float ta_x(void) { return g_ime.x; }
/* the caret x at the start of a plain Latin line: the area's left edge */
static float left_ref(void) {
    ta_t *t = ta_new("abc", true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    float x = ta_x();
    ta_free(t);
    return x;
}
#define HEW HE " " HE " " HE " " HE " " HE " " HE " " HE " " HE " " HE " " HE " " HE   /* eleven words: about 255 points, a little under the field's width */

/* a paragraph's direction comes from its first strong character; right-to-left ones are right-aligned, every wrapped line */
static void test_para_align(void) {
    float L = left_ref();
    ta_t *t = ta_new("abc\n" HE " " HE "\n" HEW " " HEW, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(CLOSE(ta_x(), L));                       /* Latin paragraph: starts at the left */
    ta_key(t, CUI_KEY_DOWN, 0);                    /* the Hebrew line: the nearest place to that x is its far (left) end */
    CHECK(ta_cur(t) == 4 + 6 + 1 + 6);
    CHECK(ta_x() > L + 150);
    ta_key(t, CUI_KEY_HOME, 0);
    CHECK(ta_cur(t) == 4);
    CHECK(ta_x() > L + 230);                       /* the start of a right-to-left line: the right edge */
    ta_keys(t, CUI_KEY_DOWN, 0, 1);                /* the long paragraph wraps: its first line starts at the right too */
    ta_key(t, CUI_KEY_HOME, 0);
    CHECK(ta_x() > L + 230);
    ta_key(t, CUI_KEY_END, CUI_MOD_CTRL);          /* the last wrapped line */
    ta_key(t, CUI_KEY_HOME, 0);
    CHECK(ta_x() > L + 230);
    ta_free(t);
    /* the same text without the option: left-aligned as before */
    t = ta_new("abc\n" HE " " HE "\n", false, 0);
    ta_key(t, CUI_KEY_DOWN, 0);
    ta_key(t, CUI_KEY_HOME, 0);
    CHECK(ta_x() < L + 100);
    ta_free(t);
    /* first strong character: digits and punctuation before the Hebrew do not count, a Latin word does */
    t = ta_new("123 " HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(ta_x() > L + 200);
    ta_free(t);
    t = ta_new("abc " HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(CLOSE(ta_x(), L));
    ta_free(t);
    t = ta_new(HE " abc", true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(ta_x() > L + 200);
    ta_free(t);
    /* an explicit direction: Latin text right-aligned, and an empty field's caret at the right */
    t = ta_new("abc", true, CUI_DIRECTION_RTL);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(ta_x() > L + 200);
    ta_free(t);
    t = ta_new("", true, CUI_DIRECTION_RTL);
    CHECK(ta_x() > L + 230);
    ta_free(t);
    t = ta_new(HE, true, CUI_DIRECTION_LTR);   /* Hebrew in a fixed left-to-right paragraph: the run still reads right to left, at the left */
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(ta_x() < L + 40);
    ta_free(t);
}

/* Up and Down keep the caret's place on the screen, whatever the direction of the lines */
static void test_para_vertical(void) {
    float L = left_ref();
    /* a Latin line, a long Hebrew one (right-aligned, from about 30 to 270), a short Hebrew one */
    ta_t *t = ta_new("abcdefghijklmnopqrstu\n" HEW "\n" HE " " HE " " HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    ta_keys(t, CUI_KEY_RIGHT, 0, 10);
    float x0 = ta_x();
    CHECK(x0 > L + 40 && x0 < L + 100);
    ta_key(t, CUI_KEY_DOWN, 0);
    int c1 = ta_cur(t);
    printf("  vert: x0 %.1f c1 %d x %.1f L %.1f\n", x0, c1, ta_x(), L);
    CHECK(c1 > 22 && c1 < 22 + (int)strlen(HEW));                  /* in the Hebrew line */
    CHECK(fabsf(ta_x() - x0) < 9);                   /* at the same place on the screen */
    float x1 = ta_x();
    ta_key(t, CUI_KEY_DOWN, 0);                      /* the short line is right-aligned: the nearest place is its left end */
    CHECK(ta_cur(t) == (int)strlen(t->buf));
    CHECK(ta_x() > x1 && ta_x() > L + 150);
    ta_key(t, CUI_KEY_UP, 0);                        /* up again: back to the place the first Down started from */
    CHECK(fabsf(ta_x() - x0) < 9);
    ta_key(t, CUI_KEY_UP, 0);
    CHECK(ta_cur(t) == 10 && fabsf(ta_x() - x0) < 1);
    ta_free(t);
    /* without the option the old logical column: the same offset into the next line */
    t = ta_new("abcdefghij\n" HE " " HE " " HE, false, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    ta_keys(t, CUI_KEY_RIGHT, 0, 4);
    ta_key(t, CUI_KEY_DOWN, 0);
    CHECK(ta_cur(t) > 11);
    ta_free(t);
}

/* Left and Right move to the neighbouring place on the screen in lines with right-to-left text */
static void test_para_arrows(void) {
    /* a Hebrew paragraph: Left moves left, which is on in the text */
    ta_t *t = ta_new(HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    float x = ta_x();
    ta_key(t, CUI_KEY_LEFT, 0);
    CHECK(ta_cur(t) == 2 && ta_x() < x - 2);
    ta_key(t, CUI_KEY_RIGHT, 0);
    CHECK(ta_cur(t) == 0 && CLOSE(ta_x(), x));
    ta_free(t);
    /* two Hebrew lines: Left at the left end goes to the start (right end) of the next line, Right comes back */
    t = ta_new(HE "\n" HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    ta_keys(t, CUI_KEY_LEFT, 0, 3);
    CHECK(ta_cur(t) == 6);                          /* the end of line 1: its left end */
    ta_key(t, CUI_KEY_LEFT, 0);
    CHECK(ta_cur(t) == 7);                          /* the start of line 2: its right end */
    ta_key(t, CUI_KEY_RIGHT, 0);
    CHECK(ta_cur(t) == 6);
    ta_free(t);
    /* a Latin word then Hebrew in a left-to-right line: Right walks the glyphs from left to right, every press goes right,
     * through the Hebrew run from its left end to its right end: offset 3 (before the alef) is reached twice, once at the
     * run's left end (after the space) and once at its right end (before the alef) */
    t = ta_new("ab " HE " cd", true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    float prev = ta_x();
    int moves = 0, seen[16] = { 0 };
    for (int i = 0; i < 12; i++) {
        ta_key(t, CUI_KEY_RIGHT, 0);
        float nx = ta_x();
        if (nx > prev + 0.1f) moves++;
        else if (nx < prev - 0.1f) { CHECK(!"Right moved left"); break; }
        prev = nx;
        if (ta_cur(t) >= 0 && ta_cur(t) < 16) seen[ta_cur(t)]++;
    }
    CHECK(moves >= 9);
    CHECK(ta_cur(t) == 12);
    CHECK(seen[1] && seen[2] && seen[5] && seen[7] && seen[10] && seen[11]);
    CHECK(seen[3] >= 2);
    ta_free(t);
    t = ta_new("ab " HE " cd", true, 0);
    ta_key(t, CUI_KEY_END, CUI_MOD_CTRL);
    prev = ta_x();
    for (int i = 0; i < 8; i++) {
        ta_key(t, CUI_KEY_LEFT, 0);
        CHECK(ta_x() <= prev + 0.1f);
        prev = ta_x();
    }
    ta_free(t);
    /* a selection collapses to its visual end in a right-to-left line */
    t = ta_new(HE " " HE, true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    ta_keys(t, CUI_KEY_RIGHT, CUI_MOD_SHIFT, 1);       /* Shift+Right goes back, at the start: stays */
    ta_keys(t, CUI_KEY_LEFT, CUI_MOD_SHIFT, 2);        /* selects two characters */
    CHECK(t->ui->edit && t->ui->edit->anchor == 0 && t->ui->edit->cursor == 4);
    ta_key(t, CUI_KEY_LEFT, 0);                        /* collapses at the selection's left end: the cursor's side */
    CHECK(ta_cur(t) == 4);
    ta_free(t);
}

/* a selection in a right-to-left line is drawn at the right, one piece per direction run */
static void test_para_selection(void) {
    enum { W = 330, H = 150 };
    static uint8_t px[W * H * 4];
    ta_t *t = ta_new("abc def\n" HE " " HE "\n", true, 0);
    cui_set_viewport(t->ui, W, H, 1);
    cui_input_key(t->ui, CUI_KEY_A, true, CUI_MOD_CTRL); ta_frame(t);
    cui_input_key(t->ui, CUI_KEY_A, false, CUI_MOD_CTRL); ta_frame(t);
    cui_sw_render(g_sw, cui_get_draw_data(t->ui), px, W, H, W * 4, CUI_SW_RGBA);
    /* the selection tint: pixels that are neither the field's background nor dark text */
    const uint8_t *bg = px + ((size_t)(H - 8) * W + 40) * 4;   /* below the field's text */
    float L = left_ref();
    int row2 = 10 + 8 + 21 + 10;                                /* inside line 2 (line height about 21) */
    int a = W, b = -1, ra = W, rb = -1;
    for (int x = 0; x < W; x++) {
        const uint8_t *p = px + ((size_t)row2 * W + x) * 4;
        bool sel = (abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 12) && p[0] + p[1] + p[2] > 300;
        if (sel) { if (x < a) a = x; if (x > b) b = x; }
    }
    for (int x = 0; x < W; x++) {
        const uint8_t *p = px + ((size_t)(10 + 8 + 5) * W + x) * 4;   /* line 1 */
        bool sel = (abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 12) && p[0] + p[1] + p[2] > 300;
        if (sel) { if (x < ra) ra = x; if (x > rb) rb = x; }
    }
    if (g_shots) {
        char path[300];
        snprintf(path, sizeof path, "%s/paras.png", g_shot_dir);
        cui_write_png(path, px, W, H, W * 4);
    }
    CHECK(b > (int)L + 230);                 /* line 2 is selected up to the right edge */
    CHECK(a > (int)L + 150);                 /* and only its short right-aligned text, not from the left edge */
    CHECK(ra <= (int)L + 3 && rb < (int)L + 80);   /* line 1: from the left edge, over its short text */
    ta_free(t);
}

/* the system caret rect (where the IME's candidate window opens) follows the caret in a right-to-left line, and typing
 * Hebrew into a Latin paragraph switches it: the paragraph is right to left from its first strong character */
static void test_para_typing(void) {
    float L = left_ref();
    ta_t *t = ta_new("", true, 0);
    ta_key(t, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(CLOSE(ta_x(), L));
    cui_input_text(t->ui, HE); ta_frame(t); ta_frame(t);
    printf("  typed: len %d x %.1f L %.1f\n", (int)strlen(t->buf), ta_x(), L);
    CHECK(strcmp(t->buf, HE) == 0);
    CHECK(ta_x() > L + 200 && ta_x() < L + 275);   /* the paragraph turned right to left: the caret is after the letters, at the left of them */
    float x3 = ta_x();
    cui_input_text(t->ui, HE); ta_frame(t); ta_frame(t);
    CHECK(ta_x() < x3 - 5);                          /* each letter pushes the caret left */
    cui_input_text(t->ui, " abc"); ta_frame(t); ta_frame(t);   /* Latin typed inside the right-to-left paragraph */
    CHECK(ta_x() > L + 100 && strlen(t->buf) == 16);
    ta_free(t);
}

/* a frame that shows more right-to-left strings than the cache keeps does not shape the surplus again on every frame: the
 * bound grows with the frame's use (and stays bounded) */
static void test_cache_growth(void) {
    cui_ctx *ui = new_ctx();
    enum { N = 200 };
    char buf[N][48];
    for (int i = 0; i < N; i++) snprintf(buf[i], sizeof buf[i], HE " %d " AR_BEH, i);
    for (int frame = 0; frame < 3; frame++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        for (int i = 0; i < N; i++) (void)cui_measure_text(ui, CUI_FONT_BODY, 14, buf[i], -1);
        cui_end_frame(ui);
    }
    CHECK(ui->shape_cache_count > 64);    /* all of the frame's strings are kept */
    CHECK(ui->shape_cache_count <= 768);
    int kept = ui->shape_cache_count;
    double t0 = now_ms();
    cui_begin_frame(ui, 1.0f / 60.0f);
    for (int i = 0; i < N; i++) (void)cui_measure_text(ui, CUI_FONT_BODY, 14, buf[i], -1);
    cui_end_frame(ui);
    double warm = now_ms() - t0;
    CHECK(ui->shape_cache_count == kept);   /* a further frame shapes nothing new */
    printf("  %d distinct strings: kept %d, a warm frame %.2f ms\n", N, kept, warm);
    cui_destroy(ui);
}

/* layout times per title: cold (every text new, so nothing cached) and cached, shaped against Latin */
static void bench(void) {
    cui_ctx *ui = new_ctx();
    static const char *T[] = {
        "Latin: The quick brown fox jumps over the lazy dog",
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d \xd7\x95\xd7\x99\xd7\xa7\xd7\x99\xd7\xa4\xd7\x93\xd7\x99\xd7\x94",
        "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85",
        "Hebrew: \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d \xd7\x95\xd7\x99\xd7\xa7\xd7\x99\xd7\xa4\xd7\x93\xd7\x99\xd7\x94 \xd7\x94\xd7\x90\xd7\xa0\xd7\xa6\xd7\x99\xd7\xa7\xd7\x9c\xd7\x95\xd7\xa4\xd7\x93\xd7\x99\xd7\x94",
        "Arabic: \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83\xd9\x85 \xd9\x88\xd9\x8a\xd9\x83\xd9\x8a\xd8\xa8\xd9\x8a\xd8\xaf\xd9\x8a\xd8\xa7",
    };
    (void)wid(ui, T[3]); (void)wid(ui, T[4]);   /* the fonts are mapped once */
    static const char *name[] = { "Latin", "Hebrew", "Arabic", "Latin+Hebrew", "Latin+Arabic" };
    printf("  %-13s %10s %10s\n", "title", "cold us", "cached us");
    for (int t = 0; t < (int)(sizeof T / sizeof *T); t++) {
        enum { N = 2000 };
        char buf[256];
        double t0 = now_ms();
        for (int i = 0; i < N; i++) {
            snprintf(buf, sizeof buf, "%s %d", T[t], i);   /* new text: shaped from scratch */
            (void)cui_measure_text(ui, CUI_FONT_BODY, 14, buf, -1);
        }
        double cold = (now_ms() - t0) * 1000.0 / N;
        snprintf(buf, sizeof buf, "%s", T[t]);
        (void)cui_measure_text(ui, CUI_FONT_BODY, 14, buf, -1);
        t0 = now_ms();
        for (int i = 0; i < N; i++) (void)cui_measure_text(ui, CUI_FONT_BODY, 14, buf, -1);
        double warm = (now_ms() - t0) * 1000.0 / N;
        printf("  %-13s %10.2f %10.2f\n", name[t], cold, warm);
    }
    cui_destroy(ui);
}

/* --bench-area: a long mixed Latin/Hebrew/Arabic document in a text area; the CPU time of a frame (build, layout and paint to
 * the draw list) while typing at the end, typing at the start, and while the wheel scrolls it. mode 0: no bidi, 1:
 * cui_input_use_bidi, 2: with paragraphs (when the build has them). Medians in ms. */
static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double median(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmp_d); return v[n / 2]; }
static void bench_area_mode(int mode, char *doc, size_t cap) {
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 480, 400, 1);
    cui_input_use_bidi(ui, mode >= 1);
#ifdef CUI_INPUT_PARAGRAPHS
    cui_input_use_paragraphs(ui, mode >= 2);
#endif
    enum { FR = 240 };
    static double ms[FR];
    const char *what[3] = { "type at end", "type at start", "scroll" };
    printf("  mode %d:", mode);
    for (int phase = 0; phase < 3; phase++) {
        int n = 0;
        for (int f = 0; f < FR + 20; f++) {
            if (f == 1) {
                int key = phase == 1 ? CUI_KEY_HOME : CUI_KEY_END;
                cui_input_key(ui, (cui_key)key, true, CUI_MOD_CTRL); cui_input_key(ui, (cui_key)key, false, CUI_MOD_CTRL);
            }
            if (f >= 20 && phase < 2) cui_input_text(ui, (f & 1) ? "x" : "\xd7\x90");
            if (f >= 20 && phase == 2) cui_input_scroll(ui, 0, (f / 60) & 1 ? 3.0f : -3.0f);
            double t0 = now_ms();
            cui_begin_frame(ui, 1.0f / 60.0f);
            cui_column(ui, .pad = 10) { cui_textarea(ui, "a", doc, cap, .autofocus = true, .height = 360, .width = CUI_PX(440)); }
            cui_end_frame(ui);
            (void)cui_get_draw_data(ui);
            if (f >= 20) ms[n++] = now_ms() - t0;
        }
        { double md = median(ms, n); printf("  %s %.2f (min %.2f)", what[phase], md, ms[0]); }
    }
    printf(" ms/frame\n");
    cui_destroy(ui);
}
static void bench_area(void) {
    enum { CAP = 1 << 20 };
    static char doc[CAP], copy[CAP];
    static const char *P[] = {
        "The quick brown fox jumps over the lazy dog and keeps running through the long field of grass.",
        "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d \xd7\xa2\xd7\x95\xd7\x9c\xd7\x9d \xd7\x95\xd7\x99\xd7\xa7\xd7\x99\xd7\xa4\xd7\x93\xd7\x99\xd7\x94 \xd7\x94\xd7\x90\xd7\xa0\xd7\xa6\xd7\x99\xd7\xa7\xd7\x9c\xd7\x95\xd7\xa4\xd7\x93\xd7\x99\xd7\x94 \xd7\x94\xd7\x97\xd7\x95\xd7\xa4\xd7\xa9\xd7\x99\xd7\xaa \xd7\x95\xd7\xa2\xd7\x95\xd7\x93 \xd7\x9e\xd7\x99\xd7\x9c\xd7\x99\xd7\x9d \xd7\x91\xd7\xa9\xd7\x95\xd7\xa8\xd7\x94 \xd7\x90\xd7\x97\xd7\xaa",
        "Note: \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7 \xd8\xa8\xd8\xa7\xd9\x84\xd8\xb9\xd8\xa7\xd9\x84\xd9\x85 \xd8\xa7\xd9\x84\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 \xd8\xb9\xd9\x84\xd9\x8a\xd9\x83\xd9\x85 (v2) and more text after the Arabic part",
    };
    static const int sizes[] = { 60, 1500 };
    for (int s = 0; s < 2; s++) {
        int len = 0;
        for (int i = 0; i < sizes[s]; i++) len += snprintf(doc + len, CAP - (size_t)len, "%s\n", P[i % 3]);
        printf("  document: %d paragraphs, %d bytes\n", sizes[s], len);
        for (int mode = 0; mode <= 2; mode++) {
#ifndef CUI_INPUT_PARAGRAPHS
            if (mode == 2) break;
#endif
            memcpy(copy, doc, (size_t)len + 1);
            bench_area_mode(mode, copy, CAP);
        }
    }
}

static bool have_segoe(void) {
    char p[300];
    const char *w = getenv("WINDIR");
    snprintf(p, sizeof p, "%s\\Fonts\\segoeui.ttf", w ? w : "C:\\Windows");
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

int main(int argc, char **argv) {
    if (!have_segoe()) { printf("test_bidi_edit: skipped (no segoeui.ttf)\n"); return 0; }
    g_sw = cui_sw_create();
    test_hebrew();
    test_mixed();
    test_arabic();
    test_field();
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--shots")) { g_shots = true; g_shot_dir = argv[i + 1]; }
    test_cut();
    test_area();
    test_para_align();
    test_para_vertical();
    test_para_arrows();
    test_para_selection();
    test_para_typing();
    test_cache_growth();
    if (argc > 1 && !strcmp(argv[1], "--bench")) bench();
    if (argc > 1 && !strcmp(argv[1], "--bench-area")) bench_area();
    cui_sw_destroy(g_sw);
    printf("test_bidi_edit: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
#endif
