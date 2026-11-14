/* Code editor: editable, highlighted code for tool builders (shader editors,
 * script consoles, config files). It runs on the text fields' engine
 * (cui_textedit.c: caret, insertion, word motion) and the code block's
 * highlighter (cui_code.c). Lines sit on a monospace grid, and only the lines
 * in view are tokenized and drawn; the line index and the caches are patched
 * by each edit (see "big texts" below), so files of a hundred megabytes stay
 * fast. Long lines scroll sideways, or with `.wrap` break into
 * visual rows at the view width (see "soft wrapping" below). */
#include "cui_internal.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#define CUI__ED_SSE2 1
#endif
#if defined(_MSC_VER)
#include <intrin.h>   /* _InterlockedExchangeAdd */
#endif
#if defined(_WIN32)
#include <process.h>  /* _beginthread: jobs on another thread (see "work on another thread") */
#elif !defined(__wasm__) || defined(__EMSCRIPTEN_PTHREADS__)
#include <pthread.h>
#define CUI__ED_PTHREADS 1
#endif

/* the scans are shared with the documents (cui_textdoc.c) */
#define cui__ed_find_byte cui__find_byte
#define cui__ed_count_byte cui__count_byte
#define cui__ed_fill_nl cui__fill_nl

#define CUI__ED_SALT  0xED17C0DEu
#define CUI__ED_BAR   12.0f     /* scrollbar zones along the right and bottom edges */
#define CUI__ED_INSET 6.0f      /* above the first line and below the last */
#define CUI__ED_GAP   8.0f      /* between the gutter and the code */

/* Big texts: nothing below reads the whole text in a frame where nothing changed. The line
 * index, the wrap counts and the line states are kept across frames, and each edit patches
 * them where it happened (cui__ed_splice). They are made again from the whole text only
 * when the text changed outside the editor: the app's `.revision` says so, or without one a
 * hash of the text taken each frame does. */

/* Splices from another editor kept one by one until the next frame; more join the last. */
#define CUI__ED_SPLICES 16

/* An edit of more lines than this gets its wrap counts the way a new text does: a guess for
 * every line, counted when in view, in one pass. */
#define CUI__ED_BIG_LINES 4096

/* The line index: ls[i] is the byte line i starts at, ls[nl] = len + 1. */
typedef struct {
    int *ls; int cap;
    int nl, len, maxlen;        /* lines, bytes, the longest line in bytes */
    uint64_t hash;              /* of the text, while the app gives no revision */
    int rev;                    /* the app's revision it is for */
    bool ok;
} cui__ed_ix;

/* A number per line, in blocks: an edit that adds or removes lines moves the numbers of one
 * block, not of every line below it, and the sums of the blocks (kept up to date lazily, from
 * the first block that changed) turn a row into a line and back in O(log n). */
#define CUI__SQ_B 1024
typedef struct { int *v; int n, cap; int sum; } cui__sqb;
typedef struct {
    cui__sqb *b; int nb, bcap;
    int *pn, *ps; int pcap;     /* before block i: how many numbers, and their sum */
    int fixed;                  /* pn and ps are right up to block `fixed` */
    int n;
    bool sums;                  /* keep sums (of absolute values) */
} cui__sq;

/* Soft wrapping: how many rows each line takes (and, from the sums, the rows above a line: a
 * scroll offset becomes a line and back). A line that fits the width is one row; a longer one
 * is first guessed from its length (rows < 0, minus the guess) and counted when it comes into
 * view, so opening a big file does not wrap every line. */
typedef struct {
    cui__sq rows;
    int n, key;                 /* lines; columns per row and tab size the counts are for */
    bool ok;
} cui__ed_wc;

/* The state at the start of each line that a line's colors depend on: markdown's (open
 * fence, front matter, HTML comment), or whether a block comment is open. st[0..valid] are
 * right. st[kept0, kept1) were right before the last edits above them, and the lines from
 * kept0 on are the same as then: once the state found for such a line is the one it had,
 * the states down to kept1 are right again, so an edit costs the lines it touched, not the
 * rest of the file. Found on demand, only as far down as the lines in view. */
typedef struct {
    cui__sq st;                 /* the state before line i, and at n the one after the last */
    int n, valid, kept0, kept1;
    const char *lc; int mode; bool bt;   /* the language the states are for */
    bool ok;
    unsigned epoch;             /* counts the times the states started over: a job's result is for one */
    int dirty;                  /* the first line an edit touched since the job for the states began (INT_MAX: none) */
} cui__ed_mc;

/* Work on another thread (see "work on another thread" below): the line states after an edit far
 * above the view, and Replace all on a document. */
typedef struct cui__ed_job cui__ed_job;

/* Undo: each edit keeps the bytes it removed and the bytes it put in (not a copy of the
 * text); edits of the same group undo together. */
typedef struct {
    int at, del_n, ins_n;       /* the bytes [at, at + del_n) became [at, at + ins_n) */
    char *data;                 /* the removed bytes, then the inserted ones */
    char *ins;                  /* or, for a big edit: the inserted bytes apart (the caller's own copy, kept as it is), and data holds only the removed ones */
    int caret, group;           /* the caret before the edit */
} cui__ed_rec;
typedef struct {
    cui__ed_rec *r; int n, cap, pos;   /* records [0, pos) are applied, [pos, n) can be redone */
    int group;                  /* the last group's number */
    size_t bytes;               /* held by the records */
} cui__ed_hist;

/* The find bar's match count, kept between frames: the text in blocks of whole lines, each with
 * its matches (-1: not counted yet). An edit marks the blocks it touched and moves the others;
 * blocks are counted a few megabytes a frame, so a big text never stalls a frame and the count
 * catches up within a few. */
typedef struct { int end, count; } cui__ed_fb;   /* a block: bytes up to end (from the end of the one before) */
typedef struct {
    char q[128]; bool cs, word, rx, on;   /* what the counts are for */
    cui__ed_fb *b; int n, cap;
    int total; bool done;
    unsigned ver;               /* changes with the blocks: the index below is for one */
    unsigned iver; int is0, is1, index; bool ion;
} cui__ed_fc;

typedef struct {
    cui_edit_state ed;          /* first: caret, anchor and undo history, as in every text field */
    double sy;                  /* scroll, points */
    float sx;
    int pref_col;               /* column kept by up and down, -1 none */
    int drag, drag_a, drag_b;   /* pointer drag unit (1 char, 2 word, 3 line) and the unit pressed */
    int bar_drag;               /* scrollbar thumb held: 1 vertical, 2 horizontal */
    float bar_grab, bar_t;
    double last_activity;
    int mode;                   /* bar: 0 none, 1 find, 2 go to line */
    char query[128], repl[128], go[24];
    bool rep_show;              /* the bar shows the replace row */
    bool f_case, f_word, f_rx;  /* find: match case, whole word, regular expression */
    bool tab_free;              /* Esc released Tab: the next one moves focus */
    bool bar_focus;             /* give the bar's field focus this frame */
    bool pend_sel, pend_goto, pend_find, init;
    int pend_a, pend_c, pend_line, pend_col;
    bool pend_edit;             /* cui_code_editor_edit: replace [pend_es, pend_ee) with pend_text, then select */
    int pend_es, pend_ee, pend_sa, pend_sc;
    char *pend_text; int pend_n, pend_nl; bool pend_cr;   /* its length, line breaks, and whether it has carriage returns */
    int *pend_lb;               /* on a document, a big text's line breaks (offsets), found as it was copied: the document reads it no more */
    float focus_t;
    cui__ed_wc *wc;             /* wrap cache, only while `.wrap` is on */
    cui__ed_mc *mc;             /* line states, for the languages with a state (markdown, yaml, xml...) and those with block comments */
    int aff_pos;                /* a caret at this byte, on a row boundary, shows at the end of the row above (End, a click past the end) */
    cui__ed_ix ix;
    cui__ed_hist hist;
    unsigned gen;               /* counts changes to the text: the bracket pair below is for one */
    unsigned br_gen; int br_at, br_a, br_b;
    bool br_ok;
    bool sp_on;                 /* cui_code_editor_splice: bytes [sp_a, sp_b0) of the text this index is for are now [sp_a, sp_b1); applied on the next frame */
    int sp_a, sp_b0, sp_b1;
    int sp[CUI__ED_SPLICES][3], sp_n;   /* the same splices one by one, in order (past the last one join), to move the caret and the undo steps exactly */
    float lh_seen;              /* the line height last frame: a splice above the view keeps the lines in view where they are */
    cui__ed_fc fc;
    cui_text_doc *doc;          /* with `.doc`: the document the caches are for, its revision they are at, and the */
    unsigned doc_seen, doc_mine; /* revision after this editor's own last edit (typing on joins its undo step only then) */
    unsigned doc_serial;        /* the document's serial: another document at the same address is another */
    cui__ed_job *sjob;          /* the line states being found on another thread, or NULL */
    cui__ed_job *rjob;          /* Replace all being worked out on another thread, or NULL */
} cui__ed_state;

/* The piece of a document read last: q[i] is byte i for i in [cs, ce) (q is biased: it points
 * cs bytes before the piece's first byte, so a line's absolute offsets index it directly). */
typedef struct { const char *q; int cs, ce; } cui__ed_ck;

/* One frame of editing: the caller's buffer and the line index, or a document. */
typedef struct {
    cui_ctx *ui;
    cui__ed_state *st;
    char *buf;
    size_t cap;
    cui_text_doc *doc;          /* doc mode: no buf, ls or cap; text through the accessors below */
    cui__ed_ck *ck;
    int len;
    int *ls, nl, maxlen;        /* line starts (ls[nl] = len + 1), count, longest line in bytes */
    int tab;
    bool use_tabs, ro;
    bool changed, refused, reveal, center;
    bool wrap;                  /* soft wrapping on; wcols is the row width in columns */
    int wcols;
    int hint_cr, hint_nl;       /* for the next replace of cui_code_editor_edit's text, read while it was copied: 1 no carriage returns, 2 some; line breaks + 1 (0: not known) */
    int *hint_lb;               /* and on a document, where they are (taken by the replace) */
    bool rep_on;                /* this frame's edits as one splice: [rep_a, rep_b0) of the text before them is [rep_a, rep_b1) now */
    int rep_a, rep_b0, rep_b1;
} cui__ed;

/* One more splice, bytes [s, s + r) of the text as it is now becoming i bytes, joined to the
 * splices before it: [*a, *b0) of the text before all of them is [*a, *b1) after. The result is
 * the smallest range that covers them all; outside it the text is the old one, moved. */
static void cui__ed_span_add(bool *on, int *a, int *b0, int *b1, int s, int r, int i) {
    if (!*on) { *on = true; *a = s; *b0 = s + r; *b1 = s + i; return; }
    int lo = CUI_MIN(*a, s), hi = CUI_MAX(*b1, s + r);
    *b0 += hi - *b1;            /* past the covered range the text is the old one, shifted */
    *b1 = hi + i - r;
    *a = lo;
}
static void cui__ed_wrap_splice(cui__ed *E, int la, int lb, int k);
static void cui__ed_wrap_sync(cui__ed *E);
static void cui__ed_job_drop(cui__ed_job **J);
typedef struct cui__ed_q cui__ed_q;
static void cui__ed_rjob_start(cui__ed *E, const cui__ed_q *Q, const char *with);
static void cui__ed_mc_splice(cui_ctx *ui, cui__ed_mc *mc, int la, int lb, int k);
static void cui__ed_fc_splice(cui__ed_fc *fc, int a, int b0, int b1);

/* ---- numbers per line, in blocks (cui__sq) ----
 * No block is empty (but the only one), and slots past nb hold no storage. */

static int cui__sq_val(const cui__sq *q, int v) { return q->sums ? abs(v) : 0; }

static void cui__sq_free(cui_ctx *ui, cui__sq *q) {
    for (int i = 0; i < q->nb; i++) cui__free(ui, q->b[i].v);
    cui__free(ui, q->b); cui__free(ui, q->pn); cui__free(ui, q->ps);
    bool sums = q->sums;
    memset(q, 0, sizeof *q);
    q->sums = sums;
}

static void cui__sq_room(cui_ctx *ui, cui__sq *q, int nb) {
    if (nb <= q->bcap) return;
    int nc = CUI_MAX(nb, q->bcap * 2);
    q->b = cui__realloc(ui, q->b, sizeof *q->b * (size_t)nc);
    memset(q->b + q->bcap, 0, sizeof *q->b * (size_t)(nc - q->bcap));
    q->bcap = nc;
}

/* n numbers, all v */
static void cui__sq_fill(cui_ctx *ui, cui__sq *q, int n, int v) {
    int nb = CUI_MAX(1, (n + CUI__SQ_B - 1) / CUI__SQ_B);
    cui__sq_room(ui, q, nb);
    for (int i = nb; i < q->nb; i++) { cui__free(ui, q->b[i].v); memset(&q->b[i], 0, sizeof q->b[i]); }
    q->nb = nb;
    for (int i = 0; i < nb; i++) {
        cui__sqb *b = &q->b[i];
        b->n = CUI_MAX(0, CUI_MIN(CUI__SQ_B, n - i * CUI__SQ_B));
        if (b->cap < CUI__SQ_B) { b->v = cui__realloc(ui, b->v, sizeof(int) * CUI__SQ_B); b->cap = CUI__SQ_B; }
        for (int k = 0; k < b->n; k++) b->v[k] = v;
        b->sum = b->n * cui__sq_val(q, v);
    }
    q->n = n; q->fixed = 0;
}

/* the prefix counts and sums, right again from the first block that changed */
static void cui__sq_fix(cui_ctx *ui, cui__sq *q) {
    if (q->pcap < q->nb + 1) {
        q->pcap = q->nb + 16;
        q->pn = cui__realloc(ui, q->pn, sizeof(int) * (size_t)q->pcap);
        q->ps = cui__realloc(ui, q->ps, sizeof(int) * (size_t)q->pcap);
        q->fixed = 0;
    }
    if (q->fixed == 0) { q->pn[0] = 0; q->ps[0] = 0; }
    for (int i = q->fixed; i < q->nb; i++) { q->pn[i + 1] = q->pn[i] + q->b[i].n; q->ps[i + 1] = q->ps[i] + q->b[i].sum; }
    q->fixed = q->nb;
}

/* the block holding number i (i == n: the end of the last block) and the offset there */
static int cui__sq_at(cui_ctx *ui, cui__sq *q, int i, int *off) {
    if (q->nb == 0) cui__sq_fill(ui, q, 0, 0);
    if (q->fixed < q->nb) cui__sq_fix(ui, q);
    int lo = 0, hi = q->nb - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (q->pn[m] <= i) lo = m; else hi = m - 1; }
    *off = CUI_CLAMP(i - q->pn[lo], 0, q->b[lo].n);
    return lo;
}

static int cui__sq_get(cui_ctx *ui, cui__sq *q, int i) {
    int off, b = cui__sq_at(ui, q, i, &off);
    return off < q->b[b].n ? q->b[b].v[off] : 0;
}

static void cui__sq_set(cui_ctx *ui, cui__sq *q, int i, int v) {
    int off, bi = cui__sq_at(ui, q, i, &off);
    cui__sqb *b = &q->b[bi];
    if (off >= b->n) return;
    int d = cui__sq_val(q, v) - cui__sq_val(q, b->v[off]);
    b->v[off] = v;
    if (d == 0) return;   /* the counts are as they were, and so are the sums: nothing to fix */
    b->sum += d;
    if (q->fixed > bi) q->fixed = bi;
}

/* numbers [i, i + n) to out, block by block */
static void cui__sq_read(cui_ctx *ui, cui__sq *q, int i, int n, int *out) {
    int off, bi = cui__sq_at(ui, q, i, &off);
    for (int k = 0; k < n && bi < q->nb; bi++, off = 0) {
        int take = CUI_MIN(n - k, q->b[bi].n - off);
        memcpy(out + k, q->b[bi].v + off, sizeof(int) * (size_t)take);
        k += take;
    }
}

/* numbers [i, i + n) become v[0, n), block by block (a queue without sums: nothing to fix) */
static void cui__sq_write(cui_ctx *ui, cui__sq *q, int i, int n, const int *v) {
    int off, bi = cui__sq_at(ui, q, i, &off);
    for (int k = 0; k < n && bi < q->nb; bi++, off = 0) {
        int take = CUI_MIN(n - k, q->b[bi].n - off);
        memcpy(q->b[bi].v + off, v + k, sizeof(int) * (size_t)take);
        k += take;
    }
}

/* the sum of numbers [0, i) */
static int cui__sq_prefix(cui_ctx *ui, cui__sq *q, int i) {
    int off, bi = cui__sq_at(ui, q, i, &off);
    int s = q->ps[bi];
    if (q->sums) for (int k = 0; k < off; k++) s += abs(q->b[bi].v[k]);
    return s;
}

static int cui__sq_total(cui_ctx *ui, cui__sq *q) {
    if (q->fixed < q->nb) cui__sq_fix(ui, q);
    return q->ps[q->nb];
}

/* the i whose span of the sums holds s: prefix(i) <= s < prefix(i + 1) (the last one past the total) */
static int cui__sq_find(cui_ctx *ui, cui__sq *q, int s) {
    if (q->fixed < q->nb) cui__sq_fix(ui, q);
    if (q->n <= 0) return 0;
    int lo = 0, hi = q->nb - 1;
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (q->ps[m] <= s) lo = m; else hi = m - 1; }
    int acc = q->ps[lo];
    const cui__sqb *b = &q->b[lo];
    for (int k = 0; k < b->n; k++) {
        acc += abs(b->v[k]);
        if (acc > s) return q->pn[lo] + k;
    }
    return q->n - 1;
}

/* numbers [i, i + del) become ins numbers of value v */
static void cui__sq_splice(cui_ctx *ui, cui__sq *q, int i, int del, int ins, int v) {
    int off, bi = cui__sq_at(ui, q, i, &off);
    q->fixed = CUI_MIN(q->fixed, bi);
    int left = CUI_MIN(del, q->n - i);
    while (left > 0) {   /* remove, block by block */
        cui__sqb *b = &q->b[bi];
        int take = CUI_MIN(left, b->n - off);
        if (q->sums) for (int k = 0; k < take; k++) b->sum -= abs(b->v[off + k]);
        memmove(b->v + off, b->v + off + take, sizeof(int) * (size_t)(b->n - off - take));
        b->n -= take;
        left -= take;
        q->n -= take;
        if (b->n == 0 && q->nb > 1) {   /* an empty block goes */
            cui__free(ui, b->v);
            memmove(q->b + bi, q->b + bi + 1, sizeof *q->b * (size_t)(q->nb - bi - 1));
            q->nb--;
            memset(&q->b[q->nb], 0, sizeof q->b[q->nb]);
            off = 0;
        } else { bi++; off = 0; }
    }
    if (ins <= 0) return;
    bi = cui__sq_at(ui, q, i, &off);
    q->fixed = CUI_MIN(q->fixed, bi);
    cui__sqb *b = &q->b[bi];
    if (b->n + ins <= 2 * CUI__SQ_B) {   /* fits the block */
        if (b->cap < b->n + ins) { b->cap = 2 * CUI__SQ_B; b->v = cui__realloc(ui, b->v, sizeof(int) * (size_t)b->cap); }
        memmove(b->v + off + ins, b->v + off, sizeof(int) * (size_t)(b->n - off));
        for (int k = 0; k < ins; k++) b->v[off + k] = v;
        b->n += ins;
        b->sum += ins * cui__sq_val(q, v);
        q->n += ins;
        return;
    }
    /* the block is cut at off; blocks of the new numbers, then one of its tail, go after it */
    int tail = b->n - off, tsum = 0;
    if (q->sums) for (int k = 0; k < tail; k++) tsum += abs(b->v[off + k]);
    int nins = (ins + CUI__SQ_B - 1) / CUI__SQ_B, nnew = nins + (tail > 0);
    cui__sq_room(ui, q, q->nb + nnew);
    b = &q->b[bi];
    memmove(q->b + bi + 1 + nnew, q->b + bi + 1, sizeof *q->b * (size_t)(q->nb - bi - 1));
    q->nb += nnew;
    for (int k = 0; k < nnew; k++) {
        cui__sqb *nbk = &q->b[bi + 1 + k];
        int cnt = k < nins ? CUI_MIN(CUI__SQ_B, ins - k * CUI__SQ_B) : tail;
        nbk->cap = CUI_MAX(cnt, CUI__SQ_B);
        nbk->v = cui__alloc(ui, sizeof(int) * (size_t)nbk->cap);
        nbk->n = cnt;
        if (k < nins) { for (int j = 0; j < cnt; j++) nbk->v[j] = v; nbk->sum = cnt * cui__sq_val(q, v); }
        else { memcpy(nbk->v, b->v + off, sizeof(int) * (size_t)tail); nbk->sum = tsum; }
    }
    b->n = off; b->sum -= tsum;
    if (b->n == 0) {   /* cut at its start: it goes */
        cui__free(ui, b->v);
        memmove(q->b + bi, q->b + bi + 1, sizeof *q->b * (size_t)(q->nb - bi - 1));
        q->nb--;
        memset(&q->b[q->nb], 0, sizeof q->b[q->nb]);
    }
    q->n += ins;
}

/* ---- reading the text ----
 * In buffer mode the text is the caller's buffer; with a document it lies in pieces. What reads
 * the text goes through these: a byte, a line's bytes (no copy when they lie in one piece, which
 * is nearly always), and a walk over many lines that copies only a line that crosses pieces. */

/* the piece holding byte i, biased: q[i] is byte i for i in [*cs, *ce) */
static const char *cui__ed_chunk(const cui__ed *E, int i, int *cs, int *ce) {
    if (!E->doc) { *cs = 0; *ce = E->len; return E->buf; }
    cui__ed_ck *k = E->ck;
    if (!(k->q && i >= k->cs && i < k->ce)) {
        const char *p = cui__doc_chunk(E->doc, i, &k->cs, &k->ce);
        k->q = p ? p - k->cs : NULL;
    }
    *cs = k->cs; *ce = k->ce;
    return k->q;
}

/* the byte at i (0 at or past the end) */
static char cui__ed_ch(const cui__ed *E, int i) {
    if (!E->doc) return E->buf[i];
    if (i < 0 || i >= E->len) return 0;
    const cui__ed_ck *k = E->ck;
    if (k->q && i >= k->cs && i < k->ce) return k->q[i];
    int cs, ce;
    return cui__ed_chunk(E, i, &cs, &ce)[i];
}

/* Bytes [a, b): q with q[a, b) them. In one piece: no copy; across pieces: a copy in frame memory. */
static const char *cui__ed_span(const cui__ed *E, int a, int b) {
    if (!E->doc) return E->buf;
    static const char none[1];
    if (b <= a) return none - a;
    int cs, ce;
    const char *q = cui__ed_chunk(E, a, &cs, &ce);
    if (q && b <= ce) return q;
    char *c = cui__frame_alloc(E->ui, (size_t)(b - a) + 1);
    cui_text_doc_read(E->doc, a, b - a, c);
    return c - a;
}

static void cui__ed_read(const cui__ed *E, int a, int b, char *out) {
    if (b <= a) return;
    if (!E->doc) memcpy(out, E->buf + a, (size_t)(b - a));
    else cui_text_doc_read(E->doc, a, b - a, out);
}

static int cui__ed_ls(const cui__ed *E, int line) { return E->doc ? cui__doc_line_start(E->doc, line) : E->ls[line]; }
static int cui__ed_end(const cui__ed *E, int line) { return cui__ed_ls(E, line + 1) - 1; }

static int cui__ed_line_of(const cui__ed *E, int pos) {
    if (E->doc) return cui__doc_line_of(E->doc, CUI_CLAMP(pos, 0, E->len));
    int lo = 0, hi = E->nl - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (E->ls[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* line `line`: its bytes [*ls, *le) as q[*ls, *le) */
static const char *cui__ed_line(const cui__ed *E, int line, int *ls, int *le) {
    *ls = cui__ed_ls(E, line);
    *le = cui__ed_end(E, line);
    return cui__ed_span(E, *ls, *le);
}

/* Walks lines from a line start on, each line's bytes where they lie (a line across pieces is
 * copied to scratch the walk owns: cui__ed_lw_done). O(1) a line, no index lookups. */
typedef struct { const cui__ed *E; int pos; char *scr; int scap; } cui__ed_lw;

static cui__ed_lw cui__ed_lw_at(const cui__ed *E, int pos) { cui__ed_lw w = { E, pos, NULL, 0 }; return w; }
static void cui__ed_lw_done(cui__ed_lw *w) { cui__free(w->E->ui, w->scr); w->scr = NULL; w->scap = 0; }

/* the next line: [*ls, *le) (le: its line break or the end) as q[*ls, *le); NULL past the end */
static const char *cui__ed_lw_next(cui__ed_lw *w, int *ls, int *le) {
    const cui__ed *E = w->E;
    int a = w->pos;
    if (a > E->len) return NULL;
    *ls = a;
    if (!E->doc) {
        const char *p = cui__ed_find_byte(E->buf + a, (size_t)(E->len - a), '\n');
        *le = p ? (int)(p - E->buf) : E->len;
        w->pos = *le + 1;
        return E->buf;
    }
    int cs, ce;
    const char *q = a < E->len ? cui__ed_chunk(E, a, &cs, &ce) : NULL;
    if (q) {
        const char *p = cui__ed_find_byte(q + a, (size_t)(ce - a), '\n');
        if (p) { *le = (int)(p - q); w->pos = *le + 1; return q; }
        if (ce == E->len) { *le = E->len; w->pos = E->len + 1; return q; }
    }
    int e = a >= E->len ? E->len : cui__ed_end(E, cui__doc_line_of(E->doc, a));
    if (e - a + 1 > w->scap) { w->scap = e - a + 1024; w->scr = cui__realloc(E->ui, w->scr, (size_t)w->scap); }
    cui_text_doc_read(E->doc, a, e - a, w->scr);
    *le = e;
    w->pos = e + 1;
    return w->scr - a;
}

/* ---- text model ---- */

/* ---- byte scans ----
 * The libc scans run at about 4 GB/s here; a 50 MB text is read a few times when it is
 * replaced as a whole (a carriage return, the line breaks twice), so these go at memory
 * speed with SSE2 where it is baseline, and fall back to memchr elsewhere. */

#if defined(CUI__ED_SSE2)
static int cui__ed_ctz(unsigned m) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward(&i, m);
    return (int)i;
#else
    return __builtin_ctz(m);
#endif
}
static int cui__ed_ctz64(uint64_t m) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward64(&i, m);
    return (int)i;
#else
    return __builtin_ctzll(m);
#endif
}
#endif

/* d = buf + a gets t[0, n) and the line breaks of it go to out, as cui__ed_fill_nl does: the
 * text is read once, not twice (the second time from memory, for a big one) */
static int cui__ed_copy_fill(char *buf, int a, const char *t, int n, int *out) {
    int k = 0, i = 0;
#if defined(CUI__ED_SSE2)
    const __m128i v = _mm_set1_epi8('\n');
    for (; i + 64 <= n; i += 64) {
        uint64_t m = 0;
        for (int q = 0; q < 4; q++) {
            __m128i x = _mm_loadu_si128((const __m128i *)(t + i + q * 16));
            _mm_storeu_si128((__m128i *)(buf + a + i + q * 16), x);
            m |= (uint64_t)(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(x, v)) << (q * 16);
        }
        while (m) { out[k++] = a + i + cui__ed_ctz64(m) + 1; m &= m - 1; }
    }
#endif
    memcpy(buf + a + i, t + i, (size_t)(n - i));
    return k + cui__ed_fill_nl(buf, a + i, a + n, out + k);
}

/* the length of a zero-terminated string, in aligned blocks (they never cross a page, so
 * reading the rest of the one the terminator is in is safe) */
static size_t cui__ed_strlen(const char *s) {
#if defined(CUI__ED_SSE2)
    const char *p = s;
    while (((uintptr_t)p & 15) != 0) { if (!*p) return (size_t)(p - s); p++; }
    const __m128i zero = _mm_setzero_si128();
    for (;; p += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)p), zero));
        if (m) return (size_t)(p - s) + (size_t)cui__ed_ctz(m);
    }
#else
    return strlen(s);
#endif
}

static const char *cui__ed_rec_ins(const cui__ed_rec *q) { return q->ins ? q->ins : q->data + q->del_n; }
static void cui__ed_rec_free(cui_ctx *ui, cui__ed_rec *q) { cui__free(ui, q->data); free(q->ins); }   /* ins is a cui_code_editor_edit text: malloc'd, as a document takes it */

/* d[0, n) = s[0, n), and in the same pass: whether s holds a carriage return, and how many
 * line breaks (cui_code_editor_edit reads the text once anyway, to keep it). */
static size_t cui__ed_copy_scan(char *d, const char *s, size_t n, bool *has_cr) {
    size_t total = 0;
    *has_cr = false;
#if defined(CUI__ED_SSE2)
    const __m128i vn = _mm_set1_epi8('\n'), vr = _mm_set1_epi8('\r'), zero = _mm_setzero_si128();
    __m128i crs = zero;
    while (n >= 16) {
        size_t blocks = CUI_MIN(n / 16, (size_t)255);
        __m128i acc = zero;
        for (size_t j = 0; j < blocks; j++) {
            __m128i x = _mm_loadu_si128((const __m128i *)s + j);
            _mm_storeu_si128((__m128i *)d + j, x);
            acc = _mm_sub_epi8(acc, _mm_cmpeq_epi8(x, vn));
            crs = _mm_or_si128(crs, _mm_cmpeq_epi8(x, vr));
        }
        __m128i sum = _mm_sad_epu8(acc, zero);
        total += (size_t)_mm_cvtsi128_si32(sum) + (size_t)_mm_cvtsi128_si32(_mm_srli_si128(sum, 8));
        s += blocks * 16; d += blocks * 16; n -= blocks * 16;
    }
    if (_mm_movemask_epi8(crs)) *has_cr = true;
    for (; n > 0; n--, s++, d++) { *d = *s; total += *s == '\n'; if (*s == '\r') *has_cr = true; }
#else
    memcpy(d, s, n);
    *has_cr = cui__ed_find_byte(s, n, '\r') != NULL;
    total = cui__ed_count_byte(s, n, '\n');
#endif
    return total;
}

/* d[0, n) = s[0, n), and in the same pass the offsets of its line breaks (*lb, malloc'd: a document
 * takes them with the text) and whether it holds a carriage return; the number of line breaks. In
 * blocks, each indexed while it is still in the cache. */
static int cui__ed_copy_lb(char *d, const char *s, int n, bool *has_cr, int **lb) {
    int k = 0, cap = 0, *o = NULL;
    *has_cr = false;
    for (int a = 0; a < n; a += 65536) {
        int e = CUI_MIN(n, a + 65536);
        if (k + (e - a) > cap) {   /* at most a line break a byte */
            int nc = CUI_MAX(cap * 2, k + (e - a));
            int *no = realloc(o, sizeof(int) * (size_t)nc);
            if (!no) { free(o); *lb = NULL; return (int)cui__ed_copy_scan(d + a, s + a, (size_t)(n - a), has_cr) + k; }   /* (without the offsets) */
            o = no; cap = nc;
        }
        int got = cui__ed_copy_fill(d, a, s + a, e - a, o + k);
        for (int i = k; i < k + got; i++) o[i]--;   /* the line break, not the byte after it */
        k += got;
        if (!*has_cr && cui__ed_find_byte(d + a, (size_t)(e - a), '\r')) *has_cr = true;
    }
    if (cap > k + k / 4 + 1024) { int *no = realloc(o, sizeof(int) * (size_t)CUI_MAX(k, 1)); if (no) o = no; }
    *lb = o;
    return k;
}

/* A hash of the whole text (four lanes, so it runs at memory speed): without the app's
 * revision, the editor finds out this way whether the text changed since the last frame. */
static uint64_t cui__ed_hash_text(const char *p, size_t n) {
    uint64_t h[4] = { 0x9E3779B97F4A7C15ull ^ (uint64_t)n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull };
    while (n >= 32) {
        for (int k = 0; k < 4; k++) {
            uint64_t w;
            memcpy(&w, p + k * 8, 8);
            h[k] = (h[k] ^ w) * 0xFF51AFD7ED558CCDull;
            h[k] ^= h[k] >> 29;
        }
        p += 32; n -= 32;
    }
    uint64_t r = h[0] ^ (h[1] * 31) ^ (h[2] * 961) ^ (h[3] * 29791);
    while (n-- > 0) r = (r ^ (unsigned char)*p++) * 0x100000001B3ull;
    return r ^ (r >> 31);
}

static void cui__ed_maxlen(cui__ed_ix *ix) {
    int m = 0;
    for (int i = 0; i < ix->nl; i++) if (ix->ls[i + 1] - 1 - ix->ls[i] > m) m = ix->ls[i + 1] - 1 - ix->ls[i];
    ix->maxlen = m;
}

/* The line index made from the whole text: the text changed outside the editor. Everything
 * kept for the old text goes: the wrap counts, the line states and the undo history (whose
 * offsets are for the old text). */
static void cui__ed_rebuild(cui_ctx *ui, cui__ed_state *st, const char *buf, int len) {
    cui__ed_ix *ix = &st->ix;
    int n = 1;   /* one pass: the index grows by the segment as it is read */
    CUI__GROW(ui, ix->ls, 0, ix->cap, 2);
    ix->ls[0] = 0;
    for (int a = 0; a < len; a += 16384) {
        int b = CUI_MIN(len, a + 16384);
        CUI__GROW(ui, ix->ls, n, ix->cap, (b - a) + 2);   /* at most a line break a byte */
        n += cui__ed_fill_nl(buf, a, b, ix->ls + n);
    }
    CUI__GROW(ui, ix->ls, n, ix->cap, 1);
    ix->ls[n] = len + 1;
    if (ix->cap - n > n / 4 + 4096) { ix->ls = cui__realloc(ui, ix->ls, sizeof(int) * (size_t)(n + 1)); ix->cap = n + 1; }
    ix->nl = n; ix->len = len; ix->ok = true;
    cui__ed_maxlen(ix);
    if (st->wc) st->wc->ok = false;
    if (st->mc) st->mc->ok = false;
    st->fc.on = false;
    cui__ed_hist *h = &st->hist;
    for (int i = 0; i < h->n; i++) cui__ed_rec_free(ui, &h->r[i]);
    h->n = h->pos = 0; h->bytes = 0;
    st->gen++;
}

/* Bring the index up to date with the caller's buffer at the start of a frame, and return
 * the text's length. With a revision (nonzero) the app promises the text changed only
 * through the editor while the revision stays the same, so nothing is read; without one
 * the text is hashed and compared. */
static int cui__ed_text_sync(cui_ctx *ui, cui__ed_state *st, char *buf, size_t cap, int rev) {
    cui__ed_ix *ix = &st->ix;
    if (rev && ix->ok && ix->rev == rev && (size_t)ix->len < cap && buf[ix->len] == 0) return ix->len;
    const char *z = cui__ed_find_byte(buf, cap, 0);
    if (!z) { buf[cap - 1] = 0; z = buf + cap - 1; }   /* never read past the capacity */
    int len = (int)(z - buf);
    uint64_t h = rev ? 0 : cui__ed_hash_text(buf, (size_t)len);
    if (!(ix->ok && ix->len == len && !rev && ix->rev == 0 && ix->hash == h)) cui__ed_rebuild(ui, st, buf, len);
    ix->hash = h; ix->rev = rev;
    return len;
}

/* Bytes [a, b) of the text became n bytes at a: the line index, the wrap counts and the
 * line states follow, each patched where the edit was. */
static void cui__ed_reindex(cui__ed *E, int a, int b, int n, const char *t) {
    cui__ed_ix *ix = &E->st->ix;
    int la = 0, lb = 0;
    {   /* the lines a and b were on, before the edit */
        int lo = 0, hi = ix->nl - 1;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (ix->ls[mid] <= a) lo = mid; else hi = mid - 1; }
        la = lo; hi = ix->nl - 1;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (ix->ls[mid] <= b) lo = mid; else hi = mid - 1; }
        lb = lo;
    }
    bool longest = false;   /* the edit touched a longest line: it may be shorter now */
    for (int i = la; i <= lb && !longest; i++) longest = ix->ls[i + 1] - 1 - ix->ls[i] >= ix->maxlen;
    bool fused = E->hint_nl != 0;   /* the line breaks are known: the new bytes are copied in as they are indexed */
    int k = fused ? E->hint_nl - 1 : (int)cui__ed_count_byte(E->buf + a, (size_t)n, '\n');
    E->hint_nl = 0;
    int d = k - (lb - la), delta = n - (b - a);
    CUI__GROW(E->ui, ix->ls, 0, ix->cap, ix->nl + d + 1);
    memmove(ix->ls + la + 1 + k, ix->ls + lb + 1, sizeof(int) * (size_t)(ix->nl - lb));
    ix->nl += d;
    for (int i = la + 1 + k; i <= ix->nl; i++) ix->ls[i] += delta;
    if (fused) cui__ed_copy_fill(E->buf, a, t, n, ix->ls + la + 1);
    else cui__ed_fill_nl(E->buf, a, a + n, ix->ls + la + 1);
    ix->len = E->len;
    if (longest) cui__ed_maxlen(ix);
    else for (int i = la; i <= la + k; i++) ix->maxlen = CUI_MAX(ix->maxlen, ix->ls[i + 1] - 1 - ix->ls[i]);
    E->ls = ix->ls; E->nl = ix->nl; E->maxlen = ix->maxlen;
    if (E->wrap && E->st->wc && E->st->wc->ok) {
        if (k > CUI__ED_BIG_LINES || lb - la > CUI__ED_BIG_LINES) { E->st->wc->ok = false; cui__ed_wrap_sync(E); }   /* guesses for all, as when opening */
        else cui__ed_wrap_splice(E, la, lb, k);
    }
    if (E->st->mc && E->st->mc->ok) cui__ed_mc_splice(E->ui, E->st->mc, la, lb, k);
    cui__ed_fc_splice(&E->st->fc, a, b, a + n);
    E->st->gen++;
}

/* Column after s[a, b) when it starts at column c: tabs go to the next stop,
 * every code point is one column. */
static int cui__ed_adv(const char *s, int a, int b, int tab, int c) {
    for (int i = a; i < b; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\t') c = (c / tab + 1) * tab;
        else if ((ch & 0xC0) != 0x80) c++;
    }
    return c;
}
/* Column of byte b in the line that starts at a. */
static int cui__ed_col(const char *s, int a, int b, int tab) { return cui__ed_adv(s, a, b, tab, 0); }

/* The byte boundary in [a, e) nearest to column x, when a is at column c. */
static int cui__ed_at_col0(const char *s, int a, int e, float x, int tab, int c) {
    int i = a;
    while (i < e) {
        int nc = s[i] == '\t' ? (c / tab + 1) * tab : c + 1;
        if (x < (float)(c + nc) * 0.5f) break;
        c = nc;
        i = cui__utf8_next(s, e, i);
    }
    return i;
}
static int cui__ed_at_col(const char *s, int a, int e, float x, int tab) { return cui__ed_at_col0(s, a, e, x, tab, 0); }

static int cui__ed_pos(const cui__ed *E, int line, float col) {
    line = CUI_CLAMP(line, 0, E->nl - 1);
    int ls, le;
    const char *s = cui__ed_line(E, line, &ls, &le);
    return cui__ed_at_col(s, ls, le, col, E->tab);
}

static int cui__ed_caret_col(const cui__ed *E, int pos) {
    int ls = cui__ed_ls(E, cui__ed_line_of(E, pos));
    return cui__ed_col(cui__ed_span(E, ls, pos), ls, pos, E->tab);
}

/* first character of the line that is not a space or a tab */
static int cui__ed_first_text(const cui__ed *E, int line) {
    int i, e;
    const char *s = cui__ed_line(E, line, &i, &e);
    while (i < e && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* ---- soft wrapping ----
 * With `.wrap` a line is cut into rows of at most `wcols` columns. A row
 * breaks after a space or punctuation where it can, inside a word where it
 * must; continuation rows start at the column of the line's indentation
 * (but leave at least CUI__ED_KEEP columns for text). Tab stops count from
 * the row's own first column. Row starts are never stored: the walk below
 * finds them from the line's bytes, and only the number of rows per line is
 * cached (cui__ed_wc), so a long line costs one walk when it changes and
 * one more for each frame in which it is in view. */

#define CUI__ED_KEEP 20

static bool cui__ed_ws(char c) { return c == ' ' || c == '\t'; }
static bool cui__ed_brk_after(char c) {
    switch (c) {
    case ' ': case '\t': case ',': case ';': case ')': case ']': case '}': case '-': case '/': case '|': case '&': return true;
    default: return false;
    }
}

/* Columns a continuation row of the line s[ls, le) is indented by. */
static int cui__ed_wrap_ind(const cui__ed *E, const char *s, int ls, int le) {
    int i = ls;
    while (i < le && cui__ed_ws(s[i])) i++;
    return CUI_MIN(cui__ed_adv(s, ls, i, E->tab, 0), CUI_MAX(0, E->wcols - CUI__ED_KEEP));
}

/* The rows of the line s[ls, le): writes the start of each into rs (up to cap)
 * and returns how many there are. */
static int cui__ed_wrap_walk(const cui__ed *E, const char *s, int ls, int le, int *rs, int cap) {
    int W = E->wcols, tab = E->tab, ind = cui__ed_wrap_ind(E, s, ls, le), a = ls, n = 0;
    for (;;) {
        if (rs && n < cap) rs[n] = a;
        n++;
        int i = a, col = n == 1 ? 0 : ind, brk = -1, end = le;
        bool text = false;
        while (i < le) {
            char c = s[i];
            int nx = i + 1, nc = c == '\t' ? (col / tab + 1) * tab : col + 1;
            while (nx < le && ((unsigned char)s[nx] & 0xC0) == 0x80) nx++;   /* the next code point */
            if (nc > W) {
                if (cui__ed_ws(c)) { while (i < le && cui__ed_ws(s[i])) i++; end = i; }   /* spaces hang past the edge */
                else end = brk > a ? brk : i > a ? i : nx;
                break;
            }
            if (!cui__ed_ws(c)) text = true;
            col = nc;
            i = nx;
            if (text && cui__ed_brk_after(c)) brk = i;
        }
        if (end >= le) break;
        a = end;
    }
    return n;
}

static int cui__ed_line_rows(const cui__ed *E, int line) {
    int ls, le;
    const char *s = cui__ed_line(E, line, &ls, &le);
    if (le - ls <= E->wcols && !memchr(s + ls, '\t', (size_t)(le - ls))) return 1;
    return cui__ed_wrap_walk(E, s, ls, le, NULL, 0);
}

static void cui__ed_wc_free(cui_ctx *ui, cui__ed_state *st) {
    if (!st->wc) return;
    cui__sq_free(ui, &st->wc->rows); cui__free(ui, st->wc);
    st->wc = NULL;
}
static void cui__ed_mc_free(cui_ctx *ui, cui__ed_state *st) {
    cui__ed_job_drop(&st->sjob);
    if (!st->mc) return;
    cui__sq_free(ui, &st->mc->st); cui__free(ui, st->mc);
    st->mc = NULL;
}
static void cui__ed_dtor(cui_ctx *ui, void *data) {
    cui__ed_state *st = data;
    cui__ed_wc_free(ui, st); cui__ed_mc_free(ui, st);
    cui__ed_job_drop(&st->rjob);   /* a job still running only hears that it may stop: it frees itself */
    for (int i = 0; i < st->hist.n; i++) cui__ed_rec_free(ui, &st->hist.r[i]);
    cui__free(ui, st->hist.r);
    cui__free(ui, st->ix.ls);
    free(st->pend_text);
    free(st->pend_lb);
    cui__free(ui, st->fc.b);
    for (int i = 0; i < st->ed.undo_count; i++) cui__free(ui, st->ed.undo[i]);   /* the text fields' history, unused here */
}

/* Bring the counts up to date at the start of a frame: all lines are counted when there are
 * none yet, the text changed outside the editor, or the row width or tab size changed. */
static void cui__ed_wrap_sync(cui__ed *E) {
    cui_ctx *ui = E->ui;
    cui__ed_state *st = E->st;
    if (!st->wc) {
        st->wc = cui__alloc(ui, sizeof *st->wc);
        memset(st->wc, 0, sizeof *st->wc);
        st->wc->rows.sums = true;
    }
    cui__ed_wc *wc = st->wc;
    int nl = E->nl, key = E->wcols * 32 + E->tab;
    if (wc->ok && wc->n == nl && wc->key == key) return;
    cui__sq *q = &wc->rows;
    cui__sq_fill(ui, q, nl, 1);   /* full blocks in order: line i is block i / B, slot i % B */
    if (!E->doc) {
        const char *tab_at = NULL, *tend = E->buf + E->len;   /* the next tab: one scan finds them all, line by line */
        for (int i = 0; i < nl; i++) {
            int ls = E->ls[i], n = E->ls[i + 1] - 1 - ls, r;
            if (n <= E->wcols) {   /* fits: one row, unless a tab may widen it (then its count is found when in view) */
                if (!tab_at || tab_at < E->buf + ls) { tab_at = cui__ed_find_byte(E->buf + ls, (size_t)(E->len - ls), '\t'); if (!tab_at) tab_at = tend; }
                r = tab_at >= E->buf + ls + n ? 1 : -1;
            } else r = -((n + E->wcols - 1) / E->wcols);
            q->b[i / CUI__SQ_B].v[i % CUI__SQ_B] = r;
        }
    } else {
        cui__ed_lw w = cui__ed_lw_at(E, 0);
        int ls, le, i = 0;
        for (const char *s; i < nl && (s = cui__ed_lw_next(&w, &ls, &le)) != NULL; i++) {
            int n = le - ls;
            int r = n > E->wcols ? -((n + E->wcols - 1) / E->wcols) : memchr(s + ls, '\t', (size_t)n) ? -1 : 1;
            q->b[i / CUI__SQ_B].v[i % CUI__SQ_B] = r;
        }
        cui__ed_lw_done(&w);
    }
    for (int b = 0; b < q->nb; b++) {
        int s = 0;
        for (int k = 0; k < q->b[b].n; k++) s += abs(q->b[b].v[k]);
        q->b[b].sum = s;
    }
    q->fixed = 0;
    wc->n = nl; wc->ok = true; wc->key = key;
}

/* An edit replaced lines [la, lb] with lines [la, la + k]: those are counted, the rest move. */
static void cui__ed_wrap_splice(cui__ed *E, int la, int lb, int k) {
    cui__ed_wc *wc = E->st->wc;
    cui__sq_splice(E->ui, &wc->rows, la, lb - la + 1, k + 1, 1);
    wc->n += k - (lb - la);
    for (int i = la; i <= la + k; i++) cui__sq_set(E->ui, &wc->rows, i, cui__ed_line_rows(E, i));
}

/* Count lines [l0, l1) whose rows are still a guess; the rows below them move. Returns
 * whether any count differed from its guess. */
static bool cui__ed_wrap_count(const cui__ed *E, int l0, int l1) {
    cui__ed_wc *wc = E->st->wc;
    bool moved = false;
    for (int i = CUI_MAX(0, l0); i < CUI_MIN(l1, wc->n); i++) {
        int v = cui__sq_get(E->ui, &wc->rows, i);
        if (v > 0) continue;
        int r = cui__ed_line_rows(E, i);
        if (r != -v) moved = true;
        cui__sq_set(E->ui, &wc->rows, i, r);
    }
    return moved;
}

static int cui__ed_total_rows(const cui__ed *E) { return cui__sq_total(E->ui, &E->st->wc->rows); }
static int cui__ed_row_of_line(const cui__ed *E, int line) { return cui__sq_prefix(E->ui, &E->st->wc->rows, line); }

/* The line that row `row` (counted from the top of the text) belongs to. */
static int cui__ed_line_of_row(const cui__ed *E, int row) {
    return CUI_CLAMP(cui__sq_find(E->ui, &E->st->wc->rows, CUI_MAX(0, row)), 0, E->nl - 1);
}

/* A line cut into rows: row j is bytes [rs[j], rs[j + 1]), the last one up to le. */
typedef struct { int line, ls, le, n, ind; int *rs; const char *s; } cui__ed_lr;

static void cui__ed_lr_get(const cui__ed *E, int line, cui__ed_lr *R) {
    cui__ed_wrap_count(E, line, line + 1);
    int n = CUI_MAX(1, cui__sq_get(E->ui, &E->st->wc->rows, line));
    R->line = line;
    R->s = cui__ed_line(E, line, &R->ls, &R->le);
    R->rs = cui__frame_alloc(E->ui, sizeof(int) * (size_t)(n + 1));
    R->ind = cui__ed_wrap_ind(E, R->s, R->ls, R->le);
    R->n = CUI_MIN(n, cui__ed_wrap_walk(E, R->s, R->ls, R->le, R->rs, n));
    R->rs[R->n] = R->le;
}

/* The row of R holding pos; a caret on a row boundary belongs to the row below, unless `aff`. */
static int cui__ed_lr_row(const cui__ed_lr *R, int pos, bool aff) {
    int lo = 0, hi = R->n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (R->rs[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    if (aff && lo > 0 && pos == R->rs[lo]) lo--;
    return lo;
}

/* Where pos is: its row counted from the top of the text, and its column in that row. */
static int cui__ed_vrow(const cui__ed *E, int pos, int *col) {
    cui__ed_lr R;
    int line = cui__ed_line_of(E, pos);
    cui__ed_lr_get(E, line, &R);
    int j = cui__ed_lr_row(&R, pos, E->st->aff_pos == pos);
    if (col) *col = cui__ed_adv(R.s, R.rs[j], pos, E->tab, j ? R.ind : 0);
    return cui__ed_row_of_line(E, line) + j;
}

/* The byte nearest to column x of row `vrow`. *aff: it is the end of a row that goes on. */
static int cui__ed_vpick(const cui__ed *E, int vrow, float x, bool *aff) {
    cui__ed_lr R;
    vrow = CUI_CLAMP(vrow, 0, cui__ed_total_rows(E) - 1);
    int line = cui__ed_line_of_row(E, vrow);
    cui__ed_lr_get(E, line, &R);
    int j = CUI_CLAMP(vrow - cui__ed_row_of_line(E, line), 0, R.n - 1), e = R.rs[j + 1];
    int pos = cui__ed_at_col0(R.s, R.rs[j], e, x, E->tab, j ? R.ind : 0);
    if (aff) *aff = j + 1 < R.n && pos == e;
    return pos;
}

/* ---- documents ----
 * With `.doc` the text is a cui_text_doc that other editors, and the app, may change too. The
 * document logs every edit (its bytes, and its lines before and after). At the start of a frame,
 * and right after each of its own edits, an editor takes the edits it has not seen: its wrap
 * counts, line states and match counts are patched over the lines they touched (once, over the
 * range covering them all), its caret and selection move with the text, and an edit by someone
 * else above the view keeps the lines in view where they are. When the log no longer reaches
 * back that far, or the whole text was set, the caches start over as for a new text. */

/* Where a position goes when bytes [a, b0) became [a, b1): past the change it moves with the
 * text, inside the removed bytes it goes to the end of the new ones. */
static int cui__ed_map(int p, int a, int b0, int b1) { return p < a ? p : p >= b0 ? p + (b1 - b0) : b1; }

static void cui__ed_doc_sync(cui__ed *E, bool own) {
    cui__ed_state *st = E->st;
    cui_text_doc *d = E->doc;
    cui_edit_state *e = &st->ed;
    unsigned rev = cui_text_doc_revision(d), seen = st->doc_seen;
    E->len = cui_text_doc_length(d); E->nl = cui_text_doc_line_count(d); E->maxlen = cui__doc_maxlen(d);
    E->ck->q = NULL;   /* the pieces may have been cut */
    bool same = st->doc == d && st->doc_serial == cui__doc_serial(d);
    if (same && seen == rev) return;
    if (!same || rev - seen > CUI__DOC_LOG || (int)(seen - cui__doc_base_rev(d)) < 0) {   /* start over */
        if (st->wc) st->wc->ok = false;
        if (st->mc) st->mc->ok = false;
        st->fc.on = false;
        st->doc = d; st->doc_serial = cui__doc_serial(d); st->doc_seen = rev; st->doc_mine = 0;
        st->gen++;
        e->cursor = CUI_CLAMP(e->cursor, 0, E->len); e->anchor = CUI_CLAMP(e->anchor, 0, E->len);
        return;
    }
    cui_ctx *ui = E->ui;
    cui__ed_wc *wc = st->wc && st->wc->ok ? st->wc : NULL;
    double lh = st->lh_seen, top_row = 0;
    int top = -1;
    if (!own && lh > 0) {   /* the line at the top of the view, to keep there */
        int row = (int)floor(st->sy / lh);
        top = wc ? cui__sq_find(ui, &wc->rows, row) : row;
        top_row = wc ? cui__sq_prefix(ui, &wc->rows, top) : top;
    }
    bool on = false, lon = false;
    int a = 0, b0 = 0, b1 = 0, la = 0, lb0 = 0, lb1 = 0, tlen = E->len;
    for (unsigned r = seen + 1; r != rev + 1; r++) tlen -= cui__doc_splice_at(d, r)->b1 - cui__doc_splice_at(d, r)->b0;   /* the length before them */
    for (unsigned r = seen + 1; r != rev + 1; r++) {
        const cui__tsplice *s = cui__doc_splice_at(d, r);
        bool whole = s->a == 0 && s->b0 == tlen;   /* the whole text replaced (a format, a reload): positions stay where they were */
        tlen += s->b1 - s->b0;
        cui__ed_span_add(&on, &a, &b0, &b1, s->a, s->b0 - s->a, s->b1 - s->a);
        cui__ed_span_add(&lon, &la, &lb0, &lb1, s->la, s->lb - s->la + 1, s->k + 1);   /* lines [la, lb] became [la, la + k] */
        if (own) cui__ed_span_add(&E->rep_on, &E->rep_a, &E->rep_b0, &E->rep_b1, s->a, s->b0 - s->a, s->b1 - s->a);
        else if (whole) {
            e->cursor = CUI_MIN(e->cursor, s->b1); e->anchor = CUI_MIN(e->anchor, s->b1); e->last_cursor = CUI_MIN(e->last_cursor, s->b1);
            st->drag_a = CUI_MIN(st->drag_a, s->b1); st->drag_b = CUI_MIN(st->drag_b, s->b1);
        } else {
            e->cursor = cui__ed_map(e->cursor, s->a, s->b0, s->b1);
            e->anchor = cui__ed_map(e->anchor, s->a, s->b0, s->b1);
            e->last_cursor = cui__ed_map(e->last_cursor, s->a, s->b0, s->b1);
            st->drag_a = cui__ed_map(st->drag_a, s->a, s->b0, s->b1);
            st->drag_b = cui__ed_map(st->drag_b, s->a, s->b0, s->b1);
        }
        if (top >= 0 && !whole) top = top < s->la ? top : top > s->lb ? top + s->k - (s->lb - s->la) : s->la;
        cui__ed_fc_splice(&st->fc, s->a, s->b0, s->b1);
    }
    /* old lines [la, lb0) are new lines [la, lb1) */
    int old_nl = E->nl - (lb1 - lb0), lb = lb0 - 1, k = lb1 - 1 - la;
    if (wc) {
        if (wc->n != old_nl || k > CUI__ED_BIG_LINES || lb - la > CUI__ED_BIG_LINES || !E->wcols) wc->ok = false;   /* counted again, as when opening */
        else cui__ed_wrap_splice(E, la, lb, k);
    }
    if (st->mc && st->mc->ok) {
        if (st->mc->n != old_nl) st->mc->ok = false;
        else cui__ed_mc_splice(ui, st->mc, la, lb, k);
    }
    if (top >= 0) {
        double nrow = wc && wc->ok ? cui__sq_prefix(ui, &wc->rows, CUI_MIN(top, wc->n - 1)) : top;
        st->sy = CUI_MAX(0.0, st->sy + (nrow - top_row) * lh);
    }
    if (!own) { e->last_edit = 0; st->aff_pos = -1; }   /* typing after someone else's edit is a new undo step */
    e->cursor = CUI_CLAMP(e->cursor, 0, E->len); e->anchor = CUI_CLAMP(e->anchor, 0, E->len);
    st->gen++;
    st->doc_seen = rev;
}

/* Bytes [a, b) become t[0, n): one move of the text after them. */
static void cui__ed_splice(cui__ed *E, int a, int b, const char *t, int n) {
    memmove(E->buf + a + n, E->buf + b, (size_t)(E->len - b + 1));
    if (!E->hint_nl) memcpy(E->buf + a, t, (size_t)n);   /* else reindex copies it */
    E->len += n - (b - a);
    cui__ed_reindex(E, a, b, n, t);
    cui__ed_span_add(&E->rep_on, &E->rep_a, &E->rep_b0, &E->rep_b1, a, b - a, n);
}

/* ---- undo ---- */

#define CUI__ED_UNDO_BYTES ((size_t)256 << 20)   /* past this the oldest steps go (one step always stays) */

static void cui__ed_hist_trim(cui_ctx *ui, cui__ed_hist *h) {
    while (h->n > 0 && (h->r[h->n - 1].group - h->r[0].group + 1 > CUI__UNDO_MAX ||
                        (h->bytes > CUI__ED_UNDO_BYTES && h->r[0].group != h->r[h->n - 1].group))) {
        int g = h->r[0].group, k = 0;
        while (k < h->n && h->r[k].group == g) { h->bytes -= (size_t)(h->r[k].del_n + h->r[k].ins_n); cui__ed_rec_free(ui, &h->r[k]); k++; }
        memmove(h->r, h->r + k, sizeof *h->r * (size_t)(h->n - k));
        h->n -= k; h->pos -= k;
    }
}

/* The record for an edit of [a, b) into m bytes, about to be made: it holds the removed bytes,
 * and room for the new ones (filled by the caller). kind: 1 typing, 2 deleting, 3 other; as in
 * the text fields, edits of the same kind at the caret within a second are one step, and
 * typing or deleting next to the last record grows that record. */
static char *cui__ed_record(cui__ed *E, int a, int b, int m, int kind, char *own) {
    cui_ctx *ui = E->ui;
    cui__ed_hist *h = &E->st->hist;
    cui_edit_state *e = &E->st->ed;
    bool merge = !own && ui->time - e->last_edit < 1.0 && h->pos > 0 && !h->r[h->pos - 1].ins && h->pos == h->n && kind == e->last_kind && kind != 3 && e->cursor == e->last_cursor;
    e->last_edit = ui->time;
    e->last_kind = kind;
    for (int i = h->pos; i < h->n; i++) { h->bytes -= (size_t)(h->r[i].del_n + h->r[i].ins_n); cui__ed_rec_free(ui, &h->r[i]); }
    h->n = h->pos;
    int del = b - a;
    h->bytes += (size_t)(del + m);
    if (merge) {
        cui__ed_rec *q = &h->r[h->n - 1];
        if (del == 0 && q->del_n == 0 && a == q->at + q->ins_n) {   /* typing on */
            q->data = cui__realloc(ui, q->data, (size_t)(q->ins_n + m) + 1);
            q->ins_n += m;
            return q->data + q->ins_n - m;
        }
        if (m == 0 && q->ins_n == 0 && (b == q->at || a == q->at)) {   /* Backspace or Delete on */
            char *d = cui__alloc(ui, (size_t)(q->del_n + del) + 1);
            if (b == q->at) { memcpy(d, E->buf + a, (size_t)del); memcpy(d + del, q->data, (size_t)q->del_n); q->at = a; }
            else { memcpy(d, q->data, (size_t)q->del_n); memcpy(d + q->del_n, E->buf + a, (size_t)del); }
            cui__free(ui, q->data);
            q->data = d;
            q->del_n += del;
            return d + q->del_n;
        }
    } else {
        h->group = (h->n ? h->r[h->n - 1].group : 0) + 1;
    }
    CUI__GROW(ui, h->r, h->n, h->cap, 1);
    cui__ed_rec *q = &h->r[h->n++];
    q->at = a; q->del_n = del; q->ins_n = m;
    q->caret = e->cursor;
    q->group = h->group;
    q->ins = own;
    q->data = cui__alloc(ui, (size_t)(own ? del : del + m) + 1);
    memcpy(q->data, E->buf + a, (size_t)del);
    h->pos = h->n;
    cui__ed_hist_trim(ui, h);
    return !own && h->n ? h->r[h->n - 1].data + del : NULL;
}

/* One step back, or forward with redo; false when there is none (or it no longer fits). The
 * caret lands where it was before the step, or after it when it is redone. */
static bool cui__ed_undo(cui__ed *E, bool redo) {
    cui__ed_hist *h = &E->st->hist;
    cui_edit_state *e = &E->st->ed;
    if (E->doc) {   /* the document's history, shared by its editors */
        int caret = 0;
        if (!cui__doc_undo(E->doc, redo, &caret)) return false;
        cui__ed_doc_sync(E, true);
        e->cursor = e->anchor = CUI_CLAMP(caret, 0, E->len);
        e->last_edit = 0;
        return true;
    }
    if (redo ? h->pos >= h->n : h->pos <= 0) return false;
    int g = h->r[redo ? h->pos : h->pos - 1].group, i = h->pos, len = E->len, most = len;
    while (redo ? i < h->n && h->r[i].group == g : i > 0 && h->r[i - 1].group == g) {   /* it fits all the way */
        const cui__ed_rec *q = &h->r[redo ? i++ : --i];
        len += redo ? q->ins_n - q->del_n : q->del_n - q->ins_n;
        most = CUI_MAX(most, len);
    }
    if ((size_t)most + 1 > E->cap) { E->refused = true; return false; }
    int caret = 0;
    while (redo ? h->pos < h->n && h->r[h->pos].group == g : h->pos > 0 && h->r[h->pos - 1].group == g) {
        const cui__ed_rec *q = &h->r[redo ? h->pos++ : --h->pos];
        if (redo) { cui__ed_splice(E, q->at, q->at + q->del_n, cui__ed_rec_ins(q), q->ins_n); caret = q->at + q->ins_n; }
        else { cui__ed_splice(E, q->at, q->at + q->ins_n, q->data, q->del_n); caret = q->caret; }
    }
    e->cursor = e->anchor = CUI_CLAMP(caret, 0, E->len);
    e->last_edit = 0;
    return true;
}

/* Edits this big keep the caller's text as the record's inserted bytes instead of copying it. */
#define CUI__ED_KEEP_BYTES 65536

/* Replace [a, b) with t[0, n). An edit that does not fit in the buffer is refused whole and
 * reported; the caret lands after the new text. Carriage returns are left out. *own is t when
 * the caller gave its own allocation: it may be changed in place and, for a big edit, becomes
 * the undo record's (then *own is NULL: the caller has nothing to free). */
static bool cui__ed_replace_x(cui__ed *E, int a, int b, const char *t, int n, int kind, char **own) {
    int *lb = E->hint_lb;   /* the line breaks of t, when known: the document takes them with t */
    E->hint_lb = NULL;
    if (E->ro) { E->hint_nl = E->hint_cr = 0; free(lb); return false; }
    const char *cr = E->hint_cr == 1 ? NULL : cui__ed_find_byte(t, (size_t)n, '\r');
    E->hint_cr = 0;
    int m = cr ? n - (int)cui__ed_count_byte(cr, (size_t)(n - (int)(cr - t)), '\r') : n;
    if (E->doc) {   /* no capacity; the document keeps the undo step */
        int nlc = E->hint_nl - 1;
        E->hint_nl = 0;
        if (cr) { free(lb); lb = NULL; }   /* squeezed: the offsets are not those of t any more */
        if (m == 0 && b == a) { free(lb); return false; }
        cui_ctx *ui = E->ui;
        cui__ed_state *st = E->st;
        cui_edit_state *e = &st->ed;
        if (cr) {   /* without carriage returns: squeezed in place in the caller's own copy, or copied */
            char *o;
            if (own && *own) o = *own;
            else { o = cui__frame_alloc(ui, (size_t)m + 1); memcpy(o, t, (size_t)(cr - t)); }
            char *w = o + (cr - t);
            for (const char *p = cr; p < t + n; p++) if (*p != '\r') *w++ = *p;
            t = o;
        }
        unsigned rev = cui_text_doc_revision(E->doc);
        bool merge = ui->time - e->last_edit < 1.0 && kind == e->last_kind && kind != 3 && e->cursor == e->last_cursor &&
                     st->doc_mine == rev && st->doc_seen == rev;
        e->last_edit = ui->time;
        e->last_kind = kind;
        char *give = own && *own && t == *own && m >= CUI__ED_KEEP_BYTES ? *own : NULL;   /* a big text of the caller's own: the document takes it */
        if (give) *own = NULL;
        if (!give) { free(lb); lb = NULL; }
        if (!cui__doc_edit(E->doc, a, b, t, m, give, NULL, NULL, e->cursor, merge, lb, nlc)) { E->refused = true; return false; }
        cui__ed_doc_sync(E, true);
        st->doc_mine = cui_text_doc_revision(E->doc);
        e->cursor = e->anchor = e->last_cursor = a + m;
        E->changed = E->reveal = true;
        st->pref_col = -1;
        st->aff_pos = -1;
        return true;
    }
    free(lb);   /* a buffer finds them as it copies the text in */
    if ((size_t)(E->len - (b - a) + m) + 1 > E->cap) { E->refused = true; E->hint_nl = 0; return false; }
    if (m == 0 && b == a) { E->hint_nl = 0; return false; }
    if (cr && own && *own) {   /* squeezed in place */
        char *o = *own + (cr - t);
        for (const char *p = cr; p < t + n; p++) if (*p != '\r') *o++ = *p;
        cr = NULL;
    }
    cui_edit_state *e = &E->st->ed;
    bool keep = own && *own && m >= CUI__ED_KEEP_BYTES;
    char *ins = cui__ed_record(E, a, b, m, kind, keep ? *own : NULL);
    if (!keep) {   /* the new bytes go in the record, without carriage returns */
        if (!cr) memcpy(ins, t, (size_t)m);
        else { char *o = ins; for (int i = 0; i < n; i++) if (t[i] != '\r') *o++ = t[i]; }
    }
    cui__ed_splice(E, a, b, keep ? *own : ins, m);
    if (keep) *own = NULL;
    e->cursor = e->anchor = e->last_cursor = a + m;
    E->changed = E->reveal = true;
    E->st->pref_col = -1;
    E->st->aff_pos = -1;
    return true;
}
static bool cui__ed_replace(cui__ed *E, int a, int b, const char *t, int n, int kind) {
    return cui__ed_replace_x(E, a, b, t, n, kind, NULL);
}

static void cui__ed_move(cui__ed *E, int to, bool extend) {
    cui_edit_state *e = &E->st->ed;
    e->cursor = CUI_CLAMP(to, 0, E->len);
    if (!extend) e->anchor = e->cursor;
    E->reveal = true;
    E->st->aff_pos = -1;
}

static int cui__ed_class(unsigned char c) { return cui__edit_is_word(c) ? 1 : (c == ' ' || c == '\t') ? 2 : 3; }

/* The word, space run or symbol at pos (double-click). */
/* code points and words through cui__ed_ch, for the arrows, Backspace and Delete in either mode */
static int cui__ed_prev(const cui__ed *E, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)cui__ed_ch(E, pos) & 0xC0) == 0x80) pos--;
    return pos;
}
static int cui__ed_next(const cui__ed *E, int pos) {
    if (pos >= E->len) return E->len;
    pos++;
    while (pos < E->len && ((unsigned char)cui__ed_ch(E, pos) & 0xC0) == 0x80) pos++;
    return pos;
}
static int cui__ed_word_left(const cui__ed *E, int pos) {
    while (pos > 0 && !cui__edit_is_word((unsigned char)cui__ed_ch(E, pos - 1))) pos--;
    while (pos > 0 && cui__edit_is_word((unsigned char)cui__ed_ch(E, pos - 1))) pos--;
    return pos;
}
static int cui__ed_word_right(const cui__ed *E, int pos) {
    while (pos < E->len && !cui__edit_is_word((unsigned char)cui__ed_ch(E, pos))) pos++;
    while (pos < E->len && cui__edit_is_word((unsigned char)cui__ed_ch(E, pos))) pos++;
    return pos;
}

/* The word, space run or symbol at pos (double-click). */
static void cui__ed_word_at(const cui__ed *E, int pos, int *a, int *b) {
    int line = cui__ed_line_of(E, pos), ls, le;
    const char *s = cui__ed_line(E, line, &ls, &le);
    int at = pos;
    if (at >= le) at = pos - 1;
    if (at < ls) { *a = *b = pos; return; }
    int k = cui__ed_class((unsigned char)s[at]);
    if (k == 3) { *a = at; *b = cui__utf8_next(s, le, at); return; }
    int i = at, j = at;
    while (i > ls && cui__ed_class((unsigned char)s[i - 1]) == k) i--;
    while (j < le && cui__ed_class((unsigned char)s[j]) == k) j++;
    *a = i; *b = j;
}

/* The whole line at pos, with its newline (triple-click, the gutter). */
static void cui__ed_line_at(const cui__ed *E, int pos, int *a, int *b) {
    int line = cui__ed_line_of(E, pos);
    *a = cui__ed_ls(E, line);
    *b = CUI_MIN(cui__ed_ls(E, line + 1), E->len);
}

/* ---- brackets ---- */

static int cui__ed_partner(char c, int *dir) {
    switch (c) {
    case '(': *dir = 1; return ')';
    case '[': *dir = 1; return ']';
    case '{': *dir = 1; return '}';
    case ')': *dir = -1; return '(';
    case ']': *dir = -1; return '[';
    case '}': *dir = -1; return '{';
    default: return 0;
    }
}

#define CUI__ED_BRACKET_REACH (1 << 20)   /* a partner further than this is not looked for: the walk is a byte loop */

static int cui__ed_match(const cui__ed *E, int at) {
    char self = cui__ed_ch(E, at);
    int dir = 0, other = cui__ed_partner(self, &dir), depth = 0;
    int lo = CUI_MAX(0, at - CUI__ED_BRACKET_REACH), hi = CUI_MIN(E->len, at + CUI__ED_BRACKET_REACH);
    for (int i = at; i >= lo && i < hi;) {   /* a piece at a time */
        int cs, ce;
        const char *s = cui__ed_chunk(E, i, &cs, &ce);
        int stop = dir > 0 ? CUI_MIN(ce, hi) : CUI_MAX(cs, lo) - 1;
        for (; i != stop; i += dir) {
            if (s[i] == self) depth++;
            else if (s[i] == other && --depth == 0) return i;
        }
    }
    return -1;
}

/* The bracket beside the caret (before it first) and its partner. An unmatched bracket
 * reads to the end of the text, so the answer is kept until the caret or the text changes. */
static void cui__ed_brackets(const cui__ed *E, int caret, int *a, int *b) {
    cui__ed_state *st = E->st;
    if (st->br_ok && st->br_gen == st->gen && st->br_at == caret) { *a = st->br_a; *b = st->br_b; return; }
    int dir;
    *a = *b = -1;
    for (int k = 0; k < 2; k++) {
        int at = k == 0 ? caret - 1 : caret;
        if (at < 0 || at >= E->len || !cui__ed_partner(cui__ed_ch(E, at), &dir)) continue;
        int m = cui__ed_match(E, at);
        if (m >= 0) { *a = at; *b = m; break; }
    }
    st->br_ok = true; st->br_gen = st->gen; st->br_at = caret; st->br_a = *a; st->br_b = *b;
}

/* ---- find ---- */

/* what to look for: the text or the pattern, and the three toggles */
struct cui__ed_q { const char *q; int qn; bool cs, word, rx; };

static cui__ed_q cui__ed_q_of(const cui__ed_state *st) {
    cui__ed_q Q = { st->query, (int)strlen(st->query), st->f_case, st->f_word, st->f_rx };
    return Q;
}

/* why a pattern does not compile (it then finds nothing), or NULL */
static const char *cui__ed_q_bad(const cui__ed_q *Q) {
    const char *err = NULL;
    if (Q->rx && Q->qn > 0 && cui_regex_search(Q->q, 0, "", 0, 0, NULL, &err) < 0) return err ? err : "Invalid pattern";
    return NULL;
}

static bool cui__ed_hit(const cui__ed_q *Q, const char *s, int i) {
    if (Q->cs) return memcmp(s + i, Q->q, (size_t)Q->qn) == 0;
    for (int k = 0; k < Q->qn; k++) if (cui__ascii_lower((unsigned char)s[i + k]) != cui__ascii_lower((unsigned char)Q->q[k])) return false;
    return true;
}

/* the first place in s[p, lim) the text's first byte is, in either case unless matching case; lim when none */
static int cui__ed_q_scan(const cui__ed_q *Q, const char *s, int p, int lim) {
    if (p >= lim) return lim;
    int lo = cui__ascii_lower((unsigned char)Q->q[0]), up = lo >= 'a' && lo <= 'z' ? lo - 32 : lo;
    if (Q->cs || lo == up) {
        const char *f = cui__ed_find_byte(s + p, (size_t)(lim - p), Q->q[0]);
        return f ? (int)(f - s) : lim;
    }
    const char *f = cui__ed_find_byte(s + p, (size_t)(lim - p), (char)lo);
    int stop = f ? (int)(f - s) : lim;
    const char *g = cui__ed_find_byte(s + p, (size_t)(stop - p), (char)up);
    return g ? (int)(g - s) : stop;
}

/* The first match in the line [ls, le) that starts at or after `from`: 1 and its bytes
 * [*a, *b), and for a pattern its groups in `m` (as offsets into s); 0 for none, -1 for a
 * pattern that does not compile or backtracks too much. Matches never span lines. */
static int cui__ed_q_line(const cui__ed_q *Q, const char *s, int ls, int le, int from, int *a, int *b, cui_regex_match *m) {
    if (Q->qn <= 0) return 0;
    for (int p = CUI_MAX(from, ls); p <= le;) {
        int x, y;
        if (Q->rx) {
            cui_regex_match M;
            int r = cui_regex_search(Q->q, Q->cs ? 0 : CUI_REGEX_ICASE, s + ls, le - ls, p - ls, &M, NULL);
            if (r < 0) return -1;
            if (r == 0) return 0;
            x = ls + M.start; y = ls + M.end;
            if (y == x) { p = x + 1; continue; }   /* an empty match is not one */
            if (m) {
                *m = M;
                m->start = x; m->end = y;
                for (int g = 0; g <= M.groups; g++)
                    if (M.group_start[g] >= 0) { m->group_start[g] += ls; m->group_end[g] += ls; }
            }
        } else {
            if (p + Q->qn > le) return 0;
            p = cui__ed_q_scan(Q, s, p, le - Q->qn + 1);
            if (p + Q->qn > le) return 0;
            if (!cui__ed_hit(Q, s, p)) { p++; continue; }
            x = p; y = p + Q->qn;
        }
        if (Q->word && ((x > ls && cui__edit_is_word((unsigned char)s[x]) && cui__edit_is_word((unsigned char)s[x - 1])) ||
                        (y < le && cui__edit_is_word((unsigned char)s[y - 1]) && cui__edit_is_word((unsigned char)s[y])))) { p = x + 1; continue; }
        *a = x; *b = y;
        return 1;
    }
    return 0;
}

/* The last match in a line that starts before `limit`. */
static int cui__ed_q_last(const cui__ed_q *Q, const char *s, int ls, int le, int limit, int *a, int *b) {
    int found = 0;
    for (int p = ls;;) {
        int x, y, r = cui__ed_q_line(Q, s, ls, le, p, &x, &y, NULL);
        if (r < 0) return -1;
        if (r == 0 || x >= limit) return found;
        *a = x; *b = y; found = 1;
        p = x + 1;
    }
}

/* First match at or after from (dir > 0), or last one before from (dir < 0), wrapping
 * around the ends: 1 with the match, 0 for none, -1 for a bad pattern. */
static int cui__ed_find(const cui__ed *E, const cui__ed_q *Q, int from, int dir, int *a, int *b, cui_regex_match *m) {
    if (Q->qn <= 0) return 0;
    from = CUI_CLAMP(from, 0, E->len);
    int fline = cui__ed_line_of(E, from), fl = cui__ed_ls(E, fline), fle = cui__ed_end(E, fline);
    if (dir > 0) {
        for (int pass = 0; pass < 2; pass++) {
            int stop = pass ? fle : E->len, ls, le, r = 0;
            cui__ed_lw w = cui__ed_lw_at(E, pass ? 0 : fl);
            for (const char *s; (s = cui__ed_lw_next(&w, &ls, &le)) != NULL;) {
                r = cui__ed_q_line(Q, s, ls, le, pass ? 0 : from, a, b, m);
                if (r || le >= stop) break;
            }
            cui__ed_lw_done(&w);
            if (r) return r;
        }
        return 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        int limit = pass ? E->len + 1 : from;
        for (int line = pass ? E->nl - 1 : fline;; line--) {
            int ls, le;
            const char *s = cui__ed_line(E, line, &ls, &le);
            int r = cui__ed_q_last(Q, s, ls, le, limit, a, b);
            if (r) {
                if (r > 0 && m) cui__ed_q_line(Q, s, ls, le, *a, a, b, m);
                return r;
            }
            if (line <= (pass ? fline : 0)) break;
        }
    }
    return 0;
}

/* ---- the match count (cui__ed_fc) ---- */

#define CUI__ED_FC_BLOCK (256 * 1024)   /* bytes of text in a count block, about */
#define CUI__ED_FC_FRAME (1 << 20)      /* bytes counted in a frame, at most (a pattern: a quarter): about 3 ms on a laptop CPU */

static bool cui__ed_fc_same(const cui__ed_fc *fc, const cui__ed_q *Q) {
    return fc->on && fc->cs == Q->cs && fc->word == Q->word && fc->rx == Q->rx && (int)strlen(fc->q) == Q->qn && !memcmp(fc->q, Q->q, (size_t)Q->qn);
}

/* the block holding byte x (the last one at or past the end) */
static int cui__ed_fc_block(const cui__ed_fc *fc, int x) {
    int lo = 0, hi = fc->n - 1;
    while (lo < hi) { int m = (lo + hi) / 2; if (fc->b[m].end > x) hi = m; else lo = m + 1; }
    return lo;
}

/* Bytes [a, b0) became [a, b1): the blocks it touched (and the one before, whose last line it may
 * have joined) become one block to count again; those after it move. */
static void cui__ed_fc_splice(cui__ed_fc *fc, int a, int b0, int b1) {
    if (!fc->on || fc->n == 0) return;
    int i0 = cui__ed_fc_block(fc, CUI_MAX(a - 1, 0)), i1 = cui__ed_fc_block(fc, b0), delta = b1 - b0;
    fc->b[i0].end = fc->b[i1].end + delta;
    fc->b[i0].count = -1;
    memmove(fc->b + i0 + 1, fc->b + i1 + 1, sizeof *fc->b * (size_t)(fc->n - i1 - 1));
    fc->n -= i1 - i0;
    for (int i = i0 + 1; i < fc->n; i++) fc->b[i].end += delta;
    fc->done = false;
    fc->ver++;
}

/* Counts the blocks not counted yet, about `budget` bytes of them; a big block is cut into
 * blocks of about CUI__ED_FC_BLOCK as it is counted. */
static void cui__ed_fc_run(const cui__ed *E, cui__ed_fc *fc, const cui__ed_q *Q, int budget) {
    int spent = 0;
    for (int i = 0; i < fc->n && spent < budget; i++) {
        if (fc->b[i].count >= 0) continue;
        int start = i ? fc->b[i - 1].end : 0, end = fc->b[i].end, cnt = 0, cut = -1, ls, le;
        cui__ed_lw w = cui__ed_lw_at(E, start);
        while (w.pos < end) {
            const char *s = cui__ed_lw_next(&w, &ls, &le);
            if (!s) break;
            for (int p = ls, a, b; cui__ed_q_line(Q, s, ls, le, p, &a, &b, NULL) > 0; p = b) cnt++;
            if (w.pos - start >= CUI__ED_FC_BLOCK && w.pos < end) { cut = w.pos; break; }
        }
        cui__ed_lw_done(&w);
        if (cut >= 0) {   /* the rest is a block of its own, counted next */
            CUI__GROW(E->ui, fc->b, fc->n, fc->cap, 1);
            memmove(fc->b + i + 2, fc->b + i + 1, sizeof *fc->b * (size_t)(fc->n - i - 1));
            fc->n++;
            fc->b[i + 1].end = end; fc->b[i + 1].count = -1;
            fc->b[i].end = cut;
        }
        fc->b[i].count = cnt;
        spent += fc->b[i].end - start;
        fc->ver++;
    }
    fc->total = 0; fc->done = true;
    for (int i = 0; i < fc->n; i++) { if (fc->b[i].count >= 0) fc->total += fc->b[i].count; else fc->done = false; }
}

/* The matches of the whole text, the number of them before s0, and whether one is [s0, s1).
 * Kept between frames and patched by edits; a big text is counted over a few frames (*partial
 * says the count is still growing). */
static int cui__ed_count(cui__ed *E, const cui__ed_q *Q, int s0, int s1, int *index, bool *on, bool *partial) {
    cui__ed_fc *fc = &E->st->fc;
    *index = 0; *on = false; *partial = false;
    if (Q->qn <= 0 || cui__ed_q_bad(Q)) { fc->on = false; return 0; }
    if (!cui__ed_fc_same(fc, Q)) {
        snprintf(fc->q, sizeof fc->q, "%.*s", Q->qn, Q->q);
        fc->cs = Q->cs; fc->word = Q->word; fc->rx = Q->rx; fc->on = true;
        CUI__GROW(E->ui, fc->b, 0, fc->cap, 1);
        fc->n = 1; fc->b[0].end = E->len; fc->b[0].count = -1;
        fc->done = false; fc->ver++;
    }
    if (!fc->done) cui__ed_fc_run(E, fc, Q, Q->rx ? CUI__ED_FC_FRAME / 4 : CUI__ED_FC_FRAME);
    if (!fc->done) { *partial = true; cui_request_redraw(E->ui); }
    int k = cui__ed_fc_block(fc, s0);
    if (fc->iver == fc->ver && fc->is0 == s0 && fc->is1 == s1) { *index = fc->index; *on = fc->ion; return fc->total; }
    int idx = 0;
    bool counted = true;
    for (int i = 0; i <= k; i++) { if (fc->b[i].count < 0) counted = false; else if (i < k) idx += fc->b[i].count; }
    if (!counted) return fc->total;
    int ls, le;
    cui__ed_lw w = cui__ed_lw_at(E, k ? fc->b[k - 1].end : 0);
    while (w.pos <= s0) {   /* the lines of s0's block up to its line */
        const char *s = cui__ed_lw_next(&w, &ls, &le);
        if (!s) break;
        for (int p = ls, a, b; cui__ed_q_line(Q, s, ls, le, p, &a, &b, NULL) > 0; p = b) {
            if (a < s0) idx++;
            else if (a == s0 && b == s1) *on = true;
        }
    }
    cui__ed_lw_done(&w);
    *index = idx;
    fc->iver = fc->ver; fc->is0 = s0; fc->is1 = s1; fc->index = idx; fc->ion = *on;
    return fc->total;
}

static bool cui__ed_find_go(cui__ed *E, int from, int dir) {
    cui__ed_q Q = cui__ed_q_of(E->st);
    int a, b;
    if (cui__ed_find(E, &Q, from, dir, &a, &b, NULL) <= 0) return false;
    E->st->ed.anchor = a;
    E->st->ed.cursor = b;
    E->reveal = E->center = true;
    E->st->pref_col = -1;
    return true;
}

/* The replacement for one match, written to out (NULL to measure); the length. With a
 * pattern, $0 and $& are the match, $1 to $9 its groups and $$ a dollar sign. */
static int cui__ed_expand(const cui__ed_q *Q, const char *with, const char *s, const cui_regex_match *m, char *out) {
    int n = 0;
    for (const char *w = with; *w; w++) {
        if (Q->rx && *w == '$' && w[1]) {
            int g = w[1] == '&' ? 0 : (w[1] >= '0' && w[1] <= '9') ? w[1] - '0' : -1;
            if (w[1] == '$') { if (out) out[n] = '$'; n++; w++; continue; }
            if (g >= 0) {
                w++;
                if (g <= m->groups && m->group_start[g] >= 0 && m->group_end[g] >= m->group_start[g]) {
                    int gn = m->group_end[g] - m->group_start[g];
                    if (out) memcpy(out + n, s + m->group_start[g], (size_t)gn);
                    n += gn;
                }
                continue;
            }
        }
        if (out) out[n] = *w;
        n++;
    }
    return n;
}

/* make room for `more` bytes in a growing malloc'd text; false when memory ran out */
static bool cui__ed_room(char **o, size_t *cap, size_t n, size_t more) {
    if (n + more + 1 <= *cap) return true;
    size_t c = CUI_MAX(*cap + *cap / 2, n + more + 1 + 4096);
    char *p = realloc(*o, c);
    if (!p) return false;
    *o = p; *cap = c;
    return true;
}

/* Every match replaced, from the first to the last, in one pass: the new text of that stretch in
 * *out (malloc'd), the stretch [*first, *last) and the number of matches. The text between
 * matches is copied line by line from where it lies (what follows the last match is copied too,
 * and cut off at the end). Returns the length, or -1 for a bad pattern (or no memory). */
static int cui__ed_rall(const cui__ed *E, const cui__ed_q *Q, const char *with, char **out, int *first, int *last, int *count) {
    int cnt = 0, ls, le;
    size_t n = 0, cap = 0, keep = 0;
    char *o = NULL;
    bool ok = true;
    cui__ed_lw w = cui__ed_lw_at(E, 0);
    for (const char *s; ok && (s = cui__ed_lw_next(&w, &ls, &le)) != NULL;) {
        int pos = ls;
        for (int p = ls, a, b; ok; p = b) {
            cui_regex_match m;
            int r = cui__ed_q_line(Q, s, ls, le, p, &a, &b, &m);
            if (r < 0) { ok = false; break; }
            if (r == 0) break;
            if (cnt == 0) { *first = a; pos = a; cap = (size_t)(E->len - a) + 4096; o = malloc(cap); if (!o) { ok = false; break; } }
            int gap = a - pos, x = cui__ed_expand(Q, with, s, &m, NULL);
            if (!cui__ed_room(&o, &cap, n, (size_t)(gap + x))) { ok = false; break; }
            memcpy(o + n, s + pos, (size_t)gap); n += (size_t)gap;
            n += (size_t)cui__ed_expand(Q, with, s, &m, o + n);
            pos = b; cnt++;
            keep = n; *last = b;
        }
        if (ok && cnt > 0) {   /* the rest of the line, up to the next match (or cut off) */
            if (!cui__ed_room(&o, &cap, n, (size_t)(le - pos) + 1)) { ok = false; break; }
            memcpy(o + n, s + pos, (size_t)(le - pos)); n += (size_t)(le - pos);
            if (le < E->len) o[n++] = '\n';
        }
    }
    cui__ed_lw_done(&w);
    if (!ok) { free(o); return -1; }
    *count = cnt;
    if (o && cap > keep + keep / 16 + 65536) { char *p = realloc(o, keep + 1); if (p) o = p; }   /* it may become the text: no spare room */
    if (o) o[keep] = 0;
    *out = o;
    return (int)keep;
}

/* Replace: the selected match (then on to the next), or when the selection is not a match,
 * just on to the next one. */
static void cui__ed_replace_one(cui__ed *E, const char *with) {
    if (E->ro) return;
    cui__ed_q Q = cui__ed_q_of(E->st);
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor), a, b, ls, le;
    cui_regex_match m;
    memset(&m, 0, sizeof m);
    const char *s = cui__ed_line(E, cui__ed_line_of(E, s0), &ls, &le);
    if (s1 > s0 && s1 <= le && cui__ed_q_line(&Q, s, ls, le, s0, &a, &b, &m) == 1 && a == s0 && b == s1) {
        int n = cui__ed_expand(&Q, with, s, &m, NULL);
        char *t = cui__frame_alloc(E->ui, (size_t)n + 1);
        cui__ed_expand(&Q, with, s, &m, t);
        if (!cui__ed_replace(E, s0, s1, t, n, 3)) return;
        cui__ed_find_go(E, e->cursor, 1);
    } else cui__ed_find_go(E, s0, 1);
}

/* Replace all, as one undo step; if the result does not fit the buffer nothing changes. The new
 * text is the edit's own: a big one becomes the undo record's (or the document's) as it is. */
static void cui__ed_replace_all(cui__ed *E, const char *with) {
    cui__ed_q Q = cui__ed_q_of(E->st);
    int first = 0, last = 0, cnt = 0;
    char *t = NULL;
    if (E->ro || Q.qn <= 0) return;
    if (E->doc && E->len >= cui__ed_rall_bytes) {   /* worked out on another thread, applied when done */
        if (!E->st->rjob && !cui__ed_q_bad(&Q)) cui__ed_rjob_start(E, &Q, with);
        return;
    }
    int n = cui__ed_rall(E, &Q, with, &t, &first, &last, &cnt);
    if (n >= 0 && cnt > 0) cui__ed_replace_x(E, first, last, t, n, 3, &t);
    free(t);
}

/* ---- indentation ---- */

/* Tab and Shift+Tab over the selected lines (or the caret's line). */
static void cui__ed_indent(cui__ed *E, bool out) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    int l0 = cui__ed_line_of(E, s0), l1 = cui__ed_line_of(E, s1);
    if (s1 > s0 && l1 > l0 && s1 == cui__ed_ls(E, l1)) l1--;   /* a selection that ends at a line start leaves that line alone */
    int lines = l1 - l0 + 1, a = cui__ed_ls(E, l0), b = cui__ed_end(E, l1);
    int *start = cui__frame_alloc(E->ui, sizeof(int) * (size_t)lines * 2), *delta = start + lines;
    char *t = cui__frame_alloc(E->ui, (size_t)(b - a) + (size_t)lines * (size_t)E->tab + 1);
    const char *s = cui__ed_span(E, a, b);
    int n = 0, total = 0;
    for (int k = 0, next = a; k < lines; k++) {   /* the lines one after the other, through the span */
        int ls = next, i = ls;
        const char *nl = cui__ed_find_byte(s + ls, (size_t)(b - ls), '\n');
        int le = nl ? (int)(nl - s) : b;
        next = le + 1;
        start[k] = ls;
        delta[k] = 0;
        if (!out && le > ls) {
            if (E->use_tabs) t[n++] = '\t';
            else for (int j = 0; j < E->tab; j++) t[n++] = ' ';
            delta[k] = E->use_tabs ? 1 : E->tab;
        } else if (out) {
            if (i < le && s[i] == '\t') i++;
            else while (i < le && i - ls < E->tab && s[i] == ' ') i++;
            delta[k] = -(i - ls);
        }
        memcpy(t + n, s + i, (size_t)(le - i));
        n += le - i;
        if (k < lines - 1) t[n++] = '\n';
        total += delta[k];
    }
    bool any = false;
    for (int k = 0; k < lines; k++) any |= delta[k] != 0;
    if (!any) return;
    int c0 = e->cursor, a0 = e->anchor;
    if (!cui__ed_replace(E, a, b, t, n, 3)) return;
    /* keep the selection on the same text */
    int map[2], src[2] = { c0, a0 };
    for (int m = 0; m < 2; m++) {
        int p = src[m];
        if (p < a) { map[m] = p; continue; }
        if (p > b) { map[m] = p + total; continue; }
        int k = lines - 1, before = 0;
        while (k > 0 && start[k] > p) k--;
        for (int j = 0; j < k; j++) before += delta[j];
        int own = out ? -CUI_MIN(-delta[k], p - start[k]) : (p > start[k] ? delta[k] : 0);
        map[m] = p + before + own;
    }
    e->cursor = e->last_cursor = map[0];
    e->anchor = map[1];
}

/* A markdown list item on line [ls, le): `pre` is where the marker starts (after the
 * indentation and any `>` quote), `mark` the end of the marker and its space, `content` where
 * the text starts (after a task box), `num` the number of an ordered item or -1. */
typedef struct { int pre, mark, content, num; char delim; bool task; } cui__ed_mdi;
static bool cui__ed_md_item(const char *s, int ls, int le, cui__ed_mdi *it) {
    int i = ls;
    for (;;) {
        while (i < le && cui__ed_ws(s[i])) i++;
        if (i < le && s[i] == '>') i++; else break;
    }
    it->pre = i; it->num = -1; it->delim = 0; it->task = false;
    if (i + 1 < le && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') it->mark = i + 2;
    else {
        int j = i, v = 0;
        while (j < le && j - i < 9 && s[j] >= '0' && s[j] <= '9') v = v * 10 + (s[j++] - '0');
        if (j == i || j + 1 >= le || (s[j] != '.' && s[j] != ')') || s[j + 1] != ' ') return false;
        it->num = v; it->delim = s[j]; it->mark = j + 2;
    }
    it->content = it->mark;
    int m = it->mark;
    if (m + 2 < le && s[m] == '[' && (s[m + 1] == ' ' || s[m + 1] == 'x' || s[m + 1] == 'X') && s[m + 2] == ']' && (m + 3 == le || s[m + 3] == ' ')) {
        it->task = true;
        it->content = m + 3 < le ? m + 4 : m + 3;
    }
    return true;
}

/* Enter in a markdown list: the next item (numbers count on, a task starts unchecked), or,
 * on an empty item, the marker goes. False: not a list line, or the caret is before the text. */
static bool cui__ed_md_newline(cui__ed *E) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    int ls, le;
    const char *s = cui__ed_line(E, cui__ed_line_of(E, s0), &ls, &le);
    cui__ed_mdi it;
    if (!cui__ed_md_item(s, ls, le, &it) || s0 < it.content) return false;
    if (s0 == s1) {
        int k = it.content;
        while (k < le && cui__ed_ws(s[k])) k++;
        if (k == le) {   /* an empty item: end the list, keep a quote's prefix */
            int keep = 0;
            for (int i = ls; i < it.pre; i++) if (s[i] == '>') keep = it.pre - ls;
            if (!cui__ed_replace(E, ls, le, s + ls, keep, 3)) return false;
            e->cursor = e->anchor = e->last_cursor = ls + keep;
            return true;
        }
    }
    int pre = it.pre - ls;
    char *t = cui__frame_alloc(E->ui, (size_t)pre + 32);
    int n = 0;
    t[n++] = '\n';
    memcpy(t + n, s + ls, (size_t)pre); n += pre;
    if (it.num >= 0) n += snprintf(t + n, 24, "%d%c ", it.num + 1, it.delim);
    else { memcpy(t + n, s + it.pre, 2); n += 2; }
    if (it.task) { memcpy(t + n, "[ ] ", 4); n += 4; }
    if (!cui__ed_replace(E, s0, s1, t, n, 3)) return false;
    e->cursor = e->anchor = e->last_cursor = s0 + n;
    return true;
}

/* Enter: a new line at the indentation of this one, a level deeper after an
 * opening brace (C-like languages) or a colon (Python); between a pair of
 * braces the closing one moves down a line of its own. */
static void cui__ed_newline(cui__ed *E, int lang_kind) {
    if (lang_kind == 3 && cui__ed_md_newline(E)) return;
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    int line = cui__ed_line_of(E, s0), ls = cui__ed_ls(E, line);
    int ind_end = CUI_MIN(cui__ed_first_text(E, line), s0);
    int k = s0;
    while (k > ls && (cui__ed_ch(E, k - 1) == ' ' || cui__ed_ch(E, k - 1) == '\t')) k--;
    char before = k > ls ? cui__ed_ch(E, k - 1) : 0;
    bool deeper = (lang_kind == 1 && (before == '{' || before == '[' || before == '(')) || (lang_kind == 2 && before == ':');
    int j = s1;
    while (j < E->len && (cui__ed_ch(E, j) == ' ' || cui__ed_ch(E, j) == '\t')) j++;
    char after = j < E->len ? cui__ed_ch(E, j) : 0;
    bool split = lang_kind == 1 && ((before == '{' && after == '}') || (before == '[' && after == ']') || (before == '(' && after == ')'));
    int ind = ind_end - ls, unit = E->use_tabs ? 1 : E->tab;
    char *t = cui__frame_alloc(E->ui, (size_t)(ind * 2 + unit + 3));
    int n = 0;
    t[n++] = '\n';
    cui__ed_read(E, ls, ls + ind, t + n); n += ind;
    if (deeper) { if (E->use_tabs) t[n++] = '\t'; else for (int i = 0; i < unit; i++) t[n++] = ' '; }
    int caret = n;
    if (split) { t[n++] = '\n'; cui__ed_read(E, ls, ls + ind, t + n); n += ind; }
    if (!cui__ed_replace(E, s0, split ? j : s1, t, n, 3)) return;
    e->cursor = e->anchor = e->last_cursor = s0 + caret;
}

/* Typing: replaces the selection. A closing brace typed on a line of only
 * indentation steps back a level. */
static void cui__ed_type(cui__ed *E, const char *text, int n, int lang_kind) {
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (lang_kind == 1 && n == 1 && (text[0] == '}' || text[0] == ']' || text[0] == ')') && s0 == s1) {
        int line = cui__ed_line_of(E, s0), ls = cui__ed_ls(E, line);
        if (cui__ed_first_text(E, line) == s0 && s0 > ls && text[0] == '}') {
            int i = s0;
            if (cui__ed_ch(E, i - 1) == '\t') i--;
            else while (i > ls && s0 - i < E->tab && cui__ed_ch(E, i - 1) == ' ') i--;
            char *t = cui__frame_alloc(E->ui, (size_t)(i - ls) + 2);
            cui__ed_read(E, ls, i, t);
            t[i - ls] = '}';
            cui__ed_replace(E, ls, s0, t, i - ls + 1, 1);
            return;
        }
    }
    cui__ed_replace(E, s0, s1, text, n, s1 > s0 ? 3 : 1);
}

/* Whether a block comment is open after s[i, e) when one was open before
 * (in): a quick scan that follows the tokenizer (strings end at the line
 * end; a line without a '/' cannot open one). */
static bool cui__ed_scan_line(const cui__lang *L, const char *s, int i, int e, bool in) {
    if (!in && !memchr(s + i, '/', (size_t)(e - i))) return false;
    size_t lc = L->line_comment ? strlen(L->line_comment) : 0;
    while (i < e) {
        if (in) {
            while (i < e && !(s[i] == '*' && i + 1 < e && s[i + 1] == '/')) i++;
            if (i < e) { i += 2; in = false; }
            continue;
        }
        char c = s[i];
        if (lc && (size_t)(e - i) >= lc && strncmp(s + i, L->line_comment, lc) == 0) break;
        if (c == '/' && i + 1 < e && s[i + 1] == '*') { in = true; continue; }
        if (c == '"' || c == '\'' || (c == '`' && L->backtick)) {
            i++;
            while (i < e && s[i] != c) { if (s[i] == '\\' && i + 1 < e) i++; i++; }
            if (i < e) i++;
            continue;
        }
        i++;
    }
    return in;
}

/* The state after one line s[a, e), from the state before it. */
static int cui__ed_state_step(const cui__lang *L, const char *s, int a, int e, int state) {
    if (L->multi) { cui__tokenize_st(L, s + a, e - a, &state, NULL, 0); return state; }
    return cui__ed_scan_line(L, s, a, e, state != 0);
}

/* ---- work on another thread ----
 * Two things can take long on a big text: the line states down to the view after an edit far above
 * it (or a jump to the end of a fresh file), and Replace all. Each runs as a job on a thread of its
 * own, over text that cannot change under it: a snapshot of the document (of just the stretch it
 * reads), or a copy of that stretch of the buffer. A job touches nothing of the editor and nothing
 * of the context: it reads its own copy of what it needs and writes its own results. The editor
 * looks at it in each frame, takes its result if the text is still the one it was for (or the part
 * of it that still is) and drops it otherwise; meanwhile the frame asks to be drawn again shortly.
 * Both hold a reference: an editor freed while its job runs only tells it to stop, and the last one
 * to let go frees it. Where no thread can be started (WebAssembly without threads), the job runs at
 * once, as the work did before. */

/* line states found on the UI thread in a frame, in bytes of text, before the rest goes to a job */
int cui__ed_sync_bytes = 256 * 1024;
/* a document at least this long works out Replace all on another thread */
int cui__ed_rall_bytes = 1 << 20;
/* bytes of a buffer a job for line states copies, at most (more: another job after it) */
int cui__ed_copy_bytes = 8 << 20;
#define CUI__ED_PART_MIN 4096         /* text between matches shorter than this is copied, not pointed at */

struct cui__ed_job {
    volatile long refs, cancel, done, progress;
    int kind;                         /* 1 line states, 2 replace all */
    cui_text_snap *snap;              /* the text it reads: a snapshot of the document's [ta, tb), */
    char *copy;                       /* or a copy of the buffer's (copy[0] is byte ta) */
    int ta, tb;
    /* line states */
    cui__lang L;
    int v, n, state0;                 /* lines [v, v + n), the state before line v */
    int *out, got;                    /* out[i]: the state before line v + 1 + i, for i < got */
    int klo, khi, *kept;              /* the states kept from before an edit (see cui__ed_mc), before lines [klo, khi) */
    bool conv;                        /* it stopped where its states met the kept ones (after line v + got - 1) */
    unsigned epoch;
    /* replace all */
    char q[128], with[128];
    int qn; bool cs, word, rx;
    unsigned rev, serial;             /* the document's revision and serial it is for */
    cui__tpart *parts; int np, pcap;  /* [first, last) becomes these */
    char *r; int rn, rcap;            /* the bytes the parts take from */
    int *rnl, rnn;                    /* r's line breaks */
    int first, last, count, newlen;
    int err;                          /* 1 a pattern that failed, 2 stopped, 3 no memory */
};

static long cui__ed_atomic(volatile long *p, long add) {   /* the value after adding (0: a read) */
#if defined(_MSC_VER)
    return _InterlockedExchangeAdd(p, add) + add;
#else
    return __atomic_add_fetch(p, add, __ATOMIC_ACQ_REL);
#endif
}

static void cui__ed_job_release(cui__ed_job *J) {
    if (!J || cui__ed_atomic(&J->refs, -1) != 0) return;
    cui_text_snap_free(J->snap);
    free(J->copy); free(J->out); free(J->kept); free(J->parts); free(J->r); free(J->rnl);
    free(J);
}

/* the editor lets go of a job: it stops soon if it still runs */
static void cui__ed_job_drop(cui__ed_job **J) {
    if (!*J) return;
    cui__ed_atomic(&(*J)->cancel, 1);
    cui__ed_job_release(*J);
    *J = NULL;
}

static bool cui__ed_job_done(cui__ed_job *J) { return cui__ed_atomic(&J->done, 0) != 0; }
static bool cui__ed_job_stopped(cui__ed_job *J) { return cui__ed_atomic(&J->cancel, 0) != 0; }

/* piece i of the job's text: its bytes, the text's [*a, *b) */
static const char *cui__ed_job_piece(const cui__ed_job *J, int i, int *a, int *b) {
    if (J->snap) return cui__snap_piece(J->snap, i, a, b);
    *a = J->ta; *b = J->tb;
    return i == 0 ? J->copy : NULL;
}
static int cui__ed_job_pieces(const cui__ed_job *J) { return J->snap ? cui__snap_pieces(J->snap) : 1; }

/* bytes [a, b) of the job's text to out */
static void cui__ed_job_read(const cui__ed_job *J, int a, int b, char *out) {
    int lo = 0, hi = cui__ed_job_pieces(J) - 1, pa, pb;
    while (lo < hi) { int m = (lo + hi + 1) / 2; cui__ed_job_piece(J, m, &pa, &pb); if (pa <= a) lo = m; else hi = m - 1; }
    for (int i = lo; a < b; i++) {
        const char *p = cui__ed_job_piece(J, i, &pa, &pb);
        if (!p) break;
        int e = CUI_MIN(b, pb);
        if (e > a) { memcpy(out, p + (a - pa), (size_t)(e - a)); out += e - a; a = e; }
    }
}

/* Walks the lines of the job's text from ta on, as cui__ed_lw does the editor's: a line in one
 * piece is read where it lies, one across pieces is copied to scratch. */
typedef struct { const cui__ed_job *J; int pi, pa, pb, pos; const char *pp; char *scr; int scap; } cui__ed_jw;

static void cui__ed_jw_init(cui__ed_jw *w, const cui__ed_job *J) {
    memset(w, 0, sizeof *w);
    w->J = J; w->pos = J->ta;
    w->pp = cui__ed_job_piece(J, 0, &w->pa, &w->pb);
}

/* the next line: q[*ls, *le) (le: its line break or the end of the stretch); NULL past the end */
static const char *cui__ed_jw_next(cui__ed_jw *w, int *ls, int *le) {
    const cui__ed_job *J = w->J;
    static const char none[1];
    int a = w->pos, np = cui__ed_job_pieces(J), n = 0;
    if (a > J->tb) return NULL;
    *ls = a;
    for (bool first = true;; first = false) {
        while (a >= w->pb && w->pi + 1 < np) w->pp = cui__ed_job_piece(J, ++w->pi, &w->pa, &w->pb);
        if (a >= J->tb || !w->pp || a >= w->pb) break;
        const char *q = w->pp - w->pa, *p = cui__find_byte(q + a, (size_t)(w->pb - a), '\n');
        int e = p ? (int)(p - q) : w->pb;
        if (first && (p || w->pb >= J->tb)) { *le = e; w->pos = e + 1; return q; }   /* in one piece */
        if (n + (e - a) > w->scap) {
            w->scap = (n + (e - a)) * 2 + 1024;
            char *ns = realloc(w->scr, (size_t)w->scap);
            if (!ns) return NULL;
            w->scr = ns;
        }
        memcpy(w->scr + n, q + a, (size_t)(e - a));
        n += e - a; a = e;
        if (p) break;
    }
    *le = *ls + n; w->pos = *le + 1;
    return n ? w->scr - *ls : none - *ls;
}

/* the line states, from line v on; stops where they meet the kept ones */
static void cui__ed_job_states(cui__ed_job *J) {
    cui__ed_jw w;
    cui__ed_jw_init(&w, J);
    int state = J->state0, ls, le;
    for (int i = 0; i < J->n; i++) {
        if ((i & 1023) == 0 && cui__ed_job_stopped(J)) break;
        const char *s = cui__ed_jw_next(&w, &ls, &le);
        if (!s) break;
        state = cui__ed_state_step(&J->L, s, ls, le, state);
        J->out[i] = state;
        J->got = i + 1;
        int j = J->v + i + 1;
        if (j >= J->klo && j < J->khi && J->kept[j - J->klo] == state) { J->conv = true; break; }
    }
    free(w.scr);
}

/* room for more bytes in r */
static bool cui__ed_rj_room(cui__ed_job *J, int more) {
    if (J->rn + more <= J->rcap) return true;
    int c = CUI_MAX(J->rcap + J->rcap / 2, J->rn + more + 65536);
    char *p = realloc(J->r, (size_t)c);
    if (!p) return false;
    J->r = p; J->rcap = c;
    return true;
}
/* one more part: bytes [at, at + len) of the text, or (at < 0) the last len bytes put in r */
static bool cui__ed_rj_part(cui__ed_job *J, int at, int len) {
    J->newlen += len;
    if (at < 0) at = -(J->rn - len) - 1;
    cui__tpart *l = J->np ? &J->parts[J->np - 1] : NULL;
    if (l && at < 0 && l->at < 0 && -l->at - 1 + l->len == -at - 1) { l->len += len; return true; }   /* r runs on */
    if (J->np == J->pcap) {
        int c = J->pcap ? J->pcap * 2 : 1024;
        cui__tpart *p = realloc(J->parts, sizeof *p * (size_t)c);
        if (!p) return false;
        J->parts = p; J->pcap = c;
    }
    J->parts[J->np++] = (cui__tpart){ at, len };
    return true;
}

/* Replace all, as cui__ed_rall does it, as parts: the text between matches stays where it is when
 * it is long (a part that points at it); the replacements and short stretches between them go to r. */
static void cui__ed_job_rall(cui__ed_job *J) {
    cui__ed_q Q = { J->q, J->qn, J->cs, J->word, J->rx };
    cui__ed_jw w;
    cui__ed_jw_init(&w, J);
    int cur = -1, ls, le, lines = 0;
    bool ok = true;
    for (const char *s; ok && (s = cui__ed_jw_next(&w, &ls, &le)) != NULL;) {
        if ((++lines & 255) == 0) {
            if (cui__ed_job_stopped(J)) { J->err = 2; ok = false; break; }
            cui__ed_atomic(&J->progress, ls - J->progress);   /* only this thread writes it */
        }
        for (int p = ls, a, b; ok; p = b) {
            cui_regex_match m;
            int r = cui__ed_q_line(&Q, s, ls, le, p, &a, &b, &m);
            if (r < 0) { J->err = 1; ok = false; break; }
            if (r == 0) break;
            if (cur < 0) J->first = cur = a;
            int gap = a - cur, x = cui__ed_expand(&Q, J->with, s, &m, NULL);
            if (gap >= CUI__ED_PART_MIN) ok = cui__ed_rj_part(J, cur, gap) && cui__ed_rj_room(J, x);
            else {
                ok = cui__ed_rj_room(J, gap + x);
                if (ok && gap > 0) {
                    if (cur >= ls) memcpy(J->r + J->rn, s + cur, (size_t)gap);
                    else cui__ed_job_read(J, cur, a, J->r + J->rn);
                    J->rn += gap;
                    ok = cui__ed_rj_part(J, -1, gap);
                }
            }
            if (!ok) break;
            J->rn += cui__ed_expand(&Q, J->with, s, &m, J->r + J->rn);
            ok = cui__ed_rj_part(J, -1, x);
            cur = b; J->last = b; J->count++;
        }
    }
    free(w.scr);
    if (!ok) { if (!J->err) J->err = 3; return; }
    if (J->rn > 0) {   /* r's line breaks, found here rather than on the UI thread */
        if (J->rcap > J->rn) { char *p = realloc(J->r, (size_t)J->rn); if (p) { J->r = p; J->rcap = J->rn; } }
        J->rnn = (int)cui__count_byte(J->r, (size_t)J->rn, '\n');
        J->rnl = malloc(sizeof(int) * (size_t)CUI_MAX(1, J->rnn));
        if (!J->rnl) { J->err = 3; return; }
        int k = cui__fill_nl(J->r, 0, J->rn, J->rnl);
        for (int i = 0; i < k; i++) J->rnl[i]--;   /* the line break itself, not the byte after it */
    }
}

static void cui__ed_job_work(cui__ed_job *J) {
    if (J->kind == 1) cui__ed_job_states(J);
    else cui__ed_job_rall(J);
    cui__ed_atomic(&J->done, 1);
    cui__ed_job_release(J);
}

#if defined(_WIN32)
static void __cdecl cui__ed_thread_main(void *arg) { cui__ed_job_work(arg); }
static bool cui__ed_thread(cui__ed_job *J) { return _beginthread(cui__ed_thread_main, 0, J) != (uintptr_t)-1; }
#elif defined(CUI__ED_PTHREADS)
static void *cui__ed_thread_main(void *arg) { cui__ed_job_work(arg); return NULL; }
static bool cui__ed_thread(cui__ed_job *J) {
    pthread_attr_t at;
    pthread_t t;
    if (pthread_attr_init(&at) != 0) return false;
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    bool ok = pthread_create(&t, &at, cui__ed_thread_main, J) == 0;
    pthread_attr_destroy(&at);
    return ok;
}
#else
static bool cui__ed_thread(cui__ed_job *J) { (void)J; return false; }
#endif

/* starts the job on a thread, or runs it here when none can be had */
static void cui__ed_job_go(cui__ed_job *J) {
    J->refs = 2;   /* the editor's and the work's */
    if (!cui__ed_thread(J)) cui__ed_job_work(J);
}

/* ---- the line states ---- */

/* The states of lines [v, upto) go to a job, which reads a snapshot of them (a document) or a copy
 * (a buffer: at most cui__ed_copy_bytes bytes, and the next job goes on from there). False when it
 * could not be made (no memory): the states are then found here. */
static bool cui__ed_sjob_start(cui__ed *E, const cui__lang *L, int v, int upto) {
    cui_ctx *ui = E->ui;
    cui__ed_state *st = E->st;
    cui__ed_mc *mc = st->mc;
    int ta = cui__ed_ls(E, v), n = upto - v;
    if (!E->doc) n = CUI_MAX(1, CUI_MIN(n, cui__ed_line_of(E, ta + cui__ed_copy_bytes) - v));
    int tb = cui__ed_ls(E, v + n) - 1;
    cui__ed_job *J = calloc(1, sizeof *J);
    if (!J) return false;
    J->kind = 1; J->L = *L; J->v = v; J->n = n; J->epoch = mc->epoch;
    J->state0 = cui__sq_get(ui, &mc->st, v);
    J->ta = ta; J->tb = tb;
    J->out = malloc(sizeof(int) * (size_t)n);
    int klo = CUI_MAX(mc->kept0, v + 1), khi = CUI_MIN(mc->kept1, v + n + 1);
    if (khi > klo && (J->kept = malloc(sizeof(int) * (size_t)(khi - klo))) != NULL) {
        cui__sq_read(ui, &mc->st, klo, khi - klo, J->kept);
        J->klo = klo; J->khi = khi;
    }
    if (E->doc) J->snap = cui__doc_snapshot_range(E->doc, ta, tb);
    else if ((J->copy = malloc((size_t)(tb - ta) + 1)) != NULL) memcpy(J->copy, E->buf + ta, (size_t)(tb - ta));
    if (!J->out || !(J->snap || J->copy)) { J->refs = 1; cui__ed_job_release(J); return false; }
    mc->dirty = INT_MAX;
    st->sjob = J;
    cui__ed_job_go(J);
    return true;
}

/* A finished job's states go in, as far as the text is still the one they were found for: down to
 * the first line an edit touched since it began. A job that edits left nothing to give is dropped
 * at once. */
static void cui__ed_sjob_take(cui__ed *E) {
    cui__ed_state *st = E->st;
    cui__ed_mc *mc = st->mc;
    cui__ed_job *J = st->sjob;
    if (J->epoch != mc->epoch || mc->dirty <= J->v) { cui__ed_job_drop(&st->sjob); return; }
    if (!cui__ed_job_done(J)) return;
    int lim = J->v + J->got;
    bool conv = J->conv && mc->dirty == INT_MAX;
    lim = CUI_MIN(lim, mc->dirty);
    if (J->v <= mc->valid && lim > mc->valid) {
        cui__sq_write(E->ui, &mc->st, mc->valid + 1, lim - mc->valid, J->out + (mc->valid - J->v));
        mc->valid = lim;
        if (conv && lim == J->v + J->got) { mc->valid = mc->kept1 - 1; mc->kept0 = mc->kept1 = 0; }   /* as before the edit from here on */
    }
    cui__ed_job_drop(&st->sjob);
}

/* The line states (see cui__ed_mc) for language L, right from line 0 to line `upto`; false
 * when L has none. A few hundred kilobytes of lines are found here in a frame; the rest goes to a
 * job (above), and until it is done the lines below the last state known draw as well as they can
 * (see cui__edf_build_lines). */
static bool cui__ed_states(cui__ed *E, const cui__lang *L, int upto) {
    cui_ctx *ui = E->ui;
    cui__ed_state *st = E->st;
    if (!L->multi && !L->block_comments) return false;
    if (!st->mc) { st->mc = cui__alloc(ui, sizeof *st->mc); memset(st->mc, 0, sizeof *st->mc); }
    cui__ed_mc *mc = st->mc;
    int nl = E->nl;
    if (!mc->ok || mc->n != nl || mc->mode != L->mode || mc->lc != L->line_comment || mc->bt != L->backtick) {
        mc->ok = true; mc->n = nl; mc->mode = L->mode; mc->lc = L->line_comment; mc->bt = L->backtick;
        mc->valid = mc->kept0 = mc->kept1 = 0;
        mc->epoch++; mc->dirty = INT_MAX;
        cui__ed_job_drop(&st->sjob);
        cui__sq_fill(ui, &mc->st, nl + 1, 0);
    }
    cui__sq_set(ui, &mc->st, 0, L->markdown ? CUI__MD_START : 0);
    upto = CUI_CLAMP(upto, 0, nl);
    if (st->sjob) cui__ed_sjob_take(E);
    if (mc->valid >= upto) return true;
    if (st->sjob) { cui__wakeup_after(ui, 1.0f / 60.0f); return true; }   /* still at work: look again next frame */
    cui__ed_lw w = cui__ed_lw_at(E, cui__ed_ls(E, mc->valid));
    int state = cui__sq_get(ui, &mc->st, mc->valid), bytes = 0;
    for (int i = mc->valid; i < upto; i++) {
        int a, e;
        const char *s = cui__ed_lw_next(&w, &a, &e);
        if (!s) break;
        state = cui__ed_state_step(L, s, a, e, state);
        if (i + 1 >= mc->kept0 && i + 1 < mc->kept1 && cui__sq_get(ui, &mc->st, i + 1) == state) {   /* as before the edit: right down to kept1 */
            mc->valid = mc->kept1 - 1;
            mc->kept0 = mc->kept1 = 0;
            i = mc->valid - 1;
            if (mc->valid < upto) { w.pos = cui__ed_ls(E, mc->valid); state = cui__sq_get(ui, &mc->st, mc->valid); }
            continue;
        }
        cui__sq_set(ui, &mc->st, i + 1, state);
        mc->valid = i + 1;
        bytes += e - a + 1;
        if (bytes > cui__ed_sync_bytes && mc->valid < upto && cui__ed_sjob_start(E, L, mc->valid, upto)) {
            if (cui__ed_job_done(st->sjob)) cui__ed_sjob_take(E);   /* it ran here: no thread */
            if (st->sjob) cui__wakeup_after(ui, 1.0f / 60.0f);
            break;
        }
    }
    cui__ed_lw_done(&w);
    return true;
}

/* ---- Replace all on another thread ----
 * On a document of at least cui__ed_rall_bytes, Replace all finds the matches on a job over a
 * snapshot and works out the new text as parts (cui__doc_edit_parts), which the editor applies as one
 * edit when the job is done, if the document has not changed since: any edit to it, or closing the
 * find bar, drops the job. Meanwhile the find bar says "Replacing". */

/* the job's result goes in: as cui__ed_replace_x would put the same text */
static void cui__ed_rjob_apply(cui__ed *E, cui__ed_job *J) {
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    e->last_edit = E->ui->time;
    e->last_kind = 3;
    bool ok = cui__doc_edit_parts(E->doc, J->first, J->last, J->parts, J->np, J->r, J->rn, J->rnl, J->rnn, e->cursor);
    J->r = NULL; J->rnl = NULL;   /* the document's now (or freed) */
    if (!ok) { E->refused = true; return; }
    cui__ed_doc_sync(E, true);
    st->doc_mine = cui_text_doc_revision(E->doc);
    e->cursor = e->anchor = e->last_cursor = J->first + J->newlen;
    E->changed = E->reveal = true;
    st->pref_col = -1;
    st->aff_pos = -1;
}

/* At the start of a frame: the job is dropped if the text changed or the bar closed, applied if done. */
static void cui__ed_rjob_take(cui__ed *E, bool ro) {
    cui__ed_state *st = E->st;
    cui__ed_job *J = st->rjob;
    if (!E->doc || ro || st->mode != 1 || cui_text_doc_revision(E->doc) != J->rev || cui__doc_serial(E->doc) != J->serial) {
        cui__ed_job_drop(&st->rjob);
        return;
    }
    if (!cui__ed_job_done(J)) { cui__wakeup_after(E->ui, 1.0f / 60.0f); return; }
    if (!J->err && J->count > 0) cui__ed_rjob_apply(E, J);
    cui__ed_job_drop(&st->rjob);
}

static void cui__ed_rjob_start(cui__ed *E, const cui__ed_q *Q, const char *with) {
    cui__ed_state *st = E->st;
    cui__ed_job *J = calloc(1, sizeof *J);
    if (!J) return;
    J->kind = 2;
    J->qn = CUI_MIN(Q->qn, (int)sizeof J->q - 1);
    memcpy(J->q, Q->q, (size_t)J->qn);
    J->cs = Q->cs; J->word = Q->word; J->rx = Q->rx;
    snprintf(J->with, sizeof J->with, "%s", with);
    J->rev = cui_text_doc_revision(E->doc); J->serial = cui__doc_serial(E->doc);
    J->ta = 0; J->tb = E->len;
    J->snap = cui__doc_snapshot_range(E->doc, 0, E->len);
    if (!J->snap) { free(J); return; }
    st->rjob = J;
    cui__ed_job_go(J);
    if (cui__ed_job_done(J)) cui__ed_rjob_take(E, E->ro);   /* it ran here: no thread */
    else cui__wakeup_after(E->ui, 1.0f / 60.0f);
}

/* An edit replaced lines [la, lb] with lines [la, la + k]: the states below them are kept,
 * to be confirmed (see cui__ed_mc). */
static void cui__ed_mc_splice(cui_ctx *ui, cui__ed_mc *mc, int la, int lb, int k) {
    int d = k - (lb - la);
    cui__sq_splice(ui, &mc->st, la + 1, lb - la, k, 0);   /* the state after line lb is now the one after la + k */
    /* below the edit the states that were right are kept; or else those kept before, as far as
     * they reach past the edit (kept ones from two different texts never mix) */
    int k0 = 0, k1 = 0;
    if (mc->valid > lb) { k0 = lb + 1; k1 = mc->valid + 1; }
    else if (mc->kept1 > lb + 1) { k0 = CUI_MAX(mc->kept0, lb + 1); k1 = mc->kept1; }
    mc->kept0 = k1 > k0 ? k0 + d : 0;
    mc->kept1 = k1 > k0 ? k1 + d : 0;
    mc->valid = CUI_MIN(mc->valid, la);
    mc->dirty = CUI_MIN(mc->dirty, la);   /* a job's states down to line la still hold */
    mc->n += d;
}

/* ---- painting ---- */

typedef struct {
    cui_id sid;
    const char *text;           /* a copy of the lines in view; text[0] is byte `base` */
    int base, first, count, nl;
    const int *ls;              /* starts of the lines in view and the one after */
    bool in_block, json, empty, focused, show_line, ro;
    const int *mst;             /* languages with states: the state before each line in view (-1: not known yet, drawn plain) */
    cui__lang lang;
    int tab, cursor, anchor, caret_line, caret_col;
    int br_line[2], br_col[2];
    cui__ed_q fq;               /* the find bar's query, when it is open */
    const cui_text_mark *marks; int nmarks;   /* the app's marks that reach the lines in view */
    cui_color mark_colors[7];
    cui_color tone_tint[9], tone_line[9];   /* marks with a tone: the box, and the full color of a line */
    const char *placeholder;
    float size, lh, cw, gutter, view_h;
    double sy, max_sy;
    float sx, max_sx, bar_t;
    int bar_big;                /* 1 vertical thumb held or hovered, 2 horizontal */
    double caret_phase;
    cui_color colors[CUI__TK_COUNT];
    cui_color num, num_on, band, sel, match, match_edge, brk, brk_edge, caret, muted, line, bar;
    int vc0, vc1;               /* columns in view */
    bool wrap;                  /* rows instead of lines: lr[k] is line first + k cut into rows, starting at row lrow[k] */
    const cui__ed_lr *lr;
    const int *lrow;
    int caret_row, br_row[2];
} cui__ed_paint;

/* Draws s[a, b) from column col; tabs expand; only the columns in view. */
/* a plain token that is only spaces and tabs draws nothing (one that starts with a space may still hold words) */
static bool cui__ed_blank(const char *s, int a, int b) {
    for (int i = a; i < b; i++) if (s[i] != ' ' && s[i] != '	') return false;
    return true;
}

static void cui__ed_run(cui_ctx *ui, const cui__ed_paint *p, const char *s, int a, int b, int col, float x0, float y, cui_color c) {
    int i = a;
    while (i < b && col < p->vc1) {
        if (s[i] == '\t') { col = (col / p->tab + 1) * p->tab; i++; continue; }
        int j = i, cj = col;
        while (j < b && s[j] != '\t') { if (((unsigned char)s[j] & 0xC0) != 0x80) cj++; j++; }
        if (cj > p->vc0) {
            int i2 = i, c2 = col;
            while (c2 < p->vc0 && i2 < j) { i2 = cui__utf8_next(s, j, i2); c2++; }
            int j2 = i2, c3 = c2;
            while (j2 < j && c3 < p->vc1) { j2 = cui__utf8_next(s, j, j2); c3++; }
            if (j2 > i2) cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0 + (float)c2 * p->cw, y, c, s + i2, j2 - i2, true);
        }
        col = cj;
        i = j;
    }
}

/* The row of the top of line first + k. */
static double cui__ed_prow(const cui__ed_paint *p, int k) { return p->wrap ? (double)p->lrow[k] : (double)(p->first + k); }

/* One mark's part in a row: a tinted box, or a wavy or straight line under the text; x and w are
 * where it goes in the view, y the row's top. */
static void cui__ed_draw_mark(cui_ctx *ui, const cui__ed_paint *p, const cui_text_mark *mk, float x, float w, float y) {
    int tone = CUI_CLAMP(mk->tone, 0, 8);
    if (mk->style != CUI_MARK_WAVY && mk->style != CUI_MARK_UNDERLINE) {
        cui_color c = tone ? p->tone_tint[tone] : p->mark_colors[CUI_CLAMP(mk->color, 0, 6)];
        cui_draw_rect(ui, (cui_rect){ x, y + 1, w, p->lh - 2 }, CUI_CORNERS(2), c);
        return;
    }
    cui_color c = tone ? p->tone_line[tone] : p->mark_colors[CUI_CLAMP(mk->color, 0, 6)];
    float yy = y + p->lh - 3.0f;
    if (mk->style == CUI_MARK_UNDERLINE) { cui_draw_line(ui, (cui_vec2){ x, yy }, (cui_vec2){ x + w, yy }, 1.2f, c); return; }
    cui_vec2 pt[48];   /* a zigzag two points wide, drawn in runs that share their end point */
    int k = 0;
    for (float px = x;; px += 2.0f) {
        bool end = px >= x + w;
        if (end) px = x + w;
        pt[k++] = (cui_vec2){ px, yy + ((((int)((px - x) * 0.5f)) & 1) ? 1.4f : -0.4f) };
        if (end || k == 48) {
            cui_draw_polyline(ui, pt, k, 1.2f, c, false);
            if (end) break;
            pt[0] = pt[k - 1]; k = 1;
        }
    }
}

/* A wrapped line's rows: selection, marks, find matches, brackets and tokens, each
 * clipped to the row's bytes. Rows are bytes [rs[j], rs[j + 1]); the last row also
 * holds the position of the newline. */
static void cui__ed_paint_wrap(cui_ctx *ui, const cui__ed_paint *p, cui_rect r, float x0, float ty, int s0, int s1) {
    cui__tok *toks = NULL;
    int tok_cap = 0;
    bool block = p->in_block;
    cui__lang plain;   /* a line whose state is not known yet */
    memset(&plain, 0, sizeof plain);
    plain.plain = true;
    const char *s = p->text;
    for (int k = 0; k < p->count; k++) {
        const cui__ed_lr *R = &p->lr[k];
        int ls = R->ls - p->base, le = R->le - p->base;
        int nn = le - ls;
        if (nn + 1 > tok_cap) { tok_cap = nn + 1; toks = cui__frame_alloc(ui, sizeof *toks * (size_t)tok_cap); }
        int nt;
        if (p->mst && p->mst[k] < 0) nt = cui__tokenize(&plain, false, s + ls, nn, &block, toks, tok_cap);   /* its state is not known yet */
        else if (p->lang.multi) { int ms = p->mst[k]; nt = cui__tokenize_st(&p->lang, s + ls, nn, &ms, toks, tok_cap); }
        else {
            if (p->mst) block = p->mst[k] != 0;
            nt = cui__tokenize(&p->lang, p->json, s + ls, nn, &block, toks, tok_cap);
        }
        int t0 = 0;
        for (int j = 0; j < R->n; j++) {
            bool last = j == R->n - 1;
            int ras = R->rs[j], rae = R->rs[j + 1];            /* in the buffer */
            int rs = ras - p->base, re = rae - p->base, rc0 = j ? R->ind : 0;
            int row = p->lrow[k] + j;
            float y = r.y + CUI__ED_INSET + (float)((double)row * p->lh - p->sy);
            if (y > r.y + r.h || y + p->lh < r.y) continue;
            if (s1 > s0 && s1 > ras && (last ? s0 <= rae : s0 < rae)) {
                int a = CUI_MAX(s0, ras) - p->base, b = CUI_MIN(s1, rae) - p->base;
                float xa = (float)cui__ed_adv(s, rs, a, p->tab, rc0) * p->cw, xb = (float)cui__ed_adv(s, rs, b, p->tab, rc0) * p->cw;
                if (last && s1 > rae) xb += p->cw * 0.6f;   /* the newline is selected too */
                cui_draw_rect(ui, (cui_rect){ x0 + xa, y, xb - xa, p->lh }, CUI_CORNERS(2), p->sel);
            }
            for (int m = 0; m < p->nmarks; m++) {
                const cui_text_mark *mk = &p->marks[m];
                if (last ? mk->start > rae : mk->start >= rae) break;
                if (mk->end <= ras && !(mk->end == mk->start && mk->start == ras)) continue;
                int a = CUI_MAX(mk->start, ras) - p->base, b = CUI_MIN(mk->end, rae) - p->base;
                float xa = (float)cui__ed_adv(s, rs, a, p->tab, rc0) * p->cw, xb = (float)cui__ed_adv(s, rs, b, p->tab, rc0) * p->cw;
                if (xb - xa < 2) { xa -= 1; xb = xa + 2; }
                cui__ed_draw_mark(ui, p, mk, x0 + xa, xb - xa, y);
            }
            if (p->fq.qn > 0) {   /* a match may run on into the next row: each row shows its part */
                for (int i = ls, mx, my; i <= le && cui__ed_q_line(&p->fq, s, ls, le, i, &mx, &my, NULL) > 0 && mx < re; i = my) {   /* (not x and y: y is the row's) */
                    if (my <= rs) continue;
                    int a = CUI_MAX(mx, rs), b = CUI_MIN(my, re);
                    float xa = (float)cui__ed_adv(s, rs, a, p->tab, rc0) * p->cw, xb = (float)cui__ed_adv(s, rs, b, p->tab, rc0) * p->cw;
                    cui_draw_rect_border(ui, (cui_rect){ x0 + xa - 1, y + 1, xb - xa + 2, p->lh - 2 }, CUI_CORNERS(3), p->match, p->match_edge, 1);
                }
            }
            for (int m = 0; m < 2; m++)
                if (p->br_row[m] == row)
                    cui_draw_rect_border(ui, (cui_rect){ x0 + (float)p->br_col[m] * p->cw - 0.5f, y + 2, p->cw + 1, p->lh - 4 }, CUI_CORNERS(3),
                                         p->brk, p->brk_edge, 1);
            while (t0 < nt && ls + toks[t0].start + toks[t0].len <= rs) t0++;
            int pos = rs, col = rc0;
            for (int t = t0; t < nt; t++) {
                int a = ls + toks[t].start, b = a + toks[t].len;
                if (a >= re && !(last && a == re)) break;
                int a2 = CUI_MAX(a, rs), b2 = CUI_MIN(b, re);
                col = cui__ed_adv(s, pos, a2, p->tab, col);
                int ca = col;
                col = cui__ed_adv(s, a2, b2, p->tab, col);
                pos = b2;
                if (toks[t].kind == 0 && cui__ed_blank(s, a, b)) continue;   /* white space */
                cui__ed_run(ui, p, s, a2, b2, ca, x0, y + ty, p->colors[toks[t].kind]);
            }
        }
    }
}

static void cui__ed_thumb(float track, double view, double max, double pos, float *at, float *len) {
    double total = view + max;
    *len = CUI_MAX(28.0f, track * (float)(view / total));
    *at = max > 0 ? (float)(pos / max) * (track - *len) : 0;
}

static void cui__paint_editor(cui_ctx *ui, cui_node *n, void *data) {
    cui__ed_paint *p = data;
    cui_rect r = n->rect;
    {   /* the wheel comes here while the pointer is over the editor and it can scroll */
        cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
        float s = ui->scale;
        cui_rect cp = { clip.x / s, clip.y / s, clip.w / s, clip.h / s };
        cui_scroll_state *ss = cui_state(ui, p->sid, sizeof *ss);
        ss->view_w = r.w; ss->view_h = r.h;
        ss->content_w = r.w + p->max_sx;
        ss->content_h = p->max_sy > 0 ? r.h + 1e6f : r.h;
        cui__paint_add_hit(ui, p->sid, cui__rect_intersect(r, cp), CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
    }
    if (r.h > p->view_h + 0.5f) cui_request_redraw(ui);   /* laid out taller than last frame: more lines next frame */
    bool unshaped = ui->text_unshaped;
    ui->text_unshaped = true;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    float ty = (p->lh - fh) * 0.5f;
    float gx = r.x + p->gutter;                     /* left edge of the code */
    float x0 = gx + CUI__ED_GAP - p->sx;            /* column 0 */
    cui_rect code = { gx, r.y, CUI_MAX(0, r.w - p->gutter), r.h };
    int s0 = CUI_MIN(p->cursor, p->anchor), s1 = CUI_MAX(p->cursor, p->anchor);
    cui_push_clip(ui, r);
    /* the caret's line, across the gutter and the code */
    if (p->show_line && s0 == s1 && p->caret_line >= p->first && p->caret_line < p->first + p->count) {
        int bk = p->caret_line - p->first;
        float y = r.y + CUI__ED_INSET + (float)(cui__ed_prow(p, bk) * p->lh - p->sy);
        float bh = p->wrap ? (float)p->lr[bk].n * p->lh : p->lh;   /* every row of a wrapped line */
        cui_draw_rect(ui, (cui_rect){ r.x, y, r.w, bh }, CUI_CORNERS(0), p->band);
    }
    /* line numbers */
    if (p->gutter > CUI__ED_GAP) {
        for (int k = 0; k < p->count; k++) {
            int line = p->first + k;
            float y = r.y + CUI__ED_INSET + (float)(cui__ed_prow(p, k) * p->lh - p->sy);
            const char *num = cui__frame_printf(ui, "%d", line + 1);
            float w = cui__text_width(ui, CUI_FONT_MONO, p->size, num, -1, true);
            bool on = p->show_line && line == p->caret_line;
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, gx - 10 - w, y + ty, on ? p->num_on : p->num, num, -1, true);
        }
        if (p->sx > 0.5f) cui_draw_rect(ui, (cui_rect){ gx, r.y, 1, r.h }, CUI_CORNERS(0), p->line);   /* code scrolled under the gutter */
    }
    cui_push_clip(ui, code);
    cui__tok *toks = NULL;
    int tok_cap = 0;
    bool block = p->in_block;
    cui__lang plain;   /* a line whose state is not known yet */
    memset(&plain, 0, sizeof plain);
    plain.plain = true;
    if (p->wrap) cui__ed_paint_wrap(ui, p, r, x0, ty, s0, s1);
    for (int k = 0; k < (p->wrap ? 0 : p->count); k++) {
        int line = p->first + k;
        int ls = p->ls[k] - p->base, le = p->ls[k + 1] - 1 - p->base;   /* in text */
        const char *s = p->text;
        float y = r.y + CUI__ED_INSET + (float)((double)line * p->lh - p->sy);
        int als = p->ls[k], ale = p->ls[k + 1] - 1;                    /* in the buffer */
        /* selection */
        if (s1 > s0 && s1 > als && s0 <= ale) {
            int a = CUI_MAX(s0, als) - p->base, b = CUI_MIN(s1, ale) - p->base;
            float xa = (float)cui__ed_col(s, ls, a, p->tab) * p->cw, xb = (float)cui__ed_col(s, ls, b, p->tab) * p->cw;
            if (s1 > ale) xb += p->cw * 0.6f;   /* the newline is selected too */
            cui_draw_rect(ui, (cui_rect){ x0 + xa, y, xb - xa, p->lh }, CUI_CORNERS(2), p->sel);
        }
        /* the app's marks, behind the find matches */
        for (int m = 0; m < p->nmarks; m++) {
            const cui_text_mark *mk = &p->marks[m];
            if (mk->start > ale) break;
            if (mk->end <= als && !(mk->end == mk->start && mk->start == als)) continue;
            int a = CUI_MAX(mk->start, als) - p->base, b = CUI_MIN(mk->end, ale) - p->base;
            float xa = (float)cui__ed_col(s, ls, a, p->tab) * p->cw, xb = (float)cui__ed_col(s, ls, b, p->tab) * p->cw;
            if (xb - xa < 2) { xa -= 1; xb = xa + 2; }   /* an empty match shows as a bar */
            cui__ed_draw_mark(ui, p, mk, x0 + xa, xb - xa, y);
        }
        /* find matches */
        if (p->fq.qn > 0) {
            for (int i = ls, mx, my; i <= le && cui__ed_q_line(&p->fq, s, ls, le, i, &mx, &my, NULL) > 0; i = my) {   /* (not x and y: y is the line's) */
                float xa = (float)cui__ed_col(s, ls, mx, p->tab) * p->cw, xb = (float)cui__ed_col(s, ls, my, p->tab) * p->cw;
                cui_draw_rect_border(ui, (cui_rect){ x0 + xa - 1, y + 1, xb - xa + 2, p->lh - 2 }, CUI_CORNERS(3), p->match, p->match_edge, 1);
            }
        }
        /* the bracket pair */
        for (int m = 0; m < 2; m++)
            if (p->br_line[m] == line)
                cui_draw_rect_border(ui, (cui_rect){ x0 + (float)p->br_col[m] * p->cw - 0.5f, y + 2, p->cw + 1, p->lh - 4 }, CUI_CORNERS(3),
                                     p->brk, p->brk_edge, 1);
        /* text: tokenized up to the last column in view */
        int stop = cui__ed_at_col(s, ls, le, (float)p->vc1 + 1, p->tab);
        if (stop < le) stop = cui__utf8_next(s, le, stop);
        int nn = stop - ls;
        if (nn + 1 > tok_cap) { tok_cap = nn + 1; toks = cui__frame_alloc(ui, sizeof *toks * (size_t)tok_cap); }
        if (p->mst && !p->lang.multi && p->mst[k] >= 0) block = p->mst[k] != 0;
        bool was_block = block;
        int nt;
        if (p->mst && p->mst[k] < 0) nt = cui__tokenize(&plain, false, s + ls, nn, &block, toks, tok_cap);   /* its state is not known yet */
        else if (p->lang.multi) { int ms = p->mst[k]; nt = cui__tokenize_st(&p->lang, s + ls, nn, &ms, toks, tok_cap); }
        else nt = cui__tokenize(&p->lang, p->json, s + ls, nn, &block, toks, tok_cap);
        int pos = ls, col = 0;
        for (int t = 0; t < nt; t++) {
            int a = ls + toks[t].start, b = a + toks[t].len;
            col = cui__ed_adv(s, pos, a, p->tab, col);
            int ca = col;
            col = cui__ed_adv(s, a, b, p->tab, col);
            pos = b;
            if (ca >= p->vc1) break;
            if (toks[t].kind == 0 && (s[a] == ' ' || s[a] == '\t')) continue;   /* white space */
            cui__ed_run(ui, p, s, a, b, ca, x0, y + ty, p->colors[toks[t].kind]);
        }
        /* a block comment opened past the columns in view still carries on */
        if (stop < le && p->lang.block_comments) block = cui__ed_scan_line(&p->lang, s, ls, le, was_block);
    }
    /* placeholder */
    if (p->empty && p->placeholder)
        cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x0, r.y + CUI__ED_INSET + (float)(-p->sy) + ty, p->muted, p->placeholder, -1, true);
    /* caret */
    if (p->focused && p->caret_line >= p->first && p->caret_line < p->first + p->count) {
        float blink = 0.5f + 0.5f * cosf((float)p->caret_phase * 2.0f * CUI_PI / 1.06f);
        float a = (blink > 0.35f || p->caret_phase < 0.5) ? 1.0f : 0.0f;
        float y = r.y + CUI__ED_INSET + (float)((double)(p->wrap ? p->caret_row : p->caret_line) * p->lh - p->sy);
        float cx = x0 + (float)p->caret_col * p->cw;
        cui_draw_rect(ui, (cui_rect){ roundf(cx * ui->scale) / ui->scale - 0.75f, y + ty, 1.5f, fh }, CUI_CORNERS(0.75f), cui_alpha(p->caret, a));
    }
    cui_pop_clip(ui);
    /* scrollbars */
    if (p->bar_t > 0.01f) {
        if (p->max_sy > 0) {
            float at, len, w = p->bar_big == 1 ? 8 : 6;
            cui__ed_thumb(r.h - 6, r.h, p->max_sy, p->sy, &at, &len);
            cui_draw_rect(ui, (cui_rect){ r.x + r.w - w - 3, r.y + 3 + at, w, len }, CUI_CORNERS(w * 0.5f),
                          cui_alpha(p->bar, (p->bar_big == 1 ? 0.38f : 0.26f) * p->bar_t));
        }
        if (p->max_sx > 0) {
            float at, len, h = p->bar_big == 2 ? 8 : 6, track = code.w - 6 - (p->max_sy > 0 ? CUI__ED_BAR : 0);
            cui__ed_thumb(track, code.w, p->max_sx, p->sx, &at, &len);
            cui_draw_rect(ui, (cui_rect){ gx + 3 + at, r.y + r.h - h - 3, len, h }, CUI_CORNERS(h * 0.5f),
                          cui_alpha(p->bar, (p->bar_big == 2 ? 0.38f : 0.26f) * p->bar_t));
        }
    }
    cui_pop_clip(ui);
    ui->text_unshaped = unshaped;
}

/* ---- one text, several editors ----
 * Two editors can show one buffer (a split view). Each reports its edits of a frame as one
 * splice (cui_code_editor_result.splice_*); the app hands it to the other editor with
 * cui_code_editor_splice, which patches its line index, wrap counts, line states and caret
 * for just that range on its next frame, the way its own edits do, instead of reading the
 * whole text again. Each editor keeps its own undo history: another editor's splice moves
 * the steps it does not touch, and the step it touches goes with all older ones. With `.doc`
 * none of this is needed: the editors of a document follow its log (see "documents" above). */

/* The history after another editor turned [a, b0) into [a, b1). Steps are walked from the
 * newest: one whose bytes the splice does not reach moves (a splice before it shifts it, one
 * after it shifts the splice back into the text before the step); the first one it reaches
 * cannot be undone exactly any more, so it goes with everything older. The redo steps go,
 * as after any new edit. */
static void cui__ed_hist_splice(cui_ctx *ui, cui__ed_hist *h, int a, int b0, int b1) {
    for (int i = h->pos; i < h->n; i++) { h->bytes -= (size_t)(h->r[i].del_n + h->r[i].ins_n); cui__ed_rec_free(ui, &h->r[i]); }
    h->n = h->pos;
    int fa = a, fdel = b0 - a, fins = b1 - a, cut = -1;
    for (int i = h->pos - 1; i >= 0; i--) {
        cui__ed_rec *q = &h->r[i];
        if (fa + fdel <= q->at) q->at += fins - fdel;                       /* the splice is before the step */
        else if (fa >= q->at + q->ins_n) fa += q->del_n - q->ins_n;          /* after it */
        else { cut = i; break; }                                              /* in it */
        q->caret = cui__ed_map(q->caret, fa, fa + fdel, fa + fins);
    }
    if (cut < 0) return;
    int g = h->r[cut].group, k = cut + 1;
    while (k < h->n && h->r[k].group == g) k++;
    for (int i = 0; i < k; i++) { h->bytes -= (size_t)(h->r[i].del_n + h->r[i].ins_n); cui__ed_rec_free(ui, &h->r[i]); }
    memmove(h->r, h->r + k, sizeof *h->r * (size_t)(h->n - k));
    h->n -= k; h->pos -= k;
}

/* a position moved by each splice in turn */
static int cui__ed_map_all(int p, int (*sp)[3], int n) {
    for (int i = 0; i < n; i++) p = cui__ed_map(p, sp[i][0], sp[i][1], sp[i][2]);
    return p;
}

/* At the start of a frame, before the text is synced: the splices another editor made in the
 * text, applied to what this editor keeps. The index, wrap counts and line states are patched
 * once, over the range that covers them all; the caret, the scroll and the undo steps follow
 * each splice in turn. Nothing happens when the index is made from the whole text this frame
 * anyway (a new revision); splices that do not fit the text say it has to be. */
static void cui__ed_take_splice(cui_ctx *ui, cui__ed_state *st, char *buf, size_t cap, int rev) {
    if (!st->sp_on) return;
    int sp[CUI__ED_SPLICES][3], n = st->sp_n;
    memcpy(sp, st->sp, sizeof sp);
    st->sp_on = false; st->sp_n = 0;
    cui__ed_ix *ix = &st->ix;
    int a = st->sp_a, b0 = st->sp_b0, b1 = st->sp_b1;
    if (!ix->ok || ix->rev != rev) return;
    long long nlen = (long long)ix->len + (b1 - b0);
    bool fits = a >= 0 && a <= b0 && b0 <= ix->len && b1 >= a && nlen >= 0 && (size_t)nlen < cap && buf[nlen] == 0;
    if (!fits) { ix->ok = false; return; }
    if (b0 == a && b1 == a) return;
    cui__ed E;
    memset(&E, 0, sizeof E);
    E.ui = ui; E.st = st; E.buf = buf; E.cap = cap; E.len = (int)nlen;
    E.ls = ix->ls; E.nl = ix->nl; E.maxlen = ix->maxlen;
    E.tab = 4;
    cui__ed_wc *wc = st->wc;
    if (wc && wc->ok && wc->n != ix->nl) wc->ok = false;
    if (st->mc && st->mc->ok && st->mc->n != ix->nl) st->mc->ok = false;
    if (wc && wc->ok) { E.wrap = true; E.wcols = wc->key / 32; E.tab = wc->key % 32; }   /* as last frame; a new width counts again anyway */
    /* the line at the top of the view: the scroll moves with its first byte */
    double lh = st->lh_seen, top_row = 0;
    int top = -1, top_at = 0;
    if (lh > 0) {
        int row = (int)floor(st->sy / lh);
        top = E.wrap ? cui__ed_line_of_row(&E, row) : CUI_CLAMP(row, 0, ix->nl - 1);
        top_row = E.wrap ? cui__ed_row_of_line(&E, top) : top;
        top_at = ix->ls[top];
    }
    cui__ed_reindex(&E, a, b0, b1 - a, NULL);
    if (top >= 0) {
        int nt = cui__ed_line_of(&E, cui__ed_map_all(top_at, sp, n));
        double nrow = E.wrap && st->wc && st->wc->ok ? cui__ed_row_of_line(&E, nt) : nt;
        st->sy = CUI_MAX(0.0, st->sy + (nrow - top_row) * lh);
    }
    cui_edit_state *e = &st->ed;
    e->cursor = cui__ed_map_all(e->cursor, sp, n);
    e->anchor = cui__ed_map_all(e->anchor, sp, n);
    e->last_cursor = cui__ed_map_all(e->last_cursor, sp, n);
    e->last_edit = 0;   /* typing after it is a new undo step */
    st->drag_a = cui__ed_map_all(st->drag_a, sp, n);
    st->drag_b = cui__ed_map_all(st->drag_b, sp, n);
    st->aff_pos = -1;
    for (int i = 0; i < n; i++) cui__ed_hist_splice(ui, &st->hist, sp[i][0], sp[i][1], sp[i][2]);
    if (!rev) ix->hash = cui__ed_hash_text(buf, (size_t)nlen);
}

void cui_code_editor_splice(cui_ctx *ui, cui_id editor, int start, int removed, int inserted) {
    cui__ed_state *s = cui_state(ui, editor ^ CUI__ED_SALT, sizeof *s);
    if (start < 0 || removed < 0 || inserted < 0) { s->ix.ok = false; return; }   /* not a splice: read the whole text */
    cui__ed_span_add(&s->sp_on, &s->sp_a, &s->sp_b0, &s->sp_b1, start, removed, inserted);
    if (s->sp_n < CUI__ED_SPLICES) {
        int *q = s->sp[s->sp_n++];
        q[0] = start; q[1] = start + removed; q[2] = start + inserted;
    } else {
        bool on = true;
        int *q = s->sp[CUI__ED_SPLICES - 1];
        cui__ed_span_add(&on, &q[0], &q[1], &q[2], start, removed, inserted);
    }
    ui->animating = true;
}

/* ---- the editor ---- */

static cui__ed_state *cui__ed_get(cui_ctx *ui, const char *id_str) {
    return cui_state(ui, cui_make_id(ui, id_str) ^ CUI__ED_SALT, sizeof(cui__ed_state));
}

void cui_code_editor_goto(cui_ctx *ui, const char *id, int line, int column) {
    cui__ed_state *s = cui__ed_get(ui, id);
    s->pend_goto = true;
    s->pend_line = line; s->pend_col = column;
    ui->animating = true;
}

void cui_code_editor_select(cui_ctx *ui, const char *id, int anchor, int cursor) {
    cui__ed_state *s = cui__ed_get(ui, id);
    s->pend_sel = true;
    s->pend_a = anchor; s->pend_c = cursor;
    ui->animating = true;
}

void cui_code_editor_edit(cui_ctx *ui, const char *id, int start, int end, const char *text, int sel_anchor, int sel_cursor) {
    cui__ed_state *s = cui__ed_get(ui, id);
    size_t n = text ? cui__ed_strlen(text) : 0;
    free(s->pend_text);   /* malloc'd: a big one becomes an undo record's or a document's own */
    free(s->pend_lb);
    s->pend_lb = NULL;
    s->pend_text = malloc(n + 1);
    if (!s->pend_text) return;
    if (s->doc && n >= CUI__ED_KEEP_BYTES) s->pend_nl = cui__ed_copy_lb(s->pend_text, text, (int)n, &s->pend_cr, &s->pend_lb);
    else s->pend_nl = (int)cui__ed_copy_scan(s->pend_text, text ? text : "", n, &s->pend_cr);
    s->pend_text[n] = 0;
    s->pend_n = (int)n;
    s->pend_edit = true;
    s->pend_es = start; s->pend_ee = end;
    s->pend_sa = sel_anchor; s->pend_sc = sel_cursor;
    ui->animating = true;
}

void cui_code_editor_find(cui_ctx *ui, const char *id, const char *query) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (!query) { s->mode = 0; return; }
    snprintf(s->query, sizeof s->query, "%s", query);
    s->mode = 1;
    s->rep_show = false;
    s->bar_focus = s->pend_find = true;
    ui->animating = true;
}

void cui_code_editor_find_ex(cui_ctx *ui, const char *id, const char *query, int flags) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (flags != CUI_FIND_KEEP) {
        s->f_case = (flags & CUI_FIND_CASE) != 0;
        s->f_word = (flags & CUI_FIND_WORD) != 0;
        s->f_rx = (flags & CUI_FIND_REGEX) != 0;
    }
    if (query) snprintf(s->query, sizeof s->query, "%s", query);
    s->mode = 1;
    s->rep_show = false;
    s->bar_focus = true;
    if (query) s->pend_find = true;
    ui->animating = true;
}

bool cui_code_editor_find_state(cui_ctx *ui, const char *id, char *query, int cap, int *flags) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (query && cap > 0) snprintf(query, (size_t)cap, "%s", s->query);
    if (flags) *flags = (s->f_case ? CUI_FIND_CASE : 0) | (s->f_word ? CUI_FIND_WORD : 0) | (s->f_rx ? CUI_FIND_REGEX : 0);
    return s->mode == 1;
}

void cui_code_editor_replace(cui_ctx *ui, const char *id, const char *query, const char *with) {
    cui__ed_state *s = cui__ed_get(ui, id);
    if (query) { snprintf(s->query, sizeof s->query, "%s", query); s->pend_find = true; }
    if (with) snprintf(s->repl, sizeof s->repl, "%s", with);
    s->mode = 1;
    s->rep_show = true;
    s->bar_focus = true;
    ui->animating = true;
}

/* how Enter indents: 3 markdown lists, 1 braces (C, C++, JavaScript, TypeScript, Rust, Go,
 * JSON), 2 colons (Python), 0 neither */
static int cui__ed_lang_kind(const char *lang) {
    if (lang && (!strcmp(lang, "markdown") || !strcmp(lang, "md"))) return 3;
    static const char *const braces[] = { "c", "cpp", "c++", "h", "js", "ts", "javascript", "typescript", "rust", "rs", "go", "json" };
    if (!lang) return 0;
    for (int i = 0; i < CUI_COUNTOF(braces); i++) if (!strcmp(lang, braces[i])) return 1;
    return !strcmp(lang, "python") || !strcmp(lang, "py") ? 2 : 0;
}

static cui_code_editor_result cui__code_editor_ex_body(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o);

/* left to right in any interface: see cui__keep_ltr */
cui_code_editor_result cui_code_editor_ex(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o) {
    int first = ui->node_count;
    cui_code_editor_result r = cui__code_editor_ex_body(ui, id_str, buf, cap, o);
    cui__keep_ltr(ui, first);
    return r;
}

/* One frame of the editor's view, around the editing context: ids, metrics, the box from
 * the last frame and the scroll limits. */
typedef struct {
    cui__ed *E;
    const cui_code_editor_opts *o;
    cui_code_editor_result *res;
    const char *id_str;
    cui_id id, lid, sid, fid, rid, gid; /* the editor, its lines, its scroll, the find, replace and go to fields */
    int lang_kind;
    float size, lh, cw, gutter, height; /* text size, line height, column width */
    cui_rect br; bool known;            /* the lines' box, last frame (known: it has one) */
    float code_w, gx;                   /* the code's width, and where it starts (right of the gutter) */
    double view_h, content_h, max_sy;
    float max_sx;
    bool over_vbar, over_hbar;          /* the pointer is over a scrollbar's zone */
    int mods; bool shift, prim;
    bool word_mod, line_mod;            /* the platform's word and line modifiers */
    cui_interact lr;
    bool focused;
    int caret_line, caret_col;
    int caret_row, caret_vcol;          /* wrapped: the caret's row from the top of the text, and its column in that row */
} cui__edf;

/* requests made from code since the last frame: select, go to, find */
static void cui__edf_requests(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    if (st->pend_edit) {
        int a = CUI_CLAMP(st->pend_es, 0, E->len), b = CUI_CLAMP(st->pend_ee, a, E->len);
        E->hint_cr = st->pend_cr ? 2 : 1; E->hint_nl = st->pend_nl + 1;
        E->hint_lb = st->pend_lb;   /* taken by the replace */
        st->pend_lb = NULL;
        if (cui__ed_replace_x(E, a, b, st->pend_text, st->pend_n, 3, &st->pend_text)) {
            int m = e->cursor - a;   /* the new text's length: the caret is after it */
            e->last_edit = 0;
            int na = st->pend_sa < 0 ? a + m : CUI_MIN(st->pend_sa, E->len);
            int nc = st->pend_sc < 0 ? na : CUI_MIN(st->pend_sc, E->len);
            if (st->pend_sa < 0 && st->pend_sc >= 0) na = nc;
            e->anchor = na; e->cursor = e->last_cursor = nc;
        }
        ui->focus = f->lid;
        free(st->pend_text);
        st->pend_text = NULL;
        st->pend_edit = false;
    }
    if (st->pend_sel) {
        e->anchor = CUI_CLAMP(st->pend_a, 0, E->len);
        e->cursor = CUI_CLAMP(st->pend_c, 0, E->len);
        ui->focus = f->lid;
        E->reveal = E->center = true;
        st->pref_col = -1;
    }
    if (st->pend_goto) {
        int line = CUI_CLAMP(st->pend_line - 1, 0, E->nl - 1);
        e->cursor = e->anchor = cui__ed_pos(E, line, (float)CUI_MAX(0, st->pend_col - 1));
        ui->focus = f->lid;
        E->reveal = E->center = true;
        st->pref_col = -1;
    }
    if (st->pend_find) cui__ed_find_go(E, CUI_MIN(e->cursor, e->anchor), 1);
    st->pend_sel = st->pend_goto = st->pend_find = false;
}

/* the find and go to line bar: Ctrl+F and Ctrl+G open it, Escape in it closes it */
static void cui__edf_bar_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool mine = ui->focus == f->lid || ui->focus == f->fid || ui->focus == f->rid || ui->focus == f->gid;
    if (mine && f->prim && (cui_key_pressed(ui, CUI_KEY_F) || cui_key_pressed(ui, CUI_KEY_H))) {
        bool rep = cui_key_pressed(ui, CUI_KEY_H);   /* Ctrl+H shows the replace row, Ctrl+F hides it */
        int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
        const char *sel = s1 > s0 && s1 - s0 < (int)sizeof st->query ? cui__ed_span(E, s0, s1) : NULL;
        if (sel && !memchr(sel + s0, '\n', (size_t)(s1 - s0)))
            snprintf(st->query, sizeof st->query, "%.*s", s1 - s0, sel + s0);
        st->mode = 1;
        st->rep_show = rep && !E->ro;
        st->bar_focus = true;
        ui->kpressed[CUI_KEY_F] = ui->kpressed[CUI_KEY_H] = 0;
    }
    if (mine && f->prim && cui_key_pressed(ui, CUI_KEY_G)) {
        st->mode = 2;
        st->go[0] = 0;
        st->bar_focus = true;
        ui->kpressed[CUI_KEY_G] = 0;
    }
    if ((ui->focus == f->fid || ui->focus == f->rid || ui->focus == f->gid) && cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        st->mode = 0;
        ui->focus = f->lid;
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
}

/* a press: on a scrollbar it grabs the thumb; in the gutter it selects lines; in the code it
 * places the caret, and a double or triple click selects a word or a line */
static void cui__edf_press(cui__edf *f, int line, float col) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    st->tab_free = false;
    if (f->over_vbar || f->over_hbar) {
        float at, len;
        if (f->over_vbar) {
            cui__ed_thumb(br.h - 6, br.h, f->max_sy, st->sy, &at, &len);
            float ty = br.y + 3 + at;
            st->bar_grab = ui->my >= ty && ui->my <= ty + len ? ui->my - ty : len * 0.5f;
            st->bar_drag = 1;
        } else {
            float track = br.w - f->gutter - 6 - (f->max_sy > 0 ? CUI__ED_BAR : 0);
            cui__ed_thumb(track, br.w - f->gutter, f->max_sx, st->sx, &at, &len);
            float tx = f->gx + 3 + at;
            st->bar_grab = ui->mx >= tx && ui->mx <= tx + len ? ui->mx - tx : len * 0.5f;
            st->bar_drag = 2;
        }
        return;
    }
    bool aff = false;
    int pos = E->wrap ? cui__ed_vpick(E, line, ui->mx < f->gx ? 0 : col, &aff) : cui__ed_pos(E, line, ui->mx < f->gx ? 0 : col);
    int clicks = ui->click_count;
    st->drag = ui->mx < f->gx ? 3 : clicks >= 3 ? 3 : clicks == 2 ? 2 : 1;
    st->aff_pos = -1;
    if (st->drag == 1) {
        e->cursor = pos;
        if (!f->shift) e->anchor = pos;
        st->drag_a = st->drag_b = e->anchor;
        if (aff) st->aff_pos = pos;
    } else {
        int a, b;
        if (st->drag == 2) cui__ed_word_at(E, pos, &a, &b); else cui__ed_line_at(E, pos, &a, &b);
        st->drag_a = a; st->drag_b = b;
        e->anchor = a; e->cursor = b;
    }
    st->pref_col = -1;
    ui->caret_time = ui->time;
}

/* dragging a scrollbar's thumb */
static void cui__edf_bar_drag(cui__edf *f) {
    cui__ed_state *st = f->E->st;
    cui_ctx *ui = f->E->ui;
    cui_rect br = f->br;
    float at, len;
    if (st->bar_drag == 1) {
        cui__ed_thumb(br.h - 6, br.h, f->max_sy, st->sy, &at, &len);
        float t = (ui->my - br.y - 3 - st->bar_grab) / CUI_MAX(1.0f, br.h - 6 - len);
        st->sy = (double)cui__clampf(t, 0, 1) * f->max_sy;
    } else {
        float track = br.w - f->gutter - 6 - (f->max_sy > 0 ? CUI__ED_BAR : 0);
        cui__ed_thumb(track, br.w - f->gutter, f->max_sx, st->sx, &at, &len);
        float t = (ui->mx - f->gx - 3 - st->bar_grab) / CUI_MAX(1.0f, track - len);
        st->sx = cui__clampf(t, 0, 1) * f->max_sx;
    }
    st->last_activity = ui->time;
}

/* dragging a selection, by characters, words or lines; past an edge the view scrolls,
 * faster further out */
static void cui__edf_drag_select(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    float gx = f->gx, lh = f->lh, cw = f->cw;
    float top = br.y, bottom = br.y + br.h;
    if (ui->my < top || ui->my > bottom) {
        float dist = ui->my < top ? top - ui->my : ui->my - bottom;
        double step = (double)ui->dt * (double)lh * (8.0 + 10.0 * dist / lh);
        st->sy = CUI_MAX(0.0, CUI_MIN(f->max_sy, st->sy + (ui->my < top ? -step : step)));
        ui->animating = true;
    }
    if (st->drag != 3 && (ui->mx < gx || ui->mx > br.x + br.w)) {
        float dist = ui->mx < gx ? gx - ui->mx : ui->mx - (br.x + br.w);
        float step = ui->dt * cw * (12.0f + 1.0f * dist);
        st->sx = cui__clampf(st->sx + (ui->mx < gx ? -step : step), 0, f->max_sx);
        ui->animating = true;
    }
    float y0 = br.y + CUI__ED_INSET - (float)st->sy;
    float x0 = gx + CUI__ED_GAP - st->sx;
    int line = (int)floor(((double)ui->my - y0) / lh);
    float col = (ui->mx - x0) / cw;
    bool aff = false;
    int pos = E->wrap ? cui__ed_vpick(E, line, st->drag == 3 ? 0 : col, &aff) : cui__ed_pos(E, line, st->drag == 3 ? 0 : col);
    if (line < 0) pos = 0;
    if (line >= (E->wrap ? cui__ed_total_rows(E) : E->nl)) pos = E->len;
    int prev = e->cursor;
    if (st->drag == 1) { e->cursor = pos; st->aff_pos = aff && pos != 0 && pos != E->len ? pos : -1; }
    else {
        int a, b;
        if (st->drag == 2) cui__ed_word_at(E, pos, &a, &b); else cui__ed_line_at(E, pos, &a, &b);
        if (a < st->drag_a) { e->anchor = st->drag_b; e->cursor = a; }
        else { e->anchor = st->drag_a; e->cursor = CUI_MAX(b, st->drag_b); }
    }
    if (e->cursor != prev) { ui->caret_time = ui->time; E->reveal = true; }
}

/* the pointer: press, drags, the wheel */
static void cui__edf_pointer(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_ctx *ui = E->ui;
    cui_rect br = f->br;
    f->lr = cui__interact(ui, f->lid, CUI_IF_FOCUSABLE | CUI_IF_DRAG | CUI_IF_TEXT);
    f->gx = br.x + f->gutter;
    float x0 = f->gx + CUI__ED_GAP - st->sx, y0 = br.y + CUI__ED_INSET - (float)st->sy;
    f->over_vbar = f->known && f->max_sy > 0 && ui->mx >= br.x + br.w - CUI__ED_BAR;
    f->max_sx = E->wrap ? 0.0f : CUI_MAX(0.0f, (float)CUI_MAX(E->maxlen, 1) * f->cw + CUI__ED_GAP + 24 - f->code_w);
    f->over_hbar = f->known && f->max_sx > 0 && ui->my >= br.y + br.h - CUI__ED_BAR && ui->mx >= f->gx && !f->over_vbar;
    if (f->lr.hovered) cui__set_cursor(ui, (f->over_vbar || f->over_hbar || ui->mx < f->gx) ? CUI_CURSOR_ARROW : CUI_CURSOR_TEXT);
    if (f->known) {
        int line = (int)floor(((double)ui->my - y0) / f->lh);
        float col = (ui->mx - x0) / f->cw;
        if (f->lr.hovered && ui->mpressed[0]) cui__edf_press(f, line, col);
        if (st->bar_drag && f->lr.active && ui->mdown[0]) cui__edf_bar_drag(f);
        if (st->drag && f->lr.active && ui->mdown[0] && !ui->mpressed[0]) cui__edf_drag_select(f);
    }
    if (!ui->mdown[0]) st->drag = st->bar_drag = 0;
    if (ui->scroll_target == f->sid && (ui->scroll_dy != 0 || ui->scroll_dx != 0)) {
        float dy = ui->scroll_dy, dx = ui->scroll_dx;
        if (f->shift && dx == 0) { dx = dy; dy = 0; }
        st->sy -= (double)dy * f->lh * 3;
        st->sx -= dx * 44;
        st->last_activity = ui->time;
        ui->scroll_dy = ui->scroll_dx = 0;   /* consumed */
    }
}

/* caret motion: arrows (by character, word or line end), up and down keeping the column,
 * pages, Home, End, select all. Reports the vertical motion. */
static void cui__edf_move_keys(cui__edf *f, int *vmove_out, int *vpage_out) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool shift = f->shift, word_mod = f->word_mod, line_mod = f->line_mod;
    bool whole = (f->mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) != 0;
    int view_lines = CUI_MAX(1, (int)(f->view_h / f->lh) - 1);
    for (int rep = ui->kpressed[CUI_KEY_LEFT]; rep > 0; rep--) {
        int s0 = CUI_MIN(e->cursor, e->anchor);
        if (!shift && e->cursor != e->anchor && !word_mod && !line_mod) { cui__ed_move(E, s0, false); continue; }
        int line = cui__ed_line_of(E, e->cursor);
        cui__ed_move(E, line_mod ? cui__ed_first_text(E, line) : word_mod ? cui__ed_word_left(E, e->cursor) : cui__ed_prev(E, e->cursor), shift);
    }
    for (int rep = ui->kpressed[CUI_KEY_RIGHT]; rep > 0; rep--) {
        int s1 = CUI_MAX(e->cursor, e->anchor);
        if (!shift && e->cursor != e->anchor && !word_mod && !line_mod) { cui__ed_move(E, s1, false); continue; }
        int line = cui__ed_line_of(E, e->cursor);
        cui__ed_move(E, line_mod ? cui__ed_end(E, line) : word_mod ? cui__ed_word_right(E, e->cursor) : cui__ed_next(E, e->cursor), shift);
    }
    int vmove = (int)ui->kpressed[CUI_KEY_DOWN] - (int)ui->kpressed[CUI_KEY_UP];
    int vpage = (int)ui->kpressed[CUI_KEY_PAGE_DOWN] - (int)ui->kpressed[CUI_KEY_PAGE_UP];
    if (line_mod && vmove) cui__ed_move(E, vmove < 0 ? 0 : E->len, shift);
    else if ((vmove || vpage) && E->wrap) {
        int col, row = cui__ed_vrow(E, e->cursor, &col), total = cui__ed_total_rows(E);
        if (st->pref_col < 0) st->pref_col = col;
        int target = row + vmove + vpage * view_lines, keep = st->pref_col;
        if (vpage) st->sy += (double)(CUI_CLAMP(target, 0, total - 1) - row) * f->lh;
        if (target < 0) cui__ed_move(E, 0, shift);
        else if (target >= total) cui__ed_move(E, E->len, shift);
        else {
            bool aff;
            int pos = cui__ed_vpick(E, target, (float)keep, &aff);
            cui__ed_move(E, pos, shift);
            if (aff) st->aff_pos = pos;
        }
        st->pref_col = keep;
    }
    else if (vmove || vpage) {
        int line = cui__ed_line_of(E, e->cursor);
        if (st->pref_col < 0) st->pref_col = cui__ed_caret_col(E, e->cursor);
        int target = line + vmove + vpage * view_lines;
        int keep = st->pref_col;
        if (vpage) st->sy += (double)(CUI_CLAMP(target, 0, E->nl - 1) - line) * f->lh;
        if (target < 0) cui__ed_move(E, 0, shift);
        else if (target >= E->nl) cui__ed_move(E, E->len, shift);
        else cui__ed_move(E, cui__ed_pos(E, target, (float)keep), shift);
        st->pref_col = keep;
    }
    if ((E->wrap && !whole) && (cui_key_pressed(ui, CUI_KEY_HOME) || cui_key_pressed(ui, CUI_KEY_END))) {
        /* the start and end of the row; Home again goes on to the line's start */
        bool home = cui_key_pressed(ui, CUI_KEY_HOME);
        cui__ed_lr R;
        int line = cui__ed_line_of(E, e->cursor);
        cui__ed_lr_get(E, line, &R);
        int j = cui__ed_lr_row(&R, e->cursor, st->aff_pos == e->cursor), to, ft = cui__ed_first_text(E, line);
        bool aff = false;
        if (home) to = j > 0 ? (e->cursor == R.rs[j] ? R.ls : R.rs[j]) : (e->cursor == ft ? R.ls : ft);
        else { to = R.rs[j + 1]; aff = j + 1 < R.n; }
        cui__ed_move(E, to, shift);
        if (aff) st->aff_pos = to;
    }
    else if (cui_key_pressed(ui, CUI_KEY_HOME)) {
        int line = cui__ed_line_of(E, e->cursor), ft = cui__ed_first_text(E, line);
        cui__ed_move(E, whole ? 0 : e->cursor == ft ? cui__ed_ls(E, line) : ft, shift);
    }
    if (!(E->wrap && !whole) && cui_key_pressed(ui, CUI_KEY_END)) cui__ed_move(E, whole ? E->len : cui__ed_end(E, cui__ed_line_of(E, e->cursor)), shift);
    if (f->prim && cui_key_pressed(ui, CUI_KEY_A)) { e->anchor = 0; e->cursor = E->len; }
    *vmove_out = vmove; *vpage_out = vpage;
}

/* Backspace: the selection, or back a character, a word, to the line start, or in the
 * indentation to the previous tab stop */
static void cui__edf_backspace(cui__edf *f) {
    cui__ed *E = f->E;
    cui_edit_state *e = &E->st->ed;
    for (int rep = E->ui->kpressed[CUI_KEY_BACKSPACE]; rep > 0; rep--) {
        int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
        if (s1 > s0) { cui__ed_replace(E, s0, s1, "", 0, 3); continue; }
        if (s0 == 0) continue;
        int line = cui__ed_line_of(E, s0), ls = cui__ed_ls(E, line), to = cui__ed_prev(E, s0);
        const char *buf = cui__ed_span(E, ls, s0);
        if (f->word_mod) to = cui__ed_word_left(E, s0);
        else if (f->line_mod) to = ls;
        else if (s0 > ls && cui__ed_first_text(E, line) >= s0 && buf[s0 - 1] == ' ') {
            int col = cui__ed_col(buf, ls, s0, E->tab), stop = (col - 1) / E->tab * E->tab;
            to = s0;
            while (to > ls && buf[to - 1] == ' ' && cui__ed_col(buf, ls, to - 1, E->tab) >= stop) to--;
        }
        cui__ed_replace(E, to, s0, "", 0, 2);
    }
}

/* editing keys (not read only): paste, undo and redo, Backspace, Delete, Enter, Tab, typing */
static void cui__edf_edit_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool shift = f->shift, prim = f->prim;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (prim && cui_key_pressed(ui, CUI_KEY_V) && ui->platform.get_clipboard) {
        const char *clip = ui->platform.get_clipboard(ui->platform.user);
        if (clip && *clip) {
            cui__ed_replace(E, s0, s1, clip, (int)strlen(clip), 3);
            e->last_edit = 0;   /* typing after a paste is its own step */
        }
    }
    bool undo = prim && !shift && cui_key_pressed(ui, CUI_KEY_Z);
    bool redo = prim && ((shift && cui_key_pressed(ui, CUI_KEY_Z)) || cui_key_pressed(ui, CUI_KEY_Y));
    if ((undo || redo) && cui__ed_undo(E, redo)) {
        E->changed = E->reveal = true;
        st->pref_col = -1;
    }
    cui__edf_backspace(f);
    for (int rep = ui->kpressed[CUI_KEY_DELETE]; rep > 0; rep--) {
        s0 = CUI_MIN(e->cursor, e->anchor); s1 = CUI_MAX(e->cursor, e->anchor);
        if (s1 > s0) { cui__ed_replace(E, s0, s1, "", 0, 3); continue; }
        if (s0 >= E->len) continue;
        int to = f->word_mod ? cui__ed_word_right(E, s0) : cui__ed_next(E, s0);
        cui__ed_replace(E, s0, to, "", 0, 2);
        e->cursor = e->anchor = e->last_cursor = s0;
    }
    for (int rep = ui->kpressed[CUI_KEY_ENTER]; rep > 0 && !prim; rep--) cui__ed_newline(E, f->lang_kind);
    if (cui_key_pressed(ui, CUI_KEY_TAB) && !(f->mods & CUI_MOD_CTRL)) {
        s0 = CUI_MIN(e->cursor, e->anchor); s1 = CUI_MAX(e->cursor, e->anchor);
        bool item = false;
        if (f->lang_kind == 3 && s1 == s0) {   /* a list item indents as a line, wherever the caret is */
            int ln = cui__ed_line_of(E, s0);
            cui__ed_mdi it;
            int ls, le;
            const char *s = cui__ed_line(E, ln, &ls, &le);
            item = cui__ed_md_item(s, ls, le, &it);
        }
        if (shift || s1 > s0 || item) cui__ed_indent(E, shift);
        else {
            int col = cui__ed_caret_col(E, s0), n = E->use_tabs ? 1 : E->tab - col % E->tab;
            char sp[17];
            memset(sp, E->use_tabs ? '\t' : ' ', sizeof sp);
            cui__ed_replace(E, s0, s0, sp, n, 3);
        }
    }
    if (ui->text_in_len > 0 && !(prim && !(f->mods & CUI_MOD_ALT))) cui__ed_type(E, ui->text_in, ui->text_in_len, f->lang_kind);
}

/* the keyboard while the lines have focus */
static void cui__edf_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    ui->edit = e;   /* cui_wants_keyboard, caret blink wakeups */
#if defined(__APPLE__)
    f->word_mod = (f->mods & CUI_MOD_ALT) != 0; f->line_mod = (f->mods & CUI_MOD_SUPER) != 0;
#else
    f->word_mod = (f->mods & CUI_MOD_CTRL) != 0; f->line_mod = false;
#endif
    int before_c = e->cursor, before_a = e->anchor;
    for (int k = CUI_KEY_NONE + 1; k < CUI_KEY_COUNT; k++)
        if (ui->kpressed[k] && k != CUI_KEY_TAB && k != CUI_KEY_ESCAPE) st->tab_free = false;
    if (ui->text_in_len > 0) st->tab_free = false;
    int vmove, vpage;
    cui__edf_move_keys(f, &vmove, &vpage);
    /* clipboard: without a selection, copy and cut take the whole line */
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (f->prim && (cui_key_pressed(ui, CUI_KEY_C) || cui_key_pressed(ui, CUI_KEY_X))) {
        int a = s0, b = s1;
        if (a == b) cui__ed_line_at(E, a, &a, &b);
        if (b > a && ui->platform.set_clipboard) {
            char *c = cui__frame_alloc(ui, (size_t)(b - a) + 1);
            cui__ed_read(E, a, b, c);
            ui->platform.set_clipboard(ui->platform.user, c);
        }
        if (cui_key_pressed(ui, CUI_KEY_X) && b > a) cui__ed_replace(E, a, b, "", 0, 3);
    }
    if (!E->ro) cui__edf_edit_keys(f);
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->mode) st->mode = 0;
        else if (e->cursor != e->anchor) e->anchor = e->cursor;
        else st->tab_free = true;   /* the next Tab leaves the editor */
    }
    if (e->cursor != before_c || e->anchor != before_a || E->changed) {
        ui->caret_time = ui->time;
        if (vmove == 0 && vpage == 0) st->pref_col = -1;
    }
    if (!E->ro && !st->tab_free) ui->tab_owner = f->lid;
}

/* a toggle in the find row: Aa, W, .* */
static bool cui__edf_toggle(cui_ctx *ui, cui_button_opts *bo, const char *label, const char *tip, bool on) {
    bo->selected = on; bo->tooltip = tip;
    bool hit = cui_button_ex(ui, label, bo);
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->selected = false;
    return hit;
}

/* the find bar's field, the toggles, the match count and the previous and next buttons;
 * the number of matches */
static int cui__edf_build_find(cui__edf *f, cui_input_opts *io, cui_button_opts *bo, bool narrow) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool shift = f->shift, rep = st->rep_show && !E->ro;
    if (st->bar_focus) { ui->focus = f->fid; ui->tf_select_all = f->fid; st->bar_focus = false; }
    io->placeholder = "Find";
    io->icon = narrow ? NULL : CUI_ICON_SEARCH;
    cui__ed_q Q = cui__ed_q_of(st);
    io->error = cui__ed_q_bad(&Q);
    int fl = cui_input_ex(ui, "find", st->query, sizeof st->query, io);
    io->error = NULL;
    Q = cui__ed_q_of(st);
    int s0 = CUI_MIN(e->cursor, e->anchor);
    if (fl & CUI_CHANGED) cui__ed_find_go(E, s0, 1);   /* as you type, from where the selection starts */
    if (fl & CUI_SUBMITTED) {
        if (rep && f->prim && (f->mods & (CUI_MOD_ALT | CUI_MOD_SHIFT))) cui__ed_replace_all(E, st->repl);
        else cui__ed_find_go(E, shift ? s0 : s0 + (e->cursor != e->anchor ? 1 : 0), shift ? -1 : 1);
    }
    bool tg = false;
    if (cui__edf_toggle(ui, bo, "Aa", "Match case", st->f_case)) { st->f_case = !st->f_case; tg = true; }
    if (cui__edf_toggle(ui, bo, "W", "Whole word", st->f_word)) { st->f_word = !st->f_word; tg = true; }
    if (cui__edf_toggle(ui, bo, ".*", "Regular expression", st->f_rx)) { st->f_rx = !st->f_rx; tg = true; }
    if (f->o->find_extra_icon) {
        bo->tooltip = f->o->find_extra_tooltip;
        if (cui_icon_button_ex(ui, f->o->find_extra_icon, bo)) f->res->find_extra_clicked = true;
        ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
        bo->tooltip = NULL;
    }
    Q = cui__ed_q_of(st);
    s0 = CUI_MIN(e->cursor, e->anchor);
    if (tg) cui__ed_find_go(E, s0, 1);
    /* the count, and which match the selection is */
    int index, total;
    bool on_match;
    int s1 = CUI_MAX(e->cursor, e->anchor);
    bool partial;
    total = cui__ed_count(E, &Q, s0, s1, &index, &on_match, &partial);
    int qn = Q.qn;
    if (on_match) index++;
    f->res->find_matches = total;
    f->res->find_case = st->f_case; f->res->find_word = st->f_word; f->res->find_regex = st->f_rx;
    f->res->find_index = on_match ? index : 0;
    const char *more = partial ? "+" : "";   /* a big text still being counted */
    const char *label = st->rjob ? cui__frame_printf(ui, narrow ? "%d%%" : "Replacingâ¦ %d%%",   /* Replace all at work on another thread */
                                                     (int)((double)cui__ed_atomic(&st->rjob->progress, 0) * 100.0 / CUI_MAX(1, E->len)))
                      : qn == 0 ? "" : narrow ? (on_match ? cui__frame_printf(ui, "%d/%d%s", index, total, more) : cui__frame_printf(ui, "%d%s", total, more))
                      : total == 0 && !partial ? "No results" : on_match ? cui__frame_printf(ui, "%d of %d%s", index, total, more)
                      : cui__frame_printf(ui, "%d%s match%s", total, more, total == 1 && !partial ? "" : "es");
    if (*label) {
        int t = cui__text_node(ui, label, -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), total ? CUI_C(ui, TEXT_SECONDARY) : CUI_C(ui, TEXT_MUTED), false);
        ui->nodes[t].flags |= CUI_NF_TABULAR;
        ui->nodes[t].line_height = 1.3f;
        ui->nodes[t].pad[1] = ui->nodes[t].pad[3] = 4;
    }
    bo->disabled = total == 0;
    bo->tooltip = "Previous match"; bo->shortcut = "Shift Enter";
    if (cui_icon_button_ex(ui, CUI_ICON_CHEVRON_UP, bo)) cui__ed_find_go(E, s0, -1);
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->tooltip = "Next match"; bo->shortcut = "Enter";
    if (cui_icon_button_ex(ui, CUI_ICON_CHEVRON_DOWN, bo)) cui__ed_find_go(E, s0 + (s1 > s0 ? 1 : 0), 1);
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    bo->disabled = false;
    return total;
}

/* the replace row under the find row: the field, Replace and Replace all */
static void cui__edf_build_replace(cui__edf *f, cui_input_opts *io, cui_button_opts *bo, bool narrow, int total) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_ctx *ui = E->ui;
    io->placeholder = "Replace";
    io->icon = NULL;
    int fl = cui_input_ex(ui, "replace", st->repl, sizeof st->repl, io);
    if (fl & CUI_SUBMITTED) {
        if (f->prim && (f->mods & (CUI_MOD_ALT | CUI_MOD_SHIFT))) cui__ed_replace_all(E, st->repl);
        else cui__ed_replace_one(E, st->repl);
    }
    bo->disabled = total == 0 || st->rjob;   /* not while Replace all is at work */
    bo->variant = CUI_BUTTON_SECONDARY;
    bo->tooltip = "Replace this match and go to the next"; bo->shortcut = "Enter";
    if (cui_button_ex(ui, "Replace", bo)) cui__ed_replace_one(E, st->repl);
    bo->tooltip = "Replace every match"; bo->shortcut = "Ctrl Alt Enter";
    if (cui_button_ex(ui, narrow ? "All" : "Replace all", bo)) cui__ed_replace_all(E, st->repl);
    bo->disabled = false;
    bo->variant = CUI_BUTTON_GHOST;
    bo->tooltip = bo->shortcut = NULL;
}

/* the go to line field: "line" or "line:column" */
static void cui__edf_build_goto(cui__edf *f, cui_input_opts *io, bool narrow) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    if (st->bar_focus) { ui->focus = f->gid; ui->tf_select_all = f->gid; st->bar_focus = false; }
    io->placeholder = narrow ? "Go to line" : cui__frame_printf(ui, "Go to line, 1 to %d", E->nl);
    io->icon = narrow ? NULL : CUI_ICON_ARROW_DOWN;
    int fl = cui_input_ex(ui, "goto", st->go, sizeof st->go, io);
    if (!(fl & CUI_SUBMITTED)) return;
    char *end = NULL;
    long line = strtol(st->go, &end, 10), col = 1;
    if (end && *end == ':') col = strtol(end + 1, NULL, 10);
    if (end != st->go) {
        line = CUI_CLAMP(line, 1, E->nl);
        e->cursor = e->anchor = cui__ed_pos(E, (int)line - 1, (float)CUI_MAX(0, col - 1));
        E->reveal = E->center = true;
        st->pref_col = -1;
        st->mode = 0;
        ui->focus = f->lid;
    }
}

/* the bar above the lines: find or go to line, and a close button */
static void cui__edf_build_bar(cui__edf *f) {
    cui__ed_state *st = f->E->st;
    cui_ctx *ui = f->E->ui;
    bool narrow = f->known && f->br.w < 360;   /* a phone: a shorter count, no search icon */
    int bar = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[bar];
    bn->dir = CUI_COLUMN; bn->gap = 4;
    bn->w = CUI_FILL;
    bn->pad[0] = bn->pad[2] = 6; bn->pad[1] = 6; bn->pad[3] = 6;
    bn->bg = CUI_C(ui, SURFACE);
    cui__open(ui, bar);
    cui_push_id(ui, f->id_str);
    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->gap = 4; rn->w = CUI_FILL;
    cui__open(ui, row);
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.size = CUI_SIZE_SM;
    io.width = CUI_FILL;
    cui_button_opts bo;
    memset(&bo, 0, sizeof bo);
    bo.size = CUI_SIZE_SM;
    bo.variant = CUI_BUTTON_GHOST;
    int total = 0;
    if (st->mode == 1) total = cui__edf_build_find(f, &io, &bo, narrow);
    else cui__edf_build_goto(f, &io, narrow);
    bo.tooltip = "Close"; bo.shortcut = "Esc";
    if (cui_icon_button_ex(ui, CUI_ICON_X, &bo)) { st->mode = 0; ui->focus = f->lid; }
    ui->nodes[ui->last_node].w = CUI_PX(26); ui->nodes[ui->last_node].h = CUI_PX(26);
    cui__close(ui);   /* the row */
    if (st->mode == 1 && st->rep_show && !f->E->ro) {
        int row2 = cui__node_new(ui, 0);
        cui_node *r2 = &ui->nodes[row2];
        r2->dir = CUI_ROW; r2->align = CUI_ALIGN_CENTER; r2->gap = 4; r2->w = CUI_FILL;
        cui__open(ui, row2);
        bo.tooltip = bo.shortcut = NULL;
        cui__edf_build_replace(f, &io, &bo, narrow, total);
        cui__close(ui);
    }
    cui_pop_id(ui);
    cui__close(ui);
    int hl = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
    ui->nodes[hl].bg = CUI_C(ui, BORDER);
}

/* follow the caret, then keep the scroll in range */
static void cui__edf_follow_caret(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    float cw = f->cw, lh = f->lh, code_w = f->code_w;
    double view_h = f->view_h;
    if (E->changed) {
        f->content_h = (double)(E->wrap ? cui__ed_total_rows(E) : E->nl) * lh;
        f->max_sy = CUI_MAX(0.0, f->content_h - view_h);
        f->max_sx = E->wrap ? 0.0f : CUI_MAX(0.0f, (float)CUI_MAX(E->maxlen, 1) * cw + CUI__ED_GAP + 24 - code_w);
    }
    f->caret_line = cui__ed_line_of(E, e->cursor);
    {
        int ls = cui__ed_ls(E, f->caret_line);
        f->caret_col = cui__ed_col(cui__ed_span(E, ls, e->cursor), ls, e->cursor, E->tab);
    }
    if (E->wrap) { f->caret_row = cui__ed_vrow(E, e->cursor, &f->caret_vcol); st->sx = 0; }
    else f->caret_vcol = f->caret_col;
    /* a line wider than the longest in bytes (tabs) still scrolls into reach */
    if (!E->wrap) f->max_sx = CUI_MAX(f->max_sx, (float)f->caret_col * cw + CUI__ED_GAP + 24 - code_w);
    if (E->reveal) {
        double cy = (double)(E->wrap ? f->caret_row : f->caret_line) * lh;
        bool out = cy < st->sy || cy + lh > st->sy + view_h;
        if (E->center && out) {   /* whole lines: none cut at the top */
            double k = floor((cy - view_h * 0.5) / lh + 0.5);
            st->sy = k > 0 ? k * lh + CUI__ED_INSET : 0;
        }
        else if (cy < st->sy) st->sy = cy;
        else if (cy + lh > st->sy + view_h) st->sy = cy + lh - view_h;
        float cx = (float)f->caret_col * cw, margin = CUI_MIN(cw * 4, code_w * 0.25f);
        if (E->wrap) cx = 0;   /* nothing to scroll sideways */
        else {
            if (cx < st->sx + margin) st->sx = cx - margin;
            if (cx > st->sx + code_w - margin) st->sx = cx - code_w + margin;
            if (E->center && (cx < st->sx || cx > st->sx + code_w)) st->sx = cx - code_w * 0.5f;
        }
    }
    st->sy = CUI_MAX(0.0, CUI_MIN(f->max_sy, st->sy));
    st->sx = cui__clampf(st->sx, 0, f->max_sx);
}

/* the editor's lines: one node whose paint draws the lines in view, copied into frame memory */
static void cui__edf_build_lines(cui__edf *f, const cui__lang *L, bool json, bool dark) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    float lh = f->lh, cw = f->cw;
    bool focused = f->focused;
    int body = cui__node_new(ui, f->lid);
    cui_node *bdn = &ui->nodes[body];
    bdn->w = CUI_FILL; bdn->h = CUI_FILL;
    cui__mark_interactive(ui, body, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui__ed_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->sid = f->sid;
    int first, last;
    for (int pass = 0;; pass++) {
        first = (int)CUI_MAX(0.0, floor((st->sy - CUI__ED_INSET) / lh));
        last = (int)CUI_MIN((double)(E->wrap ? cui__ed_total_rows(E) : E->nl), ceil((st->sy + f->view_h + 2 * CUI__ED_INSET) / lh) + 1);
        if (first > last) first = last;
        if (!E->wrap) break;
        /* the rows in view are found; from here on lines */
        last = last > first ? cui__ed_line_of_row(E, last - 1) + 1 : first;
        first = CUI_MIN(cui__ed_line_of_row(E, first), last);
        /* lines whose rows were a guess are counted, which moves the ones below: again */
        if (pass == 3 || !cui__ed_wrap_count(E, first, last)) break;
        f->content_h = (double)cui__ed_total_rows(E) * lh;
        f->max_sy = CUI_MAX(0.0, f->content_h - f->view_h);
        st->sy = CUI_MIN(st->sy, f->max_sy);
    }
    p->first = first; p->count = last - first; p->nl = E->nl;
    int a = cui__ed_ls(E, first), b = last > first ? cui__ed_end(E, last - 1) : a;
    p->base = a;
    char *text = cui__frame_alloc(ui, (size_t)(b - a) + 1);   /* a copy: the caller may change the buffer before painting */
    cui__ed_read(E, a, b, text);
    p->text = text;
    int *ls = cui__frame_alloc(ui, sizeof(int) * (size_t)(p->count + 1));
    if (!E->doc) memcpy(ls, E->ls + first, sizeof(int) * (size_t)(p->count + 1));
    else {   /* found in the copy, not looked up line by line */
        ls[0] = a;
        for (int k = 1; k <= p->count; k++) {
            const char *nl = k < p->count ? memchr(text + (ls[k - 1] - a), '\n', (size_t)(b - ls[k - 1])) : NULL;
            ls[k] = nl ? (int)(nl - text) + a + 1 : (k == p->count ? cui__ed_ls(E, last) : b + 1);
        }
    }
    p->ls = ls;
    if (E->wrap) {
        cui__ed_lr *lr = cui__frame_alloc(ui, sizeof *lr * (size_t)CUI_MAX(1, p->count));
        int *lrow = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, p->count));
        for (int k = 0; k < p->count; k++) { cui__ed_lr_get(E, first + k, &lr[k]); lrow[k] = k ? lrow[k - 1] + lr[k - 1].n : cui__ed_row_of_line(E, first); }
        p->wrap = true; p->lr = lr; p->lrow = lrow;
        p->caret_row = f->caret_row;
    }
    p->lang = *L; p->json = json;
    if (cui__ed_states(E, L, L->multi ? last : first)) {   /* only down to the lines in view */
        /* The state before each line in view. While a job still finds them (cui__ed_states), a line
         * below the last one known takes it from the line above when that one has a state, else the
         * state it had before the edit when its text is the same (kept0 to kept1), else none: it draws
         * plain until the job is done. */
        const cui__ed_mc *mc = st->mc;
        int *mst = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, p->count));
        for (int k = 0; k < p->count; k++) {
            int j = first + k;
            if (j <= mc->valid) mst[k] = cui__sq_get(ui, &st->mc->st, j);
            else if (k > 0 && mst[k - 1] >= 0) mst[k] = cui__ed_state_step(L, text - a, ls[k - 1], ls[k] - 1, mst[k - 1]);
            else if (j >= mc->kept0 && j < mc->kept1) mst[k] = cui__sq_get(ui, &st->mc->st, j);
            else mst[k] = -1;
        }
        p->mst = mst;
        p->in_block = !L->multi && p->count > 0 && mst[0] > 0;
    }
    p->tab = E->tab;
    p->cursor = e->cursor; p->anchor = e->anchor;
    p->caret_line = f->caret_line; p->caret_col = f->caret_vcol;
    p->focused = focused;
    p->show_line = focused || ui->focus == f->fid || ui->focus == f->gid;
    p->ro = E->ro;
    p->empty = E->len == 0;
    p->placeholder = f->o->placeholder;
    p->size = f->size; p->lh = lh; p->cw = cw; p->gutter = f->gutter;
    p->view_h = f->known ? f->br.h : 0;
    p->sy = st->sy; p->max_sy = f->max_sy;
    p->sx = st->sx; p->max_sx = f->max_sx;
    p->vc0 = (int)floorf(st->sx / cw) - 1;
    p->vc1 = (int)ceilf((st->sx + f->code_w + CUI__ED_GAP) / cw) + 1;
    if (E->wrap) { p->vc0 = -1; p->vc1 = INT_MAX / 2; }   /* rows are cut to the view already */
    p->caret_phase = ui->time - ui->caret_time;
    p->br_line[0] = p->br_line[1] = p->br_row[0] = p->br_row[1] = -1;
    if (e->cursor == e->anchor && p->show_line && !f->o->plain) {
        int ba, bb;
        cui__ed_brackets(E, e->cursor, &ba, &bb);
        if (ba >= 0) {
            f->res->bracket = ba; f->res->bracket_match = bb;
            int pos[2] = { ba, bb };
            for (int m = 0; m < 2; m++) {
                p->br_line[m] = cui__ed_line_of(E, pos[m]);
                if (E->wrap) p->br_row[m] = cui__ed_vrow(E, pos[m], &p->br_col[m]);
                else { int bls = cui__ed_ls(E, p->br_line[m]); p->br_col[m] = cui__ed_col(cui__ed_span(E, bls, pos[m]), bls, pos[m], E->tab); }
            }
        }
    }
    if (st->mode == 1 && st->query[0]) { p->fq = cui__ed_q_of(st); p->fq.q = cui__frame_strdup(ui, st->query, -1); }
    if (f->o->marks && f->o->mark_count > 0) {   /* the marks that reach the lines in view, copied: the caller owns them */
        const cui_text_mark *mk = f->o->marks;
        int n = f->o->mark_count, lo = 0, hi = n;
        while (lo < hi) { int m = lo + (hi - lo) / 2; if (mk[m].start < a - 4096) lo = m + 1; else hi = m; }   /* starts are sorted; marks are short */
        int k0 = lo, k1 = lo;
        while (k1 < n && mk[k1].start <= b) k1++;
        int c = 0;
        cui_text_mark *keep = cui__frame_alloc(ui, sizeof *keep * (size_t)CUI_MAX(1, k1 - k0));
        for (int k = k0; k < k1; k++) if (mk[k].end >= a) keep[c++] = mk[k];
        p->marks = keep; p->nmarks = c;
        for (int k = 0; k < 7; k++) {
            cui_color base = k == 0 ? (ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT)) : ui->theme.colors[CUI_TOKEN_SERIES_1 + (k - 1)];
            p->mark_colors[k] = cui_alpha(base, dark ? 0.34f : 0.26f);
        }
        for (int k = 1; k < 9; k++) {
            cui_color c = cui__tone_color(ui, (cui_tone)k);
            p->tone_line[k] = c;
            p->tone_tint[k] = cui_alpha(c, dark ? 0.34f : 0.26f);
        }
    }
    cui__syntax_colors(ui, p->colors);
    cui_color fg = CUI_C(ui, FOREGROUND);
    cui_color acc = ui->accent == CUI_ACCENT_NEUTRAL ? cui_oklch(0.6f, 0.13f, 250, 1) : CUI_C(ui, ACCENT);
    p->num = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.7f);
    p->num_on = CUI_C(ui, TEXT_SECONDARY);
    p->band = cui_alpha(fg, dark ? 0.05f : 0.04f);
    p->sel = cui_alpha(acc, ui->accent == CUI_ACCENT_NEUTRAL ? 0.28f : 0.26f);
    if (!focused && !p->show_line) p->sel = cui_alpha(fg, dark ? 0.12f : 0.08f);   /* resting: a quiet selection */
    p->match = cui_alpha(acc, 0.12f);
    p->match_edge = cui_alpha(acc, dark ? 0.55f : 0.45f);
    p->brk = cui_alpha(fg, dark ? 0.10f : 0.06f);
    p->brk_edge = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.55f);
    p->caret = ui->accent == CUI_ACCENT_NEUTRAL ? fg : CUI_C(ui, ACCENT);
    p->muted = CUI_C(ui, TEXT_MUTED);
    p->line = CUI_C(ui, BORDER);
    p->bar = ui->accent == CUI_ACCENT_NEUTRAL ? fg : CUI_C(ui, ACCENT);
    /* the scrollbars show while scrolling, while hovered or held */
    bool recent = st->last_activity > 0 && ui->time - st->last_activity < 0.9;
    bool vb = f->over_vbar && (f->lr.hovered || st->bar_drag == 1), hb = f->over_hbar && (f->lr.hovered || st->bar_drag == 2);
    bool show_bar = recent || vb || hb || st->bar_drag || (f->lr.hovered && (f->max_sy > 0 || f->max_sx > 0));
    if (recent) cui__wakeup_after(ui, (float)(0.9 - (ui->time - st->last_activity)) + 0.01f);
    cui__tween(ui, &st->bar_t, show_bar ? 1.0f : 0.0f, show_bar ? 0.12f : 0.3f);
    p->bar_t = st->bar_t;
    p->bar_big = (vb || st->bar_drag == 1) ? 1 : (hb || st->bar_drag == 2) ? 2 : 0;
    bdn->paint = cui__paint_editor;
    bdn->paint_data = p;
}

static cui_code_editor_result cui__code_editor_ex_body(cui_ctx *ui, const char *id_str, char *buf, size_t cap, const cui_code_editor_opts *o) {
    cui_code_editor_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_code_editor_result res;
    memset(&res, 0, sizeof res);
    res.bracket = res.bracket_match = -1;
    static char empty[1];
    if (!buf || cap == 0) { buf = empty; cap = 1; empty[0] = 0; }

    cui__edf f;
    memset(&f, 0, sizeof f);
    f.o = o; f.res = &res; f.id_str = id_str;
    f.id = cui_make_id(ui, id_str);
    f.lid = f.id ^ 0xED1B0D1u; f.sid = f.id ^ 0xED15C011u;
    cui__ed_state *st = cui_state(ui, f.id ^ CUI__ED_SALT, sizeof *st);
    cui__edit_attach(ui, f.id ^ CUI__ED_SALT, &st->ed);
    if (!st->init) {
        st->init = true;
        st->pref_col = -1;
        st->aff_pos = -1;
        cui__state_set_dtor(ui, f.id ^ CUI__ED_SALT, cui__ed_dtor);   /* the kept index, caches and history */
        if (o->autofocus) ui->focus = f.lid;
    }
    cui_push_id(ui, id_str);
    f.fid = cui_make_id(ui, "find");
    f.rid = cui_make_id(ui, "replace");
    f.gid = cui_make_id(ui, "goto");
    cui_pop_id(ui);

    cui__ed E;
    memset(&E, 0, sizeof E);
    cui__ed_ck ck = { NULL, 0, 0 };
    E.ui = ui; E.st = st; E.ck = &ck;
    if (o->doc) {   /* a document: take the edits made since last frame (by other editors or the app) */
        E.doc = o->doc;
        st->sp_on = false; st->sp_n = 0;   /* splices are for buffers */
        if (st->hist.n) { for (int i = 0; i < st->hist.n; i++) cui__ed_rec_free(ui, &st->hist.r[i]); st->hist.n = st->hist.pos = 0; st->hist.bytes = 0; }
        if (st->wc && st->wc->ok) { E.wrap = true; E.wcols = st->wc->key / 32; E.tab = st->wc->key % 32; }   /* as last frame, to count the lines an edit touched */
        cui__ed_doc_sync(&E, false);
        if (st->rjob) cui__ed_rjob_take(&E, o->read_only);   /* Replace all, done on another thread: applied now, before the frame lays out the text */
        E.wrap = false; E.wcols = 0;
    } else {
        if (st->doc) { st->doc = NULL; st->ix.ok = false; cui__ed_job_drop(&st->rjob); }   /* was on a document: everything is made again */
        E.buf = buf; E.cap = cap;
        cui__ed_take_splice(ui, st, buf, cap, o->revision);
        E.len = cui__ed_text_sync(ui, st, buf, cap, o->revision);
        E.ls = st->ix.ls; E.nl = st->ix.nl; E.maxlen = st->ix.maxlen;
    }
    E.tab = o->tab_size > 0 ? CUI_MIN(o->tab_size, 16) : 4;
    E.use_tabs = o->use_tabs;
    E.ro = o->read_only;
    f.E = &E;
    cui_edit_state *e = &st->ed;
    e->cursor = CUI_CLAMP(e->cursor, 0, E.len);
    e->anchor = CUI_CLAMP(e->anchor, 0, E.len);
    st->drag_a = CUI_CLAMP(st->drag_a, 0, E.len);   /* the text may change during a drag */
    st->drag_b = CUI_CLAMP(st->drag_b, 0, E.len);
    const char *lang = o->plain ? "text" : o->lang;   /* plain: no colors, whatever the language */
    f.lang_kind = cui__ed_lang_kind(lang);
    bool json = lang && !strcmp(lang, "json");
    cui__lang L = cui__lang_for(json ? NULL : lang);
    if (!L.multi && !L.block_comments) cui__ed_mc_free(ui, st);

    f.size = cui__fs(ui, o->size > 0 ? o->size : 13);
    f.lh = roundf(f.size * 1.6f);
    st->lh_seen = f.lh;
    f.cw = cui__text_width(ui, CUI_FONT_MONO, f.size, "0000000000000000", 16, true) / 16.0f;
    int digits = 2;
    for (int v = E.nl / 100; v > 0; v /= 10) digits++;
    f.gutter = o->no_line_numbers ? CUI__ED_GAP : 12 + (float)digits * f.cw + 14;
    f.height = o->height > 0 ? o->height : 320;

    f.br = cui__prev_rect(ui, f.lid);
    f.known = cui__has_prev_rect(ui, f.lid) && f.br.w > 0;
    if (!f.known) { ui->animating = true; f.br.w = 480; f.br.h = f.height - 2 - (st->mode ? 41 : 0); }
    f.code_w = CUI_MAX(1.0f, f.br.w - f.gutter - CUI__ED_GAP);
    f.view_h = CUI_MAX((double)f.lh, (double)f.br.h - 2 * CUI__ED_INSET);
    if (o->wrap) {
        E.wrap = true;
        E.wcols = CUI_MAX(8, (int)floorf((f.code_w - CUI__ED_BAR) / f.cw));   /* the bar's zone stays free of text */
        cui__ed_wrap_sync(&E);
    } else cui__ed_wc_free(ui, st);
    f.content_h = (double)(E.wrap ? cui__ed_total_rows(&E) : E.nl) * f.lh;
    f.max_sy = CUI_MAX(0.0, f.content_h - f.view_h);

    cui__edf_requests(&f);   /* after the wrap width is known: an edit from code patches the wrap counts too */

    /* input */
    f.mods = ui->mods;
    f.shift = (f.mods & CUI_MOD_SHIFT) != 0; f.prim = (f.mods & CUI_MOD_PRIMARY) != 0;
    cui__edf_bar_keys(&f);
    cui__edf_pointer(&f);
    f.focused = ui->focus == f.lid;
    if (!f.focused) st->tab_free = false;
    if (f.focused) cui__edf_keys(&f);

    /* build */
    bool dark = ui->mode == CUI_THEME_DARK;
    int outer = cui__node_new(ui, f.id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(f.height);
    on->min_h = 80;
    on->radius = CUI_CORNERS(o->borderless ? 0 : ui->theme.radius_control);
    cui_color bg = o->borderless ? CUI_C(ui, BACKGROUND)
                 : dark ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, BACKGROUND), 0.4f) : CUI_C(ui, SURFACE_MUTED);
    on->bg = bg;   /* the code block's surface */
    on->border = o->borderless ? 0 : 1;
    cui__tween(ui, &st->focus_t, (ui->focus == f.lid || ui->focus == f.fid || ui->focus == f.rid || ui->focus == f.gid) ? 1.0f : 0.0f, CUI__T_FOCUS);
    on->border_color = cui_mix(CUI_C(ui, BORDER), cui__edge_on(ui), st->focus_t * 0.7f);
    on->flags |= CUI_NF_CLIP;
    cui__open(ui, outer);
    if (st->mode) cui__edf_build_bar(&f);
    cui__edf_follow_caret(&f);
    cui__edf_build_lines(&f, &L, json, dark);
    if (f.focused) cui__wakeup_after(ui, 0.53f);
    if (f.focused && ui->platform.set_text_input_rect) {
        cui_rect cr = { f.gx + CUI__ED_GAP - st->sx + (float)f.caret_vcol * f.cw,
                        f.br.y + CUI__ED_INSET + (float)((double)(E.wrap ? f.caret_row : f.caret_line) * f.lh - st->sy), 1, f.lh };
        ui->platform.set_text_input_rect(ui->platform.user, cr);
    }
    if (f.focused && !o->borderless) cui__focus_ring(ui, outer, ui->focus_visible ? 1.0f : 0.0f);
    cui__close(ui);   /* outer */
    ui->last_id = f.id;

    if (E.changed && !o->revision && !E.doc) st->ix.hash = cui__ed_hash_text(buf, (size_t)E.len);   /* next frame: the text is as the editor left it */
    res.changed = E.changed;
    res.splice_start = E.rep_on ? E.rep_a : -1;
    res.splice_removed = E.rep_on ? E.rep_b0 - E.rep_a : 0;
    res.splice_inserted = E.rep_on ? E.rep_b1 - E.rep_a : 0;
    res.refused = E.refused;
    res.working = st->sjob || st->rjob;
    res.bar_open = st->mode != 0;
    res.focused = ui->focus == f.lid || ui->focus == f.fid || ui->focus == f.rid || ui->focus == f.gid;
    res.cursor = e->cursor; res.anchor = e->anchor;
    res.line = f.caret_line + 1; res.column = f.caret_col + 1;
    res.line_count = E.nl;
    res.length = E.len;
    return res;
}
