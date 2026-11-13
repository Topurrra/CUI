/* Syntax highlighting for the config, markup and C-like languages: yaml, toml, ini, xml and html, css,
 * powershell, csharp, java and batch. One line at a time, like the rest of cui_code.c; the multi-line
 * constructs (block scalars, multi-line strings, comments, open tags, here-strings) are an int state
 * that goes from one line to the next. The state is small: the low 4 bits are a kind, the rest a
 * payload (an indent, a nesting depth, flags), so the code editor caches it per line and finds a line
 * from the line above it, never from line 0 per frame. A caller that has only a bool passes 0 or 1:
 * state 1 is the first kind of each language, with its payload 0. */
#include "cui_internal.h"

enum { TK_TEXT, TK_KEYWORD, TK_TYPE, TK_STRING, TK_NUMBER, TK_COMMENT, TK_FUNC, TK_PREPROC, TK_PUNCT, TK_CONST };   /* as in cui_code.c */

/* ---- emitting: the tokens cover the whole line, with TEXT in the gaps, and tab runs are tokens of their own ---- */

typedef struct { const char *s; int n, pos, cap, count; cui__tok *out; } cui__lx;

static void lx_put(cui__lx *x, int st, int len, int kind) {
    if (x->count < x->cap) { x->out[x->count].start = st; x->out[x->count].len = len; x->out[x->count].kind = (uint8_t)kind; x->count++; }
}
static void lx_gap(cui__lx *x, int upto) {
    int i = x->pos;
    while (i < upto) {
        int j = i;
        if (x->s[i] == '\t') while (j < upto && x->s[j] == '\t') j++;
        else while (j < upto && x->s[j] != '\t') j++;
        lx_put(x, i, j - i, TK_TEXT);
        i = j;
    }
    if (upto > x->pos) x->pos = upto;
}
/* white space at the start of a token is left to the gap */
static void lx_e(cui__lx *x, int st, int len, int kind) {
    while (len > 0 && (x->s[st] == ' ' || x->s[st] == '\t')) { st++; len--; }
    if (st < x->pos) { len -= x->pos - st; st = x->pos; }
    if (len <= 0) return;
    lx_gap(x, st);
    lx_put(x, st, len, kind);
    x->pos = st + len;
}

static bool lx_ws(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool lx_digit(char c) { return c >= '0' && c <= '9'; }
static bool lx_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool lx_alnum(char c) { return lx_alpha(c) || lx_digit(c); }
static bool lx_xdigit(char c) { return lx_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int lx_skip_ws(const char *s, int i, int n) { while (i < n && lx_ws(s[i])) i++; return i; }
static int lx_trim(const char *s, int i, int n) { while (n > i && lx_ws(s[n - 1])) n--; return n; }
static bool lx_has(const char *s, int i, int n, const char *w) { size_t l = strlen(w); return (size_t)(n - i) >= l && strncmp(s + i, w, l) == 0; }

/* the first place in [i, n) that w starts, or -1 */
static int lx_find(const char *s, int i, int n, const char *w) {
    size_t l = strlen(w);
    for (; i + (int)l <= n; i++) {
        const char *p = memchr(s + i, w[0], (size_t)(n - i - (int)l + 1));
        if (!p) return -1;
        i = (int)(p - s);
        if (strncmp(s + i, w, l) == 0) return i;
    }
    return -1;
}
/* the start of a closing triple quote in [i, n), a backslash skipping a character when esc; -1 when there is none */
static int lx_triple(const char *s, int i, int n, char q, bool esc) {
    for (; i + 2 < n; i++) {
        if (esc && s[i] == '\\') { i++; continue; }
        if (s[i] == q && s[i + 1] == q && s[i + 2] == q) return i;
    }
    return -1;
}
static int lx_find_ci(const char *s, int i, int n, const char *w) {   /* w is lowercase */
    size_t l = strlen(w);
    for (; i + (int)l <= n; i++) {
        size_t k = 0;
        while (k < l && (s[i + (int)k] | 32) == w[k]) k++;
        if (k == l) return i;
    }
    return -1;
}
static bool lx_word_is(const char *s, int a, int b, const char *w) {   /* the word [a, b) is w, in any case; w is lowercase */
    if ((int)strlen(w) != b - a) return false;
    for (int k = a; k < b; k++) if ((s[k] | 32) != w[k - a]) return false;
    return true;
}
/* the end of a quoted string that starts at i (the quote q); esc: a backslash escapes, dbl: a doubled quote is one; the line end when open */
static int lx_quoted(const char *s, int i, int n, char q, bool esc, bool dbl) {
    int j = i + 1;
    while (j < n) {
        if (esc && s[j] == '\\' && j + 1 < n) { j += 2; continue; }
        if (s[j] == q) { if (dbl && j + 1 < n && s[j + 1] == q) { j += 2; continue; } return j + 1; }
        j++;
    }
    return n;
}
/* a number as C-like languages write it: 0x1F_FFL, 1_000, 1.5e-3f */
static int lx_number(const char *s, int i, int n) {
    bool hex = s[i] == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
    int j = i + 1;
    while (j < n) {
        char c = s[j];
        if (cui__ident_char(c)) { j++; continue; }
        if (c == '.' && j + 1 < n && lx_digit(s[j + 1])) { j++; continue; }
        if ((c == '+' || c == '-') && !hex && (s[j - 1] == 'e' || s[j - 1] == 'E') && j + 1 < n && lx_digit(s[j + 1])) { j++; continue; }
        break;
    }
    return j;
}
static int lx_next(const char *s, int i, int n) { return lx_skip_ws(s, i, n) < n ? s[lx_skip_ws(s, i, n)] : 0; }   /* the next character that is not white space, or 0 */

/* ======================================================================== */
/* YAML                                                                     */
/* ======================================================================== */

/* state: 1 = inside a block scalar (| or >) whose key sits at the indent in the upper bits */
static const char *const yaml_const[] = { "true", "false", "null", "yes", "no", "on", "off", NULL };

/* a plain scalar [a, b) as a token kind */
static int yaml_scalar_kind(const char *s, int a, int b) {
    if (b <= a) return TK_TEXT;
    if (cui__in_list(yaml_const, s + a, b - a, true) || (b - a == 1 && s[a] == '~')) return TK_CONST;
    int p = a;
    if (s[p] == '+' || s[p] == '-') p++;
    if (p >= b) return TK_TEXT;
    if (b - p > 2 && s[p] == '0' && (s[p + 1] == 'x' || s[p + 1] == 'o')) {
        for (int q = p + 2; q < b; q++) if (!lx_xdigit(s[q])) return TK_TEXT;
        return TK_NUMBER;
    }
    if (lx_word_is(s, p, b, ".inf") || lx_word_is(s, p, b, ".nan")) return TK_NUMBER;
    bool dig = false;
    for (int q = p; q < b; q++) {
        if (lx_digit(s[q])) dig = true;
        else if (!strchr(".eE+-_", s[q])) return TK_TEXT;
    }
    return dig && (lx_digit(s[p]) || s[p] == '.') ? TK_NUMBER : TK_TEXT;
}

/* a value from i on: scalars, flow collections, anchors, tags, a block scalar header, a comment */
static void yaml_value(cui__lx *x, int i, int pind, int *state) {
    const char *s = x->s;
    int n = x->n, depth = 0;
    bool first = true;
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '#' && (i == 0 || lx_ws(s[i - 1]))) { lx_e(x, i, n - i, TK_COMMENT); return; }
        if (c == '"' || c == '\'') {
            int j = lx_quoted(s, i, n, c, c == '"', c == '\'');
            lx_e(x, i, j - i, depth > 0 && lx_next(s, j, n) == ':' ? TK_FUNC : TK_STRING);
            i = j; first = false;
            continue;
        }
        if (c == '&' || c == '*' || c == '!') {
            int j = i + 1;
            while (j < n && !lx_ws(s[j]) && !(depth > 0 && strchr(",]}", s[j]))) j++;
            lx_e(x, i, j - i, c == '!' ? TK_PREPROC : TK_TYPE);
            i = j;
            continue;      /* a tag or an anchor leaves the value first: key: !!str | */
        }
        if (first && depth == 0 && (c == '|' || c == '>')) {
            int j = i + 1;
            while (j < n && strchr("+-0123456789", s[j])) j++;
            if (j >= n || lx_ws(s[j])) { lx_e(x, i, j - i, TK_PUNCT); *state = 1 | (pind << 8); i = j; first = false; continue; }
        }
        first = false;
        if (c == '{' || c == '[') { depth++; lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (c == '}' || c == ']') { if (depth > 0) depth--; lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (depth > 0 && (c == ',' || (c == ':' && (i + 1 >= n || lx_ws(s[i + 1]))))) { lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        int j = i;
        while (j < n) {
            char d = s[j];
            if (depth > 0 && (strchr(",[]{}", d) || (d == ':' && (j + 1 >= n || lx_ws(s[j + 1]))))) break;
            if (lx_ws(d) && j + 1 < n && s[j + 1] == '#') break;
            j++;
        }
        int e = lx_trim(s, i, j);
        if (e <= i) { lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        lx_e(x, i, e - i, depth > 0 && lx_next(s, e, n) == ':' ? TK_FUNC : yaml_scalar_kind(s, i, e));
        i = j;
    }
}

static void lx_yaml(cui__lx *x, int *state) {
    const char *s = x->s;
    int n = x->n, i = lx_skip_ws(s, 0, n), pind;
    if ((*state & 15) == 1) {
        int p = *state >> 8, e = lx_trim(s, i, n);
        if (e <= i) return;                                          /* a blank line stays in the scalar */
        if (i > p) { lx_e(x, i, e - i, TK_STRING); return; }
        *state = 0;
    } else if (x->cap == 0 && n > 0 && !memchr(s, '|', (size_t)n) && !memchr(s, '>', (size_t)n)) return;   /* only a header changes the state */
    if (i >= n) return;
    if (s[i] == '#') { lx_e(x, i, n - i, TK_COMMENT); return; }
    if (i == 0 && n >= 3 && (lx_has(s, 0, n, "---") || lx_has(s, 0, n, "...")) && (n == 3 || lx_ws(s[3]))) {   /* a document marker */
        lx_e(x, 0, 3, TK_PREPROC);
        yaml_value(x, 3, 0, state);
        return;
    }
    int dash = -1;
    while (i < n && s[i] == '-' && (i + 1 >= n || lx_ws(s[i + 1]))) { lx_e(x, i, 1, TK_PUNCT); dash = i; i = lx_skip_ws(s, i + 1, n); }
    pind = dash >= 0 ? dash : i;
    /* a key: quoted, or plain up to ": " */
    int ks = i, ke = -1, colon = -1;
    if (i < n && (s[i] == '"' || s[i] == '\'')) {
        int j = lx_quoted(s, i, n, s[i], s[i] == '"', s[i] == '\'');
        int k = lx_skip_ws(s, j, n);
        if (j <= n && s[j - 1] == s[i] && j - i > 1 && k < n && s[k] == ':' && (k + 1 >= n || lx_ws(s[k + 1]))) { ke = j; colon = k; }
    } else if (i < n && !strchr("{[|>&*!%@`", s[i])) {
        for (int j = i; j < n; j++) {
            if (s[j] == ':' && (j + 1 >= n || lx_ws(s[j + 1]))) { ke = lx_trim(s, i, j); colon = j; break; }
            if (s[j] == '#' && j > i && lx_ws(s[j - 1])) break;
        }
    }
    if (colon >= 0 && ke > ks) {
        lx_e(x, ks, ke - ks, TK_FUNC);
        lx_e(x, colon, 1, TK_PUNCT);
        pind = ks;
        i = colon + 1;
    }
    yaml_value(x, i, pind, state);
}

/* ======================================================================== */
/* TOML                                                                     */
/* ======================================================================== */

/* state kinds: 1 inside a """ string, 2 inside a ''' string, 3 inside an array (that goes on a further line); the depth of [ and { is in the upper bits */
static void toml_value(cui__lx *x, int i, int depth, int *state) {
    const char *s = x->s;
    int n = x->n;
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '#') { lx_e(x, i, n - i, TK_COMMENT); break; }
        if (c == '"' || c == '\'') {
            if (i + 2 < n && s[i + 1] == c && s[i + 2] == c) {      /* a multi-line string */
                int j = lx_triple(s, i + 3, n, c, c == '"');
                if (j < 0) { lx_e(x, i, n - i, TK_STRING); *state = (c == '"' ? 1 : 2) | (depth << 8); return; }
                int e = j + 3;
                while (e < n && s[e] == c && e - j < 5) e++;           /* up to two quotes belong to the string */
                lx_e(x, i, e - i, TK_STRING);
                i = e;
                continue;
            }
            int j = lx_quoted(s, i, n, c, c == '"', false);
            lx_e(x, i, j - i, lx_next(s, j, n) == '=' ? TK_FUNC : TK_STRING);
            i = j;
            continue;
        }
        if (c == '[' || c == '{') { depth++; lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (c == ']' || c == '}') { if (depth > 0) depth--; lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (c == ',' || c == '=' || c == '.') { lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (lx_digit(c) || ((c == '+' || c == '-') && i + 1 < n && (lx_digit(s[i + 1]) || lx_alpha(s[i + 1])))) {
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || strchr("_.:+-", s[j]))) j++;
            if (lx_next(s, j, n) == '=') lx_e(x, i, j - i, TK_FUNC);
            else if (!lx_digit(c) && lx_alpha(s[i + 1])) lx_e(x, i, j - i, TK_CONST);   /* +inf, -nan */
            else lx_e(x, i, j - i, TK_NUMBER);
            i = j;
            continue;
        }
        if (lx_alpha(c) || c == '_') {
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || s[j] == '_' || s[j] == '-')) j++;
            bool key = lx_next(s, j, n) == '=';
            if (key) lx_e(x, i, j - i, TK_FUNC);
            else if (lx_word_is(s, i, j, "true") || lx_word_is(s, i, j, "false") || lx_word_is(s, i, j, "inf") || lx_word_is(s, i, j, "nan"))
                lx_e(x, i, j - i, TK_CONST);
            i = j;
            continue;
        }
        i++;
    }
    *state = depth > 0 ? 3 | (depth << 8) : 0;
}

static void lx_toml(cui__lx *x, int *state) {
    const char *s = x->s;
    int n = x->n, kind = *state & 15, depth = *state >> 8, i = 0;
    if (kind == 0 && depth == 0 && x->cap == 0 && !memchr(s, '[', (size_t)n) && lx_find(s, 0, n, "\"\"\"") < 0 && lx_find(s, 0, n, "'''") < 0) return;
    if (kind == 1 || kind == 2) {      /* inside a multi-line string: up to its end */
        char q = kind == 1 ? '"' : '\'';
        int j = lx_triple(s, 0, n, q, q == '"');
        if (j < 0) { lx_e(x, 0, n, TK_STRING); return; }
        int e = j + 3;
        while (e < n && s[e] == q && e - j < 5) e++;
        lx_e(x, 0, e, TK_STRING);
        toml_value(x, e, depth, state);
        return;
    }
    if (kind == 3 || depth > 0) { toml_value(x, 0, depth, state); return; }
    *state = 0;
    i = lx_skip_ws(s, 0, n);
    if (i >= n) return;
    if (s[i] == '#') { lx_e(x, i, n - i, TK_COMMENT); return; }
    if (s[i] == '[') {      /* a table [a.b] or an array of tables [[a]] */
        int b = i + 1 < n && s[i + 1] == '[' ? 2 : 1, j = i + b, k = j;
        while (k < n && s[k] != ']') { if (s[k] == '"' || s[k] == '\'') k = lx_quoted(s, k, n, s[k], s[k] == '"', false); else k++; }
        lx_e(x, i, b, TK_PUNCT);
        lx_e(x, j, lx_trim(s, j, k) - j, TK_TYPE);
        if (k < n) { int c = 0; while (c < b && k + c < n && s[k + c] == ']') c++; lx_e(x, k, c, TK_PUNCT); k += c; }
        toml_value(x, k, 0, state);
        return;
    }
    while (i < n) {      /* the key, dotted, then = */
        char c = s[i];
        if (lx_ws(c)) i++;
        else if (c == '"' || c == '\'') { int j = lx_quoted(s, i, n, c, c == '"', false); lx_e(x, i, j - i, TK_FUNC); i = j; }
        else if (c == '.') { lx_e(x, i, 1, TK_PUNCT); i++; }
        else if (c == '=') { lx_e(x, i, 1, TK_PUNCT); i++; break; }
        else if (lx_alnum(c) || c == '_' || c == '-') { int j = i + 1; while (j < n && (lx_alnum(s[j]) || s[j] == '_' || s[j] == '-')) j++; lx_e(x, i, j - i, TK_FUNC); i = j; }
        else break;
    }
    toml_value(x, i, 0, state);
}

/* ======================================================================== */
/* INI, cfg, conf, properties                                               */
/* ======================================================================== */

static void lx_ini(cui__lx *x) {
    const char *s = x->s;
    int n = x->n, i = lx_skip_ws(s, 0, n);
    if (i >= n) return;
    if (s[i] == ';' || s[i] == '#') { lx_e(x, i, n - i, TK_COMMENT); return; }
    if (s[i] == '[') {
        int j = i + 1;
        while (j < n && s[j] != ']') j++;
        lx_e(x, i, 1, TK_PUNCT);
        lx_e(x, i + 1, lx_trim(s, i + 1, j) - i - 1, TK_TYPE);
        if (j < n) lx_e(x, j, 1, TK_PUNCT);
        return;
    }
    int j = i;
    while (j < n && s[j] != '=' && s[j] != ':') j++;
    if (j >= n) return;
    lx_e(x, i, lx_trim(s, i, j) - i, TK_FUNC);
    lx_e(x, j, 1, TK_PUNCT);
}

/* ======================================================================== */
/* XML, HTML, SVG, XAML                                                     */
/* ======================================================================== */

/* state kinds: 1 comment, 2 CDATA, 3 inside a tag (attributes go on), 4 and 5 inside a "..." or '...' value, 6 a <! or <? declaration,
 * 7 raw text of <script> or <style> (html only: left plain). Flags in the upper bits: 1 script, 2 style, 4 a closing tag. */
static void lx_xml(cui__lx *x, bool html, int *state) {
    const char *s = x->s;
    int n = x->n, kind = *state & 15, fl = *state >> 8, i = 0, cs = 0;
    if (kind == 0 && x->cap == 0 && !memchr(s, '<', (size_t)n)) return;
    while (i < n) {
        if (kind == 0) {
            char c = s[i];
            if (c == '&') {
                int j = i + 1;
                if (j < n && s[j] == '#') j++;
                while (j < n && lx_alnum(s[j])) j++;
                if (j < n && s[j] == ';' && j > i + 1) { lx_e(x, i, j + 1 - i, TK_PREPROC); i = j + 1; } else i++;
            } else if (c != '<') i++;
            else if (lx_has(s, i, n, "<!--")) { cs = i; i += 4; kind = 1; }
            else if (lx_has(s, i, n, "<![CDATA[")) { lx_e(x, i, 9, TK_PREPROC); i += 9; kind = 2; }
            else if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) { cs = i; i += 2; kind = 6; }
            else {
                bool close = i + 1 < n && s[i + 1] == '/';
                int j = i + (close ? 2 : 1), k = j;
                if (j < n && (lx_alpha(s[j]) || s[j] == '_' || s[j] == ':')) {
                    while (k < n && (lx_alnum(s[k]) || strchr("_:.-", s[k]))) k++;
                    lx_e(x, i, j - i, TK_PUNCT);
                    lx_e(x, j, k - j, TK_KEYWORD);
                    fl = close ? 4 : html && lx_word_is(s, j, k, "script") ? 1 : html && lx_word_is(s, j, k, "style") ? 2 : 0;
                    kind = 3; i = k;
                } else i++;
            }
        } else if (kind == 1) {
            int j = lx_find(s, i, n, "-->"), e = j >= 0 ? j + 3 : n;
            lx_e(x, cs, e - cs, TK_COMMENT);
            i = e; cs = e;
            if (j >= 0) kind = 0;
        } else if (kind == 2) {
            int j = lx_find(s, i, n, "]]>");
            lx_e(x, i, (j >= 0 ? j : n) - i, TK_STRING);
            if (j >= 0) { lx_e(x, j, 3, TK_PREPROC); i = j + 3; kind = 0; } else i = n;
        } else if (kind == 6) {
            int j = lx_find(s, i, n, ">"), e = j >= 0 ? j + 1 : n;
            lx_e(x, cs, e - cs, TK_PREPROC);
            i = e; cs = e;
            if (j >= 0) kind = 0;
        } else if (kind == 4 || kind == 5) {
            const char *q = kind == 4 ? "\"" : "'";
            int j = lx_find(s, i, n, q), e = j >= 0 ? j + 1 : n;
            lx_e(x, i, e - i, TK_STRING);
            i = e;
            if (j >= 0) kind = 3;
        } else if (kind == 7) {
            int j = lx_find_ci(s, i, n, (fl & 1) ? "</script" : "</style");
            i = j >= 0 ? j : n;      /* the text of the script or style is left plain */
            if (j >= 0) { kind = 0; fl = 0; }
        } else {      /* 3: in a tag */
            char c = s[i];
            if (lx_ws(c)) i++;
            else if (c == '>') {
                lx_e(x, i, 1, TK_PUNCT); i++;
                kind = !(fl & 4) && (fl & 3) ? 7 : 0;
                if (kind == 0) fl = 0;
            } else if (c == '/' && i + 1 < n && s[i + 1] == '>') { lx_e(x, i, 2, TK_PUNCT); i += 2; kind = 0; fl = 0; }
            else if (c == '=' || c == '/') { lx_e(x, i, 1, TK_PUNCT); i++; }
            else if (c == '"' || c == '\'') {
                int j = lx_find(s, i + 1, n, c == '"' ? "\"" : "'"), e = j >= 0 ? j + 1 : n;
                lx_e(x, i, e - i, TK_STRING);
                i = e;
                if (j < 0) kind = c == '"' ? 4 : 5;
            } else {
                int j = i + 1;
                while (j < n && !lx_ws(s[j]) && !strchr("=/>\"'<", s[j])) j++;
                lx_e(x, i, j - i, TK_TYPE);
                i = j;
            }
        }
    }
    *state = kind == 0 ? 0 : kind | ((kind == 3 || kind == 4 || kind == 5 || kind == 7) ? fl << 8 : 0);
}

/* ======================================================================== */
/* CSS, SCSS, LESS                                                          */
/* ======================================================================== */

/* state kinds: 1 inside a comment, 2 inside a value (that goes on), 3 a selector that goes on; the depth of { is in the upper bits.
 * The line is read as selector, property or value, by what follows. */
enum { CSS_SEL, CSS_PROP, CSS_VAL, CSS_STMT };

/* the statement that starts at i is a selector: a { comes before any ; or }, or there is no colon, or it ends with a comma */
static bool css_is_sel(const cui__lx *x, int i, bool line_comments) {
    const char *s = x->s;
    int n = x->n, last = 0;
    bool colon = false, decl = false;      /* any colon, and one with white space after it: a declaration */
    for (int j = i; j < n; j++) {
        char c = s[j];
        if (c == '"' || c == '\'') { j = lx_quoted(s, j, n, c, true, false) - 1; last = c; continue; }
        if (c == '/' && j + 1 < n && s[j + 1] == '*') { int e = lx_find(s, j + 2, n, "*/"); if (e < 0) break; j = e + 1; continue; }
        if (line_comments && c == '/' && j + 1 < n && s[j + 1] == '/') break;
        if (c == '{') return true;
        if (c == ';' || c == '}') return false;
        if (c == ':') { colon = true; if (j + 1 >= n || lx_ws(s[j + 1])) decl = true; }
        if (!lx_ws(c)) last = c;
    }
    return !decl && (!colon || last == ',');
}

static void lx_css(cui__lx *x, const cui__lang *L, int *state) {
    const char *s = x->s;
    int n = x->n, kind = *state & 15, depth = *state >> 8, i = 0, mode;
    bool lc = L->line_comment != NULL;      /* scss and less: // comments */
    if (kind == 0 && depth == 0 && x->cap == 0 && !memchr(s, '{', (size_t)n) && !memchr(s, '/', (size_t)n)) return;
    mode = kind == 2 ? CSS_VAL : (kind == 3 || depth == 0) ? CSS_SEL : CSS_STMT;
    if (kind == 1) {
        int j = lx_find(s, 0, n, "*/"), e = j >= 0 ? j + 2 : n;
        lx_e(x, 0, e, TK_COMMENT);
        if (j < 0) return;
        i = e;
        mode = depth == 0 ? CSS_SEL : CSS_STMT;
    }
    kind = 0;
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = lx_find(s, i + 2, n, "*/"), e = j >= 0 ? j + 2 : n;
            lx_e(x, i, e - i, TK_COMMENT);
            i = e;
            if (j < 0) { kind = 1; break; }
            continue;
        }
        if (lc && c == '/' && i + 1 < n && s[i + 1] == '/') { lx_e(x, i, n - i, TK_COMMENT); break; }
        if (mode == CSS_STMT && c != '}' && c != ';') mode = css_is_sel(x, i, lc) ? CSS_SEL : CSS_PROP;
        if (c == '{') { depth++; lx_e(x, i, 1, TK_PUNCT); mode = CSS_STMT; i++; continue; }
        if (c == '}' || c == ';') {
            if (c == '}' && depth > 0) depth--;
            lx_e(x, i, 1, TK_PUNCT);
            mode = depth == 0 ? CSS_SEL : CSS_STMT;
            i++;
            continue;
        }
        if (c == '"' || c == '\'') { int j = lx_quoted(s, i, n, c, true, false); lx_e(x, i, j - i, TK_STRING); i = j; continue; }
        if (c == '@') {
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
            lx_e(x, i, j - i, TK_PREPROC);
            mode = CSS_VAL;
            i = j > i + 1 ? j : i + 1;
            continue;
        }
        if (c == '$' && i + 1 < n && (lx_alpha(s[i + 1]) || s[i + 1] == '_' || s[i + 1] == '-')) {      /* a scss variable */
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
            lx_e(x, i, j - i, TK_TYPE);
            i = j;
            continue;
        }
        if (mode == CSS_SEL) {
            if ((c == '.' || c == '#') && i + 1 < n && (lx_alnum(s[i + 1]) || s[i + 1] == '_' || s[i + 1] == '-')) {
                int j = i + 1;
                while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
                lx_e(x, i, j - i, TK_TYPE);
                i = j;
            } else if (c == ':' && i + 1 < n && (lx_alpha(s[i + 1]) || s[i + 1] == ':' || s[i + 1] == '-')) {
                int j = i + 1;
                if (s[j] == ':') j++;
                while (j < n && (lx_alnum(s[j]) || s[j] == '-')) j++;
                lx_e(x, i, j - i, TK_FUNC);
                i = j;
            } else if (lx_digit(c)) {
                int j = i + 1;
                while (j < n && (lx_digit(s[j]) || s[j] == '.' || s[j] == '%')) j++;
                lx_e(x, i, j - i, TK_NUMBER);
                i = j;
            } else if (lx_alpha(c) || c == '_' || c == '-') {
                int j = i + 1;
                while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
                lx_e(x, i, j - i, j < n && s[j] == '(' ? TK_FUNC : TK_KEYWORD);
                i = j;
            } else { lx_e(x, i, 1, TK_PUNCT); i++; }
            continue;
        }
        if (mode == CSS_PROP) {
            if (lx_alpha(c) || c == '_' || c == '-') {
                int j = i + 1;
                while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
                lx_e(x, i, j - i, TK_FUNC);
                i = j;
            } else { if (c == ':') mode = CSS_VAL; lx_e(x, i, 1, TK_PUNCT); i++; }
            continue;
        }
        /* a value */
        if (c == '#') {
            int j = i + 1;
            while (j < n && lx_xdigit(s[j])) j++;
            int h = j - i - 1;
            if ((h == 3 || h == 4 || h == 6 || h == 8) && !(j < n && cui__ident_char(s[j]))) { lx_e(x, i, j - i, TK_CONST); i = j; }
            else { lx_e(x, i, 1, TK_PUNCT); i++; }
        } else if (lx_digit(c) || ((c == '.' || c == '-' || c == '+') && i + 1 < n && (lx_digit(s[i + 1]) || (s[i + 1] == '.' && i + 2 < n && lx_digit(s[i + 2]))))) {
            int j = i + 1;
            while (j < n && (lx_digit(s[j]) || s[j] == '.')) j++;
            while (j < n && (lx_alpha(s[j]) || s[j] == '%')) j++;      /* the unit */
            lx_e(x, i, j - i, TK_NUMBER);
            i = j;
        } else if (c == '!' && i + 1 < n && lx_alpha(s[i + 1])) {
            int j = i + 1;
            while (j < n && lx_alnum(s[j])) j++;
            lx_e(x, i, j - i, TK_PREPROC);
            i = j;
        } else if (lx_alpha(c) || c == '_' || (c == '-' && i + 1 < n && (lx_alpha(s[i + 1]) || s[i + 1] == '-'))) {
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || s[j] == '-' || s[j] == '_')) j++;
            if (j < n && s[j] == '(') lx_e(x, i, j - i, TK_FUNC);
            i = j;
        } else { lx_e(x, i, 1, TK_PUNCT); i++; }
    }
    if (kind != 1) kind = mode == CSS_VAL ? 2 : (mode == CSS_SEL && depth > 0) ? 3 : 0;
    if (depth > 100000) depth = 100000;
    *state = kind | (depth << 8);
}

/* ======================================================================== */
/* PowerShell                                                               */
/* ======================================================================== */

/* state: 1 inside <# #>, 2 inside an @" "@ here-string, 3 inside an @' '@ one */
static const char *const ps_kw[] = { "if", "elseif", "else", "switch", "foreach", "for", "while", "do", "until", "break", "continue", "return",
                                     "function", "filter", "param", "begin", "process", "end", "try", "catch", "finally", "throw", "trap",
                                     "exit", "in", "data", "class", "enum", "using", "workflow", "parallel", "default", NULL };
static const char *const ps_ops[] = { "eq", "ne", "gt", "ge", "lt", "le", "like", "notlike", "match", "notmatch", "contains", "notcontains",
                                      "in", "notin", "is", "isnot", "as", "and", "or", "not", "xor", "band", "bor", "bnot", "bxor", "shl", "shr",
                                      "replace", "split", "join", "f", "ceq", "cne", "ieq", "ine", NULL };
static const char *const ps_const[] = { "true", "false", "null", NULL };

static void lx_powershell(cui__lx *x, int *state) {
    const char *s = x->s;
    int n = x->n, kind = *state & 15, i = 0;
    if (kind == 0 && x->cap == 0 && !memchr(s, '@', (size_t)n) && lx_find(s, 0, n, "<#") < 0) return;
    if (kind == 1) {
        int j = lx_find(s, 0, n, "#>"), e = j >= 0 ? j + 2 : n;
        lx_e(x, 0, e, TK_COMMENT);
        if (j < 0) return;
        i = e;
    } else if (kind == 2 || kind == 3) {      /* the end must start the line */
        if (n >= 2 && s[1] == '@' && s[0] == (kind == 2 ? '"' : '\'')) { lx_e(x, 0, 2, TK_STRING); i = 2; }
        else { lx_e(x, 0, n, TK_STRING); return; }
    }
    kind = 0;
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '#') { lx_e(x, i, n - i, TK_COMMENT); break; }
        if (c == '<' && i + 1 < n && s[i + 1] == '#') {
            int j = lx_find(s, i + 2, n, "#>"), e = j >= 0 ? j + 2 : n;
            lx_e(x, i, e - i, TK_COMMENT);
            i = e;
            if (j < 0) kind = 1;
            continue;
        }
        if (c == '@' && i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '\'') && lx_skip_ws(s, i + 2, n) >= n) {      /* a here-string opens */
            lx_e(x, i, n - i, TK_STRING);
            kind = s[i + 1] == '"' ? 2 : 3;
            break;
        }
        if (c == '"' || c == '\'') {
            int j = i + 1;
            while (j < n) {
                if (c == '"' && s[j] == '`' && j + 1 < n) { j += 2; continue; }
                if (s[j] == c) { if (j + 1 < n && s[j + 1] == c) { j += 2; continue; } j++; break; }
                j++;
            }
            if (j > n) j = n;
            lx_e(x, i, j - i, TK_STRING);
            i = j;
            continue;
        }
        if (c == '$') {
            int j = i + 1;
            if (j < n && s[j] == '{') { while (j < n && s[j] != '}') j++; if (j < n) j++; }
            else if (j < n && strchr("$?^", s[j])) j++;
            else while (j < n && (cui__ident_char(s[j]) || (s[j] == ':' && j + 1 < n && cui__ident_start(s[j + 1])))) j++;
            if (j == i + 1) { lx_e(x, i, 1, TK_PUNCT); i++; continue; }
            lx_e(x, i, j - i, cui__in_list(ps_const, s + i + 1, j - i - 1, true) ? TK_CONST : TK_TYPE);
            i = j;
            continue;
        }
        if (lx_digit(c)) { int j = lx_number(s, i, n); lx_e(x, i, j - i, TK_NUMBER); i = j; continue; }
        if (c == '-' && i + 1 < n && lx_alpha(s[i + 1]) && (i == 0 || lx_ws(s[i - 1]) || strchr("({[,;|", s[i - 1]))) {   /* -Name, or an operator */
            int j = i + 1;
            while (j < n && lx_alnum(s[j])) j++;
            lx_e(x, i, j - i, cui__in_list(ps_ops, s + i + 1, j - i - 1, true) ? TK_KEYWORD : TK_PREPROC);
            i = j;
            continue;
        }
        if (c == '[') {      /* [string], [System.IO.File] and [Parameter(...)] */
            int j = i + 1, k = j;
            while (k < n && (cui__ident_char(s[k]) || s[k] == '.')) k++;
            lx_e(x, i, 1, TK_PUNCT);
            if (k > j && cui__ident_start(s[j]) && k < n && (s[k] == ']' || s[k] == '(')) { lx_e(x, j, k - j, TK_TYPE); i = k; } else i = j;
            continue;
        }
        if (cui__ident_start(c)) {
            int j = i + 1;
            bool verb = false;
            while (j < n && cui__ident_char(s[j])) j++;
            while (j + 1 < n && s[j] == '-' && lx_alpha(s[j + 1])) { verb = true; j++; while (j < n && cui__ident_char(s[j])) j++; }   /* Verb-Noun */
            if (verb) lx_e(x, i, j - i, TK_FUNC);
            else if (cui__in_list(ps_kw, s + i, j - i, true)) lx_e(x, i, j - i, TK_KEYWORD);
            else if (lx_next(s, j, n) == '(') lx_e(x, i, j - i, TK_FUNC);
            i = j;
            continue;
        }
        lx_e(x, i, 1, TK_PUNCT);
        i++;
    }
    *state = kind;
}

/* ======================================================================== */
/* C# and Java                                                              */
/* ======================================================================== */

/* state: 1 inside a block comment, 2 inside a """ string (a java text block, a c# raw string), 3 inside a c# @"verbatim" string */
static const char *const cs_kw[] = { "abstract", "as", "base", "break", "case", "catch", "checked", "class", "const", "continue", "default",
                                     "delegate", "do", "else", "enum", "event", "explicit", "extern", "finally", "fixed", "for", "foreach", "goto",
                                     "if", "implicit", "in", "interface", "internal", "is", "lock", "namespace", "new", "operator", "out",
                                     "override", "params", "private", "protected", "public", "readonly", "ref", "return", "sealed", "sizeof",
                                     "stackalloc", "static", "struct", "switch", "this", "throw", "try", "typeof", "unchecked", "unsafe", "using",
                                     "virtual", "volatile", "while", "async", "await", "yield", "record", "init", "partial", "where", "when",
                                     "required", "nameof", "with", NULL };
static const char *const cs_ty[] = { "bool", "byte", "char", "decimal", "double", "float", "int", "long", "object", "sbyte", "short", "string",
                                     "uint", "ulong", "ushort", "void", "dynamic", "var", NULL };
static const char *const java_kw[] = { "abstract", "assert", "break", "case", "catch", "class", "const", "continue", "default", "do", "else",
                                       "enum", "extends", "final", "finally", "for", "goto", "if", "implements", "import", "instanceof", "interface",
                                       "native", "new", "package", "private", "protected", "public", "return", "static", "strictfp", "super",
                                       "switch", "synchronized", "this", "throw", "throws", "transient", "try", "volatile", "while", "record",
                                       "sealed", "permits", "yield", NULL };
static const char *const java_ty[] = { "boolean", "byte", "char", "double", "float", "int", "long", "short", "void", "var", "String", "Object", NULL };
static const char *const cl_const[] = { "true", "false", "null", NULL };

static void lx_clike(cui__lx *x, bool cs, int *state) {
    const char *s = x->s;
    int n = x->n, kind = *state & 15, i = 0;
    const char *const *kw = cs ? cs_kw : java_kw, *const *ty = cs ? cs_ty : java_ty;
    if (kind == 0 && x->cap == 0 && lx_find(s, 0, n, "/*") < 0 && lx_find(s, 0, n, "\"\"\"") < 0 && (!cs || lx_find(s, 0, n, "@\"") < 0)) return;
    if (kind == 1) {
        int j = lx_find(s, 0, n, "*/"), e = j >= 0 ? j + 2 : n;
        lx_e(x, 0, e, TK_COMMENT);
        if (j < 0) return;
        i = e;
    } else if (kind == 2) {
        int j = lx_triple(s, 0, n, '"', true);
        if (j < 0) { lx_e(x, 0, n, TK_STRING); return; }
        lx_e(x, 0, j + 3, TK_STRING);
        i = j + 3;
    } else if (kind == 3) {
        int j = 0;
        while (j < n && !(s[j] == '"' && !(j + 1 < n && s[j + 1] == '"'))) j += s[j] == '"' ? 2 : 1;
        if (j >= n) { lx_e(x, 0, n, TK_STRING); return; }
        lx_e(x, 0, j + 1, TK_STRING);
        i = j + 1;
    }
    kind = 0;
    int first = lx_skip_ws(s, 0, n);
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') { lx_e(x, i, n - i, TK_COMMENT); break; }      /* also /// */
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            int j = lx_find(s, i + 2, n, "*/"), e = j >= 0 ? j + 2 : n;
            lx_e(x, i, e - i, TK_COMMENT);
            i = e;
            if (j < 0) kind = 1;
            continue;
        }
        if (cs && c == '#' && i == first) {      /* #if, #region... */
            int j = i + 1;
            while (j < n && cui__ident_char(s[j])) j++;
            lx_e(x, i, j - i, TK_PREPROC);
            i = j;
            continue;
        }
        if (cs && (c == '@' || c == '$')) {      /* @"verbatim", $"interpolated", $@"both" */
            int j = i;
            bool verb = false;
            while (j < n && (s[j] == '@' || s[j] == '$') && j - i < 2) { verb = verb || s[j] == '@'; j++; }
            if (j < n && s[j] == '"') {
                if (verb) {
                    int k = j + 1;
                    while (k < n && !(s[k] == '"' && !(k + 1 < n && s[k + 1] == '"'))) k += s[k] == '"' ? 2 : 1;
                    if (k >= n) { lx_e(x, i, n - i, TK_STRING); kind = 3; break; }
                    lx_e(x, i, k + 1 - i, TK_STRING);
                    i = k + 1;
                } else { int k = lx_quoted(s, j, n, '"', true, false); lx_e(x, i, k - i, TK_STRING); i = k; }
                continue;
            }
        }
        if (c == '"') {
            if (i + 2 < n && s[i + 1] == '"' && s[i + 2] == '"') {      /* a text block, a raw string */
                int j = lx_triple(s, i + 3, n, '"', true);
                if (j < 0) { lx_e(x, i, n - i, TK_STRING); kind = 2; break; }
                lx_e(x, i, j + 3 - i, TK_STRING);
                i = j + 3;
                continue;
            }
            int j = lx_quoted(s, i, n, '"', true, false);
            lx_e(x, i, j - i, TK_STRING);
            i = j;
            continue;
        }
        if (c == '\'') { int j = lx_quoted(s, i, n, '\'', true, false); lx_e(x, i, j - i, TK_STRING); i = j; continue; }
        if (cs && c == '[' && i == first) {      /* an attribute [Obsolete] */
            int j = i + 1;
            lx_e(x, i, 1, TK_PUNCT);
            while (j < n && (cui__ident_char(s[j]) || s[j] == '.')) j++;
            lx_e(x, i + 1, j - i - 1, TK_PREPROC);
            i = j;
            continue;
        }
        if (!cs && c == '@' && i + 1 < n && cui__ident_start(s[i + 1])) {      /* an annotation @Override */
            int j = i + 1;
            while (j < n && (cui__ident_char(s[j]) || s[j] == '.')) j++;
            lx_e(x, i, j - i, lx_word_is(s, i + 1, j, "interface") ? TK_KEYWORD : TK_PREPROC);
            i = j;
            continue;
        }
        if (lx_digit(c)) { int j = lx_number(s, i, n); lx_e(x, i, j - i, TK_NUMBER); i = j; continue; }
        if (cui__ident_start(c) || (cs && c == '@')) {
            int j = i + 1;
            while (j < n && cui__ident_char(s[j])) j++;
            int len = j - i, k = TK_TEXT;
            if (cui__in_list(kw, s + i, len, false)) k = TK_KEYWORD;
            else if (cui__in_list(ty, s + i, len, false)) k = TK_TYPE;
            else if (cui__in_list(cl_const, s + i, len, false)) k = TK_CONST;
            else if (lx_next(s, j, n) == '(') k = TK_FUNC;
            else if (s[i] >= 'A' && s[i] <= 'Z' && len > 1) k = TK_TYPE;
            if (k != TK_TEXT) lx_e(x, i, len, k);
            i = j;
            continue;
        }
        lx_e(x, i, 1, TK_PUNCT);
        i++;
    }
    *state = kind;
}

/* ======================================================================== */
/* Batch                                                                    */
/* ======================================================================== */

static const char *const bat_kw[] = { "echo", "set", "if", "else", "for", "in", "do", "goto", "call", "exit", "setlocal", "endlocal", "start",
                                      "pushd", "popd", "cd", "chdir", "md", "mkdir", "rd", "rmdir", "del", "erase", "copy", "xcopy", "robocopy",
                                      "move", "ren", "rename", "type", "cls", "pause", "shift", "not", "exist", "defined", "errorlevel", "equ",
                                      "neq", "lss", "leq", "gtr", "geq", "title", "color", "path", "cmd", "prompt", "ver", "vol", "dir", "find",
                                      "findstr", "sort", "more", "choice", "timeout", "tasklist", "taskkill", "assoc", "mklink", NULL };

static void lx_batch(cui__lx *x) {
    const char *s = x->s;
    int n = x->n, i = lx_skip_ws(s, 0, n);
    bool cmd = true, text = false;      /* a command can come; the rest of an echo is text */
    if (i >= n) return;
    while (i < n && (s[i] == '@' || s[i] == '(')) { lx_e(x, i, 1, TK_PUNCT); i = lx_skip_ws(s, i + 1, n); }
    if (i >= n) return;
    if (s[i] == ':') {
        if (i + 1 < n && s[i + 1] == ':') { lx_e(x, i, n - i, TK_COMMENT); return; }
        int j = i + 1;
        while (j < n && !lx_ws(s[j])) j++;
        lx_e(x, i, j - i, TK_TYPE);
        return;
    }
    {
        int j = i;
        while (j < n && lx_alpha(s[j])) j++;
        if (lx_word_is(s, i, j, "rem") && (j >= n || lx_ws(s[j]))) { lx_e(x, i, n - i, TK_COMMENT); return; }
    }
    while (i < n) {
        char c = s[i];
        if (lx_ws(c)) { i++; continue; }
        if (c == '"') { int j = lx_quoted(s, i, n, '"', false, false); lx_e(x, i, j - i, TK_STRING); i = j; continue; }
        if (c == '%') {
            int j = i + 1;
            if (j < n && s[j] == '%') {      /* %%i, %%~dpi */
                j++;
                if (j < n && s[j] == '~') j++;
                while (j < n && lx_alnum(s[j])) j++;
            } else if (j < n && s[j] == '~') { j++; while (j < n && lx_alnum(s[j])) j++; }      /* %~dp0 */
            else if (j < n && (lx_digit(s[j]) || s[j] == '*')) j++;                              /* %1, %* */
            else { int k = j; while (k < n && s[k] != '%' && !lx_ws(s[k])) k++; j = k < n && s[k] == '%' && k > j ? k + 1 : j; }   /* %name% */
            lx_e(x, i, j - i, j > i + 1 && !(s[i + 1] == '%' && j == i + 2) ? TK_TYPE : TK_PUNCT);
            i = j;
            continue;
        }
        if (c == '!') {
            int k = i + 1;
            while (k < n && s[k] != '!' && !lx_ws(s[k])) k++;
            if (k < n && s[k] == '!' && k > i + 1) { lx_e(x, i, k + 1 - i, TK_TYPE); i = k + 1; } else { lx_e(x, i, 1, TK_PUNCT); i++; }
            continue;
        }
        if (c == '&' || c == '|') { cmd = true; text = false; lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        if (text) { i++; continue; }
        if ((c == '/' || c == '-') && i + 1 < n && lx_alpha(s[i + 1]) && (i == 0 || lx_ws(s[i - 1]))) {      /* a switch /a, -p */
            int j = i + 1;
            while (j < n && lx_alnum(s[j])) j++;
            lx_e(x, i, j - i, TK_PREPROC);
            i = j;
            continue;
        }
        if (lx_digit(c)) { int j = i + 1; while (j < n && lx_digit(s[j])) j++; if (!(j < n && cui__ident_char(s[j]))) lx_e(x, i, j - i, TK_NUMBER); i = j; continue; }
        if (lx_alpha(c) || c == '_') {
            int j = i + 1;
            while (j < n && (lx_alnum(s[j]) || s[j] == '_')) j++;
            if (cui__in_list(bat_kw, s + i, j - i, true)) {
                lx_e(x, i, j - i, TK_KEYWORD);
                if (cmd && lx_word_is(s, i, j, "echo")) text = true;
            }
            cmd = false;
            i = j;
            continue;
        }
        if (strchr("=()<>^,", c)) { lx_e(x, i, 1, TK_PUNCT); i++; continue; }
        i++;
    }
}

int cui__tokenize_x(const cui__lang *L, const char *s, int n, int *state, cui__tok *out, int cap) {
    cui__lx x = { s, n, 0, cap, 0, out };
    switch (L->mode) {
    case CUI__LM_YAML: lx_yaml(&x, state); break;
    case CUI__LM_TOML: lx_toml(&x, state); break;
    case CUI__LM_INI: lx_ini(&x); break;
    case CUI__LM_XML: lx_xml(&x, false, state); break;
    case CUI__LM_HTML: lx_xml(&x, true, state); break;
    case CUI__LM_CSS: lx_css(&x, L, state); break;
    case CUI__LM_POWERSHELL: lx_powershell(&x, state); break;
    case CUI__LM_CSHARP: lx_clike(&x, true, state); break;
    case CUI__LM_JAVA: lx_clike(&x, false, state); break;
    case CUI__LM_BATCH: lx_batch(&x); break;
    default: break;
    }
    lx_gap(&x, n);
    return x.count;
}
