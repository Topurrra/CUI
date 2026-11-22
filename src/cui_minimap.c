/* The code editor's minimap (cui_code_editor_opts.minimap): a strip at the editor's right edge with one bar a row, in
 * the colour of most of the row's text, and the rows in view outlined; a press or a drag on it scrolls there. A line's
 * bar (where its text starts, how long it is, the token kind of most of it) is made from the line's first bytes when
 * the line first shows on the strip and kept per line: an edit moves the bars below it and makes the lines it touched
 * again, so the strip costs its own rows a frame, not the text. The editor (cui_editor.c) builds the strip's rows and
 * scrolls; this file keeps the bars, works out where the strip and its box are, and paints them. */
#include "cui_internal.h"

void cui__mini_free(cui_ctx *ui, cui__minimap *M) {
    if (!M) return;
    cui__free(ui, M->b);
    cui__free(ui, M);
}

void cui__mini_reset(cui__minimap *M) { if (M) M->n = 0; }

/* one bar a line: all of them made again when the line count is not the one kept */
void cui__mini_lines(cui_ctx *ui, cui__minimap *M, int nl) {
    if (M->n == nl) return;
    if (M->cap < nl) {
        M->cap = nl + nl / 8 + 64;
        M->b = cui__realloc(ui, M->b, sizeof *M->b * (size_t)M->cap);
    }
    memset(M->b, 0, sizeof *M->b * (size_t)nl);
    M->n = nl;
}

/* lines [la, lb] became [la, la + k]: the bars below move, those lines are made again */
void cui__mini_splice(cui_ctx *ui, cui__minimap *M, int la, int lb, int k) {
    if (!M || M->n == 0) return;
    if (la < 0 || lb >= M->n || lb < la || k < 0) { M->n = 0; return; }
    int d = k - (lb - la), n = M->n + d;
    if (n > M->cap) {
        M->cap = n + n / 8 + 64;
        M->b = cui__realloc(ui, M->b, sizeof *M->b * (size_t)M->cap);
    }
    if (d) memmove(M->b + la + k + 1, M->b + lb + 1, sizeof *M->b * (size_t)(M->n - lb - 1));
    memset(M->b + la, 0, sizeof *M->b * (size_t)(k + 1));
    M->n = n;
}

/* the bar of a line: its text's columns and the token kind of most of it (a kind other than plain text when it covers a
 * quarter of the text or more) */
static void cui__mini_make(cui_ctx *ui, cui__mini_bar *b, const cui__lines *L, const cui__lang *lang, bool json, int i, int state) {
    int ls, le;
    const char *q = L->line(L->u, i, &ls, &le);
    memset(b, 0, sizeof *b);
    b->st = (uint8_t)((state & 0x7F) + 1);
    if (!q || le <= ls) return;
    const char *s = q + ls;
    int n = le - ls, tab = L->tab > 0 ? L->tab : 4;
    int col = 0, ind = -1, end = 0;
    for (int k = 0; k < n; k++) {   /* columns: where the text starts and where it ends */
        char c = s[k];
        if (((unsigned char)c & 0xC0) == 0x80) continue;
        if (c == '\t') { col = (col / tab + 1) * tab; continue; }
        if (c == '\r') continue;
        col++;
        if (c != ' ') { if (ind < 0) ind = col - 1; end = col; }
    }
    if (ind < 0) return;
    b->ind = (uint8_t)CUI_MIN(ind, 255);
    b->len = (uint8_t)CUI_MIN(end - ind, 255 - b->ind);
    if (lang->plain) return;
    cui__tok *toks = cui__frame_alloc(ui, sizeof *toks * (size_t)(n + 1));
    int nt;
    if (lang->multi) { int ms = state; nt = cui__tokenize_st(lang, s, n, &ms, toks, n + 1); }
    else { bool block = state > 0; nt = cui__tokenize(lang, json, s, n, &block, toks, n + 1); }
    int count[CUI__TK_COUNT] = { 0 }, all = 0;
    for (int t = 0; t < nt; t++) {
        int a = toks[t].start, z = CUI_MIN(n, a + toks[t].len), kind = toks[t].kind < CUI__TK_COUNT ? toks[t].kind : 0;
        for (int k = a; k < z; k++) if (s[k] != ' ' && s[k] != '\t' && ((unsigned char)s[k] & 0xC0) != 0x80) { count[kind]++; all++; }
    }
    int best = 1;
    for (int k = 2; k < CUI__TK_COUNT; k++) if (count[k] > count[best]) best = k;
    b->kind = (uint8_t)(count[best] > 0 && count[best] * 4 >= all ? best : 0);
}

const cui__mini_bar *cui__mini_bar_of(cui_ctx *ui, cui__minimap *M, const cui__lines *L, const cui__lang *lang, bool json, int i) {
    if (i < 0 || i >= M->n) { static const cui__mini_bar none; return &none; }
    int state = L->state ? L->state(L->u, i) : 0;
    if (state < 0) state = lang->markdown && i == 0 ? CUI__MD_START : 0;
    cui__mini_bar *b = &M->b[i];
    if (b->st != (uint8_t)((state & 0x7F) + 1)) cui__mini_make(ui, b, L, lang, json, i, state);   /* not made, or for another state */
    return b;
}

/* Where the strip is for a text of `rows` rows of lh points, scrolled sy of max_sy, view_h in view, on a strip h high:
 * the text's row at its top, and the box of the rows in view. The box moves k points for each point of scroll. */
void cui__mini_geo_of(cui__mini_geo *g, float h, int rows, double lh, double sy, double max_sy, double view_h) {
    double rh = CUI__MINI_ROW, m = h / rh, t = rows, s = max_sy / lh;
    g->rh = (float)rh;
    g->top = t > m && s > 0 ? sy / max_sy * (t - m) : 0;
    g->k = rh / lh * (t > m && s > 0 ? 1.0 - (t - m) / s : 1.0);
    if (g->k < 1e-6) g->k = 1e-6;
    g->vy = (float)((sy / lh - g->top) * rh);
    g->vh = (float)CUI_MAX(6.0, view_h / lh * rh);
}

void cui__mini_paint_fn(cui_ctx *ui, cui_node *n, void *data) {
    const cui__mini_paint *p = data;
    cui_rect r = n->rect;
    {   /* the wheel over the strip scrolls the text */
        cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
        float s = ui->scale;
        cui_rect cp = { clip.x / s, clip.y / s, clip.w / s, clip.h / s };
        cui__paint_add_hit(ui, p->sid, cui__rect_intersect(r, cp), CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
    }
    cui_push_clip(ui, r);
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, 1, r.h }, CUI_CORNERS(0), p->edge);
    float x0 = r.x + 6, bh = CUI_MAX(1.0f, p->rh - 0.6f), maxw = r.w - 10;
    for (int i = 0; i < p->n; i++) {
        const cui__mini_bar *b = &p->b[i];
        if (!b->len) continue;
        float x = x0 + (float)b->ind * p->cw, w = CUI_MIN((float)b->len * p->cw, maxw - (float)b->ind * p->cw);
        if (w < 0.5f) continue;
        cui_draw_rect(ui, (cui_rect){ x, r.y + p->y[i], CUI_MAX(1.0f, w), bh }, CUI_CORNERS(0), b->kind ? p->colors[b->kind] : p->fg);
    }
    cui_rect v = { r.x + 1, r.y + p->vy, r.w - 1, p->vh };
    cui_draw_rect(ui, v, CUI_CORNERS(0), p->hot ? p->view_hot : p->view);
    cui_pop_clip(ui);
}
