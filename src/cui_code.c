/* Code block with syntax highlighting, and a JSON viewer. */
#include "cui_internal.h"


/* ------------------------------------------------------------------------ */
/* Tokenizer                                                                */
/* ------------------------------------------------------------------------ */

enum { TK_TEXT, TK_KEYWORD, TK_TYPE, TK_STRING, TK_NUMBER, TK_COMMENT, TK_FUNC, TK_PREPROC, TK_PUNCT, TK_CONST, TK_COUNT };

typedef char cui__tk_count_check[TK_COUNT == CUI__TK_COUNT ? 1 : -1];   /* cui__lang and cui__tok are in cui_internal.h */

static const char *const kw_c[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return", "goto",
                                    "typedef", "struct", "union", "enum", "static", "const", "extern", "volatile", "inline", "sizeof",
                                    "class", "public", "private", "protected", "namespace", "template", "typename", "new", "delete",
                                    "using", "virtual", "override", "auto", "constexpr", "nullptr", "this", "operator", "try", "catch", "throw", NULL };
static const char *const ty_c[] = { "void", "char", "short", "int", "long", "float", "double", "bool", "unsigned", "signed", "size_t",
                                    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t", "int16_t", "int32_t", "int64_t", "FILE", NULL };
static const char *const kw_js[] = { "if", "else", "for", "while", "do", "switch", "case", "default", "break", "continue", "return",
                                     "function", "const", "let", "var", "new", "class", "extends", "import", "export", "from", "async",
                                     "await", "try", "catch", "throw", "typeof", "instanceof", "in", "of", "this", "yield", "interface",
                                     "type", "implements", "public", "private", "readonly", "as", NULL };
static const char *const ty_js[] = { "string", "number", "boolean", "any", "unknown", "never", "void", "Promise", "Array", "Record", NULL };
static const char *const kw_py[] = { "if", "elif", "else", "for", "while", "def", "class", "return", "import", "from", "as", "with",
                                     "try", "except", "finally", "raise", "lambda", "yield", "pass", "break", "continue", "in", "is",
                                     "not", "and", "or", "global", "nonlocal", "async", "await", "del", "assert", NULL };
static const char *const ty_py[] = { "int", "float", "str", "bool", "list", "dict", "tuple", "set", "bytes", "self", NULL };
static const char *const kw_rs[] = { "fn", "let", "mut", "if", "else", "match", "for", "while", "loop", "return", "struct", "enum",
                                     "impl", "trait", "pub", "use", "mod", "crate", "self", "Self", "where", "as", "ref", "move",
                                     "async", "await", "const", "static", "unsafe", "dyn", "in", "break", "continue", NULL };
static const char *const ty_rs[] = { "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "usize", "isize", "bool",
                                     "char", "str", "String", "Vec", "Option", "Result", "Box", NULL };
static const char *const kw_go[] = { "func", "package", "import", "var", "const", "type", "struct", "interface", "map", "chan", "if",
                                     "else", "for", "range", "switch", "case", "default", "return", "break", "continue", "go", "defer",
                                     "select", "fallthrough", "goto", NULL };
static const char *const ty_go[] = { "int", "int64", "int32", "uint", "uint64", "float64", "float32", "string", "bool", "byte", "rune", "error", NULL };
static const char *const kw_sh[] = { "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while", "case", "esac", "function",
                                     "return", "export", "local", "echo", "cd", "sudo", "cmake", "make", "git", "apt", "brew", NULL };
static const char *const none[] = { NULL };

cui__lang cui__lang_for(const char *lang) {
    cui__lang l = { none, none, NULL, NULL, false, false, false, false, false, false };
    if (!lang) return l;
    if (!strcmp(lang, "c") || !strcmp(lang, "cpp") || !strcmp(lang, "c++") || !strcmp(lang, "h")) {
        l.kw = kw_c; l.types = ty_c; l.line_comment = "//"; l.block_comments = true; l.hash_preproc = true;
    } else if (!strcmp(lang, "js") || !strcmp(lang, "ts") || !strcmp(lang, "javascript") || !strcmp(lang, "typescript")) {
        l.kw = kw_js; l.types = ty_js; l.line_comment = "//"; l.block_comments = true; l.backtick = true;
    } else if (!strcmp(lang, "python") || !strcmp(lang, "py")) {
        l.kw = kw_py; l.types = ty_py; l.line_comment = "#";
    } else if (!strcmp(lang, "rust") || !strcmp(lang, "rs")) {
        l.kw = kw_rs; l.types = ty_rs; l.line_comment = "//"; l.block_comments = true;
    } else if (!strcmp(lang, "go")) {
        l.kw = kw_go; l.types = ty_go; l.line_comment = "//"; l.block_comments = true; l.backtick = true;
    } else if (!strcmp(lang, "shell") || !strcmp(lang, "sh") || !strcmp(lang, "bash")) {
        l.kw = kw_sh; l.line_comment = "#"; l.no_caps_types = true;
    } else if (!strcmp(lang, "markdown") || !strcmp(lang, "md")) {
        l.markdown = true;
    } else if (!strcmp(lang, "text") || !strcmp(lang, "plain")) {
        l.plain = true;
    }
    return l;
}

static bool cui__in_list(const char *const *list, const char *s, int n) {
    for (int k = 0; list[k]; k++) if ((int)strlen(list[k]) == n && strncmp(list[k], s, (size_t)n) == 0) return true;
    return false;
}
static bool cui__ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (unsigned char)c >= 0x80; }
static bool cui__ident_char(char c) { return cui__ident_start(c) || (c >= '0' && c <= '9'); }

/* ---- markdown ---------------------------------------------------------- */

/* The multi-line state of a markdown line: 0 none; MD_OPEN a block of unknown
 * kind (all that the bool of the other callers can say); MD_FENCE (fence char
 * and length in the upper bits); MD_FM front matter; MD_CMT an HTML comment.
 * CUI__MD_START marks the first line, where front matter can begin. */
enum { MD_NONE, MD_OPEN, MD_FENCE, MD_FM, MD_CMT };

typedef struct { cui__tok *out; int cap, count; } cui__mdc;

static void cui__md_e(cui__mdc *m, int st, int len, int kind) {
    if (len > 0 && m->count < m->cap) { m->out[m->count].start = st; m->out[m->count].len = len; m->out[m->count].kind = (uint8_t)kind; m->count++; }
}
static bool cui__md_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool cui__md_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80; }
static int cui__md_trim(const char *s, int i, int n) { while (n > i && cui__md_ws(s[n - 1])) n--; return n; }

/* a run of three or more ` or ~ at i (at most three spaces in): its length, 0 if none */
static int cui__md_fence(const char *s, int i, int n, char *ch) {
    if (i > 3 || i >= n || (s[i] != '`' && s[i] != '~')) return 0;
    int k = i;
    while (k < n && s[k] == s[i]) k++;
    *ch = s[i];
    return k - i >= 3 ? k - i : 0;
}
static bool cui__md_is_hr(const char *s, int i, int n) {
    int e = cui__md_trim(s, i, n), cnt = 0;
    if (i > 3 || i >= e || (s[i] != '-' && s[i] != '*' && s[i] != '_')) return false;
    for (int k = i; k < e; k++) { if (s[k] == s[i]) cnt++; else if (s[k] != ' ' && s[k] != '\t') return false; }
    return cnt >= 3;
}
/* the line from i is exactly w, up to trailing white space */
static bool cui__md_is(const char *s, int i, int n, const char *w) {
    int e = cui__md_trim(s, i, n), l = (int)strlen(w);
    return e - i == l && strncmp(s + i, w, (size_t)l) == 0;
}
static bool cui__md_has(const char *s, int i, int n, const char *w) { return i + (int)strlen(w) <= n && strncmp(s + i, w, strlen(w)) == 0; }

/* the start of the closer of an emphasis run (c, k long) searched in [from, to), or -1 */
static int cui__md_close(const char *s, int from, int to, char c, int k) {
    for (int j = from; j < to; j++) {
        if (s[j] == '\\') { j++; continue; }
        if (s[j] != c) continue;
        int r = j;
        while (r < to && s[r] == c) r++;
        if (r - j == k && j > from && !cui__md_ws(s[j - 1]) && (c != '_' || r >= to || !cui__md_alnum(s[r]))) return j;
        j = r - 1;
    }
    return -1;
}

/* the ] that matches the [ at i, or -1 */
static int cui__md_bracket(const char *s, int i, int to) {
    int d = 0;
    for (int j = i; j < to; j++) {
        if (s[j] == '\\') j++;
        else if (s[j] == '[') d++;
        else if (s[j] == ']' && --d == 0) return j;
    }
    return -1;
}

/* [[note]], [[note|label]] and ![[embed]] starting at i, whose [[ is at b: the end, or -1 */
static int cui__md_wiki(cui__mdc *m, const char *s, int i, int b, int to) {
    int j = b + 2, bar = -1;
    while (j + 1 < to && !(s[j] == ']' && s[j + 1] == ']') && s[j] != '[') { if (s[j] == '|' && bar < 0) bar = j; j++; }
    if (j + 1 >= to || s[j] != ']' || j == b + 2) return -1;
    cui__md_e(m, i, b + 2 - i, TK_PUNCT);
    if (bar < 0) cui__md_e(m, b + 2, j - b - 2, TK_FUNC);
    else {
        cui__md_e(m, b + 2, bar - b - 2, TK_COMMENT);
        cui__md_e(m, bar, 1, TK_PUNCT);
        cui__md_e(m, bar + 1, j - bar - 1, TK_FUNC);
    }
    cui__md_e(m, j, 2, TK_PUNCT);
    return j + 2;
}

/* [text](url) and ![alt](src) starting at i, whose [ is at b: the end, or -1 */
static int cui__md_link(cui__mdc *m, const char *s, int i, int b, int to) {
    int j = cui__md_bracket(s, b, to), d = 0, k;
    if (j < 0 || j + 1 >= to || s[j + 1] != '(') return -1;
    for (k = j + 1; k < to; k++) { if (s[k] == '(') d++; else if (s[k] == ')' && --d == 0) break; }
    if (k >= to) return -1;
    cui__md_e(m, i, b + 1 - i, TK_PUNCT);
    cui__md_e(m, b + 1, j - b - 1, TK_FUNC);
    cui__md_e(m, j, 2, TK_PUNCT);
    cui__md_e(m, j + 2, k - j - 2, TK_COMMENT);
    cui__md_e(m, k, 1, TK_PUNCT);
    return k + 1;
}

/* inline spans in [i, to) */
static void cui__md_inline(cui__mdc *m, const char *s, int i, int to, bool table, int *state) {
    int ts = i;
#define FLUSH(at) cui__md_e(m, ts, (at) - ts, TK_TEXT)
    while (i < to) {
        char c = s[i];
        if (c == '\\' && i + 1 < to) { i += 2; continue; }
        if (c == '`') {
            int k = i, end = -1;
            while (k < to && s[k] == '`') k++;
            for (int j = k; j < to && end < 0; j++) {
                if (s[j] != '`') continue;
                int r = j;
                while (r < to && s[r] == '`') r++;
                if (r - j == k - i) end = r;
                j = r - 1;
            }
            if (end < 0) { i = k; continue; }
            FLUSH(i); cui__md_e(m, i, end - i, TK_STRING); i = ts = end;
            continue;
        }
        if (c == '<' && cui__md_has(s, i, to, "<!--")) {
            int j = i + 4;
            while (j < to && !cui__md_has(s, j, to, "-->")) j++;
            FLUSH(i);
            if (j < to) { cui__md_e(m, i, j + 3 - i, TK_COMMENT); i = ts = j + 3; continue; }
            cui__md_e(m, i, to - i, TK_COMMENT);
            *state = MD_CMT;
            return;
        }
        if (c == '[' || (c == '!' && i + 1 < to && s[i + 1] == '[')) {
            int b = c == '!' ? i + 1 : i, end;
            int hold = m->count;
            FLUSH(i);
            end = b + 1 < to && s[b + 1] == '[' ? cui__md_wiki(m, s, i, b, to) : cui__md_link(m, s, i, b, to);
            if (end >= 0) { i = ts = end; continue; }
            m->count = hold;      /* not a link: plain text */
            i = b + 1;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            int k = i;
            while (k < to && s[k] == c) k++;
            int run = k - i, cl = -1;
            if ((c == '~' ? run == 2 : run <= 3) && k < to && !cui__md_ws(s[k]) && (c != '_' || i == 0 || !cui__md_alnum(s[i - 1])))
                cl = cui__md_close(s, k, to, c, run);
            if (cl < 0) { i = k; continue; }
            FLUSH(i);
            cui__md_e(m, i, run, TK_PUNCT);
            cui__md_e(m, k, cl - k, c == '~' ? TK_COMMENT : TK_TYPE);
            cui__md_e(m, cl, run, TK_PUNCT);
            i = ts = cl + run;
            continue;
        }
        if (c == '|' && table) { FLUSH(i); cui__md_e(m, i, 1, TK_PUNCT); i = ts = i + 1; continue; }
        i++;
    }
    FLUSH(to);
#undef FLUSH
}

/* a front matter line: key: value */
static void cui__md_fm_line(cui__mdc *m, const char *s, int i, int n) {
    int k = i;
    if (i < n && s[i] == '#') { cui__md_e(m, i, n - i, TK_COMMENT); return; }
    if (i < n && s[i] == '-' && (i + 1 == n || s[i + 1] == ' ')) { cui__md_e(m, i, 1, TK_PUNCT); cui__md_e(m, i + 1, n - i - 1, TK_TEXT); return; }
    while (k < n && (cui__md_alnum(s[k]) || s[k] == '_' || s[k] == '-' || s[k] == '.')) k++;
    if (k > i && k < n && s[k] == ':') {
        cui__md_e(m, i, k - i, TK_FUNC);
        cui__md_e(m, k, 1, TK_PUNCT);
        cui__md_e(m, k + 1, n - k - 1, TK_TEXT);
        return;
    }
    cui__md_e(m, i, n - i, TK_TEXT);
}

/* Tokenize one markdown line. *state is the multi-line state between lines. */
int cui__tokenize_md(const char *s, int n, int *state, cui__tok *out, int cap) {
    cui__mdc m = { out, cap, 0 };
    int i = 0, st = *state & 15, e;
    char ch;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    cui__md_e(&m, 0, i, TK_TEXT);
    e = cui__md_trim(s, i, n);
    if (st == CUI__MD_START) {
        if (i == 0 && cui__md_is(s, i, n, "---")) { cui__md_e(&m, i, n - i, TK_COMMENT); *state = MD_FM; return m.count; }
        st = MD_NONE;
    }
    if (st == MD_FM || (st == MD_OPEN && cui__md_is(s, i, n, "---"))) {
        if (cui__md_is(s, i, n, "---") || cui__md_is(s, i, n, "...")) { cui__md_e(&m, i, n - i, TK_COMMENT); *state = MD_NONE; }
        else { cui__md_fm_line(&m, s, i, n); *state = MD_FM; }
        return m.count;
    }
    if (st == MD_FENCE || st == MD_OPEN) {
        int len = cui__md_fence(s, i, e, &ch);
        bool close = len > 0 && e - i == len && (st == MD_OPEN || (ch == ((*state & 16) ? '~' : '`') && len >= (*state >> 8)));
        if (close) { cui__md_e(&m, i, n - i, TK_COMMENT); *state = MD_NONE; return m.count; }
        if (st == MD_OPEN) {      /* maybe the end of an HTML comment */
            for (int k = i; k + 2 < n; k++)
                if (strncmp(s + k, "-->", 3) == 0 && !memchr(s + i, '`', (size_t)(k - i))) {
                    cui__md_e(&m, i, k + 3 - i, TK_COMMENT);
                    *state = MD_NONE;
                    cui__md_inline(&m, s, k + 3, n, false, state);
                    return m.count;
                }
        }
        cui__md_e(&m, i, n - i, TK_STRING);
        return m.count;      /* the state stays */
    }
    if (st == MD_CMT) {
        int k = i;
        while (k < n && !cui__md_has(s, k, n, "-->")) k++;
        if (k < n) {
            cui__md_e(&m, i, k + 3 - i, TK_COMMENT);
            *state = MD_NONE;
            cui__md_inline(&m, s, k + 3, n, false, state);
        } else cui__md_e(&m, i, n - i, TK_COMMENT);
        return m.count;
    }
    *state = MD_NONE;
    {   /* a fence opens */
        int len = cui__md_fence(s, i, n, &ch);
        if (len && !(ch == '`' && memchr(s + i + len, '`', (size_t)(n - i - len)))) {
            cui__md_e(&m, i, n - i, TK_COMMENT);
            *state = MD_FENCE | (ch == '~' ? 16 : 0) | (len << 8);
            return m.count;
        }
    }
    if (i <= 3 && i < n && s[i] == '#') {
        int k = i;
        while (k < n && s[k] == '#') k++;
        if (k - i <= 6 && (k == n || cui__md_ws(s[k]))) { cui__md_e(&m, i, n - i, TK_KEYWORD); return m.count; }
    }
    if (cui__md_is_hr(s, i, n)) { cui__md_e(&m, i, n - i, TK_PUNCT); return m.count; }
    while (i < n && s[i] == '>') {      /* quote markers */
        int k = i + 1;
        cui__md_e(&m, i, 1, TK_PUNCT);
        while (k < n && s[k] == ' ') k++;
        cui__md_e(&m, i + 1, k - i - 1, TK_TEXT);
        i = k;
    }
    if (i + 1 < n && s[i] == '[' && s[i + 1] == '!') {      /* a callout head */
        int k = i;
        while (k < n && s[k] != ']') k++;
        if (k < n) {
            k++;
            if (k < n && (s[k] == '+' || s[k] == '-')) k++;
            cui__md_e(&m, i, k - i, TK_PREPROC);
            cui__md_inline(&m, s, k, n, false, state);
            return m.count;
        }
    }
    {   /* a list item */
        int k = i;
        while (k < n && s[k] >= '0' && s[k] <= '9') k++;
        bool num = k > i && k - i <= 9 && k < n && (s[k] == '.' || s[k] == ')');
        if (num) k++;
        else k = i < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') ? i + 1 : -1;
        if (k > 0 && k < n && s[k] == ' ') {
            int a = k;
            while (a < n && s[a] == ' ') a++;
            cui__md_e(&m, i, k - i, TK_PUNCT);
            cui__md_e(&m, k, a - k, TK_TEXT);
            i = a;
            if (i + 2 < n && s[i] == '[' && (s[i + 1] == ' ' || s[i + 1] == 'x' || s[i + 1] == 'X') && s[i + 2] == ']' && (i + 3 == n || s[i + 3] == ' ')) {
                cui__md_e(&m, i, 3, TK_CONST);
                i += 3;
            }
        }
    }
    bool table = i < n && s[i] == '|';
    if (table) {      /* a delimiter row |---|:-:| is all punctuation */
        bool dash = false, only = true;
        for (int k = i; k < e; k++) { if (s[k] == '-') dash = true; else if (!strchr("|: \t", s[k])) only = false; }
        if (dash && only) { cui__md_e(&m, i, n - i, TK_PUNCT); return m.count; }
    }
    cui__md_inline(&m, s, i, n, table, state);
    return m.count;
}

/* Tokenize one line. *in_block carries an open block comment between lines. */
int cui__tokenize(const cui__lang *L, bool json, const char *s, int n, bool *in_block, cui__tok *out, int cap) {
    int count = 0, i = 0;
#define EMIT(st, ln, k) do { if (count < cap) { out[count].start = (st); out[count].len = (ln); out[count].kind = (uint8_t)(k); count++; } } while (0)
    if (L->plain) {
        /* leading white space is its own token, as in the other languages: the
         * code editor skips white space tokens, and an indented line must not vanish */
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i > 0) EMIT(0, i, TK_TEXT);
        if (i < n) EMIT(i, n - i, TK_TEXT);
        return count;
    }
    if (L->markdown) {      /* the bool can only say that a block is open */
        int ms = *in_block ? 1 : 0;
        count = cui__tokenize_md(s, n, &ms, out, cap);
        *in_block = ms != 0;
        return count;
    }
    while (i < n) {
        if (*in_block) {
            int st = i;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) i++;
            if (i < n) { i += 2; *in_block = false; }
            EMIT(st, i - st, TK_COMMENT);
            continue;
        }
        char c = s[i];
        if (c == ' ' || c == '\t') { int st = i; while (i < n && (s[i] == ' ' || s[i] == '\t')) i++; EMIT(st, i - st, TK_TEXT); continue; }
        if (L->line_comment && strncmp(s + i, L->line_comment, strlen(L->line_comment)) == 0) { EMIT(i, n - i, TK_COMMENT); break; }
        if (L->block_comments && c == '/' && i + 1 < n && s[i + 1] == '*') { *in_block = true; continue; }
        if (L->hash_preproc && c == '#') {
            int st = i;
            i++;
            while (i < n && cui__ident_char(s[i])) i++;
            EMIT(st, i - st, TK_PREPROC);
            continue;
        }
        if (c == '"' || c == '\'' || (c == '`' && L->backtick)) {
            int st = i;
            i++;
            while (i < n && s[i] != c) { if (s[i] == '\\' && i + 1 < n) i++; i++; }
            if (i < n) i++;
            /* JSON keys are strings followed by a colon */
            int k = i;
            while (k < n && s[k] == ' ') k++;
            EMIT(st, i - st, json && k < n && s[k] == ':' ? TK_FUNC : TK_STRING);
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '-' && json && i + 1 < n && s[i + 1] >= '0' && s[i + 1] <= '9')) {
            int st = i;
            i++;
            while (i < n && (cui__ident_char(s[i]) || s[i] == '.')) i++;
            EMIT(st, i - st, TK_NUMBER);
            continue;
        }
        if (cui__ident_start(c)) {
            int st = i;
            while (i < n && cui__ident_char(s[i])) i++;
            int len = i - st;
            int kind = TK_TEXT;
            if (json) kind = TK_CONST;
            else if (cui__in_list(L->kw, s + st, len)) kind = TK_KEYWORD;
            else if (cui__in_list(L->types, s + st, len)) kind = TK_TYPE;
            else if ((len == 4 && (!strncmp(s + st, "true", 4) || !strncmp(s + st, "NULL", 4) || !strncmp(s + st, "None", 4) || !strncmp(s + st, "null", 4))) ||
                     (len == 5 && (!strncmp(s + st, "false", 5) || !strncmp(s + st, "False", 5))) || (len == 4 && !strncmp(s + st, "True", 4)))
                kind = TK_CONST;
            else {
                int k = i;
                while (k < n && s[k] == ' ') k++;
                if (k < n && s[k] == '(') kind = TK_FUNC;
                else if (!L->no_caps_types && s[st] >= 'A' && s[st] <= 'Z' && len > 1) kind = TK_TYPE;
            }
            EMIT(st, len, kind);
            continue;
        }
        EMIT(i, 1, TK_PUNCT);
        i++;
    }
#undef EMIT
    return count;
}

void cui__syntax_colors(cui_ctx *ui, cui_color *c) {
    bool dark = ui->mode == CUI_THEME_DARK;
    c[TK_TEXT] = CUI_C(ui, FOREGROUND);
    c[TK_KEYWORD] = dark ? cui_oklch(0.76f, 0.14f, 305, 1) : cui_oklch(0.52f, 0.20f, 305, 1);
    c[TK_TYPE] = dark ? cui_oklch(0.80f, 0.11f, 195, 1) : cui_oklch(0.52f, 0.11f, 200, 1);
    c[TK_STRING] = dark ? cui_oklch(0.80f, 0.13f, 145, 1) : cui_oklch(0.52f, 0.14f, 150, 1);
    c[TK_NUMBER] = dark ? cui_oklch(0.80f, 0.13f, 60, 1) : cui_oklch(0.58f, 0.16f, 50, 1);
    c[TK_COMMENT] = CUI_C(ui, TEXT_MUTED);
    c[TK_FUNC] = dark ? cui_oklch(0.78f, 0.12f, 250, 1) : cui_oklch(0.50f, 0.17f, 255, 1);
    c[TK_PREPROC] = dark ? cui_oklch(0.76f, 0.12f, 350, 1) : cui_oklch(0.52f, 0.18f, 355, 1);
    c[TK_PUNCT] = CUI_C(ui, TEXT_SECONDARY);
    c[TK_CONST] = dark ? cui_oklch(0.80f, 0.13f, 60, 1) : cui_oklch(0.58f, 0.16f, 50, 1);
}

/* the highlighter by language name, for other components (the diff viewer) */
int cui__tokenize_lang(const char *lang, const char *s, int n, bool *in_block, cui__tok *out, int cap) {
    bool json = lang && !strcmp(lang, "json");
    cui__lang L = cui__lang_for(json ? NULL : lang);
    return cui__tokenize(&L, json, s, n, in_block, out, cap < 256 ? cap : 256);
}

float cui__draw_code_line(cui_ctx *ui, const char *lang, const char *s, int n, bool *in_block, float x, float y, float size, const cui_color *colors, float max_x) {
    if (!lang) {
        cui__draw_text_line(ui, CUI_FONT_MONO, size, x, y, colors[TK_TEXT], s, n, true);
        return x + cui__text_width(ui, CUI_FONT_MONO, size, s, n, true);
    }
    cui__tok tk[256];
    int nt = cui__tokenize_lang(lang, s, n, in_block, tk, 256), pos = 0;
    for (int t = 0; t < nt && x <= max_x; t++) {
        if (tk[t].start > pos) x += cui__text_width(ui, CUI_FONT_MONO, size, s + pos, tk[t].start - pos, true);
        cui__draw_text_line(ui, CUI_FONT_MONO, size, x, y, colors[tk[t].kind], s + tk[t].start, tk[t].len, true);
        x += cui__text_width(ui, CUI_FONT_MONO, size, s + tk[t].start, tk[t].len, true);
        pos = tk[t].start + tk[t].len;
    }
    return x;
}

/* ------------------------------------------------------------------------ */
/* Code block                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
    const char *code;
    int len;
    cui__lang lang;
    bool json;
    bool line_numbers;
    int hl_from, hl_to;
    float size, lh, gutter, pad;
    cui_color colors[TK_COUNT], gutter_c, hl_c;
} cui__code_paint;

static void cui__paint_code(cui_ctx *ui, cui_node *n, void *data) {
    cui__code_paint *p = data;
    cui_rect r = n->rect;
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, p->size);
    float x0 = r.x + p->pad + p->gutter, y = r.y + p->pad;
    bool block = false;
    int mdst = CUI__MD_START;
    int line = 1, i = 0;
    cui__tok toks[256];
    while (i <= p->len) {
        if (i == p->len && p->len > 0 && p->code[p->len - 1] == '\n') break;   /* no phantom line after a final newline */
        int e = i;
        while (e < p->len && p->code[e] != '\n') e++;
        if (line >= p->hl_from && line <= p->hl_to && p->hl_from > 0)
            cui_draw_rect(ui, (cui_rect){ r.x, y - 1, r.w, p->lh }, CUI_CORNERS(0), p->hl_c);
        float ty = y + (p->lh - fh) * 0.5f - 1;
        if (p->line_numbers) {
            const char *ln = cui__frame_printf(ui, "%d", line);
            float w = cui__text_width(ui, CUI_FONT_MONO, p->size, ln, -1, true);
            cui__draw_text_line(ui, CUI_FONT_MONO, p->size, r.x + p->pad + p->gutter - 16 - w, ty, p->gutter_c, ln, -1, true);
        }
        int nt = p->lang.markdown ? cui__tokenize_md(p->code + i, e - i, &mdst, toks, 256)
                                  : cui__tokenize(&p->lang, p->json, p->code + i, e - i, &block, toks, 256);
        float x = x0;
        for (int k = 0; k < nt; k++) {
            const char *s = p->code + i + toks[k].start;
            int len = toks[k].len;
            /* tabs become four spaces */
            if (len > 0 && s[0] == '\t') { x += cui__text_width(ui, CUI_FONT_MONO, p->size, "    ", 4, true) * (float)len; continue; }
            if (toks[k].kind != TK_TEXT || s[0] != ' ')
                cui__draw_text_line(ui, CUI_FONT_MONO, p->size, x, ty, p->colors[toks[k].kind], s, len, true);
            x += cui__text_width(ui, CUI_FONT_MONO, p->size, s, len, true);
        }
        y += p->lh;
        line++;
        if (e >= p->len) break;
        i = e + 1;
    }
}

static void cui__code_block_ex_body(cui_ctx *ui, const char *id_str, const char *code, const cui_code_opts *o);

/* left to right in any interface: see cui__keep_ltr */
void cui_code_block_ex(cui_ctx *ui, const char *id_str, const char *code, const cui_code_opts *o) {
    int first = ui->node_count;
    cui__code_block_ex_body(ui, id_str, code, o);
    cui__keep_ltr(ui, first);
}

static void cui__code_block_ex_body(cui_ctx *ui, const char *id_str, const char *code, const cui_code_opts *o) {
    cui_code_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!code) code = "";
    cui_id id = cui_make_id(ui, id_str);
    float size = cui__fs(ui, o->size > 0 ? o->size : 13);
    float lh = size * 1.6f;
    int len = (int)strlen(code);
    int lines = 1;
    float maxw = 0;
    {
        int i = 0;
        while (i <= len) {
            int e = i;
            while (e < len && code[e] != '\n') e++;
            int tabs = 0;
            for (int k = i; k < e; k++) if (code[k] == '\t') tabs++;
            float w = cui__text_width(ui, CUI_FONT_MONO, size, code + i, e - i, true) + tabs * cui__text_width(ui, CUI_FONT_MONO, size, "   ", 3, true);
            if (w > maxw) maxw = w;
            if (e >= len) break;
            i = e + 1;
            lines++;
        }
        if (len > 0 && code[len - 1] == '\n') lines--;
    }
    float pad = 16;
    float gutter = o->line_numbers ? 16 + cui__text_width(ui, CUI_FONT_MONO, size, "000", lines >= 100 ? 3 : 2, true) + 4 : 0;
    float content_h = lines * lh + pad * 2;
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = CUI_FILL;
    on->radius = CUI_CORNERS(ui->theme.radius_control);
    on->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, BACKGROUND), 0.4f) : CUI_C(ui, SURFACE_MUTED);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->flags |= CUI_NF_CLIP;   /* the outline is drawn over the title bar */
    cui__open(ui, outer);
    if ((o->title || o->lang) && !o->no_header) {
        int head = cui__node_new(ui, 0);
        cui_node *hn = &ui->nodes[head];
        hn->dir = CUI_ROW; hn->align = CUI_ALIGN_CENTER; hn->gap = 8;
        hn->w = CUI_FILL; hn->h = CUI_PX(40);
        hn->pad[1] = 6; hn->pad[3] = 14;
        hn->bg = CUI_C(ui, SURFACE);
        hn->radius = (cui_corners){ ui->theme.radius_control, ui->theme.radius_control, 0, 0 };
        cui__open(ui, head);
        cui__icon_node(ui, o->title ? CUI_ICON_FILE_TEXT : CUI_ICON_CODE, 14, CUI_C(ui, TEXT_MUTED));
        int t = cui__text_node(ui, o->title ? o->title : o->lang, -1, CUI_FONT_MEDIUM, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_SECONDARY), false);
        ui->nodes[t].line_height = 1.3f;
        ui->nodes[t].w = CUI_FILL;
        if (o->title && o->lang) {
            cui_badge_opts bo;
            memset(&bo, 0, sizeof bo);
            bo.size = CUI_SIZE_SM;
            cui_badge_ex(ui, o->lang, &bo);
        }
        cui_push_id(ui, id_str);
        cui_copy_button(ui, "copy", code);
        cui_pop_id(ui);
        cui__close(ui);
        int line = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
        ui->nodes[line].bg = CUI_C(ui, BORDER);
    }
    cui_box sb;
    memset(&sb, 0, sizeof sb);
    sb.width = CUI_FILL;
    if (o->max_height > 0 && content_h > o->max_height) sb.height = CUI_PX(o->max_height);
    cui_scroll_state *ss = cui_state(ui, id ^ 0x5C0Du, sizeof *ss);
    ss->fade = cui__backdrop(ui);   /* long lines fade out at the edge */
    cui__scroll_begin_ex(ui, id ^ 0x5C0Du, &sb, true);
    if (!(o->max_height > 0 && content_h > o->max_height)) { ui->nodes[cui__current(ui)].h = CUI_FIT; ui->nodes[cui__current(ui)].min_h = 0; }
    int canvas = cui__leaf(ui, 0, CUI_FILL, CUI_PX(content_h));
    cui_node *cn = &ui->nodes[canvas];
    cn->min_w = maxw + gutter + pad * 2 + 8;   /* at least as wide as the code; the band spans the block */
    cui__code_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->code = cui__frame_strdup(ui, code, len);
    p->len = len;
    p->json = o->lang && !strcmp(o->lang, "json");
    p->lang = cui__lang_for(p->json ? NULL : o->lang);
    p->line_numbers = o->line_numbers;
    p->hl_from = o->highlight_from; p->hl_to = o->highlight_to > 0 ? o->highlight_to : o->highlight_from;
    p->size = size; p->lh = lh; p->gutter = gutter; p->pad = pad;
    cui__syntax_colors(ui, p->colors);
    p->gutter_c = cui_alpha(CUI_C(ui, TEXT_MUTED), 0.7f);
    p->hl_c = cui_alpha(cui__emphasis(ui), ui->mode == CUI_THEME_DARK ? 0.12f : 0.08f);
    cn->paint = cui__paint_code;
    cn->paint_data = p;
    cui__scroll_end_ex(ui);
    cui__close(ui);
    ui->last_id = id;
}

/* ------------------------------------------------------------------------ */
/* JSON                                                                     */
/* ------------------------------------------------------------------------ */

enum { JV_NULL, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };
typedef struct { uint8_t type; int key_start, key_len; int val_start, val_len; int parent, first, next, count, depth; } cui__jnode;

typedef struct { const char *s; int n, i; cui__jnode *nodes; int count, cap; const char *err; int err_at; } cui__jparse;

static void cui__jws(cui__jparse *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\t' || p->s[p->i] == '\n' || p->s[p->i] == '\r')) p->i++; }
static bool cui__jfail(cui__jparse *p, const char *msg) { if (!p->err) { p->err = msg; p->err_at = p->i; } return false; }

static bool cui__jstring(cui__jparse *p, int *start, int *len) {
    if (p->i >= p->n || p->s[p->i] != '"') return cui__jfail(p, "expected a string");
    int st = ++p->i;
    while (p->i < p->n && p->s[p->i] != '"') {
        if (p->s[p->i] == '\\') p->i++;
        else if ((unsigned char)p->s[p->i] < 0x20) return cui__jfail(p, "control character in string");
        p->i++;
    }
    if (p->i >= p->n) return cui__jfail(p, "unterminated string");
    *start = st; *len = p->i - st;
    p->i++;
    return true;
}

static int cui__jvalue(cui__jparse *p, int parent, int depth, int key_start, int key_len);

static int cui__jnew(cui__jparse *p, int parent, int depth, int key_start, int key_len) {
    if (p->count >= p->cap) { cui__jfail(p, "too large"); return -1; }
    cui__jnode *nd = &p->nodes[p->count];
    memset(nd, 0, sizeof *nd);
    nd->parent = parent; nd->first = -1; nd->next = -1; nd->depth = depth;
    nd->key_start = key_start; nd->key_len = key_len;
    return p->count++;
}

static int cui__jvalue(cui__jparse *p, int parent, int depth, int key_start, int key_len) {
    if (depth > 64) { cui__jfail(p, "nested too deeply"); return -1; }
    cui__jws(p);
    if (p->i >= p->n) { cui__jfail(p, "unexpected end"); return -1; }
    int me = cui__jnew(p, parent, depth, key_start, key_len);
    if (me < 0) return -1;
    char c = p->s[p->i];
    int st = p->i;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        p->nodes[me].type = obj ? JV_OBJ : JV_ARR;
        p->i++;
        cui__jws(p);
        int last = -1;
        if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; return me; }
        for (;;) {
            int ks = 0, kl = -1;
            cui__jws(p);
            if (obj) {
                if (!cui__jstring(p, &ks, &kl)) return -1;
                cui__jws(p);
                if (p->i >= p->n || p->s[p->i] != ':') { cui__jfail(p, "expected ':'"); return -1; }
                p->i++;
            }
            int child = cui__jvalue(p, me, depth + 1, ks, kl);
            if (child < 0) return -1;
            if (last < 0) p->nodes[me].first = child; else p->nodes[last].next = child;
            last = child;
            p->nodes[me].count++;
            cui__jws(p);
            if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
            if (p->i < p->n && p->s[p->i] == (obj ? '}' : ']')) { p->i++; break; }
            cui__jfail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
            return -1;
        }
        return me;
    }
    if (c == '"') {
        int s0, l0;
        if (!cui__jstring(p, &s0, &l0)) return -1;
        p->nodes[me].type = JV_STR;
        p->nodes[me].val_start = s0 - 1; p->nodes[me].val_len = l0 + 2;
        return me;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        p->i++;
        while (p->i < p->n && ((p->s[p->i] >= '0' && p->s[p->i] <= '9') || p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E' ||
                               p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        p->nodes[me].type = JV_NUM;
    } else if (!strncmp(p->s + p->i, "true", 4) || !strncmp(p->s + p->i, "null", 4)) {
        p->nodes[me].type = p->s[p->i] == 't' ? JV_BOOL : JV_NULL;
        p->i += 4;
    } else if (!strncmp(p->s + p->i, "false", 5)) {
        p->nodes[me].type = JV_BOOL;
        p->i += 5;
    } else {
        cui__jfail(p, "unexpected character");
        return -1;
    }
    p->nodes[me].val_start = st; p->nodes[me].val_len = p->i - st;
    return me;
}

typedef struct { uint8_t collapsed[512]; bool init; } cui__json_state;   /* by node index, bit per node */

static void cui__json_rows(cui_ctx *ui, cui_id id, cui__jparse *p, cui__json_state *st, int node, const cui_color *col, bool last) {
    cui__jnode *nd = &p->nodes[node];
    bool container = nd->type == JV_ARR || nd->type == JV_OBJ;
    bool collapsed = node < 4096 && (st->collapsed[node >> 3] >> (node & 7) & 1);
    cui_id rid = cui__hash(&node, sizeof node, id);
    cui_interact r = cui__interact(ui, rid, 0);
    if (r.clicked && container && node < 4096) { st->collapsed[node >> 3] ^= (uint8_t)(1 << (node & 7)); collapsed = !collapsed; }
    float hv_v = cui__hover_fade(ui, rid, (r.hovered && container), CUI__T_HIGHLIGHT), *hv = &hv_v;
    float fs = cui__fs(ui, 13);
    int row = cui__node_new(ui, rid);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->gap = 0;
    rn->h = CUI_PX(24); rn->w = CUI_FILL;
    rn->pad[3] = 8 + 16.0f * (float)nd->depth;
    rn->radius = CUI_CORNERS(6);
    rn->bg = cui_alpha(CUI_C(ui, HOVER), *hv);
    if (container) { cui__mark_interactive(ui, row, 0); if (r.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND); }
    cui__open(ui, row);
    if (container) cui__chevron_node(ui, collapsed ? -CUI_PI * 0.5f : 0, 12, CUI_C(ui, TEXT_MUTED));
    else cui_gap(ui, 12);
    cui_gap(ui, 4);
    if (nd->key_len >= 0 && nd->parent >= 0 && p->nodes[nd->parent].type == JV_OBJ) {
        int k = cui__text_node(ui, cui__frame_printf(ui, "\"%.*s\"", nd->key_len, p->s + nd->key_start), -1, CUI_FONT_MONO, fs, col[TK_FUNC], false);
        ui->nodes[k].line_height = 1.3f;
        int c = cui__text_node(ui, ": ", 2, CUI_FONT_MONO, fs, col[TK_PUNCT], false);
        ui->nodes[c].line_height = 1.3f;
    }
    const char *val;
    cui_color vc = col[TK_TEXT];
    switch (nd->type) {
    case JV_OBJ: val = collapsed ? cui__frame_printf(ui, "{ %d key%s }", nd->count, nd->count == 1 ? "" : "s") : "{"; vc = col[TK_PUNCT]; break;
    case JV_ARR: val = collapsed ? cui__frame_printf(ui, "[ %d item%s ]", nd->count, nd->count == 1 ? "" : "s") : "["; vc = col[TK_PUNCT]; break;
    case JV_STR: val = cui__frame_strdup(ui, p->s + nd->val_start, nd->val_len); vc = col[TK_STRING]; break;
    case JV_NUM: val = cui__frame_strdup(ui, p->s + nd->val_start, nd->val_len); vc = col[TK_NUMBER]; break;
    default: val = cui__frame_strdup(ui, p->s + nd->val_start, nd->val_len); vc = col[TK_CONST]; break;
    }
    int v = cui__text_node(ui, val, -1, CUI_FONT_MONO, fs, vc, false);
    ui->nodes[v].line_height = 1.3f;
    if (!container || collapsed) {
        if (!last) { int c = cui__text_node(ui, ",", 1, CUI_FONT_MONO, fs, col[TK_PUNCT], false); ui->nodes[c].line_height = 1.3f; }
    }
    cui__close(ui);
    if (container && !collapsed) {
        for (int c = nd->first; c >= 0; c = p->nodes[c].next) cui__json_rows(ui, id, p, st, c, col, p->nodes[c].next < 0);
        int close = cui__node_new(ui, 0);
        cui_node *cn = &ui->nodes[close];
        cn->dir = CUI_ROW; cn->align = CUI_ALIGN_CENTER;
        cn->h = CUI_PX(24); cn->pad[3] = 8 + 16.0f * (float)nd->depth + 16;
        cui__open(ui, close);
        int t = cui__text_node(ui, nd->type == JV_OBJ ? (last ? "}" : "},") : (last ? "]" : "],"), -1, CUI_FONT_MONO, fs, col[TK_PUNCT], false);
        ui->nodes[t].line_height = 1.3f;
        cui__close(ui);
    }
}

static bool cui__json_view_ex_body(cui_ctx *ui, const char *id_str, const char *json, const cui_json_opts *o);

/* left to right in any interface: see cui__keep_ltr */
bool cui_json_view_ex(cui_ctx *ui, const char *id_str, const char *json, const cui_json_opts *o) {
    int first = ui->node_count;
    bool r = cui__json_view_ex_body(ui, id_str, json, o);
    cui__keep_ltr(ui, first);
    return r;
}

static bool cui__json_view_ex_body(cui_ctx *ui, const char *id_str, const char *json, const cui_json_opts *o) {
    cui_json_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_id id = cui_make_id(ui, id_str);
    cui__json_state *st = cui_state(ui, id, sizeof *st);
    cui__jparse p;
    memset(&p, 0, sizeof p);
    p.s = json ? json : "";
    p.n = (int)strlen(p.s);
    p.cap = 4096;
    p.nodes = cui__frame_alloc(ui, sizeof(cui__jnode) * (size_t)p.cap);
    int root = cui__jvalue(&p, -1, 0, 0, -1);
    if (root >= 0) { cui__jws(&p); if (p.i < p.n) cui__jfail(&p, "unexpected text after the value"); }
    bool ok = root >= 0 && !p.err;
    if (ok && !st->init) {
        int depth = o->expand_depth > 0 ? o->expand_depth : 2;
        for (int k = 0; k < p.count && k < 4096; k++)
            if ((p.nodes[k].type == JV_ARR || p.nodes[k].type == JV_OBJ) && p.nodes[k].depth >= depth) st->collapsed[k >> 3] |= (uint8_t)(1 << (k & 7));
        st->init = true;
    }
    cui_color col[TK_COUNT];
    cui__syntax_colors(ui, col);
    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = CUI_FILL;
    on->radius = CUI_CORNERS(ui->theme.radius_control);
    on->bg = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, BACKGROUND), 0.4f) : CUI_C(ui, SURFACE_MUTED);
    on->border = 1; on->border_color = CUI_C(ui, BORDER);
    on->flags |= CUI_NF_CLIP;   /* the outline is drawn over the title bar */
    cui__open(ui, outer);
    cui_box sb;
    memset(&sb, 0, sizeof sb);
    sb.width = CUI_FILL;
    sb.pad = 8;
    if (o->max_height > 0) sb.max_height = o->max_height;
    cui__scroll_begin_ex(ui, id ^ 0x15011u, &sb, false);
    ui->nodes[cui__current(ui)].h = CUI_FIT;
    ui->nodes[cui__current(ui)].min_h = 0;
    if (o->max_height > 0) ui->nodes[cui__current(ui)].max_h = o->max_height;
    if (ok) {
        cui_push_id(ui, id_str);
        cui__json_rows(ui, id, &p, st, root, col, true);
        cui_pop_id(ui);
    } else {
        int line = 1, colno = 1;
        for (int k = 0; k < p.err_at && k < p.n; k++) { if (p.s[k] == '\n') { line++; colno = 1; } else colno++; }
        int row = cui__node_new(ui, 0);
        ui->nodes[row].dir = CUI_ROW; ui->nodes[row].gap = 8; ui->nodes[row].align = CUI_ALIGN_CENTER;
        ui->nodes[row].pad[0] = ui->nodes[row].pad[2] = 6; ui->nodes[row].pad[3] = 6;
        cui__open(ui, row);
        cui__icon_node(ui, CUI_ICON_CIRCLE_ALERT, 16, CUI_C(ui, DANGER));
        int t = cui__text_node(ui, cui__frame_printf(ui, "Invalid JSON: %s (line %d, column %d)", p.err ? p.err : "error", line, colno), -1,
                               CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, DANGER), true);
        ui->nodes[t].line_height = 1.4f;
        cui__close(ui);
    }
    cui__scroll_end_ex(ui);
    cui__close(ui);
    ui->last_id = id;
    return ok;
}
