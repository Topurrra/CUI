#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Measures the code editor over big generated texts: the first frame, idle
 * frames, scrolling in the middle and at the end, typing near the top and
 * near the end, and memory. Headless, like the tests.
 *
 *     cui_bench_editor [MB ...]          default: 1 10 100
 *     cui_bench_editor --quick           1 and 10 MB only
 *     cui_bench_editor --replace [A B]   replace the whole text of A MB with B MB (20, 50)
 *     cui_bench_editor --shared [MB ...] two editors on one buffer, each told of the other's
 *                                        edits with cui_code_editor_splice (default 10 100)
 *     cui_bench_editor --doc [MB ...]    the same as the default, on a cui_text_doc (`.doc`)
 *     cui_bench_editor --shared-doc [MB ...]   two editors on one cui_text_doc
 *     cui_bench_editor --replace-doc [A B]     --replace on a cui_text_doc
 *     cui_bench_editor --compare [MB ...]      buffer (with .revision) and document side by side:
 *                                        open, typing near the top, middle and end, Enter,
 *                                        scrolling, replace all and its undo, memory (default 1 10 100)
 */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
static double now(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart; }
static double mem_mb(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return (double)m.PrivateUsage / (1024.0 * 1024.0);
}
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec * 1e-9; }
static double mem_mb(void) {
    FILE *f = fopen("/proc/self/statm", "r");
    long pages = 0, rss = 0;
    if (f) { if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0; fclose(f); }
    return (double)rss * 4096.0 / (1024.0 * 1024.0);
}
#endif

typedef struct {
    char *buf; size_t cap;
    const char *lang;
    bool wrap, plain, big;
    int rev;                    /* the revision to give; 0: 1 with .big */
    cui_text_doc *doc;          /* edit this document instead of buf */
    cui_code_editor_result r;
} bench_ed;

static void build(cui_ctx *ui, void *u) {
    bench_ed *b = u;
#if defined(BENCH_BASELINE)   /* the library before .plain and .revision */
    b->r = cui_code_editor(ui, "ed", b->buf, b->cap, .lang = b->plain ? "text" : b->lang, .wrap = b->wrap, .height = -1);
#else
    if (b->doc) b->r = cui_code_editor(ui, "ed", NULL, 0, .doc = b->doc, .lang = b->lang, .wrap = b->wrap, .height = -1, .plain = b->plain);
    else b->r = cui_code_editor(ui, "ed", b->buf, b->cap, .lang = b->lang, .wrap = b->wrap, .height = -1,
                                .plain = b->plain, .revision = b->rev ? b->rev : b->big ? 1 : 0);
#endif
}

static double frames(cui_ctx *ui, bench_ed *b, int n, int scroll) {
    double worst = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (scroll) cui_input_scroll(ui, 0, (float)-scroll);
        double t0 = now();
        cui_begin_frame(ui, 1.0f / 60.0f);
        build(ui, b);
        cui_end_frame(ui);
        double t = (now() - t0) * 1000;
        total += t;
        if (t > worst) worst = t;
    }
    (void)worst;
    return total / n;
}

/* Text: short lines like a log, or long lines (about 3000 bytes) like minified code. Lines
 * hold words, brackets, quotes and comment markers, and every 40th line in markdown is a fence. */
/* yaml and xml: items with keys, values, comments, block scalars (yaml), tags with attributes and entities (xml) */
static size_t gen_struct(char *buf, size_t size, bool long_lines, bool xml) {
    size_t n = 0;
    unsigned seed = 777;
    int line = 0;
    while (n + 4000 < size) {
        seed = seed * 1103515245u + 12345u;
        int target = long_lines ? 2500 + (int)(seed % 1000) : 0, start = (int)n;
        if (xml) {
            if (line % 50 == 49) n += (size_t)sprintf(buf + n, "<!-- note %d -->", line);
            else n += (size_t)sprintf(buf + n, "  <item id=\"%d\" name='n%d' on=\"true\">text &amp; more %d</item>", line, line, line * 7);
        } else if (line % 40 == 36) n += (size_t)sprintf(buf + n, "  text: |");
        else if (line % 40 > 36) n += (size_t)sprintf(buf + n, "    block scalar line %d: not a key # nor a comment", line);
        else if (line % 7 == 0) n += (size_t)sprintf(buf + n, "# comment %d", line);
        else n += (size_t)sprintf(buf + n, "  - key_%d: \"value %d\"  # c  [1, 2.5, true]", line, line * 3);
        while ((int)n - start < target) n += (size_t)sprintf(buf + n, xml ? " <a b=\"c\">d</a>" : ", item: 12");
        buf[n++] = '\n';
        line++;
    }
    buf[n] = 0;
    return n;
}

/* a formatted JSON document: one object, nested arrays and objects, an opening brace on the
 * first line whose partner is on the last */
static size_t gen_json(char *buf, size_t size, bool long_lines) {
    size_t n = (size_t)sprintf(buf, "{\n");
    int line = 0;
    while (n + 4000 < size - 8) {
        n += (size_t)sprintf(buf + n, "  \"key_%d\": {\n    \"id\": %d,\n    \"name\": \"item %d\",\n    \"tags\": [\"a\", \"b\", \"c\"],\n", line, line, line);
        if (long_lines) for (int k = 0; k < 40; k++) n += (size_t)sprintf(buf + n, "    \"field_%d\": [%d, %d, %d, \"value\"],\n", k, k, line, k * line);
        n += (size_t)sprintf(buf + n, "    \"ok\": true\n  },\n");
        line++;
    }
    n += (size_t)sprintf(buf + n, "  \"end\": null\n}\n");
    return n;
}

static size_t gen(char *buf, size_t size, bool long_lines, const char *lang) {
    if (!strcmp(lang, "json")) return gen_json(buf, size, long_lines);
    if (!strcmp(lang, "yaml") || !strcmp(lang, "xml")) return gen_struct(buf, size, long_lines, lang[0] == 'x');
    static const char *const words[] = { "INFO", "worker", "request", "(id=42)", "{ok}", "\"path\"", "-> done;", "//", "x[3]",
                                         "# heading", "`code`", "*em*", "value=0x1F", "elapsed", "ms", "retry" };
    size_t n = 0;
    unsigned seed = 12345;
    int line = 0;
    while (n + 4000 < size) {
        int target = long_lines ? 2500 + (int)(seed % 1000) : 40 + (int)(seed % 80);
        int start = (int)n;
        if (line % 40 == 39) { memcpy(buf + n, "```", 3); n += 3; }
        else {
            n += (size_t)sprintf(buf + n, "%08d ", line);
            while ((int)n - start < target) {
                seed = seed * 1103515245u + 12345u;
                const char *w = words[(seed >> 16) % 16];
                size_t wl = strlen(w);
                memcpy(buf + n, w, wl); n += wl;
                buf[n++] = ' ';
            }
        }
        buf[n++] = '\n';
        line++;
        seed = seed * 1103515245u + 12345u;
    }
    buf[n] = 0;
    return n;
}

static void one(size_t mb, bool long_lines, const char *lang, bool wrap, bool plain, bool big, bool doc) {
    size_t size = mb * 1024 * 1024;
    bench_ed b;
    memset(&b, 0, sizeof b);
    b.cap = doc ? size + 8192 : size + 1024 * 1024;
    b.buf = malloc(b.cap);
    size_t len = gen(b.buf, size, long_lines, lang);
    b.lang = lang; b.wrap = wrap; b.plain = plain; b.big = big;
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1200, 800, 1);
    cui_set_reduced_motion(ui, true);
    double m0 = mem_mb();
    double t0 = now();
    if (doc) { b.doc = cui_text_doc_adopt(b.buf, (int)len, NULL, NULL); b.buf = NULL; }   /* the text itself, no copy */
    double open = (now() - t0) * 1000 + frames(ui, &b, 1, 0);
    frames(ui, &b, 2, 0);
    double idle = frames(ui, &b, 5, 0);
    double mem = mem_mb() - m0;
    cui_input_mouse_move(ui, 600, 400);
    int nl = b.r.line_count;
    cui_code_editor_goto(ui, "ed", nl / 2, 1);
    frames(ui, &b, 2, 0);
    double mid = frames(ui, &b, 5, 3);
    cui_code_editor_goto(ui, "ed", nl, 1);
    frames(ui, &b, 2, 0);
    double end = frames(ui, &b, 5, -3);
    /* typing: a character each frame, near the top and near the end */
    cui_code_editor_goto(ui, "ed", 3, 5);
    frames(ui, &b, 2, 0);
    double ttop = 0, tend = 0;
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, "a"); ttop += frames(ui, &b, 1, 0); }
    cui_code_editor_goto(ui, "ed", nl - 3, 5);
    frames(ui, &b, 2, 0);
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, "b"); tend += frames(ui, &b, 1, 0); }
    double mem2 = mem_mb() - m0;
    /* Enter near the top: a line is added, every line below moves */
    cui_code_editor_goto(ui, "ed", 3, 5);
    frames(ui, &b, 2, 0);
    double enter = 0;
    for (int i = 0; i < 5; i++) { cui_input_key(ui, CUI_KEY_ENTER, true, 0); cui_input_key(ui, CUI_KEY_ENTER, false, 0); enter += frames(ui, &b, 1, 0); }
    double mem3 = mem_mb() - m0;
    printf("%4zu MB %-5s %-8s %-4s %-5s %-3s %8d lines | open %8.1f  idle %7.2f  mid %7.2f  end %7.2f  type top %7.2f  end %7.2f  enter %7.2f ms | mem %6.0f %6.0f %6.0f MB\n",
           mb, long_lines ? "long" : "short", plain ? "plain" : lang, wrap ? "wrap" : "", doc ? "doc" : big ? "rev" : "", "", nl,
           open, idle, mid, end, ttop / 5, tend / 5, enter / 5, mem, mem2, mem3);
    fflush(stdout);
    (void)len;
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_text_doc_free(b.doc);
    free(b.buf);
}

/* the worst of n frames, each with one key or text */
static double keys_worst(cui_ctx *ui, bench_ed *b, int n, const char *text, cui_key key, int mods, double *avg) {
    double worst = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (text) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, text); }
        else { cui_input_key(ui, key, true, mods); cui_input_key(ui, key, false, mods); }
        double t = frames(ui, b, 1, 0);
        total += t;
        if (t > worst) worst = t;
    }
    if (avg) *avg = total / n;
    return worst;
}

/* Buffer mode (with .revision, the fastest it has) and document mode on the same text, measured
 * the same way: average (worst) ms a frame. */
static void compare(size_t mb, const char *lang, bool wrap, bool doc) {
    size_t size = mb * 1024 * 1024;
    bench_ed b;
    memset(&b, 0, sizeof b);
    b.cap = doc ? size + 8192 : size + size / 8 + 256 * 1024;   /* what an app keeps for edits (ed: an eighth) */
    b.buf = malloc(b.cap);
    size_t len = gen(b.buf, size, false, lang);
    b.lang = lang; b.wrap = wrap; b.big = true;
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1200, 800, 1);
    cui_set_reduced_motion(ui, true);
    double m0 = mem_mb();
    double t0 = now();
    if (doc) { b.doc = cui_text_doc_adopt(b.buf, (int)len, NULL, NULL); b.buf = NULL; }
    double open = (now() - t0) * 1000 + frames(ui, &b, 1, 0);
    frames(ui, &b, 3, 0);
    double held = (double)(doc ? len : b.cap) / 1048576.0;   /* the text was made before m0: counted here, for both */
    double mem = mem_mb() - m0 + held;
    int nl = b.r.line_count;
    double at[3], aw[3], enter, ew;
    int lines[3] = { 3, nl / 2, nl - 3 };
    for (int k = 0; k < 3; k++) {
        cui_code_editor_goto(ui, "ed", lines[k], 5);
        frames(ui, &b, 2, 0);
        aw[k] = keys_worst(ui, &b, 20, "a", CUI_KEY_NONE, 0, &at[k]);
    }
    cui_code_editor_goto(ui, "ed", 3, 5);
    frames(ui, &b, 2, 0);
    ew = keys_worst(ui, &b, 10, NULL, CUI_KEY_ENTER, 0, &enter);
    double undo, uw = keys_worst(ui, &b, 3, NULL, CUI_KEY_Z, CUI_MOD_PRIMARY, &undo);
    /* scrolling in the middle */
    cui_code_editor_goto(ui, "ed", nl / 2, 1);
    frames(ui, &b, 2, 0);
    cui_input_mouse_move(ui, 600, 400);
    double sw_ = 0, st = 0;
    for (int i = 0; i < 20; i++) { cui_input_scroll(ui, 0, -3); double t = frames(ui, &b, 1, 0); st += t; if (t > sw_) sw_ = t; }
    double mem2 = mem_mb() - m0 + held;
    /* replace all from the find bar (about a sixteenth of the words), then its undo */
    cui_code_editor_replace(ui, "ed", "worker", "WORKER!");
    frames(ui, &b, 3, 0);
    for (int i = 0; i < 40 && b.r.find_matches == 0; i++) frames(ui, &b, 1, 0);
    int matches = b.r.find_matches;
    cui_input_key(ui, CUI_KEY_ENTER, true, CUI_MOD_PRIMARY | CUI_MOD_ALT);
    cui_input_key(ui, CUI_KEY_ENTER, false, CUI_MOD_PRIMARY | CUI_MOD_ALT);
    double rall = frames(ui, &b, 1, 0), rnext = 0;
    for (int i = 0; i < 4; i++) { double t = frames(ui, &b, 1, 0); if (t > rnext) rnext = t; }
    int after = b.r.length;
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0); cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    frames(ui, &b, 2, 0);
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY); cui_input_key(ui, CUI_KEY_Z, false, CUI_MOD_PRIMARY);
    double rundo = frames(ui, &b, 1, 0);
    bool undone = b.r.length != after;
    double mem3 = mem_mb() - m0 + held;
    printf("%-3s %4zu MB %-8s %-4s %8d lines | open %7.1f | type top %5.2f (%5.2f) mid %5.2f (%5.2f) end %5.2f (%5.2f) enter %5.2f (%5.2f) undo %5.2f (%5.2f)"
           " | scroll %5.2f (%5.2f) | replace all %d: %7.1f then %6.2f, undo %7.1f%s | mem %5.0f %5.0f %5.0f MB (text %4.0f)\n",
           doc ? "doc" : "buf", mb, lang, wrap ? "wrap" : "", nl, open, at[0], aw[0], at[1], aw[1], at[2], aw[2], enter, ew, undo, uw,
           st / 20, sw_, matches, rall, rnext, rundo, undone ? "" : " (NOT UNDONE)", mem, mem2, mem3, (double)len / 1048576.0);
    fflush(stdout);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_text_doc_free(b.doc);
    free(b.buf);
}

/* Replacing (almost) the whole text of a big buffer: cui_code_editor_edit over [0, len) with
 * a text of another size (the way an app applies a formatter's output so that Ctrl Z undoes
 * it), then undo and redo; then the app swapping the text and changing .revision. Each
 * number is the ms of one frame, in order. */
static void replace_all(size_t from_mb, size_t to_mb, const char *lang, bool wrap, bool doc) {
    bench_ed b;
    memset(&b, 0, sizeof b);
    b.cap = to_mb * 1024 * 1024 + 8 * 1024 * 1024;
    b.buf = malloc(b.cap);
    size_t len = gen(b.buf, from_mb * 1024 * 1024, false, lang);
    char *nt = malloc(to_mb * 1024 * 1024 + 4096);
    gen(nt, to_mb * 1024 * 1024, false, lang);
    b.lang = lang; b.wrap = wrap; b.big = true; b.rev = 1;
    if (doc) { b.doc = cui_text_doc_adopt(b.buf, (int)len, NULL, NULL); b.buf = NULL; }
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1200, 800, 1);
    cui_set_reduced_motion(ui, true);
    frames(ui, &b, 3, 0);
    cui_code_editor_goto(ui, "ed", b.r.line_count / 2, 1);
    frames(ui, &b, 2, 0);
    double c0 = now();
    cui_code_editor_edit(ui, "ed", 0, (int)len, nt, 0, 0);
    double call = (now() - c0) * 1000;
    double e1 = frames(ui, &b, 1, 0), e2 = frames(ui, &b, 1, 0), e3 = frames(ui, &b, 1, 0);
    int nl = b.r.line_count;
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY);
    double u1 = frames(ui, &b, 1, 0), u2 = frames(ui, &b, 1, 0);
    int ok_undo = b.r.length == (int)len;
    cui_input_key(ui, CUI_KEY_Z, true, CUI_MOD_PRIMARY | CUI_MOD_SHIFT);
    double r1 = frames(ui, &b, 1, 0), r2 = frames(ui, &b, 1, 0);
    int ok_redo = b.r.line_count == nl;
    /* the app swaps in another text of the new size and bumps the revision (or sets the document) */
    double v0 = 0;
    if (doc) { size_t n2 = gen(nt, to_mb * 1024 * 1024, true, lang); v0 = now(); cui_text_doc_set(b.doc, nt, (int)n2); v0 = (now() - v0) * 1000; }   /* the set (a copy and its line breaks), not the making of the text */
    else gen(b.buf, to_mb * 1024 * 1024, true, lang);
    b.rev = 2;
    double v1 = frames(ui, &b, 1, 0) + v0, v2 = frames(ui, &b, 1, 0);
    printf("replace %2zu -> %2zu MB %-8s %-4s %-3s | edit call %5.1f frames %6.1f %6.1f %6.1f | undo %6.1f %6.1f | redo %6.1f %6.1f | new text %6.1f %6.1f ms | undo %s redo %s\n",
           from_mb, to_mb, lang, wrap ? "wrap" : "", doc ? "doc" : "", call, e1, e2, e3, u1, u2, r1, r2, v1, v2, ok_undo ? "ok" : "BAD", ok_redo ? "ok" : "BAD");
    fflush(stdout);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_text_doc_free(b.doc);
    free(b.buf); free(nt);
}

/* Two editors side by side on one buffer (a split view), with .revision: each frame draws both,
 * and an edit in one is handed to the other as a splice. Typing near the top and the end, Enter,
 * typing in the other editor, undo in each; each number is the ms of a frame (both editors). */
typedef struct {
    char *buf; size_t cap;
    cui_text_doc *doc;          /* both editors on this document: no splices to pass */
    bool wrap;
    cui_id id[2];
    cui_code_editor_result r[2];
} bench_two;
static void build_two(cui_ctx *ui, void *u) {
    bench_two *b = u;
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
        for (int k = 0; k < 2; k++) {
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
                if (b->doc) b->r[k] = cui_code_editor(ui, k ? "b" : "a", NULL, 0, .doc = b->doc, .lang = "text", .plain = true, .wrap = b->wrap, .height = -1);
                else b->r[k] = cui_code_editor(ui, k ? "b" : "a", b->buf, b->cap, .lang = "text", .plain = true, .wrap = b->wrap, .height = -1, .revision = 1);
                b->id[k] = cui_last_id(ui);
            }
            if (!b->doc && b->r[k].changed && b->id[1 - k]) cui_code_editor_splice(ui, b->id[1 - k], b->r[k].splice_start, b->r[k].splice_removed, b->r[k].splice_inserted);
        }
    }
}
static double frame_two(cui_ctx *ui, bench_two *b, double *worst) {
    double t0 = now();
    cui_begin_frame(ui, 1.0f / 60.0f);
    build_two(ui, b);
    cui_end_frame(ui);
    double t = (now() - t0) * 1000;
    if (t > *worst) *worst = t;
    return t;
}
/* n frames, each with the key or text given, in editor k (focused first): the average, and the worst in *w */
static double keys_two(cui_ctx *ui, bench_two *b, int n, const char *text, cui_key key, int mods, double *w) {
    double total = 0;
    *w = 0;
    for (int i = 0; i < n; i++) {
        if (text) { cui_input_key(ui, CUI_KEY_NONE, false, 0); cui_input_text(ui, text); }
        else { cui_input_key(ui, key, true, mods); cui_input_key(ui, key, false, mods); }
        total += frame_two(ui, b, w);
    }
    return total / n;
}
static void shared(size_t mb, bool wrap, bool doc) {
    bench_two b;
    memset(&b, 0, sizeof b);
    b.cap = mb * 1024 * 1024 + 1024 * 1024;
    b.buf = malloc(b.cap);
    size_t len = gen(b.buf, mb * 1024 * 1024, false, "text");
    b.wrap = wrap;
    double t0 = now();
    if (doc) { b.doc = cui_text_doc_adopt(b.buf, (int)len, NULL, NULL); b.buf = NULL; }
    t0 = (now() - t0) * 1000;
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 1400, 800, 1);
    cui_set_reduced_motion(ui, true);
    double w = 0, open = frame_two(ui, &b, &w) + t0;
    for (int i = 0; i < 3; i++) frame_two(ui, &b, &w);
    int nl = b.r[0].line_count;
    cui_code_editor_goto(ui, "b", nl / 2, 1);   /* the other editor looks at the middle */
    for (int i = 0; i < 2; i++) frame_two(ui, &b, &w);
    double idle = 0, wi = 0;
    for (int i = 0; i < 10; i++) idle += frame_two(ui, &b, &wi);
    double wt, we, wn, wo, wu, wv;
    cui_code_editor_goto(ui, "a", 3, 5);
    for (int i = 0; i < 2; i++) frame_two(ui, &b, &w);
    double top = keys_two(ui, &b, 10, "a", CUI_KEY_NONE, 0, &wt);
    double enter = keys_two(ui, &b, 10, NULL, CUI_KEY_ENTER, 0, &wn);
    cui_code_editor_goto(ui, "a", nl - 3, 5);
    for (int i = 0; i < 2; i++) frame_two(ui, &b, &w);
    double end = keys_two(ui, &b, 10, "b", CUI_KEY_NONE, 0, &we);
    cui_code_editor_goto(ui, "b", nl / 2, 5);   /* now type in the other one */
    for (int i = 0; i < 2; i++) frame_two(ui, &b, &w);
    double other = keys_two(ui, &b, 10, "c", CUI_KEY_NONE, 0, &wo);
    double undo_b = keys_two(ui, &b, 3, NULL, CUI_KEY_Z, CUI_MOD_PRIMARY, &wu);
    cui_code_editor_select(ui, "a", 10, 10);
    for (int i = 0; i < 2; i++) frame_two(ui, &b, &w);
    double undo_a = keys_two(ui, &b, 3, NULL, CUI_KEY_Z, CUI_MOD_PRIMARY, &wv);
    int real = b.doc ? cui_text_doc_length(b.doc) : (int)strlen(b.buf);
    bool same = b.r[0].length == b.r[1].length && b.r[0].line_count == b.r[1].line_count && b.r[0].length == real;
    printf("shared %4zu MB %-4s %-3s %8d lines | open %6.1f  idle %5.2f (%5.2f) | type top %6.2f (%6.2f)  enter %6.2f (%6.2f)  end %5.2f (%5.2f)"
           "  other %6.2f (%6.2f) | undo there %6.2f (%6.2f) here %6.2f (%6.2f) ms, avg (worst) | %s\n",
           mb, wrap ? "wrap" : "", doc ? "doc" : "", nl, open, idle / 10, wi, top, wt, enter, wn, end, we, other, wo, undo_b, wu, undo_a, wv, same ? "both agree" : "DISAGREE");
    fflush(stdout);
    cui_destroy(ui);
    cui_sw_destroy(sw);
    cui_text_doc_free(b.doc);
    free(b.buf);
}

int main(int argc, char **argv) {
    bool sh = argc > 1 && (!strcmp(argv[1], "--shared") || !strcmp(argv[1], "--shared-doc"));
    if (sh) {   /* --shared[-doc] [MB ...] */
        bool doc = !strcmp(argv[1], "--shared-doc");
        size_t mbs[8] = { 10, 100 };
        int n = 2;
        if (argc > 2) { n = 0; for (int i = 2; i < argc && n < 8; i++) mbs[n++] = (size_t)atoi(argv[i]); }
        for (int i = 0; i < n; i++) for (int w = 0; w < 2; w++) shared(mbs[i], w, doc);
        return 0;
    }

    if (argc > 1 && (!strcmp(argv[1], "--replace") || !strcmp(argv[1], "--replace-doc"))) {   /* --replace[-doc] [FROM_MB TO_MB] */
        bool doc = !strcmp(argv[1], "--replace-doc");
        size_t f = argc > 3 ? (size_t)atoi(argv[2]) : 20, t = argc > 3 ? (size_t)atoi(argv[3]) : 50;
        static const char *const rl[] = { "text", "c", "markdown", "json" };
        for (int w = 0; w < 2; w++) for (int l = 0; l < 4; l++) replace_all(f, t, rl[l], w, doc);
        return 0;
    }

    if (argc > 1 && !strcmp(argv[1], "--compare")) {   /* --compare [MB ...] */
        size_t mbs[8] = { 1, 10, 100 };
        int n = 3;
        if (argc > 2) { n = 0; for (int i = 2; i < argc && n < 8; i++) mbs[n++] = (size_t)atoi(argv[i]); }
        static const char *const cl[] = { "text", "c", "markdown" };
        for (int i = 0; i < n; i++)
            for (int w = 0; w < 2; w++)
                for (int l = 0; l < 3; l++) { compare(mbs[i], cl[l], w, false); compare(mbs[i], cl[l], w, true); }
        return 0;
    }
    bool docs = argc > 1 && !strcmp(argv[1], "--doc");
    if (docs) { argc--; argv++; }
    size_t sizes[8] = { 1, 10, 100 };
    int ns = 3;
    bool bigs[2] = { true, false };   /* with .revision first: the way an app opens a big file */
    int nb = 2;
#if defined(BENCH_BASELINE)
    nb = 1;
#endif
    if (argc > 1 && !strcmp(argv[1], "--quick")) ns = 2;
    else if (argc > 1) { ns = 0; for (int i = 1; i < argc && ns < 8; i++) sizes[ns++] = (size_t)atoi(argv[i]); }
    static const char *const langs[] = { "text", "c", "markdown", "yaml", "xml" };
    for (int s = 0; s < ns; s++)
        for (int ll = 0; ll < 2; ll++)
            for (int w = 0; w < 2; w++)
                for (int k = 0; k < nb; k++) {
                    if (docs && k) continue;   /* a document needs no revision: once */
                    for (int l = 0; l < 5; l++) one(sizes[s], ll, langs[l], w, false, bigs[k], docs);
                    one(sizes[s], ll, "text", w, true, bigs[k], docs);
                }
    return 0;
}
