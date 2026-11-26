/* Saving a screen and loading it again (cui_vt_save, cui_vt_load; the format is in cui.h).
 *
 * The saved rows are a terminal byte stream: the text of each cell, SGR when the pen changes, CUF over blank cells, ECH for
 * blank cells with a background, CR LF between rows that do not wrap (a soft-wrapped row is written to its edge and the next
 * one goes on without a line break, so the screen wraps it again, also at another width), OSC 8 around hyperlinked cells and
 * OSC 133 / 633;E where the command marks were. Loading writes that stream into the screen as output, after checking that
 * nothing else is in it, so the screen's own code puts every cell, link and mark back: there is no second way of filling the
 * grid to keep in step with the first. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")                    /* runs at a save and a load, never per byte of output: small over fast */
#endif
#include "cui_internal.h"

#include <limits.h>

#define CUI__VTS_HEAD 22
#define CUI__VTS_ROW 1024                      /* the widest row (CUI__VT_MAX_COLS is 1000) */
#define CUI__VTS_STREAM_MAX ((size_t)1 << 28)  /* a load refuses a stream past 256 MB */
#define CUI__VTS_NONE INT64_MIN

typedef struct { cui_vt_saved *b; bool oom; } cui__vts_w;

static void cui__vts_put(cui__vts_w *w, const void *p, size_t n) {
    cui_vt_saved *b = w->b;
    if (w->oom || n == 0) return;
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n) cap *= 2;
        unsigned char *nd = (unsigned char *)realloc(b->data, cap);
        if (!nd) { w->oom = true; return; }
        b->data = nd; b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void cui__vts_str(cui__vts_w *w, const char *s) { cui__vts_put(w, s, strlen(s)); }
/* a number in decimal at t (no snprintf: a save writes tens of thousands of them); returns the end */
static char *cui__vts_dec(char *t, long v) {
    char d[24];
    int n = 0;
    unsigned long u = v < 0 ? 0ul - (unsigned long)v : (unsigned long)v;
    do { d[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *t++ = '-';
    while (n) *t++ = d[--n];
    return t;
}
static void cui__vts_num(cui__vts_w *w, const char *pre, int n, const char *post) {
    char t[64], *e = t;
    size_t a = strlen(pre), b = strlen(post);
    memcpy(e, pre, a); e += a;
    e = cui__vts_dec(e, n);
    memcpy(e, post, b); e += b;
    cui__vts_put(w, t, (size_t)(e - t));
}
static void cui__vts_u32(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24); }
static uint32_t cui__vts_rd32(const unsigned char *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static size_t cui__vts_utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* ---- the pen ---- */

typedef struct { int attrs; int32_t fg, bg; } cui__vts_pen;

static char *cui__vts_cat(char *t, const char *s) { while (*s) *t++ = *s++; return t; }

static char *cui__vts_color(char *t, int32_t c, const char *base) {
    if (c < 0) return t;
    t = cui__vts_cat(t, base);
    if (c < 256) { t = cui__vts_cat(t, "5;"); return cui__vts_dec(t, c); }
    t = cui__vts_cat(t, "2;");
    t = cui__vts_dec(t, (c >> 16) & 255); *t++ = ';';
    t = cui__vts_dec(t, (c >> 8) & 255); *t++ = ';';
    return cui__vts_dec(t, c & 255);
}

static void cui__vts_sgr(cui__vts_w *w, cui__vts_pen *cur, int attrs, int32_t fg, int32_t bg) {
    if (cur->attrs == attrs && cur->fg == fg && cur->bg == bg) return;
    char t[96], *e = cui__vts_cat(t, "\x1b[0");
    if (attrs & CUI_VT_BOLD) e = cui__vts_cat(e, ";1");
    if (attrs & CUI_VT_DIM) e = cui__vts_cat(e, ";2");
    if (attrs & CUI_VT_ITALIC) e = cui__vts_cat(e, ";3");
    if (attrs & CUI_VT_UNDERLINE)
        e = cui__vts_cat(e, (attrs & CUI_VT_UNDERLINE_DOUBLE) ? ";4:2" : (attrs & CUI_VT_UNDERLINE_CURLY) ? ";4:3" :
                            (attrs & CUI_VT_UNDERLINE_DOTTED) ? ";4:4" : (attrs & CUI_VT_UNDERLINE_DASHED) ? ";4:5" : ";4");
    if (attrs & CUI_VT_INVERSE) e = cui__vts_cat(e, ";7");
    if (attrs & CUI_VT_STRIKE) e = cui__vts_cat(e, ";9");
    if (attrs & CUI_VT_OVERLINE) e = cui__vts_cat(e, ";53");
    e = cui__vts_color(e, fg, ";38;");
    e = cui__vts_color(e, bg, ";48;");
    *e++ = 'm';
    cui__vts_put(w, t, (size_t)(e - t));
    cur->attrs = attrs; cur->fg = fg; cur->bg = bg;
}

/* ---- marks: each A, B (with 633;E), C, D as an event at its cell, in the order of the cells ---- */

typedef struct { int64_t row; int col; int mark, kind; } cui__vts_ev;    /* kind: 0 A, 1 B, 2 C, 3 D */

static int cui__vts_ev_cmp(const void *a, const void *b) {
    const cui__vts_ev *x = (const cui__vts_ev *)a, *y = (const cui__vts_ev *)b;
    if (x->row != y->row) return x->row < y->row ? -1 : 1;
    if (x->col != y->col) return x->col < y->col ? -1 : 1;
    if (x->mark != y->mark) return x->mark < y->mark ? -1 : 1;
    return x->kind - y->kind;
}

static void cui__vts_emit_ev(cui__vts_w *w, const cui__vts_ev *e, const cui__vt_mkx *mk) {
    const cui__vt_mkx *m = &mk[e->mark];
    switch (e->kind) {
    case 0: cui__vts_str(w, "\x1b]133;A\x1b\\"); break;
    case 1:
        cui__vts_str(w, "\x1b]133;B\x1b\\");
        if (m->cmd && m->cmd[0]) {                                 /* 633;E's escapes: \\ and \xHH for ; and the control bytes */
            cui__vts_str(w, "\x1b]633;E;");
            for (const unsigned char *s = (const unsigned char *)m->cmd; *s; s++) {
                char t[8];
                if (*s == '\\') cui__vts_put(w, "\\\\", 2);
                else if (*s < 0x20 || *s == 0x7F || *s == ';') { snprintf(t, sizeof t, "\\x%02X", *s); cui__vts_put(w, t, 4); }
                else cui__vts_put(w, s, 1);
            }
            cui__vts_str(w, "\x1b\\");
        }
        break;
    case 2: cui__vts_str(w, "\x1b]133;C\x1b\\"); break;
    default:
        if (m->exit_code == -1) cui__vts_str(w, "\x1b]133;D\x1b\\");
        else cui__vts_num(w, "\x1b]133;D;", m->exit_code, "\x1b\\");
        break;
    }
}

/* ---- links ---- */

static const char *cui__vts_uri(const cui_vt *v, uint16_t id) {
    const char *u = id ? cui__vt_lk_uri_of(v, id) : NULL;
    if (!u || !*u) return NULL;
    for (const unsigned char *s = (const unsigned char *)u; *s; s++) if (*s < 0x20 || *s == 0x7F) return NULL;   /* never in an OSC we write */
    return u;
}

static void cui__vts_link(cui__vts_w *w, const cui_vt *v, uint16_t *cur, uint16_t id) {
    if (id == *cur) return;
    const char *u = cui__vts_uri(v, id);
    if (!u) { if (*cur) cui__vts_str(w, "\x1b]8;;\x1b\\"); *cur = 0; return; }
    cui__vts_str(w, "\x1b]8;;");
    cui__vts_str(w, u);
    cui__vts_str(w, "\x1b\\");
    *cur = id;
}

/* ---- saving ---- */

static bool cui__vts_blank(const cui__vt_scell *c) { return c->cp == 0 && !c->spacer; }

size_t cui_vt_save(const cui_vt *v, int max_lines, cui_vt_saved *out) {
    if (!v || !out) return 0;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    cui__vt_scell *row = (cui__vt_scell *)malloc(sizeof *row * CUI__VTS_ROW);
    if (!row) return 0;
    size_t start = out->len;
    cui__vts_w w = { out, false };
    unsigned char head[CUI__VTS_HEAD] = { 'C', 'U', 'V', 'T', CUI_VT_SAVE_VERSION, 0 };
    head[6] = (unsigned char)in.cols; head[7] = (unsigned char)(in.cols >> 8);
    head[8] = (unsigned char)in.rows; head[9] = (unsigned char)(in.rows >> 8);
    cui__vts_put(&w, head, sizeof head);

    /* the rows: the scrollback, then the main screen down to its last row that holds anything (or the cursor's) */
    int cx, cy, wr;
    cui__vt_save_cursor(v, &cx, &cy);
    int last = cy;
    for (int y = in.rows - 1; y > last; y--) {
        int n = cui__vt_save_row(v, (int64_t)in.sb_total + y, row, CUI__VTS_ROW, &wr);
        bool any = false;
        for (int x = 0; x < n && !any; x++) any = !cui__vts_blank(&row[x]) || row[x].bg != -1;
        if (any) { last = y; break; }
    }
    int64_t end = (int64_t)in.sb_total + last;                  /* the last row saved */
    int64_t first = (int64_t)in.sb_total - in.sb_count;
    if (max_lines > 0 && end - first + 1 > max_lines) first = end - max_lines + 1;

    /* the marks whose prompt is among them */
    cui__vt_mkx *mk = NULL;
    cui__vts_ev *ev = NULL;
    int nm = cui__vt_mk_export(v, NULL, 0), ne = 0, m0 = nm, used_marks = 0;
    if (nm > 0) {
        mk = (cui__vt_mkx *)malloc(sizeof *mk * (size_t)nm);
        ev = (cui__vts_ev *)malloc(sizeof *ev * (size_t)nm * 4);
        if (!mk || !ev) { free(mk); free(ev); free(row); out->len = start; return 0; }
        cui__vt_mk_export(v, mk, nm);
        for (int i = 0; i < nm; i++) {
            if (mk[i].row[0] == CUI__VTS_NONE || mk[i].row[0] < first || mk[i].row[0] > end) continue;
            if (m0 == nm) m0 = i;
            used_marks = i - m0 + 1;
            for (int k = 0; k < 4; k++)
                if (mk[i].row[k] != CUI__VTS_NONE && mk[i].row[k] >= first && mk[i].row[k] <= end)
                    ev[ne++] = (cui__vts_ev){ mk[i].row[k], mk[i].col[k], i, k };
        }
        qsort(ev, (size_t)ne, sizeof *ev, cui__vts_ev_cmp);
    }

    cui__vts_pen pen = { 0, -1, -1 };
    uint16_t link = 0;
    int ei = 0;
    bool pend = false;                                          /* the last row was written to its edge and wraps on */
    for (int64_t r = first; r <= end; r++) {
        int n = cui__vt_save_row(v, r, row, CUI__VTS_ROW, &wr);
        if (n < 0) n = 0;
        int lid_n = 0;
        const uint16_t *lid = cui__vt_lk_main_ids(v, r, &lid_n);
        int use = n;
        bool wraps = false;
        if (wr == 2 && use == in.cols && use > 0 && cui__vts_blank(&row[use - 1])) { use--; wraps = r < end; }   /* the padding before a wide character that moved on */
        else {
            while (use > 0 && cui__vts_blank(&row[use - 1]) && row[use - 1].bg == -1) use--;
            wraps = wr != 0 && r < end && use == in.cols && !cui__vts_blank(&row[use - 1]);
        }
        if (!pend && r > first) {                               /* a line break before this row */
            cui__vts_link(&w, v, &link, 0);
            cui__vts_sgr(&w, &pen, 0, -1, -1);                  /* a row a line feed brings in is cleared with the pen */
            cui__vts_str(&w, "\r\n");
        }
        int at = 0;                                             /* the column the cursor is at in the screen it is loaded into */
        for (int x = 0; x < use;) {
            while (ei < ne && ev[ei].row == r && ev[ei].col <= x) cui__vts_emit_ev(&w, &ev[ei++], mk);
            int next_ev = ei < ne && ev[ei].row == r ? ev[ei].col : INT_MAX;
            const cui__vt_scell *c = &row[x];
            uint16_t id = lid && x < lid_n ? lid[x] : 0;
            if (cui__vts_blank(c) || c->spacer) {               /* blank cells; a second half reached here lost its first (an orphan) */
                int k = 1;
                while (x + k < use && x + k < next_ev && row[x + k].bg == c->bg && cui__vts_blank(&row[x + k])) k++;
                cui__vts_link(&w, v, &link, 0);
                if (c->bg != -1) {
                    cui__vts_sgr(&w, &pen, 0, -1, c->bg);
                    cui__vts_num(&w, "\x1b[", k, "X");
                }
                if (x + k < in.cols) { cui__vts_num(&w, "\x1b[", k, "C"); at = x + k; }
                x += k;
                continue;
            }
            cui__vts_link(&w, v, &link, id);
            cui__vts_sgr(&w, &pen, c->attrs, c->fg, c->bg);
            char u[4];
            uint32_t cp = c->cp < 0x20 || c->cp == 0x7F || c->cp > 0x10FFFF ? 0xFFFD : c->cp;
            cui__vts_put(&w, u, cui__vts_utf8(cp, u));
            x += x + 1 < use && row[x + 1].spacer ? 2 : 1;
            at = x;
        }
        bool moved = false;
        while (ei < ne && ev[ei].row == r) {                    /* marks past the last cell written (a prompt's end, an empty line) */
            if (ev[ei].col != at) { cui__vts_num(&w, "\x1b[", ev[ei].col + 1, "G"); at = ev[ei].col; moved = true; }
            cui__vts_emit_ev(&w, &ev[ei++], mk);
        }
        pend = wraps && !moved;
    }

    /* the cursor where it was, a running command ended, the pen and the link closed */
    int64_t crow = (int64_t)in.sb_total + cy;
    if (crow >= first && crow <= end) {
        if (end > crow) cui__vts_num(&w, "\x1b[", (int)(end - crow), "A");
        cui__vts_num(&w, "\x1b[", cx + 1, "G");
    }
    if (used_marks > 0 && !mk[m0 + used_marks - 1].finished) cui__vts_str(&w, "\x1b]133;D\x1b\\");
    cui__vts_link(&w, v, &link, 0);
    cui__vts_str(&w, "\x1b[0m");
    size_t stream = w.oom ? 0 : out->len - start - CUI__VTS_HEAD;
    for (int i = 0; i < used_marks; i++) {
        const cui__vt_mkx *m = &mk[m0 + i];
        double d = m->dur_ms;
        uint32_t ms = m->finished && d >= 0 ? (d > 4e9 ? 0xFFFFFFFEu : (uint32_t)(d + 0.5)) : 0xFFFFFFFFu;
        unsigned char t[4];
        cui__vts_u32(t, ms);
        cui__vts_put(&w, t, 4);
    }
    free(mk); free(ev); free(row);
    if (w.oom) { out->len = start; return 0; }
    unsigned char *h = out->data + start;
    cui__vts_u32(h + 10, (uint32_t)(end - first + 1));
    cui__vts_u32(h + 14, (uint32_t)stream);
    cui__vts_u32(h + 18, (uint32_t)used_marks);
    return out->len - start;
}

void cui_vt_saved_free(cui_vt_saved *b) {
    if (!b) return;
    free(b->data);
    memset(b, 0, sizeof *b);
}

/* ---- loading ---- */

static bool cui__vts_prefix(const unsigned char *s, size_t n, const char *p) {
    size_t k = strlen(p);
    return n >= k && !memcmp(s, p, k);
}

/* only what cui_vt_save writes: text, CR, LF, SGR, CUF, ECH, CUU, CHA, OSC 8, OSC 133 A..D, 633;E */
static bool cui__vts_stream_ok(const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = d[i];
        if (c == 0x1B) {
            if (i + 1 >= n) return false;
            if (d[i + 1] == '[') {
                size_t j = i + 2;
                while (j < n && ((d[j] >= '0' && d[j] <= '9') || d[j] == ';' || d[j] == ':') && j - i < 200) j++;
                if (j >= n || !strchr("mCXAG", d[j]) || !d[j]) return false;
                i = j + 1;
                continue;
            }
            if (d[i + 1] != ']') return false;
            size_t j = i + 2;
            while (j < n && d[j] != 0x1B) { if (d[j] < 0x20 || d[j] == 0x7F) return false; j++; }
            if (j + 1 >= n || d[j + 1] != '\\') return false;
            const unsigned char *s = d + i + 2;
            size_t k = j - (i + 2);
            bool ok = cui__vts_prefix(s, k, "8;") || cui__vts_prefix(s, k, "633;E;") ||
                      (cui__vts_prefix(s, k, "133;") && k >= 5 && s[4] >= 'A' && s[4] <= 'D' && (k == 5 || s[5] == ';'));
            if (!ok) return false;
            i = j + 2;
            continue;
        }
        if (c < 0x20 && c != '\r' && c != '\n') return false;
        if (c == 0x7F) return false;
        i++;
    }
    return true;
}

bool cui_vt_load(cui_vt *v, const void *data, size_t len) {
    const unsigned char *d = (const unsigned char *)data;
    if (!v || !d || len < CUI__VTS_HEAD || memcmp(d, "CUVT", 4) || d[4] != CUI_VT_SAVE_VERSION) return false;
    uint32_t n = cui__vts_rd32(d + 14), m = cui__vts_rd32(d + 18);
    if (n > CUI__VTS_STREAM_MAX || m > 1000000u || (uint64_t)CUI__VTS_HEAD + n + 4ull * m != (uint64_t)len) return false;
    if (!cui__vts_stream_ok(d + CUI__VTS_HEAD, n)) return false;
    double *dur = m ? (double *)malloc(sizeof *dur * m) : NULL;
    if (m && !dur) return false;
    cui_vt_write(v, (const char *)d + CUI__VTS_HEAD, n);
    for (uint32_t i = 0; i < m; i++) {
        uint32_t ms = cui__vts_rd32(d + CUI__VTS_HEAD + n + 4 * i);
        dur[i] = ms == 0xFFFFFFFFu ? -1 : (double)ms;
    }
    int have = cui_vt_mark_count(v);
    if (m && have >= (int)m) cui__vt_mk_set_durations(v, have - (int)m, dur, (int)m);
    free(dur);
    return true;
}
