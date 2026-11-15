/* Cell grid: a spreadsheet-like view of a big table (see cui.h, "Cell grid").
 *
 * One node for the whole grid. The rows and columns in view are found with arithmetic (rows have one height) and a
 * binary search over cumulative column widths, the text of those cells is asked for while the grid is built and
 * kept in frame memory, and one paint callback draws backgrounds, lines, clipped text, the selection and the
 * scroll bars. A frame costs the cells on screen, not the table: 10 million rows by 2,000 columns is a few
 * hundred text draws.
 *
 * Scroll offsets are doubles (a float cannot place row 10,000,000 to the pixel). Input is read against last
 * frame's rectangle, as everywhere in CUI; the offsets are applied at once (no easing), so the cells asked for
 * are always the cells drawn.
 *
 * Phases, in order: geometry (sizes, column edges), input (mouse, bars, keys, wheel), gather (the cells in
 * view), build (the node and its paint data), paint. The pure parts (column search, selection keys, scroll into
 * view) are separate functions declared in cui_internal.h so the tests can call them. */
#include "cui_internal.h"

#define CUI__CG_MAX_COLS 16384
#define CUI__CG_DEF_W 120.0f        /* a column width <= 0 */
#define CUI__CG_MIN_W 24.0f         /* the smallest a drag or a fit makes a column */
#define CUI__CG_MAX_W 1200.0f
#define CUI__CG_PAD 8.0f            /* text inset in a cell */
#define CUI__CG_EDGE 4.0f           /* a header edge grabs within this many points */
#define CUI__CG_BAR 10.0f           /* the scroll bar zones */
#define CUI__CG_MAX_ITEMS 24000     /* cells kept per frame, however small the cells are */
#define CUI__CG_TEXT 1024           /* bytes of a cell's text that are measured and drawn */
#define CUI__CG_FALLBACK_W 1200.0f  /* the view before the first layout */
#define CUI__CG_FALLBACK_H 800.0f

enum { CUI__CG_DRAG_NONE, CUI__CG_DRAG_CELL, CUI__CG_DRAG_ROW, CUI__CG_DRAG_VBAR, CUI__CG_DRAG_HBAR,
       CUI__CG_DRAG_RESIZE, CUI__CG_DRAG_HEADER };
enum { CUI__CG_OUT, CUI__CG_CORNER, CUI__CG_HEAD, CUI__CG_ROWNUM, CUI__CG_BODY, CUI__CG_VBAR, CUI__CG_HBAR };

/* advance widths of the ASCII characters at one font and size, so a cell of plain text is measured by adding
 * numbers and drawn in one pass over its glyphs (kerning, which only ever tightens text a little, is left out) */
typedef struct cui__cg_adv {
    float w[128];
    float ell;                    /* the width of an ellipsis */
    float size;
    int font;
    bool tab, ok;
} cui__cg_adv;

typedef struct cui__cg_state {
    double x, y;                  /* scroll offsets in points (0 = the first column and row at the frame) */
    double *edges;                /* ncols + 1 cumulative column edges; one block with `seen` */
    float *seen;                  /* the raw widths `edges` was built from, to notice a change */
    int ncols;
    cui_cellgrid_sel last;        /* the selection at the end of last frame: a different one now was set from outside */
    cui_cellgrid_sel own;         /* the selection when the caller gives none */
    int drag, drag_col, drag_row;
    float press_x, start_w;
    double grab;                  /* bar drag: where in the thumb the pointer holds it, in points */
    double last_activity;
    float bar_t;
    int rn_rows;                  /* the row-number width below was measured for these rows and this size */
    float rn_size, rn_w;
    cui__cg_adv adv[3];           /* the body font, the body font with tabular figures, the header font */
    bool init, want_vis;          /* the cursor still has to be scrolled into view (no layout yet) */
} cui__cg_state;

typedef struct { const char *s; int len; } cui__cg_item;

/* what the paint callback needs, in frame memory; positions are in points from the grid's top left corner */
typedef struct cui__cg_paint {
    int nr, nc, r0, c0;
    float row_y0;                 /* y of row r0's top, from the body's top */
    float rh, head_h, rn_w, size, fh, pad, s;
    float cols_end, rows_end;     /* where the last column and the last row end */
    float *lx;                    /* nc + 1 column edges */
    cui__cg_item *cells, *hdr, *rn;
    const cui__cg_adv *adv;       /* the three tables of the state */
    unsigned char *right;         /* nc */
    bool striped, lines, has_head, has_rn, focused, has_sel, multi, sortable, ring;
    cui_rect selr, curr;          /* the selection and the cursor cell, clamped to a little outside the body */
    float hl_x0, hl_x1, hl_y0, hl_y1;   /* the selected columns and rows, for the header and the row numbers */
    int sort_i, hover_i;          /* header index of the sorted column and of the hovered one, or -1 */
    bool sort_desc;
    bool vbar, hbar, big_v, big_h;
    float bar_t, vpos, vlen, hpos, hlen;
    cui_id wid;
} cui__cg_paint;

/* what one call works with */
typedef struct cui__cg {
    cui_ctx *ui;
    const cui_cellgrid_opts *o;
    cui__cg_state *st;
    cui_cellgrid_sel *sel;
    cui_rect pr;                  /* last frame's rectangle */
    bool known;                   /* pr is a real layout (else the view is assumed to be CUI__CG_FALLBACK_*) */
    int rows, cols;
    float s, size, fh, rh, head_h, rn_w, pad;
    float view_w, view_h, body_w, body_h;
    double total_w, total_h, max_x, max_y;
    cui_font font, hfont;
    int ev;
} cui__cg;

/* ------------------------------------------------------------------------ */
/* Pure parts                                                               */
/* ------------------------------------------------------------------------ */

/* The column with edges[k] <= x < edges[k + 1], or -1 when x is left of the first or past the last edge. edges has
 * cols + 1 ascending entries. */
int cui__cg_col_at(const double *edges, int cols, double x) {
    if (cols <= 0 || !(x >= edges[0]) || x >= edges[cols]) return -1;
    int lo = 0, hi = cols - 1;   /* the answer is in [lo, hi] */
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (edges[mid] <= x) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* One navigation key applied to a selection. Arrows move the cursor, Page Up and Down move it by `page` rows, Home
 * and End go to the row's first and last cell, Ctrl+Home and Ctrl+End to the first and last cell of the table,
 * Ctrl with an arrow to the edge of the table, Ctrl+A selects everything. Shift keeps the anchor (a range), else the
 * anchor follows the cursor. With no selection yet, the first key puts the cursor on the first cell. Returns
 * CUI__CG_NAV_NONE for a key that is not one of these, else whether the selection changed and whether the view
 * should follow the cursor. */
int cui__cg_nav(cui_cellgrid_sel *s, int rows, int cols, cui_key key, int mods, int page) {
    if (rows <= 0 || cols <= 0 || (mods & CUI_MOD_ALT)) return CUI__CG_NAV_NONE;
    bool shift = (mods & CUI_MOD_SHIFT) != 0, ctrl = (mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) != 0;
    cui_cellgrid_sel was = *s;
    if (key == CUI_KEY_A) {
        if (!ctrl || shift) return CUI__CG_NAV_NONE;
        s->anchor_row = 0; s->anchor_col = 0; s->row = rows - 1; s->col = cols - 1;
        return cui__cg_sel_same(&was, s) ? CUI__CG_NAV_SAME : CUI__CG_NAV_ALL;
    }
    bool none = s->row < 0 || s->col < 0;
    int r = none ? 0 : CUI_CLAMP(s->row, 0, rows - 1), c = none ? 0 : CUI_CLAMP(s->col, 0, cols - 1);
    int nr = r, nc = c;
    if (page < 1) page = 1;
    switch (key) {
    case CUI_KEY_LEFT:      nc = ctrl ? 0 : c - 1; break;
    case CUI_KEY_RIGHT:     nc = ctrl ? cols - 1 : c + 1; break;
    case CUI_KEY_UP:        nr = ctrl ? 0 : r - 1; break;
    case CUI_KEY_DOWN:      nr = ctrl ? rows - 1 : r + 1; break;
    case CUI_KEY_PAGE_UP:   nr = r - page; break;
    case CUI_KEY_PAGE_DOWN: nr = r + page; break;
    case CUI_KEY_HOME:      nc = 0; if (ctrl) nr = 0; break;
    case CUI_KEY_END:       nc = cols - 1; if (ctrl) nr = rows - 1; break;
    default: return CUI__CG_NAV_NONE;
    }
    nr = CUI_CLAMP(nr, 0, rows - 1); nc = CUI_CLAMP(nc, 0, cols - 1);
    s->row = nr; s->col = nc;
    if (!shift) { s->anchor_row = nr; s->anchor_col = nc; }
    else if (none || s->anchor_row < 0 || s->anchor_col < 0) { s->anchor_row = r; s->anchor_col = c; }   /* a range from where the cursor was */
    return cui__cg_sel_same(&was, s) ? CUI__CG_NAV_SAME : CUI__CG_NAV_MOVED;
}

/* The scroll offset that shows [lo, hi) in a view of `view` points, moving as little as possible; a span wider
 * than the view shows its start. */
double cui__cg_into_view(double scroll, double view, double lo, double hi) {
    if (hi - lo >= view || lo < scroll) return lo;
    if (hi > scroll + view) return hi - view;
    return scroll;
}

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static void cui__cg_free(cui_ctx *ui, void *data) {
    cui__cg_state *st = (cui__cg_state *)data;
    cui__free(ui, st->edges);
    st->edges = NULL; st->seen = NULL; st->ncols = 0;
}

static float cui__cg_width(float w) {
    if (!(w > 0)) return CUI__CG_DEF_W;
    return CUI_CLAMP(w, 2.0f, 100000.0f);
}

/* The cumulative column edges, rebuilt only when the column count or a width changed. */
static void cui__cg_edges(cui_ctx *ui, cui__cg_state *st, const float *w, int cols) {
    if (st->edges && st->ncols == cols && (cols == 0 || memcmp(st->seen, w, (size_t)cols * sizeof *w) == 0)) return;
    if (!st->edges || st->ncols != cols) {
        cui__free(ui, st->edges);
        size_t n = (size_t)cols + 1;
        st->edges = (double *)cui__alloc(ui, n * sizeof(double) + (size_t)cols * sizeof(float) + sizeof(float));
        st->seen = (float *)(void *)(st->edges + n);
        st->ncols = cols;
    }
    if (cols > 0) memcpy(st->seen, w, (size_t)cols * sizeof *w);
    st->edges[0] = 0;
    for (int k = 0; k < cols; k++) st->edges[k + 1] = st->edges[k] + (double)cui__cg_width(w[k]);
}

/* A selection inside the table: ranges that no longer exist are pulled in. */
static void cui__cg_clamp_sel(cui_cellgrid_sel *s, int rows, int cols) {
    if (rows <= 0 || cols <= 0 || s->row < 0 || s->col < 0) {
        s->row = s->col = s->anchor_row = s->anchor_col = -1;
        return;
    }
    s->row = CUI_MIN(s->row, rows - 1); s->col = CUI_MIN(s->col, cols - 1);
    if (s->anchor_row < 0 || s->anchor_col < 0) { s->anchor_row = s->row; s->anchor_col = s->col; }
    s->anchor_row = CUI_MIN(s->anchor_row, rows - 1); s->anchor_col = CUI_MIN(s->anchor_col, cols - 1);
}

/* ------------------------------------------------------------------------ */
/* Cells                                                                    */
/* ------------------------------------------------------------------------ */

/* A cell's text for drawing or measuring: asked from the callback, cut to what could be seen in max_w points, line
 * breaks and tabs turned into spaces. Written to dst (CUI__CG_TEXT + 1 bytes); returns its length. */
static int cui__cg_fetch(const cui__cg *g, int row, int col, float max_w, char *dst) {
    char raw[512];
    raw[0] = 0;
    const char *t = g->o->cell(g->o->ctx, row, col, raw, sizeof raw);
    if (!t) return 0;
    int cap = (int)(max_w / (g->size * 0.2f)) + 4;   /* no glyph is narrower than a fifth of the size */
    cap = CUI_MIN(cap * 4, CUI__CG_TEXT);
    int n = 0;
    while (n < cap && t[n]) { dst[n] = (unsigned char)t[n] < 0x20 ? ' ' : t[n]; n++; }
    if (n == cap && t[n]) while (n > 0 && ((unsigned char)t[n] & 0xC0) == 0x80) n--;   /* not inside a character */
    dst[n] = 0;
    return n;
}

static cui__cg_item cui__cg_keep(cui_ctx *ui, const char *tmp, int len) {
    cui__cg_item it = { NULL, 0 };
    if (len <= 0) return it;
    char *p = (char *)cui__frame_alloc(ui, (size_t)len + 1);
    memcpy(p, tmp, (size_t)len);
    it.s = p; it.len = len;
    return it;
}

/* The width of the row-number column: the widest label is taken to be the last row's. */
static float cui__cg_rn_width(cui__cg *g) {
    cui__cg_state *st = g->st;
    if (st->rn_rows == g->rows && st->rn_size == g->size && st->rn_w > 0) return st->rn_w;
    char tmp[CUI__CG_TEXT + 1];
    float w = 0;
    if (g->rows > 0) {
        int n = cui__cg_fetch(g, g->rows - 1, -1, 400, tmp);
        w = cui__text_width(g->ui, g->font, g->size, tmp, n, true);
    }
    st->rn_w = CUI_MAX(ceilf(w + 2 * g->pad), 32.0f);
    st->rn_rows = g->rows; st->rn_size = g->size;
    return st->rn_w;
}

/* ------------------------------------------------------------------------ */
/* Geometry                                                                 */
/* ------------------------------------------------------------------------ */

/* The advance table number k, made again when the font or the size changed. */
static void cui__cg_adv_get(cui__cg *g, int k, cui_font font, bool tab) {
    cui__cg_adv *a = &g->st->adv[k];
    int rf = (int)cui__resolve_font(g->ui, font);
    if (a->ok && a->font == rf && a->size == g->size) return;
    char c[2] = { 0, 0 };
    for (int i = 0; i < 128; i++) {
        c[0] = (char)(i < 32 ? ' ' : i);
        a->w[i] = cui__text_width(g->ui, font, g->size, c, 1, tab);
    }
    a->ell = cui__text_width(g->ui, font, g->size, "\xe2\x80\xa6", 3, false);
    a->font = rf; a->size = g->size; a->tab = tab; a->ok = true;
}

/* The width of text made of ASCII from the table, or -1 when it holds anything else. */
static float cui__cg_est(const cui__cg_adv *a, const char *s, int len) {
    float t = 0;
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 128) return -1.0f;
        t += a->w[c];
    }
    return t;
}

static void cui__cg_geometry(cui__cg *g) {
    cui_ctx *ui = g->ui;
    const cui_cellgrid_opts *o = g->o;
    g->s = ui->scale > 0 ? ui->scale : 1.0f;
    g->size = cui__fs(ui, o->font_size > 0 ? o->font_size : ui->theme.text_body);
    g->font = CUI_FONT_BODY; g->hfont = CUI_FONT_MEDIUM;
    g->fh = cui__font_line_height(ui, g->font, g->size);
    float rh = o->row_height > 0 ? o->row_height : ceilf(g->fh + 10.0f);
    rh = CUI_MAX(rh, 8.0f);
    g->rh = CUI_MAX(floorf(rh * g->s + 0.5f), 1.0f) / g->s;   /* whole device pixels: the lines stay sharp */
    g->pad = CUI__CG_PAD;
    g->head_h = o->header ? g->rh : 0;
    g->rn_w = o->row_numbers ? cui__cg_rn_width(g) : 0;
    g->view_w = g->known ? g->pr.w : CUI__CG_FALLBACK_W;
    g->view_h = g->known ? g->pr.h : CUI__CG_FALLBACK_H;
    g->body_w = CUI_MAX(g->view_w - g->rn_w, 0.0f);
    g->body_h = CUI_MAX(g->view_h - g->head_h, 0.0f);
    g->total_w = g->st->edges[g->cols];
    g->total_h = (double)g->rows * (double)g->rh;
    g->max_x = CUI_MAX(0.0, g->total_w - (double)g->body_w);
    g->max_y = CUI_MAX(0.0, g->total_h - (double)g->body_h);
    cui__cg_adv_get(g, 0, g->font, false);
    cui__cg_adv_get(g, 1, g->font, true);
    cui__cg_adv_get(g, 2, g->hfont, false);
}

static void cui__cg_clamp_scroll(cui__cg *g) {
    g->st->x = CUI_CLAMP(g->st->x, 0.0, g->max_x);
    g->st->y = CUI_CLAMP(g->st->y, 0.0, g->max_y);
}

/* Scrolls so that the cell shows, moving as little as possible. */
static void cui__cg_reveal(cui__cg *g, int row, int col) {
    if (row < 0 || col < 0 || row >= g->rows || col >= g->cols) return;
    if (!g->known) { g->st->want_vis = true; return; }
    cui__cg_state *st = g->st;
    st->y = cui__cg_into_view(st->y, (double)g->body_h, (double)row * (double)g->rh, (double)(row + 1) * (double)g->rh);
    st->x = cui__cg_into_view(st->x, (double)g->body_w, st->edges[col], st->edges[col + 1]);
    cui__cg_clamp_scroll(g);
}

static int cui__cg_row_at(const cui__cg *g, double y_in_body) {
    double r = floor((g->st->y + y_in_body) / (double)g->rh);
    return (int)CUI_CLAMP(r, 0.0, (double)(g->rows - 1));
}

/* the column at x, which may be left of the first or past the last (then the nearest) */
static int cui__cg_col_near(const cui__cg_state *st, int cols, double x) {
    int c = cui__cg_col_at(st->edges, cols, x);
    if (c >= 0) return c;
    return x < 0 ? 0 : cols - 1;
}

static int cui__cg_col_clamped(const cui__cg *g, double x_in_body) {
    return cui__cg_col_near(g->st, g->cols, g->st->x + x_in_body);
}

/* The column whose right edge is within the grab zone of lx (points from the grid's left), or -1. */
static int cui__cg_edge_at(const cui__cg *g, float lx) {
    double xx = g->st->x + (double)(lx - g->rn_w);
    if (g->cols <= 0 || xx < 0) return -1;
    const double *e = g->st->edges;
    int c = cui__cg_col_at(e, g->cols, xx);
    if (c < 0) return xx - e[g->cols] <= (double)CUI__CG_EDGE ? g->cols - 1 : -1;
    if (e[c + 1] - xx <= (double)CUI__CG_EDGE) return c;
    if (c > 0 && xx - e[c] <= (double)CUI__CG_EDGE) return c - 1;
    return -1;
}

static int cui__cg_region(const cui__cg *g, float lx, float ly) {
    if (lx < 0 || ly < 0 || lx >= g->view_w || ly >= g->view_h) return CUI__CG_OUT;
    if (g->max_y > 0 && lx >= g->view_w - CUI__CG_BAR && ly >= g->head_h) return CUI__CG_VBAR;
    if (g->max_x > 0 && ly >= g->view_h - CUI__CG_BAR && lx >= g->rn_w) return CUI__CG_HBAR;
    if (ly < g->head_h) return lx < g->rn_w ? CUI__CG_CORNER : CUI__CG_HEAD;
    return lx < g->rn_w ? CUI__CG_ROWNUM : CUI__CG_BODY;
}

/* A scroll bar thumb: its start and length in points along the track of `view` points. */
static void cui__cg_thumb(double view, double content, double scroll, double max, float *pos, float *len) {
    double tl = CUI_MAX(28.0, view * view / CUI_MAX(content, 1.0));
    tl = CUI_MIN(tl, CUI_MAX(view - 6.0, 1.0));
    double track = CUI_MAX(view - tl - 6.0, 1.0);
    *len = (float)tl;
    *pos = (float)(3.0 + (max > 0 ? scroll / max * track : 0.0));
}

/* ------------------------------------------------------------------------ */
/* Input                                                                    */
/* ------------------------------------------------------------------------ */

/* The width that fits a column's header and the cells in view. */
static float cui__cg_fit(cui__cg *g, int col) {
    cui_ctx *ui = g->ui;
    char tmp[CUI__CG_TEXT + 1];
    float w = 0;
    if (g->o->header) {
        int n = cui__cg_fetch(g, -1, col, 2000, tmp);
        w = cui__text_width(ui, g->hfont, g->size, tmp, n, false) + (g->o->sort ? 18.0f : 0.0f);
    }
    int r0 = (int)CUI_MAX(floor(g->st->y / (double)g->rh), 0.0);
    int r1 = (int)CUI_MIN(ceil((g->st->y + (double)g->body_h) / (double)g->rh), (double)g->rows);
    bool right = g->o->right_align && g->o->right_align[col];
    for (int r = r0; r < r1; r++) {
        int n = cui__cg_fetch(g, r, col, 2000, tmp);
        w = CUI_MAX(w, cui__text_width(ui, g->font, g->size, tmp, n, right));
    }
    return CUI_CLAMP(ceilf(w + 2 * g->pad + 2), CUI__CG_MIN_W, CUI__CG_MAX_W);
}

static void cui__cg_set(cui__cg *g, int r, int c, int ar, int ac) {
    g->sel->row = r; g->sel->col = c; g->sel->anchor_row = ar; g->sel->anchor_col = ac;
}

/* Selects a row (or rows, from the anchor row) of every column. */
static void cui__cg_select_rows(cui__cg *g, int row, int anchor_row) {
    cui__cg_set(g, row, 0, anchor_row, g->cols - 1);
}

static void cui__cg_press(cui__cg *g, int region, float lx, float ly) {
    cui_ctx *ui = g->ui;
    cui__cg_state *st = g->st;
    bool shift = (ui->mods & CUI_MOD_SHIFT) != 0;
    bool had = g->sel->row >= 0 && g->sel->col >= 0;
    if (region == CUI__CG_VBAR || region == CUI__CG_HBAR) {
        bool v = region == CUI__CG_VBAR;
        float pos, len;
        double view = v ? (double)g->body_h : (double)g->body_w;
        cui__cg_thumb(view, v ? g->total_h : g->total_w, v ? st->y : st->x, v ? g->max_y : g->max_x, &pos, &len);
        double at = v ? (double)(ly - g->head_h) : (double)(lx - g->rn_w);
        st->grab = at >= (double)pos && at <= (double)(pos + len) ? at - (double)pos : (double)len * 0.5;
        st->drag = v ? CUI__CG_DRAG_VBAR : CUI__CG_DRAG_HBAR;
        st->last_activity = ui->time;
    } else if (region == CUI__CG_HEAD) {
        int e = cui__cg_edge_at(g, lx);
        if (e >= 0) {
            if (ui->click_count >= 2) {
                g->o->col_widths[e] = cui__cg_fit(g, e);
                g->ev |= CUI_CELLGRID_RESIZED;
            } else {
                st->drag = CUI__CG_DRAG_RESIZE; st->drag_col = e;
                st->press_x = ui->mx; st->start_w = cui__cg_width(g->o->col_widths[e]);
            }
        } else if (g->o->sort) {
            st->drag = CUI__CG_DRAG_HEADER;
            st->drag_col = cui__cg_col_clamped(g, (double)(lx - g->rn_w));
        }
    } else if (region == CUI__CG_CORNER) {
        if (g->rows > 0 && g->cols > 0) cui__cg_set(g, g->rows - 1, g->cols - 1, 0, 0);   /* everything; the view stays */
    } else if (region == CUI__CG_ROWNUM && g->rows > 0 && g->cols > 0) {
        int r = cui__cg_row_at(g, (double)(ly - g->head_h));
        int a = shift && had ? g->sel->anchor_row : r;
        cui__cg_select_rows(g, r, a);
        st->drag = CUI__CG_DRAG_ROW; st->drag_row = a;
    } else if (region == CUI__CG_BODY && g->rows > 0 && g->cols > 0) {
        int r = cui__cg_row_at(g, (double)(ly - g->head_h)), c = cui__cg_col_clamped(g, (double)(lx - g->rn_w));
        if (shift && had) cui__cg_set(g, r, c, g->sel->anchor_row, g->sel->anchor_col);
        else cui__cg_set(g, r, c, r, c);
        st->drag = CUI__CG_DRAG_CELL;
        if (ui->click_count >= 2 && !shift) g->ev |= CUI_CELLGRID_ACTIVATE;
        cui__cg_reveal(g, r, c);
    }
}

/* A held button: drag a selection (scrolling when the pointer leaves the body), a bar or a column edge. */
static void cui__cg_drag(cui__cg *g) {
    cui_ctx *ui = g->ui;
    cui__cg_state *st = g->st;
    float lx = ui->mx - g->pr.x, ly = ui->my - g->pr.y;
    switch (st->drag) {
    case CUI__CG_DRAG_CELL: case CUI__CG_DRAG_ROW: {
        if (g->rows <= 0 || g->cols <= 0) break;
        float by = ly - g->head_h, bx = lx - g->rn_w;
        float dy = by < 0 ? by : (by > g->body_h ? by - g->body_h : 0);
        float dx = bx < 0 ? bx : (bx > g->body_w ? bx - g->body_w : 0);
        if (dy != 0) st->y += (double)((dy < 0 ? -1.0f : 1.0f) * (fabsf(dy) + 12.0f) * 14.0f * ui->dt);
        if (dx != 0 && st->drag == CUI__CG_DRAG_CELL) st->x += (double)((dx < 0 ? -1.0f : 1.0f) * (fabsf(dx) + 12.0f) * 14.0f * ui->dt);
        if (dx != 0 || dy != 0) { cui__cg_clamp_scroll(g); ui->animating = true; }
        int r = cui__cg_row_at(g, (double)CUI_CLAMP(by, 0.0f, CUI_MAX(g->body_h - 1.0f, 0.0f)));
        if (st->drag == CUI__CG_DRAG_ROW) cui__cg_select_rows(g, r, st->drag_row);
        else {
            int c = cui__cg_col_clamped(g, (double)CUI_CLAMP(bx, 0.0f, CUI_MAX(g->body_w - 1.0f, 0.0f)));
            cui__cg_set(g, r, c, g->sel->anchor_row, g->sel->anchor_col);
        }
        break;
    }
    case CUI__CG_DRAG_VBAR: case CUI__CG_DRAG_HBAR: {
        bool v = st->drag == CUI__CG_DRAG_VBAR;
        double view = v ? (double)g->body_h : (double)g->body_w, max = v ? g->max_y : g->max_x;
        float pos, len;
        cui__cg_thumb(view, v ? g->total_h : g->total_w, 0, max, &pos, &len);
        double track = CUI_MAX(view - (double)len - 6.0, 1.0);
        double at = v ? (double)(ly - g->head_h) : (double)(lx - g->rn_w);
        double f = (at - 3.0 - st->grab) / track;
        if (v) st->y = CUI_CLAMP(f, 0.0, 1.0) * max; else st->x = CUI_CLAMP(f, 0.0, 1.0) * max;
        st->last_activity = ui->time;
        break;
    }
    case CUI__CG_DRAG_RESIZE: {
        float nw = CUI_CLAMP(st->start_w + (ui->mx - st->press_x), CUI__CG_MIN_W, CUI__CG_MAX_W);
        if (nw != g->o->col_widths[st->drag_col]) { g->o->col_widths[st->drag_col] = nw; g->ev |= CUI_CELLGRID_RESIZED; }
        break;
    }
    default: break;
    }
}

static void cui__cg_keys(cui__cg *g) {
    cui_ctx *ui = g->ui;
    static const cui_key nav[] = { CUI_KEY_LEFT, CUI_KEY_RIGHT, CUI_KEY_UP, CUI_KEY_DOWN, CUI_KEY_HOME, CUI_KEY_END,
                                   CUI_KEY_PAGE_UP, CUI_KEY_PAGE_DOWN, CUI_KEY_A };
    int page = CUI_MAX((int)(g->body_h / g->rh) - 1, 1);
    bool follow = false;
    for (int k = 0; k < CUI_COUNTOF(nav); k++) {
        if (!cui_key_pressed(ui, nav[k])) continue;
        int res = cui__cg_nav(g->sel, g->rows, g->cols, nav[k], ui->mods, page);
        if (res == CUI__CG_NAV_NONE) continue;
        cui_key_consume(ui, nav[k]);
        follow = follow || res == CUI__CG_NAV_MOVED;
    }
    if (follow) cui__cg_reveal(g, g->sel->row, g->sel->col);
    int plain = ui->mods & (CUI_MOD_SHIFT | CUI_MOD_CTRL | CUI_MOD_ALT | CUI_MOD_SUPER);
    if (g->sel->row >= 0 && plain == 0 && cui_key_pressed(ui, CUI_KEY_ENTER)) {
        g->ev |= CUI_CELLGRID_ACTIVATE;
        cui_key_consume(ui, CUI_KEY_ENTER);
    }
    if (plain == CUI_MOD_PRIMARY && cui_key_pressed(ui, CUI_KEY_C)) {
        g->ev |= CUI_CELLGRID_COPY;
        cui_key_consume(ui, CUI_KEY_C);
    }
}

/* ------------------------------------------------------------------------ */
/* Paint                                                                    */
/* ------------------------------------------------------------------------ */

static inline float cui__cg_snap(float v, float s) { return floorf(v * s + 0.5f) / s; }

static cui_color cui__cg_sel_fill(cui_ctx *ui) {
    return ui->accent == CUI_ACCENT_NEUTRAL ? CUI_C(ui, ACTIVE)
        : cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, ACCENT), ui->mode == CUI_THEME_DARK ? 0.16f : 0.09f);
}

/* A cell's text, inset, cut with an ellipsis; right aligned when it fits. Plain ASCII is measured from the advance
 * table `a` and drawn in one pass (a cut line is cut from the table too); anything else, and a width within a
 * few percent of the cell's, is measured exactly by the text code. `tab`: tabular figures. */
static void cui__cg_draw_item(cui_ctx *ui, const cui__cg_paint *p, const cui__cg_adv *a, cui_font font, float x, float y,
                              float w, float reserve, bool right, bool tab, cui_color col, const cui__cg_item *it) {
    float maxw = w - 2 * p->pad - reserve;
    if (maxw <= 1 || it->len == 0) return;
    float ty = cui__cg_snap(y + (p->rh - p->fh) * 0.5f, p->s);
    float est = cui__cg_est(a, it->s, it->len), margin = CUI_MAX(1.5f, est * 0.03f);
    if (est >= 0 && est + margin <= maxw) {            /* it fits */
        float tx = right ? x + w - p->pad - reserve - est : x + p->pad;
        cui__draw_text_line(ui, font, p->size, tx, ty, col, it->s, it->len, tab);
        return;
    }
    if (est >= 0 && est - margin > maxw) {             /* it does not: cut where the table says, then an ellipsis */
        float avail = maxw - a->ell, acc = 0, cut_w = 0;
        int cut = 0;
        for (int i = 0; i < it->len; i++) {
            acc += a->w[(unsigned char)it->s[i]];
            if (acc > avail) break;
            cut = i + 1; cut_w = acc;
        }
        while (cut > 0 && it->s[cut - 1] == ' ') { cut--; cut_w -= a->w[' ']; }
        if (avail <= 0) return;
        cui__draw_text_line(ui, font, p->size, x + p->pad, ty, col, it->s, cut, tab);
        cui__draw_text_line(ui, font, p->size, x + p->pad + cut_w, ty, col, "\xe2\x80\xa6", 3, false);
        return;
    }
    float tx = x + p->pad;
    if (right) {
        float tw = cui__text_width(ui, font, p->size, it->s, it->len, tab);
        if (tw <= maxw) tx = x + w - p->pad - reserve - tw;
    }
    cui__draw_text_ellipsis(ui, font, p->size, tx, ty, maxw, col, it->s, it->len, tab);
}

static void cui__cg_outline(cui_ctx *ui, cui_rect r, float th, cui_color c) {
    if (r.w <= 0 || r.h <= 0) return;
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, r.w, th }, CUI_CORNERS(0), c);
    cui_draw_rect(ui, (cui_rect){ r.x, r.y + r.h - th, r.w, th }, CUI_CORNERS(0), c);
    cui_draw_rect(ui, (cui_rect){ r.x, r.y + th, th, r.h - 2 * th }, CUI_CORNERS(0), c);
    cui_draw_rect(ui, (cui_rect){ r.x + r.w - th, r.y + th, th, r.h - 2 * th }, CUI_CORNERS(0), c);
}

static void cui__cg_paint_body(cui_ctx *ui, const cui__cg_paint *p, cui_rect R, cui_rect B) {
    float s = p->s, px = 1.0f / s;
    cui_color fg = CUI_C(ui, FOREGROUND), subtle = CUI_C(ui, BORDER_SUBTLE), edge = cui__emphasis(ui);
    cui_push_clip(ui, B);
    float top = R.y + p->head_h + p->row_y0;   /* row r0's top */
    if (p->striped) {
        cui_color stripe = cui_alpha(fg, 0.025f);
        for (int i = 0; i < p->nr; i++)
            if ((p->r0 + i) & 1) cui_draw_rect(ui, (cui_rect){ B.x, top + (float)i * p->rh, B.w, p->rh }, CUI_CORNERS(0), stripe);
    }
    cui_rect sr = { R.x + p->selr.x, R.y + p->selr.y, p->selr.w, p->selr.h };
    cui_rect cr = { R.x + p->curr.x, R.y + p->curr.y, p->curr.w, p->curr.h };
    if (p->has_sel && p->multi) {
        cui_draw_rect(ui, sr, CUI_CORNERS(0), cui__cg_sel_fill(ui));
        cui_draw_rect(ui, cr, CUI_CORNERS(0), CUI_C(ui, SURFACE));
    }
    if (p->lines) {
        float y1 = CUI_MIN(R.y + p->rows_end, B.y + B.h), x1 = CUI_MIN(R.x + p->cols_end, B.x + B.w);
        for (int i = 0; i <= p->nc; i++) {
            float x = cui__cg_snap(R.x + p->lx[i], s) - px;
            if (x < B.x - 1 || x > B.x + B.w || y1 <= B.y) continue;
            cui_draw_rect(ui, (cui_rect){ x, B.y, px, y1 - B.y }, CUI_CORNERS(0), subtle);
        }
        for (int i = 0; i < p->nr; i++) {
            float y = cui__cg_snap(top + (float)(i + 1) * p->rh, s) - px;
            if (y > B.y + B.h || x1 <= B.x) continue;
            cui_draw_rect(ui, (cui_rect){ B.x, y, x1 - B.x, px }, CUI_CORNERS(0), subtle);
        }
    }
    for (int i = 0; i < p->nr; i++) {
        float y = top + (float)i * p->rh;
        if (y > B.y + B.h || y + p->rh < B.y) continue;
        for (int j = 0; j < p->nc; j++) {
            const cui__cg_item *it = &p->cells[i * p->nc + j];
            if (!it->len) continue;
            float x = R.x + p->lx[j];
            if (x > B.x + B.w || R.x + p->lx[j + 1] < B.x) continue;
            cui__cg_draw_item(ui, p, &p->adv[p->right[j] ? 1 : 0], CUI_FONT_BODY, x, y, p->lx[j + 1] - p->lx[j], 0, p->right[j] != 0, p->right[j] != 0, fg, it);
        }
    }
    if (p->has_sel) {
        if (p->multi) cui__cg_outline(ui, sr, 1.0f, cui_alpha(edge, 0.55f));
        cui__cg_outline(ui, cr, 2.0f, p->focused ? edge : cui_alpha(edge, 0.55f));
    }
    cui_pop_clip(ui);
}

/* the header band and the frozen row-number column, over the body */
static void cui__cg_paint_frozen(cui_ctx *ui, const cui__cg_paint *p, cui_rect R) {
    float s = p->s, px = 1.0f / s;
    float ir = CUI_MAX(0.0f, ui->theme.radius_control - 1);
    cui_color muted = CUI_C(ui, SURFACE_MUTED), subtle = CUI_C(ui, BORDER_SUBTLE), line = CUI_C(ui, BORDER);
    cui_color sel_fill = cui__cg_sel_fill(ui);
    float bw = R.w - p->rn_w, bh = R.h - p->head_h;
    if (p->has_head) {
        cui_draw_rect(ui, (cui_rect){ R.x, R.y, R.w, p->head_h }, (cui_corners){ ir, ir, 0, 0 }, muted);
        if (p->has_sel && p->hl_x1 > p->hl_x0)
            cui_draw_rect(ui, (cui_rect){ R.x + p->hl_x0, R.y, p->hl_x1 - p->hl_x0, p->head_h }, CUI_CORNERS(0), sel_fill);
        if (p->hover_i >= 0 && p->hover_i < p->nc)
            cui_draw_rect(ui, (cui_rect){ R.x + p->lx[p->hover_i], R.y, p->lx[p->hover_i + 1] - p->lx[p->hover_i], p->head_h }, CUI_CORNERS(0), CUI_C(ui, HOVER));
        if (bw > 0) {
            cui_push_clip(ui, (cui_rect){ R.x + p->rn_w, R.y, bw, p->head_h });
            for (int j = 0; j < p->nc; j++) {
                float x = R.x + p->lx[j], w = p->lx[j + 1] - p->lx[j];
                bool sorted = j == p->sort_i;
                cui_color col = sorted ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_SECONDARY);
                float reserve = p->sortable && (sorted || j == p->hover_i) ? 16.0f : 0.0f;
                cui__cg_draw_item(ui, p, &p->adv[2], CUI_FONT_MEDIUM, x, R.y, w, reserve, p->right[j] != 0, false, col, &p->hdr[j]);
                if (reserve > 0 && w > 2 * p->pad + 16) {
                    const char *icon = sorted ? (p->sort_desc ? CUI_ICON_ARROW_DOWN : CUI_ICON_ARROW_UP) : CUI_ICON_CHEVRONS_UP_DOWN;
                    cui_color ic = sorted ? CUI_C(ui, FOREGROUND) : cui_alpha(CUI_C(ui, TEXT_MUTED), 0.7f);
                    cui_draw_icon(ui, icon, (cui_rect){ x + w - p->pad - 12, R.y + (p->head_h - 12) * 0.5f, 12, 12 }, 12, ic);
                }
                float sx = cui__cg_snap(R.x + p->lx[j + 1], s) - px;
                cui_draw_rect(ui, (cui_rect){ sx, R.y + p->head_h * 0.25f, px, p->head_h * 0.5f }, CUI_CORNERS(0), subtle);
            }
            cui_pop_clip(ui);
        }
        cui_draw_rect(ui, (cui_rect){ R.x, R.y + p->head_h - px, R.w, px }, CUI_CORNERS(0), line);
    }
    if (p->has_rn && bh > 0) {
        cui_draw_rect(ui, (cui_rect){ R.x, R.y + p->head_h, p->rn_w, bh }, (cui_corners){ p->has_head ? 0.0f : ir, 0, 0, ir }, muted);
        cui_push_clip(ui, (cui_rect){ R.x, R.y + p->head_h, p->rn_w, bh });
        if (p->has_sel && p->hl_y1 > p->hl_y0)
            cui_draw_rect(ui, (cui_rect){ R.x, R.y + p->hl_y0, p->rn_w, p->hl_y1 - p->hl_y0 }, CUI_CORNERS(0), sel_fill);
        float top = R.y + p->head_h + p->row_y0;
        for (int i = 0; i < p->nr; i++) {
            float y = top + (float)i * p->rh;
            if (y > R.y + R.h || y + p->rh < R.y + p->head_h) continue;
            bool in_sel = p->has_sel && y + p->rh > R.y + p->hl_y0 && y < R.y + p->hl_y1;
            cui__cg_draw_item(ui, p, &p->adv[1], CUI_FONT_BODY, R.x, y, p->rn_w, 0, true, true, in_sel ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_MUTED), &p->rn[i]);
            if (p->lines) cui_draw_rect(ui, (cui_rect){ R.x, cui__cg_snap(y + p->rh, s) - px, p->rn_w, px }, CUI_CORNERS(0), subtle);
        }
        cui_pop_clip(ui);
    }
    if (p->has_rn) cui_draw_rect(ui, (cui_rect){ R.x + p->rn_w - px, R.y, px, R.h }, CUI_CORNERS(0), line);
}

static void cui__cg_paint_bars(cui_ctx *ui, const cui__cg_paint *p, cui_rect R) {
    if (p->bar_t <= 0.01f) return;
    cui_color c = CUI_C(ui, FOREGROUND);
    if (p->vbar) {
        float w = p->big_v ? 8 : 6;
        cui_draw_rect(ui, (cui_rect){ R.x + R.w - w - 3, R.y + p->head_h + p->vpos, w, p->vlen }, CUI_CORNERS(w * 0.5f),
                      cui_alpha(c, (p->big_v ? 0.38f : 0.26f) * p->bar_t));
    }
    if (p->hbar) {
        float h = p->big_h ? 8 : 6;
        cui_draw_rect(ui, (cui_rect){ R.x + p->rn_w + p->hpos, R.y + R.h - h - 3, p->hlen, h }, CUI_CORNERS(h * 0.5f),
                      cui_alpha(c, (p->big_h ? 0.38f : 0.26f) * p->bar_t));
    }
}

static void cui__paint_cellgrid(cui_ctx *ui, cui_node *n, void *data) {
    const cui__cg_paint *p = (const cui__cg_paint *)data;
    cui_rect R = n->rect;
    cui__paint_wheel_hit(ui, p->wid, R);   /* the wheel is this grid's, ahead of any scroll area it sits in */
    cui_rect B = { R.x + p->rn_w, R.y + p->head_h, R.w - p->rn_w, R.h - p->head_h };
    cui_push_clip(ui, R);
    if (B.w > 0 && B.h > 0) cui__cg_paint_body(ui, p, R, B);
    cui__cg_paint_frozen(ui, p, R);
    cui__cg_paint_bars(ui, p, R);
    cui_pop_clip(ui);
    if (p->ring) cui__draw_focus_ring(ui, R, n->radius, 1.0f);
}

/* ------------------------------------------------------------------------ */
/* Build                                                                    */
/* ------------------------------------------------------------------------ */

/* The rectangle of the cells [r0, r1] x [c0, c1] from the grid's corner, clamped to just outside the body so
 * that an outline is cut by the clip instead of being drawn at the clamp. */
static cui_rect cui__cg_cells_rect(const cui__cg *g, double ox, double oy, int r0, int r1, int c0, int c1) {
    const double *e = g->st->edges;
    double x0 = (double)g->rn_w + e[c0] - ox, x1 = (double)g->rn_w + e[c1 + 1] - ox;
    double y0 = (double)g->head_h + (double)r0 * (double)g->rh - oy, y1 = (double)g->head_h + (double)(r1 + 1) * (double)g->rh - oy;
    double lox = (double)g->rn_w - 8.0, hix = (double)g->view_w + 8.0, loy = (double)g->head_h - 8.0, hiy = (double)g->view_h + 8.0;
    x0 = CUI_CLAMP(x0, lox, hix); x1 = CUI_CLAMP(x1, lox, hix);
    y0 = CUI_CLAMP(y0, loy, hiy); y1 = CUI_CLAMP(y1, loy, hiy);
    return (cui_rect){ (float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0) };
}

/* The cells in view, their text kept in frame memory, and everything the paint needs. */
static cui__cg_paint *cui__cg_gather(cui__cg *g, cui_id wid, bool focused, int hover_col) {
    cui_ctx *ui = g->ui;
    const cui_cellgrid_opts *o = g->o;
    cui__cg_state *st = g->st;
    cui__cg_paint *p = (cui__cg_paint *)cui__frame_alloc(ui, sizeof *p);
    float s = g->s;
    double ox = floor(st->x * (double)s + 0.5) / (double)s, oy = floor(st->y * (double)s + 0.5) / (double)s;
    p->adv = st->adv;
    p->s = s; p->rh = g->rh; p->head_h = g->head_h; p->rn_w = g->rn_w; p->size = g->size; p->fh = g->fh; p->pad = g->pad;
    p->striped = o->striped; p->lines = o->grid_lines; p->has_head = o->header; p->has_rn = o->row_numbers;
    p->focused = focused; p->wid = wid; p->sortable = o->sort != NULL;
    p->sort_i = p->hover_i = -1;
    int r0 = 0, r1 = 0, c0 = 0, c1 = -1;
    if (g->rows > 0 && g->cols > 0 && g->body_h > 0 && g->body_w > 0) {
        r0 = (int)CUI_CLAMP(floor(oy / (double)g->rh), 0.0, (double)(g->rows - 1));
        r1 = (int)CUI_MIN(ceil((oy + (double)g->body_h) / (double)g->rh), (double)g->rows);
        c0 = cui__cg_col_near(st, g->cols, ox);
        c1 = cui__cg_col_at(st->edges, g->cols, ox + (double)g->body_w);
        if (c1 < 0) c1 = g->cols - 1;
    }
    int nr = CUI_MAX(r1 - r0, 0), nc = CUI_MAX(c1 - c0 + 1, 0);
    if (nc > 0 && nr > CUI__CG_MAX_ITEMS / nc) nr = CUI_MAX(CUI__CG_MAX_ITEMS / nc, 1);
    p->nr = nr; p->nc = nc; p->r0 = r0; p->c0 = c0;
    p->row_y0 = (float)((double)r0 * (double)g->rh - oy);
    p->lx = (float *)cui__frame_alloc(ui, sizeof(float) * (size_t)(nc + 1));
    p->right = (unsigned char *)cui__frame_alloc(ui, (size_t)nc + 1);
    p->hdr = (cui__cg_item *)cui__frame_alloc(ui, sizeof(cui__cg_item) * (size_t)(nc + 1));
    p->rn = (cui__cg_item *)cui__frame_alloc(ui, sizeof(cui__cg_item) * (size_t)(nr + 1));
    p->cells = (cui__cg_item *)cui__frame_alloc(ui, sizeof(cui__cg_item) * (size_t)nr * (size_t)(nc + 1));
    for (int j = 0; j <= nc; j++) p->lx[j] = (float)((double)g->rn_w + st->edges[c0 + j] - ox);
    p->cols_end = (float)CUI_CLAMP((double)g->rn_w + g->total_w - ox, -1.0e6, 1.0e6);
    p->rows_end = (float)CUI_CLAMP((double)g->head_h + g->total_h - oy, -1.0e6, 1.0e6);
    char tmp[CUI__CG_TEXT + 1];
    for (int j = 0; j < nc; j++) {
        int c = c0 + j;
        p->right[j] = o->right_align && o->right_align[c] ? 1 : 0;
        float w = p->lx[j + 1] - p->lx[j];
        if (o->header) p->hdr[j] = cui__cg_keep(ui, tmp, cui__cg_fetch(g, -1, c, w, tmp));
        if (o->sort && o->sort->column == c) { p->sort_i = j; p->sort_desc = o->sort->descending; }
        if (hover_col == c) p->hover_i = j;
        for (int i = 0; i < nr; i++) p->cells[i * nc + j] = cui__cg_keep(ui, tmp, cui__cg_fetch(g, r0 + i, c, w, tmp));
    }
    if (o->row_numbers)
        for (int i = 0; i < nr; i++) p->rn[i] = cui__cg_keep(ui, tmp, cui__cg_fetch(g, r0 + i, -1, g->rn_w, tmp));
    const cui_cellgrid_sel *S = g->sel;
    if (S->row >= 0 && S->col >= 0 && g->rows > 0 && g->cols > 0) {
        int sr0 = CUI_MIN(S->row, S->anchor_row), sr1 = CUI_MAX(S->row, S->anchor_row);
        int sc0 = CUI_MIN(S->col, S->anchor_col), sc1 = CUI_MAX(S->col, S->anchor_col);
        p->has_sel = true;
        p->multi = sr0 != sr1 || sc0 != sc1;
        p->selr = cui__cg_cells_rect(g, ox, oy, sr0, sr1, sc0, sc1);
        p->curr = cui__cg_cells_rect(g, ox, oy, S->row, S->row, S->col, S->col);
        p->hl_x0 = CUI_MAX(p->selr.x, g->rn_w); p->hl_x1 = CUI_MAX(p->selr.x + p->selr.w, p->hl_x0);
        p->hl_y0 = CUI_MAX(p->selr.y, g->head_h); p->hl_y1 = CUI_MAX(p->selr.y + p->selr.h, p->hl_y0);
    }
    p->bar_t = st->bar_t;
    if (g->max_y > 0) { p->vbar = true; cui__cg_thumb((double)g->body_h, g->total_h, st->y, g->max_y, &p->vpos, &p->vlen); }
    if (g->max_x > 0) { p->hbar = true; cui__cg_thumb((double)g->body_w, g->total_w, st->x, g->max_x, &p->hpos, &p->hlen); }
    p->big_v = st->drag == CUI__CG_DRAG_VBAR;
    p->big_h = st->drag == CUI__CG_DRAG_HBAR;
    return p;
}

/* Input: the mouse, a drag in progress, the keys, then the wheel. Returns the column whose header the pointer is over. */
static int cui__cg_input(cui__cg *g, cui_interact in, cui_id wid) {
    cui_ctx *ui = g->ui;
    cui__cg_state *st = g->st;
    const cui_cellgrid_opts *o = g->o;
    int hover_col = -1;
    float lx = ui->mx - g->pr.x, ly = ui->my - g->pr.y;
    if (in.hovered && g->known) {
        int region = cui__cg_region(g, lx, ly);
        if (ui->mpressed[CUI_MOUSE_LEFT]) cui__cg_press(g, region, lx, ly);
        if (region == CUI__CG_HEAD && st->drag == CUI__CG_DRAG_NONE) {
            if (cui__cg_edge_at(g, lx) >= 0) cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
            else if (o->sort) { hover_col = cui__cg_col_clamped(g, (double)(lx - g->rn_w)); cui__set_cursor(ui, CUI_CURSOR_HAND); }
        }
    }
    if (st->drag != CUI__CG_DRAG_NONE) {
        if (ui->mdown[CUI_MOUSE_LEFT] && g->known) {
            cui__cg_drag(g);
            if (st->drag == CUI__CG_DRAG_RESIZE) cui__set_cursor(ui, CUI_CURSOR_RESIZE_EW);
            if (st->drag == CUI__CG_DRAG_HEADER) hover_col = st->drag_col;
        } else {
            if (st->drag == CUI__CG_DRAG_HEADER && o->sort && ui->mreleased[CUI_MOUSE_LEFT] && g->known &&
                cui__cg_region(g, lx, ly) == CUI__CG_HEAD && cui__cg_col_clamped(g, (double)(lx - g->rn_w)) == st->drag_col) {
                if (o->sort->column == st->drag_col) o->sort->descending = !o->sort->descending;
                else { o->sort->column = st->drag_col; o->sort->descending = false; }
                g->ev |= CUI_CELLGRID_SORTED;
            }
            st->drag = CUI__CG_DRAG_NONE;
        }
    }
    if (in.keyboard_focused && st->drag == CUI__CG_DRAG_NONE) cui__cg_keys(g);
    /* the wheel: three rows a notch; Shift, or a sideways scroll, goes along the columns */
    if (ui->scroll_target == wid && (ui->scroll_dy != 0 || ui->scroll_dx != 0)) {
        float dy = ui->scroll_dy, dx = ui->scroll_dx;
        if ((ui->mods & CUI_MOD_SHIFT) && dx == 0) { dx = dy; dy = 0; }
        if (dy != 0 && g->max_y > 0) st->y -= (double)dy * 3.0 * (double)g->rh;
        if (dx != 0 && g->max_x > 0) st->x -= (double)dx * 60.0;
        st->last_activity = ui->time;
        ui->scroll_dx = ui->scroll_dy = 0;
    }
    cui__cg_clamp_scroll(g);
    return hover_col;
}

int cui_cellgrid_ex(cui_ctx *ui, const char *id_str, int rows, int cols, const cui_cellgrid_opts *opts) {
    cui_cellgrid_opts d;
    memset(&d, 0, sizeof d);
    if (!opts) opts = &d;
    const cui_cellgrid_opts *o = opts;
    cui_id id = cui_make_id(ui, id_str ? id_str : "cellgrid");
    cui_id wid = id ^ 0xCE11u;
    rows = CUI_MAX(rows, 0);
    cols = CUI_CLAMP(cols, 0, CUI__CG_MAX_COLS);
    int first = ui->node_count;
    cui__cg g;
    memset(&g, 0, sizeof g);
    g.ui = ui; g.o = o; g.rows = rows; g.cols = cols;
    cui__cg_state *st = (cui__cg_state *)cui_state(ui, id, sizeof *st);
    cui__state_set_dtor(ui, id, cui__cg_free);
    g.st = st;
    bool usable = o->cell && o->col_widths;
    if (!st->init) {
        st->own.row = st->own.col = st->own.anchor_row = st->own.anchor_col = -1;
        st->init = true;
    }
    g.sel = o->sel ? o->sel : &st->own;
    g.pr = cui__prev_rect(ui, id);
    g.known = g.pr.w > 0 && g.pr.h > 0;
    cui_interact in;
    memset(&in, 0, sizeof in);
    int hover_col = -1;
    if (usable) {
        cui__cg_edges(ui, st, o->col_widths, cols);
        cui__cg_geometry(&g);
        cui__cg_clamp_sel(g.sel, rows, cols);
        /* a selection set from outside moves the view to its cursor */
        if (!cui__cg_sel_same(g.sel, &st->last) || st->want_vis) { st->want_vis = false; cui__cg_reveal(&g, g.sel->row, g.sel->col); }
        cui_cellgrid_sel before = *g.sel;
        in = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
        hover_col = cui__cg_input(&g, in, wid);
        if (g.ev & CUI_CELLGRID_RESIZED) {   /* the new width places this frame's cells */
            cui__cg_edges(ui, st, o->col_widths, cols);
            cui__cg_geometry(&g);
            cui__cg_clamp_scroll(&g);
        }
        if (!cui__cg_sel_same(g.sel, &before)) g.ev |= CUI_CELLGRID_SELECTED;
        st->last = *g.sel;
        /* the bars show while the pointer is over the grid, after a scroll and while one is held */
        bool recent = st->last_activity > 0 && ui->time - st->last_activity < 0.9;
        if (recent) cui__wakeup_after(ui, (float)(0.9 - (ui->time - st->last_activity)) + 0.01f);
        bool show = recent || in.hovered || st->drag == CUI__CG_DRAG_VBAR || st->drag == CUI__CG_DRAG_HBAR;
        cui__tween(ui, &st->bar_t, show ? 1.0f : 0.0f, show ? 0.12f : 0.3f);
    }
    int leaf = cui__leaf(ui, id, o->width.kind ? o->width : CUI_FILL, o->height.kind ? o->height : CUI_FILL);
    cui_node *ln = &ui->nodes[leaf];
    ln->min_h = 60;
    ln->radius = CUI_CORNERS(ui->theme.radius_control);
    ln->border = 1; ln->border_color = CUI_C(ui, BORDER);
    ln->bg = CUI_C(ui, SURFACE);
    ln->flags |= CUI_NF_CLIP;
    cui__mark_interactive(ui, leaf, CUI_IF_FOCUSABLE);
    if (usable) {
        ln->paint = cui__paint_cellgrid;
        ln->paint_data = cui__cg_gather(&g, wid, in.focused, hover_col);
        ((cui__cg_paint *)ln->paint_data)->ring = in.focus_visible;   /* the keyboard focus ring, drawn by the paint (a node has one paint) */
    }
    cui__keep_ltr(ui, first);   /* columns, numbers and the scroll bars do not mirror */
    ui->last_id = id;
    return g.ev;
}
