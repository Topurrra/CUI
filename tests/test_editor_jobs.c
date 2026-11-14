/* The code editor's work on another thread (src/cui_editor.c, "work on another thread"): the line
 * states below an edit far above the view, and Replace all on a document. Each check drives an editor
 * whose work goes to jobs (the limits lowered so that even small texts use them) next to one that does
 * it all at once, with the same input, and compares what they hold, report and draw once the jobs are
 * done; edits made while a job runs; and editors and documents freed while their jobs run. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static void nap_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void nap_ms(int ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static cui_sw_renderer *g_sw;
static unsigned g_rng = 4242u;
static unsigned rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

/* the limits: everything on jobs (and a buffer copied a few kilobytes a job), or all at once */
static void jobs_on(void) { cui__ed_sync_bytes = 1; cui__ed_rall_bytes = 0; cui__ed_copy_bytes = 3000; }
static void jobs_off(void) { cui__ed_sync_bytes = 1 << 30; cui__ed_rall_bytes = 1 << 30; cui__ed_copy_bytes = 1 << 30; }

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

typedef struct {
    char *buf; size_t cap;      /* a buffer, or */
    cui_text_doc *doc;          /* a document */
    const char *lang;
    bool wrap;
    cui_code_editor_result r;
} ej;

static void build_ej(cui_ctx *ui, void *u) {
    ej *c = u;
    cui_column(ui, .pad = 20) {
        if (c->doc) c->r = cui_code_editor(ui, "ed", NULL, 0, .doc = c->doc, .lang = c->lang, .wrap = c->wrap, .height = 300, .width = CUI_PX(600));
        else c->r = cui_code_editor(ui, "ed", c->buf, c->cap, .lang = c->lang, .wrap = c->wrap, .height = 300, .width = CUI_PX(600), .revision = 1);
    }
}
static void frame1(cui_ctx *ui, ej *c) { cui_begin_frame(ui, 1.0f / 60.0f); build_ej(ui, c); cui_end_frame(ui); }

/* one frame of each, A with its work on jobs and B with none */
static void frame2(cui_ctx *ua, ej *A, cui_ctx *ub, ej *B) {
    jobs_on(); frame1(ua, A);
    jobs_off(); frame1(ub, B);
}
/* frames of both until A has nothing at work (B runs as many, so both are at the same time); false
 * when it never got there */
static bool settle2(cui_ctx *ua, ej *A, cui_ctx *ub, ej *B, int *seen) {
    for (int i = 0; i < 4000; i++) {
        frame2(ua, A, ub, B);
        if (A->r.working && seen) (*seen)++;
        if (!A->r.working) return true;
        nap_ms(1);
    }
    return false;
}

/* the frame as drawn: positions and colors of every vertex */
static uint64_t draw_hash(cui_ctx *ui) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < dd->vertex_count; i++) {
        const cui_vertex *v = &dd->vertices[i];
        int32_t q[4] = { (int32_t)lroundf(v->x * 64), (int32_t)lroundf(v->y * 64), 0, 0 };
        memcpy(&q[2], &v->color, 4);
        memcpy(&q[3], &v->border_color, 4);
        for (int k = 0; k < 4; k++) h = (h ^ (uint32_t)q[k]) * 1099511628211ull;
    }
    return h ^ (uint64_t)dd->vertex_count;
}

static char *text_of(const ej *c, int *n) {
    if (c->doc) return cui_text_doc_copy(c->doc, 0, -1, n);
    *n = (int)strlen(c->buf);
    char *t = malloc((size_t)*n + 1);
    memcpy(t, c->buf, (size_t)*n + 1);
    return t;
}

/* NULL when both hold, report and draw the same; else what differs */
static const char *differs(cui_ctx *ua, const ej *A, cui_ctx *ub, const ej *B) {
    int na, nb;
    char *ta = text_of(A, &na), *tb = text_of(B, &nb);
    bool same = na == nb && memcmp(ta, tb, (size_t)na) == 0;
    free(ta); free(tb);
    if (!same) return "text";
    const cui_code_editor_result *x = &A->r, *y = &B->r;
    if (x->cursor != y->cursor || x->anchor != y->anchor || x->line_count != y->line_count || x->length != y->length) return "caret or size";
    if (x->find_matches != y->find_matches || x->find_index != y->find_index) return "find";
    if (draw_hash(ua) != draw_hash(ub)) return "drawing";
    return NULL;
}

/* text with what carries a state from line to line: block comments, fences, front matter, HTML
 * comments, YAML block scalars, CDATA; and some long runs of lines without any */
static char *random_text(int lines, bool sparse) {
    static const char *frag[] = { "int x = f(a, b); // tile", "/* a comment", "still in it */ y++;", "```", "```c", "# heading tile", "---",
                                  "  - item [ ] task", "\tif (q) { r(); } /* one line */", "<!-- note", "end -->", "key: |", "  block: tile",
                                  "<![CDATA[ x", "]]>", "\"a /* not */ string\"", "tile tile TILE tiled", "", "{", "}", "x = '/*';",
                                  "a line of words (with [brackets] {and} more words) tile" };
    size_t cap = (size_t)lines * 64 + 16, n = 0;
    char *s = malloc(cap);
    for (int i = 0; i < lines; i++) {
        const char *f = sparse && (i / 300) % 2 ? "plain words with nothing in them at all" : frag[rnd() % (sizeof frag / sizeof frag[0])];
        n += (size_t)snprintf(s + n, cap - n, "%s\n", f);
    }
    s[n] = 0;
    return s;
}

static void ej_make(ej *c, const char *text, bool doc, const char *lang) {
    memset(c, 0, sizeof *c);
    c->lang = lang;
    if (doc) c->doc = cui_text_doc_new(text, -1);
    else {
        c->cap = strlen(text) + (1 << 20);
        c->buf = calloc(1, c->cap);
        memcpy(c->buf, text, strlen(text));
    }
}
static void ej_free(ej *c) { cui_text_doc_free(c->doc); free(c->buf); }

/* The line states: random texts in languages with states, on documents and buffers, viewed at random
 * lines (the end of a fresh text first) and edited at random places, some edits made while the jobs
 * still run. After the jobs, both editors draw the same. */
static void test_states(void) {
    printf("line states on jobs: the same as found at once\n");
    static const char *langs[] = { "c", "markdown", "yaml", "xml", "html", "js", "css", "java" };
    static const char *ins[] = { "/*", "*/", "```\n", "<!--", "-->", "x\n", "---\n", "\n", "key: |\n", "" };
    int compared = 0, working = 0;
    for (int mode = 0; mode < 16; mode++) {
        bool doc = mode % 2 == 0;
        const char *lang = langs[(mode / 2) % 8];
        char *text = random_text(2500 + (int)(rnd() % 2000), false);
        ej A, B;
        ej_make(&A, text, doc, lang); ej_make(&B, text, doc, lang);
        A.wrap = B.wrap = mode % 5 == 3;
        cui_ctx *ua = new_ctx(), *ub = new_ctx();
        for (int i = 0; i < 3; i++) frame2(ua, &A, ub, &B);
        const char *why = NULL;
        for (int step = 0; step < 24 && !why; step++) {
            int nl = B.r.line_count, len = B.r.length;
            unsigned r = rnd();
            cui_ctx *both[2] = { ua, ub };
            for (int k = 0; k < 2; k++) {
                if (step == 0) cui_code_editor_goto(both[k], "ed", nl, 1);   /* a fresh text: to the end */
                else if (r % 3 == 0) cui_code_editor_goto(both[k], "ed", 1 + (int)((r >> 4) % (unsigned)nl), 1);
                else {
                    int a = len ? (int)((r >> 4) % (unsigned)len) : 0, b = a + (int)((r >> 20) % 3 == 0 ? (r >> 8) % 40 : 0);
                    if (b > len) b = len;
                    cui_code_editor_edit(both[k], "ed", a, b, ins[(r >> 12) % 10], -1, (r >> 14) % 2 ? -1 : (int)((r >> 2) % (unsigned)(len + 1)));
                }
            }
            frame2(ua, &A, ub, &B);
            if ((r >> 16) % 3 == 0) continue;   /* the next step while the jobs still run */
            if (!settle2(ua, &A, ub, &B, &working)) { why = "never done"; break; }
            why = differs(ua, &A, ub, &B);
            compared++;
        }
        if (why) printf("  mode %d (%s, %s%s): %s\n", mode, lang, doc ? "document" : "buffer", A.wrap ? ", wrap" : "", why);
        CHECK(why == NULL);
        cui_destroy(ua); cui_destroy(ub);
        ej_free(&A); ej_free(&B);
        free(text);
    }
    printf("  %d views compared, %d frames with a job at work\n", compared, working);
    CHECK(working > 0);
}

/* Replace all: the find bar's Replace all on a document worked out on a job against the same done at
 * once: the same text, caret, splice and undo, for plain text, case, whole word and patterns with
 * groups, on texts in one piece and in many (edits made before), with matches close together and far
 * apart (text between them kept where it lies). */
typedef struct { const char *q, *with; int flags; } rq;
static void test_replace(void) {
    printf("Replace all on a job: the same as done at once\n");
    static const rq qs[] = { { "tile", "TILE!", 0 }, { "TILE", "x", CUI_FIND_CASE }, { "tile", "", CUI_FIND_WORD },
                             { "(\\w+)\\(", "<$1>(", CUI_FIND_REGEX }, { "t(i)le", "$1$$$0", CUI_FIND_REGEX | CUI_FIND_CASE },
                             { "/*", "/**/", 0 }, { "e", "ee", 0 }, { "nothing here", "x", 0 } };
    int compared = 0, jobs = 0;
    for (int mode = 0; mode < 24; mode++) {
        const rq *q = &qs[mode % 8];
        bool sparse = mode % 3 == 1, pieces = mode % 4 >= 2;
        char *text = random_text(sparse ? 6000 : 1500 + (int)(rnd() % 1500), sparse);
        ej A, B;
        ej_make(&A, text, true, "c"); ej_make(&B, text, true, "c");
        if (pieces)   /* the document in many pieces, some lines across them */
            for (int i = 0; i < 60; i++) {
                int len = cui_text_doc_length(A.doc), a = (int)(rnd() % (unsigned)len);
                const char *t = i % 3 ? "tile" : "x\n";
                cui_text_doc_replace(A.doc, a, a + (i % 2), t, -1); cui_text_doc_replace(B.doc, a, a + (i % 2), t, -1);
            }
        cui_ctx *ua = new_ctx(), *ub = new_ctx();
        for (int i = 0; i < 2; i++) frame2(ua, &A, ub, &B);
        cui_code_editor_find_ex(ua, "ed", q->q, q->flags); cui_code_editor_find_ex(ub, "ed", q->q, q->flags);
        cui_code_editor_replace(ua, "ed", NULL, q->with); cui_code_editor_replace(ub, "ed", NULL, q->with);
        for (int i = 0; i < 60; i++) frame2(ua, &A, ub, &B);   /* the count done */
        int before = cui_text_doc_length(B.doc);
        unsigned rev = cui_text_doc_revision(A.doc);
        cui_input_key(ua, CUI_KEY_ENTER, true, CUI_MOD_PRIMARY | CUI_MOD_ALT); cui_input_key(ua, CUI_KEY_ENTER, false, CUI_MOD_PRIMARY | CUI_MOD_ALT);
        cui_input_key(ub, CUI_KEY_ENTER, true, CUI_MOD_PRIMARY | CUI_MOD_ALT); cui_input_key(ub, CUI_KEY_ENTER, false, CUI_MOD_PRIMARY | CUI_MOD_ALT);
        frame2(ua, &A, ub, &B);
        cui_code_editor_result rb = B.r, ra = A.r;   /* B replaced in that frame; A in the frame its job is taken */
        if (A.r.working) jobs++;
        for (int i = 0; i < 4000 && !ra.changed && A.r.working; i++) {   /* B runs as many frames: both at the same time */
            nap_ms(1);
            frame2(ua, &A, ub, &B);
            if (A.r.changed) ra = A.r;
        }
        CHECK((cui_text_doc_revision(A.doc) != rev) == rb.changed);   /* (A may still find line states: c) */
        const char *why = NULL;
        int na, nb;
        char *ta = text_of(&A, &na), *tb = text_of(&B, &nb);
        if (na != nb || memcmp(ta, tb, (size_t)na)) why = "text";
        else if (ra.changed != rb.changed || ra.cursor != rb.cursor || ra.anchor != rb.anchor) why = "caret";
        else if (ra.splice_start != rb.splice_start || ra.splice_removed != rb.splice_removed || ra.splice_inserted != rb.splice_inserted) why = "splice";
        free(ta); free(tb);
        /* one undo step back to the text before, and redo */
        if (!why && rb.changed) {
            cui_text_doc_undo(A.doc, NULL); cui_text_doc_undo(B.doc, NULL);
            ta = text_of(&A, &na); tb = text_of(&B, &nb);
            if (na != before || nb != before || memcmp(ta, tb, (size_t)na)) why = "undo";
            free(ta); free(tb);
            cui_text_doc_redo(A.doc, NULL); cui_text_doc_redo(B.doc, NULL);
            ta = text_of(&A, &na); tb = text_of(&B, &nb);
            if (!why && (na != nb || memcmp(ta, tb, (size_t)na))) why = "redo";
            free(ta); free(tb);
        }
        if (!why) {   /* and both editors show the same after it */
            for (int i = 0; i < 3; i++) frame2(ua, &A, ub, &B);
            if (draw_hash(ua) != draw_hash(ub) || A.r.line_count != B.r.line_count) why = "drawing after";
        }
        if (why) printf("  mode %d (\"%s\" -> \"%s\", flags %d%s%s): %s\n", mode, q->q, q->with, q->flags, sparse ? ", far apart" : "", pieces ? ", pieces" : "", why);
        CHECK(why == NULL);
        compared++;
        cui_destroy(ua); cui_destroy(ub);
        ej_free(&A); ej_free(&B);
        free(text);
    }
    printf("  %d replacements compared (%d waited for a job)\n", compared, jobs);
    CHECK(jobs > 0);
}

/* a big text with many matches, so that a job takes a while */
static char *big_text(int mb, int *n) {
    size_t cap = (size_t)mb << 20, k = 0;
    char *s = malloc(cap + 256);
    for (int i = 0; k + 128 < cap; i++) k += (size_t)snprintf(s + k, 128, i % 37 == 0 ? "/* tile %d\n" : i % 41 == 0 ? "*/ tile %d\n" : "tile x y z %d tile\n", i);
    s[k] = 0;
    *n = (int)k;
    return s;
}

/* Edits while a job runs: an edit to the document drops Replace all (nothing is replaced), and so does
 * closing the find bar; typing far below the line states being found keeps what they found above it. */
static void test_stale(void) {
    printf("jobs and edits made while they run\n");
    jobs_off();
    cui__ed_rall_bytes = 0;
    int n;
    char *t = big_text(12, &n);
    for (int how = 0; how < 2; how++) {
        ej A;
        memset(&A, 0, sizeof A);
        A.lang = "text";
        A.doc = cui_text_doc_new(t, n);
        cui_ctx *ui = new_ctx();
        for (int i = 0; i < 2; i++) frame1(ui, &A);
        cui_code_editor_replace(ui, "ed", "tile", "TILE");
        for (int i = 0; i < 3; i++) frame1(ui, &A);
        cui_input_key(ui, CUI_KEY_ENTER, true, CUI_MOD_PRIMARY | CUI_MOD_ALT); cui_input_key(ui, CUI_KEY_ENTER, false, CUI_MOD_PRIMARY | CUI_MOD_ALT);
        frame1(ui, &A);
        CHECK(A.r.working);   /* the job runs: 12 MB of matches take longer than a frame */
        if (how == 0) CHECK(cui_text_doc_replace(A.doc, 0, 0, "#", 1));   /* the app edits the text */
        else { cui_input_key(ui, CUI_KEY_ESCAPE, true, 0); cui_input_key(ui, CUI_KEY_ESCAPE, false, 0); }   /* the find bar closes */
        for (int i = 0; i < 3; i++) frame1(ui, &A);
        CHECK(!A.r.working);   /* dropped at once */
        for (int i = 0; i < 50; i++) { nap_ms(2); frame1(ui, &A); }
        int m;
        char *now = cui_text_doc_copy(A.doc, 0, -1, &m);
        CHECK(m == n + (how == 0) && memcmp(now + (how == 0), t, (size_t)n) == 0);   /* nothing replaced */
        CHECK(cui_text_doc_can_undo(A.doc) == (how == 0));
        free(now);
        cui_destroy(ui);
        cui_text_doc_free(A.doc);
    }
    /* line states: to the end of a fresh text, then a line typed in view while the job runs; the job's
     * states above the line hold, and once all is done the view is drawn as without jobs */
    ej A, B;
    char *md = big_text(4, &n);
    ej_make(&A, md, true, "c"); ej_make(&B, md, true, "c");
    cui_ctx *ua = new_ctx(), *ub = new_ctx();
    for (int i = 0; i < 2; i++) frame2(ua, &A, ub, &B);
    int nl = B.r.line_count;
    cui_code_editor_goto(ua, "ed", nl - 5, 1); cui_code_editor_goto(ub, "ed", nl - 5, 1);
    frame2(ua, &A, ub, &B);
    bool ran = A.r.working;
    for (int i = 0; i < 5; i++) {
        cui_code_editor_edit(ua, "ed", A.r.cursor, A.r.cursor, "x /* y", -1, -1); cui_code_editor_edit(ub, "ed", B.r.cursor, B.r.cursor, "x /* y", -1, -1);
        frame2(ua, &A, ub, &B);
    }
    CHECK(settle2(ua, &A, ub, &B, NULL));
    const char *why = differs(ua, &A, ub, &B);
    if (why) printf("  typing at the end while the states are found: %s\n", why);
    CHECK(ran && why == NULL);
    cui_destroy(ua); cui_destroy(ub);
    ej_free(&A); ej_free(&B);
    /* A comment opened on the first line changes the state of every line below (no line closes it),
     * so the job goes from the top to the view; while it runs, the app closes the comment halfway
     * down. What the job found below that line no longer holds: the view at the end is not in a
     * comment. */
    size_t pc = 300000 * 16 + 16, pn = 0;
    char *plain = malloc(pc);
    for (int i = 0; i < 300000; i++) pn += (size_t)snprintf(plain + pn, pc - pn, "int x%d; // a\n", i % 10);
    ej_make(&A, plain, true, "c"); ej_make(&B, plain, true, "c");
    ua = new_ctx(); ub = new_ctx();
    for (int i = 0; i < 2; i++) frame2(ua, &A, ub, &B);
    nl = B.r.line_count;
    cui_code_editor_goto(ua, "ed", nl - 5, 1); cui_code_editor_goto(ub, "ed", nl - 5, 1);
    CHECK(settle2(ua, &A, ub, &B, NULL));
    cui_text_doc_replace(A.doc, 0, 0, "/*", 2); cui_text_doc_replace(B.doc, 0, 0, "/*", 2);
    frame2(ua, &A, ub, &B);
    ran = A.r.working;
    int mid = cui_text_doc_line_start(A.doc, nl / 2);
    cui_text_doc_replace(A.doc, mid, mid, "*/", 2); cui_text_doc_replace(B.doc, mid, mid, "*/", 2);
    CHECK(settle2(ua, &A, ub, &B, NULL));
    why = differs(ua, &A, ub, &B);
    if (why) printf("  a comment closed while the states are found: %s\n", why);
    CHECK(ran && why == NULL);
    cui_destroy(ua); cui_destroy(ub);
    ej_free(&A); ej_free(&B);
    free(plain);
    free(md); free(t);
    jobs_off();
}

/* An editor freed while its jobs run (its context destroyed), and then its document: the jobs stop and
 * free what they hold; nothing reads freed memory (run it under a sanitizer to see that). */
static void test_freed(void) {
    printf("editors and documents freed while their jobs run\n");
    int n;
    char *t = big_text(24, &n);
    for (int how = 0; how < 3; how++) {
        jobs_off();
        cui__ed_sync_bytes = 1;
        cui__ed_rall_bytes = 0;
        cui__ed_copy_bytes = 4 << 20;
        ej A;
        memset(&A, 0, sizeof A);
        A.lang = "c";
        if (how < 2) A.doc = cui_text_doc_new(t, n);
        else { A.cap = (size_t)n + 1024; A.buf = calloc(1, A.cap); memcpy(A.buf, t, (size_t)n); }
        cui_ctx *ui = new_ctx();
        for (int i = 0; i < 2; i++) frame1(ui, &A);
        cui_code_editor_goto(ui, "ed", A.r.line_count, 1);   /* the states down to the end: a job */
        frame1(ui, &A);
        bool ran = A.r.working;
        if (how == 1) {   /* and Replace all */
            cui_code_editor_replace(ui, "ed", "tile", "x");
            frame1(ui, &A);
            cui_input_key(ui, CUI_KEY_ENTER, true, CUI_MOD_PRIMARY | CUI_MOD_ALT); cui_input_key(ui, CUI_KEY_ENTER, false, CUI_MOD_PRIMARY | CUI_MOD_ALT);
            frame1(ui, &A);
            ran = ran && A.r.working;
        }
        CHECK(ran);
        cui_destroy(ui);              /* the editor goes while the jobs run */
        cui_text_doc_free(A.doc);     /* and the document */
        free(A.buf);
        nap_ms(how == 0 ? 0 : 30);
    }
    nap_ms(400);   /* the jobs see that they may stop, and free what they hold */
    /* the library still works */
    ej B;
    ej_make(&B, "int x; /* y */\n", true, "c");
    cui_ctx *ui = new_ctx();
    for (int i = 0; i < 3; i++) frame1(ui, &B);
    CHECK(B.r.length == 15 && !B.r.working);
    cui_destroy(ui);
    ej_free(&B);
    free(t);
    jobs_off();
}

int main(void) {
    g_sw = cui_sw_create();
    int sync_bytes = cui__ed_sync_bytes, rall_bytes = cui__ed_rall_bytes, copy_bytes = cui__ed_copy_bytes;
    test_states();
    test_replace();
    test_stale();
    test_freed();
    cui__ed_sync_bytes = sync_bytes; cui__ed_rall_bytes = rall_bytes; cui__ed_copy_bytes = copy_bytes;
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
