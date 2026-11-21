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

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
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
    if (argc > 1 && !strcmp(argv[1], "--bench")) bench();
    cui_sw_destroy(g_sw);
    printf("test_bidi_edit: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
#endif
