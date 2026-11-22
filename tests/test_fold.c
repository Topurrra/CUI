/* test_fold.c - the code editor's folding (cui_code_editor_opts.folding) and minimap (.minimap): where blocks start and
 * end (by indentation and by markdown headings), the folded set and its rows with the hidden lines taken out, edits
 * around folds, and the editor driven headless (keys, presses, find, wrap, documents, the minimap's scrolling).
 * --shot <file.png> draws a dark editor with folds and the minimap for a look. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;

static cui_ctx *new_ctx(bool dark) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, 600, 400, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

static void fold_done(cui_ctx *ui, cui__fold *F) {
    cui__free(ui, F->h); cui__free(ui, F->e); cui__free(ui, F->va); cui__free(ui, F->vb); cui__free(ui, F->vp);
}

/* ---- the model, over an array of lines ---- */

typedef struct { const char *const *ln; int n; const int *st; } lines_src;
static const char *src_line(void *u, int i, int *ls, int *le) {
    const lines_src *s = u;
    if (i < 0 || i >= s->n) return NULL;
    *ls = 0; *le = (int)strlen(s->ln[i]);
    return s->ln[i];
}
static int src_state(void *u, int i) { const lines_src *s = u; return s->st ? s->st[i] : -1; }
static cui__lines lines_of(lines_src *s) { cui__lines L = { src_line, src_state, s, s->n, 4 }; return L; }

static const char *const PY[] = {
    "def a():",           /* 0: a block down to 6 */
    "    x = 1",          /* 1 */
    "    if x:",          /* 2: a block down to 5 */
    "        y = 2",      /* 3 */
    "",                   /* 4: blank, inside */
    "        z = 3",      /* 5 */
    "    return x",       /* 6 */
    "",                   /* 7 */
    "def b():",           /* 8: down to 9 (the blank after it is not part of it) */
    "\tpass",             /* 9: a tab is 4 columns */
    "",                   /* 10 */
};

static const char *const MD[] = {
    "# Title",            /* 0: to the end */
    "intro",              /* 1 */
    "## A",               /* 2: to 6 */
    "text a",             /* 3 */
    "```sh",              /* 4 */
    "# not a heading",    /* 5: code */
    "```",                /* 6 */
    "",                   /* 7 */
    "## B",               /* 8: to 10 */
    "### B1",             /* 9: to 10 */
    "b1 text",            /* 10 */
    "## C",               /* 11: nothing under it */
    "## D",               /* 12: to 13 */
    "d",                  /* 13 */
};

static void test_blocks(void) {
    lines_src s = { PY, CUI_COUNTOF(PY), NULL };
    cui__lines L = lines_of(&s);
    CHECK(cui__fold_starts(&L, false, 0) && cui__fold_end(&L, false, 0) == 6);
    CHECK(!cui__fold_starts(&L, false, 1) && cui__fold_end(&L, false, 1) == -1);
    CHECK(cui__fold_starts(&L, false, 2) && cui__fold_end(&L, false, 2) == 5);   /* past the blank line inside */
    CHECK(!cui__fold_starts(&L, false, 4) && !cui__fold_starts(&L, false, 7));   /* blank lines start nothing */
    CHECK(cui__fold_starts(&L, false, 8) && cui__fold_end(&L, false, 8) == 9);
    CHECK(!cui__fold_starts(&L, false, 9) && !cui__fold_starts(&L, false, 10));

    lines_src m = { MD, CUI_COUNTOF(MD), NULL };
    cui__lines M = lines_of(&m);
    CHECK(cui__fold_starts(&M, true, 0) && cui__fold_end(&M, true, 0) == 13);
    CHECK(!cui__fold_starts(&M, true, 1));
    CHECK(cui__fold_starts(&M, true, 2) && cui__fold_end(&M, true, 2) == 6);     /* the # in the fence is code; the blank is left out */
    CHECK(cui__fold_starts(&M, true, 8) && cui__fold_end(&M, true, 8) == 10);
    CHECK(cui__fold_starts(&M, true, 9) && cui__fold_end(&M, true, 9) == 10);
    CHECK(!cui__fold_starts(&M, true, 11) && cui__fold_end(&M, true, 11) == -1); /* a heading of its level right after it */
    CHECK(cui__fold_starts(&M, true, 12) && cui__fold_end(&M, true, 12) == 13);
    int st[CUI_COUNTOF(MD)] = { CUI__MD_START, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0 };   /* lines 5 and 6 in a fence, as the editor knows */
    m.st = st;
    CHECK(!cui__fold_starts(&M, true, 5));
    m.st = NULL;
    CHECK(cui__fold_starts(&M, true, 5));   /* (without the line states a line alone looks like a heading) */

    /* the innermost block around a line that is not folded yet */
    cui_ctx *ui = new_ctx(false);
    cui__fold F;
    memset(&F, 0, sizeof F);
    int e = 0;
    CHECK(cui__fold_around(&L, false, &F, 3, &e) == 2 && e == 5);
    CHECK(cui__fold_around(&L, false, &F, 2, &e) == 2 && e == 5);
    CHECK(cui__fold_around(&L, false, &F, 6, &e) == 0 && e == 6);
    CHECK(cui__fold_around(&L, false, &F, 7, &e) == -1);
    cui__fold_set(ui, &F, 2, 5);
    CHECK(cui__fold_around(&L, false, &F, 2, &e) == 0 && e == 6);   /* folded: the one around it */
    CHECK(cui__fold_around(&M, true, &F, 10, &e) == 9 && e == 10);
    CHECK(cui__fold_around(&M, true, &F, 9, &e) == 9);
    CHECK(cui__fold_around(&M, true, &F, 3, &e) == 0 && e == 13);   /* line 2 is folded here (F is shared): its parent */
    cui__fold_clear(&F);

    /* fold everything: nested blocks too, in header order */
    CHECK(cui__fold_all(ui, &F, &L, false) == 3);
    CHECK(F.n == 3 && F.h[0] == 0 && F.e[0] == 6 && F.h[1] == 2 && F.e[1] == 5 && F.h[2] == 8 && F.e[2] == 9);
    CHECK(F.vn == 2 && F.hidden == 7);
    cui__fold_clear(&F);
    CHECK(cui__fold_all(ui, &F, &M, true) == 5);
    CHECK(F.n == 5 && F.h[0] == 0 && F.e[0] == 13 && F.h[1] == 2 && F.e[1] == 6 && F.h[2] == 8 && F.h[3] == 9 && F.h[4] == 12);
    CHECK(F.hidden == 13);
    fold_done(ui, &F);
    cui_destroy(ui);
}

static void test_rows(void) {
    cui_ctx *ui = new_ctx(false);
    cui__fold F;
    memset(&F, 0, sizeof F);
    const int nl = 11;
    CHECK(cui__fold_row(&F, 5) == 5 && cui__fold_line(&F, 5) == 5 && !cui__fold_hidden(&F, 3));
    CHECK(cui__fold_set(ui, &F, 2, 5) && cui__fold_set(ui, &F, 8, 9));
    CHECK(!cui__fold_set(ui, &F, 2, 5));   /* no change */
    CHECK(F.hidden == 4 && F.vn == 2);
    CHECK(cui__fold_hidden(&F, 3) && cui__fold_hidden(&F, 5) && !cui__fold_hidden(&F, 2) && !cui__fold_hidden(&F, 6) && cui__fold_hidden(&F, 9));
    CHECK(cui__fold_row(&F, 6) == 3 && cui__fold_line(&F, 3) == 6);
    CHECK(cui__fold_row(&F, 4) == 3);               /* a hidden line: the row of the next line shown */
    CHECK(cui__fold_row(&F, 10) == 6 && cui__fold_line(&F, 6) == 10);
    CHECK(cui__fold_line(&F, 7) == 11);             /* past the end: on past the last line */
    CHECK(cui__fold_line(&F, -1) == -1);
    CHECK(cui__fold_next(&F, 3) == 6 && cui__fold_next(&F, 2) == 2 && cui__fold_prev(&F, 4) == 2 && cui__fold_prev(&F, 9) == 8);
    CHECK(cui__fold_gap(&F, 0) == 3 && cui__fold_gap(&F, 4) == 4 && cui__fold_gap(&F, 6) == 9 && cui__fold_gap(&F, 10) == INT_MAX);
    /* every line shown has its own row, in order, and every row its line */
    int row = 0, bad = 0;
    for (int line = 0; line < nl; line++) {
        if (cui__fold_hidden(&F, line)) continue;
        if (cui__fold_row(&F, line) != row || cui__fold_line(&F, row) != line) bad++;
        row++;
    }
    CHECK(bad == 0 && row == nl - F.hidden);

    /* nested: the outer fold hides the inner one's header too */
    CHECK(cui__fold_set(ui, &F, 0, 6));
    CHECK(F.vn == 2 && F.va[0] == 1 && F.vb[0] == 6 && F.hidden == 7);
    CHECK(cui__fold_line(&F, 1) == 7 && cui__fold_line(&F, 2) == 8 && cui__fold_line(&F, 3) == 10);
    CHECK(cui__fold_unset(ui, &F, 0) && F.hidden == 4);   /* the inner fold is still there */
    CHECK(!cui__fold_unset(ui, &F, 1));

    /* the caret goes inside: the folds around it open */
    CHECK(cui__fold_set(ui, &F, 0, 6));
    CHECK(cui__fold_reveal(ui, &F, 4) && F.n == 1 && F.h[0] == 8);   /* both (0, 6) and (2, 5) held line 4 */
    CHECK(!cui__fold_reveal(ui, &F, 4));

    /* edits: lines [la, lb] became [la, la + k] */
    cui__fold_clear(&F);
    cui__fold_set(ui, &F, 2, 5); cui__fold_set(ui, &F, 8, 9);
    cui__fold_splice(ui, &F, 2, 2, 0);            /* typing on a folded line keeps its fold */
    CHECK(F.n == 2 && F.h[0] == 2 && F.e[0] == 5);
    cui__fold_splice(ui, &F, 0, 0, 2);            /* two lines more above: both move */
    CHECK(F.n == 2 && F.h[0] == 4 && F.e[0] == 7 && F.h[1] == 10 && F.e[1] == 11);
    cui__fold_splice(ui, &F, 6, 6, 0);            /* an edit of a hidden line opens that fold */
    CHECK(F.n == 1 && F.h[0] == 10 && F.hidden == 1);
    cui__fold_splice(ui, &F, 8, 9, 0);            /* lines 8 and 9 joined, above the fold: it moves up */
    CHECK(F.n == 1 && F.h[0] == 9 && F.e[0] == 10);
    cui__fold_splice(ui, &F, 9, 9, 1);            /* Enter on the folded line: it opens */
    CHECK(F.n == 0 && F.hidden == 0 && F.vn == 0);
    cui__fold_set(ui, &F, 3, 6);
    cui__fold_splice(ui, &F, 7, 7, 0);            /* just below: kept */
    cui__fold_splice(ui, &F, 2, 2, 0);            /* just above: kept */
    CHECK(F.n == 1 && F.h[0] == 3 && F.e[0] == 6);
    CHECK(F.chg0 < F.chg1);                       /* the lines whose rows changed are kept for the wrap counts */
    fold_done(ui, &F);
    cui_destroy(ui);
}

/* ---- the editor ---- */

typedef struct {
    char buf[64 * 1024];
    cui_text_doc *doc;
    const char *lang;
    bool wrap, fold, mm, dark;
    float h;
    cui_code_editor_result r;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        if (c->doc) c->r = cui_code_editor(ui, "ed", NULL, 0, .doc = c->doc, .lang = c->lang ? c->lang : "c", .height = c->h > 0 ? c->h : 300,
                                           .width = CUI_PX(400), .wrap = c->wrap, .folding = c->fold, .minimap = c->mm);
        else c->r = cui_code_editor(ui, "ed", c->buf, sizeof c->buf, .lang = c->lang ? c->lang : "c", .height = c->h > 0 ? c->h : 300,
                                    .width = CUI_PX(400), .wrap = c->wrap, .folding = c->fold, .minimap = c->mm);
    }
}

static void run(cui_ctx *ui, ed *c, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, c); cui_end_frame(ui); }
}

static void key(cui_ctx *ui, ed *c, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    run(ui, c, 1);
    cui_input_key(ui, k, false, mods);
    if (mods) cui_input_key(ui, CUI_KEY_NONE, false, 0);   /* the modifiers let go too */
    run(ui, c, 1);
}

static void press(cui_ctx *ui, ed *c, float x, float y) {
    cui_input_mouse_move(ui, x, y);
    run(ui, c, 2);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, c, 2);
}

static const char CODE[] =
    "int f(void) {\n"          /* 0: a block down to 5 */
    "    int a = 1;\n"         /* 1 */
    "    if (a) {\n"           /* 2: down to 3 */
    "        a++;\n"           /* 3 */
    "    }\n"                  /* 4 */
    "    return a;\n"          /* 5 */
    "}\n"                      /* 6 */
    "\n"                       /* 7 */
    "int g(void) {\n"          /* 8: down to 9 */
    "    return 2; /* hidden_word */\n"   /* 9 */
    "}\n";                     /* 10, and an empty line 11 */

static int line_start(const char *s, int line) {
    int at = 0;
    for (int l = 0; l < line; l++) { const char *nl = strchr(s + at, '\n'); if (!nl) return (int)strlen(s); at = (int)(nl - s) + 1; }
    return at;
}

/* folds by the API, the caret stepping over them, find and edits opening them, presses on the markers and boxes */
static void test_editor(bool doc, bool wrap) {
    cui_ctx *ui = new_ctx(false);
    ed *c = calloc(1, sizeof *c);
    snprintf(c->buf, sizeof c->buf, "%s", CODE);
    if (doc) c->doc = cui_text_doc_new(CODE, -1);
    c->fold = true; c->wrap = wrap;
    run(ui, c, 3);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    float x0 = c->r.caret_x, y0 = c->r.caret_y, lh = c->r.caret_h;
    CHECK(c->r.fold_count == 0 && c->r.hidden_lines == 0 && lh > 10);
    cui_code_editor_select(ui, "ed", 1, 1);
    run(ui, c, 2);
    float cw = c->r.caret_x - x0;
    CHECK(cw > 4 && cw < 12);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);

    /* fold the block of line 1 (0 based 0): lines 1 to 5 hidden; Down steps over them, Up comes back */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 1);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 5);
    key(ui, c, CUI_KEY_DOWN, 0);
    CHECK(c->r.line == 7);
    CHECK(c->r.caret_y > y0 + lh * 0.5f && c->r.caret_y < y0 + lh * 1.5f);   /* on the second row */
    key(ui, c, CUI_KEY_UP, 0);
    CHECK(c->r.line == 1);
    /* End, then Right: on to the line after the fold; Left: back to the end of the folded line */
    key(ui, c, CUI_KEY_END, 0);
    int end0 = c->r.cursor;
    key(ui, c, CUI_KEY_RIGHT, 0);
    CHECK(c->r.line == 7 && c->r.column == 1 && c->r.fold_count == 1);
    key(ui, c, CUI_KEY_LEFT, 0);
    CHECK(c->r.cursor == end0 && c->r.fold_count == 1);
    /* Shift+Down selects across the fold, which stays */
    key(ui, c, CUI_KEY_DOWN, CUI_MOD_SHIFT);
    CHECK(c->r.line == 7 && c->r.anchor == end0 && c->r.fold_count == 1);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);

    /* a press on the folded line's box opens it; a press on the marker in the gutter folds it again */
    float bx = x0 + 13 * cw + 0.6f * cw + CUI__FOLD_BOX_COLS * cw * 0.5f;   /* "int f(void) {" is 13 columns */
    press(ui, c, bx, y0 + lh * 0.5f);
    CHECK(c->r.fold_count == 0 && c->r.hidden_lines == 0);
    CHECK(c->r.cursor == 0 && c->r.anchor == 0);   /* the press only opened it */
    float lane_x = x0 - 8 - CUI__FOLD_W * 0.5f;
    press(ui, c, lane_x, y0 + lh * 0.5f);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 5);
    CHECK(c->r.cursor == 0 && c->r.anchor == 0);   /* no line selected */
    press(ui, c, lane_x, y0 + lh * 1.5f);          /* the next row shows line 7, "}": no block, a usual gutter press */
    CHECK(c->r.fold_count == 1 && c->r.anchor == line_start(CODE, 6) && c->r.cursor > c->r.anchor);   /* line 7 selected */
    press(ui, c, lane_x, y0 + lh * 0.5f);          /* the marker of a folded line opens it */
    CHECK(c->r.fold_count == 0);

    /* a find match inside a fold opens it */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 9);   /* line 9 is "int g(void) {" */
    run(ui, c, 2);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 1);
    cui_code_editor_find(ui, "ed", "hidden_word");
    run(ui, c, 3);
    CHECK(c->r.fold_count == 0 && c->r.line == 10);
    cui_code_editor_find(ui, "ed", NULL);
    run(ui, c, 2);

    /* folding with the caret inside: it goes to the end of the folded line */
    int l3 = line_start(CODE, 3);
    cui_code_editor_select(ui, "ed", l3 + 4, l3 + 4);
    run(ui, c, 2);
    CHECK(c->r.line == 4);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 0);   /* at the caret: the innermost block, "if (a) {" */
    run(ui, c, 2);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 1 && c->r.line == 3 && c->r.cursor == line_start(CODE, 3) - 1);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 0);   /* again: the block around it */
    run(ui, c, 2);
    CHECK(c->r.fold_count == 2 && c->r.hidden_lines == 5 && c->r.line == 1);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN, 0);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 1);   /* the inner one is still folded */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN_ALL, 0);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 0);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_ALL, 0);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 3 && c->r.hidden_lines == 6);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    key(ui, c, CUI_KEY_DOWN, 0); key(ui, c, CUI_KEY_DOWN, 0); key(ui, c, CUI_KEY_DOWN, 0);
    CHECK(c->r.line == 9);   /* rows: 1, 7, 8, 9 */
    key(ui, c, CUI_KEY_A, CUI_MOD_PRIMARY);   /* select all keeps the folds */
    CHECK(c->r.fold_count == 3 && c->r.anchor == 0 && c->r.cursor == c->r.length);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN_ALL, 0);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);

    /* edits: typing on the folded line keeps the fold, an edit of a hidden line opens it, lines above move it */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 1);
    run(ui, c, 2);
    key(ui, c, CUI_KEY_END, 0);
    cui_input_text(ui, " ");
    run(ui, c, 2);
    CHECK(c->r.changed || c->r.length == (int)strlen(CODE) + 1);
    CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 5);
    if (doc) {   /* someone else's edits: above a fold, then inside it */
        cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN_ALL, 0);
        cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 9);   /* "int g(void) {": its line 10 hidden */
        run(ui, c, 2);
        cui_text_doc_replace(c->doc, 0, 0, "// top\n", -1);
        run(ui, c, 2);
        CHECK(c->r.fold_count == 1 && c->r.hidden_lines == 1);
        int g = cui_text_doc_line_start(c->doc, 9);
        cui_code_editor_select(ui, "ed", g, g);
        run(ui, c, 2);
        key(ui, c, CUI_KEY_DOWN, 0);
        CHECK(c->r.line == 12);   /* the fold moved down a line with its text */
        int h = cui_text_doc_line_start(c->doc, 10);
        cui_text_doc_replace(c->doc, h, h, "x", 1);
        run(ui, c, 2);
        CHECK(c->r.fold_count == 0 && c->r.hidden_lines == 0);
        cui_text_doc_set(c->doc, CODE, -1);   /* a new text: no folds */
    } else {
        cui_code_editor_edit(ui, "ed", line_start(c->buf, 3), line_start(c->buf, 3), "x", -1, -1);   /* in a hidden line */
        run(ui, c, 2);
        CHECK(c->r.fold_count == 0);
        cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 1);
        run(ui, c, 2);
        CHECK(c->r.fold_count == 1);
        snprintf(c->buf, sizeof c->buf, "%s", CODE);   /* a new text: no folds */
    }
    run(ui, c, 2);
    CHECK(c->r.fold_count == 0);

    /* folding off: every line shows again */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 1);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 1);
    c->fold = false;
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    key(ui, c, CUI_KEY_DOWN, 0);
    CHECK(c->r.fold_count == 0 && c->r.line == 2);

    /* idle: no frames wanted */
    c->fold = true; c->mm = true;
    cui_code_editor_fold(ui, "ed", CUI_FOLD_ALL, 0);
    cui_input_mouse_move(ui, 5, 5);
    run(ui, c, 40);
    CHECK(c->r.fold_count == 3 && !cui_needs_redraw(ui));
    if (c->doc) cui_text_doc_free(c->doc);
    free(c);
    cui_destroy(ui);
}

/* markdown headings in an editor of a document */
static void test_markdown(void) {
    cui_ctx *ui = new_ctx(false);
    ed *c = calloc(1, sizeof *c);
    char md[1024] = "";
    for (int i = 0; i < CUI_COUNTOF(MD); i++) { strcat(md, MD[i]); strcat(md, "\n"); }
    c->doc = cui_text_doc_new(md, -1);
    c->lang = "markdown"; c->fold = true; c->wrap = true;
    run(ui, c, 3);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_ALL, 0);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 5 && c->r.hidden_lines == 13);   /* everything under "# Title" (not the empty last line) */
    cui_code_editor_fold(ui, "ed", CUI_FOLD_OPEN, 1);
    run(ui, c, 2);
    CHECK(c->r.fold_count == 4);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    key(ui, c, CUI_KEY_DOWN, 0); key(ui, c, CUI_KEY_DOWN, 0); key(ui, c, CUI_KEY_DOWN, 0); key(ui, c, CUI_KEY_DOWN, 0);
    CHECK(c->r.line == 9);   /* intro, ## A (folded), the blank line, ## B (folded) */
    cui_text_doc_free(c->doc);
    free(c);
    cui_destroy(ui);
}

/* the minimap: a press low on the strip scrolls down, a drag up scrolls back, the wheel over it scrolls the text */
static void test_minimap(void) {
    cui_ctx *ui = new_ctx(false);
    ed *c = calloc(1, sizeof *c);
    int n = 0;
    for (int i = 0; i < 400; i++) n += snprintf(c->buf + n, sizeof c->buf - (size_t)n, "%sline %d = %d; // note\n", i % 5 ? "    " : "", i, i * 7);
    c->mm = true;
    run(ui, c, 3);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 3);
    float y0 = c->r.caret_y, x0 = c->r.caret_x;
    float sx = 20 + 400 - CUI_MM_WIDTH * 0.5f;   /* the strip's middle */
    CHECK(x0 < 20 + 400 - CUI_MM_WIDTH);         /* the text is left of it */
    press(ui, c, sx, 20 + 300 - 12);
    CHECK(c->r.caret_y < y0 - 100);              /* scrolled down: the caret's line went up out of view */
    float yd = c->r.caret_y;
    cui_input_mouse_move(ui, sx, 20 + 300 - 12);
    run(ui, c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, c, 1);
    for (int k = 1; k <= 8; k++) { cui_input_mouse_move(ui, sx, 20 + 300 - 12 - (float)k * 30); run(ui, c, 1); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, c, 2);
    CHECK(c->r.caret_y > yd + 50);               /* dragged up: scrolled back up */
    float yw = c->r.caret_y;
    cui_input_mouse_move(ui, sx, 150);
    run(ui, c, 1);
    cui_input_scroll(ui, 0, -3);
    run(ui, c, 2);
    CHECK(c->r.caret_y < yw);                    /* the wheel over the strip */
    CHECK(c->r.cursor == 0 && c->r.anchor == 0); /* none of it moved the caret */
    /* a typed line moves the bars below it; the strip still draws */
    cui_input_mouse_move(ui, 100, 100);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    cui_input_text(ui, "x");
    key(ui, c, CUI_KEY_ENTER, 0);
    run(ui, c, 30);
    CHECK(c->r.line == 2 && !cui_needs_redraw(ui));
    free(c);
    cui_destroy(ui);
}

static void shot(const char *path) {
    cui_ctx *ui = new_ctx(true);
    cui_set_viewport(ui, 640, 360, 1);
    ed *c = calloc(1, sizeof *c);
    int n = snprintf(c->buf, sizeof c->buf, "%s", CODE);
    for (int i = 0; i < 60; i++) n += snprintf(c->buf + n, sizeof c->buf - (size_t)n, "int h%d(int x) {\n    if (x > %d) {\n        return x * %d; // more\n    }\n    return %d;\n}\n", i, i, i, i);
    c->fold = true; c->mm = true; c->h = 320; c->dark = true;
    run(ui, c, 3);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 1);
    run(ui, c, 2);
    cui_code_editor_fold(ui, "ed", CUI_FOLD_CLOSE, 13);
    run(ui, c, 2);
    cui_input_mouse_move(ui, 40, 120);   /* over the gutter: the open blocks' markers show */
    run(ui, c, 3);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
    free(c);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--shot")) { shot(argv[2]); cui_sw_destroy(g_sw); return 0; }
    test_blocks();
    test_rows();
    test_editor(false, false);
    test_editor(true, false);
    test_editor(false, true);
    test_editor(true, true);
    test_markdown();
    test_minimap();
    cui_sw_destroy(g_sw);
    printf("test_fold: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
