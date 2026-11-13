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
#define CUI__VT_CP(ca) ((ca) & 0x1FFFFFu)
#define CUI__VT_ATTRS(ca) ((ca) & 0x7F000000u)

typedef struct { uint32_t ca; uint16_t fg, bg; } cui__vt_cell;
typedef struct { cui__vt_cell *cells; uint16_t *map; uint8_t *wrap; } cui__vt_screen;
typedef struct { uint32_t off; uint16_t len; uint8_t wrapped; } cui__vt_line;

enum { CUI__VT_GROUND, CUI__VT_ESC, CUI__VT_ESC_INT, CUI__VT_CSI, CUI__VT_OSC, CUI__VT_OSC_ESC, CUI__VT_STR, CUI__VT_STR_ESC };

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
    uint32_t cp; int need;            /* a UTF-8 sequence cut by the end of a write */
    char osc[160]; int osc_n;
    char title[128];
    char reply[256]; int reply_n;
    cui__vt_cell *sb; size_t sb_cap, sb_pos;
    cui__vt_line *sb_lines; int sb_max, sb_head, sb_count;
    uint64_t sb_total;                /* lines ever pushed to the scrollback */
    uint32_t rgb[CUI__VT_RGB_MAX]; int rgb_n;
    uint8_t *tabs;
};

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
    cui__vt_screen_free(&v->scr[0]); cui__vt_screen_free(&v->scr[1]);
    free(v->sb); free(v->sb_lines); free(v->tabs);
    free(v);
}

size_t cui_vt_memory(const cui_vt *v) {
    size_t n = sizeof *v + (size_t)CUI__VT_MAX_COLS;
    n += (size_t)v->cols * (size_t)v->rows * sizeof(cui__vt_cell) * (v->scr[1].cells ? 2 : 1);
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
}

/* rows top..bot move up by one (the first row leaves), n times; rows that leave the top of the main screen go to the scrollback */
static void cui__vt_scroll_rows(cui_vt *v, int top, int bot, int n, bool keep) {
    cui__vt_screen *s = CUI__VT_S(v);
    n = CUI_MIN(n, bot - top + 1);
    for (int i = 0; i < n; i++) {
        uint16_t slot = s->map[top];
        if (keep && v->cur == 0 && top == 0) cui__vt_sb_push(v, cui__vt_row(v, s, top), v->cols, s->wrap[slot]);
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
}

static void cui__vt_reply(cui_vt *v, const char *s) {
    size_t n = strlen(s);
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

static void cui__vt_sgr(cui_vt *v) {
    if (v->np == 0) { v->attr = 0; v->fg = v->bg = 0; return; }
    for (int k = 0; k < v->np; k++) {
        int c = v->p[k];
        if (c == 0) { v->attr = 0; v->fg = v->bg = 0; }
        else if (c == 1) v->attr |= CUI_VT_BOLD;
        else if (c == 2) v->attr |= CUI_VT_DIM;
        else if (c == 3) v->attr |= CUI_VT_ITALIC;
        else if (c == 4 || c == 21) v->attr |= CUI_VT_UNDERLINE;
        else if (c == 7) v->attr |= CUI_VT_INVERSE;
        else if (c == 9) v->attr |= CUI_VT_STRIKE;
        else if (c == 22) v->attr &= ~(uint32_t)(CUI_VT_BOLD | CUI_VT_DIM);
        else if (c == 23) v->attr &= ~(uint32_t)CUI_VT_ITALIC;
        else if (c == 24) v->attr &= ~(uint32_t)CUI_VT_UNDERLINE;
        else if (c == 27) v->attr &= ~(uint32_t)CUI_VT_INVERSE;
        else if (c == 29) v->attr &= ~(uint32_t)CUI_VT_STRIKE;
        else if (c >= 30 && c <= 37) v->fg = (uint16_t)(1 + c - 30);
        else if (c >= 90 && c <= 97) v->fg = (uint16_t)(1 + c - 90 + 8);
        else if (c >= 40 && c <= 47) v->bg = (uint16_t)(1 + c - 40);
        else if (c >= 100 && c <= 107) v->bg = (uint16_t)(1 + c - 100 + 8);
        else if (c == 39) v->fg = 0;
        else if (c == 49) v->bg = 0;
        else if ((c == 38 || c == 48) && k + 1 < v->np) {
            uint16_t col;
            if (v->p[k + 1] == 5 && k + 2 < v->np) { col = (uint16_t)(1 + (v->p[k + 2] & 255)); k += 2; }
            else if (v->p[k + 1] == 2 && k + 4 < v->np) { col = cui__vt_rgb_id(v, v->p[k + 2], v->p[k + 3], v->p[k + 4]); k += 4; }
            else continue;
            if (c == 38) v->fg = col; else v->bg = col;
        }
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
    if (alt) {
        if (!v->scr[1].cells && !cui__vt_screen_alloc(&v->scr[1], v->cols, v->rows)) return;
        if (save) { v->sx = v->cx; v->sy = v->cy; v->spend = v->pend; v->sattr = v->attr; v->sfg = v->fg; v->sbg = v->bg; }
        v->cur = 1;
        for (int y = 0; y < v->rows; y++) { uint16_t b = v->bg; v->bg = 0; cui__vt_clear_row(v, y); v->bg = b; }
    } else {
        v->cur = 0;
        if (save) { cui__vt_cursor_to(v, v->sx, v->sy); v->pend = v->spend; v->attr = v->sattr; v->fg = v->sfg; v->bg = v->sbg; }
    }
    v->stop = 0; v->sbot = v->rows - 1;
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
        case 2004: v->bracketed = set; break;
        default: break;
        }
    }
}

static void cui__vt_csi(cui_vt *v, char f) {
    int n = cui__vt_arg(v, 0, 1);
    cui__vt_screen *s = CUI__VT_S(v);
    if (v->priv && f != 'h' && f != 'l' && f != 'c') return;
    if (v->inter == '!' && f == 'p') {      /* soft reset */
        v->attr = 0; v->fg = v->bg = 0; v->stop = 0; v->sbot = v->rows - 1; v->autowrap = true; v->cursor_vis = true;
        v->app_cursor = false; v->pend = false;
        return;
    }
    if (v->inter) return;
    switch (f) {
    case '@': {         /* insert blank characters */
        cui__vt_cell *row = cui__vt_row(v, s, v->cy);
        n = CUI_MIN(n, v->cols - v->cx);
        memmove(row + v->cx + n, row + v->cx, sizeof *row * (size_t)(v->cols - v->cx - n));
        cui__vt_fill(row + v->cx, n, v->bg);
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
                                     else if (v->cur == 0) { v->sb_count = 0; v->sb_pos = 0; } }
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
        if (v->cur == 1) cui__vt_switch_screen(v, false, false);
        for (int y = 0; y < v->rows; y++) cui__vt_clear_row(v, y);
        cui__vt_cursor_to(v, 0, 0);
        break;
    }
    default: break;
    }
}

static void cui__vt_osc_end(cui_vt *v) {
    v->osc[v->osc_n] = 0;
    int code = 0, i = 0;
    while (i < v->osc_n && v->osc[i] >= '0' && v->osc[i] <= '9') code = code * 10 + (v->osc[i++] - '0');
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
                if (c == 0x1B) { v->state = CUI__VT_ESC; v->np = 0; v->pany = false; v->priv = v->inter = 0; }
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
            if (c == '[') { v->state = CUI__VT_CSI; v->np = 0; v->pany = false; memset(v->p, 0, sizeof v->p); v->priv = v->inter = 0; }
            else if (c == ']') { v->state = CUI__VT_OSC; v->osc_n = 0; }
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
            if (c == 0x07) { cui__vt_osc_end(v); v->state = CUI__VT_GROUND; }
            else if (c == 0x1B) v->state = CUI__VT_OSC_ESC;
            else if (v->osc_n + 1 < (int)sizeof v->osc) v->osc[v->osc_n++] = (char)c;
            break;
        case CUI__VT_OSC_ESC:
            if (c == '\\') { cui__vt_osc_end(v); v->state = CUI__VT_GROUND; }
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
    /* the cursor must stay on screen: rows above it move into the scrollback */
    if (v->cur == 0 && v->cy >= rows) {
        int drop = v->cy - rows + 1;
        for (int i = 0; i < drop; i++) {
            cui__vt_screen *s = &v->scr[0];
            cui__vt_sb_push(v, cui__vt_row(v, s, 0), v->cols, s->wrap[s->map[0]]);
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
    out->attrs = (int)CUI__VT_ATTRS(c->ca) & ~(int)CUI__VT_SPACER;
    for (int k = 0; k < 2; k++) {
        uint16_t id = k ? c->bg : c->fg;
        int32_t r = id == 0 ? -1 : id < CUI__VT_PALETTE ? id - 1 : (int32_t)(0x1000000u | v->rgb[id - CUI__VT_PALETTE]);
        if (k) out->bg = r; else out->fg = r;
    }
    return true;
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
    char out[768]; int out_n;
} cui__vt_state;

static void cui__vt_out(cui__vt_state *st, const char *s, int n) {
    if (st->out_n + n > (int)sizeof st->out) return;
    memcpy(st->out + st->out_n, s, (size_t)n);
    st->out_n += n;
}
static void cui__vt_outs(cui__vt_state *st, const char *s) { cui__vt_out(st, s, (int)strlen(s)); }

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

/* ------------------------------------------------------------------------ */
/* Box drawing                                                              */
/* ------------------------------------------------------------------------ */

/* U+2500..U+257F as four arms (left, right, up, down), each 0 none, 1 light, 2 heavy, 3 double, so lines meet
 * exactly across cells whatever the font has. Dashed lines are drawn solid; the diagonals are lines. */
#define CUI__VT_ARMS(l, r, u, d) ((uint8_t)((l) | ((r) << 2) | ((u) << 4) | ((d) << 6)))
static uint8_t cui__vt_box_arms(uint32_t c) {
    static const uint8_t t[] = {
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2), CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(0,1,0,1), CUI__VT_ARMS(0,2,0,1), CUI__VT_ARMS(0,1,0,2), CUI__VT_ARMS(0,2,0,2), CUI__VT_ARMS(1,0,0,1), CUI__VT_ARMS(2,0,0,1), CUI__VT_ARMS(1,0,0,2), CUI__VT_ARMS(2,0,0,2),
        CUI__VT_ARMS(0,1,1,0), CUI__VT_ARMS(0,2,1,0), CUI__VT_ARMS(0,1,2,0), CUI__VT_ARMS(0,2,2,0), CUI__VT_ARMS(1,0,1,0), CUI__VT_ARMS(2,0,1,0), CUI__VT_ARMS(1,0,2,0), CUI__VT_ARMS(2,0,2,0),
        CUI__VT_ARMS(0,1,1,1), CUI__VT_ARMS(0,2,1,1), CUI__VT_ARMS(0,1,2,1), CUI__VT_ARMS(0,1,1,2), CUI__VT_ARMS(0,1,2,2), CUI__VT_ARMS(0,2,2,1), CUI__VT_ARMS(0,2,1,2), CUI__VT_ARMS(0,2,2,2),
        CUI__VT_ARMS(1,0,1,1), CUI__VT_ARMS(2,0,1,1), CUI__VT_ARMS(1,0,2,1), CUI__VT_ARMS(1,0,1,2), CUI__VT_ARMS(1,0,2,2), CUI__VT_ARMS(2,0,2,1), CUI__VT_ARMS(2,0,1,2), CUI__VT_ARMS(2,0,2,2),
        CUI__VT_ARMS(1,1,0,1), CUI__VT_ARMS(2,1,0,1), CUI__VT_ARMS(1,2,0,1), CUI__VT_ARMS(2,2,0,1), CUI__VT_ARMS(1,1,0,2), CUI__VT_ARMS(2,1,0,2), CUI__VT_ARMS(1,2,0,2), CUI__VT_ARMS(2,2,0,2),
        CUI__VT_ARMS(1,1,1,0), CUI__VT_ARMS(2,1,1,0), CUI__VT_ARMS(1,2,1,0), CUI__VT_ARMS(2,2,1,0), CUI__VT_ARMS(1,1,2,0), CUI__VT_ARMS(2,1,2,0), CUI__VT_ARMS(1,2,2,0), CUI__VT_ARMS(2,2,2,0),
        CUI__VT_ARMS(1,1,1,1), CUI__VT_ARMS(2,1,1,1), CUI__VT_ARMS(1,2,1,1), CUI__VT_ARMS(2,2,1,1), CUI__VT_ARMS(1,1,2,1), CUI__VT_ARMS(1,1,1,2), CUI__VT_ARMS(1,1,2,2), CUI__VT_ARMS(2,1,2,1),
        CUI__VT_ARMS(1,2,2,1), CUI__VT_ARMS(2,1,1,2), CUI__VT_ARMS(1,2,1,2), CUI__VT_ARMS(2,2,2,1), CUI__VT_ARMS(2,2,1,2), CUI__VT_ARMS(2,1,2,2), CUI__VT_ARMS(1,2,2,2), CUI__VT_ARMS(2,2,2,2),
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(3,3,0,0), CUI__VT_ARMS(0,0,3,3), CUI__VT_ARMS(0,3,0,1), CUI__VT_ARMS(0,1,0,3), CUI__VT_ARMS(0,3,0,3), CUI__VT_ARMS(3,0,0,1), CUI__VT_ARMS(1,0,0,3), CUI__VT_ARMS(3,0,0,3),
        CUI__VT_ARMS(0,3,1,0), CUI__VT_ARMS(0,1,3,0), CUI__VT_ARMS(0,3,3,0), CUI__VT_ARMS(3,0,1,0), CUI__VT_ARMS(1,0,3,0), CUI__VT_ARMS(3,0,3,0),
        CUI__VT_ARMS(0,3,1,1), CUI__VT_ARMS(0,1,3,3), CUI__VT_ARMS(0,3,3,3), CUI__VT_ARMS(3,0,1,1), CUI__VT_ARMS(1,0,3,3), CUI__VT_ARMS(3,0,3,3),
        CUI__VT_ARMS(3,3,0,1), CUI__VT_ARMS(1,1,0,3), CUI__VT_ARMS(3,3,0,3), CUI__VT_ARMS(3,3,1,0), CUI__VT_ARMS(1,1,3,0), CUI__VT_ARMS(3,3,3,0),
        CUI__VT_ARMS(3,3,1,1), CUI__VT_ARMS(1,1,3,3), CUI__VT_ARMS(3,3,3,3),
        CUI__VT_ARMS(0,1,0,1), CUI__VT_ARMS(1,0,0,1), CUI__VT_ARMS(1,0,1,0), CUI__VT_ARMS(0,1,1,0),
    };
    if (c >= 0x2574 && c <= 0x257F) {
        static const uint8_t h[] = { CUI__VT_ARMS(1,0,0,0), CUI__VT_ARMS(0,0,1,0), CUI__VT_ARMS(0,1,0,0), CUI__VT_ARMS(0,0,0,1), CUI__VT_ARMS(2,0,0,0), CUI__VT_ARMS(0,0,2,0),
                                     CUI__VT_ARMS(0,2,0,0), CUI__VT_ARMS(0,0,0,2), CUI__VT_ARMS(1,2,0,0), CUI__VT_ARMS(0,0,1,2), CUI__VT_ARMS(2,1,0,0), CUI__VT_ARMS(0,0,2,1) };
        return h[c - 0x2574];
    }
    return c >= 0x2500 && c < 0x2500 + sizeof t ? t[c - 0x2500] : 0;
}

/* a rectangle on whole device pixels, so neighbouring cells meet without seams */
static void cui__vt_snap_rect(cui_ctx *ui, float x0, float y0, float x1, float y1, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    x0 = floorf(x0 * s + 0.5f) / s; x1 = floorf(x1 * s + 0.5f) / s;
    y0 = floorf(y0 * s + 0.5f) / s; y1 = floorf(y1 * s + 0.5f) / s;
    if (x1 > x0 && y1 > y0) cui_draw_rect(ui, (cui_rect){ x0, y0, x1 - x0, y1 - y0 }, CUI_CORNERS(0), col);
}

/* One arm of a box character. `i`: 0 left, 1 right, 2 up, 3 down. A double arm is two lines; where the crossing
 * line is double too, each line reaches the crossing line on its side (a corner closes) or stops at its inner
 * line (a gap for the arm on the other side to pass through). */
static void cui__vt_box_arm(cui_ctx *ui, int i, const int *arm, cui_rect c, float light, float heavy, cui_color col) {
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

static bool cui__vt_box(cui_ctx *ui, uint32_t cp, cui_rect c, cui_color col) {
    if (cp < 0x2500 || cp > 0x257F) return false;
    if (cp >= 0x2571 && cp <= 0x2573) {     /* diagonals */
        if (cp != 0x2572) cui_draw_line(ui, (cui_vec2){ c.x, c.y + c.h }, (cui_vec2){ c.x + c.w, c.y }, 1, col);
        if (cp != 0x2571) cui_draw_line(ui, (cui_vec2){ c.x, c.y }, (cui_vec2){ c.x + c.w, c.y + c.h }, 1, col);
        return true;
    }
    uint8_t a = cui__vt_box_arms(cp);
    if (!a) return false;
    int arm[4] = { a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3 };     /* left, right, up, down */
    float s = ui->scale > 0 ? ui->scale : 1;
    float light = fmaxf(1.0f, floorf(c.h / 16.0f * s + 0.5f) / s), heavy = light * 2.0f + (c.h > 24 ? 1.0f : 0.0f);
    for (int i = 0; i < 4; i++) if (arm[i]) cui__vt_box_arm(ui, i, arm, c, light, heavy, col);
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

static void cui__paint_vt(cui_ctx *ui, cui_node *n, void *data) {
    cui__vt_paint *p = (cui__vt_paint *)data;
    const cui_vt *v = p->v;
    cui_rect r = n->rect;
    cui__paint_wheel_hit(ui, p->wheel, r);
    float x0 = r.x + p->padx, y0 = r.y + p->pady, fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    cui_push_clip(ui, (cui_rect){ r.x, r.y + 1, r.w, r.h - 2 });
    char *buf = (char *)cui__frame_alloc(ui, (size_t)v->cols * 4 + 8);
    for (int y = 0; y < v->rows; y++) {
        float ry = y0 + (float)y * p->lh;
        if (ry > r.y + r.h) break;
        int cn; bool wrapped;
        int64_t arow = (int64_t)v->sb_total - p->off + y;
        const cui__vt_cell *cells = cui__vt_cells_abs(v, arow, &cn, &wrapped);
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
            if (cui__vt_box(ui, ch, (cui_rect){ x0 + x * p->cw, ry, p->cw, p->lh }, fg)) { x++; continue; }
            if (ch >= 0x2580 && ch <= 0x2593) { cui__tm_block(ui, (int)(ch - 0x2580), (cui_rect){ x0 + x * p->cw, ry, p->cw, p->lh }, fg); x++; continue; }
            if (ch >= 128) {
                char u[4]; size_t k = cui__vt_utf8(ch, u);
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw, ty, fg, u, (int)k, true);
                if (at & CUI_VT_BOLD) cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + x * p->cw + 0.45f, ty, fg, u, (int)k, true);
                if (at & CUI_VT_UNDERLINE) cui_draw_rect(ui, (cui_rect){ x0 + x * p->cw, ry + p->lh - 2, p->cw, 1 }, CUI_CORNERS(0), fg);
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
            if (at & CUI_VT_UNDERLINE) cui_draw_rect(ui, (cui_rect){ x0 + x * p->cw, ry + p->lh - 2, len * p->cw, 1 }, CUI_CORNERS(0), fg);
            if (at & CUI_VT_STRIKE) cui_draw_rect(ui, (cui_rect){ x0 + x * p->cw, ry + p->lh * 0.5f, len * p->cw, 1 }, CUI_CORNERS(0), fg);
            x = e;
        }
    }
    /* the cursor */
    if (p->show_cursor && v->cursor_vis && p->off == 0) {
        float cx = x0 + v->cx * p->cw, cy = y0 + v->cy * p->lh;
        if (p->focused) {
            cui__vt_snap_rect(ui, cx, cy, cx + p->cw, cy + p->lh, p->cursor);
            int cn; bool w;
            const cui__vt_cell *cells = cui__vt_cells_at(v, v->cy, &cn, &w);
            uint32_t ch = cells && v->cx < cn ? CUI__VT_CP(cells[v->cx].ca) : 0;
            if (ch > ' ' && ch != 0x7F) {
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
    float lh = ceilf(size * 1.4f), padx = 10, pady = 6;
    cui_rect pr = cui__prev_rect(ui, id);
    if (!v) return res;
    if (o->focus) ui->focus = id;
    cui_interact in = cui__interact(ui, id, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    bool focused = ui->focus == id;
    res.focused = focused;
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
    /* wheel */
    if (ui->scroll_target == wid && ui->scroll_dy != 0) {
        float d = ui->scroll_dy;
        if (v->cur == 0) st->off = CUI_CLAMP(st->off + (int)lroundf(d * 3), 0, v->sb_count);
        else {   /* a full screen program has no scrollback: the wheel moves its cursor */
            int k = (int)lroundf(fabsf(d) * 3);
            for (int i = 0; i < k; i++) cui__vt_outs(st, d > 0 ? (v->app_cursor ? "\x1bOA" : "\x1b[A") : (v->app_cursor ? "\x1bOB" : "\x1b[B"));
        }
        ui->scroll_dy = 0;
    }
    /* mouse: select */
    if (in.hovered) cui__set_cursor(ui, CUI_CURSOR_TEXT);
    if (pr.w > 0) {
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
        cui__vt_keys(ui, v, st);
        if (st->out_n > before) { st->off = 0; st->has_sel = false; }
        for (int k = 1; k < CUI_KEY_COUNT; k++) ui->kpressed[k] = 0;   /* nothing else sees what the program got */
    }
    /* right click: copy the selection, or paste */
    if (in.right_clicked && !o->no_input) {
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
    p->focused = focused; p->show_cursor = true;
    p->has_sel = st->has_sel;
    cui__vt_sel_order(st, &p->s0, &p->s1);
    p->fg = CUI_C(ui, FOREGROUND);
    p->bg = ln->bg;
    p->sel = cui_alpha(cui__emphasis(ui), ui->mode == CUI_THEME_DARK ? 0.35f : 0.22f);
    p->cursor = ui->mode == CUI_THEME_DARK ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_SECONDARY);
    p->bar = CUI_C(ui, BORDER_STRONG);
    p->bar_t = in.hovered || st->off > 0 ? 1.0f : 0.0f;
    p->wheel = wid;
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
