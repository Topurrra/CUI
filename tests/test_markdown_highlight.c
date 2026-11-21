/* Marked ranges in the markdown view (cui_markdown_opts.highlights, find in page):
 *
 *   - a range is drawn under its text, in a match tone, and the current one stronger; the pixels that change are a box
 *     on the word, nothing else; light and dark, and the text stays readable (contrast of the text on each tone);
 *   - zero ranges, and ranges that are empty, out of order or past the end, change no pixel;
 *   - the result reports where the current range is (highlight_y, found, exact): a heading, a line of a wrapped
 *     paragraph, a paragraph joined from several source lines, a table cell, a code block;
 *   - a big text: a range in a block that is not built is placed by an estimate first and exactly once the view has
 *     scrolled there, and it is drawn.
 *
 *     cui_test_markdown_highlight
 */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define W 600
#define H 420

static cui_sw_renderer *g_sw;
static cui_ctx *new_ctx(bool dark) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, W, H, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

typedef struct {
    const char *text;
    int len;
    const cui_markdown_range *hl; int nhl, cur;
    bool no_select;
    cui_markdown_result res;
} view;

static void build(cui_ctx *ui, view *v) {
    cui_scroll(ui, .id = "s", .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .pad = 24, .width = CUI_FILL) {
            v->res = cui_markdown(ui, "md", v->text, .length = v->len ? v->len : (int)strlen(v->text), .no_select = v->no_select,
                                  .highlights = v->hl, .highlight_count = v->nhl, .highlight_current = v->cur);
        }
    }
}
static void frame(cui_ctx *ui, view *v) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    build(ui, v);
    cui_end_frame(ui);
}
static void settle(cui_ctx *ui, view *v) {
    for (int n = 0; n < 60; n++) {
        frame(ui, v);
        if (!cui_needs_redraw(ui)) break;
    }
    frame(ui, v);
}
static void pixels(cui_ctx *ui, uint8_t *px) {
    cui_sw_render(g_sw, cui_get_draw_data(ui), px, W, H, W * 4, CUI_SW_RGBA);
}

typedef struct { int x0, y0, x1, y1, n; } box;
static box diff_box(const uint8_t *a, const uint8_t *b) {
    box r = { W, H, -1, -1, 0 };
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (memcmp(a + (y * W + x) * 4, b + (y * W + x) * 4, 4)) {
                if (x < r.x0) r.x0 = x;
                if (y < r.y0) r.y0 = y;
                if (x > r.x1) r.x1 = x;
                if (y > r.y1) r.y1 = y;
                r.n++;
            }
    return r;
}
/* the colour most often seen where the two images differ, in b */
static void tone_of(const uint8_t *a, const uint8_t *b, int out[3]) {
    static uint32_t key[4096];
    static int cnt[4096];
    int nk = 0;
    for (int i = 0; i < W * H; i++) {
        if (!memcmp(a + i * 4, b + i * 4, 4)) continue;
        uint32_t k = (uint32_t)b[i * 4] << 16 | (uint32_t)b[i * 4 + 1] << 8 | b[i * 4 + 2];
        int j = 0;
        while (j < nk && key[j] != k) j++;
        if (j == nk && nk < 4096) { key[nk] = k; cnt[nk++] = 0; }
        if (j < nk) cnt[j]++;
    }
    int best = 0;
    for (int j = 1; j < nk; j++) if (cnt[j] > cnt[best]) best = j;
    out[0] = nk ? (int)(key[best] >> 16) : 0; out[1] = nk ? (int)(key[best] >> 8 & 255) : 0; out[2] = nk ? (int)(key[best] & 255) : 0;
}
static double lum(const int c[3]) {
    double v[3];
    for (int i = 0; i < 3; i++) { double s = c[i] / 255.0; v[i] = s <= 0.03928 ? s / 12.92 : pow((s + 0.055) / 1.055, 2.4); }
    return 0.2126 * v[0] + 0.7152 * v[1] + 0.0722 * v[2];
}
static double contrast(const int a[3], const int b[3]) {
    double la = lum(a), lb = lum(b);
    return (fmax(la, lb) + 0.05) / (fmin(la, lb) + 0.05);
}
static int dist(const int a[3], const int b[3]) { return abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]); }

static cui_markdown_range range_of(const char *text, const char *what, int nth) {
    const char *p = text;
    for (int k = 0; k <= nth; k++) { p = strstr(p, what); if (!p) { cui_markdown_range r = { 0, 0 }; return r; } if (k < nth) p++; }
    cui_markdown_range r = { (int)(p - text), (int)(p - text) + (int)strlen(what) };
    return r;
}

static uint8_t g_a[W * H * 4], g_b[W * H * 4], g_c[W * H * 4];

/* a word is drawn marked; only a box on it changes; the current one is stronger; readable; light and dark */
static void test_draw(bool dark) {
    printf("  drawing, %s\n", dark ? "dark" : "light");
    static const char text[] = "Plain words alpha beta gamma delta and a second sentence here.\n";
    cui_ctx *ui = new_ctx(dark);
    view v = { .text = text };
    settle(ui, &v);
    pixels(ui, g_a);
    cui_markdown_range alpha = range_of(text, "alpha", 0), beta = range_of(text, "beta", 0), gamma = range_of(text, "gamma", 0);

    v.hl = &beta; v.nhl = 1; v.cur = 0;
    settle(ui, &v);
    pixels(ui, g_b);
    box bb = diff_box(g_a, g_b);
    CHECK(bb.n > 100);
    CHECK(bb.y1 - bb.y0 >= 10 && bb.y1 - bb.y0 <= 40);
    CHECK(bb.x1 - bb.x0 >= 20 && bb.x1 - bb.x0 <= 70);   /* "beta" */
    int tone[3];
    tone_of(g_a, g_b, tone);

    v.cur = 1;
    settle(ui, &v);
    pixels(ui, g_c);
    box bc = diff_box(g_a, g_c);
    CHECK(bc.x0 == bb.x0 && bc.x1 == bb.x1 && bc.y0 == bb.y0 && bc.y1 == bb.y1);
    int cur[3], bg[3] = { g_a[(bb.y0 * W + bb.x0) * 4], g_a[(bb.y0 * W + bb.x0) * 4 + 1], g_a[(bb.y0 * W + bb.x0) * 4 + 2] };
    tone_of(g_a, g_c, cur);
    CHECK(dist(cur, bg) > dist(tone, bg) + 20);          /* the current one is stronger */
    CHECK(dist(tone, bg) > 25);                           /* and a match is visible */
    cui_color fgc = ui->theme.colors[CUI_TOKEN_FOREGROUND];
    int fg[3] = { fgc.r, fgc.g, fgc.b };
    double c1 = contrast(fg, tone), c2 = contrast(fg, cur);
    printf("    tone %02x%02x%02x contrast %.1f, current %02x%02x%02x contrast %.1f\n", tone[0], tone[1], tone[2], c1, cur[0], cur[1], cur[2], c2);
    CHECK(c1 >= 4.5 && c2 >= 4.5);

    /* the boxes of three words go left to right, apart */
    cui_markdown_range two[2] = { alpha, gamma };
    v.hl = &alpha; v.nhl = 1; v.cur = 0;
    settle(ui, &v);
    pixels(ui, g_c);
    box ba = diff_box(g_a, g_c);
    v.hl = &gamma;
    settle(ui, &v);
    pixels(ui, g_c);
    box bg2 = diff_box(g_a, g_c);
    CHECK(ba.x1 < bb.x0 + 8 && bb.x1 < bg2.x0 + 8 && ba.x0 < bb.x0 && bb.x0 < bg2.x0);
    /* two ranges at once: both drawn, and the pixels of the word between them do not change */
    v.hl = two; v.nhl = 2; v.cur = 2;
    settle(ui, &v);
    pixels(ui, g_c);
    box b2 = diff_box(g_a, g_c);
    CHECK(b2.x0 == ba.x0 && b2.x1 == bg2.x1);
    cui_destroy(ui);
}

/* no ranges, or ranges that do not count: no pixel changes */
static void test_off(void) {
    printf("  off\n");
    static const char text[] = "# Head\n\nOne two three four.\n\n- item five\n";
    cui_ctx *ui = new_ctx(false);
    view v = { .text = text };
    settle(ui, &v);
    pixels(ui, g_a);
    cui_markdown_range bad[] = { { 10, 10 }, { 8, 4 }, { 5000, 5010 }, { -3, 0 } };
    v.hl = bad; v.nhl = 4; v.cur = 2;
    settle(ui, &v);
    pixels(ui, g_b);
    CHECK(!memcmp(g_a, g_b, sizeof g_a));
    CHECK(!v.res.highlight_found);
    cui_markdown_range one = range_of(text, "three", 0);
    v.hl = &one; v.nhl = 1; v.cur = 0;               /* marked, but none current */
    settle(ui, &v);
    CHECK(!v.res.highlight_found);
    v.hl = NULL; v.nhl = 0; v.cur = 1;
    settle(ui, &v);
    pixels(ui, g_b);
    CHECK(!memcmp(g_a, g_b, sizeof g_a) && !v.res.highlight_found);
    cui_destroy(ui);
}

static void test_y(void) {
    printf("  where the current one is\n");
    static const char text[] =
        "# First heading\n\nShort paragraph here.\n\n## Second heading with needle\n\n"
        "A long paragraph that goes on and on, with enough words in it to wrap over several lines at this width, so that the last word, "
        "which is tail, sits well below the first word, which is head; and it keeps going to be sure of it: one two three four five six "
        "seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty.\n\n"
        "Joined line one\nthen joined line two has joinword in it\nand line three.\n\n"
        "| col a | col b |\n|---|---|\n| cell x | cell needletwo |\n\n"
        "```c\nint a;\nint codeword;\nint c;\n```\n\nEnd.\n";
    cui_ctx *ui = new_ctx(false);
    view v = { .text = text };
    settle(ui, &v);
    pixels(ui, g_a);
    float lh = 0;
    {   /* the heading */
        cui_markdown_range r = range_of(text, "needle", 0);
        v.hl = &r; v.nhl = 1; v.cur = 1;
        settle(ui, &v);
        CHECK(v.res.highlight_found && v.res.highlight_exact);
        CHECK(v.res.heading_count >= 2 && fabsf(v.res.highlight_y - v.res.headings[1].y) < 1.0f);
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n > 50);
    }
    float y_head, y_tail;
    {   /* the first and the last line of a wrapped paragraph */
        cui_markdown_range r = range_of(text, "A long", 0);
        v.hl = &r;
        settle(ui, &v);
        CHECK(v.res.highlight_found);
        y_head = v.res.highlight_y;
        r = range_of(text, "twenty", 0);
        settle(ui, &v);
        y_tail = v.res.highlight_y;
        r = range_of(text, "tail", 0);
        settle(ui, &v);
        float y_mid = v.res.highlight_y;
        lh = (y_tail - y_head) / 3.0f;
        printf("    head %.1f mid %.1f tail %.1f\n", (double)y_head, (double)y_mid, (double)y_tail);
        CHECK(y_tail > y_head + 20 && y_mid > y_head);
    }
    {   /* a paragraph joined from three source lines: drawn, and placed on its second line */
        cui_markdown_range r = range_of(text, "joinword", 0);
        v.hl = &r;
        settle(ui, &v);
        CHECK(v.res.highlight_found && v.res.highlight_y > y_tail);
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n > 50);
        cui_markdown_range r1 = range_of(text, "Joined", 0);
        v.hl = &r1;
        settle(ui, &v);
        float y1 = v.res.highlight_y;
        v.hl = &r;
        settle(ui, &v);
        CHECK(v.res.highlight_y >= y1);   /* the soft break may keep it on one line */
        cui_markdown_range r3 = range_of(text, "three.", 0);
        v.hl = &r3;
        settle(ui, &v);
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n > 50);
    }
    {   /* a table cell, and the header of a table (selectable view) */
        cui_markdown_range r = range_of(text, "needletwo", 0);
        v.hl = &r;
        settle(ui, &v);
        CHECK(v.res.highlight_found);
        float y_cell = v.res.highlight_y;
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n > 50);
        cui_markdown_range h = range_of(text, "col b", 0);
        v.hl = &h;
        settle(ui, &v);
        CHECK(v.res.highlight_found && v.res.highlight_y < y_cell);
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n > 50);
    }
    {   /* a code block: placed (by its line), not drawn */
        cui_markdown_range r = range_of(text, "codeword", 0), r0 = range_of(text, "int a", 0);
        v.hl = &r0;
        settle(ui, &v);
        CHECK(v.res.highlight_found);
        float ya = v.res.highlight_y;
        v.hl = &r;
        settle(ui, &v);
        CHECK(v.res.highlight_found && v.res.highlight_y > ya);
        pixels(ui, g_b);
        CHECK(diff_box(g_a, g_b).n == 0);
    }
    (void)lh;
    cui_destroy(ui);
}

/* a big text: placed by an estimate, then exactly once it scrolled there; drawn */
static void test_big(void) {
    printf("  big text\n");
    int cap = 200 * 1024, n = 0;
    char *t = malloc((size_t)cap);
    for (int k = 0; k < 400 && n < cap - 600; k++) {
        n += snprintf(t + n, (size_t)(cap - n), "## Section %d\n\nParagraph of section %d with some words to fill the line and then a few more so that it wraps over "
                      "two or three lines in the view, %s.\n\n", k, k, k == 320 ? "the needleword is here" : "nothing special");
    }
    CHECK(n > 32 * 1024);
    int saved_big = cui__md_big_bytes, saved_sync = cui__md_sync_bytes;
    cui__md_big_bytes = 32 * 1024; cui__md_sync_bytes = 0x7FFFFFFF;
    cui_ctx *ui = new_ctx(false);
    view v = { .text = t, .len = n };
    settle(ui, &v);
    pixels(ui, g_a);
    cui_markdown_range r = range_of(t, "needleword", 0);
    v.hl = &r; v.nhl = 1; v.cur = 1;
    int first_exact = -1, frames = 0, jumps = 0;
    for (int f = 0; f < 120; f++) {
        frame(ui, &v);
        frames++;
        if (v.res.highlight_found && v.res.highlight_exact && first_exact < 0) first_exact = f;
        if (v.res.highlight_found) {
            cui_scroll_info si = cui_scroll_get(ui, "s");
            float y = 24 + v.res.highlight_y;                       /* in the scroll's content */
            if (y < si.target_y + 20 || y > si.target_y + si.view_h - 60) { cui_scroll_set(ui, "s", y - si.view_h * 0.3f, true); jumps++; }
            else if (v.res.highlight_exact && f > first_exact + 4) break;
        }
    }
    settle(ui, &v);
    printf("    frames %d, jumps %d, exact from frame %d\n", frames, jumps, first_exact);
    CHECK(v.res.highlight_found && v.res.highlight_exact);
    cui_scroll_info si = cui_scroll_get(ui, "s");
    float y = 24 + v.res.highlight_y;
    CHECK(y >= si.y && y <= si.y + si.view_h - 10);
    pixels(ui, g_b);
    v.hl = NULL; v.nhl = 0; v.cur = 0;
    settle(ui, &v);
    pixels(ui, g_c);
    box bx = diff_box(g_b, g_c);
    CHECK(bx.n > 100 && bx.y0 >= (int)(y - si.y) - 6 && bx.y1 <= (int)(y - si.y) + 30);
    cui_destroy(ui);
    cui__md_big_bytes = saved_big; cui__md_sync_bytes = saved_sync;
    free(t);
}

int main(void) {
    g_sw = cui_sw_create();
    printf("markdown highlights\n");
    test_draw(false);
    test_draw(true);
    test_off();
    test_y();
    test_big();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
