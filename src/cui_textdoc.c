/* Text documents: a piece table, so an edit costs about the same anywhere in a text of any size.
 *
 * The text is a sequence of pieces, each a run of bytes in a buffer: the text it was made from
 * (copied, or adopted as it is), and append-only buffers that edits add their bytes to. Buffers
 * never change once written and never move, so a piece is three numbers and the text an edit
 * removed is still there for undo. The pieces sit in a treap (a balanced tree by random
 * priorities) keyed by byte offset; each node knows the bytes and line breaks of its subtree,
 * and each buffer the positions of its own line breaks, so the start of a line, the line of an
 * offset and the piece holding a byte are found in O(log n). An edit splits the tree at its two
 * ends, drops the middle and puts one piece in: O(log n) plus the bytes typed. Typing on at the
 * end of the last insert just makes that piece longer.
 *
 * Undo keeps pieces, not bytes: undoing a replace of 100 MB swaps two short piece lists. Each
 * edit is also kept in a short log by revision, so editors showing the document patch their
 * caches for just the lines an edit touched (see cui_editor.c). Nothing here needs a UI. */
#include "cui_internal.h"
#include <limits.h>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <emmintrin.h>
#define CUI__TD_SSE2 1
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* ---- byte scans (shared with the code editor) ---- */

#if defined(CUI__TD_SSE2)
static int cui__td_ctz(unsigned m) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward(&i, m);
    return (int)i;
#else
    return __builtin_ctz(m);
#endif
}
static int cui__td_ctz64(uint64_t m) {
#if defined(_MSC_VER)
    unsigned long i;
    _BitScanForward64(&i, m);
    return (int)i;
#else
    return __builtin_ctzll(m);
#endif
}
#endif

const char *cui__find_byte(const char *p, size_t n, char c) {
#if defined(CUI__TD_SSE2)
    const __m128i v = _mm_set1_epi8(c);
    size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        __m128i x0 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), v);
        __m128i x1 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i + 16)), v);
        __m128i x2 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i + 32)), v);
        __m128i x3 = _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i + 48)), v);
        if (_mm_movemask_epi8(_mm_or_si128(_mm_or_si128(x0, x1), _mm_or_si128(x2, x3)))) break;
    }
    for (; i + 16 <= n; i += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), v));
        if (m) return p + i + cui__td_ctz(m);
    }
    for (; i < n; i++) if (p[i] == c) return p + i;
    return NULL;
#else
    return memchr(p, c, n);
#endif
}

const char *cui__find_byte_back(const char *p, size_t n, char c) {
    while (n > 0) { if (p[n - 1] == c) return p + n - 1; n--; }
    return NULL;
}

size_t cui__count_byte(const char *p, size_t n, char c) {
    size_t total = 0;
#if defined(CUI__TD_SSE2)
    const __m128i v = _mm_set1_epi8(c), zero = _mm_setzero_si128();
    while (n >= 16) {
        size_t blocks = CUI_MIN(n / 16, (size_t)255);   /* a byte lane holds up to 255 hits */
        __m128i acc = zero;
        for (size_t j = 0; j < blocks; j++) acc = _mm_sub_epi8(acc, _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p + j), v));
        __m128i sum = _mm_sad_epu8(acc, zero);
        total += (size_t)_mm_cvtsi128_si32(sum) + (size_t)_mm_cvtsi128_si32(_mm_srli_si128(sum, 8));
        p += blocks * 16; n -= blocks * 16;
    }
    for (; n > 0; n--) total += *p++ == c;
#else
    for (const char *e = p + n; (p = memchr(p, c, (size_t)(e - p))) != NULL; p++) total++;
#endif
    return total;
}

int cui__fill_nl(const char *s, int a, int b, int *out) {
    int k = 0, i = a;
#if defined(CUI__TD_SSE2)
    const __m128i v = _mm_set1_epi8('\n');
    for (; i + 64 <= b; i += 64) {   /* one mask for 64 bytes: the loop ends once per block, not per 16 bytes */
        uint64_t m = 0;
        for (int q = 0; q < 4; q++)
            m |= (uint64_t)(unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s + i + q * 16)), v)) << (q * 16);
        while (m) { out[k++] = i + cui__td_ctz64(m) + 1; m &= m - 1; }
    }
    for (; i + 16 <= b; i += 16) {
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(s + i)), v));
        while (m) { out[k++] = i + cui__td_ctz(m) + 1; m &= m - 1; }
    }
#endif
    for (; i < b; i++) if (s[i] == '\n') out[k++] = i + 1;
    return k;
}

/* ---- buffers ---- */

/* The bytes of a buffer, shared by a document and its snapshots (which may be on other threads):
 * freed when the last one lets go. */
typedef struct {
    char *p;
    void (*release)(char *p, void *user);   /* adopted bytes: how to give them back; NULL: free() */
    void *user;
    volatile long refs;
} cui__tblk;

static long cui__td_atomic_add(volatile long *p, long v) {
#if defined(_MSC_VER)
    return _InterlockedExchangeAdd(p, v) + v;
#else
    return __atomic_add_fetch(p, v, __ATOMIC_ACQ_REL);
#endif
}
static void cui__tblk_drop(cui__tblk *k) {
    if (!k || cui__td_atomic_add(&k->refs, -1) != 0) return;
    if (k->release) k->release(k->p, k->user);
    else free(k->p);
    free(k);
}

typedef struct {
    cui__tblk *k;
    int len, cap;               /* bytes used; the add buffer takes appends up to cap, and never moves */
    int *nl; int nn, ncap;      /* the line breaks in [0, len): nl[i] is the offset of one */
} cui__tbuf;

/* A node of the tree: one piece, and what its subtree holds. */
typedef struct {
    int l, r;
    uint32_t pri;
    int buf, start, len;        /* bytes [start, start + len) of buffer buf */
    int nl0, nl;                /* its line breaks are nl[nl0, nl0 + nl) of that buffer */
    int sl, snl;                /* bytes and line breaks of the subtree */
} cui__tnode;

typedef struct { int buf, start, len; } cui__tpc;   /* a piece kept for undo */

/* One undo record: bytes [at, at + del_n) became [at, at + ins_n), as pieces both ways. */
typedef struct {
    int at, del_n, ins_n;
    cui__tpc *del, *ins;
    int ndel, nins, cdel, cins;
    int caret, group;
    unsigned after;             /* the text's state id after it */
} cui__trec;

struct cui_text_doc {
    cui__tbuf *b; int nb, bcap;
    cui__tnode *n; int nn, ncap, freel;   /* n[0] is the empty tree */
    int root;
    int add;                    /* the buffer appends go to, or -1 */
    int added;                  /* bytes appended so far: add buffers grow with it */
    uint32_t seed;
    unsigned rev, base_rev;     /* base_rev: the last revision the whole text was set */
    unsigned serial;            /* unique among the documents of a process: an editor told a new one at an old one's address knows */
    unsigned state, states, base_state;   /* the text's state id; ids given so far; the state before the oldest undo step */
    int maxlen, maxcnt;         /* the longest line in bytes, and how many lines are that long */
    bool maxstale;              /* an edit replaced too many lines to count them out: measure all again */
    cui__tsplice log[CUI__DOC_LOG];
    cui__trec *h; int hn, hcap, hpos;
    int hmax;                   /* undo groups kept */
    cui__tpc *tmp; int tmpn, tmpcap;      /* scratch: pieces taken out by an edit */
};

#define CUI__TD_HISTORY 1000          /* undo groups kept */
#define CUI__TD_ADD_MIN (16 * 1024)   /* the first add buffer; later ones grow with what was added */
#define CUI__TD_ADD_MAX (1024 * 1024)
#define CUI__TD_OWN 65536             /* an inserted text this big gets a buffer of its own (adopted when the caller gives it) */

static void *cui__td_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 16;
    while (c < need) c = c > (1 << 29) ? need : c * 2;
    void *np = realloc(p, elem * (size_t)c);
    if (!np) { fprintf(stderr, "cui: out of memory\n"); abort(); }
    *cap = c;
    return np;
}
#define CUI__TD_GROW(arr, count, cap, need) ((arr) = cui__td_grow((arr), &(cap), (count) + (need), sizeof *(arr)))

/* first index i in [lo, hi) with a[i] >= x */
static int cui__td_lower(const int *a, int lo, int hi, int x) {
    while (lo < hi) { int m = lo + (hi - lo) / 2; if (a[m] < x) lo = m + 1; else hi = m; }
    return lo;
}

/* a new buffer of the given bytes (adopted) or capacity (to append to); returns its index */
static int cui__td_buf_new(cui_text_doc *d, char *p, int len, int cap, void (*release)(char *, void *), void *user) {
    CUI__TD_GROW(d->b, d->nb, d->bcap, 1);
    cui__tbuf *b = &d->b[d->nb];
    memset(b, 0, sizeof *b);
    b->k = calloc(1, sizeof *b->k);
    if (!b->k) abort();
    b->k->p = p ? p : malloc((size_t)CUI_MAX(cap, 1));
    if (!b->k->p) abort();
    b->k->release = release; b->k->user = user; b->k->refs = 1;
    b->len = len; b->cap = p ? len : cap;
    if (len > 0) {   /* its line breaks, in blocks so the index grows by what is read */
        for (int a = 0; a < len; a += 1 << 16) {
            int e = CUI_MIN(len, a + (1 << 16));
            CUI__TD_GROW(b->nl, b->nn, b->ncap, e - a);
            int k = cui__fill_nl(b->k->p, a, e, b->nl + b->nn);
            for (int i = 0; i < k; i++) b->nl[b->nn + i]--;   /* fill_nl gives the byte after */
            b->nn += k;
        }
        if (b->ncap > b->nn + b->nn / 4 + 64) { b->nl = realloc(b->nl, sizeof(int) * (size_t)CUI_MAX(b->nn, 1)); b->ncap = CUI_MAX(b->nn, 1); }
    }
    return d->nb++;
}

/* Puts t[0, n) where a piece can point at it: at the end of the add buffer, in a new one, or
 * (own, a big text the caller gave away) in a buffer of its own. Returns the buffer, and in
 * *start where the bytes begin. */
static int cui__td_place(cui_text_doc *d, const char *t, int n, char *own, void (*release)(char *, void *), void *user, int *start) {
    if (own && n >= CUI__TD_OWN) { *start = 0; return cui__td_buf_new(d, own, n, n, release, user); }
    int bi;
    if (n >= CUI__TD_OWN) {   /* a big text: a buffer that fits it exactly */
        char *p = malloc((size_t)n);
        if (!p) abort();
        memcpy(p, t, (size_t)n);
        bi = cui__td_buf_new(d, p, n, n, NULL, NULL);
        *start = 0;
    } else {
        if (d->add < 0 || d->b[d->add].len + n > d->b[d->add].cap) {
            int cap = CUI_CLAMP(d->added / 4, CUI__TD_ADD_MIN, CUI__TD_ADD_MAX);
            d->add = cui__td_buf_new(d, NULL, 0, CUI_MAX(cap, n), NULL, NULL);
        }
        bi = d->add;
        cui__tbuf *b = &d->b[bi];
        *start = b->len;
        memcpy(b->k->p + b->len, t, (size_t)n);
        CUI__TD_GROW(b->nl, b->nn, b->ncap, (int)cui__count_byte(t, (size_t)n, '\n'));
        int k = cui__fill_nl(b->k->p, b->len, b->len + n, b->nl + b->nn);
        for (int i = 0; i < k; i++) b->nl[b->nn + i]--;
        b->nn += k;
        b->len += n;
        d->added += n;
    }
    if (own) { if (release) release(own, user); else free(own); }
    return bi;
}

/* ---- the tree ---- */

#define TN(i) d->n[i]

static void cui__td_upd(cui_text_doc *d, int t) {
    cui__tnode *x = &TN(t);
    x->sl = TN(x->l).sl + x->len + TN(x->r).sl;
    x->snl = TN(x->l).snl + x->nl + TN(x->r).snl;
}

static void cui__td_reserve(cui_text_doc *d, int more) {
    int have = 0;
    for (int f = d->freel; f && have < more; f = TN(f).l) have++;
    if (have < more) CUI__TD_GROW(d->n, d->nn, d->ncap, more - have);
}

/* a node for bytes [start, start + len) of buffer bi; the caller reserved room */
static int cui__td_node(cui_text_doc *d, int bi, int start, int len) {
    int t;
    if (d->freel) { t = d->freel; d->freel = TN(t).l; }
    else t = d->nn++;
    d->seed ^= d->seed << 13; d->seed ^= d->seed >> 17; d->seed ^= d->seed << 5;
    cui__tnode *x = &TN(t);
    const cui__tbuf *b = &d->b[bi];
    x->l = x->r = 0;
    x->pri = d->seed;
    x->buf = bi; x->start = start; x->len = len;
    x->nl0 = cui__td_lower(b->nl, 0, b->nn, start);
    x->nl = cui__td_lower(b->nl, x->nl0, b->nn, start + len) - x->nl0;
    x->sl = len; x->snl = x->nl;
    return t;
}

static int cui__td_merge(cui_text_doc *d, int a, int b) {
    if (!a) return b;
    if (!b) return a;
    if (TN(a).pri > TN(b).pri) { int r = cui__td_merge(d, TN(a).r, b); TN(a).r = r; cui__td_upd(d, a); return a; }
    int l = cui__td_merge(d, a, TN(b).l);
    TN(b).l = l;
    cui__td_upd(d, b);
    return b;
}

/* *a gets the first k bytes of tree t, *b the rest; a piece across k is cut in two (one new node) */
static void cui__td_split(cui_text_doc *d, int t, int k, int *a, int *b) {
    if (!t) { *a = *b = 0; return; }
    int ll = TN(TN(t).l).sl, len = TN(t).len;
    if (k <= ll) {
        int x, y;
        cui__td_split(d, TN(t).l, k, &x, &y);
        TN(t).l = y; cui__td_upd(d, t);
        *a = x; *b = t;
    } else if (k >= ll + len) {
        int x, y;
        cui__td_split(d, TN(t).r, k - ll - len, &x, &y);
        TN(t).r = x; cui__td_upd(d, t);
        *a = t; *b = y;
    } else {
        int off = k - ll;
        int u = cui__td_node(d, TN(t).buf, TN(t).start + off, len - off);
        TN(t).len = off;
        TN(t).nl -= TN(u).nl;
        int r = TN(t).r;
        TN(t).r = 0; cui__td_upd(d, t);
        *a = t;
        *b = cui__td_merge(d, u, r);
    }
}

static void cui__td_take(cui_text_doc *d, int t) {   /* the pieces of t, in order, to d->tmp; its nodes freed */
    if (!t) return;
    int l = TN(t).l, r = TN(t).r;
    cui__td_take(d, l);
    CUI__TD_GROW(d->tmp, d->tmpn, d->tmpcap, 1);
    d->tmp[d->tmpn++] = (cui__tpc){ TN(t).buf, TN(t).start, TN(t).len };
    TN(t).l = d->freel; d->freel = t;
    cui__td_take(d, r);
}

/* The piece that ends at byte k grows by n bytes (and nlc line breaks) when it ends where the
 * buffer's new bytes begin: typing on. */
static bool cui__td_extend(cui_text_doc *d, int t, int k, int bi, int at, int n, int nlc) {
    if (!t) return false;
    int ll = TN(t).l ? TN(TN(t).l).sl : 0, len = TN(t).len;
    bool ok;
    if (k <= ll) ok = cui__td_extend(d, TN(t).l, k, bi, at, n, nlc);
    else if (k < ll + len) return false;
    else if (k == ll + len) {
        ok = TN(t).buf == bi && TN(t).start + len == at;
        if (ok && n >= 0) { TN(t).len += n; TN(t).nl += nlc; }
    } else ok = cui__td_extend(d, TN(t).r, k - ll - len, bi, at, n, nlc);
    if (ok && n >= 0) { TN(t).sl += n; TN(t).snl += nlc; }   /* n < 0: only whether it can */
    return ok;
}

/* ---- lookups ---- */

int cui__doc_line_start(const cui_text_doc *d, int line) {
    if (line <= 0) return 0;
    int t = d->root, acc = 0;
    if (line > TN(t).snl) return TN(t).sl + 1;
    while (t) {
        const cui__tnode *x = &TN(t);
        int lnl = TN(x->l).snl;
        if (line <= lnl) { t = x->l; continue; }
        line -= lnl;
        if (line <= x->nl) return acc + TN(x->l).sl + (d->b[x->buf].nl[x->nl0 + line - 1] - x->start) + 1;
        line -= x->nl;
        acc += TN(x->l).sl + x->len;
        t = x->r;
    }
    return TN(d->root).sl + 1;
}

int cui__doc_line_of(const cui_text_doc *d, int pos) {
    int t = d->root, acc = 0;
    while (t) {
        const cui__tnode *x = &TN(t);
        int ll = TN(x->l).sl;
        if (pos < ll) { t = x->l; continue; }
        pos -= ll;
        acc += TN(x->l).snl;
        if (pos < x->len) return acc + cui__td_lower(d->b[x->buf].nl, x->nl0, x->nl0 + x->nl, x->start + pos) - x->nl0;
        pos -= x->len;
        acc += x->nl;
        t = x->r;
    }
    return acc;
}

const char *cui__doc_chunk(const cui_text_doc *d, int pos, int *cs, int *ce) {
    int t = d->root, acc = 0;
    while (t) {
        const cui__tnode *x = &TN(t);
        int ll = TN(x->l).sl;
        if (pos < acc + ll) { t = x->l; continue; }
        if (pos < acc + ll + x->len) {
            *cs = acc + ll; *ce = acc + ll + x->len;
            return d->b[x->buf].k->p + x->start;
        }
        acc += ll + x->len;
        t = x->r;
    }
    *cs = *ce = TN(d->root).sl;
    return NULL;
}

/* The longest line, exactly, and how many lines are that long: an edit takes the lengths of the
 * lines it replaces out of the count and puts the new ones in; only when the last line of the
 * longest length got shorter is every line measured again (from the line break index, not the
 * bytes). */
static void cui__td_maxlen_walk(const cui_text_doc *d, int t, int *pos, int *last, int *m, int *cnt) {
    if (!t) return;
    const cui__tnode *x = &TN(t);
    cui__td_maxlen_walk(d, x->l, pos, last, m, cnt);
    const int *nl = d->b[x->buf].nl;
    for (int i = 0; i < x->nl; i++) {
        int at = *pos + nl[x->nl0 + i] - x->start, n = at - *last;
        if (n > *m) { *m = n; *cnt = 1; } else if (n == *m) (*cnt)++;
        *last = at + 1;
    }
    *pos += x->len;
    cui__td_maxlen_walk(d, x->r, pos, last, m, cnt);
}
static void cui__td_maxlen_all(cui_text_doc *d) {
    int pos = 0, last = 0, m = 0, cnt = 0;
    cui__td_maxlen_walk(d, d->root, &pos, &last, &m, &cnt);
    if (pos - last > m) { m = pos - last; cnt = 1; } else if (pos - last == m) cnt++;
    d->maxlen = m; d->maxcnt = cnt; d->maxstale = false;
}

/* lines [la, lb] are about to be replaced: their lengths leave the count */
static void cui__td_max_out(cui_text_doc *d, int la, int lb) {
    if (d->maxstale) return;
    if (lb - la > 1024) { d->maxstale = true; return; }
    int s = cui__doc_line_start(d, la);
    for (int i = la; i <= lb; i++) {
        int e = cui__doc_line_start(d, i + 1) - 1;
        if (e - s == d->maxlen) d->maxcnt--;
        s = e + 1;
    }
}

/* lines [la, la + k] were just written: their lengths join the count */
static void cui__td_max_in(cui_text_doc *d, int la, int k) {
    if (!d->maxstale && k <= 1024) {
        int s = cui__doc_line_start(d, la);
        for (int i = la; i <= la + k; i++) {
            int e = cui__doc_line_start(d, i + 1) - 1;
            if (e - s > d->maxlen) { d->maxlen = e - s; d->maxcnt = 1; }
            else if (e - s == d->maxlen) d->maxcnt++;
            s = e + 1;
        }
        if (d->maxcnt > 0) return;
    }
    cui__td_maxlen_all(d);
}

/* ---- editing ---- */

static void cui__td_log(cui_text_doc *d, int a, int b0, int b1, int la, int lb, int k) {
    d->rev++;
    cui__tsplice *s = &d->log[d->rev % CUI__DOC_LOG];
    s->rev = d->rev; s->a = a; s->b0 = b0; s->b1 = b1; s->la = la; s->lb = lb; s->k = k;
}

/* Bytes [a, b) become the pieces pc[0, npc) (whose line breaks number k): the removed pieces are
 * left in d->tmp. Logged as one splice. */
static void cui__td_put(cui_text_doc *d, int a, int b, const cui__tpc *pc, int npc, int n, int k) {
    int la = cui__doc_line_of(d, a), lb = b == a ? la : cui__doc_line_of(d, b);
    cui__td_max_out(d, la, lb);
    d->tmpn = 0;
    cui__td_reserve(d, npc + 2);
    int A, R, M, B;
    cui__td_split(d, d->root, a, &A, &R);
    cui__td_split(d, R, b - a, &M, &B);
    cui__td_take(d, M);
    int mid = 0;
    for (int i = 0; i < npc; i++)
        if (pc[i].len > 0) mid = cui__td_merge(d, mid, cui__td_node(d, pc[i].buf, pc[i].start, pc[i].len));
    d->root = cui__td_merge(d, cui__td_merge(d, A, mid), B);
    cui__td_log(d, a, b, a + n, la, lb, k);
    cui__td_max_in(d, la, k);
}

static void cui__td_rec_free(cui__trec *q) { free(q->del); free(q->ins); }

static void cui__td_pcs_add(cui__tpc **arr, int *n, int *cap, cui__tpc p, bool front) {
    if (p.len <= 0) return;
    if (*n > 0) {   /* joins its neighbour when the bytes run on */
        cui__tpc *q = front ? &(*arr)[0] : &(*arr)[*n - 1];
        if (!front && q->buf == p.buf && q->start + q->len == p.start) { q->len += p.len; return; }
        if (front && q->buf == p.buf && p.start + p.len == q->start) { q->start = p.start; q->len += p.len; return; }
    }
    *arr = cui__td_grow(*arr, cap, *n + 1, sizeof **arr);
    if (front) memmove(*arr + 1, *arr, sizeof **arr * (size_t)*n);
    (*arr)[front ? 0 : *n] = p;
    (*n)++;
}

static void cui__td_trim(cui_text_doc *d) {
    while (d->hn > 0 && d->h[d->hn - 1].group - d->h[0].group + 1 > d->hmax) {
        int g = d->h[0].group, k = 0;
        while (k < d->hn && d->h[k].group == g) { d->base_state = d->h[k].after; cui__td_rec_free(&d->h[k++]); }
        memmove(d->h, d->h + k, sizeof *d->h * (size_t)(d->hn - k));
        d->hn -= k; d->hpos -= k;
    }
}

bool cui__doc_edit(cui_text_doc *d, int a, int b, const char *t, int n, char *own, void (*release)(char *, void *), void *user,
                   int caret, bool merge) {
    int len = TN(d->root).sl;
    a = CUI_CLAMP(a, 0, len); b = CUI_CLAMP(b, a, len);
    if (n < 0) n = 0;
    if ((long long)len - (b - a) + n > INT_MAX - 2) { if (own) { if (release) release(own, user); else free(own); } return false; }
    if (a == b && n == 0) { if (own) { if (release) release(own, user); else free(own); } return false; }
    int nlc = n > 0 ? (int)cui__count_byte(t, (size_t)n, '\n') : 0;
    int start = 0, bi = -1;
    if (n > 0) bi = cui__td_place(d, t, n, own, release, user, &start);
    /* the history: redo steps go, then the edit joins the last record or starts one */
    for (int i = d->hpos; i < d->hn; i++) cui__td_rec_free(&d->h[i]);
    d->hn = d->hpos;
    cui__trec *q = NULL;
    bool joined = false;
    if (merge && d->hn > 0) {
        q = &d->h[d->hn - 1];
        if (b == a && q->del_n == 0 && a == q->at + q->ins_n) joined = true;
        else if (n == 0 && q->ins_n == 0 && (b == q->at || a == q->at)) joined = true;
    }
    int group = d->hn > 0 ? d->h[d->hn - 1].group + (merge ? 0 : 1) : 1;   /* numbered on from the last step kept, as the editor's own history */
    if (!joined) {
        CUI__TD_GROW(d->h, d->hn, d->hcap, 1);
        q = &d->h[d->hn++];
        memset(q, 0, sizeof *q);
        q->at = a; q->caret = caret; q->group = group;
        d->hpos = d->hn;
    }
    /* the text: typing on grows the piece before it; anything else is split, cut and joined */
    int la = cui__doc_line_of(d, a);
    bool typing = b == a && bi >= 0 && cui__td_extend(d, d->root, a, bi, start, -1, 0);   /* (a check: can it grow?) */
    if (typing) {
        cui__td_max_out(d, la, la);
        cui__td_extend(d, d->root, a, bi, start, n, nlc);
        d->tmpn = 0;
        cui__td_log(d, a, a, a + n, la, la, nlc);
        cui__td_max_in(d, la, nlc);
    } else {
        cui__tpc pc = { bi, start, n };
        cui__td_put(d, a, b, &pc, n > 0 ? 1 : 0, n, nlc);
    }
    /* the record keeps the pieces both ways */
    if (b > a) {
        if (joined && b == q->at) { for (int i = d->tmpn - 1; i >= 0; i--) cui__td_pcs_add(&q->del, &q->ndel, &q->cdel, d->tmp[i], true); q->at = a; }
        else for (int i = 0; i < d->tmpn; i++) cui__td_pcs_add(&q->del, &q->ndel, &q->cdel, d->tmp[i], false);
        q->del_n += b - a;
    }
    if (n > 0) { cui__td_pcs_add(&q->ins, &q->nins, &q->cins, (cui__tpc){ bi, start, n }, false); q->ins_n += n; }
    q->after = d->state = ++d->states;
    cui__td_trim(d);
    return true;
}

/* sums the line breaks of pieces */
static int cui__td_pcs_nl(const cui_text_doc *d, const cui__tpc *pc, int n) {
    int k = 0;
    for (int i = 0; i < n; i++) {
        const cui__tbuf *b = &d->b[pc[i].buf];
        int i0 = cui__td_lower(b->nl, 0, b->nn, pc[i].start);
        k += cui__td_lower(b->nl, i0, b->nn, pc[i].start + pc[i].len) - i0;
    }
    return k;
}

bool cui__doc_undo(cui_text_doc *d, bool redo, int *caret) {
    if (redo ? d->hpos >= d->hn : d->hpos <= 0) return false;
    int g = d->h[redo ? d->hpos : d->hpos - 1].group, c = 0;
    while (redo ? d->hpos < d->hn && d->h[d->hpos].group == g : d->hpos > 0 && d->h[d->hpos - 1].group == g) {
        const cui__trec *q = &d->h[redo ? d->hpos++ : --d->hpos];
        if (redo) {
            cui__td_put(d, q->at, q->at + q->del_n, q->ins, q->nins, q->ins_n, cui__td_pcs_nl(d, q->ins, q->nins));
            c = q->at + q->ins_n;
        } else {
            cui__td_put(d, q->at, q->at + q->ins_n, q->del, q->ndel, q->del_n, cui__td_pcs_nl(d, q->del, q->ndel));
            c = q->caret;
        }
    }
    d->state = d->hpos > 0 ? d->h[d->hpos - 1].after : d->base_state;
    if (caret) *caret = c;
    return true;
}

const cui__tsplice *cui__doc_splice_at(const cui_text_doc *d, unsigned rev) { return &d->log[rev % CUI__DOC_LOG]; }
unsigned cui__doc_base_rev(const cui_text_doc *d) { return d->base_rev; }
unsigned cui__doc_serial(const cui_text_doc *d) { return d->serial; }
int cui__doc_maxlen(const cui_text_doc *d) { return d->maxlen; }

/* ---- the public API ---- */

static void cui__td_clear(cui_text_doc *d) {
    for (int i = 0; i < d->nb; i++) { cui__tblk_drop(d->b[i].k); free(d->b[i].nl); }
    d->nb = 0;
    d->nn = 1; d->freel = 0; d->root = 0;
    d->add = -1; d->added = 0;
    for (int i = 0; i < d->hn; i++) cui__td_rec_free(&d->h[i]);
    d->hn = d->hpos = 0;
}

static void cui__td_set(cui_text_doc *d, char *own, const char *text, int len, void (*release)(char *, void *), void *user) {
    cui__td_clear(d);
    if (len > 0) {
        char *p = own;
        if (!p) { p = malloc((size_t)len); if (!p) abort(); memcpy(p, text, (size_t)len); release = NULL; }
        int bi = cui__td_buf_new(d, p, len, len, release, user);
        cui__td_reserve(d, 1);
        d->root = cui__td_node(d, bi, 0, len);
    } else if (own) { if (release) release(own, user); else free(own); }
    d->rev++;
    d->base_rev = d->rev;
    d->state = d->base_state = ++d->states;
    cui__td_maxlen_all(d);
}

static cui_text_doc *cui__td_alloc(void) {
    cui_text_doc *d = calloc(1, sizeof *d);
    if (!d) abort();
    d->ncap = 64;
    d->n = calloc((size_t)d->ncap, sizeof *d->n);
    if (!d->n) abort();
    d->nn = 1;
    d->add = -1;
    d->seed = 0x9E3779B9u;
    d->hmax = CUI__TD_HISTORY;
    static volatile long serials;
    d->serial = (unsigned)cui__td_atomic_add(&serials, 1);
    return d;
}

static int cui__td_len_of(const char *text, int len) { return len < 0 ? (text ? (int)CUI_MIN(strlen(text), (size_t)INT_MAX - 2) : 0) : len; }

cui_text_doc *cui_text_doc_new(const char *text, int len) {
    cui_text_doc *d = cui__td_alloc();
    cui__td_set(d, NULL, text, cui__td_len_of(text, len), NULL, NULL);
    return d;
}

cui_text_doc *cui_text_doc_adopt(char *text, int len, void (*release)(char *text, void *user), void *user) {
    cui_text_doc *d = cui__td_alloc();
    cui__td_set(d, text, text, cui__td_len_of(text, len), release, user);
    return d;
}

void cui_text_doc_set(cui_text_doc *d, const char *text, int len) { cui__td_set(d, NULL, text, cui__td_len_of(text, len), NULL, NULL); }
void cui_text_doc_set_adopt(cui_text_doc *d, char *text, int len, void (*release)(char *text, void *user), void *user) {
    cui__td_set(d, text, text, cui__td_len_of(text, len), release, user);
}

void cui_text_doc_free(cui_text_doc *d) {
    if (!d) return;
    cui__td_clear(d);
    free(d->b); free(d->n); free(d->h); free(d->tmp);
    free(d);
}

int cui_text_doc_length(const cui_text_doc *d) { return d ? TN(d->root).sl : 0; }
int cui_text_doc_line_count(const cui_text_doc *d) { return d ? TN(d->root).snl + 1 : 1; }
int cui_text_doc_line_start(const cui_text_doc *d, int line) {
    if (!d) return 0;
    if (line >= TN(d->root).snl + 1) return TN(d->root).sl;
    return cui__doc_line_start(d, line);
}
int cui_text_doc_line_end(const cui_text_doc *d, int line) {
    if (!d || line < 0) return 0;
    if (line >= TN(d->root).snl) return TN(d->root).sl;
    return cui__doc_line_start(d, line + 1) - 1;
}
int cui_text_doc_line_of(const cui_text_doc *d, int pos) {
    if (!d) return 0;
    return cui__doc_line_of(d, CUI_CLAMP(pos, 0, TN(d->root).sl));
}
unsigned cui_text_doc_revision(const cui_text_doc *d) { return d ? d->rev : 0; }
int cui_text_doc_longest_line(const cui_text_doc *d) { return d ? d->maxlen : 0; }

int cui_text_doc_read(const cui_text_doc *d, int start, int len, char *out) {
    if (!d) return 0;
    int total = TN(d->root).sl;
    start = CUI_CLAMP(start, 0, total);
    len = CUI_CLAMP(len, 0, total - start);
    int done = 0;
    while (done < len) {
        int cs, ce;
        const char *p = cui__doc_chunk(d, start + done, &cs, &ce);
        if (!p) break;
        int k = CUI_MIN(ce, start + len) - (start + done);
        memcpy(out + done, p + (start + done - cs), (size_t)k);
        done += k;
    }
    return done;
}

int cui_text_doc_char(const cui_text_doc *d, int pos) {
    int cs, ce;
    const char *p = d ? cui__doc_chunk(d, pos, &cs, &ce) : NULL;
    return p && pos >= 0 ? (unsigned char)p[pos - cs] : -1;
}

char *cui_text_doc_copy(const cui_text_doc *d, int start, int end, int *out_len) {
    int total = cui_text_doc_length(d);
    start = CUI_CLAMP(start, 0, total);
    end = end < 0 ? total : CUI_CLAMP(end, start, total);
    char *s = malloc((size_t)(end - start) + 1);
    if (!s) return NULL;
    int n = cui_text_doc_read(d, start, end - start, s);
    s[n] = 0;
    if (out_len) *out_len = n;
    return s;
}

cui_text_doc_iter cui_text_doc_iter_at(const cui_text_doc *d, int start, int end) {
    cui_text_doc_iter it;
    memset(&it, 0, sizeof it);
    int total = cui_text_doc_length(d);
    it.doc_ = d;
    it.pos = CUI_CLAMP(start, 0, total);
    it.end_ = end < 0 ? total : CUI_CLAMP(end, it.pos, total);
    return it;
}

bool cui_text_doc_iter_next(cui_text_doc_iter *it) {
    it->pos += it->len;
    it->len = 0; it->p = NULL;
    if (!it->doc_ || it->pos >= it->end_) return false;
    int cs, ce;
    const char *p = cui__doc_chunk(it->doc_, it->pos, &cs, &ce);
    if (!p) return false;
    it->p = p + (it->pos - cs);
    it->len = CUI_MIN(ce, it->end_) - it->pos;
    return true;
}

bool cui_text_doc_replace(cui_text_doc *d, int start, int end, const char *text, int len) {
    if (!d) return false;
    return cui__doc_edit(d, start, end, text ? text : "", text ? cui__td_len_of(text, len) : 0, NULL, NULL, NULL, CUI_MAX(start, 0), false);
}

bool cui_text_doc_undo(cui_text_doc *d, int *caret) { return d && cui__doc_undo(d, false, caret); }
bool cui_text_doc_redo(cui_text_doc *d, int *caret) { return d && cui__doc_undo(d, true, caret); }
bool cui_text_doc_can_undo(const cui_text_doc *d) { return d && d->hpos > 0; }
bool cui_text_doc_can_redo(const cui_text_doc *d) { return d && d->hpos < d->hn; }
void cui_text_doc_set_history(cui_text_doc *d, int steps) {
    if (!d) return;
    d->hmax = steps > 0 ? steps : CUI__TD_HISTORY;
    cui__td_trim(d);
}

void cui_text_doc_clear_history(cui_text_doc *d) {
    if (!d) return;
    for (int i = 0; i < d->hn; i++) cui__td_rec_free(&d->h[i]);
    d->hn = d->hpos = 0;
    d->base_state = d->state;
}

unsigned cui_text_doc_state(const cui_text_doc *d) { return d ? d->state : 0; }

size_t cui_text_doc_memory(const cui_text_doc *d) {
    if (!d) return 0;
    size_t m = sizeof *d + sizeof *d->n * (size_t)d->ncap + sizeof *d->b * (size_t)d->bcap + sizeof *d->h * (size_t)d->hcap;
    for (int i = 0; i < d->nb; i++) m += (size_t)d->b[i].cap + sizeof(int) * (size_t)d->b[i].ncap;
    for (int i = 0; i < d->hn; i++) m += sizeof(cui__tpc) * (size_t)(d->h[i].cdel + d->h[i].cins);
    return m;
}

int cui_text_doc_piece_count(const cui_text_doc *d) {
    if (!d) return 0;
    int unused = 0;
    for (int f = d->freel; f; f = TN(f).l) unused++;
    return d->nn - 1 - unused;
}

/* ---- snapshots ---- */

struct cui_text_snap {
    int len, n;
    const char **p;             /* chunk i: p[i][0, off[i + 1] - off[i]) */
    int *off;
    cui__tblk **k; int nk;      /* the buffers it keeps alive */
};

static void cui__td_snap_walk(const cui_text_doc *d, int t, cui_text_snap *s) {
    if (!t) return;
    cui__td_snap_walk(d, TN(t).l, s);
    s->p[s->n] = d->b[TN(t).buf].k->p + TN(t).start;
    s->off[s->n + 1] = s->off[s->n] + TN(t).len;
    s->n++;
    cui__td_snap_walk(d, TN(t).r, s);
}

cui_text_snap *cui_text_doc_snapshot(const cui_text_doc *d) {
    cui_text_snap *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    int pieces = d ? d->nn : 1;
    s->p = malloc(sizeof *s->p * (size_t)pieces);
    s->off = calloc((size_t)pieces + 1, sizeof *s->off);
    s->k = malloc(sizeof *s->k * (size_t)CUI_MAX(1, d ? d->nb : 0));
    if (!s->p || !s->off || !s->k) { free(s->p); free(s->off); free(s->k); free(s); return NULL; }
    if (d) {
        cui__td_snap_walk(d, d->root, s);
        s->len = TN(d->root).sl;
        for (int i = 0; i < d->nb; i++) { cui__td_atomic_add(&d->b[i].k->refs, 1); s->k[s->nk++] = d->b[i].k; }
    }
    return s;
}

void cui_text_snap_free(cui_text_snap *s) {
    if (!s) return;
    for (int i = 0; i < s->nk; i++) cui__tblk_drop(s->k[i]);
    free(s->p); free(s->off); free(s->k);
    free(s);
}

int cui_text_snap_length(const cui_text_snap *s) { return s ? s->len : 0; }
int cui_text_snap_chunks(const cui_text_snap *s) { return s ? s->n : 0; }
const char *cui_text_snap_chunk(const cui_text_snap *s, int i, int *len) {
    if (!s || i < 0 || i >= s->n) { if (len) *len = 0; return NULL; }
    if (len) *len = s->off[i + 1] - s->off[i];
    return s->p[i];
}

int cui_text_snap_read(const cui_text_snap *s, int start, int len, char *out) {
    if (!s) return 0;
    start = CUI_CLAMP(start, 0, s->len);
    len = CUI_CLAMP(len, 0, s->len - start);
    int lo = 0, hi = s->n - 1;   /* the chunk holding start */
    while (lo < hi) { int m = (lo + hi + 1) / 2; if (s->off[m] <= start) lo = m; else hi = m - 1; }
    int done = 0;
    for (int i = lo; i < s->n && done < len; i++) {
        int a = CUI_MAX(start + done, s->off[i]), e = CUI_MIN(start + len, s->off[i + 1]);
        if (e <= a) continue;
        memcpy(out + done, s->p[i] + (a - s->off[i]), (size_t)(e - a));
        done += e - a;
    }
    return done;
}

char *cui_text_snap_copy(const cui_text_snap *s, int *out_len) {
    int n = cui_text_snap_length(s);
    char *t = malloc((size_t)n + 1);
    if (!t) return NULL;
    n = cui_text_snap_read(s, 0, n, t);
    t[n] = 0;
    if (out_len) *out_len = n;
    return t;
}
