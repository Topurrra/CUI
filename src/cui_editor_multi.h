/* Multiple cursors and column (box) selection in the code editor, with cui_code_editor_opts.multi_cursor.
 *
 * cui_editor.c includes this file once, at its end: it works on the editor's own state with the editor's own
 * edit functions, and the editor calls in at a few hooks (the frame's start, the keys, a press and a drag, every
 * splice, painting). Without `.multi_cursor` none of it runs and st->mcur stays NULL.
 *
 * The primary selection is the editor's own caret and anchor (st->ed), so everything that knows one caret (the
 * find bar, following the caret, the result) works as before; the others are kept here, sorted and apart. A key
 * with several cursors runs the editor's one-caret edit (typing, Enter with its indentation and list items,
 * Backspace with its tab stops...) at each of them in turn, from the last to the first, so each edit's offsets are
 * still those of the text it read. The cursors already done only move by the bytes the later ones (before them)
 * add or take away, which is one running sum, so a key costs one edit per cursor and nothing per pair; an edit that
 * reaches past its own cursor (Backspace into the selection before it) moves the ones it reached exactly. All the
 * edits of a key are one undo step (cui__ed.mjoin joins them), typing on joins the step under way as with one
 * caret, and undo puts a caret back at each edit it takes back. */

/* Nothing here runs per byte or per line in view, only per key and cursor: compiled for size even where the editor is
 * built for speed (GCC). */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC push_options
#pragma GCC optimize("Os")
#endif

#define CUI__EDM_MAX 10000     /* cursors at most: every match, a column over many lines */
#define CUI__EDM_RUN 200       /* edits one step of typing on gathers at most (a document's log keeps 256 splices) */
#define CUI__EDM_FIND 65536    /* the longest selection Ctrl+D and Ctrl+Shift+L look for */

typedef struct { int anchor, cursor, pref, prim; } cui__edm_sel;   /* pref: the column Up and Down keep (-1 none); prim: the primary */

struct cui__edm {
    cui__edm_sel *x; int n, cap;        /* the other selections: sorted and apart once cui__edm_put made them */
    bool busy;                          /* the edits are this file's: it moves the cursors itself */
    bool collect;                       /* undo: keep the splices it makes */
    int (*sp)[3]; int spn, spcap;
    bool box;                           /* a column selection is under way, from line bl0, column bc0 to bl1, bc1 */
    int bl0, bc0, bl1, bc1;
    bool word;                          /* Ctrl+D looks for whole words (it began at a caret), while the selections */
    uint32_t word_sig;                  /* are the ones it left */
    int run_kind, run_n;                /* typing on: the last key's kind, the edits of its step, when, the cursors */
    double run_t;                       /* after it and the text's mark (a document's revision, or the history's size) */
    uint32_t run_sig; unsigned run_mark;
    uint32_t cp_hash; int cp_len, cp_n; /* the last copy of several selections: its text, and where each part is in it, */
    int *cp_cut; int cp_cap;            /* so a paste of it at as many cursors gives each its own */
    int *add; int add_n, add_cap;       /* cui_code_editor_add_selection: anchor, cursor pairs for the next frame */
};

/* what is in view: the other selections that reach it (sorted), and their carets on its lines (row, column) */
struct cui__edm_pv { const int *sa, *sb; int ns; const int *cl, *cc; int nc; };

static void *cui__edm_grow(void *p, int *cap, int need, size_t elem) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 16;
    while (c < need) c *= 2;
    void *q = realloc(p, elem * (size_t)c);
    if (!q) { fprintf(stderr, "cui: out of memory\n"); abort(); }
    *cap = c;
    return q;
}

static int cui__edm_count(const cui__ed_state *st) { return st->mcur ? st->mcur->n : 0; }
static int cui__edm_lo(const cui__edm_sel *s) { return CUI_MIN(s->anchor, s->cursor); }
static int cui__edm_hi(const cui__edm_sel *s) { return CUI_MAX(s->anchor, s->cursor); }

static void cui__edm_free(cui_ctx *ui, cui__ed_state *st) {
    (void)ui;
    cui__edm *m = st->mcur;
    if (!m) return;
    free(m->x); free(m->sp); free(m->cp_cut); free(m->add);
    free(m);
    st->mcur = NULL;
}

static void cui__edm_keys_default(cui_editor_keys *k) {
    int p = CUI_MOD_PRIMARY, a = CUI_MOD_ALT, s = CUI_MOD_SHIFT;
#if defined(__APPLE__)
    int col = p | a | s;   /* Option+Shift+arrows select by word there */
#else
    int col = a | s;
#endif
    k->combo[CUI_EDK_CURSOR_ABOVE][0] = (cui_key_combo){ p | a, CUI_KEY_UP };
    k->combo[CUI_EDK_CURSOR_BELOW][0] = (cui_key_combo){ p | a, CUI_KEY_DOWN };
    k->combo[CUI_EDK_ADD_NEXT][0] = (cui_key_combo){ p, CUI_KEY_D };
    k->combo[CUI_EDK_SELECT_MATCHES][0] = (cui_key_combo){ p | s, CUI_KEY_L };
    k->combo[CUI_EDK_COLUMN_UP][0] = (cui_key_combo){ col, CUI_KEY_UP };
    k->combo[CUI_EDK_COLUMN_DOWN][0] = (cui_key_combo){ col, CUI_KEY_DOWN };
    k->combo[CUI_EDK_COLUMN_LEFT][0] = (cui_key_combo){ col, CUI_KEY_LEFT };
    k->combo[CUI_EDK_COLUMN_RIGHT][0] = (cui_key_combo){ col, CUI_KEY_RIGHT };
}

/* ---- the set of selections ---- */

static int cui__edm_cmp(const void *pa, const void *pb) {
    const cui__edm_sel *a = pa, *b = pb;
    int x = cui__edm_lo(a), y = cui__edm_lo(b);
    if (x != y) return x < y ? -1 : 1;
    x = cui__edm_hi(a); y = cui__edm_hi(b);
    return x < y ? -1 : x > y;
}

/* Sorts s[0, n) and makes one of those that overlap, start together, or touch where one of them is a caret; the
 * primary stays one. Returns how many are left. */
static int cui__edm_merge(cui__edm_sel *s, int n) {
    if (n < 2) return n;
    qsort(s, (size_t)n, sizeof *s, cui__edm_cmp);
    int k = 0;
    for (int i = 1; i < n; i++) {
        cui__edm_sel *p = &s[k], *q = &s[i];
        int plo = cui__edm_lo(p), phi = cui__edm_hi(p), qlo = cui__edm_lo(q), qhi = cui__edm_hi(q);
        if (!(qlo < phi || qlo == plo || (qlo == phi && (qlo == qhi || plo == phi)))) { s[++k] = *q; continue; }
        bool pe = phi == plo, qe = qhi == qlo;
        const cui__edm_sel *d = pe != qe ? (pe ? q : p) : q->prim ? q : p;   /* the direction of a selection over a caret's, else the primary's */
        bool fwd = d->cursor >= d->anchor;
        int a = plo, b = CUI_MAX(phi, qhi), prim = p->prim | q->prim, pref = d->pref;
        p->anchor = fwd ? a : b; p->cursor = fwd ? b : a;
        p->pref = pref; p->prim = prim;
    }
    return k + 1;
}

/* every selection, the primary among them, sorted and apart: frame memory, with room for `more` */
static cui__edm_sel *cui__edm_all(cui_ctx *ui, cui__ed_state *st, int len, int *count, int more) {
    cui__edm *m = st->mcur;
    cui__edm_sel *s = cui__frame_alloc(ui, sizeof *s * (size_t)(m->n + 1 + more));
    for (int i = 0; i < m->n; i++) {
        s[i] = m->x[i];
        s[i].anchor = CUI_CLAMP(s[i].anchor, 0, len); s[i].cursor = CUI_CLAMP(s[i].cursor, 0, len);
        s[i].prim = 0;
    }
    s[m->n] = (cui__edm_sel){ st->ed.anchor, st->ed.cursor, st->pref_col, 1 };
    *count = cui__edm_merge(s, m->n + 1);
    return s;
}

/* s[0, n) become the selections: the primary (the last one when none is) the editor's, the others kept here */
static int cui__edm_put(cui__ed_state *st, cui__edm_sel *s, int n) {
    cui__edm *m = st->mcur;
    if (n <= 0) return 0;
    int p = -1;
    for (int i = 0; i < n; i++) if (s[i].prim) { if (p < 0) p = i; else s[i].prim = 0; }
    if (p < 0) s[n - 1].prim = 1;
    n = cui__edm_merge(s, n);
    m->x = cui__edm_grow(m->x, &m->cap, n, sizeof *m->x);
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].prim) { m->x[k++] = s[i]; continue; }
        st->ed.anchor = s[i].anchor; st->ed.cursor = s[i].cursor;
        st->pref_col = s[i].pref;
    }
    m->n = k;
    return n;
}

static uint32_t cui__edm_sig(const cui__edm_sel *s, int n) {
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) { h = (h ^ (uint32_t)s[i].anchor) * 16777619u; h = (h ^ (uint32_t)s[i].cursor) * 16777619u; }
    return h;
}

/* ---- hooks from the editor ---- */

/* bytes [a, b0) of the text became [a, b1): the other cursors move with it, unless the edit is this file's own */
static void cui__edm_spliced(cui__ed_state *st, int a, int b0, int b1, bool whole) {
    cui__edm *m = st->mcur;
    if (m->collect) {
        m->sp = cui__edm_grow(m->sp, &m->spcap, m->spn + 1, sizeof *m->sp);
        m->sp[m->spn][0] = a; m->sp[m->spn][1] = b0; m->sp[m->spn][2] = b1;
        m->spn++;
    }
    if (whole) { m->n = 0; m->box = false; return; }
    if (m->busy || !m->n) return;
    for (int i = 0; i < m->n; i++) {
        m->x[i].anchor = cui__ed_map(m->x[i].anchor, a, b0, b1);
        m->x[i].cursor = cui__ed_map(m->x[i].cursor, a, b0, b1);
    }
    m->box = false;
}

/* a select, go to or edit from code: one cursor again, unless it selects what the primary one has */
static void cui__edm_requests(cui__ed_state *st) {
    if (st->pend_edit || st->pend_goto || (st->pend_sel && (st->pend_a != st->ed.anchor || st->pend_c != st->ed.cursor))) {
        st->mcur->n = 0;
        st->mcur->box = false;
    }
}

static void cui__edm_frame(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_ctx *ui = E->ui;
    if (!f->o->multi_cursor) { cui__edm_free(ui, st); return; }
    if (!st->mcur) {
        st->mcur = calloc(1, sizeof *st->mcur);
        if (!st->mcur) { fprintf(stderr, "cui: out of memory\n"); abort(); }
    }
    cui__edm *m = st->mcur;
    if (!m->add_n) return;
    int n;
    cui__edm_sel *s = cui__edm_all(ui, st, E->len, &n, m->add_n);
    for (int i = 0; i < n; i++) s[i].prim = 0;
    for (int i = 0; i < m->add_n && n < CUI__EDM_MAX; i++)
        s[n++] = (cui__edm_sel){ CUI_CLAMP(m->add[2 * i], 0, E->len), CUI_CLAMP(m->add[2 * i + 1], 0, E->len), -1, i == m->add_n - 1 };
    m->add_n = 0;
    cui__edm_put(st, s, n);
    m->box = false;
    ui->focus = f->lid;
    E->reveal = true;
}

/* ---- edits at every cursor ---- */

typedef void (*cui__edm_op)(cui__edf *f, void *u, int i);

/* Whether a key of this kind (1 typing, 2 deleting, 3 other) goes on with the last one's undo step: within a second,
 * at the cursors it left, with nothing else edited in between. */
static bool cui__edm_joins(cui__edf *f, const cui__edm_sel *s, int n, int kind) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    if (kind == 3 || m->run_kind != kind || E->ui->time - m->run_t >= 1.0 || m->run_n + n > CUI__EDM_RUN) return false;
    if (m->run_sig != cui__edm_sig(s, n)) return false;
    if (E->doc) {
        unsigned rev = cui_text_doc_revision(E->doc);
        return st->doc_mine == rev && st->doc_seen == rev && m->run_mark == rev;
    }
    return st->hist.n > 0 && st->hist.pos == st->hist.n && (unsigned)st->hist.n == m->run_mark;
}

/* Runs op at every selection as if it were the only one (st->ed is that selection while it runs), from the last to
 * the first, as one undo step; then the selections are where the edits left them, joined where they meet. */
static void cui__edm_each(cui__edf *f, int kind, cui__edm_op op, void *u) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    int n;
    cui__edm_sel *s = cui__edm_all(ui, st, E->len, &n, 0);
    int *dat = cui__frame_alloc(ui, sizeof(int) * (size_t)n);   /* the running sum when a selection was done */
    bool join = cui__edm_joins(f, s, n, kind);
    bool on = E->rep_on;
    int ra = E->rep_a, rb0 = E->rep_b0, rb1 = E->rep_b1, D = 0, edits = 0;
    m->busy = true;
    for (int i = n - 1; i >= 0; i--) {
        E->mjoin = join || edits > 0;
        e->anchor = s[i].anchor; e->cursor = s[i].cursor;
        e->last_cursor = E->mjoin ? e->cursor : -1;   /* a new step: the one-caret rule must not join it either */
        st->pref_col = -1;
        E->rep_on = false;
        op(f, u, i);
        if (E->rep_on) {   /* the edit, [a, b0) of the text before it now [a, b1) */
            int a = E->rep_a, b0 = E->rep_b0, b1 = E->rep_b1, d = b1 - b0;
            for (int j = i + 1; j < n; j++) {   /* done ones it reached (an Enter that took the spaces after its caret) */
                int sh = D - dat[j];
                if (cui__edm_lo(&s[j]) + sh >= b0) break;
                s[j].anchor = cui__ed_map(s[j].anchor + sh, a, b0, b1);
                s[j].cursor = cui__ed_map(s[j].cursor + sh, a, b0, b1);
                dat[j] = D + d;
            }
            for (int k = i - 1; k >= 0 && cui__edm_hi(&s[k]) > a; k--) {   /* ones still to do (Backspace into a selection) */
                s[k].anchor = cui__ed_map(s[k].anchor, a, b0, b1);
                s[k].cursor = cui__ed_map(s[k].cursor, a, b0, b1);
            }
            D += d;
            edits++;
            cui__ed_span_add(&on, &ra, &rb0, &rb1, a, b0 - a, b1 - a);
        }
        s[i].anchor = e->anchor; s[i].cursor = e->cursor; s[i].pref = -1;
        dat[i] = D;
    }
    for (int j = 0; j < n; j++) { int sh = D - dat[j]; s[j].anchor += sh; s[j].cursor += sh; }
    m->busy = false;
    E->mjoin = false;
    E->rep_on = on; E->rep_a = ra; E->rep_b0 = rb0; E->rep_b1 = rb1;
    e->last_edit = 0;   /* one caret typing after this is a step of its own */
    n = cui__edm_put(st, s, n);
    if (edits) {
        m->run_n = (join ? m->run_n : 0) + edits;
        m->run_kind = kind; m->run_t = ui->time; m->run_sig = cui__edm_sig(s, n);
        m->run_mark = E->doc ? cui_text_doc_revision(E->doc) : (unsigned)st->hist.n;
        E->reveal = true;
    }
    m->box = false;
}

typedef struct { const char *t; int n; const int *ps, *pe; } cui__edm_txt;   /* ps, pe: each cursor's part of t, or NULL */

static void cui__edm_op_type(cui__edf *f, void *u, int i) {
    (void)i;
    const cui__edm_txt *x = u;
    cui__ed_type(f->E, x->t, x->n, f->lang_kind);
}
static void cui__edm_op_paste(cui__edf *f, void *u, int i) {
    const cui__edm_txt *x = u;
    cui_edit_state *e = &f->E->st->ed;
    const char *t = x->ps ? x->t + x->ps[i] : x->t;
    int n = x->ps ? x->pe[i] - x->ps[i] : x->n;
    cui__ed_replace(f->E, CUI_MIN(e->cursor, e->anchor), CUI_MAX(e->cursor, e->anchor), t, n, 3);
}
static void cui__edm_op_newline(cui__edf *f, void *u, int i) { (void)u; (void)i; cui__ed_newline(f->E, f->lang_kind); }
static void cui__edm_op_back(cui__edf *f, void *u, int i) { (void)u; (void)i; cui__edf_backspace(f); }
static void cui__edm_op_del(cui__edf *f, void *u, int i) {
    (void)u; (void)i;
    cui__ed *E = f->E;
    cui_edit_state *e = &E->st->ed;
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor);
    if (s1 > s0) { cui__ed_replace(E, s0, s1, "", 0, 3); return; }
    if (s0 >= E->len) return;
    int to = f->word_mod ? cui__ed_word_right(E, s0) : cui__ed_next(E, s0);
    if (cui__ed_replace(E, s0, to, "", 0, 2)) e->cursor = e->anchor = e->last_cursor = s0;
}
static void cui__edm_op_tab(cui__edf *f, void *u, int i) {
    (void)u; (void)i;
    cui__ed *E = f->E;
    int s0 = CUI_MIN(E->st->ed.cursor, E->st->ed.anchor);
    int col = cui__ed_caret_col(E, s0), n = E->use_tabs ? 1 : E->tab - col % E->tab;
    char sp[17];
    memset(sp, E->use_tabs ? '\t' : ' ', sizeof sp);
    cui__ed_replace(E, s0, s0, sp, n, 3);
}

/* Tab and Shift+Tab on lines: every line any selection touches, once, indented or outdented as one undo step. The
 * edits are at line starts, made from the last line up; a position then moves by the edits before it (a caret at a
 * line's start stays there, as with one selection). */
static void cui__edm_indent(cui__edf *f, bool out) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_ctx *ui = E->ui;
    int n;
    cui__edm_sel *s = cui__edm_all(ui, st, E->len, &n, 0);
    int *L = cui__frame_alloc(ui, sizeof(int) * (size_t)n * 2), nr = 0, lines = 0;
    for (int i = 0; i < n; i++) {   /* the lines, as runs */
        int s0 = cui__edm_lo(&s[i]), s1 = cui__edm_hi(&s[i]);
        int l0 = cui__ed_line_of(E, s0), l1 = cui__ed_line_of(E, s1);
        if (s1 > s0 && l1 > l0 && s1 == cui__ed_ls(E, l1)) l1--;   /* a selection that ends at a line start leaves that line alone */
        if (nr && l0 <= L[2 * nr - 1]) { lines += CUI_MAX(0, l1 - L[2 * nr - 1]); L[2 * nr - 1] = CUI_MAX(L[2 * nr - 1], l1); }
        else { L[2 * nr] = l0; L[2 * nr + 1] = l1; nr++; lines += l1 - l0 + 1; }
    }
    int *ea = cui__frame_alloc(ui, sizeof(int) * (size_t)(lines * 3 + 1)), *ed = ea + lines, *pre = ed + lines;
    int ne = lines, unit = E->use_tabs ? 1 : E->tab;
    char sp[17];
    memset(sp, E->use_tabs ? '\t' : ' ', sizeof sp);
    m->busy = true;
    for (int r = nr - 1; r >= 0; r--)
        for (int line = L[2 * r + 1]; line >= L[2 * r]; line--) {
            int ls = cui__ed_ls(E, line), le = cui__ed_end(E, line), del = 0, ins = 0;
            if (!out) ins = le > ls ? unit : 0;
            else if (ls < le && cui__ed_ch(E, ls) == '\t') del = 1;
            else while (ls + del < le && del < E->tab && cui__ed_ch(E, ls + del) == ' ') del++;
            if (!del && !ins) continue;
            E->mjoin = ne < lines;
            if (!cui__ed_replace(E, ls, ls + del, sp, ins, 3)) continue;
            ne--;
            ea[ne] = ls; ed[ne] = ins - del;   /* (ins - del: one of them is 0) */
        }
    m->busy = false;
    E->mjoin = false;
    st->ed.last_edit = 0;
    int k = lines - ne;   /* the edits, from line start ea[ne] up */
    if (!k) return;
    ea += ne; ed += ne;
    pre[0] = 0;
    for (int i = 0; i < k; i++) pre[i + 1] = pre[i] + ed[i];
    for (int i = 0; i < n; i++)
        for (int w = 0; w < 2; w++) {
            int *p = w ? &s[i].cursor : &s[i].anchor;
            int lo = 0, hi = k;   /* the edits at line starts before *p */
            while (lo < hi) { int mid = (lo + hi) / 2; if (ea[mid] < *p) lo = mid + 1; else hi = mid; }
            if (lo > 0 && ed[lo - 1] < 0 && *p < ea[lo - 1] - ed[lo - 1]) *p = ea[lo - 1] + pre[lo - 1];   /* in the indentation taken away */
            else *p += pre[lo];
        }
    cui__edm_put(st, s, n);
    m->box = false;
    m->run_kind = 0;
}

/* Copy and cut: each selection's text, or the whole line of a caret (once a line), joined by line breaks; cut
 * takes them away as one undo step. The parts are kept so a paste at as many cursors gives each its own. */
static void cui__edm_clip(cui__edf *f, bool cut) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_ctx *ui = E->ui;
    int n;
    cui__edm_sel *s = cui__edm_all(ui, st, E->len, &n, 0);
    int *ra = cui__frame_alloc(ui, sizeof(int) * (size_t)n * 3), *rb = ra + n, *own = rb + n, nr = 0;
    size_t total = 0;
    for (int i = 0; i < n; i++) {
        int a = cui__edm_lo(&s[i]), b = cui__edm_hi(&s[i]);
        if (a == b) cui__ed_line_at(E, a, &a, &b);
        if (nr && a < rb[nr - 1]) { total += (size_t)CUI_MAX(0, b - rb[nr - 1]); rb[nr - 1] = CUI_MAX(rb[nr - 1], b); }
        else { ra[nr] = a; rb[nr] = b; nr++; total += (size_t)(b - a); }
        own[i] = nr - 1;
    }
    char *t = cui__frame_alloc(ui, total + (size_t)nr + 1);
    m->cp_cut = cui__edm_grow(m->cp_cut, &m->cp_cap, nr * 2, sizeof(int));
    int len = 0;
    for (int r = 0; r < nr; r++) {
        if (len && t[len - 1] != '\n') t[len++] = '\n';
        m->cp_cut[r] = len;
        cui__ed_read(E, ra[r], rb[r], t + len);
        len += rb[r] - ra[r];
        m->cp_cut[nr + r] = len;
    }
    t[len] = 0;
    m->cp_n = nr; m->cp_len = len; m->cp_hash = cui__hash(t, (size_t)len, 0xC0B1u);
    if (len > 0 && ui->platform.set_clipboard) ui->platform.set_clipboard(ui->platform.user, t);
    m->box = false;
    if (!cut || E->ro || len == 0) return;
    m->busy = true;
    bool first = true;
    for (int r = nr - 1; r >= 0; r--) {
        if (rb[r] <= ra[r]) continue;
        E->mjoin = !first;
        if (cui__ed_replace(E, ra[r], rb[r], "", 0, 3)) first = false;
    }
    m->busy = false;
    E->mjoin = false;
    st->ed.last_edit = 0;
    int gone = 0;   /* each selection goes to the start of what it was in */
    for (int r = 0, i = 0; r < nr; r++) {
        for (; i < n && own[i] == r; i++) { s[i].anchor = s[i].cursor = ra[r] - gone; s[i].pref = -1; }
        gone += rb[r] - ra[r];
    }
    cui__edm_put(st, s, n);
    m->run_kind = 0;
}

static void cui__edm_paste(cui__edf *f, const char *clip) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_ctx *ui = E->ui;
    size_t cl = strlen(clip);
    if (cl > (size_t)(INT_MAX / 2)) return;
    int len = (int)cl, n;
    cui__edm_all(ui, st, E->len, &n, 0);
    cui__edm_txt x = { clip, len, NULL, NULL };
    if (m->cp_n == n && m->cp_len == len && m->cp_hash == cui__hash(clip, cl, 0xC0B1u)) { x.ps = m->cp_cut; x.pe = m->cp_cut + n; }
    else if (n > 1) {   /* as many lines as cursors (a last line break ends the last line): one each */
        int lines = (int)cui__ed_count_byte(clip, cl, '\n') + 1;
        if (lines > 1 && clip[len - 1] == '\n') lines--;
        if (lines == n) {
            int *ps = cui__frame_alloc(ui, sizeof(int) * (size_t)n * 2), *pe = ps + n;
            for (int i = 0, a = 0; i < n; i++) {
                const char *nl = memchr(clip + a, '\n', (size_t)(len - a));
                int b = nl ? (int)(nl - clip) : len;
                ps[i] = a; pe[i] = b;
                a = b + 1;
            }
            x.ps = ps; x.pe = pe;
        }
    }
    cui__edm_each(f, 3, cui__edm_op_paste, &x);
    m->run_kind = 0;
}

/* Undo and redo with the carets of the step: one at each edit taken back (where its caret was) or done again
 * (after it). A step of several keys repeats its edits once a key, in one direction a key (up the text for undo):
 * the carets of the last key undone are those of the step. */
static void cui__edm_undo(cui__edf *f, bool redo) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    m->spn = 0;
    m->collect = m->busy = true;
    bool ok = cui__ed_undo(E, redo);
    m->collect = m->busy = false;
    m->run_kind = 0; m->box = false;
    if (!ok) return;
    E->changed = E->reveal = true;
    st->pref_col = -1;
    m->n = 0;
    int k = m->spn;
    if (k < 2) return;
    cui__edm_sel *s = cui__frame_alloc(E->ui, sizeof *s * (size_t)k);
    int *cd = cui__frame_alloc(E->ui, sizeof(int) * (size_t)k), c = 0, D = 0, prev = 0;
    for (int i = 0; i < k; i++) {
        int a = m->sp[i][0], b0 = m->sp[i][1], b1 = m->sp[i][2];
        if (c && (redo ? a > prev : a < prev)) c = D = 0;   /* the next key back */
        prev = a;
        D += b1 - b0;
        s[c] = (cui__edm_sel){ b1, b1, -1, 0 };
        cd[c++] = D;
    }
    int best = 0;
    for (int i = 0; i < c; i++) {
        if (redo) { s[i].anchor += D - cd[i]; s[i].cursor = s[i].anchor; }   /* the edits after it were before it */
        s[i].anchor = s[i].cursor = CUI_CLAMP(s[i].anchor, 0, E->len);
        if (abs(s[i].cursor - e->cursor) < abs(s[best].cursor - e->cursor)) best = i;
    }
    s[best].prim = 1;
    cui__edm_put(st, s, c);
}

/* ---- adding cursors ---- */

/* Ctrl+Alt+Up and Down: a caret on the line above the first cursor, or below the last, at its column */
static void cui__edm_add_line(cui__edf *f, int dir) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    int n;
    cui__edm_sel *s = cui__edm_all(E->ui, st, E->len, &n, 1);
    st->mcur->box = false;
    if (n >= CUI__EDM_MAX) return;
    const cui__edm_sel *src = dir < 0 ? &s[0] : &s[n - 1];
    int line = cui__ed_line_of(E, dir < 0 ? cui__edm_lo(src) : cui__edm_hi(src)) + dir;
    if (line < 0 || line >= E->nl) return;
    int col = src->pref >= 0 ? src->pref : cui__ed_caret_col(E, src->cursor);
    int pos = cui__ed_pos(E, line, col);
    for (int i = 0; i < n; i++) s[i].prim = 0;
    s[n] = (cui__edm_sel){ pos, pos, col, 1 };
    cui__edm_put(st, s, n + 1);
    E->reveal = true;
}

/* a column selection: on each line from bl0 to bl1, columns bc0 to bc1; lines that end before it are left out (but
 * for a column of carets, and the line the column is being drawn to) */
static void cui__edm_box_apply(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    int l0 = CUI_MIN(m->bl0, m->bl1), l1 = CUI_MAX(m->bl0, m->bl1);
    if (l1 - l0 >= CUI__EDM_MAX) { if (m->bl1 > m->bl0) l0 = l1 - CUI__EDM_MAX + 1; else l1 = l0 + CUI__EDM_MAX - 1; }   /* the lines next to the pointer */
    int lo = CUI_MIN(m->bc0, m->bc1), hi = CUI_MAX(m->bc0, m->bc1), c = 0;
    cui__edm_sel *s = cui__frame_alloc(E->ui, sizeof *s * (size_t)(l1 - l0 + 1));
    for (int l = l0; l <= l1; l++) {
        if (lo != hi && l != m->bl1 && cui__ed_col_at(E, l, cui__ed_end(E, l)) < lo) continue;
        s[c++] = (cui__edm_sel){ cui__ed_pos(E, l, m->bc0), cui__ed_pos(E, l, m->bc1), m->bc1, l == m->bl1 };
    }
    m->n = 0;
    cui__edm_put(st, s, c);
    st->aff_pos = -1;
    E->reveal = true;
}

/* Alt+Shift+arrows: the column from the primary selection, its far end moved a line or a column */
static void cui__edm_box_key(cui__edf *f, int dl, int dc) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    if (!m->box) {
        m->bl0 = cui__ed_line_of(E, e->anchor); m->bc0 = cui__ed_col_at(E, m->bl0, e->anchor);
        m->bl1 = cui__ed_line_of(E, e->cursor);
        m->bc1 = st->pref_col >= 0 ? st->pref_col : cui__ed_col_at(E, m->bl1, e->cursor);
        m->box = true;
    }
    m->bl1 = CUI_CLAMP(m->bl1 + dl, 0, E->nl - 1);
    m->bc1 = CUI_CLAMP(m->bc1 + dc, 0, 1 << 24);
    cui__edm_box_apply(f);
}

static bool cui__edm_isword(const cui__ed *E, int i) { return i >= 0 && i < E->len && cui__edit_is_word((unsigned char)cui__ed_ch(E, i)); }

/* the word at or just before pos */
static bool cui__edm_word(const cui__ed *E, int pos, int *a, int *b) {
    int i = pos, j = pos;
    while (cui__edm_isword(E, i - 1)) i--;
    while (cui__edm_isword(E, j)) j++;
    *a = i; *b = j;
    return j > i;
}

static bool cui__edm_eq(const cui__ed *E, int at, const char *q, int qn) {
    int cs, ce;
    const char *s = cui__ed_chunk(E, at, &cs, &ce);
    if (s && at + qn <= ce) return memcmp(s + at, q, (size_t)qn) == 0;
    for (int i = 0; i < qn; i++) if (cui__ed_ch(E, at + i) != q[i]) return false;
    return true;
}

/* the first match of q[0, qn) starting in [from, to - qn], whole words only with word; -1 none */
static int cui__edm_find(const cui__ed *E, const char *q, int qn, int from, int to, bool word) {
    to = CUI_MIN(to, E->len);
    for (int p = CUI_MAX(from, 0); p <= to - qn;) {
        int cs, ce;
        const char *s = cui__ed_chunk(E, p, &cs, &ce);
        if (!s) return -1;
        int lim = CUI_MIN(ce, to - qn + 1);
        const char *h = cui__find_byte(s + p, (size_t)(lim - p), q[0]);
        if (!h) { p = lim; continue; }
        int at = (int)(h - s);
        if (cui__edm_eq(E, at, q, qn) && (!word || (!cui__edm_isword(E, at - 1) && !cui__edm_isword(E, at + qn)))) return at;
        p = at + 1;
    }
    return -1;
}

/* whether [a, b) overlaps one of the sorted, apart selections s[0, n) */
static bool cui__edm_taken(const cui__edm_sel *s, int n, int a, int b) {
    int lo = 0, hi = n;
    while (lo < hi) { int mid = (lo + hi) / 2; if (cui__edm_hi(&s[mid]) <= a) lo = mid + 1; else hi = mid; }
    return lo < n && cui__edm_lo(&s[lo]) < b;
}

/* Ctrl+D: at a caret the word there (at every caret); with a selection the next match of it after the primary one,
 * around past the end, that no selection holds yet */
static void cui__edm_next(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    int n;
    cui__edm_sel *s = cui__edm_all(E->ui, st, E->len, &n, 1);
    m->box = false;
    E->reveal = true;
    if (e->cursor == e->anchor) {
        bool any = false;
        for (int i = 0; i < n; i++) {
            int a, b;
            if (s[i].anchor == s[i].cursor && cui__edm_word(E, s[i].cursor, &a, &b)) { s[i].anchor = a; s[i].cursor = b; any = true; }
        }
        if (any) { m->word = true; m->word_sig = cui__edm_sig(s, cui__edm_put(st, s, n)); }
        return;
    }
    int s0 = CUI_MIN(e->cursor, e->anchor), s1 = CUI_MAX(e->cursor, e->anchor), qn = s1 - s0;
    if (qn > CUI__EDM_FIND || n >= CUI__EDM_MAX) return;
    bool word = m->word = m->word && m->word_sig == cui__edm_sig(s, n);
    char *q = cui__frame_alloc(E->ui, (size_t)qn);
    cui__ed_read(E, s0, s1, q);
    for (int from = s1, to = E->len, round = 0;;) {
        int at = cui__edm_find(E, q, qn, from, to, word);
        if (at < 0) {
            if (round++) return;
            from = 0; to = s0 - 1 + qn;   /* from the top, up to the primary one */
            continue;
        }
        if (cui__edm_taken(s, n, at, at + qn)) { from = at + 1; continue; }
        for (int i = 0; i < n; i++) s[i].prim = 0;
        s[n] = (cui__edm_sel){ at, at + qn, -1, 1 };
        m->word_sig = cui__edm_sig(s, cui__edm_put(st, s, n + 1));
        return;
    }
}

/* Ctrl+Shift+L: every match of the selection (at a caret, of the word there), from the primary one on and around,
 * CUI__EDM_MAX at most */
static void cui__edm_matches(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    int a = CUI_MIN(e->cursor, e->anchor), b = CUI_MAX(e->cursor, e->anchor), n0;
    const cui__edm_sel *s0 = cui__edm_all(E->ui, st, E->len, &n0, 0);
    bool word = m->word && m->word_sig == cui__edm_sig(s0, n0);
    m->box = false;
    if (a == b) { if (!cui__edm_word(E, a, &a, &b)) return; word = true; }
    int qn = b - a;
    if (qn > CUI__EDM_FIND) return;
    char *q = cui__frame_alloc(E->ui, (size_t)qn);
    cui__ed_read(E, a, b, q);
    int n = 0, cap = 64;
    cui__edm_sel *s = malloc(sizeof *s * (size_t)cap);
    if (!s) return;
    for (int round = 0; round < 2 && n < CUI__EDM_MAX; round++)
        for (int p = round ? 0 : a, to = round ? a - 1 + qn : E->len; n < CUI__EDM_MAX;) {
            int at = cui__edm_find(E, q, qn, p, to, word);
            if (at < 0) break;
            s = cui__edm_grow(s, &cap, n + 1, sizeof *s);
            s[n++] = (cui__edm_sel){ at, at + qn, -1, at == a };
            p = at + qn;
        }
    if (n) {
        m->n = 0;
        m->word = word;
        m->word_sig = cui__edm_sig(s, cui__edm_put(st, s, n));
        E->reveal = true;
    }
    free(s);
}

/* ---- the keys ---- */

/* one of the commands (.keys or the usual ones) was pressed: its key is taken, so the one-caret keys below miss it */
static bool cui__edm_k(cui_ctx *ui, const cui_editor_keys *K, int cmd) {
    for (int j = 0; j < 2; j++) {
        cui_key_combo c = K->combo[cmd][j];
        if (c.key && cui_shortcut(ui, c.mods, c.key)) { ui->kpressed[c.key] = 0; return true; }
    }
    return false;
}
static void cui__edm_take(cui_ctx *ui, const cui_code_editor_opts *o, int cmd, cui_key usual) {
    if (!o->keys) { ui->kpressed[usual] = 0; return; }
    for (int j = 0; j < 2; j++) ui->kpressed[o->keys->combo[cmd][j].key] = 0;
}

/* the arrows, pages, Home and End at every cursor: the one-caret motion at each */
static bool cui__edm_moves(cui__edf *f) {
    static const cui_key keys[] = { CUI_KEY_LEFT, CUI_KEY_RIGHT, CUI_KEY_UP, CUI_KEY_DOWN, CUI_KEY_PAGE_UP, CUI_KEY_PAGE_DOWN,
                                    CUI_KEY_HOME, CUI_KEY_END };
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    bool any = false;
    for (int k = 0; k < CUI_COUNTOF(keys); k++) any |= ui->kpressed[keys[k]] != 0;
    if (!any) return false;
    int n;
    cui__edm_sel *s = cui__edm_all(ui, st, E->len, &n, 0);
    double sy = st->sy, psy = sy;
    int aff = st->aff_pos, paff = -1;
    for (int i = 0; i < n; i++) {
        e->anchor = s[i].anchor; e->cursor = s[i].cursor;
        st->pref_col = s[i].pref;
        st->aff_pos = s[i].prim ? aff : -1;
        int vm = 0, vp = 0;
        cui__edf_move_keys(f, &vm, &vp);
        s[i].anchor = e->anchor; s[i].cursor = e->cursor;
        s[i].pref = vm || vp ? st->pref_col : -1;
        if (s[i].prim) { psy = st->sy; paff = st->aff_pos; }
        st->sy = sy;   /* a page moves the view once, with the primary one */
    }
    st->sy = psy;
    for (int k = 0; k < CUI_COUNTOF(keys); k++) ui->kpressed[keys[k]] = 0;
    cui__edm_put(st, s, n);
    st->aff_pos = paff;
    st->mcur->box = false;
    st->mcur->run_kind = 0;
    return true;
}

static bool cui__edm_keys(cui__edf *f) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    const cui_code_editor_opts *o = f->o;
    cui_editor_keys def;
    if (!o->keys) cui_editor_keys_default(&def);
    const cui_editor_keys *K = o->keys ? o->keys : &def;
    if (!E->ro) {   /* undo and redo bring the step's carets back, also once the others are gone */
        bool undo = cui__edk(ui, o, CUI_EDK_UNDO, f->prim && !f->shift && cui_key_pressed(ui, CUI_KEY_Z));
        bool redo = cui__edk(ui, o, CUI_EDK_REDO, f->prim && ((f->shift && cui_key_pressed(ui, CUI_KEY_Z)) || cui_key_pressed(ui, CUI_KEY_Y)));
        if (undo || redo) {
            cui__edm_undo(f, redo);
            cui__edm_take(ui, o, CUI_EDK_UNDO, CUI_KEY_Z);
            cui__edm_take(ui, o, CUI_EDK_REDO, CUI_KEY_Y);
            if (!o->keys) ui->kpressed[CUI_KEY_Z] = 0;
            return true;
        }
    }
    if (m->n && cui_key_pressed(ui, CUI_KEY_ESCAPE)) {   /* one caret again: the primary one */
        m->n = 0; m->box = false;
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
        return true;
    }
    bool cmd = true;
    if (cui__edm_k(ui, K, CUI_EDK_CURSOR_ABOVE)) cui__edm_add_line(f, -1);
    else if (cui__edm_k(ui, K, CUI_EDK_CURSOR_BELOW)) cui__edm_add_line(f, 1);
    else if (cui__edm_k(ui, K, CUI_EDK_COLUMN_UP)) cui__edm_box_key(f, -1, 0);
    else if (cui__edm_k(ui, K, CUI_EDK_COLUMN_DOWN)) cui__edm_box_key(f, 1, 0);
    else if (cui__edm_k(ui, K, CUI_EDK_COLUMN_LEFT)) cui__edm_box_key(f, 0, -1);
    else if (cui__edm_k(ui, K, CUI_EDK_COLUMN_RIGHT)) cui__edm_box_key(f, 0, 1);
    else if (cui__edm_k(ui, K, CUI_EDK_ADD_NEXT)) cui__edm_next(f);
    else if (cui__edm_k(ui, K, CUI_EDK_SELECT_MATCHES)) cui__edm_matches(f);
    else cmd = false;
    if (cmd) { m->run_kind = 0; return true; }
    if (!m->n) return false;
    /* several cursors: the keys below are theirs, and the one-caret code under this sees none of them */
    if (cui__edk(ui, o, CUI_EDK_SELECT_ALL, f->prim && cui_key_pressed(ui, CUI_KEY_A))) { m->n = 0; m->box = false; return false; }
    bool did = cui__edm_moves(f);
    bool k_copy = cui__edk(ui, o, CUI_EDK_COPY, f->prim && cui_key_pressed(ui, CUI_KEY_C));
    bool k_cut = cui__edk(ui, o, CUI_EDK_CUT, f->prim && cui_key_pressed(ui, CUI_KEY_X));
    if (k_copy || k_cut) {
        cui__edm_clip(f, k_cut);
        cui__edm_take(ui, o, CUI_EDK_COPY, CUI_KEY_C);
        cui__edm_take(ui, o, CUI_EDK_CUT, CUI_KEY_X);
        did = true;
    }
    if (E->ro) return did;
    bool empty = e->cursor == e->anchor;   /* all carets, no selection: typing and deleting go on in one step */
    for (int i = 0; i < m->n && empty; i++) empty = m->x[i].anchor == m->x[i].cursor;
    if (cui__edk(ui, o, CUI_EDK_PASTE, f->prim && cui_key_pressed(ui, CUI_KEY_V))) {
        const char *clip = ui->platform.get_clipboard ? ui->platform.get_clipboard(ui->platform.user) : NULL;
        if (clip && *clip) cui__edm_paste(f, clip);
        cui__edm_take(ui, o, CUI_EDK_PASTE, CUI_KEY_V);
        did = true;
    }
    int reps = ui->kpressed[CUI_KEY_BACKSPACE];
    for (int rep = 0; rep < reps; rep++) { ui->kpressed[CUI_KEY_BACKSPACE] = 1; cui__edm_each(f, empty ? 2 : 3, cui__edm_op_back, NULL); empty = true; }
    reps = ui->kpressed[CUI_KEY_DELETE];
    for (int rep = 0; rep < reps; rep++) { cui__edm_each(f, empty ? 2 : 3, cui__edm_op_del, NULL); empty = true; }
    if (!f->prim) {
        for (int rep = ui->kpressed[CUI_KEY_ENTER]; rep > 0; rep--) cui__edm_each(f, 3, cui__edm_op_newline, NULL);
        did |= ui->kpressed[CUI_KEY_ENTER] != 0;
        ui->kpressed[CUI_KEY_ENTER] = 0;
    }
    did |= ui->kpressed[CUI_KEY_BACKSPACE] || ui->kpressed[CUI_KEY_DELETE];
    ui->kpressed[CUI_KEY_BACKSPACE] = ui->kpressed[CUI_KEY_DELETE] = 0;
    if (cui_key_pressed(ui, CUI_KEY_TAB) && !(f->mods & CUI_MOD_CTRL)) {
        bool lines = f->shift || !empty;   /* indent lines; at carets only, spaces to the next stop (a list item: its line) */
        for (int i = -1; i < m->n && !lines && f->lang_kind == 3; i++) {
            int ls, le;
            cui__ed_mdi it;
            const char *s = cui__ed_line(E, cui__ed_line_of(E, i < 0 ? e->cursor : m->x[i].cursor), &ls, &le);
            lines = cui__ed_md_item(s, ls, le, &it);
        }
        if (lines) cui__edm_indent(f, f->shift);
        else cui__edm_each(f, 3, cui__edm_op_tab, NULL);
        ui->kpressed[CUI_KEY_TAB] = 0;
        did = true;
    }
    if (ui->text_in_len > 0 && !(f->prim && !(f->mods & CUI_MOD_ALT))) {
        cui__edm_txt x = { ui->text_in, ui->text_in_len, NULL, NULL };
        cui__edm_each(f, empty ? 1 : 3, cui__edm_op_type, &x);
        ui->text_in_len = 0;
        did = true;
    }
    return did;
}

/* ---- the pointer ---- */

/* the line and column under the pointer, for a column selection (past a line's end the column goes on; wrapped, the
 * column of the byte picked) */
static void cui__edm_at(cui__edf *f, int line, double col, int *l, int *c) {
    cui__ed *E = f->E;
    if (E->wrap) {
        bool aff;
        int pos = cui__ed_vpick(E, CUI_MAX(0, line), col, &aff);
        *l = cui__ed_line_of(E, pos); *c = cui__ed_col_at(E, *l, pos);
        return;
    }
    *l = CUI_CLAMP(line, 0, E->nl - 1);
    *c = (int)CUI_CLAMP(floor(col + 0.5), 0.0, (double)(1 << 24));
}

/* A press in the code: Alt+Shift starts a column; Alt adds a caret (the editor's press then puts the primary one
 * there) or takes away the one there; a press without Alt is one caret again. True: the press is taken. */
static bool cui__edm_press(cui__edf *f, int line, double col) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_edit_state *e = &st->ed;
    cui_ctx *ui = E->ui;
    m->box = false; m->run_kind = 0;
    if (!(f->mods & CUI_MOD_ALT) || (f->mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) || ui->mx < f->gx) { m->n = 0; return false; }
    if (f->shift) {
        cui__edm_at(f, line, col, &m->bl0, &m->bc0);
        m->bl1 = m->bl0; m->bc1 = m->bc0;
        m->box = true;
        cui__edm_box_apply(f);
        st->drag = 4;   /* (a drag goes on with the column: cui__edm_drag) */
        ui->caret_time = ui->time;
        return true;
    }
    if (ui->click_count >= 2) return false;   /* Alt+double click: the word at the caret the first click added */
    bool aff = false;
    int pos = E->wrap ? cui__ed_vpick(E, line, col, &aff) : cui__ed_pos(E, line, col);
    for (int i = 0; i < m->n; i++)
        if (m->x[i].anchor == pos && m->x[i].cursor == pos) {   /* a caret there goes */
            memmove(&m->x[i], &m->x[i + 1], sizeof *m->x * (size_t)(m->n - i - 1));
            m->n--;
            return true;
        }
    if (m->n && e->cursor == pos && e->anchor == pos) {   /* the primary one goes: the last other one takes its place */
        cui__edm_sel q = m->x[--m->n];
        e->anchor = q.anchor; e->cursor = q.cursor; st->pref_col = q.pref;
        return true;
    }
    if (m->n + 1 >= CUI__EDM_MAX) return true;
    m->x = cui__edm_grow(m->x, &m->cap, m->n + 1, sizeof *m->x);
    m->x[m->n++] = (cui__edm_sel){ e->anchor, e->cursor, -1, 0 };   /* sorted again before it is used */
    return false;
}

static bool cui__edm_drag(cui__edf *f, int line, double col, int pos) {
    (void)pos;
    cui__ed_state *st = f->E->st;
    cui__edm *m = st->mcur;
    if (st->drag != 4) return false;
    int l, c;
    cui__edm_at(f, line, col, &l, &c);
    if (!m->box) { m->box = true; m->bl0 = l; m->bc0 = c; }
    if (l != m->bl1 || c != m->bc1) {
        m->bl1 = l; m->bc1 = c;
        cui__edm_box_apply(f);
        f->E->ui->caret_time = f->E->ui->time;
    }
    return true;
}

/* ---- painting ---- */

static const struct cui__edm_pv *cui__edm_view(cui__edf *f, int a, int b) {
    cui__ed *E = f->E;
    cui__ed_state *st = E->st;
    cui__edm *m = st->mcur;
    cui_ctx *ui = E->ui;
    if (!m->n) return NULL;
    int n;
    cui__edm_sel *all = cui__edm_all(ui, st, E->len, &n, 0);
    cui__edm_put(st, all, n);   /* sorted and apart from here on */
    if (!m->n) return NULL;
    const cui__edm_sel *x = m->x;
    int lo = 0, hi = m->n;   /* the first that ends at a or after, then those that start by b */
    while (lo < hi) { int mid = (lo + hi) / 2; if (cui__edm_hi(&x[mid]) < a) lo = mid + 1; else hi = mid; }
    int i1 = lo;
    while (i1 < m->n && cui__edm_lo(&x[i1]) <= b) i1++;
    int k = i1 - lo;
    if (!k) return NULL;
    struct cui__edm_pv *v = cui__frame_alloc(ui, sizeof *v);
    int *buf = cui__frame_alloc(ui, sizeof(int) * (size_t)k * 4);
    int *sa = buf, *sb = buf + k, *cl = buf + 2 * k, *cc = buf + 3 * k;
    v->ns = v->nc = 0;
    for (int i = lo; i < i1; i++) {
        int s0 = cui__edm_lo(&x[i]), s1 = cui__edm_hi(&x[i]), c = x[i].cursor;
        if (s1 > s0) { sa[v->ns] = s0; sb[v->ns] = s1; v->ns++; }
        if (c < a || c > b) continue;
        int col, line = cui__ed_line_of(E, c);
        if (E->wrap) line = cui__ed_vrow(E, c, &col);
        else col = cui__ed_col_at(E, line, c);
        cl[v->nc] = line; cc[v->nc] = col; v->nc++;
    }
    v->sa = sa; v->sb = sb; v->cl = cl; v->cc = cc;
    return v;
}

/* the other selections on one line, or one row of a wrapped line: bytes [als, ale) of it, of which s holds [ls, le)
 * from column c0 (last: the row ends the line, so a selection past it takes its line break) */
static void cui__edm_paint_sel(cui_ctx *ui, const struct cui__edm_pv *v, const char *s, int als, int ale, int ls, int le, int c0, bool last,
                               int cb, int tab, float cw, float x0, float y, float lh, cui_color col) {
    int lo = 0, hi = v->ns;
    while (lo < hi) { int mid = (lo + hi) / 2; if (v->sb[mid] <= als) lo = mid + 1; else hi = mid; }
    for (int i = lo; i < v->ns; i++) {
        int s0 = v->sa[i], s1 = v->sb[i];
        if (last ? s0 > ale : s0 >= ale) break;
        int a = CUI_CLAMP(s0, ls, le), b = CUI_CLAMP(s1, ls, le);
        float xa = (float)(cui__ed_adv(s, ls, a, tab, c0) - cb) * cw, xb = (float)(cui__ed_adv(s, ls, b, tab, c0) - cb) * cw;
        if (last && s1 > ale) xb += cw * 0.6f;   /* the line break is selected too */
        cui_draw_rect(ui, (cui_rect){ x0 + xa, y, xb - xa, lh }, CUI_CORNERS(2), col);
    }
}

/* the other carets, blinking with the primary one (top: where row 0 is before the scroll) */
static void cui__edm_paint_carets(cui_ctx *ui, const struct cui__edm_pv *v, float x0, float top, double sy, float ty, float fh, int cb,
                                  float cw, float lh, cui_color c, double phase) {
    float blink = 0.5f + 0.5f * cosf((float)phase * 2.0f * CUI_PI / 1.06f);
    float a = (blink > 0.35f || phase < 0.5 || !cui__caret_blinks(ui, phase)) ? 1.0f : 0.0f;
    if (a <= 0) return;
    for (int i = 0; i < v->nc; i++) {
        float y = top + (float)((double)v->cl[i] * lh - sy);
        float cx = x0 + (float)(v->cc[i] - cb) * cw;
        cui_draw_rect(ui, (cui_rect){ roundf(cx * ui->scale) / ui->scale - 0.75f, y + ty, 1.5f, fh }, CUI_CORNERS(0.75f), cui_alpha(c, a));
    }
}

/* ---- the API ---- */

void cui_code_editor_add_selection(cui_ctx *ui, const char *id, int anchor, int cursor) {
    cui__ed_state *st = cui__ed_get(ui, id);
    cui__edm *m = st->mcur;
    if (!m) return;
    m->add = cui__edm_grow(m->add, &m->add_cap, 2 * (m->add_n + 1), sizeof(int));
    m->add[2 * m->add_n] = anchor; m->add[2 * m->add_n + 1] = cursor;
    m->add_n++;
    ui->animating = true;
}

int cui_code_editor_selections(cui_ctx *ui, const char *id, int *anchors, int *cursors, int cap) {
    cui__ed_state *st = cui__ed_get(ui, id);
    const cui__edm *m = st->mcur;
    int n = m ? m->n : 0, k = 0;
    cui__edm_sel p = { st->ed.anchor, st->ed.cursor, -1, 1 };
    for (int i = 0; i <= n; i++) {   /* the others are sorted: the primary one where it belongs */
        bool before = i == n || cui__edm_cmp(&p, &m->x[i]) < 0;
        if (before && p.prim) {
            if (k < cap) { if (anchors) anchors[k] = p.anchor; if (cursors) cursors[k] = p.cursor; }
            k++; p.prim = 0;
        }
        if (i < n) {
            if (k < cap) { if (anchors) anchors[k] = m->x[i].anchor; if (cursors) cursors[k] = m->x[i].cursor; }
            k++;
        }
    }
    return k;
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC pop_options
#endif
