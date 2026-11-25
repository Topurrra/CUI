/* Tests for the VT screen's command marks, progress and copy requests (CUI_VT_FEATURE_MARKS): OSC 133 and 633 parsed whole
 * and split, the unknown variants skipped, rows through scrolling, scroll regions, the alternate screen, a resize and REFLOW,
 * marks dropped with their rows, the command and output texts, OSC 9;4, OSC 52 (base64, long ones, the size cap, queries
 * ignored), nothing kept without the feature, and the view's ticks: drawn, clicked, a mark selected and copied. Prints the
 * memory a mark takes and the parse speed with marks on and off. Runs headless with the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

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

#define A "\x1b]133;A\x1b\\"
#define B "\x1b]133;B\x07"
#define C "\x1b]133;C\x07"
#define D0 "\x1b]133;D;0\x07"
#define D1 "\x1b]133;D;1\x07"

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static void W1(cui_vt *v, const char *s) { for (; *s; s++) cui_vt_write(v, s, 1); }     /* a byte at a time */
static cui_vt_mark MK(const cui_vt *v, int i) { cui_vt_mark m; memset(&m, 0x7f, sizeof m); cui_vt_mark_get(v, i, &m); return m; }
static char g_t[8192];
static const char *OUT(const cui_vt *v, int i) { cui_vt_mark_output(v, i, g_t, sizeof g_t); return g_t; }
static const char *CMD(const cui_vt *v, int i) { cui_vt_mark_command(v, i, g_t, sizeof g_t); return g_t; }
static cui_vt *mkvt(int cols, int rows, int sb, unsigned extra) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, CUI_VT_FEATURE_MARKS | extra); return v; }

static double now_ms(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

/* a whole command as a shell with marks prints it */
static void run(cui_vt *v, const char *cmd, const char *out, int code) {
    char b[4096];
    snprintf(b, sizeof b, A "PS> " B "%s\r\n" C "%s\x1b]133;D;%d\x07", cmd, out, code);
    W(v, b);
}

static void test_off(void) {
    cui_vt *v = cui_vt_create(40, 10, 100);
    W(v, A "PS> " B "ls\r\n" C "x\r\n" D0 "\x1b]9;4;1;50\x07\x1b]52;c;aGk=\x07" "after");
    char t[64];
    CHECK_EQ(cui_vt_mark_count(v), 0);
    cui_vt_line_text(v, 0, t, sizeof t); CHECK_STR(t, "PS> ls");          /* the sequences are skipped as before */
    cui_vt_line_text(v, 2, t, sizeof t); CHECK_STR(t, "after");
    int pct = 7;
    CHECK_EQ(cui_vt_progress(v, &pct), CUI_VT_PROGRESS_NONE); CHECK_EQ(pct, 0);
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);
    CHECK_EQ(cui_vt_mark_seq(v), 0);
    CHECK_EQ(cui_vt_mark_at_row(v, 0), -1);
    CHECK_EQ(cui_vt_mark_near(v, 5, -1), -1);
    cui_vt_mark m;
    CHECK(!cui_vt_mark_get(v, 0, &m));
    CHECK_EQ(cui_vt_mark_output(v, 0, t, sizeof t), 0); CHECK_STR(t, "");
    size_t base = cui_vt_memory(v);
    cui_vt_set_features(v, CUI_VT_FEATURE_MARKS);
    run(v, "a", "b\r\n", 0);
    CHECK_EQ(cui_vt_mark_count(v), 1);
    CHECK(cui_vt_memory(v) > base);
    cui_vt_set_features(v, 0);                                             /* off frees everything */
    CHECK_EQ(cui_vt_mark_count(v), 0);
    CHECK_EQ(cui_vt_memory(v), base);
    cui_vt_destroy(v);
}

static void test_parse(void) {
    cui_vt *v = mkvt(40, 10, 100, 0);
    unsigned s0 = cui_vt_mark_seq(v);
    W(v, "\x1b]133;D;0\x07");                                              /* a D with no command open (the first prompt) */
    CHECK_EQ(cui_vt_mark_count(v), 0);
    W(v, "PS> ls\r\n");
    double t0 = cui_vt_now_ms();
    W(v, A "PS> " B "dir\r\n" C "a\r\nb\r\n" "\x1b]133;D;3\x07");
    CHECK_EQ(cui_vt_mark_count(v), 1);
    CHECK(cui_vt_mark_seq(v) != s0);
    cui_vt_mark m = MK(v, 0);
    CHECK_EQ(m.prompt_row, 1); CHECK_EQ(m.cmd_row, 1); CHECK_EQ(m.out_row, 2); CHECK_EQ(m.end_row, 4);
    CHECK_EQ(m.exit_code, 3); CHECK(m.finished);
    CHECK(m.start_ms >= t0 && m.end_ms >= m.start_ms && m.end_ms <= cui_vt_now_ms());
    CHECK_STR(CMD(v, 0), "dir");
    CHECK_STR(OUT(v, 0), "a\nb");
    CHECK_EQ(cui_vt_mark_at_row(v, 0), -1);
    CHECK_EQ(cui_vt_mark_at_row(v, 1), 0);
    CHECK_EQ(cui_vt_mark_at_row(v, 3), 0);
    CHECK_EQ(cui_vt_mark_at_row(v, 4), -1);                                /* the row of D is the next prompt's */
    W(v, A "PS> " B "sleep");                                              /* typing: no C yet */
    CHECK_EQ(cui_vt_mark_count(v), 2);
    m = MK(v, 1);
    CHECK_EQ(m.prompt_row, 4); CHECK_EQ(m.out_row, INT_MIN); CHECK_EQ(m.end_row, INT_MIN); CHECK_EQ(m.exit_code, -1); CHECK(!m.finished);
    CHECK(m.start_ms > 0); CHECK_EQ(m.end_ms, 0);
    CHECK_STR(CMD(v, 1), "sleep");
    CHECK_EQ(cui_vt_mark_at_row(v, 4), 1);
    CHECK_EQ(cui_vt_mark_at_row(v, 9), 1);                                 /* a running one: to the screen's end */
    W(v, "\r\n" C "zz");
    CHECK_STR(OUT(v, 1), "zz");                                            /* running: C to the cursor */
    W(v, "\r\n" D0);

    /* the VS Code variants: 633;A..D, E with the command line (escapes), D with no code */
    W(v, "\x1b]633;A\x07$ \x1b]633;B\x07" "echo hi\r\n" "\x1b]633;E;echo\\x3b hi \\\\ there\x07" "\x1b]633;C\x07hi\r\n\x1b]633;D\x07");
    CHECK_EQ(cui_vt_mark_count(v), 3);
    m = MK(v, 2);
    CHECK(m.finished); CHECK_EQ(m.exit_code, -1);
    CHECK_STR(CMD(v, 2), "echo; hi \\ there");
    CHECK_STR(OUT(v, 2), "hi");
    /* 133;D with no code, a code that is no number, a negative one */
    W(v, A "> " B "x\r\n" C "\x1b]133;D\x07");
    CHECK_EQ(MK(v, 3).exit_code, -1); CHECK(MK(v, 3).finished);
    W(v, A "> " B "x\r\n" C "\x1b]133;D;abc\x07");
    CHECK_EQ(MK(v, 4).exit_code, -1);
    W(v, A "> " B "x\r\n" C "\x1b]133;D;-2;aid=7\x07");
    CHECK_EQ(MK(v, 5).exit_code, -2);
    CHECK_EQ(cui_vt_mark_count(v), 6);
    /* unknown letters and malformed ones are skipped; parameters after a known letter are allowed */
    unsigned s1 = cui_vt_mark_seq(v);
    W(v, "\x1b]133;Z\x07\x1b]133;P;k=v\x07\x1b]633;P;Cwd=C:\\x\x07\x1b]133;AB\x07\x1b]133;\x07\x1b]133\x07\x1b]1330;A\x07");
    CHECK_EQ(cui_vt_mark_count(v), 6);
    CHECK_EQ(cui_vt_mark_seq(v), s1);
    W(v, "\x1b]133;A;aid=5;cl=m\x07> ");
    CHECK_EQ(cui_vt_mark_count(v), 7);
    /* an A while only the prompt came moves it; a second C keeps the first */
    int n = cui_vt_mark_count(v);
    W(v, "\r\n" A "> ");
    CHECK_EQ(cui_vt_mark_count(v), n);
    CHECK_EQ(MK(v, n - 1).prompt_row, cui_vt_mark_get(v, n - 2, &m) ? m.end_row + 1 : -99);
    W(v, B "cmd\r\n" C "o1\r\n" C "o2\r\n" D0);
    CHECK_STR(OUT(v, n - 1), "o1\no2");
    /* B and C with no prompt open start a mark at their row */
    W(v, B "bare\r\n" C "out\r\n" D0);
    m = MK(v, n);
    CHECK_EQ(m.prompt_row, m.cmd_row); CHECK_STR(CMD(v, n), "bare"); CHECK_STR(OUT(v, n), "out");
    W(v, C "only-c\r\n" D1);
    m = MK(v, n + 1);
    CHECK_EQ(m.prompt_row, m.out_row); CHECK_EQ(m.exit_code, 1); CHECK_STR(OUT(v, n + 1), "only-c");
    /* an A that never got its D ends the command before it, exit unknown */
    W(v, A "> " B "x\r\n" C "y\r\n" A "> ");
    m = MK(v, n + 2);
    CHECK(m.finished); CHECK_EQ(m.exit_code, -1); CHECK_STR(OUT(v, n + 2), "y");
    cui_vt_destroy(v);
}

/* the same output a byte at a time gives the same marks */
static void test_split(void) {
    const char *s = "\x1b]133;D;0\x07" A "PS> " B "dir\r\n" C "a\r\nb\r\n" "\x1b]133;D;3\x1b\\" "\x1b]633;A\x07$ \x1b]633;B\x07" "echo hi\r\n"
                    "\x1b]633;E;echo hi\x07" "\x1b]633;C\x07hi\r\n\x1b]633;D;0\x07" "\x1b]9;4;1;33\x07" "\x1b]52;c;c3BsaXQ=\x1b\\" A "> ";
    cui_vt *a = mkvt(40, 10, 100, 0), *b = mkvt(40, 10, 100, 0);
    W(a, s); W1(b, s);
    CHECK_EQ(cui_vt_mark_count(a), 3); CHECK_EQ(cui_vt_mark_count(b), 3);
    for (int i = 0; i < 3; i++) {
        cui_vt_mark x = MK(a, i), y = MK(b, i);
        CHECK_EQ(x.prompt_row, y.prompt_row); CHECK_EQ(x.cmd_row, y.cmd_row); CHECK_EQ(x.out_row, y.out_row);
        CHECK_EQ(x.end_row, y.end_row); CHECK_EQ(x.exit_code, y.exit_code); CHECK_EQ(x.finished, y.finished);
        char ca[256], cb[256];
        cui_vt_mark_command(a, i, ca, sizeof ca); cui_vt_mark_command(b, i, cb, sizeof cb); CHECK_STR(ca, cb);
        cui_vt_mark_output(a, i, ca, sizeof ca); cui_vt_mark_output(b, i, cb, sizeof cb); CHECK_STR(ca, cb);
    }
    CHECK_STR(CMD(b, 1), "echo hi");
    int pa, pb;
    CHECK_EQ(cui_vt_progress(a, &pa), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(cui_vt_progress(b, &pb), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(pb, 33);
    char t[32];
    CHECK_EQ(cui_vt_take_clipboard(b, t, sizeof t), 5); CHECK_STR(t, "split");
    cui_vt_line_text(b, 5, t, sizeof t); CHECK_STR(t, "> ");
    cui_vt_destroy(a); cui_vt_destroy(b);
}

static void test_text(void) {
    cui_vt *v = mkvt(10, 12, 100, 0);
    run(v, "wrap", "0123456789abcde\r\n", 0);                              /* rows a wrap joined are one line */
    CHECK_STR(OUT(v, 0), "0123456789abcde");
    run(v, "sp", "\r\n  a   \r\n\r\n  b  \r\n\r\n", 0);                     /* blank lines at the start and end, spaces at line ends: cut */
    CHECK_STR(OUT(v, 1), "  a\n\n  b");
    char b4[4];
    CHECK_EQ(cui_vt_mark_output(v, 1, b4, sizeof b4), 8); CHECK_STR(b4, "  a");   /* cut, the whole length returned */
    CHECK_EQ(cui_vt_mark_output(v, 1, NULL, 0), 8);
    run(v, "w", "日本\r\n", 0);
    CHECK_STR(OUT(v, 2), "日本");
    char b5[5];
    CHECK_EQ(cui_vt_mark_output(v, 2, b5, sizeof b5), 6); CHECK_STR(b5, "日");      /* whole characters only */
    run(v, "long command line", "x\r\n", 0);                               /* the command wraps too */
    CHECK_STR(CMD(v, 3), "long command line");
    W(v, A "PS> " B "  spaced  \r\n" C "\x1b]133;D;0\x07");
    CHECK_STR(CMD(v, 4), "spaced");
    CHECK_STR(OUT(v, 4), "");
    CHECK_EQ(cui_vt_mark_output(v, 4, NULL, 0), 0);
    /* a program that ends without a line break: D comes after its text, on the same row as the next prompt */
    W(v, A "PS> " B "nonl\r\n" C "abc" D0 A "PS> " B);
    CHECK_STR(OUT(v, 5), "abc");
    int n = cui_vt_mark_count(v);
    CHECK_EQ(cui_vt_mark_at_row(v, MK(v, n - 1).prompt_row), n - 1);
    cui_vt_destroy(v);

    /* cmd: A and B from its PROMPT, D with no code, never C: the output is the lines after the command's */
    v = mkvt(40, 10, 100, 0);
    W(v, "\x1b]133;D\x1b\\" A "C:\\>" B "dir\r\n" "file1\r\nfile2\r\n\r\n" "\x1b]133;D\x1b\\" A "C:\\>" B "echo x\r\nx\r\n" "\x1b]133;D\x1b\\" A "C:\\>" B);
    CHECK_EQ(cui_vt_mark_count(v), 3);
    cui_vt_mark m = MK(v, 0);
    CHECK_EQ(m.out_row, INT_MIN); CHECK(m.finished); CHECK_EQ(m.exit_code, -1); CHECK_EQ(m.prompt_row, 0); CHECK_EQ(m.end_row, 4);
    CHECK_STR(CMD(v, 0), "dir");
    CHECK_STR(OUT(v, 0), "file1\nfile2");
    CHECK_STR(CMD(v, 1), "echo x");
    CHECK_STR(OUT(v, 1), "x");
    W(v, "\r\n");                                                          /* an empty line: D A B on the next row */
    W(v, "\x1b]133;D\x1b\\" A "C:\\>" B);
    CHECK_STR(OUT(v, 2), "");
    cui_vt_destroy(v);
}

static void test_scroll(void) {
    cui_vt *v = mkvt(40, 5, 100, 0);
    run(v, "a", "out\r\n", 0);
    CHECK_EQ(MK(v, 0).prompt_row, 0); CHECK_EQ(MK(v, 0).end_row, 2);
    for (int i = 0; i < 10; i++) W(v, "line\r\n");                        /* the screen scrolls: rows go into the scrollback, numbers go down */
    cui_vt_mark m = MK(v, 0);
    int sh = 12 - 4;                                                       /* 12 rows written, the cursor on the last of 5 */
    CHECK_EQ(m.prompt_row, 0 - sh); CHECK_EQ(m.out_row, 1 - sh); CHECK_EQ(m.end_row, 2 - sh);
    CHECK_STR(OUT(v, 0), "out"); CHECK_STR(CMD(v, 0), "a");
    CHECK_EQ(cui_vt_mark_at_row(v, -sh), 0);
    CHECK_EQ(cui_vt_mark_near(v, 0, -1), 0);
    CHECK_EQ(cui_vt_mark_near(v, -sh, -1), -1);
    CHECK_EQ(cui_vt_mark_near(v, -sh - 1, 1), 0);
    CHECK_EQ(cui_vt_mark_near(v, -sh, 1), -1);
    cui_vt_destroy(v);

    /* marks go with their rows: a scrollback of 3 lines */
    v = mkvt(40, 5, 3, 0);
    run(v, "a", "out\r\n", 0);
    run(v, "b", "out\r\n", 1);
    unsigned s = cui_vt_mark_seq(v);
    for (int i = 0; i < 3; i++) W(v, "line\r\n");                         /* 3 rows pushed: the scrollback is full */
    CHECK_EQ(cui_vt_mark_count(v), 2);
    W(v, "line\r\n");                                                      /* a's prompt (row 0 ever) leaves */
    CHECK_EQ(cui_vt_mark_count(v), 1);
    CHECK(cui_vt_mark_seq(v) != s);
    CHECK_STR(CMD(v, 0), "b");
    for (int i = 0; i < 10; i++) W(v, "line\r\n");
    CHECK_EQ(cui_vt_mark_count(v), 0);
    cui_vt_destroy(v);

    /* no scrollback at all: a mark goes when its row leaves the top */
    v = mkvt(40, 5, 0, 0);
    run(v, "a", "out\r\n", 0);
    W(v, "1\r\n2\r\n");
    CHECK_EQ(MK(v, 0).prompt_row, 0);
    W(v, "3\r\n");
    CHECK_EQ(cui_vt_mark_count(v), 0);
    cui_vt_destroy(v);

    /* scroll regions, inserted and deleted lines */
    v = mkvt(40, 10, 100, 0);
    W(v, "\x1b[6;1H");
    run(v, "r", "o\r\n", 0);                                               /* prompt on row 5, D on row 7 */
    CHECK_EQ(MK(v, 0).prompt_row, 5);
    W(v, "\x1b[1;4r\x1b[4;1H\n\n");                                        /* the region 0..3 scrolls twice: its top rows go into the scrollback, row 5 stays */
    CHECK_EQ(cui_vt_scrollback_count(v), 2);
    CHECK_EQ(MK(v, 0).prompt_row, 5); CHECK_EQ(MK(v, 0).end_row, 7);
    CHECK_STR(OUT(v, 0), "o");
    W(v, "\x1b[r\x1b[3;1H\x1b[2L");                                        /* two lines inserted at row 2: the mark moves down 2 */
    CHECK_EQ(MK(v, 0).prompt_row, 7); CHECK_EQ(MK(v, 0).out_row, 8); CHECK_EQ(MK(v, 0).end_row, 9);
    CHECK_STR(OUT(v, 0), "o");
    W(v, "\x1b[1;1H\x1b[M");                                               /* one deleted at row 0: up 1 */
    CHECK_EQ(MK(v, 0).prompt_row, 6);
    W(v, "\x1b[3;8r\x1b[3;1H\x1b[3S");                                     /* region 2..7 scrolls up 3: the prompt (row 6) goes to row 3 */
    CHECK_EQ(MK(v, 0).prompt_row, 3); CHECK_EQ(MK(v, 0).out_row, 4);
    W(v, "\x1b[2T");                                                       /* down 2: back to 5; D (row 8 would be 7: past the region's end) stops at it */
    CHECK_EQ(MK(v, 0).prompt_row, 5);
    W(v, "\x1b[6;1H\x1b[M");                                               /* deleting the prompt's row drops the mark */
    CHECK_EQ(cui_vt_mark_count(v), 0);
    cui_vt_destroy(v);

    /* a prompt drawn above earlier marks (the screen was cleared) drops them; the scrollback's stay; ESC [ 3 J drops those */
    v = mkvt(40, 6, 100, 0);
    for (int i = 0; i < 4; i++) run(v, "c", "o\r\n", 0);                    /* 4 marks, 12 rows: 7 in the scrollback */
    CHECK_EQ(cui_vt_mark_count(v), 4);
    int on_screen = 0;
    for (int i = 0; i < 4; i++) if (MK(v, i).prompt_row >= 0) on_screen++;
    W(v, "\x1b[2J\x1b[H" A "PS> " B);
    CHECK_EQ(cui_vt_mark_count(v), 4 - on_screen + 1);
    W(v, "\x1b[3J");
    CHECK_EQ(cui_vt_mark_count(v), 1);
    CHECK_EQ(MK(v, 0).prompt_row, 0);
    W(v, "\x1b" "c");                                                      /* a full reset: the screen's marks go */
    CHECK_EQ(cui_vt_mark_count(v), 0);
    cui_vt_destroy(v);
}

static void test_alt(void) {
    cui_vt *v = mkvt(40, 6, 100, 0);
    run(v, "vim", "", 0);
    W(v, A "PS> " B "less\r\n" C "before\r\n");
    W(v, "\x1b[?1049h\x1b[H" A "alt" B C "\x1b]133;D;5\x07");              /* the alternate screen keeps no marks */
    CHECK_EQ(cui_vt_mark_count(v), 2);
    for (int i = 0; i < 20; i++) W(v, "alt line\r\n");                    /* its scrolling moves no mark */
    cui_vt_mark m = MK(v, 1);
    CHECK_EQ(m.prompt_row, 1); CHECK(!m.finished);
    CHECK_STR(OUT(v, 1), "before");                                        /* the main screen's rows, behind it */
    CHECK_STR(CMD(v, 1), "less");
    W(v, "\x1b[?1049l" D0);
    m = MK(v, 1);
    CHECK(m.finished); CHECK_EQ(m.exit_code, 0); CHECK_EQ(m.end_row, 3);
    CHECK_STR(OUT(v, 1), "before");
    cui_vt_destroy(v);
}

/* marks keep their cells through a resize: cut (no REFLOW), and reflowed */
static void check_texts(cui_vt *v, const char *what) {
    int n = cui_vt_mark_count(v);
    CHECK_EQ(n, 2);
    if (n != 2) { printf("  (%s)\n", what); return; }
    CHECK_STR(CMD(v, 0), "echo abcdefghijklmnopqrstuvwxyz");
    CHECK_STR(OUT(v, 0), "abcdefghijklmnopqrstuvwxyz0123");
    cui_vt_mark m = MK(v, 0), m1 = MK(v, 1);
    CHECK(m.prompt_row <= m.cmd_row && m.cmd_row < m.out_row && m.out_row < m.end_row && m.end_row == m1.prompt_row);
    char t[64]; cui_vt_line_text(v, m1.prompt_row, t, sizeof t);
    CHECK(strncmp(t, "PS>", 3) == 0);                                     /* a reflow drops the space at the line's end */
}

static void test_reflow(void) {
    const char *s = A "PS> " B "echo abcdefghijklmnopqrstuvwxyz\r\n" C "abcdefghijklmnopqrstuvwxyz0123\r\n" D0 A "PS> " B;
    cui_vt *v = mkvt(20, 6, 100, CUI_VT_FEATURE_REFLOW);
    W(v, s);
    cui_vt_mark m = MK(v, 0);
    CHECK_EQ(m.prompt_row, 0); CHECK_EQ(m.out_row, 2); CHECK_EQ(m.end_row, 4); CHECK_EQ(MK(v, 1).prompt_row, 4);
    check_texts(v, "20");
    unsigned s0 = cui_vt_mark_seq(v);
    cui_vt_resize(v, 40, 6);                                               /* wider: the command and its output each fit one row */
    CHECK(cui_vt_mark_seq(v) != s0);
    m = MK(v, 0);
    CHECK_EQ(m.prompt_row, 0); CHECK_EQ(m.out_row, 1); CHECK_EQ(m.end_row, 2); CHECK_EQ(MK(v, 1).prompt_row, 2);
    check_texts(v, "40");
    cui_vt_resize(v, 10, 6);                                               /* narrower: 4 + 3 + 1 rows, 2 go into the scrollback */
    m = MK(v, 0);
    CHECK_EQ(m.prompt_row, -2); CHECK_EQ(m.out_row, 2); CHECK_EQ(m.end_row, 5);
    check_texts(v, "10");
    cui_vt_resize(v, 20, 6);                                               /* the two rows in the scrollback join into one there; the screen starts with the line's rest */
    m = MK(v, 0);
    CHECK_EQ(m.prompt_row, -1); CHECK_EQ(m.out_row, 1); CHECK_EQ(m.end_row, 3);
    check_texts(v, "20 again");
    cui_vt_destroy(v);

    /* a host told of the size only at the end of a drag (cui_vt_resize_sent): every step reflows from the same base */
    v = mkvt(20, 6, 100, CUI_VT_FEATURE_REFLOW);
    W(v, s);
    cui_vt_resize_sent(v);
    cui_vt_resize(v, 10, 6); check_texts(v, "drag 10");
    cui_vt_resize(v, 7, 4); check_texts(v, "drag 7x4");
    cui_vt_resize(v, 40, 8); check_texts(v, "drag 40x8");
    cui_vt_resize(v, 20, 6); check_texts(v, "drag back");
    m = MK(v, 0);    CHECK_EQ(m.prompt_row, 0); CHECK_EQ(m.out_row, 2); CHECK_EQ(m.end_row, 4);
    cui_vt_resize_sent(v);
    cui_vt_destroy(v);

    /* marks in the scrollback reflow with it */
    v = mkvt(20, 6, 100, CUI_VT_FEATURE_REFLOW);
    W(v, s);
    for (int i = 0; i < 12; i++) W(v, "\r\nfill");
    CHECK(MK(v, 1).prompt_row < 0);
    check_texts(v, "sb 20");
    cui_vt_resize(v, 33, 6); check_texts(v, "sb 33");
    cui_vt_resize(v, 8, 6); check_texts(v, "sb 8");
    cui_vt_resize(v, 20, 3); check_texts(v, "sb 20x3");
    cui_vt_destroy(v);

    /* without REFLOW a resize cuts: rows stay, those above the cursor go up when the screen gets shorter */
    v = mkvt(40, 10, 100, 0);
    run(v, "a", "out\r\n", 0);
    for (int i = 0; i < 5; i++) W(v, "x\r\n");
    cui_vt_resize(v, 30, 4);                                               /* the cursor was on row 7: 4 rows go up */
    CHECK_EQ(MK(v, 0).prompt_row, -4);
    CHECK_STR(OUT(v, 0), "out");
    cui_vt_resize(v, 50, 10);
    CHECK_EQ(MK(v, 0).prompt_row, -4);
    cui_vt_destroy(v);
}

static void test_progress(void) {
    cui_vt *v = mkvt(40, 5, 10, 0);
    int p = -1;
    CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NONE); CHECK_EQ(p, 0);
    W(v, "\x1b]9;4;1;42\x07"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(p, 42);
    W(v, "\x1b]9;4;2\x07"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_ERROR); CHECK_EQ(p, 42);      /* left out: kept */
    W(v, "\x1b]9;4;4;50\x1b\\"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_PAUSED); CHECK_EQ(p, 50);
    W(v, "\x1b]9;4;3;\x07"); CHECK_EQ(cui_vt_progress(v, NULL), CUI_VT_PROGRESS_INDETERMINATE);
    W(v, "\x1b]9;4;1;150\x07"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(p, 100);
    W(v, "\x1b]9;hello\x07\x1b]9;4;7;5\x07\x1b]9;4;x\x07\x1b]9;4\x07");        /* a notification, a state past 4, no number: skipped */
    CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(p, 100);
    W(v, "\x1b]9;4;0\x07"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NONE); CHECK_EQ(p, 0);
    W(v, "\x1b]9;4;1;7\x07\x1b" "c"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NONE); CHECK_EQ(p, 0);   /* a full reset clears it */
    W1(v, "\x1b]9;4;1;64\x07"); CHECK_EQ(cui_vt_progress(v, &p), CUI_VT_PROGRESS_NORMAL); CHECK_EQ(p, 64);
    cui_vt_destroy(v);
}

static char *b64(const unsigned char *s, size_t n) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char *o = (char *)malloc((n + 2) / 3 * 4 + 1), *p = o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned x = (unsigned)s[i] << 16 | (i + 1 < n ? (unsigned)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        *p++ = t[(x >> 18) & 63]; *p++ = t[(x >> 12) & 63];
        *p++ = i + 1 < n ? t[(x >> 6) & 63] : '='; *p++ = i + 2 < n ? t[x & 63] : '=';
    }
    *p = 0;
    return o;
}

static void osc52(cui_vt *v, const unsigned char *s, size_t n) {
    char *e = b64(s, n);
    W(v, "\x1b]52;c;"); W(v, e); W(v, "\x07");
    free(e);
}

static void test_clipboard(void) {
    cui_vt *v = mkvt(40, 5, 10, 0);
    char t[64];
    W(v, "\x1b]52;c;aGVsbG8=\x07" "after");
    CHECK_EQ(cui_vt_take_clipboard(v, t, 3), 5); CHECK_STR(t, "");          /* too small: nothing written, kept */
    CHECK_EQ(cui_vt_take_clipboard(v, NULL, 0), 5);
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 5); CHECK_STR(t, "hello");
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);                     /* taken */
    cui_vt_line_text(v, 0, t, sizeof t); CHECK_STR(t, "after");
    W(v, "\x1b]52;c;Zmlyc3Q=\x07\x1b]52;p;c2Vjb25k\x1b\\");                 /* the newest wins; any Pc */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 6); CHECK_STR(t, "second");
    W(v, "\x1b]52;;aGk=\x07"); CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 2); CHECK_STR(t, "hi");
    W(v, "\x1b]52;c;?\x07");                                                /* a query: never answered, nothing kept */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);
    CHECK_EQ(cui_vt_reply(v, t, sizeof t), 0);
    W(v, "\x1b]52;c;\x07\x1b]52;c;@@@@\x07\x1b]52;c;aGk=x=\x07\x1b]52;c\x07");     /* empty, not base64, data after the padding, no data */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);
    osc52(v, (const unsigned char *)"\xff\xfe", 2);                          /* not UTF-8 */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);
    osc52(v, (const unsigned char *)"a\0b", 3);                              /* a NUL */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 0);
    W1(v, "\x1b]52;c;0J/RgNC40LLQtdGC\x1b\\x");                             /* a byte at a time, UTF-8 */
    CHECK_EQ(cui_vt_take_clipboard(v, t, sizeof t), 12); CHECK_STR(t, "Привет");
    /* long: past the OSC buffer; 1 MB is kept, a byte more is dropped whole */
    size_t big = (size_t)1 << 20;
    unsigned char *s = (unsigned char *)malloc(big + 1);
    for (size_t i = 0; i <= big; i++) s[i] = (unsigned char)('a' + i % 26);
    osc52(v, s, 3000);
    char *o = (char *)malloc(big + 2);
    CHECK_EQ(cui_vt_take_clipboard(v, o, big + 2), 3000); CHECK(memcmp(o, s, 3000) == 0);
    osc52(v, s, big);
    CHECK_EQ(cui_vt_take_clipboard(v, o, big + 2), big); CHECK(memcmp(o, s, big) == 0);
    size_t m0 = cui_vt_memory(v);
    osc52(v, s, big + 1);
    CHECK_EQ(cui_vt_take_clipboard(v, o, big + 2), 0);
    CHECK(cui_vt_memory(v) <= m0 + 4096);                                   /* nothing held for it */
    free(s); free(o);
    /* the feature switched off in the middle of a request: the rest is skipped, the text after it shows */
    W(v, "\x1b[2J\x1b[H\x1b]52;c;aGVs");
    cui_vt_set_features(v, 0);
    W(v, "bG8=\x07shown");
    cui_vt_line_text(v, 0, t, sizeof t); CHECK_STR(t, "shown");
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
static char g_clip[8192];
static void set_clip(void *user, const char *text) { (void)user; snprintf(g_clip, sizeof g_clip, "%s", text); }
static const char *get_clip(void *user) { (void)user; return g_clip; }
typedef struct { cui_vt *vt; cui_vt_result r; bool focus, marks; } scene;

static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { s->r = cui_vt_view(ui, "t", s->vt, .focus = s->focus, .marks = s->marks); }
    cui_end_frame(ui);
    s->focus = false;
}

static uint32_t g_px[640 * 400];
static void render(cui_ctx *ui) { cui_sw_render(g_sw, cui_get_draw_data(ui), g_px, 640, 400, 640 * 4, CUI_SW_RGBA); }
static bool reddish(uint32_t p) { int r = p & 255, g = (p >> 8) & 255, b = (p >> 16) & 255; return r > 150 && g < 120 && b < 120; }
/* the rows of the tick column that hold a red pixel */
static int red_rows(int *first) {
    int n = 0; *first = -1;
    for (int y = 0; y < 400; y++) {
        bool any = false;
        for (int x = 626; x < 640; x++) if (reddish(g_px[y * 640 + x])) any = true;
        if (any) { if (*first < 0) *first = y; n++; }
    }
    return n;
}

static void test_view(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    cui_platform_iface pf = { 0 };
    pf.set_clipboard = set_clip; pf.get_clipboard = get_clip;
    cui_set_platform(ui, &pf);
    scene s = { mkvt(80, 24, 1000, 0), { 0 }, true, false };
    for (int i = 0; i < 3; i++) build(ui, &s);
    CHECK_EQ(s.r.mark, -1);
    char b[256];
    for (int i = 0; i < 30; i++) {
        snprintf(b, sizeof b, "out %d a\r\nout %d b\r\nout %d c\r\n", i, i, i);
        run(&*s.vt, i % 7 == 3 ? "fail" : "ok", b, i % 7 == 3 ? 1 : 0);
    }
    W(s.vt, A "PS> " B);
    cui_input_mouse_move(ui, 300, 200);                                    /* hovered: the scrollbar shows */
    build(ui, &s); build(ui, &s);
    render(ui);
    int y0, nred = red_rows(&y0);
    CHECK_EQ(nred, 0);                                                     /* without .marks: no ticks */
    s.marks = true;
    build(ui, &s); build(ui, &s);
    render(ui);
    nred = red_rows(&y0);
    CHECK(nred >= 4 && nred <= 5 * 3);                                     /* 5 failed commands, a tick or two device rows each */
    cui_input_mouse_leave(ui);
    build(ui, &s); build(ui, &s);
    render(ui);
    int y1; CHECK_EQ(red_rows(&y1), 0);                                    /* not hovered, at the live screen: no bar, no ticks */
    /* a click on a red tick returns its mark */
    cui_input_mouse_move(ui, 634, (float)y0 + 0.5f);
    build(ui, &s);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    int hit = s.r.mark;
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    CHECK(hit >= 0);
    CHECK_EQ(s.r.mark, -1);
    if (hit >= 0) CHECK_EQ(MK(s.vt, hit).exit_code, 1);
    CHECK_EQ(hit, 3);                                                      /* the first failed one (i = 3) is the topmost red tick */
    CHECK(!s.r.selected);                                                  /* the click started no selection */
    /* selecting its output scrolls it a third down and selects it; Ctrl+C copies it */
    cui_vt_mark_select(s.vt, 3, CUI_VT_MARK_SELECT_OUTPUT);
    CHECK_EQ(cui_vt_mark_selected(s.vt), 3);
    build(ui, &s); build(ui, &s);
    cui_vt_mark m = MK(s.vt, 3);
    int want = s.r.rows / 3 - m.prompt_row;                                /* the view sized the screen to itself */
    CHECK_EQ(s.r.scrolled, want);
    CHECK(s.r.selected);
    g_clip[0] = 0;
    cui_input_key(ui, CUI_KEY_C, true, CUI_MOD_CTRL);
    build(ui, &s);
    cui_input_key(ui, CUI_KEY_C, false, CUI_MOD_CTRL);
    CHECK(s.r.copied);
    CHECK_STR(g_clip, "out 3 a\nout 3 b\nout 3 c");
    CHECK_STR(OUT(s.vt, 3), "out 3 a\nout 3 b\nout 3 c");
    /* only showing: the view moves, the selection does not come */
    cui_vt_mark_select(s.vt, 10, CUI_VT_MARK_SHOW);
    build(ui, &s); build(ui, &s);
    CHECK_EQ(s.r.scrolled, s.r.rows / 3 - MK(s.vt, 10).prompt_row);
    CHECK(!s.r.selected);
    /* the selected tick is drawn in the accent: the picture changes with the choice */
    render(ui);
    uint32_t h1 = 0; for (int i = 0; i < 640 * 400; i++) h1 = h1 * 31 + g_px[i];
    cui_vt_mark_select(s.vt, 11, CUI_VT_MARK_SHOW);
    build(ui, &s); build(ui, &s);
    cui_vt_mark_select(s.vt, 10, CUI_VT_MARK_SHOW);
    build(ui, &s); build(ui, &s);
    render(ui);
    uint32_t h2 = 0; for (int i = 0; i < 640 * 400; i++) h2 = h2 * 31 + g_px[i];
    CHECK(h1 == h2);
    /* without .marks the view ignores the request */
    s.marks = false;
    int at = s.r.scrolled;
    cui_vt_mark_select(s.vt, 20, CUI_VT_MARK_SHOW);
    build(ui, &s); build(ui, &s);
    CHECK_EQ(s.r.scrolled, at);
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* memory per mark, and the parse speed with marks kept against without (printed: the figures are the measure) */
static void test_cost(void) {
    cui_vt *v = mkvt(120, 30, 5000, 0);
    size_t m0 = cui_vt_memory(v);
    for (int i = 0; i < 1000; i++) run(v, "ls", "a\r\n", 0);
    size_t m1 = cui_vt_memory(v);
    printf("  memory per mark: %.1f bytes (1000 marks, 633;E text aside)\n", (double)(m1 - m0) / 1000.0);
    CHECK((m1 - m0) / 1000 <= 160);
    char *flood = (char *)malloc(8 << 20);
    size_t n = 0;
    for (int i = 0; n + 200 < (size_t)(8 << 20); i++) n += (size_t)snprintf(flood + n, 200, "\x1b[32mline %d\x1b[0m the quick brown fox jumps over the lazy dog %d\r\n", i, i * 7);
    double best[2] = { 1e9, 1e9 };
    for (int pass = 0; pass < 6; pass++) {
        int k = pass & 1;
        cui_vt *f = cui_vt_create(120, 30, 5000);
        if (k) { cui_vt_set_features(f, CUI_VT_FEATURE_MARKS); for (int i = 0; i < 500; i++) run(f, "ls", "a\r\n", 0); }
        double t0 = now_ms();
        cui_vt_write(f, flood, n);
        double t = now_ms() - t0;
        if (t < best[k]) best[k] = t;
        cui_vt_destroy(f);
    }
    printf("  parse %.1f MB: marks off %.1f ms (%.0f MB/s), on with 500 marks %.1f ms (%.0f MB/s)\n", n / 1048576.0,
           best[0], n / 1048576.0 / (best[0] / 1000), best[1], n / 1048576.0 / (best[1] / 1000));
    CHECK(best[1] < best[0] * 1.5 + 5);                                    /* loose: a busy machine */
    free(flood);
    cui_vt_destroy(v);
}

int main(void) {
    g_sw = cui_sw_create();
    test_off();
    test_parse();
    test_split();
    test_text();
    test_scroll();
    test_alt();
    test_reflow();
    test_progress();
    test_clipboard();
    test_view();
    test_cost();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
