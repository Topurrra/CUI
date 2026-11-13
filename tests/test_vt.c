/* Tests for the VT terminal: the screen (text, cursor, erase, colors, scrolling, regions, the alternate screen,
 * scrollback limits, reports, UTF-8 split across writes, resize) and the view (grid size, the bytes keys turn
 * into, selection, paste, focus). Runs headless with the software renderer. Prints the parse speed too. */
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
static const char *row(const cui_vt *v, int y) { static char b[4][1100]; static int i; i = (i + 1) & 3; cui_vt_line_text(v, y, b[i], sizeof b[i]); return b[i]; }

static void test_text(void) {
    cui_vt *v = cui_vt_create(10, 4, 100);
    W(v, "hello\r\nworld");
    CHECK_STR(row(v, 0), "hello");
    CHECK_STR(row(v, 1), "world");
    int x, y; cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 5); CHECK_EQ(y, 1);
    W(v, "\rX");                                  /* carriage return then overwrite */
    CHECK_STR(row(v, 1), "Xorld");
    W(v, "\n");                                    /* line feed keeps the column */
    cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 1); CHECK_EQ(y, 2);
    W(v, "\x08\x08Z");                             /* backspace moves, does not erase */
    CHECK_STR(row(v, 2), "Z");
    W(v, "\r\n\tT");                               /* tab to column 8 */
    CHECK_STR(row(v, 3), "        T");
    cui_vt_destroy(v);
}

static void test_wrap(void) {
    cui_vt *v = cui_vt_create(5, 3, 100);
    W(v, "abcdefgh");
    CHECK_STR(row(v, 0), "abcde");
    CHECK_STR(row(v, 1), "fgh");
    W(v, "\r\n12345");                             /* exactly at the margin: the wrap waits for the next character */
    int x, y; cui_vt_cursor(v, &x, &y);
    CHECK_EQ(y, 2); CHECK_EQ(x, 4);
    W(v, "6");
    CHECK_EQ(cui_vt_scrollback_count(v), 1);       /* the screen scrolled once */
    CHECK_STR(row(v, 1), "12345");
    CHECK_STR(row(v, 2), "6");
    CHECK_STR(row(v, -1), "abcde");
    cui_vt_destroy(v);
}

static void test_cursor_erase(void) {
    cui_vt *v = cui_vt_create(10, 5, 100);
    W(v, "\x1b[2J\x1b[H");
    W(v, "\x1b[3;4Hx");
    CHECK_STR(row(v, 2), "   x");
    W(v, "\x1b[Hab\x1b[2Cc");                       /* cursor right 2 */
    CHECK_STR(row(v, 0), "ab  c");
    W(v, "\x1b[1;1H0123456789\x1b[1;4H\x1b[K");     /* erase to end of line */
    CHECK_STR(row(v, 0), "012");
    W(v, "\x1b[1;1H0123456789\x1b[1;4H\x1b[1K");    /* erase from start through the cursor */
    CHECK_STR(row(v, 0), "    456789");
    W(v, "\x1b[1;1H0123456789\x1b[1;3H\x1b[2P");    /* delete 2 characters */
    CHECK_STR(row(v, 0), "01456789");
    W(v, "\x1b[1;1H0123456789\x1b[1;3H\x1b[2@");    /* insert 2 blanks */
    CHECK_STR(row(v, 0), "01  234567");
    W(v, "\x1b[1;1H0123456789\x1b[1;3H\x1b[3X");    /* erase 3 characters */
    CHECK_STR(row(v, 0), "01   56789");
    W(v, "\x1b[5;1Hlast\x1b[2J");
    CHECK_STR(row(v, 4), "");
    W(v, "\x1b[3;3H\x1b[A\x1b[A\x1b[A");            /* stops at the top */
    int x, y; cui_vt_cursor(v, &x, &y);
    CHECK_EQ(y, 0); CHECK_EQ(x, 2);
    W(v, "\x1b[99;99H");
    cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 9); CHECK_EQ(y, 4);
    W(v, "\x1b[2;3H\x1b[s\x1b[5;5H\x1b[u");
    cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 2); CHECK_EQ(y, 1);
    cui_vt_destroy(v);
}

static void test_colors(void) {
    cui_vt *v = cui_vt_create(20, 3, 100);
    cui_vt_cell c;
    W(v, "\x1b[31mR\x1b[0mN\x1b[1;32mG\x1b[38;5;200mP\x1b[48;2;1;2;3mT\x1b[0m\x1b[7mI\x1b[0m");
    CHECK(cui_vt_cell_at(v, 0, 0, &c)); CHECK_EQ(c.ch, 'R'); CHECK_EQ(c.fg, 1); CHECK_EQ(c.bg, -1);
    CHECK(cui_vt_cell_at(v, 1, 0, &c)); CHECK_EQ(c.fg, -1); CHECK_EQ(c.attrs, 0);
    CHECK(cui_vt_cell_at(v, 2, 0, &c)); CHECK_EQ(c.fg, 2); CHECK(c.attrs & CUI_VT_BOLD);
    CHECK(cui_vt_cell_at(v, 3, 0, &c)); CHECK_EQ(c.fg, 200);
    CHECK(cui_vt_cell_at(v, 4, 0, &c)); CHECK_EQ(c.bg, 0x1010203); CHECK_EQ(c.fg, 200);
    CHECK(cui_vt_cell_at(v, 5, 0, &c)); CHECK(c.attrs & CUI_VT_INVERSE); CHECK_EQ(c.bg, -1);
    W(v, "\x1b[44m\x1b[K\x1b[0m");                  /* erasing keeps the pen's background */
    CHECK(cui_vt_cell_at(v, 10, 0, &c)); CHECK_EQ(c.bg, 4); CHECK_EQ(c.ch, 0);
    W(v, "\x1b[91;102mx");
    CHECK(cui_vt_cell_at(v, 6, 0, &c)); CHECK_EQ(c.fg, 9); CHECK_EQ(c.bg, 10);
    /* many true colors: the table is bounded, nothing breaks */
    for (int i = 0; i < 3000; i++) { char b[40]; snprintf(b, sizeof b, "\x1b[38;2;%d;%d;%dmq", i & 255, (i >> 3) & 255, (i * 7) & 255); W(v, b); }
    cui_vt_destroy(v);
}

static void test_scroll(void) {
    cui_vt *v = cui_vt_create(8, 4, 100);
    for (int i = 0; i < 10; i++) { char b[16]; snprintf(b, sizeof b, "line%d\r\n", i); W(v, b); }
    CHECK_STR(row(v, 0), "line7");
    CHECK_STR(row(v, 2), "line9");
    CHECK_STR(row(v, 3), "");
    CHECK_EQ(cui_vt_scrollback_count(v), 7);
    CHECK_STR(row(v, -1), "line6");
    CHECK_STR(row(v, -7), "line0");
    CHECK_STR(row(v, -8), "");
    /* scroll region: only the region moves, nothing goes to the scrollback */
    W(v, "\x1b[2J\x1b[H1\r\n2\r\n3\r\n4");
    int before = cui_vt_scrollback_count(v);
    W(v, "\x1b[2;3r\x1b[3;1H\n");
    CHECK_STR(row(v, 0), "1");
    CHECK_STR(row(v, 1), "3");
    CHECK_STR(row(v, 2), "");
    CHECK_STR(row(v, 3), "4");
    CHECK_EQ(cui_vt_scrollback_count(v), before);
    W(v, "\x1b[2;1H\x1bM");                         /* reverse index at the region top scrolls it down */
    CHECK_STR(row(v, 1), "");
    CHECK_STR(row(v, 2), "3");
    /* insert and delete lines */
    W(v, "\x1b[r\x1b[2J\x1b[Ha\r\nb\r\nc\r\nd");
    W(v, "\x1b[2;1H\x1b[L");
    CHECK_STR(row(v, 1), ""); CHECK_STR(row(v, 2), "b"); CHECK_STR(row(v, 3), "c");
    W(v, "\x1b[2;1H\x1b[2M");
    CHECK_STR(row(v, 1), "c"); CHECK_STR(row(v, 2), ""); CHECK_STR(row(v, 3), "");
    CHECK_STR(row(v, 0), "a");
    W(v, "\x1b[H\x1b[M");                           /* delete at the top of the main screen: no scrollback */
    CHECK_STR(row(v, 0), "c");
    cui_vt_destroy(v);
}

static void test_alt_screen(void) {
    cui_vt *v = cui_vt_create(10, 3, 100);
    W(v, "main\x1b[2;3H");
    W(v, "\x1b[?1049h");
    CHECK(cui_vt_alt_screen(v));
    CHECK_STR(row(v, 0), "");
    W(v, "\x1b[Halt text\r\nmore\r\nmore\r\nmore");   /* scrolls: not into the scrollback */
    CHECK_EQ(cui_vt_scrollback_count(v), 0);
    W(v, "\x1b[?1049l");
    CHECK(!cui_vt_alt_screen(v));
    CHECK_STR(row(v, 0), "main");
    int x, y; cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 2); CHECK_EQ(y, 1);
    W(v, "\x1b[?25l\x1b[?25h\x1b[?1h\x1b[?2004h");
    CHECK(cui_vt_app_cursor(v)); CHECK(cui_vt_bracketed_paste(v));
    W(v, "\x1b[?1l\x1b[?2004l");
    CHECK(!cui_vt_app_cursor(v)); CHECK(!cui_vt_bracketed_paste(v));
    cui_vt_destroy(v);
}

static void test_reports(void) {
    cui_vt *v = cui_vt_create(20, 5, 100);
    char r[64];
    W(v, "\x1b[3;7H\x1b[6n");
    size_t n = cui_vt_reply(v, r, sizeof r);
    r[n] = 0;
    CHECK_STR(r, "\x1b[3;7R");
    CHECK_EQ(cui_vt_reply(v, r, sizeof r), 0);
    W(v, "\x1b[c");
    n = cui_vt_reply(v, r, sizeof r); r[n] = 0;
    CHECK_STR(r, "\x1b[?1;0c");
    W(v, "\x1b]0;my title\x07\x1b]2;second\x1b\\");
    CHECK_STR(cui_vt_title(v), "second");
    W(v, "\x1b]0;first\x07");
    CHECK_STR(cui_vt_title(v), "first");
    /* unknown sequences, strings and cut sequences do not print anything */
    W(v, "\x1bP1$r\x1b\\\x1b_apc\x1b\\\x1b[?9001h\x1b[?1004h\x1b(B\x1b=\x1b>ok");
    CHECK_STR(row(v, 2), "      ok");
    W(v, "\x1b[3");
    W(v, "1mX");
    cui_vt_cell c;
    CHECK(cui_vt_cell_at(v, 8, 2, &c)); CHECK_EQ(c.ch, 'X'); CHECK_EQ(c.fg, 1);
    cui_vt_destroy(v);
}

static void test_utf8(void) {
    cui_vt *v = cui_vt_create(12, 3, 100);
    const char *s = "h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80!";    /* h e-acute llo euro emoji ! */
    for (size_t i = 0; i < strlen(s); i++) cui_vt_write(v, s + i, 1);  /* one byte at a time */
    CHECK_STR(row(v, 0), "h\xc3\xa9llo \xe2\x82\xac \xf0\x9f\x98\x80!");
    cui_vt_cell c;
    CHECK(cui_vt_cell_at(v, 1, 0, &c)); CHECK_EQ(c.ch, 0xE9);
    CHECK(cui_vt_cell_at(v, 8, 0, &c)); CHECK_EQ(c.ch, 0x1F600);       /* wide: two cells */
    CHECK(cui_vt_cell_at(v, 10, 0, &c)); CHECK_EQ(c.ch, '!');
    W(v, "\r\n\xe6\x97\xa5\xe6\x9c\xac");                              /* two wide characters = 4 cells */
    int x, y; cui_vt_cursor(v, &x, &y);
    CHECK_EQ(x, 4);
    W(v, "\xff\xc3(");                                                  /* invalid bytes become U+FFFD, not a crash */
    cui_vt_destroy(v);
}

static void test_resize_and_limits(void) {
    cui_vt *v = cui_vt_create(10, 5, 50);
    W(v, "one\r\ntwo\r\nthree");
    cui_vt_resize(v, 20, 8);
    CHECK_STR(row(v, 0), "one");
    CHECK_STR(row(v, 2), "three");
    int c, r; cui_vt_size(v, &c, &r);
    CHECK_EQ(c, 20); CHECK_EQ(r, 8);
    cui_vt_resize(v, 6, 2);                         /* shrinking: the cursor row stays, rows above go to the scrollback */
    CHECK_STR(row(v, 1), "three");
    CHECK_STR(row(v, 0), "two");
    CHECK_STR(row(v, -1), "one");
    cui_vt_resize(v, 6, 2);
    cui_vt_destroy(v);

    /* the scrollback has a fixed size: 100k lines through 1000 lines of room */
    v = cui_vt_create(80, 24, 1000);
    size_t mem = cui_vt_memory(v);
    for (int i = 0; i < 100000; i++) { char b[64]; snprintf(b, sizeof b, "line number %d with some text\r\n", i); W(v, b); }
    CHECK_EQ(cui_vt_memory(v), mem);
    CHECK_EQ(cui_vt_scrollback_count(v), 1000);
    char want[64], got[128];
    snprintf(want, sizeof want, "line number %d with some text", 100000 - 24);
    cui_vt_line_text(v, -1, got, sizeof got);
    CHECK_STR(got, want);
    snprintf(want, sizeof want, "line number %d with some text", 100000 - 1023);
    cui_vt_line_text(v, -1000, got, sizeof got);
    CHECK_STR(got, want);
    /* long lines use the ring up: older lines drop, the newest stay right */
    for (int i = 0; i < 3000; i++) { char b[300]; memset(b, 'a' + i % 26, 200); b[200] = 0; strcat(b, "\r\n"); W(v, b); }
    cui_vt_line_text(v, -1, got, sizeof got);
    CHECK_EQ(strlen(got), 80);                       /* wrapped at 80 columns */
    CHECK_EQ(cui_vt_memory(v), mem);
    cui_vt_destroy(v);
}

static void test_speed(void) {
    cui_vt *v = cui_vt_create(120, 40, 5000);
    size_t cap = 8 << 20;
    char *buf = (char *)malloc(cap);
    size_t n = 0;
    int i = 0;
    while (n + 120 < cap) n += (size_t)snprintf(buf + n, cap - n, "\x1b[32m%7d\x1b[0m  Directory of C:\\Windows\\System32\\drivers\\etc  some more text here\r\n", i++);
    double t0 = (double)clock() / CLOCKS_PER_SEC;
    for (int k = 0; k < 8; k++) cui_vt_write(v, buf, n);
    double dt = (double)clock() / CLOCKS_PER_SEC - t0;
    printf("  parse speed: %.0f MB/s (%zu MB in %.0f ms, %d lines)\n", (double)n * 8 / dt / 1e6, n * 8 >> 20, dt * 1000, i * 8);
    free(buf);
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
typedef struct { cui_vt *vt; cui_vt_result r; bool focus; float w, h; } scene;

static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { s->r = cui_vt_view(ui, "t", s->vt, .focus = s->focus); }
    cui_end_frame(ui);
    s->focus = false;
}

static void key(cui_ctx *ui, scene *s, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    build(ui, s);
    cui_input_key(ui, k, false, mods);
}

static void set_clip(void *user, const char *text) { snprintf((char *)user, 256, "%s", text); }
static const char *get_clip(void *user) { return (const char *)user; }

static bool got(const scene *s, const char *bytes) { return s->r.input_len == (int)strlen(bytes) && !memcmp(s->r.input, bytes, (size_t)s->r.input_len); }

static void test_view(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    scene s = { cui_vt_create(80, 24, 1000), { 0 }, true, 640, 400 };
    for (int i = 0; i < 3; i++) build(ui, &s);
    CHECK(s.r.cols > 40 && s.r.cols < 100);
    CHECK(s.r.rows > 10 && s.r.rows < 30);
    int c, r; cui_vt_size(s.vt, &c, &r);
    CHECK_EQ(c, s.r.cols); CHECK_EQ(r, s.r.rows);
    CHECK(s.r.focused);

    cui_input_text(ui, "ab");
    build(ui, &s);
    CHECK(got(&s, "ab"));
    build(ui, &s);
    CHECK_EQ(s.r.input_len, 0);
    key(ui, &s, CUI_KEY_ENTER, 0); CHECK(got(&s, "\r"));
    key(ui, &s, CUI_KEY_TAB, 0); CHECK(got(&s, "\t"));
    key(ui, &s, CUI_KEY_TAB, CUI_MOD_SHIFT); CHECK(got(&s, "\x1b[Z"));
    key(ui, &s, CUI_KEY_ESCAPE, 0); CHECK(got(&s, "\x1b"));
    key(ui, &s, CUI_KEY_BACKSPACE, 0); CHECK(got(&s, "\x7f"));
    key(ui, &s, CUI_KEY_UP, 0); CHECK(got(&s, "\x1b[A"));
    key(ui, &s, CUI_KEY_LEFT, CUI_MOD_CTRL); CHECK(got(&s, "\x1b[1;5D"));
    key(ui, &s, CUI_KEY_DELETE, 0); CHECK(got(&s, "\x1b[3~"));
    key(ui, &s, CUI_KEY_PAGE_UP, 0); CHECK(got(&s, "\x1b[5~"));
    key(ui, &s, CUI_KEY_F5, 0); CHECK(got(&s, "\x1b[15~"));
    key(ui, &s, CUI_KEY_F1, 0); CHECK(got(&s, "\x1bOP"));
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL); CHECK(got(&s, "\x03"));           /* nothing selected: interrupt */
    key(ui, &s, CUI_KEY_D, CUI_MOD_CTRL); CHECK(got(&s, "\x04"));
    key(ui, &s, CUI_KEY_L, CUI_MOD_CTRL); CHECK(got(&s, "\x0c"));
    key(ui, &s, CUI_KEY_B, CUI_MOD_ALT); CHECK(got(&s, "\x1b" "b"));
    key(ui, &s, CUI_KEY_SPACE, CUI_MOD_CTRL); CHECK(s.r.input_len == 1 && s.r.input[0] == 0);
    W(s.vt, "\x1b[?1h");
    key(ui, &s, CUI_KEY_UP, 0); CHECK(got(&s, "\x1bOA"));                  /* application cursor keys */
    W(s.vt, "\x1b[?1l");

    /* scrollback and the wheel, with the program output below */
    for (int i = 0; i < 100; i++) { char b[32]; snprintf(b, sizeof b, "row %d\r\n", i); W(s.vt, b); }
    build(ui, &s);
    cui_input_scroll(ui, 0, 5);
    build(ui, &s); build(ui, &s);
    key(ui, &s, CUI_KEY_PAGE_UP, CUI_MOD_SHIFT);
    CHECK_EQ(s.r.input_len, 0);                                              /* Shift+PageUp scrolls, sends nothing */
    key(ui, &s, CUI_KEY_A, 0);                                               /* typing goes back to the live screen */
    /* selecting: a double click takes the word, Ctrl+C then copies it instead of interrupting, a right click pastes */
    {
        static char clip[256];
        clip[0] = 0;
        cui_platform_iface pf = { 0 };
        pf.set_clipboard = set_clip; pf.get_clipboard = get_clip; pf.user = clip;
        cui_set_platform(ui, &pf);
        cui_input_text(ui, "x");                  /* typing returns to the live screen */
        build(ui, &s);
        W(s.vt, "\x1b[2J\x1b[Hhello world");
        build(ui, &s);
        float cw = 13 * 0.6f;                      /* about; the click lands inside the word either way */
        cui_input_mouse_move(ui, 10 + 2 * cw, 6 + 8);
        build(ui, &s);
        for (int i = 0; i < 2; i++) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s); cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s); }
        key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
        CHECK(s.r.copied);
        CHECK_EQ(s.r.input_len, 0);
        CHECK_STR(clip, "hello");
        key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);      /* the selection is gone: now it is the interrupt */
        CHECK(got(&s, "\x03"));
        snprintf(clip, sizeof clip, "%s", "pasted text");
        key(ui, &s, CUI_KEY_V, CUI_MOD_CTRL | CUI_MOD_SHIFT);
        CHECK(s.r.paste && s.r.paste_len == 11 && !memcmp(s.r.paste, "pasted text", 11));
        CHECK(!s.r.paste_bracketed);
        W(s.vt, "\x1b[?2004h");
        key(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
        CHECK(s.r.paste_bracketed);
    }
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_text();
    test_wrap();
    test_cursor_erase();
    test_colors();
    test_scroll();
    test_alt_screen();
    test_reports();
    test_utf8();
    test_resize_and_limits();
    test_speed();
    test_view();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
