/* Tests for the VT screen's links (cui_vt_links.c): the detector on real compiler, git, shell and browser lines (URLs with
 * brackets and punctuation, Windows paths with spaces, every position form), OSC 8 hyperlinks kept with their cells (parse,
 * overwrite, erase, shift, scrollback, the table filling up, resize), the view's Ctrl hover and Ctrl+click, the smart double
 * and triple click, trimmed copies and copy on select. Runs headless with the software renderer and prints what detection
 * costs a frame. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a), *b_ = (b); if (!a_ || !b_ || strcmp(a_, b_)) { g_failed++; \
    printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_ ? b_ : "(null)"); } } while (0)

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }

/* ---- the detector ---- */

typedef struct { const char *line, *hover; int kind; const char *text; int ln, col; } lcase;
#define U CUI_VT_LINK_URL
#define P CUI_VT_LINK_PATH

static const lcase g_cases[] = {
    /* URLs: brackets they open are theirs, punctuation at the end and brackets they did not open are not */
    { "see https://example.com/a_(b) for more", "example", U, "https://example.com/a_(b)", 0, 0 },
    { "(see https://example.com/path)", "path", U, "https://example.com/path", 0, 0 },
    { "Visit https://example.com/x.", "https", U, "https://example.com/x", 0, 0 },
    { "<https://example.com/a>", "example", U, "https://example.com/a", 0, 0 },
    { "[docs](https://github.com/owner/repo/pull/12#issuecomment-1),", "owner", U, "https://github.com/owner/repo/pull/12#issuecomment-1", 0, 0 },
    { "\"https://x.example/a?b=1&c=2\",", "x.example", U, "https://x.example/a?b=1&c=2", 0, 0 },
    { "server at http://[::1]:8080/status]", "::1", U, "http://[::1]:8080/status", 0, 0 },
    { "https://en.wikipedia.org/wiki/C_(programming_language)!", "wiki/C", U, "https://en.wikipedia.org/wiki/C_(programming_language)", 0, 0 },
    { "see 'https://example.org/q?x=a,b'.", "example", U, "https://example.org/q?x=a,b", 0, 0 },
    { "Open HTTPS://EXAMPLE.COM/A now", "EXAMPLE", U, "HTTPS://EXAMPLE.COM/A", 0, 0 },
    { "remote:   https://github.com/owner/repo/pull/new/feature-x", "pull", U, "https://github.com/owner/repo/pull/new/feature-x", 0, 0 },
    { "  Local:   http://localhost:5173/", "localhost", U, "http://localhost:5173/", 0, 0 },
    { "file:///C:/Program%20Files/App/readme.txt", "Program", P, "C:\\Program Files\\App\\readme.txt", 0, 0 },
    { "file://server/share/dir/x.log", "share", P, "\\\\server\\share\\dir\\x.log", 0, 0 },
    { " FAIL  file:///C:/src/a.test.ts:12:5", "a.test", P, "C:\\src\\a.test.ts", 12, 5 },
    /* compilers */
    { "main.c:12:5: error: 'x' undeclared (first use in this function)", "main", P, "main.c", 12, 5 },
    { "C:\\src\\app\\main.c:12:5: warning: unused variable 'y' [-Wunused-variable]", "app", P, "C:\\src\\app\\main.c", 12, 5 },
    { "In file included from include/foo.h:3,", "foo", P, "include/foo.h", 3, 0 },
    { "C:\\src\\foo.cpp(12): error C2065: 'x': undeclared identifier", "foo", P, "C:\\src\\foo.cpp", 12, 0 },
    { "foo.cpp(12,5): warning C4996: 'strcpy': This function or variable may be unsafe.", "foo", P, "foo.cpp", 12, 5 },
    { "Program.cs(12,17): error CS0103: The name 'x' does not exist in the current context [C:\\src\\app\\app.csproj]", "Program", P, "Program.cs", 12, 17 },
    { "Program.cs(12,17): error CS0103: The name 'x' does not exist in the current context [C:\\src\\app\\app.csproj]", "app.csproj", P, "C:\\src\\app\\app.csproj", 0, 0 },
    { "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h(1234): warning C4996: deprecated", "stdio", P,
      "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h", 1234, 0 },
    { "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h(1234): warning C4996: deprecated", "Program", P,
      "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h", 1234, 0 },
    { "C:\\My Projects\\app\\main.c:40:2: error: expected ';'", "Projects", P, "C:\\My Projects\\app\\main.c", 40, 2 },
    { "src/index.ts(12,5): error TS2322: Type 'string' is not assignable to type 'number'.", "index", P, "src/index.ts", 12, 5 },
    { "src/index.ts:12:5 - error TS2322: Type 'string' is not assignable", "src/", P, "src/index.ts", 12, 5 },
    { "  --> src\\main.rs:12:5", "main", P, "src\\main.rs", 12, 5 },
    { "./main.go:12:5: undefined: x", "main", P, "./main.go", 12, 5 },
    { "/usr/include/stdio.h:12:1: note: declared here", "stdio", P, "/usr/include/stdio.h", 12, 1 },
    { "  File \"C:\\Users\\me\\My Scripts\\app.py\", line 12, in <module>", "Scripts", P, "C:\\Users\\me\\My Scripts\\app.py", 12, 0 },
    { "    at Object.<anonymous> (C:\\app\\index.js:12:5)", "index", P, "C:\\app\\index.js", 12, 5 },
    { "    at main (file:///C:/app/main.mjs:3:9)", "main.mjs", P, "C:\\app\\main.mjs", 3, 9 },
    /* PowerShell, cmd, jnsh */
    { "At C:\\Users\\me\\script.ps1:12 char:5", "script", P, "C:\\Users\\me\\script.ps1", 12, 5 },
    { "ParserError: C:\\Users\\me\\t.ps1:3", "t.ps1", P, "C:\\Users\\me\\t.ps1", 3, 0 },
    { "Get-Item: Cannot find path 'C:\\Program Files\\Missing App' because it does not exist.", "Missing", P, "C:\\Program Files\\Missing App", 0, 0 },
    { "PS C:\\Program Files\\PowerShell> ", "Files", P, "C:\\Program Files\\PowerShell", 0, 0 },
    { "    Directory: C:\\Users\\me\\My Projects", "Projects", P, "C:\\Users\\me\\My Projects", 0, 0 },
    { "C:\\Users\\me\\Projects> dir", "Users", P, "C:\\Users\\me\\Projects", 0, 0 },
    { "copy \"C:\\My Docs\\a b.txt\" D:\\out\\", "Docs", P, "C:\\My Docs\\a b.txt", 0, 0 },
    { "copy \"C:\\My Docs\\a b.txt\" D:\\out\\", "out", P, "D:\\out\\", 0, 0 },
    { "\\\\server\\share\\builds\\log.txt", "builds", P, "\\\\server\\share\\builds\\log.txt", 0, 0 },
    { "run .\\build\\jnshell.exe now", "build", P, ".\\build\\jnshell.exe", 0, 0 },
    { "cd ..\\CUI\\src", "CUI", P, "..\\CUI\\src", 0, 0 },
    { "ls ~/projects/x", "projects", P, "~/projects/x", 0, 0 },
    { "d----  docs\\", "docs", P, "docs\\", 0, 0 },
    { "--out=dist/app.js", "dist", P, "dist/app.js", 0, 0 },
    /* git */
    { "\tmodified:   apps/jnshell/main.c", "apps", P, "apps/jnshell/main.c", 0, 0 },
    { "diff --git a/src/x.c b/src/x.c", "a/src", P, "a/src/x.c", 0, 0 },
    { "+++ b/docs/readme.md", "docs", P, "b/docs/readme.md", 0, 0 },
    { "src/tabs.c:42:    int x = 0;", "tabs", P, "src/tabs.c", 42, 0 },
    { "CONFLICT (content): Merge conflict in JN/jnshell/apps/jnshell/main.c", "jnshell/apps", P, "JN/jnshell/apps/jnshell/main.c", 0, 0 },
    { " apps/jnshell/tabs.c | 12 ++++++------", "tabs", P, "apps/jnshell/tabs.c", 0, 0 },
    { "apps\\jnshell\\tabs.c:120:5:    cui_vt_view(", "tabs", P, "apps\\jnshell\\tabs.c", 120, 5 },
    /* not links */
    { "this and/or that", "and", 0, NULL, 0, 0 },
    { "Your branch is up to date with 'origin/main'.", "origin", 0, NULL, 0, 0 },
    { "on 1/2/2024 at 12:30:45", "1/2", 0, NULL, 0, 0 },
    { "on 1/2/2024 at 12:30:45", "12:30", 0, NULL, 0, 0 },
    { "version v1.2.3 is out", "v1", 0, NULL, 0, 0 },
    { "e.g. this", "e.g", 0, NULL, 0, 0 },
    { "\tmodified:   apps/jnshell/main.c", "modified", 0, NULL, 0, 0 },
    { "x = a/b;", "a/b", 0, NULL, 0, 0 },
    { "error: something failed", "something", 0, NULL, 0, 0 },
    { "see http:// alone", "http", 0, NULL, 0, 0 },
    { "drive C: only", "C:", 0, NULL, 0, 0 },
    { "dir /s", "/s", 0, NULL, 0, 0 },
    { "copy C:\\x\\a.c and b.c", "and", 0, NULL, 0, 0 },
    { "Visit https://example.com/x.", "x.", 0, NULL, 0, 0 },     /* on the URL's last letter it is a link; this hover is on the dot */
};

static void test_detect(void) {
    int n = (int)(sizeof g_cases / sizeof g_cases[0]);
    for (int i = 0; i < n; i++) {
        const lcase *c = &g_cases[i];
        const char *h = strstr(c->line, c->hover);
        if (!h) { g_failed++; printf("  FAIL case %d: '%s' is not in the line\n", i, c->hover); continue; }
        int at = (int)(h - c->line);
        if (!strcmp(c->hover, "x.")) at++;                   /* the dot after the URL */
        cui_vt_link l; char buf[512]; int b0 = -1, b1 = -1;
        bool ok = cui_vt_link_find(c->line, (int)strlen(c->line), at, &l, buf, sizeof buf, &b0, &b1);
        g_checks++;
        if (ok != (c->kind != 0) || (ok && (l.kind != c->kind || strcmp(l.text, c->text) || l.line != c->ln || l.col != c->col))) {
            g_failed++;
            printf("  FAIL case %d: %s\n        hover '%s': got %s kind %d '%s' %d:%d, expected kind %d '%s' %d:%d\n", i, c->line, c->hover, ok ? "a link" : "none",
                   ok ? l.kind : 0, ok ? l.text : "", ok ? l.line : 0, ok ? l.col : 0, c->kind, c->text ? c->text : "", c->ln, c->col);
            continue;
        }
        if (ok) CHECK(b0 <= at && at < b1);
    }
    /* the span: a URL whole, a path with its position, never quotes */
    {
        const char *s = "at \"C:\\a b\\c.txt\" and src/x.c:12:5: err";
        cui_vt_link l; char buf[256]; int b0, b1;
        CHECK(cui_vt_link_find(s, (int)strlen(s), 6, &l, buf, sizeof buf, &b0, &b1));
        CHECK_EQ(b0, 4); CHECK_EQ(b1, 16);
        int at = (int)(strstr(s, "src") - s);
        CHECK(cui_vt_link_find(s, (int)strlen(s), at, &l, buf, sizeof buf, &b0, &b1));
        CHECK_EQ(b0, at); CHECK_EQ(b1, at + 12);              /* src/x.c:12:5 without gcc's colon */
    }
    /* a buffer too small gives no link, never a cut one */
    {
        const char *s = "https://example.com/a/long/path";
        cui_vt_link l; char buf[8];
        CHECK(!cui_vt_link_find(s, (int)strlen(s), 3, &l, buf, sizeof buf, NULL, NULL));
        CHECK(!cui_vt_link_find(s, (int)strlen(s), 99, &l, buf, sizeof buf, NULL, NULL));
    }
    printf("  %d lines\n", n);
}

/* ---- OSC 8 ---- */

static void test_osc8(void) {
    cui_vt *v = cui_vt_create(20, 5, 100);
    size_t m0 = cui_vt_memory(v);
    cui_vt_set_features(v, CUI_VT_FEATURE_LINKS);
    CHECK_EQ(cui_vt_memory(v), m0);                           /* nothing before the first link */
    W(v, "\x1b]8;;https://a.example/x\x1b\\link\x1b]8;;\x1b\\ plain");
    CHECK(cui_vt_memory(v) > m0);
    char line[64];
    cui_vt_line_text(v, 0, line, sizeof line);
    CHECK_STR(line, "link plain");
    for (int x = 0; x < 4; x++) CHECK_STR(cui_vt_hyperlink(v, x, 0), "https://a.example/x");
    CHECK(cui_vt_hyperlink(v, 4, 0) == NULL);
    CHECK(cui_vt_hyperlink(v, 5, 0) == NULL);
    /* BEL ends it too; an id parameter; the same link again is the same entry */
    W(v, "\r\n\x1b]8;id=7;https://b.example\x07" "B\x1b]8;;\x07" "-\x1b]8;id=7;https://b.example\x07" "B\x1b]8;;\x07");
    CHECK_STR(cui_vt_hyperlink(v, 0, 1), "https://b.example");
    CHECK(cui_vt_hyperlink(v, 1, 1) == NULL);
    CHECK(cui_vt_hyperlink(v, 0, 1) == cui_vt_hyperlink(v, 2, 1));
    /* overwriting, erasing, inserting and deleting */
    W(v, "\x1b[1;1Hxx");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL && cui_vt_hyperlink(v, 1, 0) == NULL);
    CHECK_STR(cui_vt_hyperlink(v, 2, 0), "https://a.example/x");
    W(v, "\x1b[1;1H\x1b[2@");
    CHECK(cui_vt_hyperlink(v, 2, 0) == NULL);
    CHECK_STR(cui_vt_hyperlink(v, 4, 0), "https://a.example/x");
    W(v, "\x1b[1;1H\x1b[2P");
    CHECK_STR(cui_vt_hyperlink(v, 2, 0), "https://a.example/x");
    W(v, "\x1b[1;3H\x1b[K");
    CHECK(cui_vt_hyperlink(v, 2, 0) == NULL && cui_vt_hyperlink(v, 3, 0) == NULL);
    /* a wide character takes the link in both cells */
    W(v, "\x1b[3;1H\x1b]8;;https://w.example\x1b\\\xe4\xb8\xad\x1b]8;;\x1b\\");
    CHECK_STR(cui_vt_hyperlink(v, 0, 2), "https://w.example");
    CHECK_STR(cui_vt_hyperlink(v, 1, 2), "https://w.example");
    /* into the scrollback with its line, and away with it */
    W(v, "\x1b[2J\x1b[H\x1b]8;;https://sb.example\x1b\\kept\x1b]8;;\x1b\\");
    for (int i = 0; i < 5; i++) W(v, "\r\n");
    cui_vt_line_text(v, -1, line, sizeof line);
    CHECK_STR(line, "kept");
    CHECK_STR(cui_vt_hyperlink(v, 0, -1), "https://sb.example");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    for (int i = 0; i < 150; i++) W(v, "line\r\n");              /* 100 lines of scrollback: the link's line is gone */
    bool any = false;
    for (int r = -cui_vt_scrollback_count(v); r < 5; r++) any |= cui_vt_hyperlink(v, 0, r) != NULL;
    CHECK(!any);
    /* the alternate screen keeps its own; leaving it takes them */
    W(v, "\x1b[?1049h\x1b[H\x1b]8;;https://alt.example\x1b\\A\x1b]8;;\x1b\\");
    CHECK_STR(cui_vt_hyperlink(v, 0, 0), "https://alt.example");
    W(v, "\x1b[?1049l");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    W(v, "\x1b[?1049h");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);               /* the alternate screen is cleared when entered */
    W(v, "\x1b[?1049l");
    /* refused: a URI with a control character, one too long for the buffer; an unclosed link ends at a full reset */
    W(v, "\x1b[H\x1b]8;;https://bad\x01.example\x1b\\Q\x1b]8;;\x1b\\");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    {
        char *big = (char *)malloc(4000);
        int k = snprintf(big, 4000, "\x1b[H\x1b]8;;https://long.example/");
        while (k < 3000) big[k++] = 'a';
        memcpy(big + k, "\x1b\\L\x1b]8;;\x1b\\", 11);
        cui_vt_write(v, big, (size_t)k + 10);
        free(big);
        cui_vt_line_text(v, 0, line, sizeof line);
        CHECK(line[0] == 'L');
        CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    }
    W(v, "\x1b[H\x1b]8;;https://open.example\x1b\\O\x1b" "cZ");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    /* a resize drops them all, the text stays */
    W(v, "\x1b[H\x1b]8;;https://r.example\x1b\\R\x1b]8;;\x1b\\");
    CHECK(cui_vt_hyperlink(v, 0, 0) != NULL);
    cui_vt_resize(v, 30, 6);
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    cui_vt_line_text(v, 0, line, sizeof line);
    CHECK(line[0] == 'R');
    /* with reflow too: the cells move, the links go, the text is joined again */
    cui_vt_set_features(v, CUI_VT_FEATURE_LINKS | CUI_VT_FEATURE_REFLOW);
    W(v, "\x1b" "c" "\x1b]8;;https://rf.example\x1b\\0123456789012345678901234567890123456789\x1b]8;;\x1b\\");
    CHECK_STR(cui_vt_hyperlink(v, 0, 1), "https://rf.example");
    cui_vt_resize(v, 50, 6);
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL && cui_vt_hyperlink(v, 35, 0) == NULL);
    cui_vt_line_text(v, 0, line, sizeof line);
    CHECK_STR(line, "0123456789012345678901234567890123456789");
    cui_vt_set_features(v, CUI_VT_FEATURE_LINKS);
    /* the feature off frees everything */
    W(v, "\x1b[H\x1b]8;;https://r.example\x1b\\R\x1b]8;;\x1b\\");
    cui_vt_set_features(v, 0);
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    cui_vt_destroy(v);

    /* without the feature OSC 8 is skipped as before: the text, no link, no memory */
    v = cui_vt_create(20, 5, 100);
    m0 = cui_vt_memory(v);
    W(v, "\x1b]8;;https://a.example/x\x1b\\link\x1b]8;;\x1b\\ plain");
    cui_vt_line_text(v, 0, line, sizeof line);
    CHECK_STR(line, "link plain");
    CHECK(cui_vt_hyperlink(v, 0, 0) == NULL);
    CHECK_EQ(cui_vt_memory(v), m0);
    cui_vt_destroy(v);

    /* the table fills up: links no cell holds any more are freed and their ids reused */
    v = cui_vt_create(20, 5, 100);
    cui_vt_set_features(v, CUI_VT_FEATURE_LINKS);
    char b[96];
    for (int i = 0; i < 20000; i++) {
        snprintf(b, sizeof b, "\x1b[H\x1b]8;;https://n.example/%d\x1b\\X\x1b]8;;\x1b\\", i);
        W(v, b);
    }
    CHECK_STR(cui_vt_hyperlink(v, 0, 0), "https://n.example/19999");
    size_t mem = cui_vt_memory(v);
    CHECK(mem < m0 + 600 * 1024);                             /* the table never holds more than 8192 links */
    printf("  20000 links through one cell: %zu KB of screen (%zu KB without links)\n", mem / 1024, m0 / 1024);
    /* every cell a different link: 8192 fit, the rest are plain text until cells free theirs */
    cui_vt_resize(v, 200, 50);
    W(v, "\x1b[H");
    for (int i = 0; i < 9000; i++) { snprintf(b, sizeof b, "\x1b]8;;u%d\x1b\\x\x1b]8;;\x1b\\", i); W(v, b); }
    CHECK_STR(cui_vt_hyperlink(v, 0, 0), "u0");
    CHECK_STR(cui_vt_hyperlink(v, 191, 40), "u8191");
    CHECK(cui_vt_hyperlink(v, 192, 40) == NULL);
    cui_vt_destroy(v);
}

/* ---- the view ---- */

static cui_sw_renderer *g_sw;
typedef struct { cui_vt *vt; cui_vt_result r; bool focus, links, smart, cos, w32, app_clip, do_copy, do_paste, no_input; } scene;
static cui_cursor g_cursor;
static char g_clip[4096];
static void set_clip(void *user, const char *text) { (void)user; snprintf(g_clip, sizeof g_clip, "%s", text); }
static const char *get_clip(void *user) { (void)user; return "pasted"; }
static void set_cur(void *user, cui_cursor c) { (void)user; g_cursor = c; }

static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        s->r = cui_vt_view(ui, "t", s->vt, .focus = s->focus, .links = s->links, .smart_select = s->smart, .copy_on_select = s->cos, .win32_keys = s->w32,
                           .no_clipboard_keys = s->app_clip, .copy = s->do_copy, .paste = s->do_paste, .no_input = s->no_input);
    }
    cui_end_frame(ui);
    s->focus = false;
    s->do_copy = s->do_paste = false;                         /* asked for one frame */
}

static float g_cw;
static void at_cell(cui_ctx *ui, scene *s, int col, int row) { cui_input_mouse_move(ui, 10 + ((float)col + 0.5f) * g_cw, 6 + ((float)row + 0.5f) * 19.0f); build(ui, s); }
static void mods(cui_ctx *ui, scene *s, int m) { cui_input_key(ui, CUI_KEY_NONE, m != 0, m); build(ui, s); }
static void click(cui_ctx *ui, scene *s) { cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, s); cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, s); }
static void pause(cui_ctx *ui, scene *s) { for (int i = 0; i < 30; i++) build(ui, s); }     /* past the double click time */
static void key(cui_ctx *ui, scene *s, cui_key k, int m) { cui_input_key(ui, k, true, m); build(ui, s); cui_input_key(ui, k, false, m); cui_input_key(ui, CUI_KEY_NONE, false, 0); build(ui, s); }   /* Ctrl goes up after the key */

static cui_ctx *view_ctx(scene *s, int cols) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 20 + (float)cols * 7.8f + 4, 400, 1);
    cui_set_reduced_motion(ui, true);
    cui_platform_iface pf = { 0 };
    pf.set_clipboard = set_clip; pf.get_clipboard = get_clip; pf.set_cursor = set_cur;
    cui_set_platform(ui, &pf);
    cui_platform_iface back;
    cui_get_platform(ui, &back);
    CHECK(back.set_clipboard == set_clip && back.set_cursor == set_cur);
    memset(s, 0, sizeof *s);
    s->vt = cui_vt_create(80, 20, 1000);
    cui_vt_set_features(s->vt, CUI_VT_FEATURE_LINKS);
    s->focus = true; s->links = true; s->smart = true;
    for (int i = 0; i < 3; i++) build(ui, s);
    g_cw = cui_measure_text(ui, CUI_FONT_MONO, 13, "0", 1).width;
    return ui;
}

/* bright pixels in the band where an underline goes, under cells c0..c1 of a row */
static int underline_thr = 160;
static int underline_px(cui_ctx *ui, int row, int c0, int c1) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    int w = dd->width_px, h = dd->height_px, n = 0;
    unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    cui_sw_render(g_sw, dd, px, w, h, w * 4, CUI_SW_RGBA);
    int y0 = (int)(6 + (float)row * 19 + 19 - 3), y1 = (int)(6 + (float)row * 19 + 19);
    for (int y = y0; y < y1 && y < h; y++)
        for (int x = (int)(10 + (float)c0 * g_cw) + 1; x < (int)(10 + (float)(c1 + 1) * g_cw) - 1 && x < w; x++) n += px[(y * w + x) * 4] > underline_thr;
    free(px);
    return n;
}

static void test_view_links(void) {
    scene s;
    cui_ctx *ui = view_ctx(&s, 80);
    W(s.vt, "\x1b[2J\x1b[Hsee https://example.com/a_(b) now\r\nsrc\\main.c:12:5: error: x\r\n\x1b]8;;https://osc.example/doc\x1b\\docs\x1b]8;;\x1b\\ here");
    build(ui, &s);
    int d0 = cui__vt_links_detections();
    at_cell(ui, &s, 10, 0);                                   /* over the URL, no Ctrl: nothing is looked for */
    CHECK_EQ(cui__vt_links_detections(), d0);
    CHECK(g_cursor == CUI_CURSOR_TEXT);
    int plain = underline_px(ui, 0, 4, 28);
    mods(ui, &s, CUI_MOD_CTRL);                               /* Ctrl down: once */
    CHECK_EQ(cui__vt_links_detections(), d0 + 1);
    CHECK(g_cursor == CUI_CURSOR_HAND);
    int under = underline_px(ui, 0, 4, 28);
    CHECK(under > plain + 20);                                /* the link is underlined */
    for (int i = 0; i < 5; i++) build(ui, &s);                /* frames with nothing changed look for nothing */
    CHECK_EQ(cui__vt_links_detections(), d0 + 1);
    at_cell(ui, &s, 12, 0);                                   /* another cell of it */
    CHECK_EQ(cui__vt_links_detections(), d0 + 2);
    W(s.vt, "");                                              /* a write may change the text: looked for again */
    build(ui, &s);
    CHECK_EQ(cui__vt_links_detections(), d0 + 3);
    mods(ui, &s, 0);                                          /* Ctrl up: the underline goes, nothing is looked for */
    CHECK(underline_px(ui, 0, 4, 28) <= plain + 2);
    at_cell(ui, &s, 14, 0);
    CHECK_EQ(cui__vt_links_detections(), d0 + 3);
    /* an OSC 8 link has a dotted underline all the time, as a hint */
    underline_thr = 70;
    int dotted = underline_px(ui, 2, 0, 3), none = underline_px(ui, 2, 5, 8);
    underline_thr = 160;
    CHECK(dotted > none + 4);
    printf("  hover underline: %d bright pixels with Ctrl, %d without\n", under, plain);
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* Ctrl+click returns the link and starts no selection; the program gets nothing */
static void test_view_click(void) {
    scene s;
    cui_ctx *ui = view_ctx(&s, 80);
    W(s.vt, "\x1b[2J\x1b[Hsee https://example.com/a_(b) now\r\nsrc\\main.c:12:5: error: x\r\n\x1b]8;;file:///C:/docs/a%20b.md\x1b\\docs\x1b]8;;\x1b\\ here");
    build(ui, &s);
    at_cell(ui, &s, 10, 0);
    mods(ui, &s, CUI_MOD_CTRL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link != NULL);
    if (s.r.link) { CHECK_EQ(s.r.link->kind, CUI_VT_LINK_URL); CHECK_STR(s.r.link->text, "https://example.com/a_(b)"); CHECK(!s.r.link->osc8); }
    CHECK_EQ(s.r.input_len, 0);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    CHECK(s.r.link == NULL);
    at_cell(ui, &s, 4, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link != NULL);
    if (s.r.link) { CHECK_EQ(s.r.link->kind, CUI_VT_LINK_PATH); CHECK_STR(s.r.link->text, "src\\main.c"); CHECK_EQ(s.r.link->line, 12); CHECK_EQ(s.r.link->col, 5); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    at_cell(ui, &s, 1, 2);                                    /* an OSC 8 link to a file */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link != NULL);
    if (s.r.link) { CHECK_EQ(s.r.link->kind, CUI_VT_LINK_PATH); CHECK_STR(s.r.link->text, "C:\\docs\\a b.md"); CHECK(s.r.link->osc8); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    at_cell(ui, &s, 6, 2);                                    /* " here": not a link, so the press selects as always */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link == NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    mods(ui, &s, 0);
    at_cell(ui, &s, 10, 0);                                   /* Ctrl up: a click is a click */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link == NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    /* without .links nothing changes: Ctrl over a URL is a text cursor, a Ctrl+click no link */
    s.links = false;
    build(ui, &s);
    mods(ui, &s, CUI_MOD_CTRL);
    CHECK(g_cursor == CUI_CURSOR_TEXT);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link == NULL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    mods(ui, &s, 0);
    /* a link that wraps: the whole of it, across rows */
    s.links = true;
    W(s.vt, "\x1b[2J\x1b[H");
    char url[160];
    int k = snprintf(url, sizeof url, "https://example.com/");
    while (k < 120) url[k++] = (char)('a' + k % 26);
    url[k] = 0;
    W(s.vt, "> "); W(s.vt, url); W(s.vt, " end");
    build(ui, &s);
    at_cell(ui, &s, 10, 1);                                   /* on the second row */
    mods(ui, &s, CUI_MOD_CTRL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link && !strcmp(s.r.link->text, url));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    mods(ui, &s, 0);
    /* a link back in the history, behind a console host that wants key records (Ctrl down is a record): holding Ctrl
     * keeps the history where it is, so the click lands on the link that was underlined */
    s.w32 = true;
    W(s.vt, "\x1b" "c" "\x1b[?9001h" "old https://history.example/x line\r\n");
    for (int i = 0; i < 60; i++) W(s.vt, "filler\r\n");
    build(ui, &s);
    mods(ui, &s, CUI_MOD_SHIFT);                              /* Shift first (its record), then Shift+Home: to the top of the history */
    key(ui, &s, CUI_KEY_HOME, CUI_MOD_SHIFT);
    at_cell(ui, &s, 8, 0);
    mods(ui, &s, CUI_MOD_CTRL);
    CHECK(s.r.input_len > 0);                                 /* Ctrl went to the program as a record */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.link && !strcmp(s.r.link->text, "https://history.example/x"));
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    mods(ui, &s, 0);
    s.w32 = false;
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* ---- selection ---- */

static void test_select(void) {
    scene s;
    cui_ctx *ui = view_ctx(&s, 80);
    int cols, rows;
    cui_vt_size(s.vt, &cols, &rows);
    /* a double click takes a whole path with its position, a whole URL across wrapped rows, or the word */
    W(s.vt, "\x1b[2J\x1b[Herror at C:\\src\\app\\main.c:12:5: oops\r\n");
    build(ui, &s);
    at_cell(ui, &s, 14, 0);
    click(ui, &s); click(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, "C:\\src\\app\\main.c:12:5");
    char url[200];
    int k = snprintf(url, sizeof url, "https://example.com/");
    while (k < cols + 30) url[k++] = (char)('a' + k % 26);
    url[k] = 0;
    W(s.vt, "\x1b[2J\x1b[H> "); W(s.vt, url); W(s.vt, " tail\r\n");
    build(ui, &s);
    at_cell(ui, &s, 5, 1);
    click(ui, &s); click(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, url);
    W(s.vt, "\x1b[2J\x1b[Hhello wor(ld) x\r\n");
    build(ui, &s);
    at_cell(ui, &s, 2, 0);
    click(ui, &s); click(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, "hello");
    /* a triple click takes the wrapped line as one, a line break nowhere */
    char longl[300];
    k = 0;
    while (k < cols * 2 + 10) { longl[k] = (char)('A' + k % 26); k++; }
    longl[k] = 0;
    W(s.vt, "\x1b[2J\x1b[H"); W(s.vt, longl); W(s.vt, "\r\nnext\r\n");
    build(ui, &s);
    at_cell(ui, &s, 3, 1);
    click(ui, &s); click(ui, &s); click(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, longl);
    /* trailing spaces go from every line of a copy; without smart_select they stay as they always did */
    W(s.vt, "\x1b" "c" "abc   \r\ndef  \r\n");                 /* a full reset: ED 2 leaves the rows' wrap marks (see the report) */
    build(ui, &s);
    pause(ui, &s);
    at_cell(ui, &s, 0, 0);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    at_cell(ui, &s, 20, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, "abc\ndef");
    s.smart = false;
    pause(ui, &s);
    at_cell(ui, &s, 0, 0);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    at_cell(ui, &s, 20, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, "abc   \ndef  ");
    /* the old double click without smart_select: one row, the word only */
    W(s.vt, "\x1b[2J\x1b[Herror at C:\\src\\app\\main.c:12:5: oops\r\n");
    build(ui, &s);
    at_cell(ui, &s, 14, 0);
    click(ui, &s); click(ui, &s);
    key(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK_STR(g_clip, "C:\\src\\app\\main.c:12:5:");
    s.smart = true;
    /* copy on select: the selection is copied when made, a right click then pastes */
    s.cos = true;
    g_clip[0] = 0;
    W(s.vt, "\x1b[2J\x1b[Hsee src/x.c:3 here\r\n");
    build(ui, &s);
    at_cell(ui, &s, 6, 0);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s); cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    CHECK(g_clip[0] == 0);                                    /* one click selects nothing */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    CHECK(s.r.copied);
    CHECK_STR(g_clip, "src/x.c:3");
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, true); build(ui, &s);
    CHECK(s.r.paste && !strcmp(s.r.paste, "pasted"));
    CHECK(!s.r.copied);
    cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, false); build(ui, &s);
    g_clip[0] = 0;
    at_cell(ui, &s, 0, 0);                                    /* a drag copies when the button goes up */
    pause(ui, &s);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); build(ui, &s);
    at_cell(ui, &s, 2, 0);
    CHECK(g_clip[0] == 0);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); build(ui, &s);
    CHECK(s.r.copied);
    CHECK_STR(g_clip, "see");
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* ---- the app's own copy and paste (.no_clipboard_keys, .copy, .paste, `selected`) ---- */

static void press(cui_ctx *ui, scene *s, cui_key k, int m) { cui_input_key(ui, k, true, m); build(ui, s); }
static void release(cui_ctx *ui, scene *s, cui_key k, int m) { cui_input_key(ui, k, false, m); cui_input_key(ui, CUI_KEY_NONE, false, 0); build(ui, s); }

static void select_hello(cui_ctx *ui, scene *s) {
    W(s->vt, "\x1b[2J\x1b[Hhello world\r\n");
    build(ui, s);
    pause(ui, s);
    at_cell(ui, s, 2, 0);
    click(ui, s); click(ui, s);                               /* a double click takes the word */
}

static void test_app_clipboard(void) {
    scene s;
    cui_ctx *ui = view_ctx(&s, 80);
    /* as before, without the option: Ctrl+C copies a selection, Ctrl+V pastes; nothing goes to the program */
    select_hello(ui, &s);
    CHECK(s.r.selected);
    g_clip[0] = 0;
    press(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK(s.r.copied); CHECK_STR(g_clip, "hello"); CHECK_EQ(s.r.input_len, 0); CHECK(!s.r.selected);
    release(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    press(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    CHECK(s.r.paste && !strcmp(s.r.paste, "pasted")); CHECK_EQ(s.r.input_len, 0);
    release(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    /* with it the keys are the program's: Ctrl+C is the interrupt even over a selection, Ctrl+V is ^V */
    s.app_clip = true;
    select_hello(ui, &s);
    CHECK(s.r.selected);
    g_clip[0] = 0;
    press(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    CHECK(!s.r.copied); CHECK(g_clip[0] == 0);
    CHECK(s.r.input_len == 1 && s.r.input[0] == 0x03);
    release(ui, &s, CUI_KEY_C, CUI_MOD_CTRL);
    select_hello(ui, &s);
    press(ui, &s, CUI_KEY_C, CUI_MOD_CTRL | CUI_MOD_SHIFT);   /* Ctrl+Shift+C too: no copy */
    CHECK(!s.r.copied); CHECK(g_clip[0] == 0);
    release(ui, &s, CUI_KEY_C, CUI_MOD_CTRL | CUI_MOD_SHIFT);
    press(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    CHECK(s.r.paste == NULL); CHECK(s.r.input_len == 1 && s.r.input[0] == 0x16);
    release(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    /* the app copies and pastes when it wants to */
    select_hello(ui, &s);
    CHECK(s.r.selected);
    s.do_copy = true; build(ui, &s);
    CHECK(s.r.copied); CHECK_STR(g_clip, "hello"); CHECK(!s.r.selected); CHECK_EQ(s.r.input_len, 0);
    g_clip[0] = 0;
    s.do_copy = true; build(ui, &s);                          /* nothing selected: nothing copied */
    CHECK(!s.r.copied); CHECK(g_clip[0] == 0);
    s.do_paste = true; build(ui, &s);
    CHECK(s.r.paste && !strcmp(s.r.paste, "pasted")); CHECK(!s.r.paste_bracketed);
    build(ui, &s);
    CHECK(s.r.paste == NULL);                                 /* one frame */
    W(s.vt, "\x1b[?2004h");
    s.do_paste = true; build(ui, &s);
    CHECK(s.r.paste && s.r.paste_bracketed);
    W(s.vt, "\x1b[?2004l");
    /* a key the app took first never reaches the program */
    cui_input_key(ui, CUI_KEY_V, true, CUI_MOD_CTRL);
    cui_key_consume(ui, CUI_KEY_V);
    build(ui, &s);
    CHECK_EQ(s.r.input_len, 0);
    release(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    /* no_input: no copy, no paste */
    select_hello(ui, &s);
    s.no_input = true;
    s.do_copy = true; build(ui, &s);
    CHECK(!s.r.copied);
    s.do_paste = true; build(ui, &s);
    CHECK(s.r.paste == NULL);
    s.no_input = false;
    /* key records (a console host): Ctrl+V goes as its record, not as a paste */
    s.w32 = true;
    W(s.vt, "\x1b[?9001h");
    build(ui, &s);
    press(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    bool rec = false;                                         /* ESC [ 86 ; ... _ : the V key's record */
    for (int i = 0; i + 4 <= s.r.input_len; i++) rec |= !memcmp(s.r.input + i, "[86;", 4);
    CHECK(s.r.paste == NULL); CHECK(rec);
    release(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    s.app_clip = false;
    press(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);                   /* without the option it is the view's paste again */
    CHECK(s.r.paste != NULL);
    release(ui, &s, CUI_KEY_V, CUI_MOD_CTRL);
    W(s.vt, "\x1b[?9001l");
    s.w32 = false;
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

/* ---- cost ---- */

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void test_cost(void) {
    const char *line = "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h(1234): warning C4996: 'strcpy': This function or variable may be unsafe. Consider using strcpy_s instead. See https://learn.microsoft.com/x for details.";
    int len = (int)strlen(line), n = 0;
    cui_vt_link l; char buf[512];
    double t0 = now_s();
    for (int i = 0; i < 200000; i++) n += cui_vt_link_find(line, len, (i * 7) % len, &l, buf, sizeof buf, NULL, NULL);
    double dt = now_s() - t0;
    printf("  cui_vt_link_find on a %d byte line: %.2f us a call (%d of 200000 found a link)\n", len, dt / 200000 * 1e6, n);
    CHECK(dt / 200000 < 50e-6);
    /* a frame of the view: the mouse moves every frame with Ctrl held (a detection a frame), against Ctrl up (none) */
    scene s;
    cui_ctx *ui = view_ctx(&s, 120);
    for (int i = 0; i < 40; i++) { W(s.vt, line); W(s.vt, "\r\n"); }
    build(ui, &s);
    double ft[2];
    int dets[2];
    for (int pass = 0; pass < 2; pass++) {
        mods(ui, &s, pass ? CUI_MOD_CTRL : 0);
        int d0 = cui__vt_links_detections();
        double a = now_s();
        for (int i = 0; i < 400; i++) at_cell(ui, &s, (i * 13) % 100, 1 + (i % 15));
        ft[pass] = (now_s() - a) / 400;
        dets[pass] = cui__vt_links_detections() - d0;
    }
    CHECK_EQ(dets[0], 0);
    CHECK(dets[1] >= 390);
    printf("  view frame (build, no render): %.1f us with Ctrl up, %.1f us with Ctrl held and a detection every frame (+%.1f us)\n",
           ft[0] * 1e6, ft[1] * 1e6, (ft[1] - ft[0]) * 1e6);
    cui_vt_destroy(s.vt);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    printf("detect\n"); test_detect();
    printf("osc8\n"); test_osc8();
    printf("view links\n"); test_view_links();
    printf("view click\n"); test_view_click();
    printf("select\n"); test_select();
    printf("app clipboard\n"); test_app_clipboard();
    printf("cost\n"); test_cost();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
