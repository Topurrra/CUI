/* VT terminal: find in the scrollback and the screen (cui_vt_find and friends in cui.h, the view's `.find`).
 *
 * A search walks the rows oldest first and joins the rows of a wrapped line into one line of UTF-8 text, with the cell
 * each byte came from, then looks for the query in it: plain bytes (with the letters folded to one case when the case
 * does not matter) or cui_regex_search. Matches are kept as cell spans, rows counted as the view counts them (sb_total
 * based), so a match in the scrollback stays where it is while new output arrives. The screen itself (cui_vt.c) only
 * counts its writes; a view with `.find` uses that count to search again when the text may have changed. */
#include "cui_internal.h"

#define CUI__VTF_MAX 50000          /* matches kept */
#define CUI__VTF_Q 512              /* bytes of query */
#define CUI__VTF_COLBITS 10         /* a cell position in a line: row in the line << 10 | column (columns are under 1024) */
#define CUI__VTF_REFRESH 0.1        /* seconds between searches a view starts because output arrived */

struct cui__vt_find {
    char q[CUI__VTF_Q];             /* the query as given */
    char fq[CUI__VTF_Q];            /* plain search: the query with its letters folded when the case does not matter */
    int fqn, flags;
    bool active;                    /* a query is set */
    cui__vt_span *m; int n, cap;
    bool truncated;
    int cur;                        /* the current match, -1 none */
    uint32_t seq;                   /* bumps when the current match moves: a view scrolls to it and selects it */
    const char *error;
    uint32_t wseq; int cols, rows, scr; uint64_t sb_total;   /* the screen as it was searched */
    double t_last;                  /* ui->time of the last search a view started */
    uint32_t *cps; int cps_cap;     /* one row's code points */
    char *text; uint32_t *pos; int tcap;   /* one line's text, and per byte the cell of the character it belongs to */
};

/* ---- text ---- */

/* one case for the letters whose two cases have UTF-8 sequences of the same length: ASCII, Latin-1, Greek, Cyrillic */
static uint32_t cui__vtf_fold(uint32_t c) {
    if (c < 128) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

static int cui__vtf_put(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | (c >> 18)); o[1] = (char)(0x80 | ((c >> 12) & 63)); o[2] = (char)(0x80 | ((c >> 6) & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}

/* the code point at s[i] (len bytes), its length in *k; a broken sequence is one byte */
static uint32_t cui__vtf_get(const char *s, int len, int i, int *k) {
    const unsigned char *u = (const unsigned char *)s + i;
    int left = len - i;
    if (u[0] < 0x80 || left < 2) { *k = 1; return u[0]; }
    if ((u[0] & 0xE0) == 0xC0) { *k = 2; return ((uint32_t)(u[0] & 31) << 6) | (u[1] & 63); }
    if ((u[0] & 0xF0) == 0xE0 && left >= 3) { *k = 3; return ((uint32_t)(u[0] & 15) << 12) | ((uint32_t)(u[1] & 63) << 6) | (u[2] & 63); }
    if ((u[0] & 0xF8) == 0xF0 && left >= 4) { *k = 4; return ((uint32_t)(u[0] & 7) << 18) | ((uint32_t)(u[1] & 63) << 12) | ((uint32_t)(u[2] & 63) << 6) | (u[3] & 63); }
    *k = 1; return u[0];
}

static bool cui__vtf_wordch(uint32_t c) { return c >= 128 || c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

/* whole word: no word character just before s or at e */
static bool cui__vtf_word_ok(const char *t, int len, int s, int e) {
    int k;
    if (s > 0) {
        int b = s - 1;
        while (b > 0 && ((unsigned char)t[b] & 0xC0) == 0x80) b--;
        if (cui__vtf_wordch(cui__vtf_get(t, len, b, &k))) return false;
    }
    return e >= len || !cui__vtf_wordch(cui__vtf_get(t, len, e, &k));
}

/* ---- search ---- */

static cui__vt_find *cui__vtf_make(cui_vt *v) {
    cui__vt_find **slot = cui__vt_find_slot(v);
    if (!*slot) {
        cui__vt_find *f = (cui__vt_find *)calloc(1, sizeof *f);
        if (!f) return NULL;
        f->cur = -1;
        *slot = f;
    }
    return *slot;
}

static bool cui__vtf_text_room(cui__vt_find *f, int need) {
    if (need <= f->tcap) return true;
    int cap = CUI_MAX(need, f->tcap * 2);
    char *t = (char *)realloc(f->text, (size_t)cap);
    if (t) f->text = t;
    uint32_t *p = (uint32_t *)realloc(f->pos, sizeof(uint32_t) * (size_t)cap);
    if (p) f->pos = p;
    if (!t || !p) return false;
    f->tcap = cap;
    return true;
}

/* one match, bytes s..e of the line that starts at row r0 */
static bool cui__vtf_add(cui__vt_find *f, int64_t line_row, int s, int e) {
    if (f->n >= CUI__VTF_MAX) { f->truncated = true; return false; }
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 64;
        cui__vt_span *m = (cui__vt_span *)realloc(f->m, sizeof *m * (size_t)cap);
        if (!m) { f->truncated = true; return false; }
        f->m = m; f->cap = cap;
    }
    uint32_t a = f->pos[s], b = f->pos[e];
    cui__vt_span sp = { line_row + (a >> CUI__VTF_COLBITS), line_row + (b >> CUI__VTF_COLBITS), (int)(a & 1023u), (int)(b & 1023u) };
    if (sp.c1 == 0 && sp.r1 > sp.r0) { sp.r1--; sp.c1 = f->cols; }      /* it ends with its row: say so on that row */
    f->m[f->n++] = sp;
    return true;
}

/* the matches in one line of text; false: stop (too many, or a pattern that failed) */
static bool cui__vtf_line(cui__vt_find *f, const cui__regex *rx, int64_t line_row, int len) {
    const char *t = f->text;
    bool word = (f->flags & CUI_FIND_WORD) != 0;
    if (rx) {
        for (int from = 0; from <= len;) {
            int s = 0, e = 0;
            const char *err = NULL;
            int r = cui__regex_find(rx, t, len, from, &s, &e, &err);
            if (r < 0) { f->error = err ? err : "Invalid pattern"; return false; }
            if (r == 0) break;
            if (e > s && (!word || cui__vtf_word_ok(t, len, s, e)) && !cui__vtf_add(f, line_row, s, e)) return false;
            from = e > s ? e : s + 1;
            while (from < len && ((unsigned char)t[from] & 0xC0) == 0x80) from++;
        }
        return true;
    }
    const char *q = f->fq;
    int qn = f->fqn;
    for (int i = 0; i + qn <= len;) {
        const char *h = (const char *)memchr(t + i, q[0], (size_t)(len - qn + 1 - i));
        if (!h) break;
        i = (int)(h - t);
        if (!memcmp(h, q, (size_t)qn) && (!word || cui__vtf_word_ok(t, len, i, i + qn))) {
            if (!cui__vtf_add(f, line_row, i, i + qn)) return false;
            i += qn;
        } else i++;
    }
    return true;
}

static bool cui__vtf_same(const cui__vt_span *a, const cui__vt_span *b) { return a->r0 == b->r0 && a->c0 == b->c0 && a->r1 == b->r1 && a->c1 == b->c1; }
static bool cui__vtf_before_eq(const cui__vt_span *a, int64_t r, int c) { return a->r0 < r || (a->r0 == r && a->c0 <= c); }

/* searches the whole screen for f's query and picks the current match */
static void cui__vtf_search(cui_vt *v, cui__vt_find *f) {
    bool had = f->cur >= 0 && f->cur < f->n;
    cui__vt_span old = had ? f->m[f->cur] : (cui__vt_span){ 0, 0, 0, 0 };
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    f->n = 0; f->truncated = false; f->error = NULL;
    f->wseq = cui__vt_wseq(v); f->cols = in.cols; f->rows = in.rows; f->scr = in.cur; f->sb_total = in.sb_total;
    bool regex = (f->flags & CUI_FIND_REGEX) != 0, fold = !regex && !(f->flags & CUI_FIND_CASE);
    cui__regex *rx = NULL;
    if (f->active && regex) {                                         /* compiled once for every line; an invalid pattern says so even on an empty screen */
        const char *err = NULL;
        rx = cui__regex_new(f->q, (f->flags & CUI_FIND_CASE) ? 0 : CUI_REGEX_ICASE, &err);
        if (!rx) f->error = err ? err : "Invalid pattern";
    }
    if (f->active && !f->error && (regex || f->fqn > 0) && (f->cps || (f->cps = (uint32_t *)malloc(sizeof(uint32_t) * 1024)) != NULL)) {
        f->cps_cap = 1024;
        int64_t r = in.cur == 1 ? (int64_t)in.sb_total : (int64_t)in.sb_total - in.sb_count;
        int64_t end = (int64_t)in.sb_total + in.rows;
        bool go = true;
        while (r < end && go) {
            int64_t line_row = r;
            int len = 0, rel = 0;
            uint32_t last_end = 0;
            for (;;) {                                                /* the rows of one line: a wrapped row goes on in the next */
                bool wrapped = false;
                int n = cui__vt_row_chars(v, r, f->cps, f->cps_cap, &wrapped);
                r++;
                if (n < 0) n = 0;
                while (n > 0 && f->cps[n - 1] == 0) n--;              /* the blank right end */
                if (!cui__vtf_text_room(f, len + n * 4 + 8)) { go = false; break; }
                char *t = f->text;
                uint32_t *pos = f->pos;
                for (int x = 0; x < n; x++) {
                    uint32_t c = f->cps[x];
                    if (c == CUI__VT_FIND_SPACER) continue;
                    if (c == 0) c = ' ';
                    else if (fold) c = cui__vtf_fold(c);
                    uint32_t p = ((uint32_t)rel << CUI__VTF_COLBITS) | (uint32_t)x;
                    int k = c < 0x80 ? (t[len] = (char)c, 1) : cui__vtf_put(c, t + len);
                    for (int j = 0; j < k; j++) pos[len + j] = p;
                    len += k;
                    last_end = p + (x + 1 < n && f->cps[x + 1] == CUI__VT_FIND_SPACER ? 2u : 1u);
                }
                rel++;
                if (!wrapped || r >= end || rel >= (1 << (32 - CUI__VTF_COLBITS)) - 1) break;
            }
            if (!go || len == 0) continue;
            f->pos[len] = last_end;
            go = cui__vtf_line(f, rx, line_row, len);
        }
        if (f->error) f->n = 0;
    }
    cui__regex_free(rx);
    /* the current match: the newest, or the one at (else nearest before) where the last current one was */
    int cur = -1;
    if (f->n > 0) {
        if (!had) cur = f->n - 1;
        else {
            int lo = 0, hi = f->n;                                    /* the first match that starts after the old one */
            while (lo < hi) { int mid = (lo + hi) / 2; if (cui__vtf_before_eq(&f->m[mid], old.r0, old.c0)) lo = mid + 1; else hi = mid; }
            cur = lo > 0 ? lo - 1 : 0;
        }
    }
    if (cur != f->cur || (cur >= 0 && (!had || !cui__vtf_same(&f->m[cur], &old)))) f->seq++;
    f->cur = cur;
}

/* ---- the API ---- */

int cui_vt_find(cui_vt *v, const char *query, int flags) {
    if (!v) return 0;
    cui__vt_find *f = cui__vtf_make(v);
    if (!f) return 0;
    if (!query) query = "";
    size_t n = strlen(query);
    if (n >= sizeof f->q) {                                           /* cut at a whole character */
        n = sizeof f->q - 1;
        while (n > 0 && ((unsigned char)query[n] & 0xC0) == 0x80) n--;
    }
    memcpy(f->q, query, n);
    f->q[n] = 0;
    f->flags = flags & (CUI_FIND_CASE | CUI_FIND_WORD | CUI_FIND_REGEX);
    f->active = n > 0;
    /* the plain query, folded as the text will be */
    bool fold = !(f->flags & (CUI_FIND_CASE | CUI_FIND_REGEX));
    f->fqn = 0;
    for (int i = 0, k; i < (int)n; i += k) {
        uint32_t c = cui__vtf_get(f->q, (int)n, i, &k);
        if (fold) c = cui__vtf_fold(c);
        if (f->fqn + 4 >= (int)sizeof f->fq) break;
        f->fqn += cui__vtf_put(c, f->fq + f->fqn);
    }
    cui__vtf_search(v, f);
    return f->n;
}

cui_vt_find_info cui_vt_find_get(const cui_vt *v) {
    cui_vt_find_info o = { 0, -1, false, NULL };
    const cui__vt_find *f = v ? cui__vt_find_get(v) : NULL;
    if (!f) return o;
    o.count = f->n; o.current = f->cur; o.truncated = f->truncated; o.error = f->error;
    return o;
}

bool cui_vt_find_match(const cui_vt *v, int i, cui_vt_match *out) {
    const cui__vt_find *f = v ? cui__vt_find_get(v) : NULL;
    if (!f || i < 0 || i >= f->n || !out) return false;
    const cui__vt_span *s = &f->m[i];
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    out->row = (int)(s->r0 - (int64_t)in.sb_total); out->col = s->c0;
    out->len = s->r1 == s->r0 ? s->c1 - s->c0 : (f->cols - s->c0) + (int)(s->r1 - s->r0 - 1) * f->cols + s->c1;
    out->end_row = (int)(s->r1 - (int64_t)in.sb_total); out->end_col = s->c1;
    if (out->end_col >= f->cols) { out->end_row++; out->end_col = 0; }
    return true;
}

int cui_vt_find_step(cui_vt *v, int dir) {
    cui__vt_find *f = v ? *cui__vt_find_slot(v) : NULL;
    if (!f || f->n == 0) return -1;
    int cur = f->cur < 0 ? (dir < 0 ? f->n - 1 : 0) : f->cur + (dir < 0 ? -1 : dir > 0 ? 1 : 0);
    f->cur = (cur % f->n + f->n) % f->n;
    f->seq++;
    return f->cur;
}

void cui_vt_find_select(cui_vt *v, int i) {
    cui__vt_find *f = v ? *cui__vt_find_slot(v) : NULL;
    if (!f || i < 0 || i >= f->n) return;
    f->cur = i;
    f->seq++;
}

void cui_vt_find_clear(cui_vt *v) { if (v) cui__vt_find_free(v); }

/* ---- for cui_vt.c ---- */

void cui__vt_find_free(cui_vt *v) {
    cui__vt_find **slot = cui__vt_find_slot(v);
    cui__vt_find *f = *slot;
    if (!f) return;
    free(f->m); free(f->cps); free(f->text); free(f->pos);
    free(f);
    *slot = NULL;
}

size_t cui__vt_find_memory(const cui_vt *v) {
    const cui__vt_find *f = cui__vt_find_get(v);
    if (!f) return 0;
    return sizeof *f + sizeof(cui__vt_span) * (size_t)f->cap + sizeof(uint32_t) * (size_t)f->cps_cap + (size_t)f->tcap * (1 + sizeof(uint32_t));
}

bool cui__vt_find_frame(cui_ctx *ui, cui_vt *v, uint32_t *seen, cui__vt_span *cur) {
    cui__vt_find *f = *cui__vt_find_slot(v);
    if (!f) return false;
    if (f->active) {
        cui__vt_info in;
        cui__vt_get_info(v, &in);
        if (f->wseq != cui__vt_wseq(v) || f->cols != in.cols || f->rows != in.rows || f->scr != in.cur) {   /* the text may have changed */
            double wait = CUI__VTF_REFRESH - (ui->time - f->t_last);
            if (wait <= 0 || wait > CUI__VTF_REFRESH) { cui__vtf_search(v, f); f->t_last = ui->time; }
            else cui_request_wakeup(ui, (float)wait + 0.002f);
        }
    }
    bool moved = *seen != f->seq && f->cur >= 0 && f->cur < f->n;
    *seen = f->seq;
    if (moved) *cur = f->m[f->cur];
    return moved;
}

void cui__vt_find_look_of(cui_ctx *ui, cui__vt_find_look *o) {
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color acc = ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT);   /* as the code editor's find */
    cui_color hot = CUI_C(ui, WARNING);
    o->fill = cui_alpha(acc, dark ? 0.16f : 0.12f);
    o->edge = cui_alpha(acc, dark ? 0.60f : 0.45f);
    o->cur_fill = cui_alpha(hot, dark ? 0.38f : 0.32f);
    o->cur_edge = hot;
}

void cui__vt_find_paint_row(cui_ctx *ui, const cui_vt *v, int64_t arow, float x0, float ry, float cw, float lh, const cui__vt_find_look *k) {
    const cui__vt_find *f = cui__vt_find_get(v);
    if (!f || !f->active || f->n == 0) return;
    int lo = 0, hi = f->n;                                            /* the first match that ends on this row or later */
    while (lo < hi) { int mid = (lo + hi) / 2; if (f->m[mid].r1 < arow) lo = mid + 1; else hi = mid; }
    for (int i = lo; i < f->n && f->m[i].r0 <= arow; i++) {
        const cui__vt_span *s = &f->m[i];
        int a = s->r0 == arow ? s->c0 : 0, b = s->r1 == arow ? s->c1 : f->cols;
        if (b <= a) continue;
        bool cur = i == f->cur;
        cui_draw_rect_border(ui, (cui_rect){ x0 + (float)a * cw - 1, ry + 1, (float)(b - a) * cw + 2, lh - 2 }, CUI_CORNERS(3),
                             cur ? k->cur_fill : k->fill, cur ? k->cur_edge : k->edge, 1);
    }
}
