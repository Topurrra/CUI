/* VT terminal: a screen of character cells driven by the escape sequences of a real terminal (cursor
 * movement, erasing, scrolling regions, colors, the alternate screen), and a view with raw keyboard input.
 *
 * The screen is two grids of 8 byte cells (code point and attributes in one word, two 16 bit color ids).
 * Rows are reached through a map of row slots, so scrolling, inserting and deleting lines rotate small
 * integers and never move cell data. Lines that scroll off the top of the main screen go to a scrollback
 * kept in one ring of cells with the blank right end of each line cut off, so its memory is fixed.
 * Writing parses in place with no allocation; the view paints only what is on screen. */
#include "cui_internal.h"

#define CUI__VT_MAX_COLS 1000
#define CUI__VT_MAX_ROWS 500
#define CUI__VT_RGB_MAX 1024
#define CUI__VT_PALETTE 257         /* color ids: 0 default, 1..256 palette index + 1, 257.. an entry of the rgb table */

enum { CUI__VT_SPACER = 1u << 30 };  /* the second cell of a wide character */
#define CUI__VT_OVER 0x80000000u       /* overline */
#define CUI__VT_ULS_SHIFT 21           /* underline style 0..5 (none, single, double, curly, dotted, dashed) in the 3 free bits below the attributes */
#define CUI__VT_ULS_MASK 0x00E00000u
#define CUI__VT_ULS(ca) (((ca) & CUI__VT_ULS_MASK) >> CUI__VT_ULS_SHIFT)
#define CUI__VT_CP(ca) ((ca) & 0x1FFFFFu)
#define CUI__VT_ATTRS(ca) ((ca) & 0xFFE00000u)

typedef struct { uint32_t ca; uint16_t fg, bg; } cui__vt_cell;
typedef char cui__vt_cell_is_8_bytes[sizeof(cui__vt_cell) == 8 ? 1 : -1];
typedef struct { cui__vt_cell *cells; uint16_t *map; uint8_t *wrap; } cui__vt_screen;
typedef struct { uint32_t off; uint16_t len; uint8_t wrapped; } cui__vt_line;

enum { CUI__VT_GROUND, CUI__VT_ESC, CUI__VT_ESC_INT, CUI__VT_CSI, CUI__VT_OSC, CUI__VT_OSC_ESC, CUI__VT_STR, CUI__VT_STR_ESC,
       CUI__VT_DCS, CUI__VT_SIXEL };   /* the last two only with an extension (cui_vt_sixel.c): a DCS's parameters, sixel data */

struct cui_vt {
    int cols, rows;
    cui__vt_screen scr[2];
    int cur;                          /* 0 main, 1 alternate */
    int cx, cy;
    bool pend;                        /* the cursor is past the last column: the next character wraps first */
    uint32_t attr; uint16_t fg, bg;   /* the pen */
    int stop, sbot;                   /* scroll region, rows */
    int sx, sy; bool spend; uint32_t sattr; uint16_t sfg, sbg;   /* saved cursor */
    bool autowrap, app_cursor, bracketed, cursor_vis;
    int state;
    int p[16], np; bool pany; char priv, inter;
    uint16_t pcolon;                  /* bit i: the separator before p[i] was a colon (a sub-parameter) */
    uint32_t cp; int need;            /* a UTF-8 sequence cut by the end of a write */
    char osc[160]; int osc_n;
    char title[128];
    char reply[256]; int reply_n;
    cui__vt_cell *sb; size_t sb_cap, sb_pos;
    cui__vt_line *sb_lines; int sb_max, sb_head, sb_count;
    uint64_t sb_total;                /* lines ever pushed to the scrollback */
    uint32_t rgb[CUI__VT_RGB_MAX]; int rgb_n;
    uint8_t *tabs;
    uint8_t mouse_modes;              /* what the program asked for: CUI__VT_M1000, _M1002, _M1003 */
    bool mouse_sgr;                   /* encoding 1006 */
    bool focus_mode;                  /* 1004: the program wants ESC [ I and ESC [ O */
    bool win32_input;                 /* 9001: the program wants every key as a Windows key record (sent only with .win32_keys) */
    uint8_t cursor_shape;             /* CUI_VT_CURSOR_* from CSI Ps SP q */
    int cell_w, cell_h;               /* the view's cell in device pixels, for the size reports (0 before a view has drawn) */
    cui__vt_cell *snap;               /* synchronized output (?2026): the screen as it was at h, allocated at the first h and kept */
    int snap_cols, snap_rows, snap_cx, snap_cy; bool snap_cursor;
    bool sync_on; uint32_t sync_seq;  /* an update is open; counts the h that opened one */
    cui__vt_x *x;                     /* the opt-in parts (cui_vt_sixel.c): NULL until cui_vt_set_features or cui_vt_set_colors */
    uint8_t pic_on;                   /* CUI__VT_PIC_*: where pictures are kept; 0 keeps every picture hook off the text paths */
    uint16_t cell_w16;                /* the view's cell width in 1/16 device pixels (cell_w is rounded), for sixel pictures' aspect */
};

enum { CUI__VT_M1000 = 1, CUI__VT_M1002 = 2, CUI__VT_M1003 = 4 };

/* ------------------------------------------------------------------------ */
/* Screens                                                                  */
/* ------------------------------------------------------------------------ */

static cui__vt_cell *cui__vt_row(const cui_vt *v, const cui__vt_screen *s, int y) { return s->cells + (size_t)s->map[y] * (size_t)v->cols; }

static bool cui__vt_screen_alloc(cui__vt_screen *s, int cols, int rows) {
    s->cells = (cui__vt_cell *)calloc((size_t)cols * (size_t)rows, sizeof(cui__vt_cell));
    s->map = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)rows);
    s->wrap = (uint8_t *)calloc((size_t)rows, 1);
    if (!s->cells || !s->map || !s->wrap) return false;
    for (int i = 0; i < rows; i++) s->map[i] = (uint16_t)i;
    return true;
}

static void cui__vt_screen_free(cui__vt_screen *s) {
    free(s->cells); free(s->map); free(s->wrap);
    memset(s, 0, sizeof *s);
}

static void cui__vt_fill(cui__vt_cell *c, int n, uint16_t bg) {
    cui__vt_cell b = { 0, 0, bg };
    for (int i = 0; i < n; i++) c[i] = b;
}

cui_vt *cui_vt_create(int cols, int rows, int scrollback_lines) {
    cols = CUI_CLAMP(cols, 2, CUI__VT_MAX_COLS);
    rows = CUI_CLAMP(rows, 1, CUI__VT_MAX_ROWS);
    cui_vt *v = (cui_vt *)calloc(1, sizeof *v);
    if (!v) return NULL;
    v->cols = cols; v->rows = rows;
    v->tabs = (uint8_t *)malloc((size_t)CUI__VT_MAX_COLS);
    if (!v->tabs || !cui__vt_screen_alloc(&v->scr[0], cols, rows)) { cui_vt_destroy(v); return NULL; }
    for (int i = 0; i < CUI__VT_MAX_COLS; i++) v->tabs[i] = i % 8 == 0;
    v->autowrap = true; v->cursor_vis = true;
    v->stop = 0; v->sbot = rows - 1;
    if (scrollback_lines > 0) {
        v->sb_max = CUI_MIN(scrollback_lines, 100000);
        v->sb_cap = (size_t)v->sb_max * 48;     /* cells: lines are cut at their last used cell, 48 is plenty on average */
        v->sb = (cui__vt_cell *)malloc(v->sb_cap * sizeof(cui__vt_cell));
        v->sb_lines = (cui__vt_line *)malloc(sizeof(cui__vt_line) * (size_t)v->sb_max);
        if (!v->sb || !v->sb_lines) { free(v->sb); free(v->sb_lines); v->sb = NULL; v->sb_lines = NULL; v->sb_max = 0; }
    }
    return v;
}

void cui_vt_destroy(cui_vt *v) {
    if (!v) return;
    if (v->x) cui__vt_x_free(v);
    cui__vt_screen_free(&v->scr[0]); cui__vt_screen_free(&v->scr[1]);
    free(v->sb); free(v->sb_lines); free(v->tabs); free(v->snap);
    free(v);
}

size_t cui_vt_memory(const cui_vt *v) {
    size_t n = sizeof *v + (size_t)CUI__VT_MAX_COLS;
    n += (size_t)v->cols * (size_t)v->rows * sizeof(cui__vt_cell) * (v->scr[1].cells ? 2 : 1);
    if (v->snap) n += (size_t)v->snap_cols * (size_t)v->snap_rows * sizeof(cui__vt_cell);
    if (v->x) n += cui__vt_x_memory(v);
    return n + v->sb_cap * sizeof(cui__vt_cell) + (size_t)v->sb_max * sizeof(cui__vt_line);
}

/* ------------------------------------------------------------------------ */
/* Scrollback                                                               */
/* ------------------------------------------------------------------------ */

static void cui__vt_sb_drop(cui_vt *v) {
    v->sb_head = (v->sb_head + 1) % v->sb_max;
    v->sb_count--;
}

static void cui__vt_sb_push(cui_vt *v, const cui__vt_cell *src, int n, bool wrapped) {
    if (v->sb_max == 0) return;
    while (n > 0 && src[n - 1].ca == 0 && src[n - 1].bg == 0) n--;      /* the blank right end is not kept */
    if ((size_t)n > v->sb_cap) n = (int)v->sb_cap;
    if (v->sb_count == v->sb_max) cui__vt_sb_drop(v);
    if (n > 0) {
        for (;;) {
            if (v->sb_count == 0) v->sb_pos = 0;
            size_t oldest = v->sb_count ? v->sb_lines[v->sb_head].off : 0;
            if (v->sb_count == 0 || v->sb_pos > oldest) {            /* data runs from oldest up to pos: room at the end, then wrap */
                if (v->sb_pos + (size_t)n <= v->sb_cap) break;
                v->sb_pos = 0;
                continue;
            }
            if (v->sb_pos + (size_t)n <= oldest) break;              /* wrapped: room between pos and the oldest line */
            cui__vt_sb_drop(v);
        }
    }
    cui__vt_line *L = &v->sb_lines[(v->sb_head + v->sb_count) % v->sb_max];
    L->off = (uint32_t)v->sb_pos; L->len = (uint16_t)n; L->wrapped = wrapped;
    if (n > 0) { memcpy(v->sb + v->sb_pos, src, (size_t)n * sizeof *src); v->sb_pos += (size_t)n; }
    v->sb_count++; v->sb_total++;
}

/* ------------------------------------------------------------------------ */
/* Editing the grid                                                         */
/* ------------------------------------------------------------------------ */

#define CUI__VT_S(v) (&(v)->scr[(v)->cur])

static void cui__vt_clear_row(cui_vt *v, int y) {
    cui__vt_screen *s = CUI__VT_S(v);
    cui__vt_fill(cui__vt_row(v, s, y), v->cols, v->bg);
    s->wrap[s->map[y]] = 0;
    if ((v->pic_on >> v->cur) & 1) cui__vt_pic_erase(v, v->cur, s->map[y], 0, v->cols);
}

/* rows top..bot move up by one (the first row leaves), n times; rows that leave the top of the main screen go to the scrollback */
static void cui__vt_scroll_rows(cui_vt *v, int top, int bot, int n, bool keep) {
    cui__vt_screen *s = CUI__VT_S(v);
    n = CUI_MIN(n, bot - top + 1);
    for (int i = 0; i < n; i++) {
        uint16_t slot = s->map[top];
        if (keep && v->cur == 0 && top == 0) {
            cui__vt_sb_push(v, cui__vt_row(v, s, top), v->cols, s->wrap[slot]);
            if (v->pic_on & (CUI__VT_PIC_MAIN | CUI__VT_PIC_SB)) cui__vt_pic_pushed(v, slot);
        }
        memmove(&s->map[top], &s->map[top + 1], sizeof(uint16_t) * (size_t)(bot - top));
        s->map[bot] = slot;
        cui__vt_clear_row(v, bot);
    }
}

static void cui__vt_scroll_up(cui_vt *v, int top, int bot, int n) { cui__vt_scroll_rows(v, top, bot, n, true); }

static void cui__vt_scroll_down(cui_vt *v, int top, int bot, int n) {
    cui__vt_screen *s = CUI__VT_S(v);
    n = CUI_MIN(n, bot - top + 1);
    for (int i = 0; i < n; i++) {
        uint16_t slot = s->map[bot];
        memmove(&s->map[top + 1], &s->map[top], sizeof(uint16_t) * (size_t)(bot - top));
        s->map[top] = slot;
        cui__vt_clear_row(v, top);
    }
}

static void cui__vt_linefeed(cui_vt *v) {
    if (v->cy == v->sbot) cui__vt_scroll_up(v, v->stop, v->sbot, 1);
    else if (v->cy < v->rows - 1) v->cy++;
}

static void cui__vt_reverse_index(cui_vt *v) {
    if (v->cy == v->stop) cui__vt_scroll_down(v, v->stop, v->sbot, 1);
    else if (v->cy > 0) v->cy--;
}

/* width in cells: 0 for combining marks, 2 for wide ones */
static int cui__vt_width(uint32_t c) {
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0xFE20 && c <= 0xFE2F) || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF) || c == 0x200D || (c >= 0x1F3FB && c <= 0x1F3FF)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF) ||
        (c >= 0x1FA70 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD)) return 2;
    return 1;
}

static void cui__vt_put(cui_vt *v, uint32_t c) {
    int w = cui__vt_width(c);
    if (w == 0) return;
    cui__vt_screen *s = CUI__VT_S(v);
    if (v->pend || v->cx + w > v->cols) {
        if (v->autowrap) {
            s->wrap[s->map[v->cy]] = 1;
            v->cx = 0;
            cui__vt_linefeed(v);
        } else v->cx = v->cols - w;
        v->pend = false;
    }
    cui__vt_cell *row = cui__vt_row(v, s, v->cy);
    if (v->pic_on) cui__vt_pic_erase(v, v->cur, s->map[v->cy], v->cx, v->cx + w);     /* text over a picture takes its cells */
    if (row[v->cx].ca & CUI__VT_SPACER && v->cx > 0) row[v->cx - 1].ca = 0;         /* overwriting half of a wide character */
    row[v->cx] = (cui__vt_cell){ c | v->attr, v->fg, v->bg };
    if (w == 2) {
        row[v->cx + 1] = (cui__vt_cell){ CUI__VT_SPACER | v->attr, v->fg, v->bg };
    }
    v->cx += w;
    if (v->cx >= v->cols) { v->cx = v->cols - 1; v->pend = true; }
}

static void cui__vt_erase(cui_vt *v, int y, int x0, int x1) {   /* cells x0..x1-1 of row y, with the pen's background */
    if (x0 < 0) x0 = 0;
    if (x1 > v->cols) x1 = v->cols;
    if (x1 > x0) cui__vt_fill(cui__vt_row(v, CUI__VT_S(v), y) + x0, x1 - x0, v->bg);
    if (x1 > x0 && ((v->pic_on >> v->cur) & 1)) cui__vt_pic_erase(v, v->cur, CUI__VT_S(v)->map[y], x0, x1);
}

void cui__vt_reply(cui_vt *v, const char *s) {
    size_t n = strlen(s);
    if (v->x && cui__vt_x_reply(v, v->reply, &v->reply_n, sizeof v->reply, s, n)) return;     /* a burst past the 256 bytes */
    if ((size_t)v->reply_n + n > sizeof v->reply) return;
    memcpy(v->reply + v->reply_n, s, n);
    v->reply_n += (int)n;
}

/* ------------------------------------------------------------------------ */
/* Colors and SGR                                                           */
/* ------------------------------------------------------------------------ */

static uint16_t cui__vt_rgb_id(cui_vt *v, int r, int g, int b) {
    uint32_t c = ((uint32_t)(r & 255) << 16) | ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
    for (int i = 0; i < v->rgb_n; i++) if (v->rgb[i] == c) return (uint16_t)(CUI__VT_PALETTE + i);
    if (v->rgb_n < CUI__VT_RGB_MAX) { v->rgb[v->rgb_n] = c; return (uint16_t)(CUI__VT_PALETTE + v->rgb_n++); }
    int best = 0, bd = 1 << 30;      /* the table is full: the closest color already there */
    for (int i = 0; i < v->rgb_n; i++) {
        int dr = (int)((v->rgb[i] >> 16) & 255) - r, dg = (int)((v->rgb[i] >> 8) & 255) - g, db = (int)(v->rgb[i] & 255) - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint16_t)(CUI__VT_PALETTE + best);
}

static void cui__vt_underline(cui_vt *v, int style) {
    v->attr &= ~(uint32_t)(CUI_VT_UNDERLINE | CUI__VT_ULS_MASK);
    if (style < 0 || style > 5) style = 1;                 /* an unknown style is a plain line */
    if (style > 0) v->attr |= CUI_VT_UNDERLINE | ((uint32_t)style << CUI__VT_ULS_SHIFT);
}

/* The color after a 38, 48 or 58 at p[k]; `end` is where its colon group stops (k + 1 when it has no sub-parameters).
 * Forms: 38;5;n  38;2;r;g;b  38:5:n  38:2:r:g:b  38:2::r:g:b (an empty color space id). Returns the index of the last
 * parameter used, or -1 when the form is not understood. */
static int cui__vt_sgr_color(cui_vt *v, int k, int end, uint16_t *col) {
    if (end > k + 1) {                                      /* colons */
        int n = end - k - 1;
        const int *s = &v->p[k + 1];
        if (s[0] == 5 && n >= 2) { *col = (uint16_t)(1 + (s[1] & 255)); return end - 1; }
        if (s[0] == 2 && n >= 5) { *col = cui__vt_rgb_id(v, s[2], s[3], s[4]); return end - 1; }
        if (s[0] == 2 && n == 4) { *col = cui__vt_rgb_id(v, s[1], s[2], s[3]); return end - 1; }
        return -1;
    }
    if (k + 2 < v->np && v->p[k + 1] == 5) { *col = (uint16_t)(1 + (v->p[k + 2] & 255)); return k + 2; }
    if (k + 4 < v->np && v->p[k + 1] == 2) { *col = cui__vt_rgb_id(v, v->p[k + 2], v->p[k + 3], v->p[k + 4]); return k + 4; }
    return -1;
}

static void cui__vt_sgr(cui_vt *v) {
    if (v->np == 0) { v->attr = 0; v->fg = v->bg = 0; return; }
    for (int k = 0; k < v->np; k++) {
        int c = v->p[k];
        int end = k + 1;
        if (v->pcolon) while (end < v->np && ((v->pcolon >> end) & 1u)) end++;      /* a colon group is k .. end - 1 */
        if (c == 0) { v->attr = 0; v->fg = v->bg = 0; }
        else if (c == 1) v->attr |= CUI_VT_BOLD;
        else if (c >= 30 && c <= 37) v->fg = (uint16_t)(1 + c - 30);
        else if (c == 2) v->attr |= CUI_VT_DIM;
        else if (c == 3) v->attr |= CUI_VT_ITALIC;
        else if (c == 4) cui__vt_underline(v, end > k + 1 ? v->p[k + 1] : 1);
        else if (c == 21) cui__vt_underline(v, 2);
        else if (c == 7) v->attr |= CUI_VT_INVERSE;
        else if (c == 9) v->attr |= CUI_VT_STRIKE;
        else if (c == 22) v->attr &= ~(uint32_t)(CUI_VT_BOLD | CUI_VT_DIM);
        else if (c == 23) v->attr &= ~(uint32_t)CUI_VT_ITALIC;
        else if (c == 24) cui__vt_underline(v, 0);
        else if (c == 27) v->attr &= ~(uint32_t)CUI_VT_INVERSE;
        else if (c == 29) v->attr &= ~(uint32_t)CUI_VT_STRIKE;
        else if (c == 53) v->attr |= CUI__VT_OVER;
        else if (c == 55) v->attr &= ~CUI__VT_OVER;
        else if (c >= 90 && c <= 97) v->fg = (uint16_t)(1 + c - 90 + 8);
        else if (c >= 40 && c <= 47) v->bg = (uint16_t)(1 + c - 40);
        else if (c >= 100 && c <= 107) v->bg = (uint16_t)(1 + c - 100 + 8);
        else if (c == 39) v->fg = 0;
        else if (c == 49) v->bg = 0;
        else if (c == 38 || c == 48 || c == 58) {
            uint16_t col = 0;
            int last = cui__vt_sgr_color(v, k, end, &col);
            if (last >= 0) {
                if (c == 38) v->fg = col; else if (c == 48) v->bg = col;      /* 58, the underline color, has no room in a cell: the line takes the text color */
                end = last + 1;
            }
        }
        k = end - 1;                                        /* the sub-parameters of a colon group, or the parameters of a color, are used */
    }
}

/* ------------------------------------------------------------------------ */
/* Sequences                                                                */
/* ------------------------------------------------------------------------ */

static int cui__vt_arg(const cui_vt *v, int i, int def) { return i < v->np && v->p[i] > 0 ? v->p[i] : def; }

static void cui__vt_cursor_to(cui_vt *v, int x, int y) {
    v->cx = CUI_CLAMP(x, 0, v->cols - 1);
    v->cy = CUI_CLAMP(y, 0, v->rows - 1);
    v->pend = false;
}

static void cui__vt_switch_screen(cui_vt *v, bool alt, bool save) {
    if (alt == (v->cur == 1)) return;
    v->sync_on = false;
    if (alt) {
        if (!v->scr[1].cells && !cui__vt_screen_alloc(&v->scr[1], v->cols, v->rows)) return;
        if (save) { v->sx = v->cx; v->sy = v->cy; v->spend = v->pend; v->sattr = v->attr; v->sfg = v->fg; v->sbg = v->bg; }
        v->cur = 1;
        for (int y = 0; y < v->rows; y++) { uint16_t b = v->bg; v->bg = 0; cui__vt_clear_row(v, y); v->bg = b; }
    } else {
        v->cur = 0;
        if (v->pic_on & CUI__VT_PIC_ALT) cui__vt_pic_drop_screen(v, 1);      /* the other screen's pictures go with it */
        if (save) { cui__vt_cursor_to(v, v->sx, v->sy); v->pend = v->spend; v->attr = v->sattr; v->fg = v->sfg; v->bg = v->sbg; }
    }
    v->stop = 0; v->sbot = v->rows - 1;
}

/* ?2026h: the view keeps painting the screen as it is now until ?2026l (or about 150 ms of frames). One copy of the visible
 * cells, allocated at the first use and kept for the next ones; a program that never asks costs nothing. */
static void cui__vt_sync_begin(cui_vt *v) {
    if (!v->snap || v->snap_cols != v->cols || v->snap_rows != v->rows) {
        free(v->snap);
        v->snap_cols = v->cols; v->snap_rows = v->rows;
        v->snap = (cui__vt_cell *)malloc((size_t)v->cols * (size_t)v->rows * sizeof(cui__vt_cell));
        if (!v->snap) return;
    }
    for (int y = 0; y < v->rows; y++) memcpy(v->snap + (size_t)y * (size_t)v->cols, cui__vt_row(v, CUI__VT_S(v), y), sizeof(cui__vt_cell) * (size_t)v->cols);
    v->snap_cx = v->cx; v->snap_cy = v->cy; v->snap_cursor = v->cursor_vis;
    v->sync_on = true; v->sync_seq++;
}

static void cui__vt_mode(cui_vt *v, bool set) {
    for (int i = 0; i < v->np; i++) {
        int m = v->p[i];
        if (v->priv != '?') continue;
        switch (m) {
        case 1: v->app_cursor = set; break;
        case 7: v->autowrap = set; break;
        case 25: v->cursor_vis = set; break;
        case 47: case 1047: cui__vt_switch_screen(v, set, false); break;
        case 1049: cui__vt_switch_screen(v, set, true); break;
        case 1000: case 1002: case 1003: {
            uint8_t bit = m == 1000 ? CUI__VT_M1000 : m == 1002 ? CUI__VT_M1002 : CUI__VT_M1003;
            if (set) v->mouse_modes |= bit; else v->mouse_modes &= (uint8_t)~bit;
            break;
        }
        case 1006: v->mouse_sgr = set; break;
        case 1004: v->focus_mode = set; break;
        case 9001: v->win32_input = set; break;
        case 2004: v->bracketed = set; break;
        case 2026: if (set) { if (!v->sync_on) cui__vt_sync_begin(v); } else v->sync_on = false; break;
        case 80: if (v->x) cui__vt_sixel_mode(v, set); break;            /* DECSDM: sixel display mode (cui_vt_sixel.c) */
        default: break;
        }
    }
}

static void cui__vt_csi(cui_vt *v, char f) {
    int n = cui__vt_arg(v, 0, 1);
    cui__vt_screen *s = CUI__VT_S(v);
    if (v->x && cui__vt_query_csi(v, f, v->priv, v->inter, v->p, v->np)) return;      /* the opt-in answers (cui_vt_sixel.c) */
    if (v->priv && f != 'h' && f != 'l' && f != 'c' && f != 'q') return;
    if (v->inter) {                         /* the few sequences with an intermediate byte; the common ones pay one test */
        if (v->inter == '!' && f == 'p') {      /* soft reset */
            v->attr = 0; v->fg = v->bg = 0; v->stop = 0; v->sbot = v->rows - 1; v->autowrap = true; v->cursor_vis = true;
            v->app_cursor = false; v->pend = false;
            v->mouse_modes = 0; v->mouse_sgr = false; v->focus_mode = false; v->cursor_shape = 0; v->sync_on = false;
        } else if (v->inter == ' ' && f == 'q') {      /* cursor shape: 0, 1, 2 block, 3, 4 underline, 5, 6 bar (blinking is not drawn) */
            int m = v->np ? v->p[0] : 0;
            if (m <= 2) v->cursor_shape = CUI_VT_CURSOR_BLOCK; else if (m <= 4) v->cursor_shape = CUI_VT_CURSOR_UNDERLINE; else if (m <= 6) v->cursor_shape = CUI_VT_CURSOR_BAR;
        }
        return;
    }
    switch (f) {
    case '@': {         /* insert blank characters */
        cui__vt_cell *row = cui__vt_row(v, s, v->cy);
        n = CUI_MIN(n, v->cols - v->cx);
        memmove(row + v->cx + n, row + v->cx, sizeof *row * (size_t)(v->cols - v->cx - n));
        cui__vt_fill(row + v->cx, n, v->bg);
        if ((v->pic_on >> v->cur) & 1) cui__vt_pic_shift(v, s->map[v->cy], v->cx, n);
        v->pend = false;
        break;
    }
    case 'A': cui__vt_cursor_to(v, v->cx, v->cy >= v->stop ? CUI_MAX(v->stop, v->cy - n) : v->cy - n); break;
    case 'B': case 'e': cui__vt_cursor_to(v, v->cx, v->cy <= v->sbot ? CUI_MIN(v->sbot, v->cy + n) : v->cy + n); break;
    case 'C': cui__vt_cursor_to(v, v->cx + n, v->cy); break;
    case 'D': cui__vt_cursor_to(v, v->cx - n, v->cy); break;
    case 'E': cui__vt_cursor_to(v, 0, v->cy + n); break;
    case 'F': cui__vt_cursor_to(v, 0, v->cy - n); break;
    case 'G': case '`': cui__vt_cursor_to(v, n - 1, v->cy); break;
    case 'H': case 'f': cui__vt_cursor_to(v, cui__vt_arg(v, 1, 1) - 1, cui__vt_arg(v, 0, 1) - 1); break;
    case 'd': cui__vt_cursor_to(v, v->cx, n - 1); break;
    case 'I': for (int i = 0; i < n; i++) { do v->cx++; while (v->cx < v->cols - 1 && !v->tabs[v->cx]); } cui__vt_cursor_to(v, v->cx, v->cy); break;
    case 'Z': for (int i = 0; i < n; i++) { do v->cx--; while (v->cx > 0 && !v->tabs[v->cx]); } cui__vt_cursor_to(v, v->cx, v->cy); break;
    case 'J': {
        int m = v->np ? v->p[0] : 0;
        if (m == 0) { cui__vt_erase(v, v->cy, v->cx, v->cols); for (int y = v->cy + 1; y < v->rows; y++) cui__vt_erase(v, y, 0, v->cols); }
        else if (m == 1) { for (int y = 0; y < v->cy; y++) cui__vt_erase(v, y, 0, v->cols); cui__vt_erase(v, v->cy, 0, v->cx + 1); }
        else if (m == 2 || m == 3) { if (m == 2) for (int y = 0; y < v->rows; y++) cui__vt_erase(v, y, 0, v->cols);
                                     else if (v->cur == 0) { v->sb_count = 0; v->sb_pos = 0; if (v->pic_on & CUI__VT_PIC_SB) cui__vt_pic_trim_sb(v); } }
        v->pend = false;
        break;
    }
    case 'K': {
        int m = v->np ? v->p[0] : 0;
        if (m == 0) cui__vt_erase(v, v->cy, v->cx, v->cols);
        else if (m == 1) cui__vt_erase(v, v->cy, 0, v->cx + 1);
        else if (m == 2) cui__vt_erase(v, v->cy, 0, v->cols);
        v->pend = false;
        break;
    }
    case 'L': if (v->cy >= v->stop && v->cy <= v->sbot) { cui__vt_scroll_down(v, v->cy, v->sbot, n); v->cx = 0; v->pend = false; } break;
    case 'M': if (v->cy >= v->stop && v->cy <= v->sbot) { cui__vt_scroll_rows(v, v->cy, v->sbot, n, false); v->cx = 0; v->pend = false; } break;
    case 'P': {
        cui__vt_cell *row = cui__vt_row(v, s, v->cy);
        n = CUI_MIN(n, v->cols - v->cx);
        memmove(row + v->cx, row + v->cx + n, sizeof *row * (size_t)(v->cols - v->cx - n));
        cui__vt_fill(row + v->cols - n, n, v->bg);
        if ((v->pic_on >> v->cur) & 1) cui__vt_pic_shift(v, s->map[v->cy], v->cx, -n);
        v->pend = false;
        break;
    }
    case 'X': cui__vt_erase(v, v->cy, v->cx, v->cx + n); break;
    case 'S': cui__vt_scroll_up(v, v->stop, v->sbot, n); break;
    case 'T': if (v->np <= 1) cui__vt_scroll_down(v, v->stop, v->sbot, n); break;
    case 'm': cui__vt_sgr(v); break;
    case 'n': {
        int m = v->np ? v->p[0] : 0;
        if (m == 5) cui__vt_reply(v, "\x1b[0n");
        else if (m == 6) { char b[32]; snprintf(b, sizeof b, "\x1b[%d;%dR", v->cy + 1, (v->pend ? v->cols : v->cx + 1)); cui__vt_reply(v, b); }
        break;
    }
    case 'c':
        if (v->priv == '>') cui__vt_reply(v, "\x1b[>0;0;0c");
        else if (!v->priv && (v->np == 0 || v->p[0] == 0)) cui__vt_reply(v, "\x1b[?1;0c");
        break;
    case 't': {         /* size reports: the text area and the cell in pixels, the grid in characters */
        int m = v->np ? v->p[0] : 0;
        char b[48];
        if (m == 14 && v->cell_w > 0) { snprintf(b, sizeof b, "\x1b[4;%d;%dt", v->rows * v->cell_h, v->cols * v->cell_w); cui__vt_reply(v, b); }
        else if (m == 16 && v->cell_w > 0) { snprintf(b, sizeof b, "\x1b[6;%d;%dt", v->cell_h, v->cell_w); cui__vt_reply(v, b); }
        else if (m == 18) { snprintf(b, sizeof b, "\x1b[8;%d;%dt", v->rows, v->cols); cui__vt_reply(v, b); }
        break;
    }
    case 'q': if (v->priv == '>') cui__vt_reply(v, "\x1bP>|cui_vt\x1b\\"); break;      /* XTVERSION */
    case 'r': {
        int top = cui__vt_arg(v, 0, 1) - 1, bot = cui__vt_arg(v, 1, v->rows) - 1;
        if (bot >= v->rows) bot = v->rows - 1;
        if (top < bot) { v->stop = top; v->sbot = bot; }
        cui__vt_cursor_to(v, 0, 0);
        break;
    }
    case 's': v->sx = v->cx; v->sy = v->cy; v->spend = v->pend; v->sattr = v->attr; v->sfg = v->fg; v->sbg = v->bg; break;
    case 'u': cui__vt_cursor_to(v, v->sx, v->sy); v->pend = v->spend; v->attr = v->sattr; v->fg = v->sfg; v->bg = v->sbg; break;
    case 'h': cui__vt_mode(v, true); break;
    case 'l': cui__vt_mode(v, false); break;
    case 'g': if (v->np == 0 || v->p[0] == 0) v->tabs[v->cx] = 0; else if (v->p[0] == 3) memset(v->tabs, 0, CUI__VT_MAX_COLS); break;
    default: break;
    }
}

static void cui__vt_esc(cui_vt *v, char c) {
    switch (c) {
    case '7': v->sx = v->cx; v->sy = v->cy; v->spend = v->pend; v->sattr = v->attr; v->sfg = v->fg; v->sbg = v->bg; break;
    case '8': cui__vt_cursor_to(v, v->sx, v->sy); v->pend = v->spend; v->attr = v->sattr; v->fg = v->sfg; v->bg = v->sbg; break;
    case 'D': cui__vt_linefeed(v); break;
    case 'E': v->cx = 0; v->pend = false; cui__vt_linefeed(v); break;
    case 'M': cui__vt_reverse_index(v); break;
    case 'H': v->tabs[v->cx] = 1; break;
    case 'c': {
        v->attr = 0; v->fg = v->bg = 0; v->stop = 0; v->sbot = v->rows - 1; v->autowrap = true; v->cursor_vis = true;
        v->app_cursor = v->bracketed = false;
        v->mouse_modes = 0; v->mouse_sgr = false; v->focus_mode = false; v->cursor_shape = 0; v->sync_on = false;
        v->win32_input = false;     /* a full reset only: the soft reset leaves it, the console host asks for it once and never again */
        if (v->x) cui__vt_x_reset(v);
        if (v->cur == 1) cui__vt_switch_screen(v, false, false);
        for (int y = 0; y < v->rows; y++) cui__vt_clear_row(v, y);
        cui__vt_cursor_to(v, 0, 0);
        break;
    }
    default: break;
    }
}

static void cui__vt_osc_end(cui_vt *v, bool bel) {
    v->osc[v->osc_n] = 0;
    int code = 0, i = 0;
    while (i < v->osc_n && v->osc[i] >= '0' && v->osc[i] <= '9' && code < 100000) code = code * 10 + (v->osc[i++] - '0');
    if (v->x && i < v->osc_n && v->osc[i] == ';') cui__vt_query_osc(v, code, v->osc + i + 1, v->osc_n - i - 1, bel);   /* color queries (cui_vt_sixel.c) */
    if ((code == 0 || code == 2) && i < v->osc_n && v->osc[i] == ';') {
        const char *t = v->osc + i + 1;
        size_t k = 0;
        for (; t[k] && k + 1 < sizeof v->title; k++) v->title[k] = (unsigned char)t[k] < 32 ? ' ' : t[k];
        v->title[k] = 0;
    }
}

static void cui__vt_control(cui_vt *v, unsigned char c) {
    switch (c) {
    case 0x08: if (v->cx > 0) v->cx--; v->pend = false; break;
    case 0x09: do v->cx++; while (v->cx < v->cols - 1 && !v->tabs[v->cx]); if (v->cx >= v->cols) v->cx = v->cols - 1; v->pend = false; break;
    case 0x0A: case 0x0B: case 0x0C: cui__vt_linefeed(v); break;      /* the terminal behind ConPTY sends \r\n: LF alone keeps the column, as xterm */
    case 0x0D: v->cx = 0; v->pend = false; break;
    default: break;
    }
}

void cui_vt_write(cui_vt *v, const char *data, size_t len) {
    if (!v) return;
    const unsigned char *d = (const unsigned char *)data;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = d[i];
        switch (v->state) {
        case CUI__VT_GROUND:
            if (c >= 0x20 && c < 0x7F) {
                v->need = 0;
                /* the common case: a run of printable ASCII written straight into the row */
                cui__vt_put(v, c);
                break;
            }
            if (c < 0x20) {
                v->need = 0;
                if (c == 0x1B) { v->state = CUI__VT_ESC; v->np = 0; v->pcolon = 0; v->pany = false; v->priv = v->inter = 0; }
                else cui__vt_control(v, c);
            } else if (c >= 0xC2 && c <= 0xF4) {
                v->need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
                v->cp = c & (0x3Fu >> v->need);
            } else if (c >= 0x80 && c < 0xC0 && v->need > 0) {
                v->cp = (v->cp << 6) | (c & 0x3Fu);
                if (--v->need == 0) cui__vt_put(v, v->cp > 0x10FFFF ? 0xFFFD : v->cp);
            } else { v->need = 0; cui__vt_put(v, 0xFFFD); }
            break;
        case CUI__VT_ESC:
            if (c == '[') { v->state = CUI__VT_CSI; v->np = 0; v->pcolon = 0; v->pany = false; memset(v->p, 0, sizeof v->p); v->priv = v->inter = 0; }
            else if (c == ']') { v->state = CUI__VT_OSC; v->osc_n = 0; }
            else if (c == 'P' && v->x) { v->state = CUI__VT_DCS; v->np = 0; v->pcolon = 0; v->pany = false; memset(v->p, 0, sizeof v->p); v->priv = v->inter = 0; }
            else if (c == 'P' || c == 'X' || c == '^' || c == '_') v->state = CUI__VT_STR;
            else if (c >= 0x20 && c <= 0x2F) v->state = CUI__VT_ESC_INT;       /* ( ) # % and the like: one more byte */
            else if (c == 0x1B) {}
            else if (c < 0x20) { cui__vt_control(v, c); v->state = CUI__VT_GROUND; }
            else { cui__vt_esc(v, (char)c); v->state = CUI__VT_GROUND; }
            break;
        case CUI__VT_ESC_INT: v->state = CUI__VT_GROUND; break;
        case CUI__VT_CSI:
            if (c >= '0' && c <= '9') {
                if (v->np == 0) v->np = 1;
                int *p = &v->p[CUI_MIN(v->np, 16) - 1];
                if (*p < 100000) *p = *p * 10 + (c - '0');
                v->pany = true;
            } else if (c == ';' || c == ':') {
                if (v->np == 0) v->np = 1;
                if (v->np < 16) v->np++;
                if (c == ':') v->pcolon |= (uint16_t)(1u << (v->np - 1));
                v->pany = true;
            } else if (c >= '<' && c <= '?') v->priv = (char)c;
            else if (c >= 0x20 && c <= 0x2F) v->inter = (char)c;
            else if (c >= 0x40 && c <= 0x7E) {
                if (v->np > 16) v->np = 16;
                cui__vt_csi(v, (char)c);
                v->state = CUI__VT_GROUND;
            } else if (c == 0x1B) { v->state = CUI__VT_ESC; v->np = 0; v->priv = v->inter = 0; }
            else if (c < 0x20) cui__vt_control(v, c);
            else v->state = CUI__VT_GROUND;
            break;
        case CUI__VT_OSC:
            if (c == 0x07) { cui__vt_osc_end(v, true); v->state = CUI__VT_GROUND; }
            else if (c == 0x1B) v->state = CUI__VT_OSC_ESC;
            else if (v->osc_n + 1 < (int)sizeof v->osc) v->osc[v->osc_n++] = (char)c;
            break;
        case CUI__VT_OSC_ESC:
            if (c == '\\') { cui__vt_osc_end(v, false); v->state = CUI__VT_GROUND; }
            else { v->state = CUI__VT_ESC; i--; }
            break;
        case CUI__VT_STR:
            if (c == 0x07) v->state = CUI__VT_GROUND;
            else if (c == 0x1B) v->state = CUI__VT_STR_ESC;
            break;
        case CUI__VT_STR_ESC:
            if (c == '\\') v->state = CUI__VT_GROUND;
            else { v->state = CUI__VT_ESC; i--; }
            break;
        case CUI__VT_DCS:                   /* only with an extension: DCS P1;P2;... final, as a CSI's parameters */
            if (c >= '0' && c <= '9') { if (v->np == 0) v->np = 1; int *p = &v->p[CUI_MIN(v->np, 16) - 1]; if (*p < 100000) *p = *p * 10 + (c - '0'); }
            else if (c == ';' || c == ':') { if (v->np == 0) v->np = 1; if (v->np < 16) v->np++; }
            else if (c >= '<' && c <= '?') v->priv = (char)c;
            else if (c >= 0x20 && c <= 0x2F) v->inter = (char)c;
            else if (c >= 0x40 && c <= 0x7E) v->state = c == 'q' && !v->priv && !v->inter && cui__vt_sixel_begin(v, v->p, v->np) ? CUI__VT_SIXEL : CUI__VT_STR;
            else if (c == 0x1B) v->state = CUI__VT_STR_ESC;
            else if (c == 0x07 || c == 0x18 || c == 0x1A) v->state = CUI__VT_GROUND;
            break;
        case CUI__VT_SIXEL: {               /* picture data, read in runs up to the terminator */
            i += cui__vt_sixel_feed(v, d + i, len - i);
            if (i >= len) break;
            cui__vt_sixel_end(v);
            if (d[i] == 0x1B) v->state = CUI__VT_STR_ESC;      /* ESC \ ends it; any other ESC starts its sequence */
            else v->state = CUI__VT_GROUND;                    /* CAN, SUB */
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Size, queries                                                            */
/* ------------------------------------------------------------------------ */

void cui_vt_resize(cui_vt *v, int cols, int rows) {
    if (!v) return;
    cols = CUI_CLAMP(cols, 2, CUI__VT_MAX_COLS);
    rows = CUI_CLAMP(rows, 1, CUI__VT_MAX_ROWS);
    if (cols == v->cols && rows == v->rows) return;
    v->sync_on = false;
    /* the cursor must stay on screen: rows above it move into the scrollback */
    if (v->cur == 0 && v->cy >= rows) {
        int drop = v->cy - rows + 1;
        for (int i = 0; i < drop; i++) {
            cui__vt_screen *s = &v->scr[0];
            cui__vt_sb_push(v, cui__vt_row(v, s, 0), v->cols, s->wrap[s->map[0]]);
            if (v->pic_on & (CUI__VT_PIC_MAIN | CUI__VT_PIC_SB)) cui__vt_pic_pushed(v, s->map[0]);
            uint16_t slot = s->map[0];
            memmove(&s->map[0], &s->map[1], sizeof(uint16_t) * (size_t)(v->rows - 1));
            s->map[v->rows - 1] = slot;
        }
        v->cy -= drop;
    }
    for (int k = 0; k < 2; k++) {
        cui__vt_screen *old = &v->scr[k], n;
        if (!old->cells) continue;
        if (!cui__vt_screen_alloc(&n, cols, rows)) { cui__vt_screen_free(&n); return; }
        int cy = CUI_MIN(v->rows, rows), cx = CUI_MIN(v->cols, cols);
        for (int y = 0; y < cy; y++) {
            memcpy(n.cells + (size_t)y * (size_t)cols, old->cells + (size_t)old->map[y] * (size_t)v->cols, sizeof(cui__vt_cell) * (size_t)cx);
            n.wrap[y] = old->wrap[old->map[y]];
        }
        if (v->x) cui__vt_x_resize(v, k, old->map, v->rows, cols, rows);     /* pictures follow their rows to the new slots */
        cui__vt_screen_free(old);
        *old = n;
    }
    v->cols = cols; v->rows = rows;
    v->stop = 0; v->sbot = rows - 1;
    v->cx = CUI_MIN(v->cx, cols - 1); v->cy = CUI_MIN(v->cy, rows - 1);
    v->pend = false;
}

size_t cui_vt_reply(cui_vt *v, char *out, size_t cap) {
    if (!v) return 0;
    if (v->x) { size_t k = cui__vt_x_take(v, out, cap); if (k) return k; }
    size_t n = CUI_MIN((size_t)v->reply_n, cap);
    memcpy(out, v->reply, n);
    memmove(v->reply, v->reply + n, (size_t)v->reply_n - n);
    v->reply_n -= (int)n;
    return n;
}

const char *cui_vt_title(const cui_vt *v) { return v ? v->title : ""; }
void cui_vt_size(const cui_vt *v, int *cols, int *rows) { if (cols) *cols = v->cols; if (rows) *rows = v->rows; }
void cui_vt_cursor(const cui_vt *v, int *x, int *y) { if (x) *x = v->cx; if (y) *y = v->cy; }
bool cui_vt_alt_screen(const cui_vt *v) { return v->cur == 1; }
bool cui_vt_app_cursor(const cui_vt *v) { return v->app_cursor; }
bool cui_vt_bracketed_paste(const cui_vt *v) { return v->bracketed; }
bool cui_vt_win32_input(const cui_vt *v) { return v && v->win32_input; }
bool cui_vt_mouse_reporting(const cui_vt *v) { return v && v->mouse_modes != 0; }
bool cui_vt_synchronized(const cui_vt *v) { return v && v->sync_on; }
int cui_vt_cursor_shape(const cui_vt *v) { return v ? v->cursor_shape : CUI_VT_CURSOR_BLOCK; }
int cui_vt_scrollback_count(const cui_vt *v) { return v->sb_count; }

/* the cells of row `row` counted from the top of the live screen: negative rows are scrollback, -1 the newest */
static const cui__vt_cell *cui__vt_cells_at(const cui_vt *v, int64_t row, int *n, bool *wrapped) {
    if (row >= 0) {
        if (row >= v->rows) { *n = 0; return NULL; }
        const cui__vt_screen *s = &v->scr[v->cur];
        *n = v->cols;
        *wrapped = s->wrap[s->map[row]] != 0;
        return cui__vt_row(v, s, (int)row);
    }
    int64_t i = (int64_t)v->sb_count + row;
    if (i < 0 || i >= v->sb_count) { *n = 0; return NULL; }
    const cui__vt_line *L = &v->sb_lines[(v->sb_head + (int)i) % v->sb_max];
    *n = L->len; *wrapped = L->wrapped != 0;
    return v->sb + L->off;
}

static size_t cui__vt_utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* the text of cells c0..c1-1 of a row, blanks as spaces; `trim` cuts the blank right end */
static size_t cui__vt_text(const cui__vt_cell *cells, int n, int c0, int c1, bool trim, char *out, size_t cap) {
    size_t len = 0;
    int last = c1;
    if (trim) { last = CUI_MIN(c1, n); while (last > c0 && CUI__VT_CP(cells[last - 1].ca) == 0) last--; }
    for (int x = c0; x < last && len + 5 < cap; x++) {
        uint32_t c = x < n ? cells[x].ca : 0;
        if (c & CUI__VT_SPACER) continue;
        c = CUI__VT_CP(c);
        len += cui__vt_utf8(c ? c : ' ', out + len);
    }
    if (out && cap) out[len < cap ? len : cap - 1] = 0;
    return len;
}

size_t cui_vt_line_text(const cui_vt *v, int row, char *out, size_t cap) {
    int n; bool w;
    const cui__vt_cell *c = cui__vt_cells_at(v, row, &n, &w);
    if (!c) { if (cap) out[0] = 0; return 0; }
    return cui__vt_text(c, n, 0, n, true, out, cap);
}

bool cui_vt_cell_at(const cui_vt *v, int x, int y, cui_vt_cell *out) {
    if (x < 0 || y < 0 || x >= v->cols || y >= v->rows) return false;
    const cui__vt_cell *c = &cui__vt_row(v, &v->scr[v->cur], y)[x];
    out->ch = CUI__VT_CP(c->ca);
    {
        uint32_t a = c->ca, style = CUI__VT_ULS(a);
        int at = (int)(a & 0x3F000000u);                                          /* bold .. strike: the same bits as CUI_VT_* */
        if (style >= 2) at |= CUI_VT_UNDERLINE_DOUBLE << (style - 2);
        if (a & CUI__VT_OVER) at |= CUI_VT_OVERLINE;
        out->attrs = at;
    }
    for (int k = 0; k < 2; k++) {
        uint16_t id = k ? c->bg : c->fg;
        int32_t r = id == 0 ? -1 : id < CUI__VT_PALETTE ? id - 1 : (int32_t)(0x1000000u | v->rgb[id - CUI__VT_PALETTE]);
        if (k) out->bg = r; else out->fg = r;
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* What the opt-in parts (cui_vt_sixel.c) reach                             */
/* ------------------------------------------------------------------------ */

void cui__vt_get_info(const cui_vt *v, cui__vt_info *o) {
    o->cols = v->cols; o->rows = v->rows; o->cur = v->cur; o->cx = v->cx; o->cy = v->cy; o->stop = v->stop; o->sbot = v->sbot;
    o->sb_count = v->sb_count; o->sb_max = v->sb_max; o->sb_total = v->sb_total;
    o->cell_w = v->cell_w; o->cell_h = v->cell_h; o->cell_w16 = v->cell_w16;
}
cui__vt_x *cui__vt_ext(const cui_vt *v) { return v->x; }
void cui__vt_set_ext(cui_vt *v, cui__vt_x *x) { v->x = x; }
void cui__vt_set_pic_bits(cui_vt *v, uint8_t bits) { v->pic_on = bits; }
int cui__vt_slot(const cui_vt *v, int scr, int y) { return v->scr[scr].map ? v->scr[scr].map[y] : y; }
void cui__vt_set_cursor(cui_vt *v, int x, int y) { cui__vt_cursor_to(v, x, y); }

void cui__vt_scroll_lines(cui_vt *v, int n) {
    v->cy = v->sbot;
    cui__vt_scroll_up(v, v->stop, v->sbot, n);
}

int cui__vt_mode_report(const cui_vt *v, int m, bool dec) {
    if (!dec) return 0;                                     /* no ANSI mode (IRM, LNM...) can be set here */
    bool on;
    switch (m) {
    case 1: on = v->app_cursor; break;
    case 7: on = v->autowrap; break;
    case 25: on = v->cursor_vis; break;
    case 47: case 1047: case 1049: on = v->cur == 1; break;
    case 1000: on = (v->mouse_modes & CUI__VT_M1000) != 0; break;
    case 1002: on = (v->mouse_modes & CUI__VT_M1002) != 0; break;
    case 1003: on = (v->mouse_modes & CUI__VT_M1003) != 0; break;
    case 1004: on = v->focus_mode; break;
    case 1006: on = v->mouse_sgr; break;
    case 2004: on = v->bracketed; break;
    case 2026: on = v->sync_on; break;
    case 9001: on = v->win32_input; break;
    default: return 0;
    }
    return on ? 1 : 2;
}

/* ------------------------------------------------------------------------ */
/* View: keys                                                               */
/* ------------------------------------------------------------------------ */

typedef struct { int64_t row; int col; } cui__vt_pos;      /* row counted from the first scrollback line ever (sb_total based) */

typedef struct {
    int off;                          /* rows scrolled back from the live screen */
    bool selecting, has_sel, init;
    cui__vt_pos a, b;
    float press_x, press_y;
    uint64_t seen_total;
    char out[4096]; int out_n;
    bool w_on;                        /* key records are being sent: the next fields are live */
    uint8_t w_cs;                     /* Shift, Ctrl and Alt as the program was last told */
    char w_pend[256]; int w_pend_n;   /* text not yet sent because a frame's output was full */
    uint8_t w_held[CUI_KEY_COUNT];    /* keys whose press went out and whose release has not */
    uint16_t w_uc[CUI_KEY_COUNT];     /* the character of that press, repeated in the release */
    uint8_t btn_down;                 /* buttons whose press went to the program */
    uint32_t sync_seen; double sync_t0;   /* the update the view last saw open, and when the view first drew it */
    int mcol, mrow; bool mvalid;      /* the last cell the mouse was reported in */
    bool finit, fin;                  /* the view had the keyboard and the window had the focus, as of the last frame */
} cui__vt_state;

static void cui__vt_out(cui__vt_state *st, const char *s, int n) {
    if (st->out_n + n > (int)sizeof st->out) return;
    memcpy(st->out + st->out_n, s, (size_t)n);
    st->out_n += n;
}
static void cui__vt_outs(cui__vt_state *st, const char *s) { cui__vt_out(st, s, (int)strlen(s)); }

/* ------------------------------------------------------------------------ */
/* View: mouse reports                                                      */
/* ------------------------------------------------------------------------ */

typedef struct { cui_rect pr; float padx, pady, cw, lh; } cui__vt_geom;

/* the cell under a point, 0-based and clamped to the grid: the geometry selection uses */
static void cui__vt_grid_at(const cui_vt *v, const cui__vt_geom *g, float mx, float my, int *col, int *row) {
    *row = CUI_CLAMP((int)floorf((my - g->pr.y - g->pady) / g->lh), 0, v->rows - 1);
    *col = CUI_CLAMP((int)floorf((mx - g->pr.x - g->padx) / g->cw), 0, v->cols - 1);
}

/* one report: SGR (ESC [ < b ; x ; y M or m) or the classic ESC [ M with bytes, coordinates capped at 223 */
static void cui__vt_mouse_out(cui__vt_state *st, const cui_vt *v, int code, int col, int row, bool release) {
    char b[32];
    int x = col + 1, y = row + 1, n;
    if (v->mouse_sgr) n = snprintf(b, sizeof b, "\x1b[<%d;%d;%d%c", code, x, y, release ? 'm' : 'M');
    else {
        if (release) code = (code & ~3) | 3;
        b[0] = 0x1b; b[1] = '['; b[2] = 'M';
        b[3] = (char)(32 + code); b[4] = (char)(32 + CUI_MIN(x, 223)); b[5] = (char)(32 + CUI_MIN(y, 223));
        n = 6;
    }
    cui__vt_out(st, b, n);
}

static int cui__vt_mouse_mods(int mods) {
    return ((mods & CUI_MOD_SHIFT) ? 4 : 0) | ((mods & CUI_MOD_ALT) ? 8 : 0) | ((mods & CUI_MOD_CTRL) ? 16 : 0);
}

/* the buttons, motion and wheel of this frame as reports. `can_press`: a new press goes to the program (no Shift).
 * A release is always reported while a press is open, wherever the pointer is, so the program never sees a stuck button. */
static void cui__vt_mouse_frame(cui_ctx *ui, cui__vt_state *st, const cui_vt *v, const cui__vt_geom *g, bool hovered, bool can_press) {
    int col = st->mcol, row = st->mrow;
    if (ui->mouse_inside) cui__vt_grid_at(v, g, ui->mx, ui->my, &col, &row);
    int mods = cui__vt_mouse_mods(ui->mods);
    bool moved = !st->mvalid || col != st->mcol || row != st->mrow;
    for (int b = 0; b < 3; b++) {
        uint8_t bit = (uint8_t)(1u << b);
        int code = b == CUI_MOUSE_RIGHT ? 2 : b == CUI_MOUSE_MIDDLE ? 1 : 0;
        if ((st->btn_down & bit) && ui->mreleased[b]) { cui__vt_mouse_out(st, v, code + mods, col, row, true); st->btn_down &= (uint8_t)~bit; }
        if (can_press && hovered && ui->mpressed[b]) {
            cui__vt_mouse_out(st, v, code + mods, col, row, false);
            st->btn_down |= bit; moved = false;
            st->has_sel = false; st->selecting = false;
        }
        if ((st->btn_down & bit) && !ui->mdown[b]) { cui__vt_mouse_out(st, v, code + mods, col, row, true); st->btn_down &= (uint8_t)~bit; }
    }
    if (moved && (st->btn_down || hovered)) {
        if (st->btn_down) {
            int first = st->btn_down & 1 ? 0 : st->btn_down & 4 ? 1 : 2;       /* the lowest button held, as a protocol code */
            if (v->mouse_modes & (CUI__VT_M1002 | CUI__VT_M1003)) cui__vt_mouse_out(st, v, first + 32 + mods, col, row, false);
        } else if (can_press && (v->mouse_modes & CUI__VT_M1003)) cui__vt_mouse_out(st, v, 3 + 32 + mods, col, row, false);
    }
    if (st->btn_down || hovered) { st->mcol = col; st->mrow = row; st->mvalid = true; }
    else st->mvalid = false;
}

/* ESC [ code ; mods final, or without mods ESC [ final (or ESC O final in application mode) */
static void cui__vt_out_key(cui__vt_state *st, bool app, int mods, const char *code, char final) {
    char b[24];
    int m = 1 + ((mods & CUI_MOD_SHIFT) ? 1 : 0) + ((mods & CUI_MOD_ALT) ? 2 : 0) + ((mods & CUI_MOD_CTRL) ? 4 : 0);
    if (m > 1) snprintf(b, sizeof b, "\x1b[%s;%d%c", *code ? code : "1", m, final);
    else if (*code) snprintf(b, sizeof b, "\x1b[%s%c", code, final);
    else snprintf(b, sizeof b, app ? "\x1bO%c" : "\x1b[%c", final);
    cui__vt_outs(st, b);
}

static char cui__vt_key_char(int k, bool shift) {
    if (k >= CUI_KEY_A && k <= CUI_KEY_Z) return (char)((shift ? 'A' : 'a') + (k - CUI_KEY_A));
    if (k >= CUI_KEY_0 && k <= CUI_KEY_9) return (char)('0' + (k - CUI_KEY_0));
    switch (k) {
    case CUI_KEY_MINUS: return '-'; case CUI_KEY_EQUAL: return '='; case CUI_KEY_COMMA: return ',';
    case CUI_KEY_PERIOD: return '.'; case CUI_KEY_SLASH: return '/'; case CUI_KEY_BACKSLASH: return '\\';
    case CUI_KEY_SPACE: return ' ';
    default: return 0;
    }
}

/* turns this frame's keys into the bytes a terminal sends; clipboard and scrollback keys are left to the caller */
static void cui__vt_keys(cui_ctx *ui, const cui_vt *v, cui__vt_state *st) {
    int mods = ui->mods;
    bool ctrl = (mods & CUI_MOD_CTRL) != 0, alt = (mods & CUI_MOD_ALT) != 0, shift = (mods & CUI_MOD_SHIFT) != 0;
    bool altgr = ctrl && alt;       /* Windows reports AltGr as Ctrl+Alt: the text comes as characters */
    bool has_text = ui->text_in_len > 0;
    if (has_text && !(ctrl && !alt)) {
        if (alt && !ctrl) cui__vt_outs(st, "\x1b");
        cui__vt_out(st, ui->text_in, ui->text_in_len);
    }
    ui->text_in_len = 0; ui->text_in[0] = 0;
    for (int k = CUI_KEY_NONE + 1; k < CUI_KEY_COUNT; k++) {
        int reps = ui->kpressed[k];
        if (!reps) continue;
        bool scroll_key = shift && !ctrl && (k == CUI_KEY_PAGE_UP || k == CUI_KEY_PAGE_DOWN || k == CUI_KEY_HOME || k == CUI_KEY_END) && v->cur == 0;
        bool sup = (mods & CUI_MOD_SUPER) != 0;
        bool clip_key = (k == CUI_KEY_C && ((ctrl && (st->has_sel || shift)) || sup)) || (k == CUI_KEY_V && ((ctrl && !alt) || sup));
        if (scroll_key || clip_key) continue;
        char fin = 0; const char *code = "";
        switch (k) {
        case CUI_KEY_UP: fin = 'A'; break;
        case CUI_KEY_DOWN: fin = 'B'; break;
        case CUI_KEY_RIGHT: fin = 'C'; break;
        case CUI_KEY_LEFT: fin = 'D'; break;
        case CUI_KEY_HOME: fin = 'H'; break;
        case CUI_KEY_END: fin = 'F'; break;
        case CUI_KEY_DELETE: fin = '~'; code = "3"; break;
        case CUI_KEY_INSERT: fin = '~'; code = "2"; break;
        case CUI_KEY_PAGE_UP: fin = '~'; code = "5"; break;
        case CUI_KEY_PAGE_DOWN: fin = '~'; code = "6"; break;
        case CUI_KEY_F1: fin = 'P'; break;
        case CUI_KEY_F2: fin = 'Q'; break;
        case CUI_KEY_F3: fin = 'R'; break;
        case CUI_KEY_F4: fin = 'S'; break;
        default: break;
        }
        if (k >= CUI_KEY_F5 && k <= CUI_KEY_F12) {
            static const char *const fc[] = { "15", "17", "18", "19", "20", "21", "23", "24" };
            fin = '~'; code = fc[k - CUI_KEY_F5];
        }
        for (int r = 0; r < reps; r++) {
            if (fin) {
                bool ss3 = k >= CUI_KEY_F1 && k <= CUI_KEY_F4;
                if (ss3 && !(mods & (CUI_MOD_SHIFT | CUI_MOD_ALT | CUI_MOD_CTRL))) { char b[4] = { 0x1b, 'O', fin, 0 }; cui__vt_outs(st, b); }
                else cui__vt_out_key(st, v->app_cursor, mods, code, fin);
                continue;
            }
            switch (k) {
            case CUI_KEY_ENTER: if (alt) cui__vt_outs(st, "\x1b"); cui__vt_outs(st, ctrl ? "\n" : "\r"); continue;
            case CUI_KEY_TAB: cui__vt_outs(st, shift ? "\x1b[Z" : "\t"); continue;
            case CUI_KEY_ESCAPE: cui__vt_outs(st, "\x1b"); continue;
            case CUI_KEY_BACKSPACE: if (alt) cui__vt_outs(st, "\x1b"); cui__vt_outs(st, ctrl ? "\x08" : "\x7f"); continue;
            default: break;
            }
            char ch = cui__vt_key_char(k, shift);
            if (!ch || altgr) continue;
            if (ctrl) {
                char c;
                if (ch >= 'a' && ch <= 'z') c = (char)(ch - 'a' + 1);
                else if (ch >= 'A' && ch <= 'Z') c = (char)(ch - 'A' + 1);
                else if (ch == ' ') c = 0;
                else if (ch == '\\') c = 0x1c;
                else if (ch == '/' || ch == '-') c = 0x1f;
                else continue;
                cui__vt_out(st, &c, 1);
            } else if (alt && !has_text) {
                cui__vt_outs(st, "\x1b");
                cui__vt_out(st, &ch, 1);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* View: Windows key records (mode 9001, opt in with .win32_keys)           */
/* ------------------------------------------------------------------------ */

/* The record is ESC [ Vk ; Sc ; Uc ; Kd ; Cs ; Rc _ : the virtual key, the scan code, the UTF-16 unit, down (1) or up (0),
 * the control key state, the repeat count. Scan codes are a fixed US layout table; Left and Right modifiers are not told
 * apart (CUI reports one flag each), so the Left ones are sent. Numpad and F13 and up are keys CUI does not have.
 * The tables below are indexed by cui_key and follow its order: none, Tab, Enter, Escape, Backspace, Delete, Space, Left,
 * Right, Up, Down, Home, End, Page Up, Page Down, A to Z, 0 to 9, - = , . /, F1 to F12, backslash, grave, Insert, [ and ]. */
enum { CUI__VT_CS_RALT = 0x01, CUI__VT_CS_LALT = 0x02, CUI__VT_CS_LCTRL = 0x08, CUI__VT_CS_SHIFT = 0x10, CUI__VT_CS_ENHANCED = 0x100 };

static const uint8_t cui__vt_w_vk[CUI_KEY_COUNT] = {
    0, 9, 13, 27, 8, 46, 32, 37, 39, 38, 40, 36, 35, 33, 34,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,
    48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 189, 187, 188, 190, 191,
    112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 220, 192, 45, 219, 221 };
static const uint8_t cui__vt_w_sc[CUI_KEY_COUNT] = {
    0, 15, 28, 1, 14, 83, 57, 75, 77, 72, 80, 71, 79, 73, 81,
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
    11, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 51, 52, 53,
    59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88, 43, 41, 82, 26, 27 };
/* what a US keyboard types on the key, plain and with Shift (0: nothing) */
static const char cui__vt_w_c0[] = "\0\0\0\0\0\0 \0\0\0\0\0\0\0\0" "abcdefghijklmnopqrstuvwxyz" "0123456789" "-=,./" "\0\0\0\0\0\0\0\0\0\0\0\0" "\\`" "\0" "[]";
static const char cui__vt_w_c1[] = "\0\0\0\0\0\0 \0\0\0\0\0\0\0\0" "ABCDEFGHIJKLMNOPQRSTUVWXYZ" ")!@#$%^&*(" "_+<>?" "\0\0\0\0\0\0\0\0\0\0\0\0" "|~" "\0" "{}";
typedef char cui__vt_w_tables_follow_cui_key[sizeof cui__vt_w_c0 == CUI_KEY_COUNT + 1 && sizeof cui__vt_w_c1 == CUI_KEY_COUNT + 1 ? 1 : -1];

/* Keys are a few a second: small code beats fast code here, and the record writer is one copy (it is called from a dozen
 * places with constant arguments, which GCC would otherwise clone for each). */
#if defined(__GNUC__) && !defined(__clang__)
#  define CUI__VT_SMALL __attribute__((optimize("Os")))
#  define CUI__VT_ONCE __attribute__((optimize("Os"), noinline, noclone))
#elif defined(__GNUC__)
#  define CUI__VT_SMALL
#  define CUI__VT_ONCE __attribute__((noinline))
#else
#  define CUI__VT_SMALL
#  define CUI__VT_ONCE
#endif
CUI__VT_ONCE static void cui__vt_w_rec(cui__vt_state *st, int vk, int sc, int uc, bool down, int cs) {
    char b[48];
    int n = snprintf(b, sizeof b, "\x1b[%d;%d;%d;%d;%d;1_", vk, sc, uc, down ? 1 : 0, cs);
    if (n > 0) cui__vt_out(st, b, n);
}

/* the control key state a key adds: the arrows and the navigation block are the enhanced keys */
static int cui__vt_w_enh(int k) { return k == CUI_KEY_DELETE || k == CUI_KEY_INSERT || (k >= CUI_KEY_LEFT && k <= CUI_KEY_PAGE_DOWN) ? CUI__VT_CS_ENHANCED : 0; }

/* the character of a press that came with no text of its own: the control code under Ctrl, the letter under Alt,
 * the control keys' own codes; a plain printable key with no text is a dead key and has none */
CUI__VT_SMALL static int cui__vt_w_uc(int k, bool shift, bool ctrl, bool alt) {
    bool plain_ctrl = ctrl && !alt;
    switch (k) {
    case CUI_KEY_ENTER: return plain_ctrl ? 0x0A : 0x0D;
    case CUI_KEY_ESCAPE: return 0x1B;
    case CUI_KEY_BACKSPACE: return plain_ctrl ? 0x7F : 0x08;
    case CUI_KEY_TAB: return plain_ctrl ? 0 : 0x09;
    default: break;
    }
    int ch = shift ? cui__vt_w_c1[k] : cui__vt_w_c0[k];
    if (!ch || ctrl != plain_ctrl) return 0;       /* no character, or AltGr: its characters come as text */
    if (plain_ctrl) return k >= CUI_KEY_A && k <= CUI_KEY_Z ? 1 + (k - CUI_KEY_A) : k == CUI_KEY_BACKSLASH ? 0x1C : k == CUI_KEY_MINUS ? 0x1F : k == CUI_KEY_SPACE ? 0x20 : 0;     /* Ctrl+Space keeps its space, as a Windows keyboard gives it */
    return alt ? ch : 0;
}

/* the keys of this frame as records: modifier changes, then text with the key that made it, then the rest of the presses,
 * then a release for every key that went up. Text and keys arrive separately (a key event and a text event), so each
 * character takes the pressed key that types it (or the one left over) and goes out once, in that record. */
CUI__VT_SMALL static void cui__vt_keys32(cui_ctx *ui, const cui_vt *v, cui__vt_state *st) {
    int mods = ui->mods;
    bool ctrl = (mods & CUI_MOD_CTRL) != 0, alt = (mods & CUI_MOD_ALT) != 0, shift = (mods & CUI_MOD_SHIFT) != 0;
    int want = (shift ? CUI__VT_CS_SHIFT : 0) | (ctrl ? CUI__VT_CS_LCTRL : 0) | (alt ? CUI__VT_CS_LALT : 0);
    static const uint8_t mk[3][3] = { { CUI__VT_CS_SHIFT, 0x10, 0x2A }, { CUI__VT_CS_LCTRL, 0x11, 0x1D }, { CUI__VT_CS_LALT, 0x12, 0x38 } };
    int cs = st->w_cs;
    for (int i = 0; i < 3; i++) {
        if (!((want ^ cs) & mk[i][0])) continue;
        cs = (cs & ~(int)mk[i][0]) | (want & mk[i][0]);
        cui__vt_w_rec(st, mk[i][1], mk[i][2], 0, (want & mk[i][0]) != 0, cs);
    }
    st->w_cs = (uint8_t)want;
    cs = want;

    /* the view keeps Ctrl+C, Ctrl+V and Shift+Page Up for itself exactly where cui__vt_keys does (its own copy of a selection
     * was taken out of kpressed already) */
    uint8_t rp[CUI_KEY_COUNT];
    bool sent[CUI_KEY_COUNT];
    bool sup = (mods & CUI_MOD_SUPER) != 0;
    memcpy(rp, ui->kpressed, sizeof rp);
    memset(sent, 0, sizeof sent);
    rp[0] = 0;
    if (shift && !ctrl && v->cur == 0) rp[CUI_KEY_PAGE_UP] = rp[CUI_KEY_PAGE_DOWN] = rp[CUI_KEY_HOME] = rp[CUI_KEY_END] = 0;
    if ((ctrl && (st->has_sel || shift)) || sup) rp[CUI_KEY_C] = 0;
    if ((ctrl && !alt) || sup) rp[CUI_KEY_V] = 0;

    /* text: what did not fit the last frame, then this frame's (nothing under a plain Ctrl), decoded; each character is given the key that types it */
    char txt[512]; int tl = st->w_pend_n;
    memcpy(txt, st->w_pend, (size_t)tl);
    if (ui->text_in_len > 0 && !(ctrl && !alt)) { memcpy(txt + tl, ui->text_in, (size_t)ui->text_in_len); tl += ui->text_in_len; }
    ui->text_in_len = 0; ui->text_in[0] = 0;
    st->w_pend_n = 0;
    uint32_t cps[512]; uint8_t nb[512]; int key_of[512]; int nc = 0;
    for (int i = 0; i < tl && nc < 512; nc++) {
        int n = cui__utf8_decode(txt + i, tl - i, &cps[nc]);
        nb[nc] = (uint8_t)n; key_of[nc] = -1; i += n;
    }
    for (int pass = 0; pass < 2; pass++) {    /* first the key that types the character; then, for another layout, any pressed key left over */
        for (int i = 0; i < nc; i++) {
            if (key_of[i] >= 0 || cps[i] > 0xFFFF || (pass && alt && !ctrl)) continue;
            for (int k = 1; k < CUI_KEY_COUNT; k++) {
                if (!rp[k] || !cui__vt_w_c0[k] || (!pass && cps[i] != (uint32_t)cui__vt_w_c0[k] && cps[i] != (uint32_t)cui__vt_w_c1[k])) continue;
                key_of[i] = k; rp[k]--;
                break;
            }
        }
    }
    int done = 0;
    for (int i = 0; i < nc; i++, done += nb[i - 1]) {
        if (st->out_n + 12 * 48 > (int)sizeof st->out) { cui_request_redraw(ui); break; }     /* the rest goes next frame */
        int ccs = ctrl && alt ? (cs & ~(int)CUI__VT_CS_LALT) | CUI__VT_CS_RALT : cs;          /* AltGr: Left Ctrl and Right Alt */
        int k = key_of[i];
        uint32_t c = cps[i];
        if (k >= 0) {
            cui__vt_w_rec(st, cui__vt_w_vk[k], cui__vt_w_sc[k], (int)c, true, ccs | cui__vt_w_enh(k));
            st->w_uc[k] = (uint16_t)c; sent[k] = true;
            continue;
        }
        int units = 1, u0 = (int)c, u1 = 0;      /* no key (an IME, a dead key, a composed character): Vk 0, down and up; beyond the BMP two units */
        if (c > 0xFFFF) { units = 2; u0 = (int)(0xD800 + ((c - 0x10000) >> 10)); u1 = (int)(0xDC00 + ((c - 0x10000) & 0x3FF)); }
        for (int u = 0; u < units; u++) for (int d = 1; d >= 0; d--) cui__vt_w_rec(st, 0, 0, u ? u1 : u0, d != 0, ccs);
    }
    if (done < tl) {                  /* what did not fit waits for the next frame (a text event is 255 bytes at most, so this is rare) */
        int left = CUI_MIN(tl - done, (int)sizeof st->w_pend);
        memcpy(st->w_pend, txt + done, (size_t)left);
        st->w_pend_n = left;
    }

    /* the presses nothing typed: control and navigation keys, Ctrl and Alt combinations, a dead key */
    for (int k = 1; k < CUI_KEY_COUNT; k++) {
        if (!cui__vt_w_vk[k]) continue;
        int uc = cui__vt_w_uc(k, shift, ctrl, alt);
        for (int r = 0; r < rp[k] && st->out_n + 2 * 48 <= (int)sizeof st->out; r++) {
            cui__vt_w_rec(st, cui__vt_w_vk[k], cui__vt_w_sc[k], uc, true, cs | cui__vt_w_enh(k));
            st->w_uc[k] = (uint16_t)uc; sent[k] = true;
        }
    }
    /* releases: a key that went up this frame, or since the last */
    for (int k = 1; k < CUI_KEY_COUNT; k++) {
        if (!sent[k] && !st->w_held[k]) continue;
        if (ui->kdown[k]) { st->w_held[k] = 1; continue; }
        st->w_held[k] = 0;
        if (st->out_n + 48 > (int)sizeof st->out) continue;
        int kcs = cs | cui__vt_w_enh(k);
        if (ctrl && alt && st->w_uc[k] >= 0x20) kcs = (kcs & ~(int)CUI__VT_CS_LALT) | CUI__VT_CS_RALT;      /* AltGr's character */
        cui__vt_w_rec(st, cui__vt_w_vk[k], cui__vt_w_sc[k], st->w_uc[k], false, kcs);
    }
}

/* ------------------------------------------------------------------------ */
/* View: selection and copy                                                 */
/* ------------------------------------------------------------------------ */

/* abs row: sb_total + y for the live screen; older rows are smaller */
static const cui__vt_cell *cui__vt_cells_abs(const cui_vt *v, int64_t abs, int *n, bool *wrapped) {
    return cui__vt_cells_at(v, abs - (int64_t)v->sb_total, n, wrapped);
}

static void cui__vt_sel_order(const cui__vt_state *st, cui__vt_pos *s0, cui__vt_pos *s1) {
    *s0 = st->a; *s1 = st->b;
    if (s1->row < s0->row || (s1->row == s0->row && s1->col < s0->col)) { cui__vt_pos t = *s0; *s0 = *s1; *s1 = t; }
}

static char *cui__vt_selection_text(cui_ctx *ui, const cui_vt *v, const cui__vt_state *st) {
    cui__vt_pos s0, s1;
    cui__vt_sel_order(st, &s0, &s1);
    size_t cap = (size_t)(s1.row - s0.row + 1) * ((size_t)v->cols * 4 + 2) + 1;
    if (cap > (64u << 20)) return NULL;
    char *out = (char *)cui__alloc(ui, cap);
    if (!out) return NULL;
    size_t len = 0;
    for (int64_t r = s0.row; r <= s1.row; r++) {
        int n; bool wrapped = false;
        const cui__vt_cell *c = cui__vt_cells_abs(v, r, &n, &wrapped);
        int c0 = r == s0.row ? s0.col : 0, c1 = r == s1.row ? s1.col + 1 : v->cols;
        if (c) len += cui__vt_text(c, n, c0, c1, !(wrapped && r != s1.row), out + len, cap - len);
        if (r != s1.row && !wrapped) out[len++] = '\n';
    }
    out[len] = 0;
    return out;
}

static cui__vt_pos cui__vt_pos_at(const cui_vt *v, const cui__vt_state *st, cui_rect pr, float padx, float pady, float cw, float lh, float mx, float my) {
    int row = (int)floorf((my - pr.y - pady) / lh);
    int col = (int)floorf((mx - pr.x - padx) / cw);
    row = CUI_CLAMP(row, 0, v->rows - 1);
    col = CUI_CLAMP(col, 0, v->cols - 1);
    return (cui__vt_pos){ (int64_t)v->sb_total - st->off + row, col };
}

static bool cui__vt_is_word(uint32_t c) { return c > 32 && c != 0x7F && (c >= 128 || !strchr("()[]{}<>\"'`,;|", (int)c)); }

/* the word under a cell: letters, digits and the usual path and option characters */
static void cui__vt_word_at(const cui_vt *v, cui__vt_pos p, cui__vt_pos *a, cui__vt_pos *b) {
    int n; bool w;
    const cui__vt_cell *c = cui__vt_cells_abs(v, p.row, &n, &w);
    *a = *b = p;
    if (!c) return;
    #define CUI__VT_WORDCH(x) ((x) < n && cui__vt_is_word(CUI__VT_CP(c[x].ca)))
    if (!CUI__VT_WORDCH(p.col)) return;
    int x0 = p.col, x1 = p.col;
    while (x0 > 0 && CUI__VT_WORDCH(x0 - 1)) x0--;
    while (x1 + 1 < v->cols && CUI__VT_WORDCH(x1 + 1)) x1++;
    #undef CUI__VT_WORDCH
    a->col = x0; b->col = x1;
}

/* a rectangle on whole device pixels, so neighbouring cells meet without seams */
static void cui__vt_snap_rect(cui_ctx *ui, float x0, float y0, float x1, float y1, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    x0 = floorf(x0 * s + 0.5f) / s; x1 = floorf(x1 * s + 0.5f) / s;
    y0 = floorf(y0 * s + 0.5f) / s; y1 = floorf(y1 * s + 0.5f) / s;
    if (x1 > x0 && y1 > y0) cui_draw_rect(ui, (cui_rect){ x0, y0, x1 - x0, y1 - y0 }, CUI_CORNERS(0), col);
}

/* ------------------------------------------------------------------------ */
/* Box drawing as it is without `.glyphs` (the complete set is in cui_vt_glyphs.c) */
/* ------------------------------------------------------------------------ */

/* U+2500..U+257F as four arms (left, right, up, down), each 0 none, 1 light, 2 heavy, 3 double, so lines meet
 * exactly across cells whatever the font has. Dashed lines are drawn solid; the diagonals are lines. */
#define CUI__VT_ARMS0(l, r, u, d) ((uint8_t)((l) | ((r) << 2) | ((u) << 4) | ((d) << 6)))
static uint8_t cui__vt_box0_arms(uint32_t c) {
    static const uint8_t t[] = {
        CUI__VT_ARMS0(1,1,0,0), CUI__VT_ARMS0(2,2,0,0), CUI__VT_ARMS0(0,0,1,1), CUI__VT_ARMS0(0,0,2,2), CUI__VT_ARMS0(1,1,0,0), CUI__VT_ARMS0(2,2,0,0), CUI__VT_ARMS0(0,0,1,1), CUI__VT_ARMS0(0,0,2,2),
        CUI__VT_ARMS0(1,1,0,0), CUI__VT_ARMS0(2,2,0,0), CUI__VT_ARMS0(0,0,1,1), CUI__VT_ARMS0(0,0,2,2),
        CUI__VT_ARMS0(0,1,0,1), CUI__VT_ARMS0(0,2,0,1), CUI__VT_ARMS0(0,1,0,2), CUI__VT_ARMS0(0,2,0,2), CUI__VT_ARMS0(1,0,0,1), CUI__VT_ARMS0(2,0,0,1), CUI__VT_ARMS0(1,0,0,2), CUI__VT_ARMS0(2,0,0,2),
        CUI__VT_ARMS0(0,1,1,0), CUI__VT_ARMS0(0,2,1,0), CUI__VT_ARMS0(0,1,2,0), CUI__VT_ARMS0(0,2,2,0), CUI__VT_ARMS0(1,0,1,0), CUI__VT_ARMS0(2,0,1,0), CUI__VT_ARMS0(1,0,2,0), CUI__VT_ARMS0(2,0,2,0),
        CUI__VT_ARMS0(0,1,1,1), CUI__VT_ARMS0(0,2,1,1), CUI__VT_ARMS0(0,1,2,1), CUI__VT_ARMS0(0,1,1,2), CUI__VT_ARMS0(0,1,2,2), CUI__VT_ARMS0(0,2,2,1), CUI__VT_ARMS0(0,2,1,2), CUI__VT_ARMS0(0,2,2,2),
        CUI__VT_ARMS0(1,0,1,1), CUI__VT_ARMS0(2,0,1,1), CUI__VT_ARMS0(1,0,2,1), CUI__VT_ARMS0(1,0,1,2), CUI__VT_ARMS0(1,0,2,2), CUI__VT_ARMS0(2,0,2,1), CUI__VT_ARMS0(2,0,1,2), CUI__VT_ARMS0(2,0,2,2),
        CUI__VT_ARMS0(1,1,0,1), CUI__VT_ARMS0(2,1,0,1), CUI__VT_ARMS0(1,2,0,1), CUI__VT_ARMS0(2,2,0,1), CUI__VT_ARMS0(1,1,0,2), CUI__VT_ARMS0(2,1,0,2), CUI__VT_ARMS0(1,2,0,2), CUI__VT_ARMS0(2,2,0,2),
        CUI__VT_ARMS0(1,1,1,0), CUI__VT_ARMS0(2,1,1,0), CUI__VT_ARMS0(1,2,1,0), CUI__VT_ARMS0(2,2,1,0), CUI__VT_ARMS0(1,1,2,0), CUI__VT_ARMS0(2,1,2,0), CUI__VT_ARMS0(1,2,2,0), CUI__VT_ARMS0(2,2,2,0),
        CUI__VT_ARMS0(1,1,1,1), CUI__VT_ARMS0(2,1,1,1), CUI__VT_ARMS0(1,2,1,1), CUI__VT_ARMS0(2,2,1,1), CUI__VT_ARMS0(1,1,2,1), CUI__VT_ARMS0(1,1,1,2), CUI__VT_ARMS0(1,1,2,2), CUI__VT_ARMS0(2,1,2,1),
        CUI__VT_ARMS0(1,2,2,1), CUI__VT_ARMS0(2,1,1,2), CUI__VT_ARMS0(1,2,1,2), CUI__VT_ARMS0(2,2,2,1), CUI__VT_ARMS0(2,2,1,2), CUI__VT_ARMS0(2,1,2,2), CUI__VT_ARMS0(1,2,2,2), CUI__VT_ARMS0(2,2,2,2),
        CUI__VT_ARMS0(1,1,0,0), CUI__VT_ARMS0(2,2,0,0), CUI__VT_ARMS0(0,0,1,1), CUI__VT_ARMS0(0,0,2,2),
        CUI__VT_ARMS0(3,3,0,0), CUI__VT_ARMS0(0,0,3,3), CUI__VT_ARMS0(0,3,0,1), CUI__VT_ARMS0(0,1,0,3), CUI__VT_ARMS0(0,3,0,3), CUI__VT_ARMS0(3,0,0,1), CUI__VT_ARMS0(1,0,0,3), CUI__VT_ARMS0(3,0,0,3),
        CUI__VT_ARMS0(0,3,1,0), CUI__VT_ARMS0(0,1,3,0), CUI__VT_ARMS0(0,3,3,0), CUI__VT_ARMS0(3,0,1,0), CUI__VT_ARMS0(1,0,3,0), CUI__VT_ARMS0(3,0,3,0),
        CUI__VT_ARMS0(0,3,1,1), CUI__VT_ARMS0(0,1,3,3), CUI__VT_ARMS0(0,3,3,3), CUI__VT_ARMS0(3,0,1,1), CUI__VT_ARMS0(1,0,3,3), CUI__VT_ARMS0(3,0,3,3),
        CUI__VT_ARMS0(3,3,0,1), CUI__VT_ARMS0(1,1,0,3), CUI__VT_ARMS0(3,3,0,3), CUI__VT_ARMS0(3,3,1,0), CUI__VT_ARMS0(1,1,3,0), CUI__VT_ARMS0(3,3,3,0),
        CUI__VT_ARMS0(3,3,1,1), CUI__VT_ARMS0(1,1,3,3), CUI__VT_ARMS0(3,3,3,3),
        CUI__VT_ARMS0(0,1,0,1), CUI__VT_ARMS0(1,0,0,1), CUI__VT_ARMS0(1,0,1,0), CUI__VT_ARMS0(0,1,1,0),
    };
    if (c >= 0x2574 && c <= 0x257F) {
        static const uint8_t h[] = { CUI__VT_ARMS0(1,0,0,0), CUI__VT_ARMS0(0,0,1,0), CUI__VT_ARMS0(0,1,0,0), CUI__VT_ARMS0(0,0,0,1), CUI__VT_ARMS0(2,0,0,0), CUI__VT_ARMS0(0,0,2,0),
                                     CUI__VT_ARMS0(0,2,0,0), CUI__VT_ARMS0(0,0,0,2), CUI__VT_ARMS0(1,2,0,0), CUI__VT_ARMS0(0,0,1,2), CUI__VT_ARMS0(2,1,0,0), CUI__VT_ARMS0(0,0,2,1) };
        return h[c - 0x2574];
    }
    return c >= 0x2500 && c < 0x2500 + sizeof t ? t[c - 0x2500] : 0;
}

/* One arm of a box character. `i`: 0 left, 1 right, 2 up, 3 down. A double arm is two lines; where the crossing
 * line is double too, each line reaches the crossing line on its side (a corner closes) or stops at its inner
 * line (a gap for the arm on the other side to pass through). */
static void cui__vt_box0_arm(cui_ctx *ui, int i, const int *arm, cui_rect c, float light, float heavy, cui_color col) {
    float cx = c.x + c.w * 0.5f, cy = c.y + c.h * 0.5f, g = light * 1.5f, half = light * 0.5f;
    bool is_h = i < 2;
    int a = is_h ? 2 : 0, b = a + 1;                     /* the crossing arms: up and down for a horizontal arm, left and right otherwise */
    bool cross_any = arm[a] || arm[b], cross3 = arm[a] == 3 || arm[b] == 3;
    float t = arm[i] == 2 ? heavy : light;
    float cross_w = 0;
    if (cross_any) cross_w = (arm[a] == 2 || arm[b] == 2 ? heavy : light) * 0.5f;
    int lines = arm[i] == 3 ? 2 : 1;
    for (int k = 0; k < lines; k++) {
        float o = lines == 2 ? (k ? g : -g) : 0;           /* offset across the arm */
        float mid;                                          /* where the line ends at the middle, along the arm */
        float mc = is_h ? cx : cy;
        float dir = (i == 0 || i == 2) ? 1.0f : -1.0f;     /* from the edge toward the middle */
        if (!cross_any) mid = mc;
        else if (lines == 2 && cross3) {
            bool side_has = is_h ? (k == 0 ? arm[2] != 0 : arm[3] != 0) : (k == 0 ? arm[0] != 0 : arm[1] != 0);
            mid = side_has ? mc - dir * (g + half) : mc + dir * (g + half);
            if (!side_has) {
                bool opp_has = is_h ? (k == 0 ? arm[3] != 0 : arm[2] != 0) : (k == 0 ? arm[1] != 0 : arm[0] != 0);
                if (!opp_has) mid = mc;
            }
        } else mid = mc + dir * cross_w;
        if (is_h) {
            float yc = cy + o;
            if (i == 0) cui__vt_snap_rect(ui, c.x, yc - t * 0.5f, mid, yc + t * 0.5f, col);
            else cui__vt_snap_rect(ui, mid, yc - t * 0.5f, c.x + c.w, yc + t * 0.5f, col);
        } else {
            float xc = cx + o;
            if (i == 2) cui__vt_snap_rect(ui, xc - t * 0.5f, c.y, xc + t * 0.5f, mid, col);
            else cui__vt_snap_rect(ui, xc - t * 0.5f, mid, xc + t * 0.5f, c.y + c.h, col);
        }
    }
}

static bool cui__vt_box0(cui_ctx *ui, uint32_t cp, cui_rect c, cui_color col) {
    if (cp < 0x2500 || cp > 0x257F) return false;
    if (cp >= 0x2571 && cp <= 0x2573) {     /* diagonals */
        if (cp != 0x2572) cui_draw_line(ui, (cui_vec2){ c.x, c.y + c.h }, (cui_vec2){ c.x + c.w, c.y }, 1, col);
        if (cp != 0x2571) cui_draw_line(ui, (cui_vec2){ c.x, c.y }, (cui_vec2){ c.x + c.w, c.y + c.h }, 1, col);
        return true;
    }
    uint8_t a = cui__vt_box0_arms(cp);
    if (!a) return false;
    int arm[4] = { a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3 };     /* left, right, up, down */
    float s = ui->scale > 0 ? ui->scale : 1;
    float light = fmaxf(1.0f, floorf(c.h / 16.0f * s + 0.5f) / s), heavy = light * 2.0f + (c.h > 24 ? 1.0f : 0.0f);
    for (int i = 0; i < 4; i++) if (arm[i]) cui__vt_box0_arm(ui, i, arm, c, light, heavy, col);
    return true;
}

/* ------------------------------------------------------------------------ */
/* View: paint                                                              */
/* ------------------------------------------------------------------------ */

typedef struct {
    const cui_vt *v;
    float cw, lh, size, padx, pady;
    int off;
    bool focused, show_cursor, has_sel;
    int shape;                        /* CUI_VT_CURSOR_* */
    bool glyphs, italic;              /* the opt-in looks: hand-drawn characters, slanted italic */
    const cui__vt_cell *snap;         /* paint this copy of the screen (a synchronized update is open) instead of the live one */
    int scx, scy; bool scursor;
    const cui__vt_pic_draw *pics;     /* each visible row's picture (cui_vt_sixel.c), or NULL when none is on screen */
    cui__vt_pos s0, s1;
    cui_color fg, bg, sel, cursor, bar;
    float bar_t;
    cui_id wheel;
} cui__vt_paint;

static cui_color cui__vt_color(cui_ctx *ui, const cui_vt *v, uint16_t id, cui_color def) {
    if (id == 0) return def;
    if (id < CUI__VT_PALETTE) return cui__tm_256(ui, id - 1);
    uint32_t c = v->rgb[id - CUI__VT_PALETTE];
    return cui__rgba((int)((c >> 16) & 255), (int)((c >> 8) & 255), (int)(c & 255), 255);
}

/* a cell's colors after inverse and dim */
static void cui__vt_cell_colors(cui_ctx *ui, const cui__vt_paint *p, const cui__vt_cell *c, cui_color *fg, cui_color *bg, bool *has_bg) {
    cui_color f = cui__vt_color(ui, p->v, c->fg, p->fg), b = cui__vt_color(ui, p->v, c->bg, p->bg);
    bool hb = c->bg != 0;
    if (c->ca & CUI_VT_INVERSE) { cui_color t = f; f = c->bg ? b : p->bg; b = t; hb = true; }
    if (c->ca & CUI_VT_DIM) f = cui_mix(f, p->bg, 0.4f);
    *fg = f; *bg = b; *has_bg = hb;
}

static bool cui__vt_run_same(const cui__vt_cell *a, const cui__vt_cell *b) {
    return a->fg == b->fg && a->bg == b->bg && CUI__VT_ATTRS(a->ca) == CUI__VT_ATTRS(b->ca);
}

/* The underline (a style, or the plain line as it always was) and the overline of `n` cells from cell `x` of a row at `ry`, in
 * the text color. Every edge is put on a whole device pixel, so the lines are the same at any scale. */
static void cui__vt_decor(cui_ctx *ui, const cui__vt_paint *p, uint32_t at, float x0, int x, int n, float ry, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    float left = x0 + (float)x * p->cw, right = x0 + (float)(x + n) * p->cw;
    float t = fmaxf(1.0f, floorf(p->lh / 16.0f * s + 0.5f)) / s;       /* line thickness: the box light line's */
    float yb = ry + p->lh - 1.0f;                                       /* bottom edge of an underline */
    uint32_t style = (at & CUI_VT_UNDERLINE) ? CUI__VT_ULS(at) : 0;
    if (at & CUI__VT_OVER) cui__vt_snap_rect(ui, left, ry, right, ry + t, col);
    if (!(at & CUI_VT_UNDERLINE)) return;
    switch (style) {
    case 2:                                                             /* double: two lines with a gap of one */
        cui__vt_snap_rect(ui, left, yb - t, right, yb, col);
        cui__vt_snap_rect(ui, left, yb - 3 * t, right, yb - 2 * t, col);
        break;
    case 3: {                                                           /* curly: a wave one cell long, column by column on device pixels */
        float amp = fmaxf(2.0f, floorf(0.12f * p->lh * s + 0.5f));      /* device pixels */
        float per = fmaxf(4.0f, floorf(p->cw * s + 0.5f));
        float top = yb - t - amp / s;
        int a = (int)floorf(left * s + 0.5f), b = (int)floorf(right * s + 0.5f);
        int run0 = a, lev = -1;
        for (int d = a; d <= b; d++) {
            int l = -1;
            if (d < b) l = (int)floorf(amp * 0.5f * (1.0f - cosf(6.2831853f * fmodf((float)d, per) / per)) + 0.5f);
            if (l != lev) {
                if (lev >= 0) cui__vt_snap_rect(ui, (float)run0 / s, top + (float)lev / s, (float)d / s, top + (float)lev / s + t, col);
                run0 = d; lev = l;
            }
        }
        break;
    }
    case 4: {                                                           /* dotted: dots of the line's width, one dot of gap */
        float dot = t, step = 2 * t;
        for (float dx = ceilf(left / step) * step; dx < right; dx += step) cui__vt_snap_rect(ui, dx, yb - t, fminf(dx + dot, right), yb, col);
        break;
    }
    case 5:                                                             /* dashed: a dash of 0.6 cell in every cell */
        for (int k = 0; k < n; k++) {
            float cx = left + (float)k * p->cw;
            cui__vt_snap_rect(ui, cx, yb - t, cx + p->cw * 0.6f, yb, col);
        }
        break;
    default:                                                            /* single: as it was drawn before the styles */
        cui_draw_rect(ui, (cui_rect){ left, ry + p->lh - 2, right - left, 1 }, CUI_CORNERS(0), col);
        break;
    }
}

/* Slants the glyphs drawn since vertex v0 around the baseline `base`: a synthesized italic (the monospace font has none). */
static void cui__vt_slant(cui_ctx *ui, int v0, float base) {
    cui_xform x = cui__draw_xform(ui);
    float by = base * ui->scale * x.scale + x.oy;
    for (int v = v0; v < ui->dl.vtx_count; v++) ui->dl.vtx[v].x += 0.2f * (by - ui->dl.vtx[v].y);
}

static void cui__paint_vt(cui_ctx *ui, cui_node *n, void *data) {
    cui__vt_paint *p = (cui__vt_paint *)data;
    const cui_vt *v = p->v;
    cui_rect r = n->rect;
    cui__paint_wheel_hit(ui, p->wheel, r);
    float x0 = r.x + p->padx, y0 = r.y + p->pady, fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size), asc = cui__font_ascent(ui, CUI_FONT_MONO, p->size);
    cui_push_clip(ui, (cui_rect){ r.x, r.y + 1, r.w, r.h - 2 });
    char *buf = (char *)cui__frame_alloc(ui, (size_t)v->cols * 4 + 8);
    for (int y = 0; y < v->rows; y++) {
        float ry = y0 + (float)y * p->lh;
        if (ry > r.y + r.h) break;
        int cn; bool wrapped;
        int64_t arow = (int64_t)v->sb_total - p->off + y;
        const cui__vt_cell *cells = cui__vt_cells_abs(v, arow, &cn, &wrapped);
        if (p->snap && arow >= (int64_t)v->sb_total && arow - (int64_t)v->sb_total < v->rows) { cells = p->snap + (size_t)(arow - (int64_t)v->sb_total) * (size_t)v->cols; cn = v->cols; }
        /* selection under the text */
        if (p->has_sel && arow >= p->s0.row && arow <= p->s1.row) {
            int c0 = arow == p->s0.row ? p->s0.col : 0, c1 = arow == p->s1.row ? p->s1.col + 1 : v->cols;
            if (c1 > c0) cui__vt_snap_rect(ui, x0 + c0 * p->cw, ry, x0 + c1 * p->cw, ry + p->lh, p->sel);
        }
        if (!cells) continue;
        /* backgrounds, one rectangle per run of the same color */
        for (int x = 0; x < cn;) {
            cui_color fg, bg; bool hb;
            cui__vt_cell_colors(ui, p, &cells[x], &fg, &bg, &hb);
            int e = x + 1;
            if (hb) {
                while (e < cn && cui__vt_run_same(&cells[e], &cells[x])) e++;
                cui__vt_snap_rect(ui, x0 + x * p->cw, ry, x0 + e * p->cw, ry + p->lh, bg);      /* whole device pixels: no seams between rows */
            } else while (e < cn && cells[e].bg == 0 && !(cells[e].ca & CUI_VT_INVERSE)) e++;
            x = e;
        }
        /* text: a run of the same style is one draw call; characters outside ASCII are placed in their own cell */
        for (int x = 0; x < cn;) {
            uint32_t ch = CUI__VT_CP(cells[x].ca);
            if ((cells[x].ca & CUI__VT_SPACER) || ch == 0 || ch == ' ') { x++; continue; }
            cui_color fg, bg; bool hb;
            cui__vt_cell_colors(ui, p, &cells[x], &fg, &bg, &hb);
            uint32_t at = cells[x].ca;
            float ty = ry + (p->lh - fh) * 0.5f;
            bool slant = p->italic && (at & CUI_VT_ITALIC);
            int v0 = ui->dl.vtx_count;
            if (ch >= 128) {
                cui_rect cell = { x0 + x * p->cw, ry, p->cw, p->lh };
                if (p->glyphs) {                            /* the complete hand-drawn set; lines above and below go with it, as with text */
                    if (cui__vt_glyph(ui, ch, cell, fg)) {
                        if (at & (CUI_VT_UNDERLINE | CUI__VT_OVER)) cui__vt_decor(ui, p, at, x0, x, 1, ry, fg);
                        x++; continue;
                    }
                } else {                                    /* as it always was: box lines and the first block elements */
                    if (cui__vt_box0(ui, ch, cell, fg)) { x++; continue; }
                    if (ch >= 0x2580 && ch <= 0x2593) { cui__tm_block(ui, (int)(ch - 0x2580), cell, fg); x++; continue; }
                }
                char u[4]; size_t k = cui__vt_utf8(ch, u);
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw, ty, fg, u, (int)k, true);
                if (at & CUI_VT_BOLD) cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw + 0.45f, ty, fg, u, (int)k, true);
                if (slant) cui__vt_slant(ui, v0, ty + asc);
                if (at & (CUI_VT_UNDERLINE | CUI__VT_OVER)) cui__vt_decor(ui, p, at, x0, x, x + 1 < cn && (cells[x + 1].ca & CUI__VT_SPACER) ? 2 : 1, ry, fg);
                x++;
                continue;
            }
            int e = x, len = 0, last = x;
            while (e < cn && cui__vt_run_same(&cells[e], &cells[x])) {
                uint32_t c2 = CUI__VT_CP(cells[e].ca);
                if (cells[e].ca & CUI__VT_SPACER) break;
                if (c2 >= 128 || (c2 >= 0x2580 && c2 <= 0x2593)) break;
                buf[len++] = c2 ? (char)c2 : ' ';
                if (c2 && c2 != ' ') last = e;
                e++;
            }
            len = last - x + 1;
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw, ty, fg, buf, len, true);
            if (at & CUI_VT_BOLD) cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw + 0.45f, ty, fg, buf, len, true);
            if (slant) cui__vt_slant(ui, v0, ty + asc);
            if (at & (CUI_VT_UNDERLINE | CUI__VT_OVER)) cui__vt_decor(ui, p, at, x0, x, len, ry, fg);
            if (at & CUI_VT_STRIKE) cui_draw_rect(ui, (cui_rect){ x0 + x * p->cw, ry + p->lh * 0.5f, len * p->cw, 1 }, CUI_CORNERS(0), fg);
            x = e;
        }
        /* a picture: over the row's text, as Windows Terminal paints it (text written into a cell removes that cell's piece,
         * so only text that was there before the picture is covered); the selection and the cursor stay visible on it */
        if (p->pics && p->pics[y].tex) {
            const cui__vt_pic_draw *pd = &p->pics[y];
            float s = ui->scale > 0 ? ui->scale : 1, px0 = floorf((x0 + pd->c0 * p->cw) * s + 0.5f) / s, px1 = floorf((x0 + pd->c1 * p->cw) * s + 0.5f) / s;
            float py0 = floorf(ry * s + 0.5f) / s, py1 = floorf((ry + p->lh) * s + 0.5f) / s;
            cui_draw_image(ui, pd->tex, (cui_rect){ px0, py0, px1 - px0, py1 - py0 }, (cui_rect){ 0, 0, 1, 1 }, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
            if (p->has_sel && arow >= p->s0.row && arow <= p->s1.row) {
                int c0 = CUI_MAX(arow == p->s0.row ? p->s0.col : 0, pd->c0), c1 = CUI_MIN(arow == p->s1.row ? p->s1.col + 1 : v->cols, pd->c1);
                if (c1 > c0) cui__vt_snap_rect(ui, x0 + c0 * p->cw, ry, x0 + c1 * p->cw, ry + p->lh, p->sel);
            }
        }
    }
    /* the cursor (where it was when a synchronized update opened, while one is shown) */
    int ccx = p->snap ? p->scx : v->cx, ccy = p->snap ? p->scy : v->cy;
    bool cvis = p->snap ? p->scursor : v->cursor_vis;
    if (p->show_cursor && cvis && p->off == 0) {
        float cx = x0 + (float)ccx * p->cw, cy = y0 + (float)ccy * p->lh;
        if (p->shape != CUI_VT_CURSOR_BLOCK) {              /* an underline or a bar, on whole device pixels; the character under it stays as it is */
            float s = ui->scale > 0 ? ui->scale : 1;
            cui_color col = p->focused ? p->cursor : cui_alpha(p->cursor, 0.55f);
            if (p->shape == CUI_VT_CURSOR_UNDERLINE) {
                float t = fmaxf(1.0f, floorf(p->lh * 0.1f * s + 0.5f)) / s;
                cui__vt_snap_rect(ui, cx, cy + p->lh - t, cx + p->cw, cy + p->lh, col);
            } else {
                float t = fmaxf(1.0f, floorf(p->cw * 0.2f * s + 0.5f)) / s;
                cui__vt_snap_rect(ui, cx, cy, cx + t, cy + p->lh, col);
            }
        } else if (p->focused) {
            cui__vt_snap_rect(ui, cx, cy, cx + p->cw, cy + p->lh, p->cursor);
            int cn; bool w;
            const cui__vt_cell *cells = p->snap ? p->snap + (size_t)ccy * (size_t)v->cols : cui__vt_cells_at(v, ccy, &cn, &w);
            if (p->snap) cn = v->cols;
            uint32_t ch = cells && ccx < cn ? CUI__VT_CP(cells[ccx].ca) : 0;
            if (ch > ' ' && ch != 0x7F && !(p->glyphs && ch >= 128 && cui__vt_glyph(ui, ch, (cui_rect){ cx, cy, p->cw, p->lh }, p->bg))) {
                char u[4]; size_t k = cui__vt_utf8(ch, u);
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, cx, cy + (p->lh - fh) * 0.5f, p->bg, u, (int)k, true);
            }
        } else {
            cui_draw_rect_border(ui, (cui_rect){ cx + 0.5f, cy + 0.5f, p->cw - 1, p->lh - 1 }, CUI_CORNERS(0), cui__rgba(0, 0, 0, 0), p->cursor, 1);
        }
    }
    cui_pop_clip(ui);
    /* a thin scrollbar while there is history */
    if (p->bar_t > 0.01f && v->sb_count > 0 && v->cur == 0) {
        float total = (float)(v->sb_count + v->rows), th = CUI_MAX(24.0f, r.h * (float)v->rows / total);
        float frac = v->sb_count > 0 ? 1.0f - (float)p->off / (float)v->sb_count : 1.0f;
        cui_draw_rect(ui, (cui_rect){ r.x + r.w - 6, r.y + frac * (r.h - th), 3, th }, CUI_CORNERS(1.5f), cui_alpha(p->bar, p->bar_t));
    }
}

/* ------------------------------------------------------------------------ */
/* View: component                                                          */
/* ------------------------------------------------------------------------ */

static cui_vt_result cui__vt_view_body(cui_ctx *ui, const char *id_str, cui_vt *v, const cui_vt_opts *o) {
    cui_vt_result res;
    memset(&res, 0, sizeof res);
    cui_id id = cui_make_id(ui, id_str ? id_str : "vt");
    cui_id wid = id ^ 0x7E3Eu;
    cui__vt_state *st = (cui__vt_state *)cui_state(ui, id, sizeof *st);
    float size = cui__fs(ui, o->size > 0 ? o->size : 13);
    float cw = cui__text_width(ui, CUI_FONT_MONO, size, "0", 1, true);
    if (cw <= 0) cw = size * 0.6f;
    float lh = ceilf(size * (o->line_height > 0 ? CUI_CLAMP(o->line_height, 1.0f, 3.0f) : 1.4f)), padx = 10, pady = 6;
    cui_rect pr = cui__prev_rect(ui, id);
    if (!v) return res;
    v->cell_w = (int)lroundf(cw * (ui->scale > 0 ? ui->scale : 1.0f)); v->cell_h = (int)lroundf(lh * (ui->scale > 0 ? ui->scale : 1.0f));
    v->cell_w16 = (uint16_t)CUI_CLAMP(lroundf(cw * (ui->scale > 0 ? ui->scale : 1.0f) * 16.0f), 0, 65535);
    if (o->focus) ui->focus = id;
    cui_interact in = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    bool focused = ui->focus == id;
    res.focused = focused;
    /* focus reports (opt in): when the view gains or loses the keyboard, or the window the focus, a program that asked gets ESC [ I or O */
    bool has_keys = focused && ui->window_focused;
    if (st->finit && has_keys != st->fin && o->focus_reports && !o->no_input && v->focus_mode) cui__vt_outs(st, has_keys ? "\x1b[I" : "\x1b[O");
    st->fin = has_keys; st->finit = true;
    /* the grid follows the view's size */
    if (pr.w > 0 && pr.h > 0) {
        int cols = (int)((pr.w - 2 * padx) / cw), rows = (int)((pr.h - 2 * pady) / lh);
        cols = CUI_MAX(cols, 2); rows = CUI_MAX(rows, 1);
        if (cols != v->cols || rows != v->rows) { cui_vt_resize(v, cols, rows); res.resized = true; }
    }
    res.cols = v->cols; res.rows = v->rows;
    /* new lines scrolled off while looking back: stay on the same text */
    if (st->init && v->sb_total > st->seen_total && st->off > 0) st->off += (int)(v->sb_total - st->seen_total);
    st->seen_total = v->sb_total; st->init = true;
    if (st->off > v->sb_count) st->off = v->sb_count;
    if (v->cur == 1) st->off = 0;
    /* the program has the mouse (opt in): its clicks, drags and wheel are reports, Shift or scrolled-back history keeps them local */
    bool mshift = (ui->mods & CUI_MOD_SHIFT) != 0;
    bool can_grab = o->mouse && !o->no_input && v->mouse_modes != 0 && st->off == 0;
    bool grab = can_grab && (!mshift || st->btn_down != 0);
    cui__vt_geom geom = { pr, padx, pady, cw, lh };
    /* wheel */
    if (ui->scroll_target == wid && ui->scroll_dy != 0 && grab) {
        float d = ui->scroll_dy;
        int k = CUI_CLAMP((int)lroundf(fabsf(d)), 1, 8), col, row;      /* one report per notch */
        cui__vt_grid_at(v, &geom, ui->mx, ui->my, &col, &row);
        for (int i = 0; i < k; i++) cui__vt_mouse_out(st, v, (d > 0 ? 64 : 65) + cui__vt_mouse_mods(ui->mods), col, row, false);
        ui->scroll_dy = 0;
    } else if (can_grab && mshift && in.hovered && v->cur == 0 && ui->scroll_dy != 0) {      /* Shift+wheel looks back in the history */
        st->off = CUI_CLAMP(st->off + (int)lroundf(ui->scroll_dy * 3), 0, v->sb_count);
        ui->scroll_dy = 0;
    } else if (ui->scroll_target == wid && ui->scroll_dy != 0) {
        float d = ui->scroll_dy;
        if (v->cur == 0) st->off = CUI_CLAMP(st->off + (int)lroundf(d * 3), 0, v->sb_count);
        else {   /* a full screen program has no scrollback: the wheel moves its cursor */
            int k = (int)lroundf(fabsf(d) * 3);
            for (int i = 0; i < k; i++) cui__vt_outs(st, d > 0 ? (v->app_cursor ? "\x1bOA" : "\x1b[A") : (v->app_cursor ? "\x1bOB" : "\x1b[B"));
        }
        ui->scroll_dy = 0;
    }
    /* mouse: select */
    if (grab || st->btn_down) cui__vt_mouse_frame(ui, st, v, &geom, in.hovered, can_grab && !mshift);
    if (in.hovered && !grab) cui__set_cursor(ui, CUI_CURSOR_TEXT);
    if (pr.w > 0 && !grab) {
        cui__vt_pos at = cui__vt_pos_at(v, st, pr, padx, pady, cw, lh, ui->mx, ui->my);
        if (in.active && ui->mpressed[CUI_MOUSE_LEFT]) {
            if (ui->click_count >= 3) { st->a = (cui__vt_pos){ at.row, 0 }; st->b = (cui__vt_pos){ at.row, v->cols - 1 }; st->has_sel = true; st->selecting = false; }
            else if (ui->click_count == 2) { cui__vt_word_at(v, at, &st->a, &st->b); st->has_sel = true; st->selecting = false; }
            else { st->a = st->b = at; st->selecting = true; st->has_sel = false; st->press_x = ui->mx; st->press_y = ui->my; }
        }
        if (st->selecting && ui->mdown[CUI_MOUSE_LEFT]) {
            st->b = at;
            if (fabsf(ui->mx - st->press_x) + fabsf(ui->my - st->press_y) > 3) st->has_sel = true;
            if (ui->my < pr.y && v->cur == 0 && st->off < v->sb_count) { st->off++; ui->animating = true; }
            if (ui->my > pr.y + pr.h && st->off > 0) { st->off--; ui->animating = true; }
        }
        if (st->selecting && !ui->mdown[CUI_MOUSE_LEFT]) st->selecting = false;
    }
    /* clipboard and scrollback keys, then the keys a program gets */
    bool ctrl = (ui->mods & CUI_MOD_CTRL) != 0, shift = (ui->mods & CUI_MOD_SHIFT) != 0, sup = (ui->mods & CUI_MOD_SUPER) != 0;
    if (focused && !o->no_input) {
        ui->tab_owner = id;
        bool copy = cui_key_pressed(ui, CUI_KEY_C) && ((ctrl && (st->has_sel || shift)) || (sup && !ctrl));
        bool paste = cui_key_pressed(ui, CUI_KEY_V) && ((ctrl && !(ui->mods & CUI_MOD_ALT)) || sup);
        if (copy && st->has_sel) {
            char *t = cui__vt_selection_text(ui, v, st);
            if (t) { cui_set_clipboard_text(ui, t); cui__free(ui, t); res.copied = true; }
            st->has_sel = false;
            ui->kpressed[CUI_KEY_C] = 0;       /* handled: it is not also the interrupt */
        }
        if (paste) {
            const char *t = cui_get_clipboard_text(ui);
            if (t && *t) { res.paste = t; res.paste_len = (int)strlen(t); res.paste_bracketed = v->bracketed; st->off = 0; st->has_sel = false; }
            ui->kpressed[CUI_KEY_V] = 0;
        }
        if (shift && !ctrl && v->cur == 0) {
            int page = CUI_MAX(1, v->rows - 1);
            if (cui_key_pressed(ui, CUI_KEY_PAGE_UP)) st->off = CUI_MIN(st->off + page, v->sb_count);
            if (cui_key_pressed(ui, CUI_KEY_PAGE_DOWN)) st->off = CUI_MAX(st->off - page, 0);
            if (cui_key_pressed(ui, CUI_KEY_HOME)) st->off = v->sb_count;
            if (cui_key_pressed(ui, CUI_KEY_END)) st->off = 0;
        }
        int before = st->out_n;
        if (o->win32_keys && v->win32_input) { st->w_on = true; cui__vt_keys32(ui, v, st); }
        else {
            if (st->w_on) { st->w_on = false; st->w_cs = 0; st->w_pend_n = 0; memset(st->w_held, 0, sizeof st->w_held); }     /* the program no longer takes records: plain bytes again */
            cui__vt_keys(ui, v, st);
        }
        if (st->out_n > before) { st->off = 0; st->has_sel = false; }
        for (int k = 1; k < CUI_KEY_COUNT; k++) ui->kpressed[k] = 0;   /* nothing else sees what the program got */
    }
    /* right click: copy the selection, or paste */
    if (in.right_clicked && !o->no_input && !grab) {
        if (st->has_sel) {
            char *t = cui__vt_selection_text(ui, v, st);
            if (t) { cui_set_clipboard_text(ui, t); cui__free(ui, t); res.copied = true; }
            st->has_sel = false;
        } else {
            const char *t = cui_get_clipboard_text(ui);
            if (t && *t) { res.paste = t; res.paste_len = (int)strlen(t); res.paste_bracketed = v->bracketed; st->off = 0; }
        }
    }
    res.input = st->out; res.input_len = st->out_n;
    st->out_n = 0;      /* the bytes stay readable in `out` until the next frame writes there */

    int leaf = cui__leaf(ui, id, o->width.kind ? o->width : CUI_FILL, o->height.kind ? o->height : CUI_FILL);
    cui_node *ln = &ui->nodes[leaf];
    ln->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.5f) : CUI_C(ui, SURFACE);
    ln->flags |= CUI_NF_CLIP;
    cui__mark_interactive(ui, leaf, CUI_IF_FOCUSABLE);
    cui__vt_paint *p = (cui__vt_paint *)cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->v = v; p->cw = cw; p->lh = lh; p->size = size; p->padx = padx; p->pady = pady; p->off = st->off;
    p->focused = focused; p->show_cursor = true; p->shape = v->cursor_shape;
    p->glyphs = o->glyphs; p->italic = o->italic;
    p->has_sel = st->has_sel;
    cui__vt_sel_order(st, &p->s0, &p->s1);
    p->fg = CUI_C(ui, FOREGROUND);
    p->bg = ln->bg;
    p->sel = cui_alpha(cui__emphasis(ui), ui->mode == CUI_THEME_DARK ? 0.35f : 0.22f);
    p->cursor = ui->mode == CUI_THEME_DARK ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_SECONDARY);
    p->bar = CUI_C(ui, BORDER_STRONG);
    p->bar_t = in.hovered || st->off > 0 ? 1.0f : 0.0f;
    p->wheel = wid;
    /* an update bracketed by ?2026h and ?2026l is open: show the screen as it was when it opened, for about 150 ms of frames at most */
    if (v->sync_on && v->snap && st->off == 0 && v->snap_cols == v->cols && v->snap_rows == v->rows) {
        if (st->sync_seen != v->sync_seq) { st->sync_seen = v->sync_seq; st->sync_t0 = ui->time; }
        double left = 0.15 - (ui->time - st->sync_t0);
        if (left > 0) {
            p->snap = v->snap; p->scx = v->snap_cx; p->scy = v->snap_cy; p->scursor = v->snap_cursor;
            cui_request_wakeup(ui, (float)left + 0.002f);           /* the live screen shows again even if nothing else asks for a frame */
        }
    }
    if (v->x) p->pics = cui__vt_pic_view(ui, id, v, st->off, p->snap != NULL);   /* textures for the rows of picture on screen */
    ln->paint = cui__paint_vt;
    ln->paint_data = p;
    ui->last_id = id;
    return res;
}

cui_vt_result cui_vt_view_ex(cui_ctx *ui, const char *id_str, cui_vt *v, const cui_vt_opts *o) {
    cui_vt_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    int first = ui->node_count;
    cui_vt_result r = cui__vt_view_body(ui, id_str, v, o);
    cui__keep_ltr(ui, first);
    return r;
}

bool cui_vt_focused(cui_ctx *ui, const char *id_str) { return ui->focus == cui_make_id(ui, id_str ? id_str : "vt"); }
