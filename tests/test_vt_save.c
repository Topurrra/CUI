/* Tests for saving a VT screen and loading it again (cui_vt_save, cui_vt_load): cells come back exactly (text, wide
 * characters, attributes, the underline styles, 16 / 256 / true colors, blank cells with a background, soft wraps, the
 * cursor), the scrollback with them, the cap on rows, hyperlinks, command marks with their exit codes, command lines and
 * run times, the main screen behind the alternate one, another width (the rows wrap again), and loads that must fail
 * without touching the screen: another magic, a newer version, a cut-off or damaged file, a stream with a sequence the
 * saver never writes (a query, a bell, a title, a clipboard request). Prints the speed of save and load. Headless. */
#include <cui/cui.h>

#include <limits.h>
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
static char g_t[8192];
static const char *LINE(const cui_vt *v, int row) { cui_vt_line_text(v, row, g_t, sizeof g_t); return g_t; }
static cui_vt *mk(int cols, int rows, int sb, unsigned f) { cui_vt *v = cui_vt_create(cols, rows, sb); if (f) cui_vt_set_features(v, f); return v; }

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

static bool same_cell(const cui_vt *a, const cui_vt *b, int x, int y) {
    cui_vt_cell p, q;
    if (!cui_vt_cell_at(a, x, y, &p) || !cui_vt_cell_at(b, x, y, &q)) return false;
    return p.ch == q.ch && p.fg == q.fg && p.bg == q.bg && p.attrs == q.attrs;
}

/* every cell of the screens, every row's text of the scrollback, the cursor, and the saved bytes again (a second save of
 * the loaded screen is the same stream: every cell, wrap and link of the scrollback came back too) */
static void check_same(const cui_vt *a, const cui_vt *b) {
    int ca, ra, cb, rb;
    cui_vt_size(a, &ca, &ra); cui_vt_size(b, &cb, &rb);
    CHECK_EQ(cb, ca); CHECK_EQ(rb, ra);
    int bad = 0;
    for (int y = 0; y < ra; y++) for (int x = 0; x < ca; x++) if (!same_cell(a, b, x, y)) { if (!bad) printf("  first different cell %d,%d\n", x, y); bad++; }
    CHECK_EQ(bad, 0);
    CHECK_EQ(cui_vt_scrollback_count(b), cui_vt_scrollback_count(a));
    char t1[2048], t2[2048];
    int lines_bad = 0;
    for (int r = -cui_vt_scrollback_count(a); r < ra; r++) {
        cui_vt_line_text(a, r, t1, sizeof t1); cui_vt_line_text(b, r, t2, sizeof t2);
        if (strcmp(t1, t2)) { if (!lines_bad) printf("  row %d: \"%s\" vs \"%s\"\n", r, t1, t2); lines_bad++; }
    }
    CHECK_EQ(lines_bad, 0);
    int x1, y1, x2, y2;
    cui_vt_cursor(a, &x1, &y1); cui_vt_cursor(b, &x2, &y2);
    CHECK_EQ(x2, x1); CHECK_EQ(y2, y1);
    cui_vt_saved s1 = {0}, s2 = {0};
    cui_vt_save(a, 0, &s1); cui_vt_save(b, 0, &s2);
    CHECK_EQ(s2.len, s1.len);
    CHECK(s1.len == s2.len && !memcmp(s1.data, s2.data, s1.len));
    cui_vt_saved_free(&s1); cui_vt_saved_free(&s2);
}

static cui_vt *round_trip(const cui_vt *a, int max_lines, unsigned f, size_t *len) {
    int c, r;
    cui_vt_size(a, &c, &r);
    cui_vt_saved s = {0};
    size_t n = cui_vt_save(a, max_lines, &s);
    CHECK(n > 22);
    CHECK_EQ(n, s.len);
    if (len) *len = n;
    cui_vt *b = mk(c, r, 10000, f);
    CHECK(cui_vt_load(b, s.data, s.len));
    cui_vt_saved_free(&s);
    return b;
}

static void test_cells(void) {
    unsigned f = CUI_VT_FEATURE_REFLOW;
    cui_vt *a = mk(40, 8, 500, f);
    W(a, "plain text\r\n");
    W(a, "\x1b[1mbold\x1b[0m \x1b[2mdim\x1b[22m \x1b[3mitalic\x1b[0m \x1b[7minv\x1b[27m \x1b[9mstrike\x1b[0m \x1b[53mover\x1b[0m\r\n");
    W(a, "\x1b[4msingle\x1b[24m \x1b[4:2mdouble\x1b[0m \x1b[4:3mcurly\x1b[0m \x1b[4:4mdots\x1b[0m \x1b[4:5mdash\x1b[0m\r\n");
    W(a, "\x1b[31mred\x1b[0m \x1b[92mbright\x1b[0m \x1b[38;5;208m208\x1b[0m \x1b[48;5;17mbg17\x1b[0m \x1b[38;2;1;2;3;48;2;250;128;7mrgb\x1b[0m\r\n");
    W(a, "wide: \xe4\xb8\xad\xe6\x96\x87 emoji \xf0\x9f\x98\x80 done\r\n");
    W(a, "\x1b[44m\x1b[5X\x1b[0m gap\x1b[5Cafter \x1b[41m\x1b[K\x1b[0m\r\n");          /* blank cells with a background, a jump, a coloured line end */
    for (int i = 0; i < 30; i++) {
        char b[96];
        snprintf(b, sizeof b, "\x1b[3%dmline %02d\x1b[0m of the scrollback, long enough to wrap at forty columns %d\r\n", i % 8, i, i);
        W(a, b);
    }
    W(a, "PS C:\\> ");
    cui_vt *b = round_trip(a, 0, f, NULL);
    check_same(a, b);
    /* the cells by hand: a wide character and its second half, a true color, a blank cell with a background */
    int x, y;
    CHECK(cui_vt_cell_at(b, 0, 0, &(cui_vt_cell){0}));
    cui_vt_cursor(b, &x, &y);
    CHECK_STR(LINE(b, y), "PS C:\\> ");
    CHECK_EQ(x, 8);
    cui_vt_destroy(a); cui_vt_destroy(b);

    /* on the screen: exact cells (no scrollback involved) */
    a = mk(30, 6, 100, 0);
    W(a, "\x1b[38;2;10;20;30mrgb\x1b[0m\xe4\xb8\xad|\x1b[45m\x1b[3X\x1b[0m\x1b[3Cz");
    b = round_trip(a, 0, 0, NULL);
    cui_vt_cell c;
    CHECK(cui_vt_cell_at(b, 0, 0, &c)); CHECK_EQ(c.ch, 'r'); CHECK_EQ(c.fg, 0x1000000 | 0x0A141E);
    CHECK(cui_vt_cell_at(b, 3, 0, &c)); CHECK_EQ(c.ch, 0x4E2D);
    CHECK(cui_vt_cell_at(b, 4, 0, &c)); CHECK_EQ(c.ch, 0);                 /* its second half */
    CHECK(cui_vt_cell_at(b, 6, 0, &c)); CHECK_EQ(c.ch, 0); CHECK_EQ(c.bg, 5);
    CHECK(cui_vt_cell_at(b, 9, 0, &c)); CHECK_EQ(c.ch, 'z');
    check_same(a, b);
    cui_vt_destroy(a); cui_vt_destroy(b);
}

/* a wide character that did not fit at a row's end moved on, leaving a padding cell (REFLOW) */
static void test_wide_wrap(void) {
    unsigned f = CUI_VT_FEATURE_REFLOW;
    cui_vt *a = mk(10, 4, 100, f);
    W(a, "123456789\xe4\xb8\xad" "abc\r\n");                                /* the wide one goes to the next row */
    for (int i = 0; i < 6; i++) W(a, "0123456789ABCDEFGHIJ\r\n");             /* soft wraps, into the scrollback */
    cui_vt *b = round_trip(a, 0, f, NULL);
    check_same(a, b);
    cui_vt_destroy(a); cui_vt_destroy(b);
}

/* the cap: the newest rows only, scrollback and screen together */
static void test_cap(void) {
    cui_vt *a = mk(80, 24, 10000, CUI_VT_FEATURE_REFLOW);
    char b[64];
    for (int i = 0; i < 8000; i++) { snprintf(b, sizeof b, "row %05d\r\n", i); W(a, b); }
    W(a, "$ ");
    cui_vt_saved s = {0};
    cui_vt_save(a, 5000, &s);
    CHECK(s.len > 22);
    unsigned rows = s.data[10] | s.data[11] << 8 | s.data[12] << 16 | (unsigned)s.data[13] << 24;
    CHECK_EQ(rows, 5000);
    cui_vt *c = mk(80, 24, 10000, CUI_VT_FEATURE_REFLOW);
    CHECK(cui_vt_load(c, s.data, s.len));
    CHECK_EQ(cui_vt_scrollback_count(c) + 24, 5000);                         /* the screen is full: 24 rows on it */
    CHECK_STR(LINE(c, -cui_vt_scrollback_count(c)), "row 03001");             /* 8000 rows and "$ ": the oldest kept is 8001 - 5000 */
    CHECK_STR(LINE(c, 23), "$ ");
    CHECK_STR(LINE(c, 22), "row 07999");
    /* a smaller scrollback keeps what it can: the newest */
    cui_vt *d = mk(80, 24, 100, 0);
    CHECK(cui_vt_load(d, s.data, s.len));
    CHECK_EQ(cui_vt_scrollback_count(d), 100);
    CHECK_STR(LINE(d, 22), "row 07999");
    cui_vt_saved_free(&s);
    cui_vt_destroy(a); cui_vt_destroy(c); cui_vt_destroy(d);
}

static void test_links(void) {
    unsigned f = CUI_VT_FEATURE_LINKS;
    cui_vt *a = mk(40, 5, 100, f);
    W(a, "see \x1b]8;;https://example.com/a\x1b\\the docs\x1b]8;;\x1b\\ now\r\n");
    for (int i = 0; i < 8; i++) W(a, "filler\r\n");                          /* into the scrollback */
    W(a, "\x1b]8;id=x;file:///C:/x.txt\x07x.txt\x1b]8;;\x07");
    cui_vt *b = round_trip(a, 0, f, NULL);
    int sb = cui_vt_scrollback_count(b);
    const char *u = cui_vt_hyperlink(b, 5, -sb);
    CHECK(u && !strcmp(u, "https://example.com/a"));
    CHECK(cui_vt_hyperlink(b, 2, -sb) == NULL);
    CHECK(cui_vt_hyperlink(b, 13, -sb) == NULL);
    int x, y;
    cui_vt_cursor(b, &x, &y);
    u = cui_vt_hyperlink(b, 0, y);
    CHECK(u && !strcmp(u, "file:///C:/x.txt"));
    check_same(a, b);
    /* without the feature the text comes back, the links do not */
    cui_vt *c = round_trip(a, 0, 0, NULL);
    CHECK(cui_vt_hyperlink(c, 0, y) == NULL);
    CHECK_STR(LINE(c, y), "x.txt");
    cui_vt_destroy(a); cui_vt_destroy(b); cui_vt_destroy(c);
}

static void test_marks(void) {
    unsigned f = CUI_VT_FEATURE_MARKS;
    cui_vt *a = mk(60, 6, 200, f);
    W(a, "\x1b]133;A\x07PS> \x1b]133;B\x07" "dir\r\n\x1b]133;C\x07" "a\r\nb\r\n\x1b]133;D;0\x07");
    W(a, "\x1b]133;A\x07PS> \x1b]133;B\x07\x1b]633;E;echo a\\x3bb \\\\x\x07" "echo a;b \\x\r\n\x1b]133;C\x07" "a;b\r\n\x1b]133;D;3\x07");
    for (int i = 0; i < 6; i++) W(a, "more output\r\n");
    W(a, "\x1b]133;A\x07PS> \x1b]133;B\x07" "sleep 9\r\n\x1b]133;C\x07");                 /* still running when saved */
    CHECK_EQ(cui_vt_mark_count(a), 3);
    cui_vt *b = round_trip(a, 0, f, NULL);
    CHECK_EQ(cui_vt_mark_count(b), 3);
    cui_vt_mark m0, m1, m2, n0, n1;
    cui_vt_mark_get(a, 0, &m0); cui_vt_mark_get(a, 1, &m1);
    cui_vt_mark_get(b, 0, &n0); cui_vt_mark_get(b, 1, &n1); cui_vt_mark_get(b, 2, &m2);
    CHECK_EQ(n0.prompt_row, m0.prompt_row); CHECK_EQ(n0.cmd_row, m0.cmd_row); CHECK_EQ(n0.out_row, m0.out_row); CHECK_EQ(n0.end_row, m0.end_row);
    CHECK_EQ(n0.exit_code, 0); CHECK(n0.finished);
    CHECK_EQ(n1.exit_code, 3); CHECK_EQ(n1.end_row, m1.end_row);
    CHECK(m2.finished); CHECK_EQ(m2.exit_code, -1);                         /* the running one ended, exit unknown */
    char t[256];
    cui_vt_mark_command(b, 0, t, sizeof t); CHECK_STR(t, "dir");
    cui_vt_mark_command(b, 1, t, sizeof t); CHECK_STR(t, "echo a;b \\x");      /* 633;E's text, escaped and back */
    cui_vt_mark_output(b, 0, t, sizeof t); CHECK_STR(t, "a\nb");
    /* the run time comes back (the clock is the screen's: ended at the load) */
    cui_vt *c = mk(60, 6, 200, f);
    W(c, "\x1b]133;A\x07$ \x1b]133;B\x07x\r\n\x1b]133;C\x07");
    double t0 = cui_vt_now_ms();
    while (cui_vt_now_ms() - t0 < 30) {}
    W(c, "\x1b]133;D;0\x07$ ");
    cui_vt *d = round_trip(c, 0, f, NULL);
    cui_vt_mark k;
    cui_vt_mark_get(d, 0, &k);
    CHECK(k.end_ms - k.start_ms >= 29 && k.end_ms - k.start_ms < 2000);
    CHECK(k.end_ms <= cui_vt_now_ms());
    cui_vt_destroy(a); cui_vt_destroy(b); cui_vt_destroy(c); cui_vt_destroy(d);
}

/* the main screen is saved while a full screen program shows the alternate one */
static void test_alt(void) {
    cui_vt *a = mk(30, 6, 100, 0);
    W(a, "shell line 1\r\nshell line 2\r\n$ vim");
    W(a, "\x1b[?1049h\x1b[Hvim's own screen");
    cui_vt_saved s = {0};
    cui_vt_save(a, 0, &s);
    cui_vt *b = mk(30, 6, 100, 0);
    CHECK(cui_vt_load(b, s.data, s.len));
    CHECK_STR(LINE(b, 0), "shell line 1");
    CHECK_STR(LINE(b, 2), "$ vim");
    CHECK(!cui_vt_alt_screen(b));
    int x, y;
    cui_vt_cursor(b, &x, &y); CHECK_EQ(x, 5); CHECK_EQ(y, 2);
    cui_vt_saved_free(&s);
    cui_vt_destroy(a); cui_vt_destroy(b);
}

/* another width: the rows wrap again as the stream is written */
static void test_other_width(void) {
    cui_vt *a = mk(80, 10, 100, CUI_VT_FEATURE_REFLOW);
    W(a, "0123456789012345678901234567890123456789012345678901234567890123456789\r\nshort\r\n$ ");
    cui_vt_saved s = {0};
    cui_vt_save(a, 0, &s);
    cui_vt *b = mk(40, 10, 100, CUI_VT_FEATURE_REFLOW);
    CHECK(cui_vt_load(b, s.data, s.len));
    CHECK_STR(LINE(b, 0), "0123456789012345678901234567890123456789");
    CHECK_STR(LINE(b, 1), "012345678901234567890123456789");
    CHECK_STR(LINE(b, 2), "short");
    cui_vt_resize(b, 80, 10);                                               /* the wrap is a soft one: it joins again */
    CHECK_STR(LINE(b, 0), "0123456789012345678901234567890123456789012345678901234567890123456789");
    cui_vt_saved_free(&s);
    cui_vt_destroy(a); cui_vt_destroy(b);
}

static bool untouched(const cui_vt *v) {
    int x, y;
    cui_vt_cursor(v, &x, &y);
    return x == 0 && y == 0 && !strcmp(LINE(v, 0), "") && cui_vt_scrollback_count(v) == 0;
}

static void test_bad(void) {
    cui_vt *a = mk(40, 5, 100, 0);
    W(a, "hello\r\nworld");
    cui_vt_saved s = {0};
    cui_vt_save(a, 0, &s);
    cui_vt *b = mk(40, 5, 100, CUI_VT_FEATURE_MARKS);
    unsigned char *d = malloc(s.len + 64);
    memcpy(d, s.data, s.len);
    CHECK(!cui_vt_load(b, d, s.len - 1)); CHECK(untouched(b));               /* cut off */
    CHECK(!cui_vt_load(b, d, 10)); CHECK(untouched(b));
    CHECK(!cui_vt_load(b, d, 0));
    CHECK(!cui_vt_load(b, NULL, s.len));
    d[4] = 2; CHECK(!cui_vt_load(b, d, s.len)); CHECK(untouched(b)); d[4] = 1;   /* a newer version */
    d[0] = 'X'; CHECK(!cui_vt_load(b, d, s.len)); d[0] = 'C';                 /* not ours */
    d[14] ^= 0x40; CHECK(!cui_vt_load(b, d, s.len)); d[14] ^= 0x40;           /* the length does not add up */
    CHECK(cui_vt_load(b, d, s.len));                                          /* put back: it loads */
    CHECK_STR(LINE(b, 0), "hello");
    /* streams the saver never writes: refused whole, nothing written */
    static const char *const evil[] = { "\x1b[6n", "ring\x07", "\x1b]2;title\x1b\\", "\x1b]52;c;aGk=\x1b\\", "\x1b[c", "\x1b]9;4;1;50\x1b\\",
                                        "\x1b" "c", "\x1b]133;A\x07", "\x1b]7;file:///C:/\x1b\\", "\x1b[?1049h", "\x1bP0;0;0q#0~\x1b\\", "tab\there" };
    for (size_t i = 0; i < sizeof evil / sizeof evil[0]; i++) {
        cui_vt *c = mk(40, 5, 100, CUI_VT_FEATURE_MARKS | CUI_VT_FEATURE_QUERIES);
        size_t n = strlen(evil[i]);
        unsigned char *e = calloc(1, 22 + n);
        memcpy(e, "CUVT\x01", 5);
        e[6] = 40; e[8] = 5; e[10] = 1; e[14] = (unsigned char)n;
        memcpy(e + 22, evil[i], n);
        CHECK(!cui_vt_load(c, e, 22 + n));
        CHECK(untouched(c));
        char r[64];
        CHECK_EQ(cui_vt_reply(c, r, sizeof r), 0);
        CHECK_EQ(cui_vt_bell_count(c), 0);
        CHECK_STR(cui_vt_title(c), "");
        free(e);
        cui_vt_destroy(c);
    }
    /* a flipped byte somewhere in the stream: refused or loaded, never a crash (the app's own check is the file's CRC) */
    for (size_t i = 22; i < s.len; i++) {
        memcpy(d, s.data, s.len);
        d[i] ^= 0x1B;
        cui_vt *c = mk(40, 5, 100, CUI_VT_FEATURE_MARKS);
        cui_vt_load(c, d, s.len);
        cui_vt_destroy(c);
    }
    free(d);
    cui_vt_saved_free(&s);
    cui_vt_destroy(a); cui_vt_destroy(b);
}

/* several screens appended to one buffer, then the empty screen */
static void test_append(void) {
    cui_vt *a = mk(20, 3, 10, 0), *b = mk(20, 3, 10, 0);
    W(a, "first"); W(b, "second");
    cui_vt_saved s = {0};
    size_t n1 = cui_vt_save(a, 0, &s), n2 = cui_vt_save(b, 0, &s);
    CHECK_EQ(s.len, n1 + n2);
    cui_vt *c = mk(20, 3, 10, 0), *d = mk(20, 3, 10, 0);
    CHECK(cui_vt_load(c, s.data, n1)); CHECK(cui_vt_load(d, s.data + n1, n2));
    CHECK_STR(LINE(c, 0), "first"); CHECK_STR(LINE(d, 0), "second");
    cui_vt *e = mk(20, 3, 10, 0), *f = mk(20, 3, 10, 0);
    cui_vt_saved z = {0};
    CHECK(cui_vt_save(e, 5000, &z) > 0);
    CHECK(cui_vt_load(f, z.data, z.len));
    CHECK(untouched(f));
    cui_vt_saved_free(&s); cui_vt_saved_free(&z);
    cui_vt_destroy(a); cui_vt_destroy(b); cui_vt_destroy(c); cui_vt_destroy(d); cui_vt_destroy(e); cui_vt_destroy(f);
}

/* 5000 rows of a coloured log at 120 columns: how long a save and a load take, and how big the stream is */
static void test_speed(void) {
    unsigned f = CUI_VT_FEATURE_REFLOW | CUI_VT_FEATURE_MARKS | CUI_VT_FEATURE_LINKS;
    cui_vt *a = mk(120, 30, 5000, f);
    char b[256];
    for (int i = 0; i < 5200; i++) {
        snprintf(b, sizeof b, "\x1b[32m%05d\x1b[0m \x1b[1;34minfo\x1b[0m  src/module_%d.c:%d: the thing happened with value %d and \x1b[33mwarning\x1b[0m %x\r\n",
                 i, i % 37, i * 7 % 900, i * 13, i * 2654435761u);
        W(a, b);
    }
    cui_vt_saved s = {0};
    double t0 = now_ms();
    int reps = 20;
    for (int i = 0; i < reps; i++) { s.len = 0; cui_vt_save(a, 5000, &s); }
    double save_ms = (now_ms() - t0) / reps;
    cui_vt *c = mk(120, 30, 5000, f);
    t0 = now_ms();
    for (int i = 0; i < reps; i++) { cui_vt_destroy(c); c = mk(120, 30, 5000, f); cui_vt_load(c, s.data, s.len); }
    double load_ms = (now_ms() - t0) / reps;
    printf("  5000 rows x 120 columns: save %.2f ms, load %.2f ms, %zu bytes (%.1f a row)\n", save_ms, load_ms, s.len, (double)s.len / 5000);
    CHECK(save_ms < 50 && load_ms < 100);
    cui_vt_saved_free(&s);
    cui_vt_destroy(a); cui_vt_destroy(c);
}

int main(void) {
    test_cells();
    test_wide_wrap();
    test_cap();
    test_links();
    test_marks();
    test_alt();
    test_other_width();
    test_bad();
    test_append();
    test_speed();
    printf("cui_test_vt_save: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
