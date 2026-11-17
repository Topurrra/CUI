/* Tests for the config, markup and C-like languages of the syntax highlighter (src/cui_code_langs.c):
 * the token kinds of key tokens, the int state that carries multi-line constructs from line to line
 * (starting a line from its saved state, as the code editor does), and garbage input. */
#include "../src/cui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

enum { TEXT, KEYWORD, TYPE, STRING, NUMBER, COMMENT, FUNC, PREPROC, PUNCT, CONST };

/* the kind of the token that covers the first occurrence of `what` in s, -1 if none does; *state goes in and out */
static int kind_of(const char *lang, const char *s, int *state, const char *what) {
    cui__lang L = cui__lang_for(lang);
    cui__tok t[256];
    int n = cui__tokenize_st(&L, s, (int)strlen(s), state, t, 256);
    const char *at = strstr(s, what);
    int pos = at ? (int)(at - s) : -1;
    for (int i = 0; i < n; i++)
        if (pos >= t[i].start && pos < t[i].start + t[i].len) return t[i].kind;
    return -1;
}
static int k1(const char *lang, const char *s, const char *what) { int st = 0; return kind_of(lang, s, &st, what); }
/* the state after a line that starts at `st` */
static int after(const char *lang, const char *s, int st) { kind_of(lang, s, &st, s); return st; }

/* the tokens of a line cover it from the first byte to the last, without gaps or overlaps */
static bool covers(const char *lang, const char *s, int *state) {
    cui__lang L = cui__lang_for(lang);
    cui__tok t[512];
    int n = (int)strlen(s), c = cui__tokenize_st(&L, s, n, state, t, 512), pos = 0;
    for (int i = 0; i < c; i++) {
        if (t[i].start != pos || t[i].len <= 0) return false;
        pos += t[i].len;
    }
    return pos == n && c <= n + 1;
}

/* the whole text with the state carried, and again each line from its saved state: the same tokens */
static bool same_from_middle(const char *lang, const char *text) {
    cui__lang L = cui__lang_for(lang);
    int lines = 0, st[256];
    const char *p = text;
    st[0] = 0;
    cui__tok a[256];
    unsigned long sum_all = 0, sum_each = 0;
    while (*p && lines < 250) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p), state = st[lines];
        int c = cui__tokenize_st(&L, p, n, &state, a, 256);
        for (int i = 0; i < c; i++) sum_all = sum_all * 131 + (unsigned long)(a[i].start * 16 + a[i].len) * 16 + a[i].kind;
        st[++lines] = state;
        p = e ? e + 1 : p + n;
    }
    p = text;
    for (int l = 0; l < lines; l++) {
        const char *e = strchr(p, '\n');
        int n = e ? (int)(e - p) : (int)strlen(p), state = st[l];       /* the saved state: no line above is read */
        int c = cui__tokenize_st(&L, p, n, &state, a, 256);
        for (int i = 0; i < c; i++) sum_each = sum_each * 131 + (unsigned long)(a[i].start * 16 + a[i].len) * 16 + a[i].kind;
        if (state != st[l + 1]) return false;
        p = e ? e + 1 : p + n;
    }
    return sum_all == sum_each;
}

static void test_names(void) {
    static const struct { const char *name; int mode; } names[] = {
        { "yaml", CUI__LM_YAML }, { "yml", CUI__LM_YAML }, { "toml", CUI__LM_TOML }, { "ini", CUI__LM_INI }, { "cfg", CUI__LM_INI },
        { "conf", CUI__LM_INI }, { "properties", CUI__LM_INI }, { "xml", CUI__LM_XML }, { "svg", CUI__LM_XML }, { "xaml", CUI__LM_XML },
        { "html", CUI__LM_HTML }, { "css", CUI__LM_CSS }, { "scss", CUI__LM_CSS }, { "less", CUI__LM_CSS },
        { "powershell", CUI__LM_POWERSHELL }, { "ps1", CUI__LM_POWERSHELL }, { "pwsh", CUI__LM_POWERSHELL },
        { "csharp", CUI__LM_CSHARP }, { "cs", CUI__LM_CSHARP }, { "c#", CUI__LM_CSHARP }, { "java", CUI__LM_JAVA },
        { "batch", CUI__LM_BATCH }, { "bat", CUI__LM_BATCH }, { "cmd", CUI__LM_BATCH }, { "markdown", CUI__LM_MARKDOWN }, { "md", CUI__LM_MARKDOWN },
        { "c", 0 }, { "sql", 0 }, { "text", 0 }, { "nonsense", 0 },
    };
    for (int i = 0; i < (int)(sizeof names / sizeof *names); i++) CHECK(cui__lang_for(names[i].name).mode == names[i].mode);
    CHECK(cui__lang_for("scss").line_comment && !cui__lang_for("css").line_comment);
    CHECK(!cui__lang_for("ini").multi && !cui__lang_for("bat").multi && cui__lang_for("yaml").multi && cui__lang_for("md").multi);
}

static void test_yaml(void) {
    CHECK(k1("yaml", "name: value", "name") == FUNC && k1("yaml", "name: value", ":") == PUNCT && k1("yaml", "name: value", "value") == TEXT);
    CHECK(k1("yaml", "  nested_key: 42", "42") == NUMBER && k1("yaml", "a: 1.5e3", "1.5e3") == NUMBER && k1("yaml", "a: 0x1F", "0x1F") == NUMBER);
    CHECK(k1("yaml", "a: true", "true") == CONST && k1("yaml", "a: null", "null") == CONST && k1("yaml", "a: ~", "~") == CONST && k1("yaml", "a: No", "No") == CONST);
    CHECK(k1("yaml", "a: \"text # not a comment\"", "text") == STRING && k1("yaml", "a: 'it''s'", "it") == STRING);
    CHECK(k1("yaml", "\"quoted key\": 1", "quoted") == FUNC);
    CHECK(k1("yaml", "a: b # note", "note") == COMMENT && k1("yaml", "# whole line", "whole") == COMMENT && k1("yaml", "a: b#c", "b#c") == TEXT);
    CHECK(k1("yaml", "base: &anchor x", "&anchor") == TYPE && k1("yaml", "use: *anchor", "*anchor") == TYPE);
    CHECK(k1("yaml", "a: !!str 12", "!!str") == PREPROC && k1("yaml", "a: !!str 12", "12") == NUMBER);
    CHECK(k1("yaml", "---", "-") == PREPROC && k1("yaml", "...", ".") == PREPROC && k1("yaml", "--- # doc", "doc") == COMMENT);
    CHECK(k1("yaml", "- item", "-") == PUNCT && k1("yaml", "- item", "item") == TEXT && k1("yaml", "  - key: v", "key") == FUNC);
    CHECK(k1("yaml", "url: http://x.io/a", "url") == FUNC && k1("yaml", "url: http://x.io/a", "http") == TEXT);
    CHECK(k1("yaml", "a: {x: 1, y: [2, 3]}", "x") == FUNC && k1("yaml", "a: {x: 1, y: [2, 3]}", "3") == NUMBER);
    CHECK(k1("yaml", "- - a", "a") == TEXT);

    /* a block scalar: its lines are strings until a line is indented no deeper than the key */
    int st = 0;
    CHECK(kind_of("yaml", "text: |", &st, "|") == PUNCT && st != 0);
    CHECK(kind_of("yaml", "  # not a comment: here", &st, "not") == STRING && st != 0);
    CHECK(kind_of("yaml", "", &st, "") == -1 && st != 0);                    /* a blank line stays inside */
    CHECK(kind_of("yaml", "    deeper: still text", &st, "deeper") == STRING);
    CHECK(kind_of("yaml", "other: 1", &st, "other") == FUNC && st == 0);
    st = 0;
    CHECK(kind_of("yaml", "  key: >-", &st, ">-") == PUNCT && kind_of("yaml", "    folded", &st, "folded") == STRING);
    CHECK(kind_of("yaml", "    more", &st, "more") == STRING && kind_of("yaml", "  next: 1", &st, "next") == FUNC && st == 0);
    st = 0;
    CHECK(kind_of("yaml", "- |", &st, "|") == PUNCT && kind_of("yaml", "  body", &st, "body") == STRING && kind_of("yaml", "- b", &st, "-") == PUNCT && st == 0);
    CHECK(after("yaml", "a: |2+", 0) != 0 && after("yaml", "a: b | c", 0) == 0 && after("yaml", "a: \"|\"", 0) == 0);
    CHECK(after("yaml", "a: x # |", 0) == 0);
    /* the key's indent is in the state: a line at the same indent ends it, a deeper one does not */
    int deep = after("yaml", "      k: |", 0);
    CHECK(kind_of("yaml", "       text", &(int){ deep }, "text") == STRING && kind_of("yaml", "      other: 1", &(int){ deep }, "other") == FUNC);
    /* the bool callers (diff viewer): state 1 is a block scalar at indent 0 */
    CHECK(kind_of("yaml", "  text", &(int){ 1 }, "text") == STRING && kind_of("yaml", "k: v", &(int){ 1 }, "k") == FUNC);
    CHECK(same_from_middle("yaml", "a: 1\nb: |\n  x: y\n  # z\n\n  w\nc:\n  - |\n    q\n  - r: >\n     s\nd: 1\n"));
}

static void test_toml(void) {
    CHECK(k1("toml", "[server]", "server") == TYPE && k1("toml", "[server]", "[") == PUNCT && k1("toml", "[a.b.c]", "b") == TYPE);
    CHECK(k1("toml", "[[products]]", "products") == TYPE && k1("toml", "[[products]]", "[") == PUNCT);
    CHECK(k1("toml", "title = \"TOML\"", "title") == FUNC && k1("toml", "title = \"TOML\"", "=") == PUNCT && k1("toml", "title = \"TOML\"", "TOML") == STRING);
    CHECK(k1("toml", "a.b = 'lit'", "b") == FUNC && k1("toml", "a.b = 'lit'", "lit") == STRING && k1("toml", "\"q k\" = 1", "q") == FUNC);
    CHECK(k1("toml", "port = 8_080", "8_080") == NUMBER && k1("toml", "f = -1.5e-3", "1.5e-3") == NUMBER && k1("toml", "h = 0xDEAD", "0xDEAD") == NUMBER);
    CHECK(k1("toml", "d = 1979-05-27T07:32:00Z", "1979") == NUMBER && k1("toml", "d = 1979-05-27", "05") == NUMBER);
    CHECK(k1("toml", "on = true", "true") == CONST && k1("toml", "x = nan", "nan") == CONST && k1("toml", "x = -inf", "inf") == CONST);
    CHECK(k1("toml", "a = 1 # note", "note") == COMMENT && k1("toml", "# top", "top") == COMMENT && k1("toml", "a = \"# no\"", "no") == STRING);
    CHECK(k1("toml", "t = { x = 1, y = \"s\" }", "x") == FUNC && k1("toml", "t = { x = 1, y = \"s\" }", "s") == STRING);

    int st = 0;      /* """ strings */
    CHECK(kind_of("toml", "s = \"\"\"", &st, "\"\"\"") == STRING && st != 0);
    CHECK(kind_of("toml", "key = not a key # nor a comment", &st, "key") == STRING && st != 0);
    CHECK(kind_of("toml", "[not.a.table]", &st, "table") == STRING && st != 0);
    CHECK(kind_of("toml", "  \\\"\"\" escaped quotes", &st, "escaped") == STRING && st != 0);
    CHECK(kind_of("toml", "end\"\"\" # tail", &st, "end") == STRING && st == 0);
    CHECK(kind_of("toml", "end\"\"\" # tail", &(int){ 1 }, "tail") == COMMENT);
    st = 0;
    CHECK(after("toml", "s = '''", 0) == 2 && after("toml", "a'''", 2) == 0 && after("toml", "a \"\"\" b", 2) == 2);
    CHECK(after("toml", "s = \"\"\"one line\"\"\"", 0) == 0 && after("toml", "s = \"a\"\"\"", 0) == 0);
    /* an array that goes on: a line starting with [ is data, not a table */
    st = 0;
    CHECK(kind_of("toml", "a = [", &st, "[") == PUNCT && st != 0);
    CHECK(kind_of("toml", "  [1, 2],", &st, "1") == NUMBER && kind_of("toml", "  [1, 2],", &(int){ st }, "[") == PUNCT && st != 0);
    CHECK(kind_of("toml", "  \"s\", # c", &st, "s") == STRING && st != 0);
    CHECK(kind_of("toml", "]", &st, "]") == PUNCT && st == 0);
    CHECK(kind_of("toml", "[next]", &st, "next") == TYPE);
    CHECK(same_from_middle("toml", "[a]\nk = \"\"\"\nx = 1\n[b]\n\"\"\"\nl = [\n [1],\n \"\"\"\n[t]\n\"\"\",\n]\n[c]\nz = '''\n'''\n"));
}

static void test_ini(void) {
    CHECK(k1("ini", "[section]", "section") == TYPE && k1("ini", "[section]", "[") == PUNCT && k1("ini", "[ a b ]", "a b") == TYPE);
    CHECK(k1("ini", "key = value", "key") == FUNC && k1("ini", "key = value", "=") == PUNCT && k1("ini", "key = value", "value") == TEXT);
    CHECK(k1("ini", "key: value", "key") == FUNC && k1("ini", "key: value", ":") == PUNCT);
    CHECK(k1("ini", "; comment", "comment") == COMMENT && k1("ini", "# comment", "comment") == COMMENT && k1("ini", "  ; indented", "indented") == COMMENT);
    CHECK(k1("ini", "key = a ; b", "b") == TEXT && k1("ini", "path=C:\\x", "C") == TEXT);
    CHECK(k1("cfg", "just text", "just") == TEXT && k1("properties", "a.b.c=1", "a.b.c") == FUNC);
    CHECK(after("ini", "[x", 0) == 0);
}

static void test_xml(void) {
    CHECK(k1("xml", "<note id=\"1\">", "note") == KEYWORD && k1("xml", "<note id=\"1\">", "<") == PUNCT && k1("xml", "<note id=\"1\">", "id") == TYPE);
    CHECK(k1("xml", "<note id=\"1\">", "1") == STRING && k1("xml", "<note id=\"1\">", "=") == PUNCT && k1("xml", "<note id=\"1\">", ">") == PUNCT);
    CHECK(k1("xml", "<a href='x y'/>", "x y") == STRING && k1("xml", "<a href='x y'/>", "/>") == PUNCT && k1("xml", "</note>", "note") == KEYWORD);
    CHECK(k1("xml", "</note>", "</") == PUNCT && k1("xml", "<x:Name a:b=\"1\">", "x:Name") == KEYWORD && k1("xml", "<x:Name a:b=\"1\">", "a:b") == TYPE);
    CHECK(k1("xml", "<p>a &amp; b &#169; &#xA9;</p>", "&amp;") == PREPROC && k1("xml", "<p>a &amp; b &#169;</p>", "&#169;") == PREPROC);
    CHECK(k1("xml", "<p>a & b &c</p>", "& b") == TEXT && k1("xml", "<p>a < b</p>", "a ") == TEXT);
    CHECK(k1("xml", "<p>text</p>", "text") == TEXT && k1("xml", "<!-- c -->", "c") == COMMENT && k1("xml", "<!-- c --><a/>", "a") == KEYWORD);
    CHECK(k1("xml", "<?xml version=\"1.0\"?>", "xml") == PREPROC && k1("xml", "<!DOCTYPE html>", "DOCTYPE") == PREPROC);
    CHECK(k1("svg", "<svg width=\"1\"/>", "svg") == KEYWORD && k1("xaml", "<Grid Margin=\"1\"/>", "Margin") == TYPE);

    int st = 0;      /* a comment across lines */
    CHECK(kind_of("xml", "<a><!-- start", &st, "start") == COMMENT && st != 0);
    CHECK(kind_of("xml", "<b> still in", &st, "<b>") == COMMENT && st != 0);
    CHECK(kind_of("xml", "end --><c/>", &st, "end") == COMMENT && st == 0);
    CHECK(kind_of("xml", "end --><c/>", &(int){ 1 }, "c") == KEYWORD);
    /* CDATA */
    st = 0;
    CHECK(kind_of("xml", "<![CDATA[ a < b", &st, "a <") == STRING && st != 0);
    CHECK(kind_of("xml", "<tag> &amp;", &st, "tag") == STRING && st != 0);
    CHECK(kind_of("xml", "]]><d/>", &st, "d") == KEYWORD && st == 0);
    /* a tag across lines, with a value across lines */
    st = 0;
    CHECK(kind_of("xml", "<Button", &st, "Button") == KEYWORD && st != 0);
    CHECK(kind_of("xml", "    Content=\"two", &st, "Content") == TYPE && st != 0);
    CHECK(kind_of("xml", "    lines\" Width='5'", &st, "lines") == STRING && kind_of("xml", "Q", &(int){ st }, "Q") == TYPE);
    CHECK(kind_of("xml", "    Height=\"1\">text", &st, "Height") == TYPE && st == 0);
    st = after("xml", "<a b=\"x", 0);
    CHECK(kind_of("xml", "y\" z=\"1\"/>after", &st, "z") == TYPE && st == 0);
    CHECK(after("xml", "<!DOCTYPE a [", 0) != 0 && after("xml", "]>", after("xml", "<!DOCTYPE a [", 0)) == 0);
    /* html: script and style text is left plain */
    st = 0;
    CHECK(kind_of("html", "<script type=\"x\">if (a<b) {", &st, "if") == TEXT && st != 0);
    CHECK(kind_of("html", "var s = \"<div>\";", &st, "div") == TEXT && st != 0);
    CHECK(kind_of("html", "}</script><p>x</p>", &st, "script") == KEYWORD && st == 0);
    CHECK(after("html", "<style>", 0) != 0 && after("html", "<style/>", 0) == 0 && after("html", "</style>", 0) == 0);
    CHECK(after("html", "<script", 0) != 0 && after("html", " src=\"a\">", after("html", "<script", 0)) != 0);
    CHECK(after("html", "<script src=\"a\"></script>", 0) == 0 && after("xml", "<script>", 0) == 0);
    CHECK(after("html", "x</SCRIPT>", after("html", "<script>", 0)) == 0);
    CHECK(same_from_middle("xml", "<a\n b=\"1\n2\"\n c='3'>\n<!-- x\n y -->\n<![CDATA[\n<z>\n]]>\n<?pi\n?>\n<!DOCTYPE q [\n]>\n<e/>\n"));
    CHECK(same_from_middle("html", "<script\n src=a>\nif (a<b)\n</script>\n<style>\na{}\n</STYLE>\n<p>\n"));
}

static void test_css(void) {
    CHECK(k1("css", ".box { color: red; }", ".box") == TYPE && k1("css", "#id { }", "#id") == TYPE && k1("css", "a:hover { }", ":hover") == FUNC);
    CHECK(k1("css", "a:hover { }", "a") == KEYWORD && k1("css", "a::before { }", "::before") == FUNC && k1("css", "ul > li { }", ">") == PUNCT);
    CHECK(k1("css", "@media (min-width: 600px) {", "@media") == PREPROC && k1("css", "@media (min-width: 600px) {", "600px") == NUMBER);
    CHECK(k1("css", "@import url(\"a.css\");", "a.css") == STRING && k1("css", "@import url(\"a.css\");", "url") == FUNC);
    CHECK(k1("css", "50% { top: 0 }", "50%") == NUMBER);
    int st = 0;
    CHECK(kind_of("css", ".a {", &st, ".a") == TYPE && st != 0);
    CHECK(kind_of("css", "  color: #fff;", &st, "color") == FUNC && kind_of("css", "  color: #fff;", &(int){ st }, "#fff") == CONST);
    CHECK(kind_of("css", "  margin: -1.5em 10px 0 auto;", &st, "1.5em") == NUMBER && kind_of("css", "  margin: -1.5em 10px 0 auto;", &(int){ st }, "10px") == NUMBER);
    CHECK(kind_of("css", "  margin: -1.5em 10px 0 auto;", &(int){ st }, "auto") == TEXT && kind_of("css", "  margin: 0;", &(int){ st }, ";") == PUNCT);
    CHECK(kind_of("css", "  width: calc(100% - 2px) !important;", &(int){ st }, "calc") == FUNC && kind_of("css", "  width: calc(100% - 2px) !important;", &(int){ st }, "!important") == PREPROC);
    CHECK(kind_of("css", "  --main-color: red;", &(int){ st }, "--main-color") == FUNC && kind_of("css", "  a: var(--x);", &(int){ st }, "--x") == TEXT);
    CHECK(kind_of("css", "  content: \"a;b\";", &(int){ st }, "a;b") == STRING && kind_of("css", "  border: 1px solid #ABCDEF80;", &(int){ st }, "#ABCDEF80") == CONST);
    CHECK(kind_of("css", "  background: #12;", &(int){ st }, "#12") == PUNCT);
    CHECK(kind_of("css", "}", &st, "}") == PUNCT && st == 0);
    /* comments across lines, and what the line is read as after them */
    st = 0;
    CHECK(kind_of("css", "/* a { b", &st, "a {") == COMMENT && st != 0);
    CHECK(kind_of("css", "   c: d", &st, "c:") == COMMENT && st != 0);
    CHECK(kind_of("css", "*/ .x {", &st, ".x") == TYPE && st != 0);
    CHECK(kind_of("css", "  /* inline */ color: red;", &(int){ st }, "inline") == COMMENT && kind_of("css", "  /* inline */ color: red;", &(int){ st }, "color") == FUNC);
    /* a declaration that goes on, and a selector list that goes on */
    st = 0;
    CHECK(after("css", "a {", 0) != 0);
    int in = after("css", "a {", 0);
    CHECK(kind_of("css", "  font-family: Arial,", &(int){ in }, "font-family") == FUNC && after("css", "  font-family: Arial,", in) != in);
    CHECK(kind_of("css", "    \"Helvetica\";", &(int){ after("css", "  font-family: Arial,", in) }, "Helvetica") == STRING);
    CHECK(after("css", "    \"Helvetica\";", after("css", "  font-family: Arial,", in)) == in);
    CHECK(kind_of("css", "h2 {", &(int){ after("css", "h1,", 0) }, "h2") == KEYWORD && after("css", "h1,", 0) == 0);
    int in2 = after("css", "a {", in);                                   /* nested: scss */
    CHECK(kind_of("scss", "  &:hover {", &(int){ in }, ":hover") == FUNC && kind_of("scss", "  $c: red;", &(int){ in }, "$c") == TYPE);
    CHECK(kind_of("scss", "  // note { }", &(int){ in }, "note") == COMMENT && kind_of("css", "  // note", &(int){ in }, "note") != COMMENT);
    CHECK(kind_of("scss", "    color: blue;", &(int){ in2 }, "color") == FUNC && after("css", "}", in2) == in && after("css", "}", in) == 0);
    CHECK(kind_of("less", "@w: 10px;", &st, "@w") == PREPROC);
    CHECK(same_from_middle("css", "a,\nb {\n /* x\n { */ color: red;\n margin: 1px\n  2px;\n .c:hover {\n }\n}\n@media x {\n p {\n}\n}\n"));
}

static void test_powershell(void) {
    CHECK(k1("ps1", "if ($x -eq 1) { }", "if") == KEYWORD && k1("ps1", "IF ($x) { }", "IF") == KEYWORD && k1("ps1", "ForEach ($i in $l) {}", "ForEach") == KEYWORD);
    CHECK(k1("ps1", "$name = 1", "$name") == TYPE && k1("ps1", "${my var} = 1", "${my var}") == TYPE && k1("ps1", "$env:Path", "$env:Path") == TYPE);
    CHECK(k1("ps1", "$_ | x", "$_") == TYPE && k1("ps1", "$true", "$true") == CONST && k1("ps1", "$null", "$null") == CONST && k1("ps1", "$False", "$False") == CONST);
    CHECK(k1("ps1", "Get-ChildItem -Path .", "Get-ChildItem") == FUNC && k1("ps1", "Get-ChildItem -Path .", "-Path") == PREPROC);
    CHECK(k1("ps1", "$a -and $b", "-and") == KEYWORD && k1("ps1", "$a -NotLike 'x'", "-NotLike") == KEYWORD && k1("ps1", "$a -5", "-") == PUNCT);
    CHECK(k1("ps1", "Write-Host \"hi `\"there`\"\"", "there") == STRING && k1("ps1", "echo 'it''s'", "it") == STRING && k1("ps1", "echo 'it''s'", "s'") == STRING);
    CHECK(k1("ps1", "$x = 0x1F + 3.5kb", "0x1F") == NUMBER && k1("ps1", "$x = 0x1F + 3.5kb", "3.5kb") == NUMBER && k1("ps1", "1..5", "5") == NUMBER);
    CHECK(k1("ps1", "# comment", "comment") == COMMENT && k1("ps1", "$x # c", "c") == COMMENT && k1("ps1", "<# c #> $x", "c") == COMMENT && k1("ps1", "<# c #> $x", "$x") == TYPE);
    CHECK(k1("ps1", "[string]$s", "string") == TYPE && k1("ps1", "[Parameter(Mandatory)]", "Parameter") == TYPE && k1("ps1", "$a[0]", "0") == NUMBER);
    CHECK(k1("ps1", "[Math]::Abs(1)", "Abs") == FUNC && k1("ps1", "function Do-It {", "function") == KEYWORD);
    int st = 0;      /* <# #> */
    CHECK(kind_of("ps1", "<# help", &st, "help") == COMMENT && st != 0);
    CHECK(kind_of("ps1", ".SYNOPSIS $x -and", &st, "$x") == COMMENT && st != 0);
    CHECK(kind_of("ps1", "#> $y", &st, "#>") == COMMENT && st == 0);
    CHECK(kind_of("ps1", "#> $y", &(int){ 1 }, "$y") == TYPE);
    /* here-strings: the end is at the start of a line */
    st = 0;
    CHECK(kind_of("ps1", "$t = @\"", &st, "@\"") == STRING && st != 0);
    CHECK(kind_of("ps1", "line $x # not a comment", &st, "$x") == STRING && st != 0);
    CHECK(kind_of("ps1", "  \"@ indented, not the end", &st, "end") == STRING && st != 0);
    CHECK(kind_of("ps1", "\"@ ; Write-Host 1", &st, "Write-Host") == FUNC && st == 0);
    CHECK(after("ps1", "$t = @'", 0) == 3 && after("ps1", "\"@", 3) == 3 && after("ps1", "'@", 3) == 0);
    CHECK(after("ps1", "@\"x", 0) == 0 && after("ps1", "$h = @{", 0) == 0 && after("ps1", "$x = \"a <# b\"", 0) == 0);
    CHECK(same_from_middle("ps1", "a\n<#\n x\n#> $y\n$t = @\"\n$z\n\"@\nb = @'\n<#\n'@\n$k\n"));
}

static void test_csharp(void) {
    CHECK(k1("cs", "public class Foo : Bar {", "public") == KEYWORD && k1("cs", "public class Foo : Bar {", "Foo") == TYPE);
    CHECK(k1("cs", "int x = 5;", "int") == TYPE && k1("cs", "var s = null;", "null") == CONST && k1("cs", "foreach (var a in b)", "foreach") == KEYWORD);
    CHECK(k1("csharp", "Console.WriteLine(x);", "WriteLine") == FUNC && k1("c#", "x.y(1)", "y") == FUNC);
    CHECK(k1("cs", "int n = 0x1F_FF + 1_000L;", "0x1F_FF") == NUMBER && k1("cs", "var d = 1.5e-3f;", "1.5e-3f") == NUMBER && k1("cs", "var m = 3.14m;", "3.14m") == NUMBER);
    CHECK(k1("cs", "// c", "c") == COMMENT && k1("cs", "/// <summary>x</summary>", "summary") == COMMENT && k1("cs", "x /* c */ y", "c") == COMMENT);
    CHECK(k1("cs", "var s = \"a\\\"b\";", "b") == STRING && k1("cs", "var s = $\"x {y}\";", "y") == STRING && k1("cs", "var c = 'x';", "x") == STRING);
    CHECK(k1("cs", "var s = @\"C:\\a\";", "C:") == STRING && k1("cs", "var s = @\"a\"\"b\";", "b") == STRING && k1("cs", "var s = $@\"x\";", "x") == STRING);
    CHECK(k1("cs", "var s = @\"a\\\"; int q;", "int") == TYPE);
    CHECK(k1("cs", "[Obsolete(\"x\")]", "Obsolete") == PREPROC && k1("cs", "  [Serializable]", "Serializable") == PREPROC && k1("cs", "a[obsolete]", "obsolete") == TEXT);
    CHECK(k1("cs", "#if DEBUG", "#if") == PREPROC && k1("cs", "#region Name", "#region") == PREPROC && k1("cs", "x #if", "#if") == PUNCT);
    int st = 0;
    CHECK(kind_of("cs", "/* start", &st, "start") == COMMENT && st != 0 && kind_of("cs", "mid \"s", &st, "mid") == COMMENT && kind_of("cs", "*/ int x;", &st, "int") == TYPE && st == 0);
    st = 0;      /* verbatim */
    CHECK(kind_of("cs", "var s = @\"line one", &st, "line") == STRING && st != 0);
    CHECK(kind_of("cs", "  // not a comment \"\" still in", &st, "comment") == STRING && st != 0);
    CHECK(kind_of("cs", "end\"; int q;", &st, "int") == TYPE && st == 0);
    /* raw string """ */
    st = 0;
    CHECK(kind_of("cs", "var s = \"\"\"", &st, "\"\"\"") == STRING && st != 0 && kind_of("cs", "text \"quoted\" // x", &st, "x") == STRING);
    CHECK(kind_of("cs", "\"\"\"; int q;", &st, "int") == TYPE && st == 0);
    CHECK(after("cs", "var s = \"\"\"one\"\"\";", 0) == 0 && after("cs", "var e = \"\";", 0) == 0);
    CHECK(same_from_middle("cs", "a\n/* x\n y */ b\nvar s = @\"1\n\"\"2\n3\"; c\nvar r = \"\"\"\n// d\n\"\"\";\nint z;\n"));
}

static void test_java(void) {
    CHECK(k1("java", "public static void main(String[] args) {", "public") == KEYWORD && k1("java", "public static void main(String[] args) {", "void") == TYPE);
    CHECK(k1("java", "public static void main(String[] args) {", "main") == FUNC && k1("java", "String s = null;", "String") == TYPE && k1("java", "String s = null;", "null") == CONST);
    CHECK(k1("java", "@Override", "@Override") == PREPROC && k1("java", "@SuppressWarnings(\"x\")", "@SuppressWarnings") == PREPROC && k1("java", "@a.b.C x", "@a.b.C") == PREPROC);
    CHECK(k1("java", "@interface Foo", "interface") == KEYWORD && k1("java", "int x = 1_000_000;", "1_000_000") == NUMBER && k1("java", "long y = 0xFFL;", "0xFFL") == NUMBER);
    CHECK(k1("java", "double d = 1e-9d;", "1e-9d") == NUMBER && k1("java", "float f = 1.5F;", "1.5F") == NUMBER && k1("java", "int b = 0b1010_0101;", "0b1010_0101") == NUMBER);
    CHECK(k1("java", "// c", "c") == COMMENT && k1("java", "/** doc */ int x;", "doc") == COMMENT && k1("java", "/** doc */ int x;", "int") == TYPE);
    CHECK(k1("java", "char c = '\\'';", "'';") == STRING && k1("java", "var s = \"a\\\"b\";", "b") == STRING);
    CHECK(k1("java", "# not preproc", "#") == PUNCT);
    int st = 0;      /* text blocks */
    CHECK(kind_of("java", "String s = \"\"\"", &st, "\"\"\"") == STRING && st != 0);
    CHECK(kind_of("java", "    <html> // not a comment", &st, "html") == STRING && st != 0);
    CHECK(kind_of("java", "    say \\\"\"\" and \"x\"", &st, "x") == STRING && st != 0);
    CHECK(kind_of("java", "    \"\"\".trim(); int q;", &st, "trim") == FUNC && st == 0);
    st = 0;
    CHECK(kind_of("java", "/* c", &st, "c") == COMMENT && st != 0 && kind_of("java", "\"\"\" */ int", &st, "int") == TYPE && st == 0);
    CHECK(after("java", "String s = \"\"\"a\"\"\";", 0) == 0);
    CHECK(same_from_middle("java", "a\n/* x\n y */ b\nString s = \"\"\"\n  a \\\"\"\"\n\"\"\"; c\n/** d */\n@Foo\nint z;\n"));
}

static void test_batch(void) {
    CHECK(k1("bat", "@echo off", "echo") == KEYWORD && k1("bat", "@echo off", "@") == PUNCT && k1("bat", "ECHO off", "ECHO") == KEYWORD);
    CHECK(k1("bat", "echo for do if", "for") == TEXT && k1("bat", "echo a & set x=1", "set") == KEYWORD && k1("bat", "echo a | find b", "find") == KEYWORD);
    CHECK(k1("bat", "REM comment", "comment") == COMMENT && k1("bat", "rem", "rem") == COMMENT && k1("bat", "  Rem x", "x") == COMMENT && k1("bat", "remote", "remote") == TEXT);
    CHECK(k1("bat", ":: comment", "comment") == COMMENT && k1("bat", ":label", "label") == TYPE && k1("bat", ":label", ":") == TYPE);
    CHECK(k1("bat", "goto :eof", "goto") == KEYWORD && k1("bat", "set /a x=1+2", "/a") == PREPROC && k1("bat", "set /a x=1+2", "1") == NUMBER);
    CHECK(k1("bat", "echo %PATH%", "%PATH%") == TYPE && k1("bat", "echo !var!", "!var!") == TYPE && k1("bat", "echo %1 %*", "%1") == TYPE && k1("bat", "echo %1 %*", "%*") == TYPE);
    CHECK(k1("bat", "cd /d %~dp0", "%~dp0") == TYPE && k1("bat", "cd /d %~dp0", "/d") == PREPROC && k1("bat", "for %%i in (*) do", "%%i") == TYPE);
    CHECK(k1("bat", "set x=50%", "50") == NUMBER && k1("bat", "echo 100%% done", "%%") == PUNCT);
    CHECK(k1("bat", "echo \"a b\" c", "a b") == STRING && k1("bat", "if exist \"x\" (", "exist") == KEYWORD && k1("bat", "if exist \"x\" (", "(") == PUNCT);
    CHECK(k1("cmd", "call :sub arg", "call") == KEYWORD && k1("bat", "x2 y", "x2") == TEXT);
}

/* garbage: every language, odd bytes, long runs, cut off anywhere, any state: no crash, the tokens cover the line */
static void test_garbage(void) {
    static const char *const langs[] = { "yaml", "toml", "ini", "xml", "html", "css", "scss", "ps1", "cs", "java", "bat", "md" };
    static const char *const bits[] = { "\"", "'", "\"\"\"", "'''", "<!--", "-->", "<![CDATA[", "]]>", "<", ">", "</", "/>", "<?", "?>", "&", ";", "&amp;", "|", ">-", "---",
                                         "#", "<#", "#>", "@\"", "\"@", "@'", "$", "${", "{", "}", "[", "]", "[[", "(", ")", ":", ": ", "=", ",", "/*", "*/", "//",
                                         "\\", "`", "%", "%%", "!", "-", "- ", "@", "\t", " ", "  ", "\r", "\xff", "\xc3", "\x80", "a", "x1", "0x", "1.", "e+", ".", "if", "rem",
                                         "true", "null", "\xe2\x82\xac" };
    unsigned seed = 12345;
    for (int l = 0; l < (int)(sizeof langs / sizeof *langs); l++) {
        cui__lang L = cui__lang_for(langs[l]);
        for (int round = 0; round < 4000; round++) {
            char line[400];
            int n = 0;
            seed = seed * 1103515245u + 12345u;
            int parts = (int)(seed >> 16) % 20;
            for (int k = 0; k < parts; k++) {
                seed = seed * 1103515245u + 12345u;
                const char *b = bits[(seed >> 16) % (sizeof bits / sizeof *bits)];
                size_t bl = strlen(b);
                if (n + (int)bl < (int)sizeof line) { memcpy(line + n, b, bl); n += (int)bl; }
            }
            line[n] = 0;
            seed = seed * 1103515245u + 12345u;
            int state = (int)((seed >> 16) % 12) | ((int)((seed >> 8) % 7) << 8);   /* any state, even one the language never makes */
            cui__tok t[512];
            int c = cui__tokenize_st(&L, line, n, &state, t, 512), pos = 0;
            bool ok = c <= n + 1 && state >= 0;
            for (int i = 0; i < c; i++) { ok = ok && t[i].start == pos && t[i].len > 0 && t[i].start + t[i].len <= n && t[i].kind < CUI__TK_COUNT; pos += t[i].len; }
            ok = ok && pos == n;
            if (!ok) { g_checks++; g_failed++; printf("  FAIL %s garbage: \"%s\"\n", langs[l], line); break; }
            /* the state alone (cap 0) is the same as with tokens, and a tiny cap does not change it */
            int s1 = state, s2 = state, s3 = state, base = state;
            state = base;
            cui__tokenize_st(&L, line, n, &s1, NULL, 0);
            cui__tokenize_st(&L, line, n, &s2, t, 3);
            cui__tokenize_st(&L, line, n, &s3, t, 512);
            if (s1 != s3 || s2 != s3) { g_checks++; g_failed++; printf("  FAIL %s state differs with the cap: \"%s\"\n", langs[l], line); break; }
            /* the bool path of the other callers */
            bool blk = (seed & 1) != 0;
            cui__tokenize(&L, false, line, n, &blk, t, 512);
        }
        g_checks++;
    }
    /* empty lines and one very long line */
    static char big[100000];
    memset(big, 'a', sizeof big - 1);
    for (int l = 0; l < (int)(sizeof langs / sizeof *langs); l++) {
        int st = 0;
        CHECK(covers(langs[l], "", &st));
        st = 0;
        big[sizeof big - 1] = 0;
        cui__lang L = cui__lang_for(langs[l]);
        static cui__tok t[100002];
        int c = cui__tokenize_st(&L, big, (int)sizeof big - 1, &st, t, 100002);
        CHECK(c >= 1);
        st = 0;
        memset(big, '<', 5000);
        c = cui__tokenize_st(&L, big, 5000, &st, t, 100002);
        CHECK(c >= 1);
        memset(big, 'a', 5000);
    }
}

/* every line of every language covers its text, with states carried through a small sample */
static void test_coverage(void) {
    static const struct { const char *lang, *text; } samples[] = {
        { "yaml", "# c\nkey: [a, b]\nlist:\n\t- x: 'y'\n  - |\n\t t\n" }, { "toml", "[a]\nb = [\"c\",\n\t'd']\n" },
        { "xml", "<a\tb='1'>\t&amp;\t</a>\n" }, { "css", "a\t{\tcolor:\tred\t}\n" }, { "ps1", "\tWrite-Host\t$x\t# c\n" },
        { "cs", "\tvar s = @\"\ta\n\tb\";\n" }, { "java", "\tString s = \"\"\"\n\ta\n\t\"\"\";\n" }, { "bat", "\techo\t%x%\n" }, { "ini", "\t[a]\n\tb\t=\t1\n" },
    };
    for (int k = 0; k < (int)(sizeof samples / sizeof *samples); k++) {
        const char *p = samples[k].text;
        int st = 0;
        while (*p) {
            const char *e = strchr(p, '\n');
            char line[200];
            int n = (int)(e - p);
            memcpy(line, p, (size_t)n);
            line[n] = 0;
            CHECK(covers(samples[k].lang, line, &st));
            p = e + 1;
        }
    }
    /* tab runs are tokens of their own, so the code block can draw them as spaces */
    cui__lang L = cui__lang_for("yaml");
    cui__tok t[16];
    int st = 0, n = cui__tokenize_st(&L, "a:\t\tb", 5, &st, t, 16), tabs = 0;
    for (int i = 0; i < n; i++) if (t[i].start == 2) tabs = t[i].len;
    CHECK(tabs == 2);
}

int main(void) {
    test_names();
    test_yaml();
    test_toml();
    test_ini();
    test_xml();
    test_css();
    test_powershell();
    test_csharp();
    test_java();
    test_batch();
    test_coverage();
    test_garbage();
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
