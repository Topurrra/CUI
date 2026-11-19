/* test_ghost.c - the code editor's ghost text (an inline suggestion): drawn at the caret, Tab takes all of it,
 * Ctrl+Right a word, Esc drops it; with a buffer and a document, one line and several, wrapped or not.
 * Headless: the software renderer. --shot <file.png> [light] draws an editor with a multi-line ghost for a look. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if defined(__APPLE__)
#define WORD_MOD CUI_MOD_ALT
#else
#define WORD_MOD CUI_MOD_CTRL
#endif

static cui_sw_renderer *g_sw;

typedef struct {
    char buf[1024];
    cui_text_doc *doc;
    bool wrap, ro;
    const char *ghost;
    int ghost_at;
    cui_code_editor_result r;
    int taken, dismissed, changed;
    int splice_start, splice_inserted;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->doc ? NULL : c->buf, c->doc ? 0 : sizeof c->buf, .lang = "c", .height = 300,
                               .width = CUI_PX(400), .wrap = c->wrap, .read_only = c->ro, .autofocus = true, .doc = c->doc,
                               .ghost = c->ghost, .ghost_at = c->ghost_at);
    }
    c->taken += c->r.ghost_taken;
    c->dismissed += c->r.ghost_dismissed;
    if (c->r.changed) { c->changed++; c->splice_start = c->r.splice_start; c->splice_inserted = c->r.splice_inserted; }
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

static void key(cui_ctx *ui, ed *c, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    run(ui, c, 1);
    cui_input_key(ui, k, false, 0);
    run(ui, c, 1);
}

static void click(cui_ctx *ui, ed *c, float x, float y) {
    cui_input_mouse_move(ui, x, y);
    run(ui, c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, c, 1);
}

static void caret_to(cui_ctx *ui, ed *c, int at) {
    cui_code_editor_select(ui, "ed", at, at);
    run(ui, c, 2);
}

static void reset(ed *c) { c->taken = c->dismissed = c->changed = 0; c->splice_start = -1; c->splice_inserted = 0; }

/* ink: some pixel of the cell at (x, y) (its middle half) differs from the background beside the editor's right edge */
static bool ink(cui_ctx *ui, float x, float y, float w, float h) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    int W = dd->width_px;
    const unsigned char *bg = px + ((size_t)(int)(y + h * 0.5f) * (size_t)W + 395) * 4;   /* the same row, far right: no text there */
    bool found = false;
    for (int yy = (int)(y + h * 0.25f); yy < (int)(y + h * 0.75f) && !found; yy++)
        for (int xx = (int)x; xx < (int)(x + w) && !found; xx++) {
            const unsigned char *p = px + ((size_t)yy * (size_t)W + (size_t)xx) * 4;
            found = abs(p[0] - bg[0]) + abs(p[1] - bg[1]) + abs(p[2] - bg[2]) > 60;
        }
    free(px);
    return found;
}

static char *doc_text(cui_text_doc *d) {
    int n;
    return cui_text_doc_copy(d, 0, cui_text_doc_length(d), &n);
}

static const char TEXT[] = "int f(void) {\n    return \n}\n";

static void test_one_line(void) {
    cui_ctx *ui = new_ctx(true);
    ed c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "%s", TEXT);
    int at = (int)(strchr(TEXT, '\n') - TEXT) + 1 + 11;   /* after "    return " */
    run(ui, &c, 3);
    caret_to(ui, &c, 0);
    float x0 = c.r.caret_x, lh = c.r.caret_h;
    caret_to(ui, &c, 1);
    float cw = c.r.caret_x - x0;
    CHECK(cw > 4 && lh > 10);
    caret_to(ui, &c, at);
    float cx = c.r.caret_x, cy = c.r.caret_y;
    CHECK(c.r.line == 2 && c.r.column == 12);
    CHECK(!ink(ui, cx + 2, cy, cw * 3, lh));                 /* nothing after the caret yet */

    /* the ghost at the caret: drawn after it, the caret stays, the text unchanged */
    c.ghost = "42;";
    c.ghost_at = at;
    run(ui, &c, 2);
    CHECK(ink(ui, cx + 2, cy, cw * 3, lh));
    CHECK(c.r.caret_x == cx && c.r.cursor == at && !c.r.changed);
    CHECK(!strcmp(c.buf, TEXT));

    /* Tab: all of it, verbatim; reported as taken and as an edit */
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 3 && c.changed == 1);
    CHECK(c.splice_start == at && c.splice_inserted == 3);
    CHECK(!strcmp(c.buf, "int f(void) {\n    return 42;\n}\n"));
    CHECK(c.r.cursor == at + 3 && c.r.anchor == at + 3);
    CHECK(c.r.focused);
    /* one undo step takes it out */
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(!strcmp(c.buf, TEXT));
    CHECK(c.r.cursor == at);

    /* Ctrl+Right: up to the end of the next word */
    c.ghost = "foo(bar, baz);";
    reset(&c);
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_RIGHT, WORD_MOD);
    CHECK(c.taken == 3);
    CHECK(!strcmp(c.buf, "int f(void) {\n    return foo\n}\n"));
    CHECK(c.r.cursor == at + 3);
    /* the app hands on the rest at the new caret: the punctuation, then white space and a word */
    c.ghost = "(bar, baz);";
    c.ghost_at = at + 3;
    reset(&c);
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_RIGHT, WORD_MOD);
    CHECK(c.taken == 1);
    c.ghost = " baz);";
    c.ghost_at = at + 4;
    key(ui, &c, CUI_KEY_RIGHT, WORD_MOD);
    CHECK(c.taken == 5);
    CHECK(!strcmp(c.buf, "int f(void) {\n    return foo( baz\n}\n"));
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);                  /* each a step of its own */
    CHECK(!strcmp(c.buf, "int f(void) {\n    return foo(\n}\n"));
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(!strcmp(c.buf, TEXT));

    /* Esc: dropped, the text and caret as they were, the editor keeps the focus */
    c.ghost = "42;";
    c.ghost_at = at;
    caret_to(ui, &c, at);
    reset(&c);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    CHECK(c.dismissed == 1 && c.taken == 0 && c.changed == 0);
    CHECK(!strcmp(c.buf, TEXT) && c.r.cursor == at && c.r.focused);

    /* the caret elsewhere: not drawn, Tab indents as before */
    caret_to(ui, &c, 0);
    CHECK(!ink(ui, cx + 2, cy, cw * 3, lh));
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 0 && c.changed == 1);
    CHECK(!strncmp(c.buf, "    int f(void)", 15));
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(!strcmp(c.buf, TEXT));

    /* a selection hides it: Tab indents the line */
    cui_code_editor_select(ui, "ed", at - 3, at);
    run(ui, &c, 2);
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 0);
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(!strcmp(c.buf, TEXT));

    /* read only: not drawn, nothing taken */
    c.ro = true;
    caret_to(ui, &c, at);
    CHECK(!ink(ui, cx + 2, cy, cw * 3, lh));
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 0 && !strcmp(c.buf, TEXT));
    c.ro = false;

    /* no focus: not drawn */
    caret_to(ui, &c, at);
    CHECK(ink(ui, cx + 2, cy, cw * 3, lh));
    click(ui, &c, 590, 390);                                  /* away from the editor */
    CHECK(!c.r.focused);
    CHECK(!ink(ui, cx + 2, cy, cw * 3, lh));
    cui_destroy(ui);
}

static void test_doc(void) {
    cui_ctx *ui = new_ctx(false);
    ed c;
    memset(&c, 0, sizeof c);
    c.doc = cui_text_doc_new(TEXT, (int)strlen(TEXT));
    int at = (int)(strchr(TEXT, '\n') - TEXT) + 1 + 11;
    run(ui, &c, 3);
    caret_to(ui, &c, at);
    c.ghost = "42;\n    // done";
    c.ghost_at = at;
    run(ui, &c, 2);
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 15 && c.changed == 1);
    char *t = doc_text(c.doc);
    CHECK(t && !strcmp(t, "int f(void) {\n    return 42;\n    // done\n}\n"));   /* verbatim: no added indentation */
    free(t);
    CHECK(c.r.cursor == at + 15);
    c.ghost = NULL;
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    t = doc_text(c.doc);
    CHECK(t && !strcmp(t, TEXT));
    free(t);
    cui_destroy(ui);
    cui_text_doc_free(c.doc);
}

/* several lines: rows of their own under the caret's, the lines below pushed down, a click below them lands on the right line */
static void multi(bool wrap) {
    cui_ctx *ui = new_ctx(false);
    ed c;
    memset(&c, 0, sizeof c);
    c.wrap = wrap;
    snprintf(c.buf, sizeof c.buf, "x\nyyyy\nz\n");
    run(ui, &c, 3);
    caret_to(ui, &c, 0);
    float x0 = c.r.caret_x, y0 = c.r.caret_y, lh = c.r.caret_h;
    caret_to(ui, &c, 1);
    float cw = c.r.caret_x - x0;
    CHECK(ink(ui, x0, y0 + lh, cw, lh));                      /* "yyyy" on row 1 */
    c.ghost = "1\n  2\n  3";
    c.ghost_at = 1;
    run(ui, &c, 2);
    CHECK(ink(ui, x0 + cw + 1, y0, cw - 1, lh));              /* "1" after the caret */
    CHECK(!ink(ui, x0, y0 + lh, cw * 2, lh));                 /* row 1: the ghost's "  2", nothing in its first columns */
    CHECK(ink(ui, x0 + cw * 2, y0 + lh, cw, lh));             /* its "2" */
    CHECK(ink(ui, x0, y0 + lh * 3, cw, lh));                  /* "yyyy" pushed down to row 3 */
    CHECK(!ink(ui, x0 - 30, y0 + lh, 20, lh));                /* the gutter beside the ghost's rows: blank */
    CHECK(ink(ui, x0 - 30, y0 + lh * 3, 26, lh));             /* line 2's number moved down with it */
    CHECK(c.r.caret_y == y0 && c.r.line == 1);
    /* a click on "yyyy" where it is drawn: line 2 */
    click(ui, &c, x0 + cw * 2.2f, y0 + lh * 3.5f);
    CHECK(c.r.line == 2 && c.r.column == 3);
    CHECK(ink(ui, x0, y0 + lh, cw, lh));                      /* the caret moved away: the ghost is gone, "yyyy" back up */
    /* Tab inserts it all as it is */
    caret_to(ui, &c, 1);
    reset(&c);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK(c.taken == 9);
    CHECK(!strcmp(c.buf, "x1\n  2\n  3\nyyyy\nz\n"));
    CHECK(c.r.line == 3 && c.r.column == 4);
    key(ui, &c, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(!strcmp(c.buf, "x\nyyyy\nz\n"));
    cui_destroy(ui);
}

static void shot(const char *path, bool dark) {
    cui_ctx *ui = new_ctx(dark);
    cui_set_viewport(ui, 520, 300, 1);
    ed c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "#include <stdio.h>\n\nint sum(const int *v, int n) {\n    int s = 0;\n    for (int i = 0; i < n; i++) \n    return s;\n}\n");
    const char *p = strstr(c.buf, "i++) ") + 5;
    int at = (int)(p - c.buf);
    run(ui, &c, 3);
    caret_to(ui, &c, at);
    c.ghost = "{\n        s += v[i];\n    }";
    c.ghost_at = at;
    run(ui, &c, 3);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--shot")) { shot(argv[2], !(argc > 3 && !strcmp(argv[3], "light"))); cui_sw_destroy(g_sw); return 0; }
    test_one_line();
    test_doc();
    multi(false);
    multi(true);
    cui_sw_destroy(g_sw);
    printf("test_ghost: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
