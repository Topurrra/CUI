/* cui_text_doc: the piece table under random edits, against a plain string doing the same. */
#include <cui/cui.h>
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
    while (cur > 10) { cui_text_doc_undo(d, NULL); cur--; }
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

int main(void) {
    test_random_edits();
    test_undo();
    test_snapshot();
    test_big();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
