/* test_editor_multi.c - the code editor's multiple cursors and column selection (cui_code_editor_opts.multi_cursor):
 * adding cursors (Alt+click, Ctrl+Alt+Up/Down, Ctrl+D, Ctrl+Shift+L, cui_code_editor_add_selection), column selection
 * (Alt+Shift+arrows and Alt+Shift+drag, over short lines, tabs and UTF-8), every key at every cursor (typing,
 * Backspace, Delete, Enter with markdown lists, Tab, Shift+Tab, Home, End, word moves), cut, copy and paste (one line
 * per cursor when the counts match), undo and redo as one step with the carets back, cursors that meet becoming one,
 * Esc, a buffer and a document giving the same, the caps, and nothing changing without the option.
 * Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a), *b_ = (b); if (strcmp(a_, b_)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n    got      \"%s\"\n    expected \"%s\"\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static cui_sw_renderer *g_sw;
static char g_clip[4096];
static void clip_set(void *u, const char *s) { (void)u; snprintf(g_clip, sizeof g_clip, "%s", s); }
static const char *clip_get(void *u) { (void)u; return g_clip; }

typedef struct {
    char buf[1 << 16];
    cui_text_doc *doc;          /* or a document */
    const char *lang;
    bool multi, wrap, ro;
    cui_code_editor_result r;
    float x0, y0;               /* where the editor is */
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->doc ? NULL : c->buf, c->doc ? 0 : sizeof c->buf, .doc = c->doc, .lang = c->lang ? c->lang : "text",
                               .height = 400, .width = CUI_PX(700), .autofocus = true, .multi_cursor = c->multi, .wrap = c->wrap,
                               .tab_size = 4, .read_only = c->ro);
    }
}

static void run(cui_ctx *ui, ed *c, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, c); cui_end_frame(ui); }
}

static void key(cui_ctx *ui, ed *c, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    run(ui, c, 1);
    cui_input_key(ui, k, false, mods);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    run(ui, c, 1);
}

static void type(cui_ctx *ui, ed *c, const char *s) {
    cui_input_text(ui, s);
    run(ui, c, 1);
}

static const char *text(ed *c) {
    static char out[1 << 20];
    if (!c->doc) return c->buf;
    int n = cui_text_doc_read(c->doc, 0, cui_text_doc_length(c->doc), out);
    out[n] = 0;
    return out;
}

/* the selections as "a-c a-c ..." in order of position */
static const char *sels(cui_ctx *ui) {
    static char out[4096];
    int an[256], cu[256];
    int n = cui_code_editor_selections(ui, "ed", an, cu, 256), k = 0;
    out[0] = 0;
    for (int i = 0; i < n && i < 256 && k < (int)sizeof out - 24; i++) k += snprintf(out + k, sizeof out - (size_t)k, i ? " %d-%d" : "%d-%d", an[i], cu[i]);
    return out;
}

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 800, 500, 1);
    cui_set_platform(ui, &(cui_platform_iface){ .set_clipboard = clip_set, .get_clipboard = clip_get });
    cui_set_reduced_motion(ui, true);
    return ui;
}

static void set_text(ed *c, const char *s) {
    if (c->doc) cui_text_doc_replace(c->doc, 0, cui_text_doc_length(c->doc), s, -1);
    else snprintf(c->buf, sizeof c->buf, "%s", s);
}

/* a fresh editor on s (a document with doc), caret at `at` */
static cui_ctx *open_ed(ed *c, const char *s, bool doc, const char *lang, int at) {
    memset(c, 0, sizeof *c);
    c->multi = true;
    c->lang = lang;
    if (doc) c->doc = cui_text_doc_new(s, -1);
    else snprintf(c->buf, sizeof c->buf, "%s", s);
    cui_ctx *ui = new_ctx();
    run(ui, c, 3);
    cui_code_editor_select(ui, "ed", at, at);
    run(ui, c, 2);
    return ui;
}
static void close_ed(cui_ctx *ui, ed *c) {
    cui_destroy(ui);
    if (c->doc) cui_text_doc_free(c->doc);
    c->doc = NULL;
}

static void carets(cui_ctx *ui, ed *c, const int *at, int n) {   /* at[0] is the primary one, the others added */
    cui_code_editor_select(ui, "ed", at[0], at[0]);
    run(ui, c, 1);
    for (int i = 1; i < n; i++) cui_code_editor_add_selection(ui, "ed", at[i], at[i]);
    run(ui, c, 1);
}

/* the point of line `line`, column `col` (from 0), from the primary caret's box: put it at (0, 0) and (0, 1) first */
typedef struct { float x0, y0, cw, lh; } geo;
static geo measure(cui_ctx *ui, ed *c) {
    geo g;
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    g.x0 = c->r.caret_x; g.y0 = c->r.caret_y; g.lh = c->r.caret_h;
    cui_code_editor_select(ui, "ed", 1, 1);
    run(ui, c, 2);
    g.cw = c->r.caret_x - g.x0;
    return g;
}
static void click(cui_ctx *ui, ed *c, geo g, int line, double col, int mods) {
    float x = g.x0 + (float)col * g.cw + 0.5f, y = g.y0 + (float)line * g.lh + g.lh * 0.5f;
    cui_input_mouse_move(ui, x, y);
    run(ui, c, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, mods);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, c, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    run(ui, c, 40);   /* not a double click with the next one */
}

enum { P = CUI_MOD_PRIMARY, A = CUI_MOD_ALT, S = CUI_MOD_SHIFT };

static void t_alt_click(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "alpha\nbeta\ngamma\n", false, NULL, 0);
    geo g = measure(ui, &c);
    click(ui, &c, g, 0, 2, 0);
    CHECK_STR(sels(ui), "2-2");
    click(ui, &c, g, 2, 3, A);
    CHECK(c.r.cursor_count == 2);
    CHECK_STR(sels(ui), "2-2 14-14");
    click(ui, &c, g, 1, 1, A);
    CHECK_STR(sels(ui), "2-2 7-7 14-14");
    type(ui, &c, "#");
    CHECK_STR(text(&c), "al#pha\nb#eta\ngam#ma\n");
    CHECK_STR(sels(ui), "3-3 9-9 17-17");
    click(ui, &c, g, 1, 2, A);   /* Alt+click on a caret: it goes */
    CHECK_STR(sels(ui), "3-3 17-17");
    click(ui, &c, g, 1, 0, 0);   /* a plain click: one caret */
    CHECK_STR(sels(ui), "7-7");
    CHECK(c.r.cursor_count == 1);
    close_ed(ui, &c);
}

static void t_lines_and_undo(bool doc) {
    ed c;
    cui_ctx *ui = open_ed(&c, "one two\nab\nthree four\nfive\n", doc, NULL, 2);
    key(ui, &c, CUI_KEY_DOWN, P | A);
    key(ui, &c, CUI_KEY_DOWN, P | A);
    CHECK(c.r.cursor_count == 3);
    CHECK_STR(sels(ui), "2-2 10-10 13-13");   /* "ab" is short: its end; the next line is back at column 2 */
    CHECK(c.r.line == 3);                      /* the last one added is the primary one */
    type(ui, &c, "X");
    type(ui, &c, "Y");
    CHECK_STR(text(&c), "onXYe two\nabXY\nthXYree four\nfive\n");
    key(ui, &c, CUI_KEY_Z, P);   /* both keys: one step */
    CHECK_STR(text(&c), "one two\nab\nthree four\nfive\n");
    CHECK_STR(sels(ui), "2-2 10-10 13-13");   /* the carets are back */
    key(ui, &c, CUI_KEY_Y, P);
    CHECK_STR(text(&c), "onXYe two\nabXY\nthXYree four\nfive\n");
    CHECK_STR(sels(ui), "4-4 14-14 19-19");
    key(ui, &c, CUI_KEY_Z, P);
    key(ui, &c, CUI_KEY_UP, P | A);            /* above the first */
    CHECK(c.r.cursor_count == 1 || c.r.cursor_count == 3);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    CHECK(c.r.cursor_count == 1);
    /* Backspace, Delete and a run of keys */
    int at[] = { 3, 9, 16 };   /* after "one", in "a|b", after "three" */
    carets(ui, &c, at, 3);
    key(ui, &c, CUI_KEY_BACKSPACE, 0);
    CHECK_STR(text(&c), "on two\nb\nthre four\nfive\n");
    key(ui, &c, CUI_KEY_DELETE, 0);
    CHECK_STR(text(&c), "ontwo\n\nthrefour\nfive\n");
    key(ui, &c, CUI_KEY_Z, P);   /* deleting on: one step for both keys */
    CHECK_STR(text(&c), "one two\nab\nthree four\nfive\n");
    close_ed(ui, &c);
}

static void t_moves(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "  foo bar\n  baz qux\n", false, NULL, 6);
    int at[] = { 6, 16 };
    carets(ui, &c, at, 2);
    key(ui, &c, CUI_KEY_RIGHT, P);   /* word moves */
    CHECK_STR(sels(ui), "9-9 19-19");
    key(ui, &c, CUI_KEY_HOME, 0);    /* the first character, then column 0 */
    CHECK_STR(sels(ui), "2-2 12-12");
    key(ui, &c, CUI_KEY_HOME, 0);
    CHECK_STR(sels(ui), "0-0 10-10");
    key(ui, &c, CUI_KEY_END, S);
    CHECK_STR(sels(ui), "0-9 10-19");
    key(ui, &c, CUI_KEY_LEFT, 0);    /* a selection collapses to its start */
    CHECK_STR(sels(ui), "0-0 10-10");
    key(ui, &c, CUI_KEY_DOWN, 0);    /* the second one runs off the end: at the end of the text */
    CHECK_STR(sels(ui), "10-10 20-20");
    key(ui, &c, CUI_KEY_HOME, P);    /* both to the start: one */
    CHECK_STR(sels(ui), "0-0");
    CHECK(c.r.cursor_count == 1);
    close_ed(ui, &c);
}

static void t_merge(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "abcd\n", false, NULL, 2);
    int at[] = { 2, 3 };
    carets(ui, &c, at, 2);
    key(ui, &c, CUI_KEY_BACKSPACE, 0);   /* each takes its character; they meet */
    CHECK_STR(text(&c), "ad\n");
    CHECK_STR(sels(ui), "1-1");
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), "abcd\n");
    /* selections that overlap become one */
    cui_code_editor_select(ui, "ed", 0, 2);
    run(ui, &c, 1);
    cui_code_editor_add_selection(ui, "ed", 1, 3);
    run(ui, &c, 1);
    CHECK_STR(sels(ui), "0-3");
    /* a caret inside a selection goes into it */
    cui_code_editor_add_selection(ui, "ed", 2, 2);
    run(ui, &c, 1);
    CHECK(c.r.cursor_count == 1);
    close_ed(ui, &c);
}

static void t_ctrl_d(bool doc) {
    ed c;
    cui_ctx *ui = open_ed(&c, "foo bar food foo\nbaz foo", doc, NULL, 1);
    key(ui, &c, CUI_KEY_D, P);   /* the word */
    CHECK_STR(sels(ui), "0-3");
    key(ui, &c, CUI_KEY_D, P);   /* whole words: not "food" */
    CHECK_STR(sels(ui), "0-3 13-16");
    key(ui, &c, CUI_KEY_D, P);
    CHECK_STR(sels(ui), "0-3 13-16 21-24");
    key(ui, &c, CUI_KEY_D, P);   /* around: all taken */
    CHECK(c.r.cursor_count == 3);
    type(ui, &c, "q");
    CHECK_STR(text(&c), "q bar food q\nbaz q");
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), "foo bar food foo\nbaz foo");
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    /* from a selection: any match, around the end */
    cui_code_editor_select(ui, "ed", 13, 15);   /* "fo" */
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_D, P);
    CHECK_STR(sels(ui), "13-15 21-23");
    key(ui, &c, CUI_KEY_D, P);
    CHECK_STR(sels(ui), "0-2 13-15 21-23");
    key(ui, &c, CUI_KEY_D, P);
    CHECK_STR(sels(ui), "0-2 8-10 13-15 21-23");
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    CHECK_STR(sels(ui), "8-10");   /* the primary one stays: the last added */
    /* every match */
    cui_code_editor_select(ui, "ed", 5, 5);   /* in "bar": one match */
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_L, P | S);
    CHECK_STR(sels(ui), "4-7");
    cui_code_editor_select(ui, "ed", 14, 14);   /* in "foo" */
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_L, P | S);
    CHECK_STR(sels(ui), "0-3 13-16 21-24");
    CHECK(c.r.anchor == 13 && c.r.cursor == 16);   /* the one it started at stays the primary one */
    close_ed(ui, &c);
}

static void t_column_keys(void) {
    ed c;
    /* line 0 "abcdef", 1 "ab" (short), 2 "\tcdefgh" (a tab to column 4), 3 "aé漢bcd" (UTF-8: a column a code point) */
    const char *s = "abcdef\nab\n\tcdefgh\na\xC3\xA9\xE6\xBC\xA2" "bcd\n";
    cui_ctx *ui = open_ed(&c, s, false, NULL, 1);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    CHECK_STR(sels(ui), "1-1 8-8 10-10 19-19");   /* carets down column 1 (the tab line: column 1 is nearer its start) */
    key(ui, &c, CUI_KEY_RIGHT, A | S);
    key(ui, &c, CUI_KEY_RIGHT, A | S);
    key(ui, &c, CUI_KEY_RIGHT, A | S);
    /* columns 1 to 4: "bcd", "b" (the line ends at 2), the tab, "é漢b" */
    CHECK_STR(sels(ui), "1-4 8-9 10-11 19-25");
    key(ui, &c, CUI_KEY_RIGHT, A | S);
    key(ui, &c, CUI_KEY_LEFT, A | S);
    CHECK(c.r.cursor_count == 4);
    type(ui, &c, "_");
    CHECK_STR(text(&c), "a_ef\na_\n_cdefgh\na_cd\n");
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), s);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    /* a column right of the short line leaves it out */
    cui_code_editor_select(ui, "ed", 3, 3);
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_RIGHT, A | S);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    CHECK_STR(sels(ui), "3-4 11-11");   /* columns 3 to 4: line 1 ends at column 2, out; on the tab line both are after the tab */
    close_ed(ui, &c);
}

static void t_column_mouse(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "0123456\n0123\n0123456\n", false, NULL, 0);
    geo g = measure(ui, &c);
    float x = g.x0 + 1 * g.cw + 0.5f, y = g.y0 + g.lh * 0.5f;
    cui_input_mouse_move(ui, x, y);
    run(ui, &c, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, A | S);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, &c, 1);
    for (int k = 1; k <= 8; k++) {   /* down to line 2, column 5 */
        cui_input_mouse_move(ui, x + (float)k * 0.5f * g.cw, y + (float)k * 0.25f * g.lh);
        run(ui, &c, 1);
    }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    run(ui, &c, 2);
    CHECK_STR(sels(ui), "1-5 9-12 14-18");   /* the short line to its end */
    close_ed(ui, &c);
}

static void t_clipboard(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "ab\ncd\nef\n", false, NULL, 0);
    int at[] = { 0, 3, 6 };
    carets(ui, &c, at, 3);
    snprintf(g_clip, sizeof g_clip, "1\n2\n3\n");   /* as many lines as carets: one each */
    key(ui, &c, CUI_KEY_V, P);
    CHECK_STR(text(&c), "1ab\n2cd\n3ef\n");
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), "ab\ncd\nef\n");
    snprintf(g_clip, sizeof g_clip, "x\ny");   /* two lines, three carets: all of it at each */
    key(ui, &c, CUI_KEY_V, P);
    CHECK_STR(text(&c), "x\nyab\nx\nycd\nx\nyef\n");
    key(ui, &c, CUI_KEY_Z, P);
    /* copy of three selections, paste of them at three carets: each its own */
    cui_code_editor_select(ui, "ed", 0, 1);
    run(ui, &c, 1);
    cui_code_editor_add_selection(ui, "ed", 3, 5);
    cui_code_editor_add_selection(ui, "ed", 7, 8);
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_C, P);
    CHECK_STR(g_clip, "a\ncd\nf");
    key(ui, &c, CUI_KEY_END, 0);
    key(ui, &c, CUI_KEY_V, P);
    CHECK_STR(text(&c), "aba\ncdcd\neff\n");
    key(ui, &c, CUI_KEY_Z, P);
    /* cut of carets: the whole lines, a line once */
    int at2[] = { 0, 1, 7 };
    carets(ui, &c, at2, 3);
    key(ui, &c, CUI_KEY_X, P);
    CHECK_STR(g_clip, "ab\nef\n");
    CHECK_STR(text(&c), "cd\n");
    CHECK_STR(sels(ui), "0-0 3-3");
    key(ui, &c, CUI_KEY_V, P);   /* the two lines back, one at each */
    CHECK_STR(text(&c), "ab\ncd\nef\n");
    close_ed(ui, &c);
}

static void t_enter_tab(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "- a\n- b\n", false, "markdown", 3);
    int at[] = { 3, 7 };
    carets(ui, &c, at, 2);
    key(ui, &c, CUI_KEY_ENTER, 0);   /* the next item at each */
    CHECK_STR(text(&c), "- a\n- \n- b\n- \n");
    type(ui, &c, "x");
    CHECK_STR(text(&c), "- a\n- x\n- b\n- x\n");
    key(ui, &c, CUI_KEY_TAB, 0);     /* list items indent as lines */
    CHECK_STR(text(&c), "- a\n    - x\n- b\n    - x\n");
    key(ui, &c, CUI_KEY_TAB, S);
    CHECK_STR(text(&c), "- a\n- x\n- b\n- x\n");
    close_ed(ui, &c);
    ui = open_ed(&c, "int f() {\n    a;\n    b;\n}\n", false, "c", 0);
    int at2[] = { 16, 23 };   /* after "a;" and "b;" */
    carets(ui, &c, at2, 2);
    key(ui, &c, CUI_KEY_ENTER, 0);   /* the indentation of each line */
    CHECK_STR(text(&c), "int f() {\n    a;\n    \n    b;\n    \n}\n");
    type(ui, &c, "c;");
    CHECK_STR(text(&c), "int f() {\n    a;\n    c;\n    b;\n    c;\n}\n");
    /* Tab at carets: spaces to the next stop at each; two carets on one line outdent it once */
    int at3[] = { 10, 12 };
    carets(ui, &c, at3, 2);
    key(ui, &c, CUI_KEY_TAB, S);
    CHECK_STR(text(&c), "int f() {\na;\n    c;\n    b;\n    c;\n}\n");
    CHECK_STR(sels(ui), "10-10");   /* both were in the indentation taken away */
    int at4[] = { 10, 11 };
    carets(ui, &c, at4, 2);
    key(ui, &c, CUI_KEY_TAB, 0);
    CHECK_STR(text(&c), "int f() {\n    a   ;\n    c;\n    b;\n    c;\n}\n");
    close_ed(ui, &c);
}

static void t_utf8(bool doc) {
    ed c;
    const char *s = "\xE6\xBC\xA2\xE5\xAD\x97 x\n\xC3\xA9t\xC3\xA9 y\n";   /* "漢字 x", "été y" */
    cui_ctx *ui = open_ed(&c, s, doc, NULL, 0);
    key(ui, &c, CUI_KEY_DOWN, P | A);
    key(ui, &c, CUI_KEY_RIGHT, 0);   /* a code point at each */
    CHECK_STR(sels(ui), "3-3 11-11");
    key(ui, &c, CUI_KEY_RIGHT, S);
    type(ui, &c, "-");
    CHECK_STR(text(&c), "\xE6\xBC\xA2- x\n\xC3\xA9-\xC3\xA9 y\n");
    key(ui, &c, CUI_KEY_BACKSPACE, 0);
    key(ui, &c, CUI_KEY_BACKSPACE, 0);
    CHECK_STR(text(&c), " x\n\xC3\xA9 y\n");
    CHECK_STR(sels(ui), "0-0 3-3");
    key(ui, &c, CUI_KEY_Z, P);   /* the two Backspaces, then the typing */
    CHECK_STR(text(&c), "\xE6\xBC\xA2- x\n\xC3\xA9-\xC3\xA9 y\n");
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), s);
    close_ed(ui, &c);
}

/* the same keys on a buffer and on a document: the same text and selections after each */
static void t_doc_same(void) {
    ed a, b;
    const char *s = "fn a() {\n  x = 1;\n}\nfn b() {\n  x = 2;\n}\n";
    cui_ctx *ua = open_ed(&a, s, false, "c", 11);   /* on the first "x" */
    cui_ctx *ub = open_ed(&b, s, true, "c", 11);
    struct { cui_key k; int mods; const char *t; } steps[] = {
        { CUI_KEY_L, P | S, NULL }, { CUI_KEY_NONE, 0, "yy" }, { CUI_KEY_LEFT, P | S, NULL }, { CUI_KEY_NONE, 0, "(" },
        { CUI_KEY_END, 0, NULL }, { CUI_KEY_ENTER, 0, NULL }, { CUI_KEY_NONE, 0, "z;" }, { CUI_KEY_UP, 0, NULL },
        { CUI_KEY_BACKSPACE, P, NULL }, { CUI_KEY_Z, P, NULL }, { CUI_KEY_Z, P, NULL }, { CUI_KEY_Y, P, NULL },
        { CUI_KEY_DELETE, 0, NULL }, { CUI_KEY_TAB, 0, NULL }, { CUI_KEY_TAB, S, NULL }, { CUI_KEY_X, P, NULL },
        { CUI_KEY_V, P, NULL }, { CUI_KEY_Z, P, NULL },
    };
    for (int i = 0; i < (int)(sizeof steps / sizeof steps[0]); i++) {
        if (steps[i].t) { type(ua, &a, steps[i].t); type(ub, &b, steps[i].t); }
        else { key(ua, &a, steps[i].k, steps[i].mods); key(ub, &b, steps[i].k, steps[i].mods); }
        char sa[4096];
        snprintf(sa, sizeof sa, "%s", sels(ua));
        CHECK_STR(text(&b), text(&a));
        CHECK_STR(sels(ub), sa);
    }
    CHECK(a.r.cursor_count == 2);
    close_ed(ua, &a);
    close_ed(ub, &b);
}

/* many cursors on a big document: Ctrl+Shift+L is capped, every one types */
static void t_many(void) {
    ed c;
    int lines = 12000;
    char *s = malloc((size_t)lines * 7 + 1);
    for (int i = 0; i < lines; i++) memcpy(s + i * 7, "a key;\n", 7);
    s[lines * 7] = 0;
    cui_ctx *ui = open_ed(&c, s, true, NULL, 3);
    key(ui, &c, CUI_KEY_L, P | S);   /* 12 000 matches: the first 10 000 from the caret on */
    CHECK(c.r.cursor_count == 10000);
    type(ui, &c, "Z");               /* "key" becomes "Z" at each */
    CHECK(c.r.length == lines * 7 - 2 * 10000);
    CHECK(c.r.line == 1);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    CHECK(c.r.cursor_count == 1);
    key(ui, &c, CUI_KEY_Z, P);       /* one step, even past the document's splice log */
    CHECK(c.r.length == lines * 7);
    CHECK_STR(text(&c), s);
    close_ed(ui, &c);
    free(s);
}

/* wrapped lines: Up and Down by rows at each caret; the app's edits of the document move the other cursors; a read
 * only editor selects but does not type; undo after Esc brings every caret back; a long run of typing is cut into
 * steps of at most 200 edits */
static void t_more(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "short\nshort\n", true, NULL, 1);
    int at[] = { 1, 7 };
    carets(ui, &c, at, 2);
    cui_text_doc_replace(c.doc, 0, 0, "new ", -1);   /* the app writes before both */
    run(ui, &c, 1);
    CHECK_STR(sels(ui), "5-5 11-11");
    cui_text_doc_replace(c.doc, 6, 6, "+", -1);      /* between them */
    run(ui, &c, 1);
    CHECK_STR(sels(ui), "5-5 12-12");
    type(ui, &c, "*");
    CHECK_STR(text(&c), "new s*h+ort\ns*hort\n");
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    CHECK(c.r.cursor_count == 1);
    key(ui, &c, CUI_KEY_Z, P);   /* the carets come back with the step */
    CHECK_STR(text(&c), "new sh+ort\nshort\n");
    CHECK(c.r.cursor_count == 2);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    /* a long run: 3 carets, 70 keys within a second are 210 edits, so two steps */
    int at2[] = { 0, 11, 17 };
    carets(ui, &c, at2, 3);
    for (int i = 0; i < 70; i++) { cui_input_text(ui, "k"); cui_begin_frame(ui, 0.004f); build(ui, &c); cui_end_frame(ui); }
    CHECK(c.r.length == 17 + 210);
    key(ui, &c, CUI_KEY_Z, P);
    CHECK(c.r.length > 17 && c.r.length < 17 + 210);
    key(ui, &c, CUI_KEY_Z, P);
    CHECK(c.r.length == 17);
    CHECK_STR(sels(ui), "0-0 11-11 17-17");
    close_ed(ui, &c);
    /* wrapped */
    char s[200];
    snprintf(s, sizeof s, "%s\n%s\n", "aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa",
             "bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb bbbbbbbbbb");
    memset(&c, 0, sizeof c);
    c.multi = true; c.wrap = true;
    snprintf(c.buf, sizeof c.buf, "%s", s);
    ui = new_ctx();
    run(ui, &c, 3);
    int at3[] = { 2, 112 };
    carets(ui, &c, at3, 2);
    key(ui, &c, CUI_KEY_DOWN, 0);   /* a row down: still in its own line, further on */
    int an[4], cu[4];
    CHECK(cui_code_editor_selections(ui, "ed", an, cu, 4) == 2);
    CHECK(cu[0] > 2 && cu[0] < 110 && cu[1] > 112 && cu[1] < 220);
    key(ui, &c, CUI_KEY_UP, 0);
    CHECK_STR(sels(ui), "2-2 112-112");
    cui_destroy(ui);
    /* read only */
    ui = open_ed(&c, "ab ab ab", false, NULL, 0);
    c.ro = true;
    run(ui, &c, 1);
    key(ui, &c, CUI_KEY_L, P | S);
    CHECK_STR(sels(ui), "0-2 3-5 6-8");
    type(ui, &c, "x");
    key(ui, &c, CUI_KEY_BACKSPACE, 0);
    CHECK_STR(text(&c), "ab ab ab");
    key(ui, &c, CUI_KEY_C, P);
    CHECK_STR(g_clip, "ab\nab\nab");
    key(ui, &c, CUI_KEY_X, P);
    CHECK_STR(text(&c), "ab ab ab");
    close_ed(ui, &c);
}

/* without .multi_cursor the keys do what they always did */
static void t_off(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, "foo foo\nbar\n", false, NULL, 1);
    c.multi = false;
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_D, P);
    CHECK_STR(sels(ui), "1-1");
    key(ui, &c, CUI_KEY_DOWN, P | A);   /* Ctrl+Down: the old motion */
    CHECK(c.r.cursor_count == 1);
    geo g = measure(ui, &c);
    click(ui, &c, g, 1, 2, A);   /* Alt+click: the caret moves */
    CHECK_STR(sels(ui), "10-10");
    cui_code_editor_add_selection(ui, "ed", 0, 0);   /* ignored */
    run(ui, &c, 1);
    CHECK(c.r.cursor_count == 1);
    close_ed(ui, &c);
}

int main(void) {
    g_sw = cui_sw_create();
    t_alt_click();
    t_lines_and_undo(false);
    t_lines_and_undo(true);
    t_moves();
    t_merge();
    t_ctrl_d(false);
    t_ctrl_d(true);
    t_column_keys();
    t_column_mouse();
    t_clipboard();
    t_enter_tab();
    t_utf8(false);
    t_utf8(true);
    t_doc_same();
    t_many();
    t_more();
    t_off();
    cui_sw_destroy(g_sw);
    printf("test_editor_multi: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
