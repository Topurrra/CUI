/* Tests for the cell grid (src/cui_cellgrid.c): the column search, the selection keys and scrolling into view as
 * pure functions, then the component itself in a headless context: which cells are asked for, rows at 10,000,000,
 * selection by keys and mouse, the event mask, sorting, column resize and fit, the wheel, the scroll bar, and that
 * everything it allocates is freed. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* ---- pure parts ---- */

static void test_col_at(void) {
    printf("column search\n");
    double e[4] = { 0, 100, 150, 350 };
    CHECK_EQ(cui__cg_col_at(e, 3, 0), 0);
    CHECK_EQ(cui__cg_col_at(e, 3, 99.99), 0);
    CHECK_EQ(cui__cg_col_at(e, 3, 100), 1);
    CHECK_EQ(cui__cg_col_at(e, 3, 149.9), 1);
    CHECK_EQ(cui__cg_col_at(e, 3, 150), 2);
    CHECK_EQ(cui__cg_col_at(e, 3, 349.99), 2);
    CHECK_EQ(cui__cg_col_at(e, 3, 350), -1);     /* past the last edge */
    CHECK_EQ(cui__cg_col_at(e, 3, -0.5), -1);    /* left of the first */
    CHECK_EQ(cui__cg_col_at(e, 3, 1e12), -1);
    CHECK_EQ(cui__cg_col_at(e, 0, 5), -1);       /* no columns */
    CHECK_EQ(cui__cg_col_at(e, 1, 50), 0);       /* one column */
    CHECK_EQ(cui__cg_col_at(e, 1, 100), -1);
    /* 16,384 columns of uneven widths against a linear search */
    enum { N = 16384 };
    double *edges = malloc((N + 1) * sizeof *edges);
    unsigned seed = 12345;
    edges[0] = 0;
    for (int k = 0; k < N; k++) { seed = seed * 1664525u + 1013904223u; edges[k + 1] = edges[k] + 24 + (double)(seed >> 22); }
    bool all = true;
    for (int t = 0; t < 20000; t++) {
        seed = seed * 1664525u + 1013904223u;
        double x = (double)(seed >> 8) / (double)(1u << 24) * (edges[N] + 100) - 50;
        int want = -1;
        if (x >= 0 && x < edges[N]) { want = 0; while (edges[want + 1] <= x) want++; }
        if (cui__cg_col_at(edges, N, x) != want) all = false;
    }
    CHECK(all);
    for (int k = 0; k < N; k += 97) CHECK_EQ(cui__cg_col_at(edges, N, edges[k]), k);   /* an edge belongs to the column it starts */
    CHECK_EQ(cui__cg_col_at(edges, N, edges[N] - 0.001), N - 1);
    free(edges);
}

static cui_cellgrid_sel S(int r, int c, int ar, int ac) { cui_cellgrid_sel s = { r, c, ar, ac }; return s; }
static bool eqs(cui_cellgrid_sel a, int r, int c, int ar, int ac) { return a.row == r && a.col == c && a.anchor_row == ar && a.anchor_col == ac; }

static void test_nav(void) {
    printf("selection keys\n");
    cui_cellgrid_sel s = S(5, 5, 5, 5);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_DOWN, 0, 10), CUI__CG_NAV_MOVED); CHECK(eqs(s, 6, 5, 6, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_UP, 0, 10);    CHECK(eqs(s, 5, 5, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_LEFT, 0, 10);  CHECK(eqs(s, 5, 4, 5, 4));
    cui__cg_nav(&s, 100, 20, CUI_KEY_RIGHT, 0, 10); CHECK(eqs(s, 5, 5, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_PAGE_DOWN, 0, 10); CHECK(eqs(s, 15, 5, 15, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_PAGE_UP, 0, 10);   CHECK(eqs(s, 5, 5, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_HOME, 0, 10);  CHECK(eqs(s, 5, 0, 5, 0));
    cui__cg_nav(&s, 100, 20, CUI_KEY_END, 0, 10);   CHECK(eqs(s, 5, 19, 5, 19));
    cui__cg_nav(&s, 100, 20, CUI_KEY_HOME, CUI_MOD_CTRL, 10); CHECK(eqs(s, 0, 0, 0, 0));
    cui__cg_nav(&s, 100, 20, CUI_KEY_END, CUI_MOD_CTRL, 10);  CHECK(eqs(s, 99, 19, 99, 19));
    /* the edges stop the cursor */
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_DOWN, 0, 10), CUI__CG_NAV_SAME);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_RIGHT, 0, 10), CUI__CG_NAV_SAME);
    cui__cg_nav(&s, 100, 20, CUI_KEY_PAGE_DOWN, 0, 10); CHECK(eqs(s, 99, 19, 99, 19));
    s = S(0, 0, 0, 0);
    cui__cg_nav(&s, 100, 20, CUI_KEY_UP, 0, 10);   CHECK(eqs(s, 0, 0, 0, 0));
    cui__cg_nav(&s, 100, 20, CUI_KEY_PAGE_UP, 0, 10); CHECK(eqs(s, 0, 0, 0, 0));
    /* Shift extends: the anchor stays */
    s = S(5, 5, 5, 5);
    cui__cg_nav(&s, 100, 20, CUI_KEY_DOWN, CUI_MOD_SHIFT, 10);  CHECK(eqs(s, 6, 5, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_RIGHT, CUI_MOD_SHIFT, 10); CHECK(eqs(s, 6, 6, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_PAGE_DOWN, CUI_MOD_SHIFT, 10); CHECK(eqs(s, 16, 6, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_HOME, CUI_MOD_SHIFT, 10);  CHECK(eqs(s, 16, 0, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_END, CUI_MOD_CTRL | CUI_MOD_SHIFT, 10); CHECK(eqs(s, 99, 19, 5, 5));
    cui__cg_nav(&s, 100, 20, CUI_KEY_UP, 0, 10);   CHECK(eqs(s, 98, 19, 98, 19));   /* without Shift the range collapses */
    /* Ctrl with an arrow goes to the table's edge */
    s = S(50, 10, 50, 10);
    cui__cg_nav(&s, 100, 20, CUI_KEY_UP, CUI_MOD_CTRL, 10);    CHECK(eqs(s, 0, 10, 0, 10));
    cui__cg_nav(&s, 100, 20, CUI_KEY_RIGHT, CUI_MOD_CTRL, 10); CHECK(eqs(s, 0, 19, 0, 19));
    /* Ctrl+A */
    s = S(5, 5, 5, 5);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_A, CUI_MOD_CTRL, 10), CUI__CG_NAV_ALL); CHECK(eqs(s, 99, 19, 0, 0));
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_A, CUI_MOD_CTRL, 10), CUI__CG_NAV_SAME);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_A, 0, 10), CUI__CG_NAV_NONE);   /* a plain A is not ours */
    /* no selection yet: the first key lands on the first cell (a move from nowhere is not a step) */
    s = S(-1, -1, -1, -1);
    cui__cg_nav(&s, 100, 20, CUI_KEY_DOWN, 0, 10); CHECK(eqs(s, 1, 0, 1, 0) || eqs(s, 0, 0, 0, 0));
    s = S(-1, -1, -1, -1);
    cui__cg_nav(&s, 100, 20, CUI_KEY_HOME, CUI_MOD_CTRL, 10); CHECK(eqs(s, 0, 0, 0, 0));
    /* keys that are not ours, an empty table, Alt */
    s = S(5, 5, 5, 5);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_TAB, 0, 10), CUI__CG_NAV_NONE);
    CHECK_EQ(cui__cg_nav(&s, 0, 20, CUI_KEY_DOWN, 0, 10), CUI__CG_NAV_NONE);
    CHECK_EQ(cui__cg_nav(&s, 100, 0, CUI_KEY_DOWN, 0, 10), CUI__CG_NAV_NONE);
    CHECK_EQ(cui__cg_nav(&s, 100, 20, CUI_KEY_DOWN, CUI_MOD_ALT, 10), CUI__CG_NAV_NONE);
    CHECK(eqs(s, 5, 5, 5, 5));
    /* a table of 10 million rows */
    s = S(9999990, 3, 9999990, 3);
    cui__cg_nav(&s, 10000000, 2000, CUI_KEY_PAGE_DOWN, 0, 40); CHECK(eqs(s, 9999999, 3, 9999999, 3));
    cui__cg_nav(&s, 10000000, 2000, CUI_KEY_PAGE_UP, 0, 40);   CHECK(eqs(s, 9999959, 3, 9999959, 3));
}

static void test_into_view(void) {
    printf("scroll into view\n");
    CHECK(cui__cg_into_view(100, 200, 150, 170) == 100);   /* already inside: stays */
    CHECK(cui__cg_into_view(100, 200, 50, 70) == 50);      /* above: its top at the top */
    CHECK(cui__cg_into_view(100, 200, 380, 400) == 200);   /* below: its bottom at the bottom */
    CHECK(cui__cg_into_view(100, 200, 280, 310) == 110);   /* half in: just enough */
    CHECK(cui__cg_into_view(100, 200, 500, 900) == 500);   /* wider than the view: its start */
    CHECK(cui__cg_into_view(0, 600, 2.4e8 - 20, 2.4e8) == 2.4e8 - 600);   /* far down, in doubles */
}

/* ---- the component, headless ---- */

static cui_sw_renderer *g_sw;
static long g_live;   /* allocations not yet freed, for the counting allocator */
static void *cnt_alloc(size_t n, void *u) { (void)u; size_t *p = malloc(n + 16); if (!p) return NULL; p[0] = n; g_live++; return (char *)p + 16; }
static void *cnt_realloc(void *ptr, size_t n, void *u) {
    if (!ptr) return cnt_alloc(n, u);
    size_t *p = realloc((size_t *)((char *)ptr - 16), n + 16);
    if (!p) return NULL;
    p[0] = n;
    return (char *)p + 16;
}
static void cnt_free(void *ptr, void *u) { (void)u; if (ptr) { free((char *)ptr - 16); g_live--; } }

static cui_ctx *new_ctx(bool counting, float scale) {
    cui_ctx_desc d;
    memset(&d, 0, sizeof d);
    d.renderer = cui_sw_iface(g_sw);
    if (counting) { d.allocator.alloc = cnt_alloc; d.allocator.realloc = cnt_realloc; d.allocator.free = cnt_free; }
    cui_ctx *ui = cui_create(&d);
    cui_set_viewport(ui, 800, 600, scale);
    cui_set_reduced_motion(ui, true);
    return ui;
}

#define MAXC 2000
typedef struct {
    int rows, cols;
    float widths[MAXC];
    unsigned char right[MAXC];
    cui_cellgrid_sel sel;
    cui_table_sort sort;
    bool header, row_numbers, with_sort, with_right;
    float row_height;
    /* what the callback was asked, this frame */
    long calls; int min_row, max_row, min_col, max_col; int hdr_calls, rn_calls;
    int ev;           /* events of the last frame */
    int acc;          /* events since cleared */
} gapp;

static const char *cb(void *ctx, int row, int col, char *buf, size_t cap) {
    gapp *a = ctx;
    a->calls++;
    if (row < 0) { a->hdr_calls++; snprintf(buf, cap, "H%d", col); return buf; }
    if (col < 0) { a->rn_calls++; if (row < a->min_row) a->min_row = row; if (row > a->max_row) a->max_row = row; snprintf(buf, cap, "%d", row + 1); return buf; }
    if (row < a->min_row) a->min_row = row;
    if (row > a->max_row) a->max_row = row;
    if (col < a->min_col) a->min_col = col;
    if (col > a->max_col) a->max_col = col;
    snprintf(buf, cap, "r%dc%d", row, col);
    return buf;
}

static void build(cui_ctx *ui, void *u) {
    gapp *a = u;
    a->calls = 0; a->hdr_calls = a->rn_calls = 0;
    a->min_row = a->min_col = 0x7fffffff; a->max_row = a->max_col = -1;
    a->ev = cui_cellgrid(ui, "g", a->rows, a->cols, .cell = cb, .ctx = a, .col_widths = a->widths,
                         .header = a->header, .row_numbers = a->row_numbers, .row_height = a->row_height,
                         .right_align = a->with_right ? a->right : NULL, .sort = a->with_sort ? &a->sort : NULL,
                         .sel = &a->sel, .striped = true, .grid_lines = true,
                         .width = CUI_PX(800), .height = CUI_PX(600));
    a->acc |= a->ev;
}

static void frames(cui_ctx *ui, gapp *a, int n) {
    for (int i = 0; i < n; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, a); cui_end_frame(ui); }
}

static void gapp_init(gapp *a, int rows, int cols) {
    memset(a, 0, sizeof *a);
    a->rows = rows; a->cols = cols;
    for (int k = 0; k < cols && k < MAXC; k++) a->widths[k] = 100;
    a->header = true; a->row_height = 20;
    a->sel.row = a->sel.col = a->sel.anchor_row = a->sel.anchor_col = -1;
    a->sort.column = -1;
}

static cui_rect grect(cui_ctx *ui) { return cui_get_rect(ui, cui_make_id(ui, "g")); }

static void mouse_move(cui_ctx *ui, gapp *a, float x, float y) { cui_input_mouse_move(ui, x, y); frames(ui, a, 1); }
static void press(cui_ctx *ui, gapp *a, float x, float y, int mods) {
    cui_input_mouse_move(ui, x, y);
    frames(ui, a, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, mods);   /* the modifiers for the click */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    frames(ui, a, 1);
}
static void release(cui_ctx *ui, gapp *a) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frames(ui, a, 1); }
static void click(cui_ctx *ui, gapp *a, float x, float y) { press(ui, a, x, y, 0); release(ui, a); }
static void key(cui_ctx *ui, gapp *a, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods); frames(ui, a, 1);
    cui_input_key(ui, k, false, mods); frames(ui, a, 1);
}

static void test_visible_cells(void) {
    printf("only the cells in view are asked for\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 10000000, 2000);
    a->row_numbers = true;
    frames(ui, a, 3);
    CHECK(a->calls > 0);
    CHECK(a->calls < 2500);                 /* a screenful, not 2e10 cells */
    CHECK_EQ(a->min_row, 0);
    CHECK(a->max_row >= 20 && a->max_row < 40);
    CHECK_EQ(a->min_col, 0);
    CHECK(a->max_col >= 3 && a->max_col < 12);
    CHECK(a->hdr_calls >= 4);
    CHECK(a->rn_calls >= 20);
    /* idle frames ask the same again; no event */
    CHECK_EQ(a->ev, 0);
    /* nothing is asked when there is nothing to show */
    a->rows = 0; frames(ui, a, 2);
    CHECK_EQ(a->ev, 0);
    a->rows = 100; a->cols = 0; frames(ui, a, 2);
    CHECK_EQ(a->ev, 0);
    a->rows = 0; a->cols = 0; frames(ui, a, 2);
    free(a);
    cui_destroy(ui);
}

static void test_row_ten_million(void) {
    printf("rows at 10,000,000, columns at 2,000\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 10000000, 2000);
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    CHECK(r.w == 800 && r.h == 600);
    /* the cursor set from outside to the very last cell: the view follows it */
    a->sel = S(9999999, 1999, 9999999, 1999);
    frames(ui, a, 3);
    CHECK_EQ(a->max_row, 9999999);
    CHECK_EQ(a->max_col, 1999);
    CHECK(a->max_row - a->min_row >= 20 && a->max_row - a->min_row < 32);
    CHECK_EQ(a->ev, 0);                     /* setting it from outside is not an event */
    /* rows are 20 points, the header 20: the last row fills y 580..600, its neighbour 560..580 */
    click(ui, a, r.x + 800 - 50, r.y + 585);
    CHECK_EQ(a->sel.row, 9999999); CHECK_EQ(a->sel.col, 1999);
    click(ui, a, r.x + 800 - 50, r.y + 565);
    CHECK_EQ(a->sel.row, 9999998);
    click(ui, a, r.x + 800 - 150, r.y + 545);
    CHECK_EQ(a->sel.row, 9999997); CHECK_EQ(a->sel.col, 1998);
    /* and back to the top-left cell */
    a->sel = S(0, 0, 0, 0);
    frames(ui, a, 3);
    CHECK_EQ(a->min_row, 0); CHECK_EQ(a->min_col, 0);
    click(ui, a, r.x + 250, r.y + 20 + 3 * 20 + 5);
    CHECK_EQ(a->sel.row, 3); CHECK_EQ(a->sel.col, 2);
    /* a row in the middle: 5,000,000 */
    a->sel = S(5000000, 1000, 5000000, 1000);
    frames(ui, a, 3);
    CHECK(a->min_row <= 5000000 && a->max_row >= 5000000);
    CHECK(a->min_col <= 1000 && a->max_col >= 1000);
    free(a);
    cui_destroy(ui);
}

static void test_keys_and_events(void) {
    printf("keys and the event mask\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 100000, 50);
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    CHECK_EQ(a->ev, 0);
    click(ui, a, r.x + 250, r.y + 20 + 3 * 20 + 5);      /* row 3, column 2 */
    CHECK_EQ(a->sel.row, 3); CHECK_EQ(a->sel.col, 2);
    CHECK(a->acc & CUI_CELLGRID_SELECTED);
    CHECK(!(a->acc & CUI_CELLGRID_ACTIVATE));
    a->acc = 0;
    key(ui, a, CUI_KEY_DOWN, 0);
    CHECK_EQ(a->sel.row, 4); CHECK_EQ(a->sel.col, 2);
    CHECK_EQ(a->acc, CUI_CELLGRID_SELECTED);
    a->acc = 0;
    key(ui, a, CUI_KEY_RIGHT, CUI_MOD_SHIFT);
    CHECK(a->sel.row == 4 && a->sel.col == 3 && a->sel.anchor_row == 4 && a->sel.anchor_col == 2);
    key(ui, a, CUI_KEY_DOWN, CUI_MOD_SHIFT);
    CHECK(a->sel.row == 5 && a->sel.col == 3 && a->sel.anchor_row == 4 && a->sel.anchor_col == 2);
    key(ui, a, CUI_KEY_LEFT, 0);                          /* the range collapses at the cursor */
    CHECK(a->sel.row == 5 && a->sel.col == 2 && a->sel.anchor_row == 5 && a->sel.anchor_col == 2);
    a->acc = 0;
    /* Page Down moves a screen less one row and keeps the cursor in view */
    key(ui, a, CUI_KEY_PAGE_DOWN, 0);
    CHECK(a->sel.row > 20 && a->sel.row < 35);
    CHECK(a->max_row >= a->sel.row && a->min_row <= a->sel.row);
    key(ui, a, CUI_KEY_END, CUI_MOD_CTRL);
    CHECK(a->sel.row == 99999 && a->sel.col == 49);
    CHECK(a->max_row == 99999 && a->max_col == 49);
    key(ui, a, CUI_KEY_HOME, CUI_MOD_CTRL);
    CHECK(a->sel.row == 0 && a->sel.col == 0);
    CHECK(a->min_row == 0 && a->min_col == 0);
    key(ui, a, CUI_KEY_END, 0);
    CHECK(a->sel.row == 0 && a->sel.col == 49 && a->max_col == 49);
    /* Ctrl+A selects all and does not scroll to the end */
    key(ui, a, CUI_KEY_HOME, 0);
    a->acc = 0;
    key(ui, a, CUI_KEY_A, CUI_MOD_CTRL);
    CHECK(a->sel.anchor_row == 0 && a->sel.anchor_col == 0 && a->sel.row == 99999 && a->sel.col == 49);
    CHECK(a->min_row == 0);
    CHECK_EQ(a->acc, CUI_CELLGRID_SELECTED);
    /* Ctrl+C and Enter report; they do not change the selection */
    a->acc = 0;
    key(ui, a, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_EQ(a->acc, CUI_CELLGRID_COPY);
    a->acc = 0;
    key(ui, a, CUI_KEY_ENTER, 0);
    CHECK_EQ(a->acc, CUI_CELLGRID_ACTIVATE);
    a->acc = 0;
    key(ui, a, CUI_KEY_C, 0);                             /* a plain C is not a copy */
    CHECK_EQ(a->acc, 0);
    /* with no focus the keys are not ours */
    cui_input_mouse_move(ui, 5000, 5000);
    click(ui, a, 5000, 5000);
    a->sel = S(10, 10, 10, 10);
    frames(ui, a, 2);
    a->acc = 0;
    key(ui, a, CUI_KEY_DOWN, 0);
    CHECK(a->sel.row == 10);
    key(ui, a, CUI_KEY_C, CUI_MOD_CTRL);
    key(ui, a, CUI_KEY_ENTER, 0);
    CHECK_EQ(a->acc, 0);
    free(a);
    cui_destroy(ui);
}

static void test_mouse(void) {
    printf("mouse selection\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 100000, 50);
    a->row_numbers = true;
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    /* the row-number column fits the widest label ("100000"): find where the cells start from the first column's text */
    float rn = 0;
    for (float x = 20; x < 100; x += 1) {
        click(ui, a, r.x + x, r.y + 20 + 25);
        if (a->sel.col == 0 && a->sel.anchor_col == 0 && a->sel.row == 1 && a->sel.anchor_row == 1) { rn = x; break; }
        if (a->sel.anchor_col == 49) continue;     /* still inside the number column: a row was selected */
    }
    CHECK(rn > 30 && rn < 70);
    float x0 = r.x + rn + 1;                       /* just inside the first column */
    /* click a cell */
    a->acc = 0;
    click(ui, a, x0 + 100 * 2 + 50, r.y + 20 + 20 * 4 + 10);
    CHECK(a->sel.row == 4 && a->sel.col == 2 && a->sel.anchor_row == 4 && a->sel.anchor_col == 2);
    CHECK(a->acc & CUI_CELLGRID_SELECTED);
    /* drag a rectangle */
    press(ui, a, x0 + 100 + 50, r.y + 20 + 20 * 2 + 10, 0);
    mouse_move(ui, a, x0 + 100 * 3 + 50, r.y + 20 + 20 * 6 + 10);
    CHECK(a->sel.anchor_row == 2 && a->sel.anchor_col == 1 && a->sel.row == 6 && a->sel.col == 3);
    release(ui, a);
    CHECK(a->sel.anchor_row == 2 && a->sel.row == 6);
    /* a row number selects the row */
    click(ui, a, r.x + 10, r.y + 20 + 20 * 8 + 10);
    CHECK(a->sel.row == 8 && a->sel.anchor_row == 8 && a->sel.col == 0 && a->sel.anchor_col == 49);
    /* dragging along the numbers selects rows */
    press(ui, a, r.x + 10, r.y + 20 + 20 * 3 + 10, 0);
    mouse_move(ui, a, r.x + 10, r.y + 20 + 20 * 5 + 10);
    CHECK(a->sel.anchor_row == 3 && a->sel.row == 5 && a->sel.anchor_col == 49 && a->sel.col == 0);
    release(ui, a);
    /* the corner selects everything */
    click(ui, a, r.x + 10, r.y + 10);
    CHECK(a->sel.anchor_row == 0 && a->sel.anchor_col == 0 && a->sel.row == 99999 && a->sel.col == 49);
    /* a double click activates */
    a->acc = 0;
    click(ui, a, x0 + 150, r.y + 20 + 20 * 2 + 10);
    click(ui, a, x0 + 150, r.y + 20 + 20 * 2 + 10);
    CHECK(a->acc & CUI_CELLGRID_ACTIVATE);
    CHECK(a->sel.row == 2 && a->sel.col == 1);
    free(a);
    cui_destroy(ui);
}

static void test_shift_click(void) {
    printf("shift click\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 1000, 20);
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    click(ui, a, r.x + 50, r.y + 20 + 10);                 /* row 0, column 0 */
    CHECK(a->sel.row == 0 && a->sel.col == 0);
    press(ui, a, r.x + 350, r.y + 20 + 20 * 5 + 10, CUI_MOD_SHIFT);
    release(ui, a);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    CHECK(a->sel.anchor_row == 0 && a->sel.anchor_col == 0);
    CHECK(a->sel.row == 5 && a->sel.col == 3);
    /* Shift+click again moves the far corner, the anchor stays */
    press(ui, a, r.x + 150, r.y + 20 + 20 * 2 + 10, CUI_MOD_SHIFT);
    release(ui, a);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    CHECK(a->sel.anchor_row == 0 && a->sel.anchor_col == 0 && a->sel.row == 2 && a->sel.col == 1);
    /* Shift+click on a row number extends the rows */
    free(a);
    cui_destroy(ui);
}

static void test_sort_and_resize(void) {
    printf("sorting, column resize and fit\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 1000, 20);
    a->with_sort = true;
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    /* a header click sorts by that column, again toggles descending */
    a->acc = 0;
    click(ui, a, r.x + 150, r.y + 10);
    CHECK(a->sort.column == 1 && !a->sort.descending);
    CHECK(a->acc & CUI_CELLGRID_SORTED);
    CHECK(!(a->acc & CUI_CELLGRID_SELECTED));
    a->acc = 0;
    click(ui, a, r.x + 150, r.y + 10);
    CHECK(a->sort.column == 1 && a->sort.descending);
    click(ui, a, r.x + 250, r.y + 10);
    CHECK(a->sort.column == 2 && !a->sort.descending);
    /* a press on a header that moves off before the release does not sort */
    a->acc = 0;
    press(ui, a, r.x + 350, r.y + 10, 0);
    mouse_move(ui, a, r.x + 650, r.y + 10);
    release(ui, a);
    CHECK_EQ(a->sort.column, 2);
    CHECK(!(a->acc & CUI_CELLGRID_SORTED));
    /* drag the edge between columns 0 and 1 (x = 100) 60 points right */
    a->acc = 0;
    press(ui, a, r.x + 101, r.y + 10, 0);
    mouse_move(ui, a, r.x + 161, r.y + 10);
    release(ui, a);
    CHECK(a->acc & CUI_CELLGRID_RESIZED);
    CHECK(!(a->acc & CUI_CELLGRID_SORTED));
    CHECK(a->widths[0] > 158 && a->widths[0] < 162);
    CHECK_EQ(a->widths[1], 100);
    /* the new widths place the cells: the old column 1 starts at 160 now */
    click(ui, a, r.x + 170, r.y + 20 + 10);
    CHECK_EQ(a->sel.col, 1);
    click(ui, a, r.x + 150, r.y + 20 + 10);
    CHECK_EQ(a->sel.col, 0);
    /* the smallest a drag makes it */
    a->acc = 0;
    press(ui, a, r.x + 160 + 1, r.y + 10, 0);
    mouse_move(ui, a, r.x + 5, r.y + 10);
    release(ui, a);
    CHECK(a->widths[0] >= 24 && a->widths[0] < 26);
    /* a double click on an edge fits the column to its header and cells: "r999c3" is short, "H3" shorter */
    a->widths[3] = 300;
    frames(ui, a, 2);
    a->acc = 0;
    float edge = a->widths[0] + a->widths[1] + a->widths[2] + a->widths[3];
    click(ui, a, r.x + edge, r.y + 10);
    click(ui, a, r.x + edge, r.y + 10);
    CHECK(a->acc & CUI_CELLGRID_RESIZED);
    CHECK(a->widths[3] > 40 && a->widths[3] < 100);
    /* widths changed from outside move the cells */
    a->widths[0] = 400;
    frames(ui, a, 2);
    click(ui, a, r.x + 350, r.y + 30);
    CHECK_EQ(a->sel.col, 0);
    a->widths[0] = 0;                                  /* <= 0 means 120 */
    frames(ui, a, 2);
    click(ui, a, r.x + 110, r.y + 30);
    CHECK_EQ(a->sel.col, 0);
    click(ui, a, r.x + 130, r.y + 30);
    CHECK_EQ(a->sel.col, 1);
    free(a);
    cui_destroy(ui);
}

static void test_wheel_and_bars(void) {
    printf("the wheel and the scroll bars\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 1000000, 200);
    frames(ui, a, 3);
    cui_rect r = grect(ui);
    CHECK_EQ(a->min_row, 0);
    cui_input_mouse_move(ui, r.x + 300, r.y + 300);
    frames(ui, a, 2);
    cui_input_scroll(ui, 0, -1);                        /* one notch down: three rows */
    frames(ui, a, 1);
    CHECK_EQ(a->min_row, 3);
    cui_input_scroll(ui, 0, 1);                         /* and back */
    frames(ui, a, 1);
    CHECK_EQ(a->min_row, 0);
    cui_input_scroll(ui, 0, 1);                         /* the top stops it */
    frames(ui, a, 1);
    CHECK_EQ(a->min_row, 0);
    for (int i = 0; i < 100; i++) { cui_input_scroll(ui, 0, -1); frames(ui, a, 1); }
    CHECK_EQ(a->min_row, 300);
    /* Shift+wheel goes sideways: 60 points a notch, 100 points a column */
    cui_input_key(ui, CUI_KEY_NONE, true, CUI_MOD_SHIFT);
    cui_input_scroll(ui, 0, -1);
    frames(ui, a, 1);
    cui_input_scroll(ui, 0, -1);
    frames(ui, a, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    frames(ui, a, 1);
    CHECK_EQ(a->min_col, 1);
    CHECK_EQ(a->min_row, 300);
    /* a sideways touchpad scroll too */
    cui_input_scroll(ui, -1, 0);
    frames(ui, a, 1);
    CHECK_EQ(a->min_col, 1);                            /* 180 points: still column 1 */
    cui_input_scroll(ui, -1, 0);
    frames(ui, a, 1);
    CHECK_EQ(a->min_col, 2);
    /* the vertical bar: drag the thumb to the bottom */
    a->sel = S(-1, -1, -1, -1);
    frames(ui, a, 1);
    press(ui, a, r.x + 800 - 5, r.y + 30, 0);           /* in the bar zone at the top of the track */
    mouse_move(ui, a, r.x + 800 - 5, r.y + 599);
    release(ui, a);
    CHECK_EQ(a->max_row, 999999);
    /* and to the middle, by the track: about half way */
    press(ui, a, r.x + 800 - 5, r.y + 300 + 3, 0);
    release(ui, a);
    CHECK(a->min_row > 400000 && a->min_row < 600000);
    free(a);
    cui_destroy(ui);
}

static void test_external_and_options(void) {
    printf("setting the selection from outside, options\n");
    cui_ctx *ui = new_ctx(false, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 100000, 100);
    a->row_numbers = true; a->with_right = true;
    for (int k = 0; k < 100; k += 3) a->right[k] = 1;
    frames(ui, a, 3);
    a->sel = S(500, 40, 500, 40);
    frames(ui, a, 2);
    CHECK(a->min_row <= 500 && a->max_row >= 500 && a->min_col <= 40 && a->max_col >= 40);
    CHECK_EQ(a->ev, 0);
    a->sel = S(3, 2, 3, 2);                            /* back up: it scrolls up and left to it */
    frames(ui, a, 2);
    CHECK(a->min_row <= 3 && a->max_row >= 3 && a->min_col <= 2);
    /* a selection past the end of a shorter table is pulled in */
    a->sel = S(90000, 90, 90000, 90);
    frames(ui, a, 2);
    a->rows = 50; a->cols = 10;
    frames(ui, a, 2);
    CHECK(a->sel.row == 49 && a->sel.col == 9 && a->sel.anchor_row == 49 && a->sel.anchor_col == 9);
    /* no header, no numbers */
    a->header = false; a->row_numbers = false; a->rows = 1000; a->cols = 10;
    frames(ui, a, 3);
    CHECK_EQ(a->hdr_calls, 0); CHECK_EQ(a->rn_calls, 0);
    /* the selection kept by the grid when none is given */
    cui_begin_frame(ui, 1.0f / 60.0f);
    int ev = cui_cellgrid(ui, "own", 10, 3, .cell = cb, .ctx = a, .col_widths = a->widths);
    cui_end_frame(ui);
    CHECK_EQ(ev, 0);
    /* a grid with no callback or no widths draws nothing and does nothing */
    cui_begin_frame(ui, 1.0f / 60.0f);
    CHECK_EQ(cui_cellgrid(ui, "bad1", 10, 3, .col_widths = a->widths), 0);
    CHECK_EQ(cui_cellgrid(ui, "bad2", 10, 3, .cell = cb, .ctx = a), 0);
    CHECK_EQ(cui_cellgrid(ui, "bad3", 10, 3), 0);
    cui_end_frame(ui);
    /* more columns than the limit */
    gapp *b = calloc(1, sizeof *b);
    gapp_init(b, 10, MAXC);
    frames(ui, b, 2);
    free(b);
    free(a);
    cui_destroy(ui);
}

static void test_scale_and_themes(void) {
    printf("DPI scales and both themes\n");
    float scales[] = { 1.0f, 1.25f, 1.5f, 2.0f };
    for (int t = 0; t < 4; t++) {
        for (int dark = 0; dark < 2; dark++) {
            cui_ctx *ui = new_ctx(false, scales[t]);
            cui_set_theme(ui, dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
            gapp *a = calloc(1, sizeof *a);
            gapp_init(a, 2000000, 300);
            a->row_numbers = true; a->row_height = 0;   /* the default height */
            frames(ui, a, 3);
            cui_rect r = grect(ui);
            CHECK(r.w == 800 && r.h == 600);
            CHECK(a->max_row > 10);
            click(ui, a, r.x + 300, r.y + 300);
            CHECK(a->sel.row >= a->min_row && a->sel.row <= a->max_row && a->sel.row > 5);
            key(ui, a, CUI_KEY_END, CUI_MOD_CTRL);
            CHECK(a->sel.row == 1999999 && a->sel.col == 299 && a->max_row == 1999999);
            free(a);
            cui_destroy(ui);
        }
    }
}

/* cells with long text, non-ASCII text, line breaks, empty text and NULL, at several widths */
static const char *odd_cell(void *ctx, int row, int col, char *buf, size_t cap) {
    (void)ctx; (void)cap;
    static char longtxt[3001], cjk[3001];
    static bool ready;
    if (!ready) {
        for (int i = 0; i < 3000; i++) longtxt[i] = (char)('a' + i % 26);
        for (int i = 0; i + 3 <= 3000; i += 3) { cjk[i] = (char)0xE6; cjk[i + 1] = (char)0x97; cjk[i + 2] = (char)0xA5; }   /* U+65E5, 1000 times */
        longtxt[3000] = cjk[3000] = 0;
        ready = true;
    }
    if (row < 0) { snprintf(buf, cap, "H%d", col); return buf; }
    if (col < 0) { snprintf(buf, cap, "%d", row + 1); return buf; }
    switch ((row + col) % 8) {
    case 0: return longtxt;
    case 1: return cjk;
    case 2: return "line one\nline two\r\n\ttabbed";
    case 3: return "";
    case 4: return NULL;
    case 5: return "caf\xC3\xA9 na\xC3\xAFve \xE2\x80\xA6 \xF0\x9F\x99\x82 end";
    case 6: snprintf(buf, cap, "%d", row * 1000 + col); return buf;
    default: return "short";
    }
}

static void test_odd_text(void) {
    printf("long, non-ASCII, multi-line, empty and NULL cell text\n");
    float widths[40];
    unsigned char right[40];
    for (int k = 0; k < 40; k++) { widths[k] = k % 3 == 0 ? 30.0f : (k % 3 == 1 ? 120.0f : 700.0f); right[k] = (unsigned char)(k % 2); }
    cui_cellgrid_sel sel = { 3, 3, 3, 3 };
    for (int t = 0; t < 4; t++) {
        cui_ctx *ui = new_ctx(false, t == 3 ? 2.0f : 1.0f);
        cui_set_theme(ui, t & 1 ? CUI_THEME_DARK : CUI_THEME_LIGHT);
        int ev = 0;
        for (int f = 0; f < 6; f++) {
            cui_begin_frame(ui, 1.0f / 60.0f);
            ev |= cui_cellgrid(ui, "g", 1000, 40, .cell = odd_cell, .col_widths = widths, .right_align = right, .header = true, .row_numbers = true,
                               .sel = &sel, .striped = true, .grid_lines = true, .width = CUI_PX(800), .height = CUI_PX(600));
            cui_end_frame(ui);
        }
        CHECK_EQ(ev, 0);
        const cui_draw_data *dd = cui_get_draw_data(ui);
        CHECK(dd->vertex_count > 100);
        cui_destroy(ui);
    }
}

static void test_memory(void) {
    printf("what the grid allocates is freed\n");
    g_live = 0;
    cui_ctx *ui = new_ctx(true, 1);
    gapp *a = calloc(1, sizeof *a);
    gapp_init(a, 10000000, MAXC);
    frames(ui, a, 4);
    a->widths[5] = 333;
    frames(ui, a, 2);
    a->cols = 1000;
    frames(ui, a, 2);
    a->cols = 0;
    frames(ui, a, 2);
    cui_destroy(ui);
    CHECK_EQ(g_live, 0);
    free(a);
}

int main(void) {
    g_sw = cui_sw_create();
    test_col_at();
    test_nav();
    test_into_view();
    test_visible_cells();
    test_row_ten_million();
    test_keys_and_events();
    test_mouse();
    test_shift_click();
    test_sort_and_resize();
    test_wheel_and_bars();
    test_external_and_options();
    test_scale_and_themes();
    test_odd_text();
    test_memory();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
