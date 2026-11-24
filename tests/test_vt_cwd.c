/* Tests for two things the VT screen reports to the app: the current folder (OSC 7, cui_vt_cwd) and the bell
 * (cui_vt_bell_count). No view, no window. */
#include <cui/cui.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a); if (!a_ || strcmp(a_, (b))) { g_failed++; \
    printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, a_ ? a_ : "(null)", (b)); } } while (0)

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static const char *row(const cui_vt *v, int y) { static char b[2][512]; static int i; i ^= 1; cui_vt_line_text(v, y, b[i], sizeof b[i]); return b[i]; }

static void set_host(const char *name) {
#if defined(_WIN32)
    char e[300];
    snprintf(e, sizeof e, "COMPUTERNAME=%s", name);
    _putenv(e);
#else
    (void)name;
#endif
}

static void test_cwd(void) {
    cui_vt *v = cui_vt_create(40, 5, 100);
    CHECK(cui_vt_cwd(v) == NULL);                                       /* nothing said yet */
    CHECK(cui_vt_cwd_host(v) == NULL);

    W(v, "\x1b]7;file:///C:/Users/me\x07");                              /* no host, a drive path: backslashes, the BEL ends it */
    CHECK_STR(cui_vt_cwd(v), "C:\\Users\\me");
    CHECK_STR(cui_vt_cwd_host(v), "");
    CHECK_EQ(cui_vt_bell_count(v), 0);                                   /* the BEL that ends an OSC is no bell */

    W(v, "\x1b]7;file://localhost/D:/My%20Projects/%C3%A9t%C3%A9%25\x1b\\");   /* encoded: space, UTF-8, a percent sign; ST ends it */
    CHECK_STR(cui_vt_cwd(v), "D:\\My Projects\\\xc3\xa9t\xc3\xa9%");

    W(v, "\x1b]7;file:///E:\x07");                                       /* a bare drive is its root */
    CHECK_STR(cui_vt_cwd(v), "E:\\");
    W(v, "\x1b]7;file:///C:/\x07");
    CHECK_STR(cui_vt_cwd(v), "C:\\");

    W(v, "\x1b]7;file://localhost/home/me/src\x07");                     /* a POSIX path stays as it is */
    CHECK_STR(cui_vt_cwd(v), "/home/me/src");

    W(v, "\x1b]7;file:///bad%zzpath%4\x07");                             /* a % that is not an escape stays literal */
    CHECK_STR(cui_vt_cwd(v), "/bad%zzpath%4");

    /* ignored, the last good one stays: another scheme, no path, a control character, a NUL */
    W(v, "\x1b]7;file:///C:/keep\x07");
    W(v, "\x1b]7;http://pc/C:/x\x07");
    W(v, "\x1b]7;file://pc\x07");
    W(v, "\x1b]7;file:///C:/a%0Ab\x07");
    W(v, "\x1b]7;file:///C:/a%00b\x07");
    W(v, "\x1b]7;\x07");
    W(v, "\x1b]7\x07");
    CHECK_STR(cui_vt_cwd(v), "C:\\keep");
    CHECK_STR(row(v, 0), "");                                            /* nothing of it reached the screen */

    /* another machine (an ssh session): known as a host, but no local folder */
#if defined(_WIN32)
    set_host("MYPC");
    W(v, "\x1b]7;file://MYPC/C:/Work\x07");
    CHECK_STR(cui_vt_cwd(v), "C:\\Work");                                /* this machine, by its name */
    W(v, "\x1b]7;file://mypc.corp.example/C:/Work2\x07");                /* case and the domain do not matter */
    CHECK_STR(cui_vt_cwd(v), "C:\\Work2");
    W(v, "\x1b]7;file://server7/home/me\x07");
    CHECK(cui_vt_cwd(v) == NULL);
    CHECK_STR(cui_vt_cwd_host(v), "server7");
    W(v, "\x1b]7;file://MYPC/C:/Back\x07");                              /* back on this machine */
    CHECK_STR(cui_vt_cwd(v), "C:\\Back");
#else
    W(v, "\x1b]7;file://surely-not-this-machine/home/me\x07");
    CHECK(cui_vt_cwd(v) == NULL);
    CHECK_STR(cui_vt_cwd_host(v), "surely-not-this-machine");
#endif

    /* a long path, cut by the end of a write anywhere */
    char path[700], seq[800];
    int k = snprintf(path, sizeof path, "C:");
    while (k < 600) k += snprintf(path + k, sizeof path - (size_t)k, "/folder%03d", k);
    int n = snprintf(seq, sizeof seq, "\x1b]7;file:///%s\x1b\\", path);
    for (int cut = 1; cut < n; cut += 97) {
        cui_vt_write(v, seq, (size_t)cut);
        cui_vt_write(v, seq + cut, (size_t)(n - cut));
    }
    for (char *c = path; *c; c++) if (*c == '/') *c = '\\';
    CHECK_STR(cui_vt_cwd(v), path);

    /* the title is untouched by it, and a title does not touch it */
    W(v, "\x1b]0;my title\x07");
    CHECK_STR(cui_vt_title(v), "my title");
    CHECK_STR(cui_vt_cwd(v), path);
    CHECK(cui_vt_memory(v) > 600);
    cui_vt_destroy(v);
    set_host("");
}

static void test_bell(void) {
    cui_vt *v = cui_vt_create(20, 3, 10);
    CHECK_EQ(cui_vt_bell_count(v), 0);
    W(v, "a\x07" "b");
    CHECK_EQ(cui_vt_bell_count(v), 1);
    CHECK_STR(row(v, 0), "ab");                                          /* nothing drawn for it */
    W(v, "\x07\x07\x07");
    CHECK_EQ(cui_vt_bell_count(v), 4);
    W(v, "\x1b]0;t\x07");                                                /* ends a title: no bell */
    W(v, "\x1b]2;t2\x1b\\");
    W(v, "\x1bP1q#0~\x07");                                              /* ends a DCS: no bell */
    W(v, "\x1b_apc\x07");                                                /* ends an APC string: no bell */
    CHECK_EQ(cui_vt_bell_count(v), 4);
    W(v, "\x1b[1\x07m");                                                 /* inside a CSI it rings, as in xterm */
    CHECK_EQ(cui_vt_bell_count(v), 5);
    cui_vt_destroy(v);
    CHECK_EQ(cui_vt_bell_count(NULL), 0);
    CHECK(cui_vt_cwd(NULL) == NULL);
}

int main(void) {
    test_cwd();
    test_bell();
    printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
