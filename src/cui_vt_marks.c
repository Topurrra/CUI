/* The VT screen's command marks (OSC 133 and 633), progress (OSC 9;4) and copy requests (OSC 52), opt in with
 * CUI_VT_FEATURE_MARKS (cui_vt_set_features).
 *
 * A mark is four cells of the main screen, A (prompt), B (command), C (output) and D (end), each a row counted from the first
 * scrollback line ever (sb_total + y, as the view counts rows) and a column. Rows that scroll into the scrollback keep their
 * number, so the common case (a line feed at the bottom of a full screen) costs nothing here: cui_vt.c calls in only when rows
 * move otherwise (a scroll region, inserted or deleted lines, a line that leaves the scrollback, a resize, a reflow). Marks
 * are kept oldest first and in order of their prompt rows (a prompt drawn above earlier ones drops them), so lookups by row are
 * binary searches. Nothing is allocated before the feature is on; the mark array grows with the first mark.
 *
 * Windows Terminal (github.com/microsoft/terminal, MIT; read for behaviour, no code copied) is the reference for the sequences:
 * FinalTerm's OSC 133 A/B/C/D[;exit], VS Code's OSC 633 with E;<command line> (\\ and \xHH escapes), ConEmu's OSC 9;4;st;pr
 * and xterm's OSC 52;Pc;base64. */
#include "cui_internal.h"

#include <limits.h>
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

#define CUI__VTM_NONE INT64_MIN
#define CUI__VTM_MAX 10000                    /* marks kept; past it the oldest go */
#define CUI__VTM_CLIP_MAX ((size_t)1 << 20)   /* an OSC 52 request decoded past this is dropped whole */
#define CUI__VTM_CMD_MAX 4096                 /* a 633;E command line kept */
enum { CUI__VTM_A, CUI__VTM_B, CUI__VTM_C, CUI__VTM_D };

typedef struct {
    int64_t row[4];                   /* A, B, C, D: sb_total + y of the main screen; CUI__VTM_NONE: not come */
    int32_t col[4];
    int32_t brow[4]; int16_t bcol[4]; /* REFLOW: the same cells in the base the next reflow starts from (cui__vt_mk_rf_capture), brow -1 none */
    uint32_t id;                      /* grows with every mark: marks are in id order too */
    int exit_code;
    bool finished;
    double start_ms, end_ms;
    char *cmd;                        /* 633;E's command line, decoded; NULL none */
} cui__vtm;

struct cui__vt_mk {
    cui__vtm *m; int n, cap;
    unsigned seq;                     /* cui_vt_mark_seq */
    uint32_t next_id;
    uint32_t sel_id; bool sel_any;    /* cui_vt_mark_select's mark (by id: indices move when old marks go) */
    unsigned sel_req; int sel_what;   /* counts the calls, so a view knows a new one came */
    int prog, pct;                    /* OSC 9;4 */
    char *clip; size_t clip_n;        /* the newest OSC 52 request, decoded */
    unsigned char *cb; size_t cb_n, cb_cap;   /* OSC 52 being read */
    uint32_t acc; int bits, phase, pc_n;
    bool bad, over, query, any, padded;
    bool busy;                        /* a resize is moving the rows: lines leaving the scrollback meanwhile are not trimmed yet */
};

double cui_vt_now_ms(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------------ */
/* The feature                                                              */
/* ------------------------------------------------------------------------ */

void cui__vt_mk_free(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    for (int i = 0; i < k->n; i++) free(k->m[i].cmd);
    free(k->m); free(k->clip); free(k->cb);
    free(k);
    cui__vt_set_mk(v, NULL);
}

void cui__vt_mk_feature(cui_vt *v, bool on) {
    if (!on) { cui__vt_mk_free(v); return; }
    if (cui__vt_mk_get(v)) return;
    cui__vt_mk *k = (cui__vt_mk *)calloc(1, sizeof *k);
    if (k) k->next_id = 1;
    cui__vt_set_mk(v, k);
}

size_t cui__vt_mk_memory(const cui_vt *v) {
    const cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return 0;
    size_t n = sizeof *k + (size_t)k->cap * sizeof(cui__vtm) + k->cb_cap + (k->clip ? k->clip_n + 1 : 0);
    for (int i = 0; i < k->n; i++) if (k->m[i].cmd) n += strlen(k->m[i].cmd) + 1;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Keeping the marks                                                        */
/* ------------------------------------------------------------------------ */

static void cui__vtm_drop(cui__vt_mk *k, int i, int count) {
    if (count <= 0) return;
    for (int j = i; j < i + count; j++) free(k->m[j].cmd);
    memmove(&k->m[i], &k->m[i + count], sizeof *k->m * (size_t)(k->n - i - count));
    k->n -= count;
    k->seq++;
}

/* marks whose prompt row was set to CUI__VTM_NONE (destroyed) go */
static void cui__vtm_compact(cui__vt_mk *k) {
    int w = 0;
    for (int i = 0; i < k->n; i++) {
        if (k->m[i].row[CUI__VTM_A] == CUI__VTM_NONE) { free(k->m[i].cmd); continue; }
        if (w != i) k->m[w] = k->m[i];
        w++;
    }
    if (w != k->n) { k->n = w; k->seq++; }
}

/* the oldest row the scrollback still holds */
static int64_t cui__vtm_oldest(const cui__vt_info *I) { return (int64_t)I->sb_total - I->sb_count; }

void cui__vt_mk_trim(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n || k->busy) return;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t oldest = cui__vtm_oldest(&I);
    int i = 0;                                 /* the oldest first: their later cells come after the prompt, so they stay when it does */
    while (i < k->n && k->m[i].row[CUI__VTM_A] < oldest) i++;
    cui__vtm_drop(k, 0, i);
}

void cui__vt_mk_reset(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    k->prog = 0; k->pct = 0;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int i = 0;                                 /* the screen's text is gone: its marks go, the scrollback's stay */
    while (i < k->n && k->m[i].row[CUI__VTM_A] < (int64_t)I.sb_total) i++;
    cui__vtm_drop(k, i, k->n - i);
}

/* Rows top..bot of the main screen move by n: how 1 up with the top row going into the scrollback (and bot above the screen's
 * last row: the rows below keep their place, so their numbers grow), 0 up with the top n rows lost, -1 down with the bottom n
 * rows lost. A mark whose prompt is lost goes; its other cells stop at the region's edge. */
void cui__vt_mk_scroll(cui_vt *v, int top, int bot, int n, int how) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n || n <= 0) return;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t t = (int64_t)I.sb_total;
    n = CUI_MIN(n, bot - top + 1);
    bool lost = false;
    for (int i = 0; i < k->n; i++) {
        cui__vtm *m = &k->m[i];
        for (int p = 0; p < 4; p++) {
            if (m->row[p] == CUI__VTM_NONE || m->row[p] < t) continue;
            int y = (int)(m->row[p] - t);
            if (how == 1) { if (y > bot) m->row[p] += n; continue; }
            if (y < top || y > bot) continue;
            int ny = how == 0 ? y - n : y + n;
            if (ny < top || ny > bot) {
                if (p == CUI__VTM_A) { m->row[p] = CUI__VTM_NONE; lost = true; break; }
                ny = how == 0 ? top : bot;
                m->col[p] = 0;
            }
            m->row[p] = t + ny;
        }
    }
    if (lost) cui__vtm_compact(k);
    k->seq++;
}

/* a resize that cut the main screen (no REFLOW, or pictures on it): rows stay, `drop` of them went up into the scrollback
 * (t0: sb_total before) */
void cui__vt_mk_cut(cui_vt *v, int64_t t0, int drop) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n) return;
    cui__vt_info I; cui__vt_get_info(v, &I);
    for (int i = 0; i < k->n; i++)
        for (int p = 0; p < 4; p++) {
            cui__vtm *m = &k->m[i];
            if (m->row[p] == CUI__VTM_NONE) continue;
            m->brow[p] = -1;
            if (m->row[p] < t0) continue;
            int y = (int)(m->row[p] - t0) - drop;
            if (y > I.rows - 1) { y = I.rows - 1; m->col[p] = 0; }
            m->row[p] = (int64_t)I.sb_total + y;
            if (m->col[p] > I.cols - 1) m->col[p] = I.cols - 1;
        }
    k->seq++;
    cui__vt_mk_trim(v);
}

/* ------------------------------------------------------------------------ */
/* Reflow                                                                   */
/* ------------------------------------------------------------------------ */

void cui__vt_mk_rf_begin(cui_vt *v) { cui__vt_mk *k = cui__vt_mk_get(v); if (k) k->busy = true; }

void cui__vt_mk_rf_capture(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    cui__vt_info I; cui__vt_get_info(v, &I);
    for (int i = 0; i < k->n; i++)
        for (int p = 0; p < 4; p++) {
            cui__vtm *m = &k->m[i];
            bool on = m->row[p] != CUI__VTM_NONE && m->row[p] >= (int64_t)I.sb_total && m->row[p] < (int64_t)I.sb_total + I.rows;
            m->brow[p] = on ? (int32_t)(m->row[p] - (int64_t)I.sb_total) : -1;
            m->bcol[p] = (int16_t)(on ? CUI_CLAMP(m->col[p], 0, I.cols - 1) : 0);
        }
}

static int cui__vtm_pt_cmp(const void *a, const void *b) {
    const cui__vt_rfpt *x = (const cui__vt_rfpt *)a, *y = (const cui__vt_rfpt *)b;
    if (x->r != y->r) return x->r < y->r ? -1 : 1;
    return x->c < y->c ? -1 : x->c > y->c;
}

/* Which cells follow which pass: those on the base (use_base) are laid out again from it, those in the scrollback (from row
 * sb_first, after the lines made from the base were taken back) follow its reflow, the rest of the screen's (t0: sb_total
 * before the resize) is cut. Each list is sorted by source row and column. */
bool cui__vt_mk_rf_plan(cui_vt *v, bool use_base, int64_t t0, cui__vt_mk_plan *pl) {
    memset(pl, 0, sizeof *pl);
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n) return false;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t first = cui__vtm_oldest(&I), end = (int64_t)I.sb_total;
    size_t cap = (size_t)k->n * 4;
    pl->sb = (cui__vt_rfpt *)malloc(sizeof *pl->sb * cap);
    pl->base = (cui__vt_rfpt *)malloc(sizeof *pl->base * cap);
    pl->cut = (cui__vt_rfpt *)malloc(sizeof *pl->cut * cap);
    if (!pl->sb || !pl->base || !pl->cut) { free(pl->sb); free(pl->base); free(pl->cut); memset(pl, 0, sizeof *pl); return false; }
    for (int i = 0; i < k->n; i++)
        for (int p = 0; p < 4; p++) {
            cui__vtm *m = &k->m[i];
            if (m->row[p] == CUI__VTM_NONE) continue;
            cui__vt_rfpt q = { 0, m->col[p], 0, 0, i, p, false };
            if (use_base && m->brow[p] >= 0) { q.r = m->brow[p]; q.c = m->bcol[p]; pl->base[pl->nbase++] = q; }
            else if (m->row[p] >= t0) { q.r = m->row[p] - t0; pl->cut[pl->ncut++] = q; }
            else if (m->row[p] >= first && m->row[p] < end) { q.r = m->row[p] - first; pl->sb[pl->nsb++] = q; }
            else { m->row[p] = first - 1; m->col[p] = 0; }       /* in lines that went: the trim takes it */
        }
    qsort(pl->sb, (size_t)pl->nsb, sizeof *pl->sb, cui__vtm_pt_cmp);
    qsort(pl->base, (size_t)pl->nbase, sizeof *pl->base, cui__vtm_pt_cmp);
    pl->sb_first = first;
    return true;
}

/* the cells a pass placed (hit) take their new rows: row0 + the row made, cut to the width */
static void cui__vtm_rf_apply(cui__vt_mk *k, cui__vt_rfpt *pts, int n, int64_t row0, int cols, int rows_max) {
    for (int i = 0; i < n; i++) {
        cui__vt_rfpt *q = &pts[i];
        cui__vtm *m = &k->m[q->mi];
        if (!q->hit) continue;
        int64_t r = q->orow;
        if (rows_max >= 0 && r > rows_max) r = rows_max;
        m->row[q->k] = row0 + r;
        m->col[q->k] = CUI_CLAMP(q->ocol, 0, cols - 1);
    }
}

/* the end of a reflowed resize: sb_ok the scrollback was wrapped again (its cells were placed); main_done the base was laid
 * out (its cells were placed, p rows of it went into the scrollback); else the screen was cut with `drop` rows going up */
void cui__vt_mk_rf_end(cui_vt *v, cui__vt_mk_plan *pl, bool sb_ok, bool main_done, int p, int drop) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    k->busy = false;
    if (pl->sb || pl->base || pl->cut) {
        cui__vt_info I; cui__vt_get_info(v, &I);
        if (sb_ok) cui__vtm_rf_apply(k, pl->sb, pl->nsb, pl->sb_first, I.cols, -1);
        int64_t t = (int64_t)I.sb_total;
        if (main_done) cui__vtm_rf_apply(k, pl->base, pl->nbase, t - p, I.cols, p + I.rows - 1);    /* row r of the base laid out is sb_total - p + r */
        for (int pass = 0; pass < 2; pass++) {     /* cut: the screen's cells (and the base's, when it could not be laid out) stay on their rows */
            cui__vt_rfpt *q = pass ? pl->cut : pl->base;
            int n = pass ? pl->ncut : (main_done ? 0 : pl->nbase);
            for (int i = 0; i < n; i++) {
                cui__vtm *m = &k->m[q[i].mi];
                int y = (int)q[i].r - drop;
                int c = q[i].c;
                if (y > I.rows - 1) { y = I.rows - 1; c = 0; }
                m->row[q[i].k] = t + y;
                m->col[q[i].k] = CUI_CLAMP(c, 0, I.cols - 1);
            }
        }
        for (int i = 0; i < k->n; i++)           /* a reflow can put B before A when A was on a dropped blank: keep the order of the cells */
            for (int q = 1; q < 4; q++) {
                cui__vtm *m = &k->m[i];
                if (m->row[q] != CUI__VTM_NONE && (m->row[q] < m->row[0] || (m->row[q] == m->row[0] && m->col[q] < m->col[0]))) { m->row[q] = m->row[0]; m->col[q] = m->col[0]; }
            }
        k->seq++;
    }
    free(pl->sb); free(pl->base); free(pl->cut);
    memset(pl, 0, sizeof *pl);
    cui__vt_mk_trim(v);
}

/* ------------------------------------------------------------------------ */
/* OSC 133, 633 and 9;4                                                     */
/* ------------------------------------------------------------------------ */

static void cui__vtm_here(const cui_vt *v, int64_t *row, int *col) {
    cui__vt_info I; cui__vt_get_info(v, &I);
    *row = (int64_t)I.sb_total + I.cy;
    *col = I.cx;
}

static bool cui__vtm_before(int64_t r0, int c0, int64_t r1, int c1) { return r0 < r1 || (r0 == r1 && c0 < c1); }

/* a new mark at (row, col): marks whose prompt is at or after it go first (the screen was cleared or drawn again) */
static cui__vtm *cui__vtm_new(cui__vt_mk *k, int64_t row, int col) {
    int i = k->n;
    while (i > 0 && !cui__vtm_before(k->m[i - 1].row[CUI__VTM_A], k->m[i - 1].col[CUI__VTM_A], row, col)) i--;
    cui__vtm_drop(k, i, k->n - i);
    if (k->n > 0 && !k->m[k->n - 1].finished) {     /* a command whose D never came ends at this prompt */
        cui__vtm *o = &k->m[k->n - 1];
        o->finished = true; o->exit_code = -1; o->end_ms = cui_vt_now_ms();
        o->row[CUI__VTM_D] = row; o->col[CUI__VTM_D] = col;
    }
    if (k->n == CUI__VTM_MAX) cui__vtm_drop(k, 0, 1);
    if (k->n == k->cap) {
        int cap = k->cap ? k->cap * 2 : 16;
        if (cap > CUI__VTM_MAX) cap = CUI__VTM_MAX;
        cui__vtm *nm = (cui__vtm *)realloc(k->m, sizeof *nm * (size_t)cap);
        if (!nm) return NULL;
        k->m = nm; k->cap = cap;
    }
    cui__vtm *m = &k->m[k->n++];
    memset(m, 0, sizeof *m);
    for (int p = 0; p < 4; p++) { m->row[p] = CUI__VTM_NONE; m->brow[p] = -1; }
    m->row[CUI__VTM_A] = row; m->col[CUI__VTM_A] = col;
    m->id = k->next_id++;
    m->exit_code = -1;
    k->seq++;
    return m;
}

static cui__vtm *cui__vtm_open(cui__vt_mk *k) { return k->n > 0 && !k->m[k->n - 1].finished ? &k->m[k->n - 1] : NULL; }

/* 633;E's text: \\ is a backslash, \xHH a byte; control characters but line breaks and tabs become spaces */
static char *cui__vtm_unescape(const char *s, int n) {
    if (n > CUI__VTM_CMD_MAX) n = CUI__VTM_CMD_MAX;
    char *o = (char *)malloc((size_t)n + 1);
    if (!o) return NULL;
    int k = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n && s[i + 1] == '\\') { c = '\\'; i++; }
        else if (c == '\\' && i + 3 < n && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
            int h = 0, ok = 1;
            for (int j = 2; j <= 3; j++) {
                char d = s[i + j];
                int x = d >= '0' && d <= '9' ? d - '0' : (d | 32) >= 'a' && (d | 32) <= 'f' ? (d | 32) - 'a' + 10 : -1;
                if (x < 0) ok = 0; else h = h * 16 + x;
            }
            if (ok) { c = (unsigned char)h; i += 3; }
        }
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) c = ' ';
        o[k++] = (char)c;
    }
    o[k] = 0;
    return o;
}

static void cui__vtm_progress(cui__vt_mk *k, const char *a, int n) {
    int i = 0, st = 0, pr = -1;
    if (i >= n || a[i] < '0' || a[i] > '9') return;
    while (i < n && a[i] >= '0' && a[i] <= '9' && st < 100) st = st * 10 + (a[i++] - '0');
    if (st > 4) return;
    if (i < n && a[i] == ';') {
        i++;
        if (i < n && a[i] >= '0' && a[i] <= '9') { pr = 0; while (i < n && a[i] >= '0' && a[i] <= '9' && pr < 1000) pr = pr * 10 + (a[i++] - '0'); }
    }
    k->prog = st;
    if (st == 0) k->pct = 0;
    else if (pr >= 0) k->pct = CUI_MIN(pr, 100);
}

void cui__vt_mk_osc(cui_vt *v, int code, const char *a, int n) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    if (code == 9) { if (n >= 2 && a[0] == '4' && a[1] == ';') cui__vtm_progress(k, a + 2, n - 2); return; }
    if (code != 133 && code != 633) return;
    if (n < 1 || (n > 1 && a[1] != ';')) return;           /* one letter, then the end or its parameters */
    cui__vt_info I; cui__vt_get_info(v, &I);
    if (I.cur != 0) return;                                 /* the alternate screen keeps no marks */
    int64_t row; int col;
    cui__vtm_here(v, &row, &col);
    cui__vtm *m = cui__vtm_open(k);
    switch (a[0]) {
    case 'A':
        if (m && m->row[CUI__VTM_B] == CUI__VTM_NONE && m->row[CUI__VTM_C] == CUI__VTM_NONE && !cui__vtm_before(row, col, m->row[CUI__VTM_A], m->col[CUI__VTM_A])) {
            m->row[CUI__VTM_A] = row; m->col[CUI__VTM_A] = col; k->seq++;      /* the prompt is drawn again before it ended: it moves */
        } else cui__vtm_new(k, row, col);
        break;
    case 'B':
        if (!m || m->row[CUI__VTM_C] != CUI__VTM_NONE) m = cui__vtm_new(k, row, col);
        if (!m) break;
        m->row[CUI__VTM_B] = row; m->col[CUI__VTM_B] = col;
        m->start_ms = cui_vt_now_ms();
        k->seq++;
        break;
    case 'C':
        if (m && m->row[CUI__VTM_C] != CUI__VTM_NONE) break;      /* a second C: the first stands */
        if (!m) { m = cui__vtm_new(k, row, col); if (!m) break; m->row[CUI__VTM_B] = row; m->col[CUI__VTM_B] = col; }
        m->row[CUI__VTM_C] = row; m->col[CUI__VTM_C] = col;
        m->start_ms = cui_vt_now_ms();
        k->seq++;
        break;
    case 'D': {
        if (!m) break;
        int code_ = -1;
        if (n > 2) {
            int i = 2, s = 1, x = 0; bool dig = false;
            if (a[i] == '-') { s = -1; i++; }
            while (i < n && a[i] >= '0' && a[i] <= '9' && x < 100000000) { x = x * 10 + (a[i++] - '0'); dig = true; }
            if (dig && (i == n || a[i] == ';')) code_ = s * x;
        }
        m->row[CUI__VTM_D] = row; m->col[CUI__VTM_D] = col;
        m->exit_code = code_; m->finished = true; m->end_ms = cui_vt_now_ms();
        k->seq++;
        break;
    }
    case 'E':
        if (!m || n < 2) break;
        free(m->cmd);
        m->cmd = cui__vtm_unescape(a + 2, n - 2);
        k->seq++;
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------ */
/* OSC 52                                                                   */
/* ------------------------------------------------------------------------ */

void cui__vt_mk_c52_begin(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    k->cb_n = 0; k->acc = 0; k->bits = 0; k->phase = 0; k->pc_n = 0;
    k->bad = k->over = k->query = k->any = k->padded = false;
}

static int cui__vtm_b64(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t cui__vt_mk_c52_feed(cui_vt *v, const unsigned char *d, size_t n) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    size_t i = 0;
    for (; i < n; i++) {
        unsigned char c = d[i];
        if (c == 0x07 || c == 0x1B) break;
        if (!k || k->bad || k->over || k->query) continue;
        if (k->phase == 0) {                                   /* Pc: which clipboard; any is the clipboard */
            if (c == ';') k->phase = 1;
            else if (++k->pc_n > 16) k->bad = true;
            continue;
        }
        if (!k->any && c == '?') { k->query = true; continue; }
        k->any = true;
        if (c == '=') { k->padded = true; continue; }
        if (c == '\r' || c == '\n' || c == ' ') continue;
        int x = cui__vtm_b64(c);
        if (x < 0 || k->padded) { k->bad = true; continue; }
        k->acc = (k->acc << 6) | (uint32_t)x; k->bits += 6;
        if (k->bits < 8) continue;
        k->bits -= 8;
        if (k->cb_n >= CUI__VTM_CLIP_MAX) { k->over = true; free(k->cb); k->cb = NULL; k->cb_cap = 0; k->cb_n = 0; continue; }
        if (k->cb_n + 1 >= k->cb_cap) {
            size_t cap = k->cb_cap ? k->cb_cap * 2 : 256;
            if (cap > CUI__VTM_CLIP_MAX + 1) cap = CUI__VTM_CLIP_MAX + 1;
            unsigned char *nb = (unsigned char *)realloc(k->cb, cap);
            if (!nb) { k->bad = true; continue; }
            k->cb = nb; k->cb_cap = cap;
        }
        k->cb[k->cb_n++] = (unsigned char)((k->acc >> k->bits) & 0xFF);
    }
    return i;
}

static bool cui__vtm_utf8_ok(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c == 0) return false;
        if (c < 0x80) { i++; continue; }
        int need = c >= 0xF0 && c <= 0xF4 ? 3 : c >= 0xE0 && c < 0xF0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : -1;
        if (need < 0 || i + (size_t)need >= n) return false;
        for (int j = 1; j <= need; j++) if ((s[i + (size_t)j] & 0xC0) != 0x80) return false;
        i += (size_t)need + 1;
    }
    return true;
}

void cui__vt_mk_c52_end(cui_vt *v) {
    cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k) return;
    bool ok = k->phase == 1 && !k->bad && !k->over && !k->query && k->cb_n > 0 && cui__vtm_utf8_ok(k->cb, k->cb_n);
    if (ok) {
        k->cb[k->cb_n] = 0;
        free(k->clip);
        k->clip = (char *)k->cb; k->clip_n = k->cb_n;
        k->cb = NULL; k->cb_cap = 0;
    } else if (k->cb_cap > 4096) { free(k->cb); k->cb = NULL; k->cb_cap = 0; }
    k->cb_n = 0;
}

size_t cui_vt_take_clipboard(cui_vt *v, char *out, size_t cap) {
    cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    if (out && cap) out[0] = 0;
    if (!k || !k->clip) return 0;
    size_t n = k->clip_n;
    if (out && cap > n) {
        memcpy(out, k->clip, n + 1);
        free(k->clip); k->clip = NULL; k->clip_n = 0;
    }
    return n;
}

int cui_vt_progress(const cui_vt *v, int *percent) {
    const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    if (percent) *percent = k ? k->pct : 0;
    return k ? k->prog : CUI_VT_PROGRESS_NONE;
}

/* ------------------------------------------------------------------------ */
/* Reading the marks                                                        */
/* ------------------------------------------------------------------------ */

int cui_vt_mark_count(const cui_vt *v) { const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL; return k ? k->n : 0; }
unsigned cui_vt_mark_seq(const cui_vt *v) { const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL; return k ? k->seq : 0; }

static const cui__vtm *cui__vtm_at(const cui_vt *v, int i) {
    const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    return k && i >= 0 && i < k->n ? &k->m[i] : NULL;
}

static int cui__vtm_row(int64_t r, int64_t t) {
    if (r == CUI__VTM_NONE) return INT_MIN;
    int64_t d = r - t;
    return (int)CUI_CLAMP(d, (int64_t)INT_MIN + 1, (int64_t)INT_MAX);
}

bool cui_vt_mark_get(const cui_vt *v, int i, cui_vt_mark *out) {
    const cui__vtm *m = cui__vtm_at(v, i);
    if (!m || !out) return false;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t t = (int64_t)I.sb_total;
    out->prompt_row = cui__vtm_row(m->row[CUI__VTM_A], t);
    out->cmd_row = m->row[CUI__VTM_B] != CUI__VTM_NONE ? cui__vtm_row(m->row[CUI__VTM_B], t) : out->prompt_row;
    out->out_row = cui__vtm_row(m->row[CUI__VTM_C], t);
    out->end_row = m->finished ? cui__vtm_row(m->row[CUI__VTM_D], t) : INT_MIN;
    out->exit_code = m->finished ? m->exit_code : -1;
    out->finished = m->finished;
    out->start_ms = m->start_ms;
    out->end_ms = m->end_ms;
    return true;
}

/* the last mark whose prompt row is at most `row` (strict: below it), -1 none */
static int cui__vtm_last_before(const cui__vt_mk *k, int64_t row, bool strict) {
    int lo = 0, hi = k->n;                    /* the first mark past it */
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int64_t r = k->m[mid].row[CUI__VTM_A];
        if (strict ? r < row : r <= row) lo = mid + 1; else hi = mid;
    }
    return lo - 1;
}

int cui_vt_mark_at_row(const cui_vt *v, int row) {
    const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    if (!k || !k->n) return -1;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t a = (int64_t)I.sb_total + row;
    int i = cui__vtm_last_before(k, a, false);
    if (i < 0) return -1;
    const cui__vtm *m = &k->m[i];
    if (!m->finished) return i;
    int64_t end = m->row[CUI__VTM_D] + (m->col[CUI__VTM_D] > 0 ? 1 : 0);
    if (end <= m->row[CUI__VTM_A]) end = m->row[CUI__VTM_A] + 1;
    return a < end ? i : -1;
}

int cui_vt_mark_near(const cui_vt *v, int row, int dir) {
    const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    if (!k || !k->n || dir == 0) return -1;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int64_t a = (int64_t)I.sb_total + row;
    if (dir < 0) return cui__vtm_last_before(k, a, true);
    int i = cui__vtm_last_before(k, a, false) + 1;
    return i < k->n ? i : -1;
}

static int cui__vtm_index_of(const cui__vt_mk *k, uint32_t id) {
    int lo = 0, hi = k->n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (k->m[mid].id == id) return mid;
        if (k->m[mid].id < id) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

int cui_vt_mark_selected(const cui_vt *v) {
    const cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    return k && k->sel_any ? cui__vtm_index_of(k, k->sel_id) : -1;
}

void cui_vt_mark_select(cui_vt *v, int i, int what) {
    cui__vt_mk *k = v ? cui__vt_mk_get(v) : NULL;
    if (!k || i < 0 || i >= k->n) return;
    k->sel_id = k->m[i].id; k->sel_any = true;
    k->sel_what = what; k->sel_req++;
}

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

/* text written as snprintf writes: the whole length is counted, whole characters are written while they fit */
typedef struct { char *out; size_t cap, len, w; bool full; int nl, sp; bool started; } cui__vtm_tb;   /* w: bytes written */

static void cui__vtm_put(cui__vtm_tb *b, const char *s, size_t n) {
    if (!b->full && b->out && b->len + n < b->cap) { memcpy(b->out + b->len, s, n); b->w = b->len + n; }
    else b->full = true;
    b->len += n;
}

static size_t cui__vtm_done(cui__vtm_tb *b) {
    if (b->out && b->cap) b->out[b->w] = 0;
    return b->len;
}

/* a character of a line: the line breaks and spaces waiting before it go first (so blanks at line ends and blank lines at
 * the start and the end are never written) */
static void cui__vtm_char(cui__vtm_tb *b, uint32_t c) {
    if (c == ' ') { b->sp++; return; }
    for (; b->nl > 0; b->nl--) cui__vtm_put(b, "\n", 1);
    for (; b->sp > 0; b->sp--) cui__vtm_put(b, " ", 1);
    char u[4]; int k = cui__utf8_encode(c, u);
    cui__vtm_put(b, u, (size_t)k);
    b->started = true;
}

static void cui__vtm_eol(cui__vtm_tb *b) { b->sp = 0; if (b->started) b->nl++; }

/* the main screen's cells from (r0, c0) up to (r1, c1), not included: rows a wrap joined are one line */
static void cui__vtm_span_text(const cui_vt *v, cui__vtm_tb *b, int64_t r0, int c0, int64_t r1, int c1, bool lead_spaces) {
    uint32_t ch[1000];
    for (int64_t r = r0; r <= r1; r++) {
        if (r == r1 && c1 <= 0) break;
        bool wrapped = false;
        int n = cui__vt_main_chars(v, r, ch, 1000, &wrapped);
        if (n < 0) { cui__vtm_eol(b); continue; }
        int x0 = r == r0 ? c0 : 0, x1 = r == r1 ? CUI_MIN(c1, n) : n;
        if (!lead_spaces && !b->started) b->sp = 0;
        for (int x = x0; x < x1; x++) {
            uint32_t c = ch[x];
            if (c == CUI__VT_FIND_SPACER) continue;
            if (c == 0) { if (wrapped && r != r1) { bool rest = false; for (int y = x + 1; y < n; y++) if (ch[y] && ch[y] != CUI__VT_FIND_SPACER) { rest = true; break; } if (!rest) break; } c = ' '; }
            if (!lead_spaces && !b->started && c == ' ') continue;
            cui__vtm_char(b, c);
        }
        if (!(wrapped && r != r1)) cui__vtm_eol(b);
    }
}

/* where the line holding row r ends: the row after its last wrapped row */
static int64_t cui__vtm_line_end(const cui_vt *v, int64_t r) {
    uint32_t ch[1];
    for (int guard = 0; guard < 100000; guard++) {
        bool w = false;
        if (cui__vt_main_chars(v, r, ch, 0, &w) < 0 || !w) return r + 1;
        r++;
    }
    return r + 1;
}

/* the output's cells: C (or the line after the command's) up to D (or the cursor; the screen's end behind the alternate one) */
static bool cui__vtm_out_span(const cui_vt *v, const cui__vtm *m, int64_t *r0, int *c0, int64_t *r1, int *c1) {
    cui__vt_info I; cui__vt_get_info(v, &I);
    if (m->row[CUI__VTM_C] != CUI__VTM_NONE) { *r0 = m->row[CUI__VTM_C]; *c0 = m->col[CUI__VTM_C]; }
    else { int p = m->row[CUI__VTM_B] != CUI__VTM_NONE ? CUI__VTM_B : CUI__VTM_A; *r0 = cui__vtm_line_end(v, m->row[p]); *c0 = 0; }
    if (m->finished) { *r1 = m->row[CUI__VTM_D]; *c1 = m->col[CUI__VTM_D]; }
    else if (I.cur == 0) { *r1 = (int64_t)I.sb_total + I.cy; *c1 = I.cx; }
    else { *r1 = (int64_t)I.sb_total + I.rows; *c1 = 0; }
    return cui__vtm_before(*r0, *c0, *r1, *c1);
}

size_t cui_vt_mark_output(const cui_vt *v, int i, char *out, size_t cap) {
    cui__vtm_tb b = { out, cap, 0, 0, false, 0, 0, false };
    const cui__vtm *m = cui__vtm_at(v, i);
    int64_t r0, r1; int c0, c1;
    if (m && cui__vtm_out_span(v, m, &r0, &c0, &r1, &c1)) cui__vtm_span_text(v, &b, r0, c0, r1, c1, true);
    return cui__vtm_done(&b);
}

/* 633;E's text, else B (or A) up to C, or to the end of B's line without C */
size_t cui_vt_mark_command(const cui_vt *v, int i, char *out, size_t cap) {
    cui__vtm_tb b = { out, cap, 0, 0, false, 0, 0, false };
    const cui__vtm *m = cui__vtm_at(v, i);
    if (!m) return cui__vtm_done(&b);
    if (m->cmd) {
        const char *s = m->cmd;
        while (*s == ' ' || *s == '\t' || *s == '\n') s++;
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n')) n--;
        for (size_t k = 0; k < n;) {           /* whole characters */
            unsigned char c = (unsigned char)s[k];
            size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            if (k + len > n) len = n - k;
            cui__vtm_put(&b, s + k, len);
            k += len;
        }
        return cui__vtm_done(&b);
    }
    int p = m->row[CUI__VTM_B] != CUI__VTM_NONE ? CUI__VTM_B : CUI__VTM_A;
    int64_t r0 = m->row[p], r1; int c0 = m->col[p], c1;
    if (m->row[CUI__VTM_C] != CUI__VTM_NONE && cui__vtm_before(r0, c0, m->row[CUI__VTM_C], m->col[CUI__VTM_C])) { r1 = m->row[CUI__VTM_C]; c1 = m->col[CUI__VTM_C]; }
    else { r1 = cui__vtm_line_end(v, r0); c1 = 0; }
    if (m->finished && cui__vtm_before(m->row[CUI__VTM_D], m->col[CUI__VTM_D], r1, c1) && cui__vtm_before(r0, c0, m->row[CUI__VTM_D], m->col[CUI__VTM_D])) { r1 = m->row[CUI__VTM_D]; c1 = m->col[CUI__VTM_D]; }
    cui__vtm_span_text(v, &b, r0, c0, r1, c1, false);
    return cui__vtm_done(&b);
}

/* ------------------------------------------------------------------------ */
/* The view (.marks)                                                        */
/* ------------------------------------------------------------------------ */

/* A cui_vt_mark_select the view has not shown yet (*seen is the request it showed last): the mark's prompt row and, when its
 * output is to be selected and is not empty, the output's cells (r1:c1 not included). */
bool cui__vt_mk_view(const cui_vt *v, unsigned *seen, int64_t *prompt, bool *sel, cui__vt_span *out) {
    const cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || *seen == k->sel_req) return false;
    *seen = k->sel_req;
    int i = k->sel_any ? cui__vtm_index_of(k, k->sel_id) : -1;
    if (i < 0) return false;
    const cui__vtm *m = &k->m[i];
    *prompt = m->row[CUI__VTM_A];
    *sel = false;
    if (k->sel_what == CUI_VT_MARK_SELECT_OUTPUT) {
        int64_t r0, r1; int c0, c1;
        if (cui__vtm_out_span(v, m, &r0, &c0, &r1, &c1)) {
            out->r0 = r0; out->c0 = c0; out->r1 = r1; out->c1 = c1;
            *sel = true;
        }
    }
    return true;
}

/* the tick of mark row `row` on a track of height h: rows spread over the scrollback and the screen */
static float cui__vtm_tick_y(const cui__vt_info *I, cui_rect r, int64_t row) {
    double total = (double)I->sb_count + I->rows, at = (double)(row - cui__vtm_oldest(I)) + 0.5;
    if (at < 0) at = 0;
    if (at > total) at = total;
    return r.y + (float)(at / total) * r.h;
}

#define CUI__VTM_TICK_W 7.0f                 /* points from the view's right edge less 2 */

void cui__vt_mk_paint(cui_ctx *ui, const cui_vt *v, cui_rect r) {
    const cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n) return;
    cui__vt_info I; cui__vt_get_info(v, &I);
    float s = ui->scale > 0 ? ui->scale : 1, th = fmaxf(1.0f, floorf(2.0f * s + 0.5f)) / s;
    float x1 = r.x + r.w - 2, x0 = x1 - CUI__VTM_TICK_W;
    cui_color plain = cui_alpha(CUI_C(ui, TEXT_SECONDARY), 0.85f), bad = CUI_C(ui, DANGER), acc = cui__emphasis(ui);
    int sel = k->sel_any ? cui__vtm_index_of(k, k->sel_id) : -1;
    int last_py = INT_MIN, pri = 0;           /* ticks on one device row are one: the selected wins, then a failed one */
    for (int i = 0; i <= k->n; i++) {
        int py = INT_MAX, p = 0;
        if (i < k->n) {
            const cui__vtm *m = &k->m[i];
            py = (int)floorf(cui__vtm_tick_y(&I, r, m->row[CUI__VTM_A]) * s);
            p = i == sel ? 3 : (m->finished && m->exit_code != 0 && m->exit_code != -1) ? 2 : 1;
        }
        if (py != last_py) {
            if (last_py != INT_MIN) {
                float y = (float)last_py / s - th * 0.5f;
                y = floorf(y * s + 0.5f) / s;
                cui_draw_rect(ui, (cui_rect){ x0, y, x1 - x0, th }, CUI_CORNERS(0), pri == 3 ? acc : pri == 2 ? bad : plain);
            }
            last_py = py; pri = p;
        } else if (p > pri) pri = p;
    }
}

/* the mark whose tick is under the mouse (within 4 points, the nearest), -1 none */
int cui__vt_mk_hit(const cui_vt *v, cui_rect r, float mx, float my) {
    const cui__vt_mk *k = cui__vt_mk_get(v);
    if (!k || !k->n || mx < r.x + r.w - 2 - CUI__VTM_TICK_W - 3 || mx > r.x + r.w || my < r.y || my > r.y + r.h) return -1;
    cui__vt_info I; cui__vt_get_info(v, &I);
    int best = -1; float bd = 4.01f;
    for (int i = 0; i < k->n; i++) {
        float d = fabsf(cui__vtm_tick_y(&I, r, k->m[i].row[CUI__VTM_A]) - my);
        if (d < bd || (d == bd && best >= 0)) { bd = d; best = i; }      /* the newer of two as near */
    }
    return best;
}
