/* cui_text_doc: the piece table under random edits, against a plain string doing the same. */
#include <cui/cui.h>
#include "../src/cui_internal.h"   /* cui__doc_edit (merged steps, as an editor's typing on) and cui__doc_edit_parts */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static unsigned g_rng = 12345u;
static unsigned rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

/* the reference: a plain growable string */
typedef struct { char *s; int n, cap; } ref;
static void ref_set(ref *r, const char *t, int n) {
    if (n + 1 > r->cap) { r->cap = n + 1024; r->s = realloc(r->s, (size_t)r->cap); }
    memcpy(r->s, t, (size_t)n); r->n = n; r->s[n] = 0;
}
static void ref_replace(ref *r, int a, int b, const char *t, int n) {
    int nn = r->n - (b - a) + n;
    if (nn + 1 > r->cap) { r->cap = nn * 2 + 1024; r->s = realloc(r->s, (size_t)r->cap); }
    memmove(r->s + a + n, r->s + b, (size_t)(r->n - b + 1));
    memcpy(r->s + a, t, (size_t)n);
    r->n = nn;
}
static ref ref_dup(const ref *r) {   /* a copy, its terminator too */
    ref c = { malloc((size_t)r->cap), r->n, r->cap };
    memcpy(c.s, r->s, (size_t)r->cap);
    return c;
}
static int ref_lines(const ref *r) { int k = 1; for (int i = 0; i < r->n; i++) k += r->s[i] == '\n'; return k; }
static int ref_line_start(const ref *r, int line) {
    if (line <= 0) return 0;
    for (int i = 0, k = 0; i < r->n; i++) if (r->s[i] == '\n' && ++k == line) return i + 1;
    return r->n;
}
static int ref_line_of(const ref *r, int pos) { int k = 0; for (int i = 0; i < pos && i < r->n; i++) k += r->s[i] == '\n'; return k; }
static int ref_maxlen(const ref *r) {
    int m = 0, last = 0;
    for (int i = 0; i <= r->n; i++) if (i == r->n || r->s[i] == '\n') { if (i - last > m) m = i - last; last = i + 1; }
    return m;
}

/* the doc agrees with the reference: bytes, length, lines, lookups, reads and the iterator */
static bool same(const cui_text_doc *d, const ref *r, bool full) {
    bool ok = cui_text_doc_length(d) == r->n;
    int lines = ref_lines(r);
    ok = ok && cui_text_doc_line_count(d) == lines;
    if (!ok) return false;
    if (cui_text_doc_longest_line(d) != ref_maxlen(r)) return false;
    if (full) {
        int n = 0;
        char *c = cui_text_doc_copy(d, 0, -1, &n);
        ok = n == r->n && memcmp(c, r->s, (size_t)n) == 0 && c[n] == 0;
        free(c);
        for (int line = 0, at = 0; line < lines && ok; line++) {
            ok = cui_text_doc_line_start(d, line) == at;
            const char *nl = memchr(r->s + at, '\n', (size_t)(r->n - at));
            int end = nl ? (int)(nl - r->s) : r->n;
            ok = ok && cui_text_doc_line_end(d, line) == end;
            at = end + 1;
        }
        for (int pos = 0, k = 0; pos <= r->n && ok; pos++) {
            ok = cui_text_doc_line_of(d, pos) == k;
            if (pos < r->n) { ok = ok && cui_text_doc_char(d, pos) == (unsigned char)r->s[pos]; k += r->s[pos] == '\n'; }
        }
        ok = ok && cui_text_doc_char(d, r->n) == -1;
        ok = ok && cui_text_doc_longest_line(d) == ref_maxlen(r);
        cui_text_doc_iter it = cui_text_doc_iter_at(d, 0, -1);
        int at = 0;
        while (ok && cui_text_doc_iter_next(&it)) { ok = it.pos == at && it.len > 0 && memcmp(it.p, r->s + at, (size_t)it.len) == 0; at += it.len; }
        ok = ok && at == r->n;
    } else {
        for (int k = 0; k < 8 && ok; k++) {
            int line = (int)(rnd() % (unsigned)lines);
            ok = cui_text_doc_line_start(d, line) == ref_line_start(r, line);
            int pos = r->n ? (int)(rnd() % (unsigned)(r->n + 1)) : 0;
            ok = ok && cui_text_doc_line_of(d, pos) == ref_line_of(r, pos);
        }
    }
    for (int k = 0; k < 4 && ok; k++) {   /* a range read and an iterator over it */
        int a = r->n ? (int)(rnd() % (unsigned)(r->n + 1)) : 0, len = (int)(rnd() % 300);
        int e = a + len > r->n ? r->n : a + len;
        char buf[400];
        ok = cui_text_doc_read(d, a, len, buf) == e - a && memcmp(buf, r->s + a, (size_t)(e - a)) == 0;
        cui_text_doc_iter it = cui_text_doc_iter_at(d, a, e);
        int at = a;
        while (ok && cui_text_doc_iter_next(&it)) { ok = it.pos == at && memcmp(it.p, r->s + at, (size_t)it.len) == 0; at += it.len; }
        ok = ok && at == e;
    }
    return ok;
}

static void random_text(char *out, int n) {
    static const char alpha[] = "abc xyz\n\t{}()0123456789\n";
    for (int i = 0; i < n; i++) out[i] = alpha[rnd() % (sizeof alpha - 1)];
}

static int g_released;
static void release_count(char *p, void *user) { (void)user; g_released++; free(p); }

static void test_random_edits(void) {
    printf("text doc: random edits, lookups, reads, iterators against a string\n");
    ref r = { 0 };
    char start[600];
    random_text(start, 600);
    ref_set(&r, start, 600);
    char *own = malloc(600);
    memcpy(own, start, 600);
    g_released = 0;
    cui_text_doc *d = cui_text_doc_adopt(own, 600, release_count, NULL);
    CHECK(same(d, &r, true));
    bool ok = true;
    for (int step = 0; step < 6000 && ok; step++) {
        int a = r.n ? (int)(rnd() % (unsigned)(r.n + 1)) : 0;
        int kind = (int)(rnd() % 10);
        int b = a;
        if (kind >= 6) b = a + (int)(rnd() % 40);           /* replace or delete */
        if (b > r.n) b = r.n;
        char t[200];
        int n = kind == 9 ? 0 : (int)(rnd() % (kind < 3 ? 3u : 120u));
        if (rnd() % 300 == 0) n = 0;
        random_text(t, n);
        if (rnd() % 50 == 0) { a = b = r.n; }               /* at the very end */
        bool changed = cui_text_doc_replace(d, a, b, t, n);
        ok = changed == (b > a || n > 0);
        ref_replace(&r, a, b, t, n);
        ok = ok && same(d, &r, step % 500 == 0);
    }
    CHECK(ok);
    CHECK(same(d, &r, true));
    CHECK(cui_text_doc_piece_count(d) > 10);
    printf("  %d bytes, %d lines, %d pieces, %zu bytes held\n", r.n, ref_lines(&r), cui_text_doc_piece_count(d), cui_text_doc_memory(d));
    /* typing on joins pieces: replacing at the end of the last insert, one byte at a time */
    int before = cui_text_doc_piece_count(d), at = r.n / 2;
    for (int i = 0; i < 200; i++) { cui_text_doc_replace(d, at + i, at + i, "q", 1); ref_replace(&r, at + i, at + i, "q", 1); }
    CHECK(cui_text_doc_piece_count(d) <= before + 2);
    CHECK(same(d, &r, true));
    /* set: a new text, the old buffer given back */
    CHECK(g_released == 0);
    cui_text_doc_set(d, "one\ntwo\n", -1);
    ref_set(&r, "one\ntwo\n", 8);
    CHECK(g_released == 1 && same(d, &r, true) && cui_text_doc_line_count(d) == 3 && !cui_text_doc_can_undo(d));
    cui_text_doc_set(d, "", 0);
    ref_set(&r, "", 0);
    CHECK(same(d, &r, true) && cui_text_doc_line_count(d) == 1 && cui_text_doc_line_start(d, 1) == 0);
    CHECK(cui_text_doc_replace(d, 0, 0, "x", 1) && cui_text_doc_length(d) == 1);
    CHECK(!cui_text_doc_replace(d, 0, 0, "", 0));
    cui_text_doc_free(d);
    free(r.s);
}

static void test_undo(void) {
    printf("text doc: undo and redo against kept states\n");
    ref r = { 0 };
    ref_set(&r, "hello\nworld\n", 12);
    cui_text_doc *d = cui_text_doc_new(r.s, r.n);
    enum { STATES = 64 };
    char *states[STATES + 1];
    int lens[STATES + 1];
    unsigned ids[STATES + 1];   /* cui_text_doc_state at each */
    ids[0] = cui_text_doc_state(d);
    states[0] = malloc((size_t)r.n + 1); memcpy(states[0], r.s, (size_t)r.n + 1); lens[0] = r.n;
    for (int i = 1; i <= STATES; i++) {
        int a = (int)(rnd() % (unsigned)(r.n + 1)), b = a + (int)(rnd() % 10);
        if (b > r.n) b = r.n;
        char t[64];
        int n = (int)(rnd() % 30);
        if (a == b && n == 0) n = 1;
        random_text(t, n);
        cui_text_doc_replace(d, a, b, t, n);
        ref_replace(&r, a, b, t, n);
        states[i] = malloc((size_t)r.n + 1); memcpy(states[i], r.s, (size_t)r.n + 1); lens[i] = r.n;
        ids[i] = cui_text_doc_state(d);
        CHECK(ids[i] != ids[i - 1]);
    }
    bool ok = true;
    int cur = STATES;
    for (int step = 0; step < 2000 && ok; step++) {   /* walk back and forth through the history */
        bool back = rnd() % 2;
        int caret = -1;
        if (back) { ok = cui_text_doc_undo(d, &caret) == (cur > 0); if (cur > 0) cur--; }
        else { ok = cui_text_doc_redo(d, &caret) == (cur < STATES); if (cur < STATES) cur++; }
        ref_set(&r, states[cur], lens[cur]);
        ok = ok && same(d, &r, step % 100 == 0) && caret >= -1 && caret <= r.n;
        ok = ok && cui_text_doc_can_undo(d) == (cur > 0) && cui_text_doc_can_redo(d) == (cur < STATES);
        ok = ok && cui_text_doc_state(d) == ids[cur];   /* back at a step, the state it had there */
    }
    CHECK(ok);
    /* an edit after undoing drops the redo steps */
    while (cur > 10) { CHECK(cui_text_doc_undo(d, NULL)); cur--; }   /* step 10, from wherever the walk ended */
    while (cur < 10) { CHECK(cui_text_doc_redo(d, NULL)); cur++; }
    CHECK(cui_text_doc_can_redo(d));
    cui_text_doc_replace(d, 0, 0, "new", 3);
    CHECK(!cui_text_doc_can_redo(d));
    for (int i = 0; i <= STATES; i++) CHECK(cui_text_doc_state(d) != ids[i]);
    CHECK(cui_text_doc_undo(d, NULL));
    ref_set(&r, states[10], lens[10]);
    CHECK(same(d, &r, true));
    for (int i = 0; i <= STATES; i++) free(states[i]);
    cui_text_doc_free(d);
    free(r.s);
}

/* The undo history as a model: the texts of steps 0..n (0: before the oldest step kept), the state ids they had, and the
 * step the text is at. An edit after an undo drops the redo steps and is a step of its own, merged or not. */
typedef struct { ref *s; unsigned *id; int n, pos, cap; } hist;

static ref ref_copy(const ref *r) { ref c = { malloc((size_t)r->n + 1), r->n, r->n + 1 }; memcpy(c.s, r->s, (size_t)r->n + 1); return c; }
static void hist_reset(hist *h, const ref *r, unsigned id) {   /* a text with no history */
    for (int i = 0; h->s && i <= h->n; i++) free(h->s[i].s);
    if (!h->cap) { h->cap = 64; h->s = malloc(sizeof *h->s * 64); h->id = malloc(sizeof *h->id * 64); }
    h->n = h->pos = 0;
    h->s[0] = ref_copy(r); h->id[0] = id;
}
static void hist_push(hist *h, const ref *r, unsigned id) {
    for (int i = h->pos + 1; i <= h->n; i++) free(h->s[i].s);
    h->n = h->pos + 1;
    if (h->n + 1 > h->cap) { h->cap *= 2; h->s = realloc(h->s, sizeof *h->s * (size_t)h->cap); h->id = realloc(h->id, sizeof *h->id * (size_t)h->cap); }
    h->s[h->n] = ref_copy(r); h->id[h->n] = id; h->pos = h->n;
}
static void hist_merge(hist *h, const ref *r, unsigned id) {   /* typing on: the last step grows, unless there is none or it was undone */
    if (h->n == 0 || h->pos < h->n) { hist_push(h, r, id); return; }
    free(h->s[h->n].s); h->s[h->n] = ref_copy(r); h->id[h->n] = id;
}
static void hist_trim(hist *h, int max) {   /* the oldest steps go first; with the text at the oldest, the farthest redo steps */
    while (h->n > max) {
        if (h->pos > 0) {
            free(h->s[0].s);
            memmove(h->s, h->s + 1, sizeof *h->s * (size_t)h->n); memmove(h->id, h->id + 1, sizeof *h->id * (size_t)h->n);
            h->n--; h->pos--;
        } else free(h->s[h->n--].s);
    }
}
static void hist_free(hist *h) { for (int i = 0; h->s && i <= h->n; i++) free(h->s[i].s); free(h->s); free(h->id); }
static bool hist_fresh(const hist *h, unsigned id) { for (int i = 0; i <= h->n; i++) if (h->id[i] == id) return false; return true; }
static bool hist_same(const cui_text_doc *d, const hist *h, bool bytes, bool full) {
    const ref *r = &h->s[h->pos];
    bool ok;
    if (bytes) {
        int n = -1;
        char *c = cui_text_doc_copy(d, 0, -1, &n);
        ok = c && n == r->n && memcmp(c, r->s, (size_t)n) == 0 && cui_text_doc_line_count(d) == 1;
        free(c);
    } else ok = same(d, r, full);
    return ok && cui_text_doc_can_undo(d) == (h->pos > 0) && cui_text_doc_can_redo(d) == (h->pos < h->n) && cui_text_doc_state(d) == h->id[h->pos];
}

static void fill(char *t, int n, bool bytes) {
    if (!bytes) { random_text(t, n); return; }
    for (int i = 0; i < n; i++) t[i] = "\n\r\0\xff" "ab\x0a\x0d"[rnd() % 8];
}

static int g_given;   /* shared prepared texts given back */
static void count_back(char *p, void *u) { (void)u; g_given++; free(p); }

/* One seed: undo, redo and every way of editing (replace, merged typing on, parts, prepared and shared prepared texts),
 * the history's length changed, cleared, the text set, checked against the model after each. Returns the first op that
 * went wrong, or -1. */
static int undo_fuzz(unsigned seed, int ops, bool bytes) {
    g_rng = seed;
    enum { BIG = 70000 };
    char *t = malloc(BIG + 8);
    int n0 = rnd() % 4 == 0 ? 0 : (int)(rnd() % 1500);
    fill(t, n0, bytes);
    ref r = { 0 };
    ref_set(&r, t, n0);
    cui_text_doc *d;
    if (bytes) { char *own = malloc((size_t)n0 + 1); memcpy(own, t, (size_t)n0); d = cui_text_doc_adopt_bytes(own, n0, NULL, NULL); }
    else d = cui_text_doc_new(t, n0);
    hist h = { 0 };
    hist_reset(&h, &r, cui_text_doc_state(d));
    int adopted = 0, cursor = 0, hmax = 1000, bad = -1;
    g_given = 0;
    if (!hist_same(d, &h, bytes, true)) bad = 0;
    for (int op = 0; op < ops && bad < 0; op++) {
        int len = r.n, k = (int)(rnd() % 100), caret = -2;
        bool ok = true;
        if (k < 40) {   /* undo or redo */
            bool redo = k >= 22, did = redo ? cui_text_doc_redo(d, &caret) : cui_text_doc_undo(d, &caret);
            ok = did == (redo ? h.pos < h.n : h.pos > 0);
            if (did) { h.pos += redo ? 1 : -1; ok = ok && caret >= 0 && caret <= h.s[h.pos].n; cursor = caret; }
            ref_set(&r, h.s[h.pos].s, h.s[h.pos].n);
        } else if (k < 42) {   /* fewer steps kept (or the default again) */
            hmax = rnd() % 4 == 0 ? 1000 : 1 + (int)(rnd() % 12);
            cui_text_doc_set_history(d, hmax);
            hist_trim(&h, hmax);
        } else if (k < 43) {
            cui_text_doc_clear_history(d);
            hist_reset(&h, &r, h.id[h.pos]);
        } else if (k < 44) {
            int n = (int)(rnd() % 800);
            fill(t, n, bytes);
            cui_text_doc_set(d, t, n);
            ref_set(&r, t, n);
            ok = hist_fresh(&h, cui_text_doc_state(d));
            hist_reset(&h, &r, cui_text_doc_state(d));
            cursor = 0;
        } else {
            int e = (int)(rnd() % (bytes ? 3u : 5u)), a, b, n;   /* 0 replace, 1 merged, 2 parts, 3 prepared, 4 shared prepared */
            if (rnd() % 2 && cursor <= len) {   /* at the caret: typing on, Backspace, Delete (merged steps join) */
                int m = (int)(rnd() % 3);
                a = b = cursor; n = 0;
                if (m == 0) n = 1 + (int)(rnd() % 3);
                else if (m == 1) a = a > 0 ? a - 1 : a;
                else b = b < len ? b + 1 : b;
            } else {
                a = len ? (int)(rnd() % (unsigned)(len + 1)) : 0;
                b = a + (int)(rnd() % 12);
                if (b > len) b = len;
                n = (int)(rnd() % 20);
            }
            if (e >= 3 && rnd() % 5 == 0) { a = 0; b = len; }                         /* the whole text */
            if (e >= 3 && seed % 8 == 1 && rnd() % 30 == 0) n = BIG - (int)(rnd() % 4000);   /* a buffer of its own (some seeds: big texts are slow to check) */
            fill(t, n, bytes);
            char out[160];
            const char *ins = t;
            unsigned id0 = cui_text_doc_state(d);
            bool changed = false;
            if (e == 0) changed = cui_text_doc_replace(d, a, b, t, n);
            else if (e == 1) changed = cui__doc_edit(d, a, b, t, n, NULL, NULL, NULL, a, true, NULL, 0);
            else if (e == 2) {   /* [a, b) becomes runs of itself, in order, between new bytes */
                cui__tpart parts[16];
                char *rb = malloc(64);
                int np = 0, rn = 0, x = a;
                n = 0;
                while (np < 14) {
                    if (rnd() % 2) { int L = (int)(rnd() % 8); fill(rb + rn, L, bytes); memcpy(out + n, rb + rn, (size_t)L); parts[np++] = (cui__tpart){ -rn - 1, L }; rn += L; n += L; }
                    if (x < b) {
                        x += (int)(rnd() % (unsigned)(b - x + 1));
                        int L = (int)(rnd() % (unsigned)(b - x + 1));
                        memcpy(out + n, r.s + x, (size_t)L); parts[np++] = (cui__tpart){ x, L }; x += L; n += L;
                    }
                    if (x >= b && rnd() % 2) break;
                }
                ins = out;
                changed = cui__doc_edit_parts(d, a, b, parts, np, rb, rn, NULL, 0, a);
            } else {
                char *p = malloc((size_t)n + 1);
                memcpy(p, t, (size_t)n);
                if (e == 4) adopted++;
                changed = cui_text_doc_replace_prepared(d, a, b, e == 3 ? cui_text_prepare(p, (size_t)n) : cui_text_prepare_adopt(p, (size_t)n, count_back, NULL));
            }
            ok = changed == (b > a || n > 0);
            if (changed) {
                ref_replace(&r, a, b, ins, n);
                unsigned id = cui_text_doc_state(d);
                ok = ok && id != id0 && hist_fresh(&h, id);
                if (e == 1) hist_merge(&h, &r, id); else hist_push(&h, &r, id);
                hist_trim(&h, hmax);
                cursor = a + n;
            }
        }
        if (!ok || !hist_same(d, &h, bytes, op % 64 == 0)) bad = op + 1;
    }
    cui_text_doc_free(d);
    if (bad < 0 && g_given != adopted) bad = ops + 1;   /* every shared text given back once */
    hist_free(&h);
    free(r.s); free(t);
    return bad;
}

static void test_undo_seeds(void) {
    printf("text doc: undo and redo against a history model, many seeds\n");
    /* the two smallest cases it found: typing on (merged) after an undo is a step of its own ... */
    cui_text_doc *d = cui_text_doc_new("", 0);
    cui_text_doc_replace(d, 0, 0, "a", 1);
    cui_text_doc_replace(d, 1, 1, "b", 1);
    CHECK(cui_text_doc_undo(d, NULL));
    CHECK(cui__doc_edit(d, 1, 1, "c", 1, NULL, NULL, NULL, 1, true, NULL, 0) && !cui_text_doc_can_redo(d));
    CHECK(cui_text_doc_undo(d, NULL) && cui_text_doc_length(d) == 1 && cui_text_doc_char(d, 0) == 'a');
    /* ... and fewer steps kept with all of them undone keeps the text and the steps nearest it */
    cui_text_doc_set(d, "", 0);
    for (int i = 0; i < 3; i++) cui_text_doc_replace(d, i, i, "xyz" + i, 1);
    while (cui_text_doc_undo(d, NULL)) {}
    cui_text_doc_set_history(d, 2);
    CHECK(cui_text_doc_length(d) == 0 && !cui_text_doc_can_undo(d) && cui_text_doc_can_redo(d));
    CHECK(cui_text_doc_redo(d, NULL) && cui_text_doc_redo(d, NULL) && !cui_text_doc_redo(d, NULL));
    char two[3] = { 0 };
    CHECK(cui_text_doc_read(d, 0, 2, two) == 2 && strcmp(two, "xy") == 0);
    cui_text_doc_free(d);
    unsigned keep = g_rng;
    int failed = 0, first = -1, at = 0;
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (unsigned seed = 1; seed <= 240; seed++) {
        int bad = undo_fuzz(seed, 300, seed % 4 == 0);   /* every 4th a bytes document */
        if (bad >= 0 && failed++ == 0) { first = (int)seed; at = bad; }
    }
    if (failed) printf("  %d seeds failed, the first seed %d at op %d\n", failed, first, at);
    printf("  240 seeds, %.0f ms\n", ((double)clock() / CLOCKS_PER_SEC - t0) * 1000);
    CHECK(failed == 0);
    g_rng = keep;
}

static void test_snapshot(void) {
    printf("text doc: snapshots keep their text while the document changes and after it is freed\n");
    cui_text_doc *d = cui_text_doc_new("alpha\nbeta\ngamma\n", -1);
    cui_text_doc_replace(d, 6, 10, "BETA!", 5);
    cui_text_snap *s = cui_text_doc_snapshot(d);
    CHECK(cui_text_snap_length(s) == 18);
    cui_text_doc_replace(d, 0, 5, "", 0);
    cui_text_doc_replace(d, 0, 0, "zzz", 3);
    cui_text_doc_set(d, "other", -1);
    int n = 0;
    char *c = cui_text_snap_copy(s, &n);
    CHECK(n == 18 && strcmp(c, "alpha\nBETA!\ngamma\n") == 0);
    free(c);
    char part[8] = { 0 };
    CHECK(cui_text_snap_read(s, 6, 5, part) == 5 && strcmp(part, "BETA!") == 0);
    int total = 0;
    for (int i = 0; i < cui_text_snap_chunks(s); i++) { int len; cui_text_snap_chunk(s, i, &len); total += len; }
    CHECK(total == 18 && cui_text_snap_chunks(s) >= 3);
    cui_text_doc_free(d);
    c = cui_text_snap_copy(s, &n);   /* the buffers outlive the document */
    CHECK(n == 18 && strcmp(c, "alpha\nBETA!\ngamma\n") == 0);
    free(c);
    cui_text_snap_free(s);
}

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void test_big(void) {
    printf("text doc: a 64 MB text, edits anywhere cost the same\n");
    int size = 64 << 20;
    char *t = malloc((size_t)size);
    for (int i = 0; i < size; i++) t[i] = (i % 61 == 60) ? '\n' : (char)('a' + i % 26);
    double t0 = now();
    cui_text_doc *d = cui_text_doc_adopt(t, size, NULL, NULL);
    double load = now() - t0;
    int lines = cui_text_doc_line_count(d);
    CHECK(lines == size / 61 + 1);
    t0 = now();
    for (int i = 0; i < 20000; i++) {
        int at = (int)(((unsigned long long)rnd() * 4099) % (unsigned)cui_text_doc_length(d));
        if (i % 3 == 2) cui_text_doc_replace(d, at, at + 3, "", 0);
        else cui_text_doc_replace(d, at, at, i % 7 ? "x" : "\n", 1);
    }
    double edits = now() - t0;
    t0 = now();
    long long sum = 0;
    for (int i = 0; i < 200000; i++) sum += cui_text_doc_line_start(d, (int)(rnd() % (unsigned)lines));
    double look = now() - t0;
    CHECK(sum > 0);
    printf("  adopt %.1f ms, %d edits %.2f us each, line lookup %.2f us, %d pieces, %.1f MB held for %.1f MB of text\n", load * 1000, 20000,
           edits / 20000 * 1e6, look / 200000 * 1e6, cui_text_doc_piece_count(d), cui_text_doc_memory(d) / 1048576.0, cui_text_doc_length(d) / 1048576.0);
    CHECK(edits / 20000 < 50e-6);
    /* a snapshot for a worker: O(pieces), no copy; the copy is made where the snapshot is read */
    t0 = now();
    cui_text_snap *s = cui_text_doc_snapshot(d);
    double snap = now() - t0;
    t0 = now();
    int n = 0;
    char *flat = cui_text_snap_copy(s, &n);
    double copy = now() - t0;
    CHECK(flat && n == cui_text_doc_length(d));
    free(flat);
    cui_text_snap_free(s);
    printf("  snapshot %.3f ms (%d pieces), its copy in one piece %.1f ms\n", snap * 1000, cui_text_doc_piece_count(d), copy * 1000);
    cui_text_doc_free(d);
}

/* a prepared text (cui_text_prepare, made on a worker): the same text as cui_text_doc_replace would make (without its
 * carriage returns), as one undo step, and undo and redo give back the longest line without measuring it */
static void test_prepared(void) {
    printf("text doc: prepared texts against replace\n");
    ref r = { 0 };
    char base[3000];
    random_text(base, (int)sizeof base);
    cui_text_doc *d = cui_text_doc_new(base, (int)sizeof base);
    ref_set(&r, base, (int)sizeof base);
    bool ok = true;
    for (int round = 0; round < 300 && ok; round++) {
        int n = round % 7 == 0 ? 70000 + (int)(rnd() % 30000) : (int)(rnd() % 400);   /* some big enough for a buffer of their own */
        char *t = malloc((size_t)n + 1), *plain = malloc((size_t)n + 1);
        random_text(t, n);
        for (int i = 0; i < n; i += 1 + (int)(rnd() % 50)) if (rnd() % 4 == 0) t[i] = '\r';
        int m = 0;
        for (int i = 0; i < n; i++) if (t[i] != '\r') plain[m++] = t[i];
        cui_text_prepared *p = cui_text_prepare(t, (size_t)n);
        ok = p && cui_text_prepared_length(p) == m && memcmp(cui_text_prepared_text(p), plain, (size_t)m) == 0;
        CHECK(ok);
        int a, b;
        if (round % 5 == 0) { a = 0; b = r.n; }   /* the whole text: its longest line comes with it */
        else { a = r.n ? (int)(rnd() % (unsigned)(r.n + 1)) : 0; b = a + (int)(rnd() % 3000); if (b > r.n) b = r.n; }
        unsigned st0 = cui_text_doc_state(d);
        int max0 = cui_text_doc_longest_line(d);
        bool changed = cui_text_doc_replace_prepared(d, a, b, p);
        ref_replace(&r, a, b, plain, m);
        ok = ok && changed == (m > 0 || b > a) && same(d, &r, round % 20 == 0);
        CHECK(ok);
        if (changed && round % 3 == 0) {   /* one step back and forth */
            ref save = ref_dup(&r);
            CHECK(cui_text_doc_undo(d, NULL));
            ok = ok && cui_text_doc_state(d) == st0 && cui_text_doc_longest_line(d) == max0;
            CHECK(ok);
            CHECK(cui_text_doc_redo(d, NULL));
            ok = ok && same(d, &save, false);
            CHECK(ok);
            free(save.s);
        }
        free(plain);
    }
    /* many steps undone (the longest line right at each) and redone */
    ref past = ref_dup(&r);
    for (int i = 0; i < 40 && ok; i++) {
        ok = cui_text_doc_undo(d, NULL);
        int n = 0;
        char *c = cui_text_doc_copy(d, 0, -1, &n);
        ref now_ = { c, n, n + 1 };
        ok = ok && cui_text_doc_longest_line(d) == ref_maxlen(&now_);
        free(c);
    }
    CHECK(ok);
    for (int i = 0; i < 40 && ok; i++) ok = cui_text_doc_redo(d, NULL);
    CHECK(ok && same(d, &past, true));
    CHECK(!cui_text_doc_replace_prepared(d, 0, 0, NULL));
    CHECK(!cui_text_doc_replace_prepared(NULL, 0, 0, cui_text_prepare(malloc(4), 0)));   /* freed, not leaked */
    cui_text_doc_free(d);
    free(r.s); free(past.s);
}

/* a shared prepared text (cui_text_prepare_adopt): read where it lies with its carriage returns, through undo and redo;
 * given back once, when the document and its snapshots have let go; a small one copied and given back at once; every
 * path that does not use it gives it back too */
typedef struct { int calls; const char *last; } give_back;
static void gb_release(char *p, void *u) { give_back *g = (give_back *)u; g->calls++; g->last = p; free(p); }

static void test_prepared_adopt(void) {
    printf("text doc: shared prepared texts\n");
    give_back g = { 0 };
    int n = 200000;
    char *t = malloc((size_t)n);
    random_text(t, n);
    for (int i = 5; i < n; i += 37) t[i] = '\r';
    ref r = { 0 };
    ref_set(&r, t, n);
    cui_text_doc *d = cui_text_doc_new("old text\nhere", -1);
    cui_text_prepared *p = cui_text_prepare_adopt(t, (size_t)n, gb_release, &g);
    CHECK(p && cui_text_prepared_length(p) == n && cui_text_prepared_text(p) == t);
    CHECK(cui_text_doc_replace_prepared(d, 0, cui_text_doc_length(d), p));
    CHECK(g.calls == 0 && same(d, &r, false));
    cui_text_doc_iter it = cui_text_doc_iter_at(d, 0, -1);
    CHECK(cui_text_doc_iter_next(&it) && it.p == t && it.len == n);   /* in place: no copy */
    CHECK(cui_text_doc_undo(d, NULL) && cui_text_doc_length(d) == 13 && g.calls == 0);
    CHECK(cui_text_doc_redo(d, NULL) && g.calls == 0 && same(d, &r, true));
    cui_text_snap *s = cui_text_doc_snapshot(d);
    cui_text_doc_free(d);
    CHECK(g.calls == 0);   /* the snapshot still reads it */
    char head[16];
    CHECK(cui_text_snap_read(s, 0, 16, head) == 16 && memcmp(head, r.s, 16) == 0);
    cui_text_snap_free(s);
    CHECK(g.calls == 1 && g.last == t);

    /* a small text: copied in, given back at once; the bytes stay right */
    g.calls = 0;
    char *small = malloc(6);
    memcpy(small, "a\r\nb\r\n", 6);
    d = cui_text_doc_new("x", -1);
    CHECK(cui_text_doc_replace_prepared(d, 0, 1, cui_text_prepare_adopt(small, 6, gb_release, &g)));
    CHECK(g.calls == 1 && cui_text_doc_length(d) == 6 && cui_text_doc_char(d, 1) == '\r' && cui_text_doc_line_count(d) == 3);
    /* not used: freed, refused, a removal, no change; a NULL release frees it */
    cui_text_prepared_free(cui_text_prepare_adopt(malloc(10), 10, gb_release, &g));
    CHECK(g.calls == 2);
    CHECK(!cui_text_doc_replace_prepared(NULL, 0, 0, cui_text_prepare_adopt(malloc(10), 10, gb_release, &g)));
    CHECK(g.calls == 3);
    CHECK(cui_text_doc_replace_prepared(d, 0, 3, cui_text_prepare_adopt(malloc(1), 0, gb_release, &g)));
    CHECK(g.calls == 4 && cui_text_doc_length(d) == 3);
    CHECK(!cui_text_doc_replace_prepared(d, 1, 1, cui_text_prepare_adopt(malloc(1), 0, gb_release, &g)));
    CHECK(g.calls == 5);
    CHECK(cui_text_doc_replace_prepared(d, 0, 0, cui_text_prepare_adopt(calloc(70000, 1), 70000, NULL, NULL)));
    CHECK(cui_text_doc_length(d) == 70003);
    CHECK(cui_text_prepare_adopt(NULL, 5, gb_release, &g) == NULL && g.calls == 5);
    cui_text_doc_free(d);
    free(r.s);
}

/* cui_text_doc_adopt_bytes: every byte kept as given (0x0A, 0x0D, 0x00 alike), no line index however many line
 * breaks, and edits, undo and snapshots against the reference string */
static void test_bytes(void) {
    printf("bytes documents (no line index)\n");
    int n = 4 << 20;
    char *nl = malloc((size_t)n), *rb = malloc((size_t)n);
    memset(nl, '\n', (size_t)n);
    cui_text_doc *lines = cui_text_doc_new(nl, n);
    cui_text_doc *bytes = cui_text_doc_adopt_bytes(nl, n, NULL, NULL);   /* takes nl */
    CHECK(cui_text_doc_line_count(lines) == n + 1 && cui_text_doc_line_count(bytes) == 1);
    CHECK(cui_text_doc_memory(bytes) < (size_t)n + 64 * 1024);           /* the bytes, not 4 more per line break */
    CHECK(cui_text_doc_memory(lines) > (size_t)n * 4);
    cui_text_doc_free(lines);
    for (int i = 0; i < n; i++) rb[i] = (char)(rnd() & 0xFF);
    ref r = { 0 };
    ref_set(&r, rb, n);
    cui_text_doc_set(bytes, rb, n);
    free(rb);
    for (int step = 0; step < 900; step++) {   /* within the 1000 undo steps kept */
        int len = r.n, a = len ? (int)(rnd() % (unsigned)(len + 1)) : 0, b = a + (int)(rnd() % 9);
        if (b > len) b = len;
        char t[8];
        int k = (int)(rnd() % 8);
        for (int i = 0; i < k; i++) t[i] = (char)"\n\r\0\xff" "ab\x0a\x0d"[rnd() % 8];
        if (a == b && k == 0) continue;
        CHECK(cui_text_doc_replace(bytes, a, b, t, k));
        ref_replace(&r, a, b, t, k);
    }
    CHECK(cui_text_doc_length(bytes) == r.n && cui_text_doc_line_count(bytes) == 1);
    char *c = cui_text_doc_copy(bytes, 0, -1, NULL);
    CHECK(c && memcmp(c, r.s, (size_t)r.n) == 0);
    free(c);
    cui_text_snap *snap = cui_text_doc_snapshot(bytes);
    for (int i = 0; i < 3000 && cui_text_doc_undo(bytes, NULL); i++) {}
    CHECK(cui_text_doc_length(bytes) == n);
    int total = 0, same_bytes = 1;
    for (int i = 0; i < cui_text_snap_chunks(snap); i++) {
        int cl = 0;
        const char *p = cui_text_snap_chunk(snap, i, &cl);
        if (cl > 0 && memcmp(p, r.s + total, (size_t)cl)) same_bytes = 0;
        total += cl;
    }
    CHECK(total == r.n && same_bytes);   /* the snapshot kept the edited bytes while the doc was undone */
    cui_text_snap_free(snap);
    cui_text_doc_free(bytes);
    free(r.s);
}

int main(void) {
    test_random_edits();
    test_undo();
    test_undo_seeds();
    test_snapshot();
    test_big();
    test_prepared();
    test_prepared_adopt();
    test_bytes();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
