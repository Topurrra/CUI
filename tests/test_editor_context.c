/* test_editor_context.c - the code editor reports a right press for the caller's context menu (context_clicked):
 * on the text with the byte under the pointer, in the gutter with -1, never in the glyph lane (that stays
 * gutter_right); a left press reports nothing; the caret does not move. Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;

typedef struct {
    char buf[256];
    bool lane, wrap;
    cui_code_editor_result r;
    int ctx, lane_right;
    bool gutter;
    int line, offset;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->buf, sizeof c->buf, .lang = "c", .height = 300, .width = CUI_PX(400),
                               .autofocus = true, .glyph_lane = c->lane, .wrap = c->wrap);
    }
    if (c->r.context_clicked) { c->ctx++; c->gutter = c->r.context_gutter; c->line = c->r.context_line; c->offset = c->r.context_offset; }
    if (c->r.gutter_clicked && c->r.gutter_right) c->lane_right++;
}

static void run(cui_ctx *ui, ed *c, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, c); cui_end_frame(ui); }
}

static void press(cui_ctx *ui, ed *c, cui_mouse_button b, float x, float y) {
    cui_input_mouse_move(ui, x, y);
    run(ui, c, 1);
    cui_input_mouse_button(ui, b, true);
    run(ui, c, 1);
    cui_input_mouse_button(ui, b, false);
    run(ui, c, 1);
}

static void one(bool lane, bool wrap) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 600, 400, 1);
    cui_set_reduced_motion(ui, true);
    ed c;
    memset(&c, 0, sizeof c);
    c.lane = lane;
    c.wrap = wrap;
    snprintf(c.buf, sizeof c.buf, "int a;\nint bb = 2;\nreturn 0;\n");
    run(ui, &c, 3);
    cui_code_editor_select(ui, "ed", 7, 7);   /* the caret at the start of line 2: its box says where line 2 is */
    run(ui, &c, 2);
    float y = c.r.caret_y + c.r.caret_h * 0.5f, x0 = c.r.caret_x;
    CHECK(c.r.line == 2);
    /* right press on the text, four columns in: the byte there */
    press(ui, &c, CUI_MOUSE_RIGHT, x0 + 4.5f * (c.r.caret_h * 0.45f), y);
    CHECK(c.ctx == 1);
    CHECK(!c.gutter);
    CHECK(c.line == 2);
    CHECK(c.offset > 7 && c.offset < 18);
    CHECK(c.r.cursor == 7 && c.r.anchor == 7);   /* the caret stays */
    /* right press on the line numbers */
    float gx = 20.0f + (lane ? 50.0f : 14.0f);
    if (gx < x0 - 4) {
        press(ui, &c, CUI_MOUSE_RIGHT, gx, y);
        CHECK(c.ctx == 2);
        CHECK(c.gutter);
        CHECK(c.line == 2);
        CHECK(c.offset == -1);
    }
    /* a left press: nothing reported */
    int before = c.ctx;
    press(ui, &c, CUI_MOUSE_LEFT, x0 + 20, y);
    CHECK(c.ctx == before);
    /* the glyph lane keeps its own right press */
    if (lane) {
        before = c.ctx;
        press(ui, &c, CUI_MOUSE_RIGHT, 20.0f + 17.0f, y);
        CHECK(c.lane_right == 1);
        CHECK(c.ctx == before);
    }
    /* below the text: the last line, if anything (as a left press puts the caret there) */
    before = c.ctx;
    press(ui, &c, CUI_MOUSE_RIGHT, x0 + 20, 280);
    CHECK(c.ctx == before || c.line == 4);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    one(false, false);
    one(true, false);
    one(true, true);
    cui_sw_destroy(g_sw);
    printf("test_editor_context: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
