/* Tests for find in the VT terminal (cui_vt_find and the view's .find): plain text with and without case, whole words,
 * regular expressions, wrapped lines, wide characters, the alternate screen, stepping, the current match kept across
 * searches, the view scrolling to it, marking it and selecting it for Ctrl+C, nothing changed without .find, memory.
 * Prints how long searching 5000 lines takes. Runs headless with the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b))) { g_failed++; printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static cui_vt_match M(const cui_vt *v, int i) { cui_vt_match m = { -99, -99, -99, -99, -99 }; cui_vt_find_match(v, i, &m); return m; }

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static void test_plain(void) {
    cui_vt *v = cui_vt_create(40, 5, 100);
    CHECK_EQ(cui_vt_find_get(v).current, -1);
    CHECK_EQ(cui_vt_find(v, "x", 0), 0);                       /* an empty screen */
    W(v, "alpha beta\r\nBeta gamma BETA\r\nbetamax");
    CHECK_EQ(cui_vt_find(v, "beta", 0), 4);
    cui_vt_match m = M(v, 0);
    CHECK_EQ(m.row, 0); CHECK_EQ(m.col, 6); CHECK_EQ(m.len, 4); CHECK_EQ(m.end_row, 0); CHECK_EQ(m.end_col, 10);
    m = M(v, 2);
    CHECK_EQ(m.row, 1); CHECK_EQ(m.col, 11);
    CHECK_EQ(cui_vt_find_get(v).current, 3);                    /* a new search starts at the newest */
    CHECK_EQ(cui_vt_find(v, "beta", CUI_FIND_CASE), 2);
    CHECK_EQ(cui_vt_find(v, "Beta", CUI_FIND_CASE), 1);
    CHECK_EQ(cui_vt_find(v, "beta", CUI_FIND_WORD), 3);         /* not "betamax" */
    CHECK_EQ(cui_vt_find(v, "", 0), 0);
    CHECK_EQ(cui_vt_find_get(v).current, -1);
    CHECK(!cui_vt_find_match(v, 0, &m));
    CHECK_EQ(cui_vt_find(v, "aa", 0), 0);
    W(v, "\r\naaaa");
    CHECK_EQ(cui_vt_find(v, "aa", 0), 2);                       /* no overlapping matches */
    CHECK_EQ(cui_vt_find(v, "a  g", 0), 0);
    CHECK_EQ(cui_vt_find(v, "a g", 0), 1);                      /* "Beta gamma": spaces as written */
    W(v, "\r\n\x1b[5Gmid");                                     /* cells never written are spaces in the middle of a line */
    CHECK_EQ(cui_vt_find(v, "    mid", 0), 1);
    CHECK_EQ(cui_vt_find(v, "Привет", 0), 0);
    W(v, "\r\nПРИВЕТ Ölçü");
    CHECK_EQ(cui_vt_find(v, "привет", 0), 1);                   /* Cyrillic and Latin-1 letters fold too */
    CHECK_EQ(cui_vt_find(v, "öLÇÜ", 0), 1);
    CHECK_EQ(cui_vt_find(v, "привет", CUI_FIND_CASE), 0);
    cui_vt_destroy(v);
}

static void test_wrap_wide(void) {
    cui_vt *v = cui_vt_create(10, 4, 100);
    W(v, "1234567hello world\r\n");                             /* "hel" ends row 0, "lo world" goes on in row 1 */
    CHECK_EQ(cui_vt_find(v, "hello", 0), 1);
    cui_vt_match m = M(v, 0);
    CHECK_EQ(m.row, 0); CHECK_EQ(m.col, 7); CHECK_EQ(m.len, 5); CHECK_EQ(m.end_row, 1); CHECK_EQ(m.end_col, 2);
    CHECK_EQ(cui_vt_find(v, "1234567hel", 0), 1);               /* a match that ends with its row */
    m = M(v, 0);
    CHECK_EQ(m.len, 10); CHECK_EQ(m.end_row, 1); CHECK_EQ(m.end_col, 0);
    CHECK_EQ(cui_vt_find(v, "world", CUI_FIND_WORD), 1);
    W(v, "\x1b[2J\x1b[H日本語 text 日本");                      /* wide characters cover two cells each */
    CHECK_EQ(cui_vt_find(v, "本語", 0), 1);
    m = M(v, 0);
    CHECK_EQ(m.row, 0); CHECK_EQ(m.col, 2); CHECK_EQ(m.len, 4);
    CHECK_EQ(cui_vt_find(v, "text", 0), 1);
    m = M(v, 0);
    CHECK_EQ(m.col, 7); CHECK_EQ(m.len, 4);
    CHECK_EQ(cui_vt_find(v, "日本", 0), 2);                     /* the second one wraps: 6 + 1 + 4 + 1 = 12 cells */
    m = M(v, 1);
    CHECK_EQ(m.row, 1); CHECK_EQ(m.col, 2); CHECK_EQ(m.len, 4);
    W(v, "\x1b[2J\x1b[H123456789日本");                         /* a wide character that does not fit leaves the last cell empty and wraps */
    CHECK_EQ(cui_vt_find(v, "9日", 0), 1);
    m = M(v, 0);
    CHECK_EQ(m.row, 0); CHECK_EQ(m.col, 8); CHECK_EQ(m.end_row, 1); CHECK_EQ(m.end_col, 2);
    cui_vt_destroy(v);
}

static void test_regex(void) {
    cui_vt *v = cui_vt_create(40, 5, 100);
    W(v, "error: one\r\nwarning: two\r\nERROR 42\r\nok");
    CHECK_EQ(cui_vt_find(v, "^(error|warning)", CUI_FIND_REGEX), 3);     /* ^ is the start of every line; case ignored */
    CHECK_EQ(cui_vt_find(v, "^(error|warning)", CUI_FIND_REGEX | CUI_FIND_CASE), 2);
    CHECK_EQ(cui_vt_find(v, "\\d+", CUI_FIND_REGEX), 1);
    cui_vt_match m = M(v, 0);
    CHECK_EQ(m.row, 2); CHECK_EQ(m.col, 6); CHECK_EQ(m.len, 2);
    CHECK_EQ(cui_vt_find(v, "o$", CUI_FIND_REGEX), 1);           /* "two" (and not "error: one", ending in e) */
    CHECK_EQ(cui_vt_find(v, "x*", CUI_FIND_REGEX), 0);           /* empty matches are skipped */
    CHECK(cui_vt_find_get(v).error == NULL);
    CHECK_EQ(cui_vt_find(v, "(err", CUI_FIND_REGEX), 0);
    CHECK(cui_vt_find_get(v).error != NULL);
    CHECK_EQ(cui_vt_find(v, "(err", 0), 0);                      /* the same text, plain: no error */
    CHECK(cui_vt_find_get(v).error == NULL);
    CHECK_EQ(cui_vt_find(v, "on", CUI_FIND_REGEX | CUI_FIND_WORD), 0);
    CHECK_EQ(cui_vt_find(v, "one", CUI_FIND_REGEX | CUI_FIND_WORD), 1);
    cui_vt_destroy(v);
}

static void test_scrollback_steps(void) {
    cui_vt *v = cui_vt_create(40, 10, 5000);
    char b[64];
    for (int i = 0; i < 5000; i++) { snprintf(b, sizeof b, "line %d\r\n", i); W(v, b); }
    CHECK_EQ(cui_vt_find(v, "line 42", 0), 111);                 /* 42, 420..429, 4200..4299 */
    CHECK_EQ(cui_vt_find(v, "line 42", CUI_FIND_WORD), 1);
    cui_vt_match m = M(v, 0);
    int sb = cui_vt_scrollback_count(v);
    CHECK_EQ(m.row, 42 - sb);                                    /* rows before the live screen are negative */
    CHECK_EQ(cui_vt_find(v, "line 4\\d{3}$", CUI_FIND_REGEX), 1000);
    CHECK_EQ(cui_vt_find(v, "line 0", 0), 1);                    /* 5001 rows: all of it is kept (5000 + 10 on screen) */
    CHECK_EQ(cui_vt_find(v, "line 9", CUI_FIND_WORD), 1);
    CHECK_EQ(cui_vt_find(v, "line 4999", 0), 1);
    /* stepping: up goes to older matches, and wraps */
    CHECK_EQ(cui_vt_find(v, "line 499", 0), 11);                 /* 499, 4990..4999 */
    CHECK_EQ(cui_vt_find_get(v).current, 10);
    CHECK_EQ(cui_vt_find_step(v, -1), 9);
    CHECK_EQ(cui_vt_find_step(v, 1), 10);
    CHECK_EQ(cui_vt_find_step(v, 1), 0);
    CHECK_EQ(cui_vt_find_step(v, -1), 10);
    cui_vt_find_select(v, 3);
    CHECK_EQ(cui_vt_find_get(v).current, 3);
    m = M(v, 3);
    /* the current match stays where it was: a longer query, new output, the same query again */
    CHECK_EQ(cui_vt_find(v, "line 4992", 0), 1);
    CHECK_EQ(cui_vt_find_get(v).current, 0);
    CHECK_EQ(M(v, 0).row, m.row);
    CHECK_EQ(cui_vt_find(v, "line 499", 0), 11);
    CHECK_EQ(cui_vt_find_get(v).current, 3);
    int at = M(v, 3).row;
    W(v, "line 4990 again\r\nmore\r\n");
    CHECK_EQ(cui_vt_find(v, "line 499", 0), 12);
    CHECK_EQ(cui_vt_find_get(v).current, 3);
    CHECK_EQ(M(v, 3).row, at - 2);                               /* the same text, two rows further back */
    CHECK_EQ(cui_vt_find(v, "line 4995", 0), 1);
    int r4995 = M(v, 0).row;
    CHECK_EQ(cui_vt_find(v, "line 49", 0), 112);                 /* a shorter query keeps the place */
    CHECK_EQ(M(v, cui_vt_find_get(v).current).row, r4995);
    CHECK_EQ(cui_vt_find(v, "line 4990", 0), 2);                 /* no match there any more: the nearest one before */
    CHECK_EQ(cui_vt_find_get(v).current, 0);
    CHECK_EQ(M(v, 0).row, r4995 - 5);
    /* more than 50000: the first 50000 are kept */
    for (int i = 0; i < 400; i++) W(v, "e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e e\r\n");
    CHECK_EQ(cui_vt_find(v, "e", 0), 50000);
    CHECK(cui_vt_find_get(v).truncated);
    size_t with = cui_vt_memory(v);
    cui_vt_find_clear(v);
    CHECK(cui_vt_memory(v) < with);
    CHECK_EQ(cui_vt_find_get(v).count, 0);
    CHECK_EQ(cui_vt_find_step(v, 1), -1);
    cui_vt_destroy(v);
    /* lines that left the scrollback are not found */
    v = cui_vt_create(20, 2, 3);
    for (int i = 0; i < 10; i++) { snprintf(b, sizeof b, "n%d\r\n", i); W(v, b); }
    CHECK_EQ(cui_vt_find(v, "n", 0), 4);                         /* n6..n9: 3 in the scrollback, 1 on screen above the cursor */
    CHECK_EQ(M(v, 0).row, -3);
    cui_vt_destroy(v);
}

static void test_alt_screen(void) {
    cui_vt *v = cui_vt_create(20, 4, 100);
    W(v, "foo one\r\nfoo two\r\n\r\n\r\n\r\nfoo three");
    CHECK_EQ(cui_vt_find(v, "foo", 0), 3);
    W(v, "\x1b[?1049h\x1b[2J\x1b[Hfoo in vim");
    CHECK_EQ(cui_vt_find(v, "foo", 0), 1);                       /* only the screen while a full screen program runs */
    W(v, "\x1b[?1049l");
    CHECK_EQ(cui_vt_find(v, "foo", 0), 3);
    cui_vt_destroy(v);
}

static void test_speed(void) {
    cui_vt *v = cui_vt_create(120, 40, 9000);                   /* the ring keeps 48 cells a line on average: room for 5000 lines of 80 */
    char b[256];
    unsigned seed = 7;
    for (int i = 0; i < 5040; i++) {                             /* 5000 lines of scrollback, 40 on screen: words of 2 to 9 letters, about 80 columns */
        int n = 0;
        while (n < 78) {
            seed = seed * 1103515245u + 12345u;
            int w = 2 + (int)((seed >> 16) % 8);
            for (int k = 0; k < w; k++) { seed = seed * 1103515245u + 12345u; b[n++] = (char)('a' + (seed >> 16) % 26); }
            b[n++] = ' ';
        }
        snprintf(b + n, sizeof b - (size_t)n, "%d\r\n", i);
        W(v, b);
    }
    CHECK_EQ(cui_vt_scrollback_count(v), 5001);
    const struct { const char *q; int flags; const char *name; } runs[] = {
        { "error", 0, "plain, any case" }, { "qz", CUI_FIND_CASE, "plain, case" }, { "e", 0, "plain, every e" },
        { "err(or)?", CUI_FIND_REGEX, "regex err(or)?" }, { "\\b[a-z]{9}\\b", CUI_FIND_REGEX, "regex 9-letter words" },
    };
    for (size_t r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        double best = 1e9;
        int n = 0;
        for (int k = 0; k < 5; k++) {
            double t0 = now_ms();
            n = cui_vt_find(v, runs[r].q, runs[r].flags);
            double t = now_ms() - t0;
            if (t < best) best = t;
            cui_vt_find(v, "", 0);
        }
        printf("  find in 5040 lines, %-22s %6d matches  %6.2f ms\n", runs[r].name, n, best);
        CHECK(best < (runs[r].flags & CUI_FIND_REGEX ? 250.0 : 50.0));       /* loose: a busy machine; the printed figure is the measure */
    }
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
static char g_clip[256];
static void set_clip(void *user, const char *text) { (void)user; snprintf(g_clip, sizeof g_clip, "%s", text); }
static const char *get_clip(void *user) { (void)user; return g_clip; }

typedef struct { cui_vt *vt; cui_vt_result r; bool focus, find; } scene;

static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { s->r = cui_vt_view(ui, "t", s->vt, .focus = s->focus, .find = s->find); }
    cui_end_frame(ui);
    s->focus = false;
}

static uint32_t paint_hash(cui_ctx *ui) {
    static uint32_t px[640 * 400];
    cui_sw_render(g_sw, cui_get_draw_data(ui), px, 640, 400, 640 * 4, CUI_SW_RGBA);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 640 * 400; i++) h = (h ^ px[i]) * 16777619u;
    return h;
}

static void test_view(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    cui_platform_iface pf = { 0 };
    pf.set_clipboard = set_clip; pf.get_clipboard = get_clip;
    cui_set_platform(ui, &pf);
    scene s = { cui_vt_create(80, 24, 1000), { 0 }, true, false };
    for (int i = 0; i < 3; i++) build(ui, &s);
    int rows = s.r.rows;
    char b[64];
    for (int i = 0; i < 300; i++) { snprintf(b, sizeof b, "row %d%s\r\n", i, i == 40 ? " needle" : ""); W(s.vt, b); }
    build(ui, &s); build(ui, &s);
    CHECK_EQ(s.r.scrolled, 0);
    uint32_t plain = paint_hash(ui);
    /* found, but the view was not asked to show it: nothing changes */
    CHECK_EQ(cui_vt_find(s.vt, "needle", 0), 1);
    build(ui, &s); build(ui, &s);
    CHECK_EQ(s.r.scrolled, 0);
    CHECK(paint_hash(ui) == plain);
    /* .find: the view scrolls back to it (to the middle) and selects it; Ctrl+C copies it */
    s.find = true;
    build(ui, &s); build(ui, &s);
    int sb = cui_vt_scrollback_count(s.vt);
    cui_vt_match m = M(s.vt, 0);
    CHECK_EQ(m.row, 40 - sb);
    CHECK_EQ(s.r.scrolled, sb - 40 + rows / 2);
    CHECK(paint_hash(ui) != plain);
    g_clip[0] = 0;
    cui_input_key(ui, CUI_KEY_C, true, CUI_MOD_CTRL);
    build(ui, &s);
    cui_input_key(ui, CUI_KEY_C, false, CUI_MOD_CTRL);
    CHECK(s.r.copied);
    CHECK_EQ(s.r.input_len, 0);                                    /* a copy, not the interrupt */
    CHECK_STR(g_clip, "needle");
    /* stepping between two matches far apart moves the view; one already in sight does not move it */
    W(s.vt, "a needle on the screen\r\n");
    build(ui, &s);                                                 /* new output: the view finds again (at most every 100 ms) */
    for (int i = 0; i < 8 && cui_vt_find_get(s.vt).count < 2; i++) { cui_input_mouse_move(ui, 1, 1); build(ui, &s); }
    CHECK_EQ(cui_vt_find(s.vt, "needle", 0), 2);
    CHECK_EQ(cui_vt_find_get(s.vt).current, 0);                     /* still the old one */
    int at = s.r.scrolled;
    cui_vt_find_step(s.vt, 1);
    build(ui, &s); build(ui, &s);
    CHECK_EQ(s.r.scrolled, 0);                                     /* the new one is on the live screen */
    g_clip[0] = 0;
    cui_input_key(ui, CUI_KEY_C, true, CUI_MOD_CTRL | CUI_MOD_SHIFT);
    build(ui, &s);
    cui_input_key(ui, CUI_KEY_C, false, CUI_MOD_CTRL | CUI_MOD_SHIFT);
    CHECK_STR(g_clip, "needle");
    cui_vt_find_step(s.vt, 1);
    build(ui, &s); build(ui, &s);
    CHECK(s.r.scrolled > 0 && s.r.scrolled >= at - 2 && s.r.scrolled <= at + 2);
    /* cleared: no marks, the view stays where it is */
    uint32_t marked = paint_hash(ui);
    cui_vt_find_clear(s.vt);
    build(ui, &s); build(ui, &s);
    CHECK(s.r.scrolled > 0);
    CHECK(paint_hash(ui) != marked);
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* key records (.win32_keys): Ctrl pressed alone, as a real keyboard sends it a frame before the C, is a record for the
 * program but not typing: the found text stays selected and Ctrl+C copies it, the view stays where it looks */
static void test_view_records(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    cui_platform_iface pf = { 0 };
    pf.set_clipboard = set_clip; pf.get_clipboard = get_clip;
    cui_set_platform(ui, &pf);
    cui_vt *vt = cui_vt_create(80, 24, 1000);
    W(vt, "\x1b[?9001h");
    char b[64];
    for (int i = 0; i < 200; i++) { snprintf(b, sizeof b, "row %d%s\r\n", i, i == 20 ? " found-it" : ""); W(vt, b); }
    cui_vt_result r = { 0 };
    bool focus = true;
    #define FRAME() do { cui_begin_frame(ui, 1.0f / 60.0f); cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { \
        r = cui_vt_view(ui, "t", vt, .focus = focus, .win32_keys = true, .find = true); } cui_end_frame(ui); focus = false; } while (0)
    FRAME(); FRAME();
    CHECK_EQ(cui_vt_find(vt, "found-it", 0), 1);
    FRAME(); FRAME();
    int back = r.scrolled;
    CHECK(back > 0);
    cui_input_key(ui, CUI_KEY_NONE, true, CUI_MOD_CTRL);         /* Ctrl alone */
    FRAME();
    CHECK(r.input_len > 0);                                       /* the program hears it */
    CHECK_EQ(r.scrolled, back);
    g_clip[0] = 0;
    cui_input_key(ui, CUI_KEY_C, true, CUI_MOD_CTRL);
    FRAME();
    cui_input_key(ui, CUI_KEY_C, false, CUI_MOD_CTRL);
    CHECK(r.copied);
    CHECK_STR(g_clip, "found-it");
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    FRAME();
    CHECK_EQ(r.scrolled, back);
    cui_input_text(ui, "x");                                      /* typing: back to the live screen */
    FRAME();
    CHECK_EQ(r.scrolled, 0);
    #undef FRAME
    cui_vt_destroy(vt);
    cui_destroy(ui);
}

/* the two helpers the find bar of an app uses: a field given the keyboard with its text selected, the platform read back */
static void test_helpers(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_platform_iface pf = { 0 }, back;
    pf.set_clipboard = set_clip; pf.get_clipboard = get_clip;
    cui_set_platform(ui, &pf);
    cui_get_platform(ui, &back);
    CHECK(back.set_clipboard == set_clip && back.get_clipboard == get_clip && back.set_cursor == NULL);
    char q[64] = "old query";
    for (int i = 0; i < 5; i++) {
        if (i == 3) cui_input_text(ui, "new");
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .width = CUI_FILL) {
            if (i == 1) cui_focus_select_all(ui, cui_make_id(ui, "q"));
            cui_input(ui, "q", q, sizeof q);
        }
        cui_end_frame(ui);
    }
    CHECK_STR(q, "new");                                          /* the selected text was replaced */
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_plain();
    test_wrap_wide();
    test_regex();
    test_scrollback_steps();
    test_alt_screen();
    test_speed();
    test_view();
    test_view_records();
    test_helpers();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
