/* test_editor_fold_multi.c - the code editor's multiple cursors (cui_code_editor_opts.multi_cursor) together with
 * folding (.folding) and the minimap (.minimap): the other carets and selections drawn on the rows they are on when
 * lines above or between them are folded (wrapped and not), a cursor or match added inside a fold opening it (as
 * common editors do), folding over other carets moving them out, column selection and Ctrl+Alt+Up/Down stepping over
 * folded lines, the arrows at every caret stepping over them, typing and Enter at carets around folds and undo/redo
 * keeping the folds, Alt+click below a fold, the minimap's bars and box with folds, and the carets resting together
 * (cui_set_caret_blink) so an idle editor with several carets draws no frames.
 * Headless: the software renderer; what is drawn is read from the draw data. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a), *b_ = (b); if (strcmp(a_, b_)) { g_failed++; \
    printf("  FAIL %s:%d: %s\n    got      \"%s\"\n    expected \"%s\"\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* the editor's folds, hidden lines and cursors (-1: any) */
#define CHECK_ST(r, folds, hidden, cursors) do { g_checks++; if (((folds) >= 0 && (r).fold_count != (folds)) || ((hidden) >= 0 && (r).hidden_lines != (hidden)) || \
    ((cursors) >= 0 && (r).cursor_count != (cursors))) { g_failed++; printf("  FAIL %s:%d: folds %d hidden %d cursors %d, expected %d %d %d\n", __FILE__, __LINE__, \
    (r).fold_count, (r).hidden_lines, (r).cursor_count, (folds), (hidden), (cursors)); } } while (0)

static cui_sw_renderer *g_sw;

typedef struct {
    char buf[1 << 16];
    cui_text_doc *doc;
    bool wrap, mm;
    float h;
    cui_code_editor_result r;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->doc ? NULL : c->buf, c->doc ? 0 : sizeof c->buf, .doc = c->doc, .lang = "c",
                               .height = c->h > 0 ? c->h : 420, .width = CUI_PX(700), .autofocus = true, .multi_cursor = true,
                               .folding = true, .minimap = c->mm, .wrap = c->wrap, .tab_size = 4);
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
    static char out[1 << 16];
    if (!c->doc) return c->buf;
    int n = cui_text_doc_read(c->doc, 0, cui_text_doc_length(c->doc), out);
    out[n] = 0;
    return out;
}

static int ls_of(ed *c, int line) {   /* where line `line` (from 0) starts */
    const char *s = text(c);
    int at = 0;
    for (int l = 0; l < line; l++) { const char *nl = strchr(s + at, '\n'); if (!nl) return (int)strlen(s); at = (int)(nl - s) + 1; }
    return at;
}

static const char *sels(cui_ctx *ui) {
    static char out[4096];
    int an[256], cu[256];
    int n = cui_code_editor_selections(ui, "ed", an, cu, 256), k = 0;
    out[0] = 0;
    for (int i = 0; i < n && i < 256 && k < (int)sizeof out - 24; i++) k += snprintf(out + k, sizeof out - (size_t)k, i ? " %d-%d" : "%d-%d", an[i], cu[i]);
    return out;
}

enum { P = CUI_MOD_PRIMARY, A = CUI_MOD_ALT, S = CUI_MOD_SHIFT };

/* lines (from 0): a block 0 to 3 (folded: 1 to 3 hidden), a long line 5 (wrapped: several rows), a block 6 to 8 */
static const char *T_LINES[] = {
    "a0 {",
    "    x1 = key;",
    "    x2 = key;",
    "    x3 = key;",
    "}",
    "b5 key",   /* + filler */
    "c6 {",
    "    y7 = key;",
    "    y8 = key;",
    "}",
    "d10 key",
    "e11 key",
    "",
};

static void make_text(char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < (int)(sizeof T_LINES / sizeof *T_LINES); i++) {
        n += (size_t)snprintf(out + n, cap - n, "%s", T_LINES[i]);
        if (i == 5) for (int k = 0; k < 30; k++) n += (size_t)snprintf(out + n, cap - n, " filler");
        if (i + 1 < (int)(sizeof T_LINES / sizeof *T_LINES)) n += (size_t)snprintf(out + n, cap - n, "\n");
    }
}

static cui_ctx *open_ed(ed *c, bool doc, bool wrap) {
    memset(c, 0, sizeof *c);
    c->wrap = wrap;
    make_text(c->buf, sizeof c->buf);
    if (doc) c->doc = cui_text_doc_new(c->buf, -1);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 800, 500, 1);
    cui_set_reduced_motion(ui, true);
    cui_set_caret_blink(ui, -1.0f);   /* the carets always on: what is drawn does not depend on the time */
    run(ui, c, 3);
    cui_code_editor_select(ui, "ed", 0, 0);
    run(ui, c, 2);
    return ui;
}
static void close_ed(cui_ctx *ui, ed *c) {
    cui_destroy(ui);
    if (c->doc) cui_text_doc_free(c->doc);
    c->doc = NULL;
}

static void fold(cui_ctx *ui, ed *c, int act, int line1) { cui_code_editor_fold(ui, "ed", act, line1); run(ui, c, 2); }
static void fold_both(cui_ctx *ui, ed *c) { fold(ui, c, CUI_FOLD_CLOSE, 1); fold(ui, c, CUI_FOLD_CLOSE, 7); }

static void carets(cui_ctx *ui, ed *c, const int *at, int n) {   /* at[0] the primary one */
    cui_code_editor_select(ui, "ed", at[0], at[0]);
    run(ui, c, 1);
    for (int i = 1; i < n; i++) cui_code_editor_add_selection(ui, "ed", at[i], at[i]);
    run(ui, c, 2);
}

/* ---- what is drawn ---- */

typedef struct { float x, y, hw, hh; } quad;

/* the rounded rects drawn this frame at least min_a opaque, one per shape (their vertices share a centre) */
static int quads(cui_ctx *ui, quad *out, int cap, int min_a) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    int n = 0;
    for (int i = 0; i < dd->vertex_count; i++) {
        const cui_vertex *v = &dd->vertices[i];
        if ((int)v->mode != CUI_SHADE_RRECT || v->color.a < min_a) continue;
        float cx = v->x - v->lx, cy = v->y - v->ly;
        bool dup = false;
        for (int k = n - 1; k >= 0 && k >= n - 8 && !dup; k--)
            dup = fabsf(out[k].x - cx) < 0.25f && fabsf(out[k].y - cy) < 0.25f && fabsf(out[k].hw - v->hw) < 0.25f && fabsf(out[k].hh - v->hh) < 0.25f;
        if (!dup && n < cap) out[n++] = (quad){ cx, cy, v->hw, v->hh };
    }
    return n;
}

/* the carets drawn (bars 1.5 wide, taller than half a row) */
static int caret_marks(cui_ctx *ui, float lh, quad *out, int cap) {
    static quad q[1 << 15];
    int n = quads(ui, q, 1 << 15, 100), k = 0;
    for (int i = 0; i < n; i++) if (fabsf(q[i].hw - 0.75f) < 0.05f && q[i].hh * 2 > lh * 0.5f && k < cap) out[k++] = q[i];
    return k;
}

typedef struct { float x, y, lh; } spot;

/* where the primary caret is drawn at pos (its row top and x): the reference the other carets must match */
static spot where(cui_ctx *ui, ed *c, int pos) {
    cui_code_editor_select(ui, "ed", pos, pos);
    run(ui, c, 2);
    return (spot){ c->r.caret_x, c->r.caret_y, c->r.caret_h };
}

/* Every caret drawn where the primary caret would be drawn at its place, and nothing more: the carets are put back as
 * they were afterwards (the last one primary). Returns the misses. */
static int check_carets(cui_ctx *ui, ed *c, const char *what) {
    int an[64], cu[64];
    int n = cui_code_editor_selections(ui, "ed", an, cu, 64);
    spot sp[64];
    for (int i = 0; i < n; i++) sp[i] = where(ui, c, cu[i]);
    cui_code_editor_select(ui, "ed", an[0], cu[0]);
    run(ui, c, 1);
    for (int i = 1; i < n; i++) cui_code_editor_add_selection(ui, "ed", an[i], cu[i]);
    run(ui, c, 2);
    quad m[128];
    int nm = caret_marks(ui, sp[0].lh, m, 128), miss = 0;
    for (int i = 0; i < n; i++) {
        bool hit = false;
        for (int k = 0; k < nm && !hit; k++) hit = fabsf(m[k].x - sp[i].x) <= 1.25f && m[k].y >= sp[i].y && m[k].y < sp[i].y + sp[i].lh;
        if (!hit) { miss++; printf("  (%s: no caret drawn at %d, expected x %.1f y %.1f)\n", what, cu[i], sp[i].x, sp[i].y); }
    }
    if (nm != n) { miss++; printf("  (%s: %d carets drawn for %d)\n", what, nm, n); }
    return miss;
}

/* a selection rect from x0 to x1 on the row whose top is y */
static bool sel_drawn(cui_ctx *ui, float x0, float x1, float y, float lh) {
    static quad q[1 << 15];
    int n = quads(ui, q, 1 << 15, 1);   /* (a selection is see-through) */
    for (int i = 0; i < n; i++)
        if (fabsf(q[i].x - (x0 + x1) * 0.5f) < 1.0f && fabsf(q[i].hw * 2 - (x1 - x0)) < 1.5f && fabsf(q[i].y - (y + lh * 0.5f)) < 1.0f && fabsf(q[i].hh * 2 - lh) < 1.0f) return true;
    return false;
}

/* ---- the tests ---- */

/* the other carets and selections on the rows they are on, with folds above and between them */
static void t_paint(bool doc, bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, doc, wrap);
    fold_both(ui, &c);
    CHECK(c.r.fold_count == 2 && c.r.hidden_lines == 5);
    int at[] = { ls_of(&c, 0) + 1, ls_of(&c, 5) + 2, ls_of(&c, 9) + 1, ls_of(&c, 10) + 1, ls_of(&c, 11) + 3 };
    carets(ui, &c, at, 5);
    CHECK(c.r.cursor_count == 5 && c.r.fold_count == 2);
    CHECK(check_carets(ui, &c, wrap ? "wrapped" : "unwrapped") == 0);
    /* an end of the long line (wrapped: on its last row) and a caret below it */
    int at2[] = { wrap ? ls_of(&c, 6) - 1 : ls_of(&c, 5) + 6, ls_of(&c, 11), ls_of(&c, 4) };   /* (unwrapped the end is out of view) */
    carets(ui, &c, at2, 3);
    CHECK(check_carets(ui, &c, "row ends") == 0);
    /* a selection of another cursor: "d10" on line 10, drawn on its row */
    int l10 = ls_of(&c, 10);
    spot a = where(ui, &c, l10), b = where(ui, &c, l10 + 3);
    cui_code_editor_select(ui, "ed", ls_of(&c, 0), ls_of(&c, 0));
    run(ui, &c, 1);
    cui_code_editor_add_selection(ui, "ed", l10, l10 + 3);
    cui_code_editor_add_selection(ui, "ed", ls_of(&c, 4), ls_of(&c, 4));   /* the last one added is the primary one */
    run(ui, &c, 2);
    CHECK(c.r.cursor_count == 3);
    CHECK(sel_drawn(ui, a.x, b.x, a.y, a.lh));
    close_ed(ui, &c);
}

/* a cursor or a match added inside a fold opens it (cui_code_editor_add_selection, Ctrl+D, Ctrl+Shift+L) */
static void t_reveal(bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, wrap);
    fold_both(ui, &c);
    int at[] = { ls_of(&c, 0), ls_of(&c, 2) + 4 };   /* line 2 is folded away */
    carets(ui, &c, at, 2);
    CHECK(c.r.cursor_count == 2 && c.r.fold_count == 1 && c.r.hidden_lines == 2);   /* the fold of line 1 opened */
    CHECK(check_carets(ui, &c, "added in a fold") == 0);
    fold_both(ui, &c);
    CHECK(c.r.fold_count == 2);
    /* Ctrl+D from "key" on line 5: the next one is on line 7, folded away */
    int k5 = ls_of(&c, 5) + 3;
    cui_code_editor_select(ui, "ed", k5, k5 + 3);
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_D, P);
    CHECK(c.r.cursor_count == 2 && c.r.fold_count == 1 && c.r.hidden_lines == 3);
    char want[64];
    snprintf(want, sizeof want, "%d-%d %d-%d", k5, k5 + 3, ls_of(&c, 7) + 9, ls_of(&c, 7) + 12);
    CHECK_STR(sels(ui), want);
    CHECK(check_carets(ui, &c, "Ctrl+D") == 0);
    /* Ctrl+Shift+L: every "key", the folded ones too: every fold opens */
    key(ui, &c, CUI_KEY_L, P | S);
    CHECK(c.r.cursor_count == 8 && c.r.fold_count == 0 && c.r.hidden_lines == 0);
    CHECK(check_carets(ui, &c, "Ctrl+Shift+L") == 0);
    close_ed(ui, &c);
}

/* folding over other carets: they go to the end of the line shown above, and the fold stays */
static void t_fold_over(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, false);
    int at[] = { ls_of(&c, 10), ls_of(&c, 2) + 6, ls_of(&c, 3) + 2, ls_of(&c, 8) + 1 };
    carets(ui, &c, at, 4);
    CHECK(c.r.cursor_count == 4);
    fold(ui, &c, CUI_FOLD_CLOSE, 1);
    run(ui, &c, 5);
    CHECK(c.r.fold_count == 1 && c.r.hidden_lines == 3);   /* not opened again by the carets that were inside */
    char want[64];
    int e0 = ls_of(&c, 1) - 1, l8 = ls_of(&c, 8) + 1, l10 = ls_of(&c, 10);
    snprintf(want, sizeof want, "%d-%d %d-%d %d-%d", e0, e0, l8, l8, l10, l10);   /* the two inside became one */
    CHECK_STR(sels(ui), want);
    CHECK(check_carets(ui, &c, "folded over") == 0);
    /* a selection whose caret is folded away: a caret at the end of the line shown above, as the primary one */
    fold(ui, &c, CUI_FOLD_OPEN_ALL, 0);
    cui_code_editor_select(ui, "ed", ls_of(&c, 11), ls_of(&c, 11));   /* one caret again (a select of another place) */
    run(ui, &c, 1);
    cui_code_editor_add_selection(ui, "ed", ls_of(&c, 5), ls_of(&c, 7) + 2);   /* from line 5 into line 7 */
    run(ui, &c, 2);
    fold(ui, &c, CUI_FOLD_CLOSE, 7);
    run(ui, &c, 3);
    CHECK_ST(c.r, 1, 2, 2);
    snprintf(want, sizeof want, "%d-%d %d-%d", ls_of(&c, 7) - 1, ls_of(&c, 7) - 1, ls_of(&c, 11), ls_of(&c, 11));
    CHECK_STR(sels(ui), want);
    close_ed(ui, &c);
}

/* column selection over a fold: the folded lines are skipped (none selected unseen), by keys and by the pointer */
static void t_column(bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, wrap);
    fold(ui, &c, CUI_FOLD_CLOSE, 1);   /* lines 1 to 3 hidden */
    cui_code_editor_select(ui, "ed", ls_of(&c, 0) + 1, ls_of(&c, 0) + 1);
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_DOWN, A | S);
    key(ui, &c, CUI_KEY_DOWN, A | S);   /* lines 0, 4 and 5 */
    char want[96];
    int l0 = ls_of(&c, 0), l4 = ls_of(&c, 4), l5 = ls_of(&c, 5), l6 = ls_of(&c, 6);
    snprintf(want, sizeof want, "%d-%d %d-%d %d-%d", l0 + 1, l0 + 1, l4 + 1, l4 + 1, l5 + 1, l5 + 1);
    CHECK_STR(sels(ui), want);
    CHECK(c.r.fold_count == 1 && c.r.hidden_lines == 3);
    key(ui, &c, CUI_KEY_UP, A | S);   /* back up: line 4 is the far end again */
    snprintf(want, sizeof want, "%d-%d %d-%d", l0 + 1, l0 + 1, l4 + 1, l4 + 1);
    CHECK_STR(sels(ui), want);
    key(ui, &c, CUI_KEY_ESCAPE, 0);
    /* Alt+Shift+drag from row 0, column 1 down to row 3 (line 6), column 3 */
    spot r0 = where(ui, &c, l0), r1 = where(ui, &c, l0 + 1);
    float cw = r1.x - r0.x, lh = r0.lh;
    float x = r0.x + 1 * cw + 0.5f, y = r0.y + lh * 0.5f;
    spot s6 = where(ui, &c, l6);   /* line 6's row (wrapped, line 5 takes several) */
    int row6 = (int)floorf((s6.y - r0.y) / lh + 0.5f);
    cui_code_editor_select(ui, "ed", l0, l0);
    run(ui, &c, 2);
    cui_input_mouse_move(ui, x, y);
    run(ui, &c, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, A | S);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, &c, 1);
    for (int k = 1; k <= 8; k++) {
        cui_input_mouse_move(ui, x + (float)k * 0.25f * cw, y + (float)k / 8.0f * (float)row6 * lh);
        run(ui, &c, 1);
    }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    run(ui, &c, 2);
    CHECK(c.r.fold_count == 1 && c.r.hidden_lines == 3);
    /* lines 0, 4 ("}" ends at the column's left: a caret at its end), 5 and 6, not 1 to 3; wrapped the column is by
     * lines too */
    snprintf(want, sizeof want, "%d-%d %d-%d %d-%d %d-%d", l0 + 1, l0 + 3, l4 + 1, l4 + 1, l5 + 1, l5 + 3, l6 + 1, l6 + 3);
    CHECK_STR(sels(ui), want);
    close_ed(ui, &c);
}

/* Ctrl+Alt+Up and Down add a caret on the next line shown, over folded lines */
static void t_add_line(bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, wrap);
    fold_both(ui, &c);
    int l0 = ls_of(&c, 0), l4 = ls_of(&c, 4), l9 = ls_of(&c, 9), l6 = ls_of(&c, 6);
    cui_code_editor_select(ui, "ed", l0 + 1, l0 + 1);
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_DOWN, P | A);
    char want[64];
    snprintf(want, sizeof want, "%d-%d %d-%d", l0 + 1, l0 + 1, l4 + 1, l4 + 1);
    CHECK_STR(sels(ui), want);
    CHECK(c.r.fold_count == 2);
    cui_code_editor_select(ui, "ed", l9 + 1, l9 + 1);
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_UP, P | A);
    snprintf(want, sizeof want, "%d-%d %d-%d", l6 + 1, l6 + 1, l9 + 1, l9 + 1);
    CHECK_STR(sels(ui), want);
    CHECK(c.r.fold_count == 2);
    CHECK(check_carets(ui, &c, "Ctrl+Alt+Up") == 0);
    close_ed(ui, &c);
}

/* the arrows at every caret step over folded lines, as the primary one does */
static void t_moves(void) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, false);
    fold_both(ui, &c);
    int at[] = { ls_of(&c, 0) + 1, ls_of(&c, 6) + 1 };
    carets(ui, &c, at, 2);
    key(ui, &c, CUI_KEY_DOWN, 0);
    char want[64];
    snprintf(want, sizeof want, "%d-%d %d-%d", ls_of(&c, 4) + 1, ls_of(&c, 4) + 1, ls_of(&c, 9) + 1, ls_of(&c, 9) + 1);
    CHECK_STR(sels(ui), want);
    key(ui, &c, CUI_KEY_UP, 0);
    int e0 = ls_of(&c, 1) - 1, e6 = ls_of(&c, 7) - 1;
    key(ui, &c, CUI_KEY_END, 0);
    snprintf(want, sizeof want, "%d-%d %d-%d", e0, e0, e6, e6);
    CHECK_STR(sels(ui), want);
    key(ui, &c, CUI_KEY_RIGHT, 0);   /* past the end of a folded line: on to the line after the fold */
    snprintf(want, sizeof want, "%d-%d %d-%d", ls_of(&c, 4), ls_of(&c, 4), ls_of(&c, 9), ls_of(&c, 9));
    CHECK_STR(sels(ui), want);
    CHECK(c.r.fold_count == 2);
    key(ui, &c, CUI_KEY_LEFT, 0);    /* and back to the end of the folded line */
    snprintf(want, sizeof want, "%d-%d %d-%d", e0, e0, e6, e6);
    CHECK_STR(sels(ui), want);
    CHECK(c.r.fold_count == 2 && c.r.cursor_count == 2);
    close_ed(ui, &c);
}

/* typing, Backspace and Enter at carets around folds; undo and redo: the folds stay with their lines */
static void t_edit_undo(bool doc) {
    ed c;
    cui_ctx *ui = open_ed(&c, doc, false);
    char before[4096];
    snprintf(before, sizeof before, "%s", text(&c));
    fold_both(ui, &c);
    int at[] = { ls_of(&c, 0), ls_of(&c, 5), ls_of(&c, 9), ls_of(&c, 11) };
    carets(ui, &c, at, 4);
    type(ui, &c, "Z");
    run(ui, &c, 2);
    CHECK(c.r.cursor_count == 4 && c.r.fold_count == 2 && c.r.hidden_lines == 5);
    const char *t = text(&c);
    CHECK(!strncmp(t + ls_of(&c, 0), "Za0 {", 5) && !strncmp(t + ls_of(&c, 5), "Zb5", 3) && !strncmp(t + ls_of(&c, 9), "Z}", 2) &&
          !strncmp(t + ls_of(&c, 11), "Ze11", 4) && !strncmp(t + ls_of(&c, 6), "c6 {", 4));
    CHECK(check_carets(ui, &c, "typed") == 0);
    char typed[4096];
    snprintf(typed, sizeof typed, "%s", text(&c));
    /* Enter at carets below the first fold's lines and between: the second fold moves down with its lines */
    int at2[] = { ls_of(&c, 5) + 1, ls_of(&c, 10) };
    carets(ui, &c, at2, 2);
    key(ui, &c, CUI_KEY_ENTER, 0);
    CHECK(c.r.line_count == 15 && c.r.fold_count == 2 && c.r.hidden_lines == 5);
    cui_code_editor_select(ui, "ed", ls_of(&c, 7), ls_of(&c, 7));   /* "c6 {" is line 7 now: Down goes past its fold */
    run(ui, &c, 2);
    key(ui, &c, CUI_KEY_DOWN, 0);
    CHECK(c.r.line == 11);   /* (1 based) "Z}" */
    /* undo the Enter, then the typing: the text and the folds as they were, the carets back */
    key(ui, &c, CUI_KEY_Z, P);   /* (a document: the step's edits come back in one sync, the folds follow edit by edit) */
    CHECK_STR(text(&c), typed);
    CHECK_ST(c.r, 2, 5, 2);
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), before);
    CHECK_ST(c.r, 2, 5, 4);
    CHECK(check_carets(ui, &c, "undone") == 0);
    key(ui, &c, CUI_KEY_Y, P);
    CHECK_STR(text(&c), typed);
    CHECK_ST(c.r, 2, 5, 4);
    /* Backspace at the carets takes the Z's away; the folds stay */
    key(ui, &c, CUI_KEY_BACKSPACE, 0);
    CHECK_STR(text(&c), before);
    CHECK_ST(c.r, 2, 5, -1);
    /* an undo that puts a caret back inside a fold opens it */
    fold(ui, &c, CUI_FOLD_OPEN_ALL, 0);
    int at3[] = { ls_of(&c, 10), ls_of(&c, 2) + 4 };
    carets(ui, &c, at3, 2);
    type(ui, &c, "Q");
    fold(ui, &c, CUI_FOLD_CLOSE, 1);
    run(ui, &c, 3);
    CHECK(c.r.fold_count == 1 && c.r.hidden_lines == 3);
    key(ui, &c, CUI_KEY_Z, P);
    CHECK_STR(text(&c), before);
    CHECK(c.r.fold_count == 0 && c.r.cursor_count == 2);
    CHECK(check_carets(ui, &c, "undone into a fold") == 0);
    close_ed(ui, &c);
}

/* Alt+click on rows below a fold adds the caret on the line shown there, and takes it away again */
static void t_alt_click(bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, wrap);
    fold_both(ui, &c);
    int l10 = ls_of(&c, 10);
    spot s = where(ui, &c, l10 + 2), o = where(ui, &c, ls_of(&c, 0));
    cui_code_editor_select(ui, "ed", ls_of(&c, 0), ls_of(&c, 0));
    run(ui, &c, 2);
    for (int pass = 0; pass < 2; pass++) {
        cui_input_mouse_move(ui, s.x + 0.5f, s.y + s.lh * 0.5f);
        run(ui, &c, 1);
        cui_input_key(ui, CUI_KEY_NONE, false, A);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        run(ui, &c, 1);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        run(ui, &c, 1);
        cui_input_key(ui, CUI_KEY_NONE, false, 0);
        run(ui, &c, 40);
        char want[64];
        if (pass == 0) snprintf(want, sizeof want, "%d-%d %d-%d", ls_of(&c, 0), ls_of(&c, 0), l10 + 2, l10 + 2);
        else snprintf(want, sizeof want, "%d-%d", ls_of(&c, 0), ls_of(&c, 0));
        CHECK_STR(sels(ui), want);
        CHECK(c.r.fold_count == 2);
    }
    (void)o;
    close_ed(ui, &c);
}

/* the minimap with folds and several carets: only the lines shown have bars, its box ends at the strip's end when the
 * view is at the end of the text */
static void t_minimap(void) {
    ed c;
    memset(&c, 0, sizeof c);
    int n = 0;
    for (int i = 0; i < 300; i++) n += snprintf(c.buf + n, sizeof c.buf - (size_t)n, "blk%d {\n    v = %d;\n    w = %d;\n}\n", i, i, i);
    snprintf(c.buf + n, sizeof c.buf - (size_t)n, "end");
    c.mm = true; c.h = 360;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 800, 500, 1);
    cui_set_reduced_motion(ui, true);
    run(ui, &c, 3);
    fold(ui, &c, CUI_FOLD_ALL, 0);
    CHECK(c.r.fold_count == 300 && c.r.hidden_lines == 600);
    int at[] = { 0, 30, (int)strlen(c.buf) };
    carets(ui, &c, at, 3);   /* the last one added is the primary one, at the end: the view goes there */
    run(ui, &c, 3);
    static quad q[1 << 15];
    int nq = quads(ui, q, 1 << 15, 1);   /* (the box is see-through) */
    int box = -1;
    for (int i = 0; i < nq; i++) if (fabsf(q[i].hw * 2 - (CUI_MM_WIDTH - 1)) < 0.5f) box = i;
    CHECK(box >= 0);
    if (box >= 0) {
        float left = q[box].x - q[box].hw - 1, bottom = q[box].y + q[box].hh;
        int bars = 0, indented = 0;
        float lowest = 0;
        for (int i = 0; i < nq; i++) {
            if (q[i].x - q[i].hw < left + 3 || q[i].x > left + CUI_MM_WIDTH || q[i].hh > 1.0f) continue;   /* the bars: 1.4 high */
            bars++;
            if (q[i].x - q[i].hw > left + 7) indented++;   /* only the indented lines start right of the margin */
            lowest = fmaxf(lowest, q[i].y + q[i].hh);
        }
        CHECK(bars > 100 && indented == 0);
        CHECK(fabsf(bottom - (lowest + 0.3f)) < 2.5f);   /* the box ends with the last row's bar */
    }
    CHECK(c.r.cursor_count == 3 && c.r.fold_count == 300);
    cui_destroy(ui);
}

/* idle: the other carets blink with the primary one and rest with it; then no frames are wanted */
static void t_idle(bool wrap) {
    ed c;
    cui_ctx *ui = open_ed(&c, false, wrap);
    fold_both(ui, &c);
    cui_set_caret_blink(ui, 5.0f);
    int at[] = { ls_of(&c, 0) + 1, ls_of(&c, 5) + 2, ls_of(&c, 10) + 1 };
    carets(ui, &c, at, 3);
    key(ui, &c, CUI_KEY_RIGHT, 0);   /* a key: the blink starts again from on */
    int odd = 0, off = 0;
    quad m[16];
    for (int i = 0; i < 60 * 5; i++) {   /* five seconds of frames: all three on, or all off */
        run(ui, &c, 1);
        int k = caret_marks(ui, c.r.caret_h, m, 16);
        if (k == 0) off++;
        else if (k != 3) odd++;
    }
    CHECK(odd == 0 && off > 20);
    run(ui, &c, 60);
    CHECK(caret_marks(ui, c.r.caret_h, m, 16) == 3);   /* at rest: all on */
    CHECK(cui_next_wakeup(ui) < 0 && !cui_needs_redraw(ui));
    close_ed(ui, &c);
}

int main(void) {
    g_sw = cui_sw_create();
    for (int w = 0; w < 2; w++) {
        t_paint(false, w);
        t_paint(true, w);
        t_reveal(w);
        t_column(w);
        t_add_line(w);
        t_alt_click(w);
        t_idle(w);
    }
    t_fold_over();
    t_moves();
    t_edit_undo(false);
    t_edit_undo(true);
    t_minimap();
    cui_sw_destroy(g_sw);
    printf("test_editor_fold_multi: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
