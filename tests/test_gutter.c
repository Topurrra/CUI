/* test_gutter.c - the code editor's glyph lane (breakpoints), presses in it, and the execution line.
 * Headless: the software renderer. --shot <file.png> draws a dark editor with every glyph kind for a look. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;

typedef struct {
    char buf[1024];
    bool lane, wrap, dark;
    const cui_gutter_glyph *gg;
    int ng, exec;
    cui_code_editor_result r;
    bool clicked, right;
    int line, clicks;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->buf, sizeof c->buf, .lang = "c", .height = 300, .width = CUI_PX(400), .wrap = c->wrap,
                               .glyph_lane = c->lane, .glyphs = c->gg, .glyph_count = c->ng, .exec_line = c->exec);
    }
    if (c->r.gutter_clicked) { c->clicked = true; c->right = c->r.gutter_right; c->line = c->r.gutter_line; c->clicks++; }
}

static void run(cui_ctx *ui, ed *c, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, c); cui_end_frame(ui); }
}

static cui_ctx *new_ctx(bool dark) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, 600, 400, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

static void press(cui_ctx *ui, ed *c, float x, float y, cui_mouse_button b) {
    cui_input_mouse_move(ui, x, y);
    run(ui, c, 1);
    cui_input_mouse_button(ui, b, true);
    run(ui, c, 1);
    cui_input_mouse_button(ui, b, false);
    run(ui, c, 1);
}

static unsigned char *pixels(cui_ctx *ui, int *w) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    *w = dd->width_px;
    return px;
}
static const unsigned char *at(const unsigned char *px, int w, float x, float y) { return px + ((size_t)(int)y * (size_t)w + (size_t)(int)x) * 4; }
static bool reddish(const unsigned char *p) { return p[0] > 150 && p[0] > p[1] + 60 && p[0] > p[2] + 60; }

static const char TEXT[] = "#include <stdio.h>\n\nint main(void) {\n    int n = 3;\n    printf(\"%d\\n\", n);\n    return 0;\n}\n";

static void test_lane(void) {
    cui_ctx *ui = new_ctx(false);
    ed c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "%s", TEXT);
    run(ui, &c, 3);
    float x_off = c.r.caret_x, y1 = c.r.caret_y, lh = c.r.caret_h;   /* caret at line 1: the code starts at x_off */
    CHECK(lh > 10);
    /* no lane: a press left of the line numbers selects the line, nothing is reported */
    press(ui, &c, 38, y1 + lh * 2.5f, CUI_MOUSE_LEFT);
    CHECK(!c.clicked);
    CHECK(c.r.cursor != c.r.anchor);                              /* the line is selected */

    /* the lane: the gutter grows by it, the code moves right by as much */
    c.lane = true;
    cui_code_editor_goto(ui, "ed", 1, 1);
    run(ui, &c, 3);
    CHECK(c.r.caret_x > x_off + 15 && c.r.caret_x < x_off + 17);
    /* a left press in the lane on line 2: reported, the selection stays */
    int cur = c.r.cursor;
    press(ui, &c, 39, y1 + lh * 1.5f, CUI_MOUSE_LEFT);
    CHECK(c.clicked && !c.right && c.line == 2 && c.clicks == 1);
    CHECK(c.r.cursor == cur && c.r.anchor == cur);
    /* the right button: reported as right */
    c.clicked = false;
    press(ui, &c, 39, y1 + lh * 4.5f, CUI_MOUSE_RIGHT);
    CHECK(c.clicked && c.right && c.line == 5);
    /* on the line numbers, right of the lane: a normal press (selects the line), nothing reported */
    c.clicked = false;
    press(ui, &c, 60, y1 + lh * 3.5f, CUI_MOUSE_LEFT);
    CHECK(!c.clicked);
    CHECK(c.r.cursor != c.r.anchor);
    /* below the last line: nothing reported */
    c.clicked = false;
    press(ui, &c, 39, y1 + lh * 12.5f, CUI_MOUSE_LEFT);
    CHECK(!c.clicked);

    /* glyphs: a breakpoint on line 2 is a red dot in the lane; the pointer over line 6 shows a faint one */
    cui_gutter_glyph gg[] = { { 2, CUI_GLYPH_BREAKPOINT }, { 4, CUI_GLYPH_BREAKPOINT_DISABLED }, { 99, CUI_GLYPH_BREAKPOINT } };
    c.gg = gg;
    c.ng = 3;                                                     /* one past the end: ignored */
    cui_input_mouse_move(ui, 39, y1 + lh * 5.5f);
    run(ui, &c, 2);
    int w;
    unsigned char *px = pixels(ui, &w);
    CHECK(reddish(at(px, w, 39, y1 + lh * 1.5f)));
    CHECK(!reddish(at(px, w, 39, y1 + lh * 3.5f)));               /* disabled: grey */
    CHECK(!reddish(at(px, w, 39, y1 + lh * 2.5f)));               /* no glyph */
    const unsigned char *gh = at(px, w, 39, y1 + lh * 5.5f), *bk = at(px, w, 39, y1 + lh * 6.5f);
    CHECK(gh[0] > gh[1] + 15 && gh[0] != bk[0]);                  /* the ghost: a faint red */
    CHECK(!reddish(gh));
    free(px);

    /* the execution line: line 5 tinted across, an arrow in the lane */
    c.exec = 5;
    cui_input_mouse_move(ui, 590, 390);
    run(ui, &c, 2);
    px = pixels(ui, &w);
    const unsigned char *t5 = at(px, w, x_off + 300, y1 + lh * 4.5f), *t7 = at(px, w, x_off + 300, y1 + lh * 6.5f);
    CHECK(memcmp(t5, t7, 3) != 0);                                /* the paused line differs from a plain one */
    CHECK(t5[0] >= t5[2] + 10);                                   /* warm: the warning tone */
    const unsigned char *ar = at(px, w, 39, y1 + lh * 4.5f);
    CHECK(ar[0] > ar[2] + 60 && ar[1] > ar[2] + 60 && ar[1] * 2 > ar[0]);   /* the arrow: amber (much green), not red */
    free(px);

    /* the lane off again: the old gutter, glyphs and presses ignored */
    c.lane = false;
    c.clicked = false;
    run(ui, &c, 2);
    cui_code_editor_goto(ui, "ed", 1, 1);
    run(ui, &c, 2);
    CHECK(c.r.caret_x > x_off - 0.5f && c.r.caret_x < x_off + 0.5f);
    press(ui, &c, 39, y1 + lh * 1.5f, CUI_MOUSE_LEFT);
    CHECK(!c.clicked);
    px = pixels(ui, &w);
    CHECK(!reddish(at(px, w, 39, y1 + lh * 1.5f)));
    free(px);
    cui_destroy(ui);
}

static void test_wrap(void) {
    cui_ctx *ui = new_ctx(false);
    ed c;
    memset(&c, 0, sizeof c);
    c.lane = c.wrap = true;
    snprintf(c.buf, sizeof c.buf, "short\n%s\nend\n",
             "a long line that wraps around the editor's width several times over, again and again, and still goes on and on");
    run(ui, &c, 3);
    float y1 = c.r.caret_y, lh = c.r.caret_h;
    press(ui, &c, 39, y1 + lh * 2.5f, CUI_MOUSE_LEFT);            /* the second row of line 2 */
    CHECK(c.clicked && c.line == 2);
    c.clicked = false;
    c.exec = 2;
    static const cui_gutter_glyph g[] = { { 2, CUI_GLYPH_BREAKPOINT_CONDITION } };
    c.gg = g;
    c.ng = 1;
    run(ui, &c, 2);
    int w;
    unsigned char *px = pixels(ui, &w);
    const unsigned char *r2 = at(px, w, 300, y1 + lh * 2.5f), *r0 = at(px, w, 300, y1 + lh * 0.5f);
    CHECK(memcmp(r2, r0, 3) != 0);                                /* every row of the paused line is tinted */
    free(px);
    cui_destroy(ui);
}

static void shot(const char *path) {
    cui_ctx *ui = new_ctx(true);
    cui_set_viewport(ui, 520, 300, 1);
    ed c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "%s", TEXT);
    static const cui_gutter_glyph g[] = { { 3, CUI_GLYPH_BREAKPOINT_UNVERIFIED }, { 4, CUI_GLYPH_BREAKPOINT },
                                          { 5, CUI_GLYPH_BREAKPOINT_CONDITION }, { 6, CUI_GLYPH_BREAKPOINT_LOG },
                                          { 7, CUI_GLYPH_BREAKPOINT_DISABLED } };
    c.lane = true;
    c.gg = g;
    c.ng = 5;
    c.exec = 5;
    run(ui, &c, 3);
    cui_input_mouse_move(ui, 39, c.r.caret_y + c.r.caret_h * 1.5f);   /* a ghost on line 2 */
    run(ui, &c, 3);
    int w;
    unsigned char *px = pixels(ui, &w);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--shot")) { shot(argv[2]); cui_sw_destroy(g_sw); return 0; }
    test_lane();
    test_wrap();
    cui_sw_destroy(g_sw);
    printf("test_gutter: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
