/* Markdown view: a CommonMark-ish subset rendered as native content.
 *
 * The text is parsed every frame straight from the caller's buffer. Blocks are
 * found line by line (containers strip their prefixes into new line lists that
 * still point into the text), inline styles come from a delimiter stack, and
 * each paragraph is wrapped by hand, since one line mixes fonts, into a single
 * painted node with an invisible hit box per link. Code blocks and tables are
 * drawn by cui_code_block and cui_table. Memory comes from the frame arena.
 *
 * Selection: every inline block (callout titles too), table header cell, code
 * block and diagram gets a number in document order, and a position is (block,
 * byte offset into the block's runs joined, a hard break counting one byte; for
 * a diagram, into its labels a line each). Those stay put while the text and
 * the callouts' folds do, at any width. A press, a drag or a shift click asks
 * each block in turn for the position under the pointer, from its latest rect. */
#include "cui_internal.h"


#define MD_MAX_DEPTH 12      /* containers nested deeper show as text */
#define MD_WINDOW 64         /* how far back a closing * or _ looks for its opener */
#define MD_MAX_RUN 64        /* longer runs of * _ ~ are plain text */
#define MD_MAX_TICKS 32      /* longer runs of backticks are plain text */
#define MD_MAX_COLS 32       /* table columns beyond this are dropped */
#define MD_SHEAR 0.2f        /* slant of the synthesized italic (about 11 degrees) */

/* ------------------------------------------------------------------------ */
/* Lines                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct { const char *s; int len, off; } md_line;   /* off: byte offset of s in the text */

static bool md_sp(char c) { return c == ' ' || c == '\t'; }

static int md_indent(md_line L, int *bytes) {
    int col = 0, i = 0;
    while (i < L.len && md_sp(L.s[i])) { col = L.s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1; i++; }
    if (bytes) *bytes = i;
    return col;
}
static bool md_blank(md_line L) {
    for (int i = 0; i < L.len; i++) if (!md_sp(L.s[i])) return false;
    return true;
}
static md_line md_skip(md_line L, int bytes) {
    if (bytes > L.len) bytes = L.len;
    L.s += bytes; L.len -= bytes; L.off += bytes;
    return L;
}
static md_line md_strip_cols(md_line L, int cols) {
    int col = 0, i = 0;
    while (i < L.len && col < cols && md_sp(L.s[i])) { col = L.s[i] == '\t' ? (col / 4 + 1) * 4 : col + 1; i++; }
    return md_skip(L, i);
}
static md_line md_trim(md_line L) {
    int b;
    md_indent(L, &b);
    L = md_skip(L, b);
    while (L.len > 0 && md_sp(L.s[L.len - 1])) L.len--;
    return L;
}
static bool md_has(md_line L, char c) { return L.len > 0 && memchr(L.s, c, (size_t)L.len) != NULL; }

/* ------------------------------------------------------------------------ */
/* Inline parsing                                                           */
/* ------------------------------------------------------------------------ */

enum { MS_STRONG = 1, MS_EM = 2, MS_STRIKE = 4, MS_CODE = 8, MS_BREAK = 16 };
typedef struct { const char *p; int len; uint8_t style; int link; int at; } md_run;   /* at: offset in the block's text */
typedef struct { const char *url; int len; bool www, pending, autolink, wiki; } md_link;
typedef struct { md_run *runs; int nruns; md_link *links; int nlinks; bool ws; } md_inline;   /* ws: spaces and tabs keep their width (plain text) */

enum { TK_TXT, TK_DELIM, TK_CODE, TK_BR, TK_LOPEN, TK_LCLOSE, TK_AUTO };
typedef struct {
    const char *p; int len;
    uint8_t kind, ch, can_open, can_close, hidden, image;
    int16_t count, orig;
    int8_t pre[3], post[3];   /* strong, em, strike: closes before / opens after this token */
    int link;
} md_tok;

static bool md_ws(unsigned char c) { return c == 0 || c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool md_punct(unsigned char c) { return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126); }
static bool md_url_lead(unsigned char c) { return md_ws(c) || c == '(' || c == '*' || c == '_' || c == '~' || c == '"' || c == '\''; }

typedef struct { md_tok *t; int n, cap; } md_toks;
static md_tok *md_add(md_toks *T, int kind, const char *p, int len) {
    if (T->n >= T->cap) return NULL;
    md_tok *t = &T->t[T->n++];
    memset(t, 0, sizeof *t);
    t->kind = (uint8_t)kind; t->p = p; t->len = len; t->link = -1;
    return t;
}

/* (destination "title") after a ]. Returns 1 and the end, 0 when it is not a
 * link, -1 when the text ends first. */
static int md_link_dest(const char *s, int n, int at, const char **url, int *ulen, int *end) {
    if (at >= n) return -1;
    if (s[at] != '(') return 0;
    int j = at + 1;
    while (j < n && md_ws((unsigned char)s[j])) j++;
    if (j >= n) return -1;
    int us = j, ue;
    if (s[j] == '<') {
        us = ++j;
        while (j < n && s[j] != '>' && s[j] != '\n') { if (s[j] == '\\' && j + 1 < n) j++; j++; }
        if (j >= n) return -1;
        if (s[j] != '>') return 0;
        ue = j++;
    } else {
        int depth = 0;
        while (j < n && !md_ws((unsigned char)s[j])) {
            if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
            if (s[j] == '(') { if (++depth > 32) return 0; }
            else if (s[j] == ')') { if (depth == 0) break; depth--; }
            j++;
        }
        if (j >= n) return -1;
        ue = j;
    }
    while (j < n && md_ws((unsigned char)s[j])) j++;
    if (j >= n) return -1;
    if (s[j] == '"' || s[j] == '\'' || s[j] == '(') {
        char q = s[j] == '(' ? ')' : s[j];
        j++;
        while (j < n && s[j] != q) { if (s[j] == '\\' && j + 1 < n) j++; j++; }
        if (j >= n) return -1;
        j++;
        while (j < n && md_ws((unsigned char)s[j])) j++;
        if (j >= n) return -1;
    }
    if (s[j] != ')') return 0;
    *url = s + us; *ulen = ue - us; *end = j + 1;
    return 1;
}

/* <scheme:...> autolink: index of the closing >, or -1 */
static int md_autolink(const char *s, int n, int i) {
    int j = i + 1, k = 0;
    if (j >= n || !((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z'))) return -1;
    while (j < n && k < 32 && ((s[j] >= 'a' && s[j] <= 'z') || (s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= '0' && s[j] <= '9') ||
                               s[j] == '+' || s[j] == '.' || s[j] == '-')) { j++; k++; }
    if (k < 2 || j >= n || s[j] != ':') return -1;
    while (j < n && s[j] != '>' && s[j] != '<' && !md_ws((unsigned char)s[j])) j++;
    return j < n && s[j] == '>' ? j : -1;
}

/* <br>, <br/>, <br />: end of the tag, or -1 */
static int md_br_tag(const char *s, int n, int i) {
    if (i + 3 >= n || (s[i + 1] != 'b' && s[i + 1] != 'B') || (s[i + 2] != 'r' && s[i + 2] != 'R')) return -1;
    int j = i + 3;
    while (j < n && s[j] == ' ') j++;
    if (j < n && s[j] == '/') j++;
    return j < n && s[j] == '>' ? j + 1 : -1;
}

/* bare https://, http:// and www. links: end of the URL, or i */
static int md_bare_url(const char *s, int n, int i, bool *www) {
    int pre = 0;
    if (n - i > 8 && !strncmp(s + i, "https://", 8)) pre = 8;
    else if (n - i > 7 && !strncmp(s + i, "http://", 7)) pre = 7;
    else if (n - i > 4 && !strncmp(s + i, "www.", 4)) { pre = 4; *www = true; }
    if (!pre) return i;
    int e = i + pre;
    while (e < n && !md_ws((unsigned char)s[e]) && s[e] != '<') e++;
    while (e > i + pre && strchr("?!.,:*_~'\";", s[e - 1])) e--;
    if (e > i + pre && s[e - 1] == ')') {
        int open = 0, close = 0;
        for (int k = i; k < e; k++) { if (s[k] == '(') open++; else if (s[k] == ')') close++; }
        if (close > open) e--;
    }
    return e > i + pre ? e : i;
}

static void md_emit(md_run *runs, int *nr, const char *p, int len, int style, int link) {
    if (len <= 0 && !(style & MS_BREAK)) return;
    if (*nr > 0 && !(style & MS_BREAK)) {
        md_run *q = &runs[*nr - 1];
        if (q->style == style && q->link == link && q->p + q->len == p) { q->len += len; return; }
    }
    md_run *r = &runs[(*nr)++];
    r->p = p; r->len = len; r->style = (uint8_t)style; r->link = link;
}

/* Parses inline markdown into styled runs. tail: the text is still arriving
 * and this is its end, so unclosed markers render as if closed. */
static md_inline md_parse_inline(cui_ctx *ui, const char *s, int n, bool tail) {
    md_inline out;
    memset(&out, 0, sizeof out);
    if (n <= 0) return out;
    int specials = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '*' || c == '_' || c == '~' || c == '`' || c == '[' || c == ']' || c == '!' || c == '\\' || c == '<' || c == '&' || c == '\n') specials++;
        else if ((c == 'h' || c == 'w') && (i == 0 || md_url_lead((unsigned char)s[i - 1]))) specials++;
    }
    md_toks T;
    T.cap = specials * 2 + 2; T.n = 0;
    T.t = cui__frame_alloc(ui, sizeof(md_tok) * (size_t)T.cap);
    int link_cap = specials + 1, nl = 0;
    md_link *links = cui__frame_alloc(ui, sizeof(md_link) * (size_t)link_cap);
    int stack[32], nb = 0;            /* open [ and ![ tokens */
    int no_tick[MD_MAX_TICKS + 1];    /* no closing run of this length at or after */
    for (int k = 0; k <= MD_MAX_TICKS; k++) no_tick[k] = n + 1;
    int i = 0, ts = 0;
#define MD_FLUSH() do { if (i > ts) md_add(&T, TK_TXT, s + ts, i - ts); } while (0)
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\' && i + 1 < n && md_punct((unsigned char)s[i + 1])) {
            MD_FLUSH();
            md_add(&T, TK_TXT, s + i + 1, 1);
            i += 2; ts = i;
            continue;
        }
        if (c == '\n') { MD_FLUSH(); md_add(&T, TK_BR, s + i, 0); i++; ts = i; continue; }
        if (c == '`') {
            int k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            MD_FLUSH();
            int close = -1;
            if (k <= MD_MAX_TICKS && i + k < no_tick[k]) {
                int j = i + k;
                while (j < n) {
                    if (s[j] != '`') { j++; continue; }
                    int m = j;
                    while (m < n && s[m] == '`') m++;
                    if (m - j == k) { close = j; break; }
                    j = m;
                }
                if (close < 0) no_tick[k] = i + k;
            }
            int cs = i + k, ce = close >= 0 ? close : n;
            if (close >= 0 || (tail && k <= MD_MAX_TICKS)) {
                bool all_sp = true;
                for (int q = cs; q < ce; q++) if (s[q] != ' ') { all_sp = false; break; }
                if (close >= 0 && ce - cs >= 2 && s[cs] == ' ' && s[ce - 1] == ' ' && !all_sp) { cs++; ce--; }
                md_add(&T, TK_CODE, s + cs, ce - cs);
                i = close >= 0 ? close + k : n;
            } else {
                md_add(&T, TK_TXT, s + i, k);
                i += k;
            }
            ts = i;
            continue;
        }
        if (c == '*' || c == '_' || c == '~') {
            int k = 0;
            while (i + k < n && s[i + k] == (char)c) k++;
            MD_FLUSH();
            md_tok *t = md_add(&T, TK_DELIM, s + i, k);
            if (t) {
                unsigned char before = i > 0 ? (unsigned char)s[i - 1] : 0, after = i + k < n ? (unsigned char)s[i + k] : 0;
                bool lf = !md_ws(after) && (!md_punct(after) || md_ws(before) || md_punct(before));
                bool rf = !md_ws(before) && (!md_punct(before) || md_ws(after) || md_punct(after));
                t->ch = c;
                t->count = t->orig = (int16_t)CUI_MIN(k, 127);
                if (c == '_') { t->can_open = lf && (!rf || md_punct(before)); t->can_close = rf && (!lf || md_punct(after)); }
                else { t->can_open = lf; t->can_close = rf; }
                if (k > MD_MAX_RUN || (c == '~' && k != 2)) t->can_open = t->can_close = 0;
                if (tail && i + k >= n) { t->hidden = 1; t->can_open = t->can_close = 0; }   /* may still grow */
            }
            i += k; ts = i;
            continue;
        }
        if ((c == '[' && i + 1 < n && s[i + 1] == '[') || (c == '!' && i + 2 < n && s[i + 1] == '[' && s[i + 2] == '[')) {
            /* [[target]], [[target|label]], [[target#heading|label]] and the ![[embed]] form */
            int os = i + (c == '!' ? 3 : 2), e = os;
            bool closed = false;
            while (e < n && s[e] != '\n') {
                if (s[e] == ']' && e + 1 < n && s[e + 1] == ']') { closed = true; break; }
                e++;
            }
            bool open_tail = !closed && tail && e >= n;
            if (open_tail && e > os && s[e - 1] == ']') e--;   /* half of the closing ]] */
            if ((closed || open_tail) && nl < link_cap) {
                int a = os, b = e, bar = -1;
                for (int q = a; q < b; q++) if (s[q] == '|') { bar = q; break; }
                int te = bar >= 0 ? bar : b;                  /* target: a..te, label: bar+1..b */
                int ta = a, la = bar + 1, lb = b;
                while (ta < te && md_sp(s[ta])) ta++;
                while (te > ta && md_sp(s[te - 1])) te--;
                if (bar >= 0) { while (la < lb && md_sp(s[la])) la++; while (lb > la && md_sp(s[lb - 1])) lb--; }
                const char *dp = NULL;
                int dl = 0;
                if (bar >= 0 && lb > la) { dp = s + la; dl = lb - la; }
                else {
                    int hash = -1;
                    for (int q = ta; q < te; q++) if (s[q] == '#') { hash = q; break; }
                    if (hash >= 0 && te - hash > 1) {
                        if (hash == ta) { dp = s + hash + 1; dl = te - hash - 1; }
                        else { dp = cui__frame_printf(ui, "%.*s > %.*s", hash - ta, s + ta, te - hash - 1, s + hash + 1); dl = (int)strlen(dp); }
                    } else if (te > ta) { dp = s + ta; dl = te - ta; }
                }
                if (dl > 0) {
                    MD_FLUSH();
                    md_tok *t = md_add(&T, TK_AUTO, dp, dl);
                    if (t) {
                        links[nl].url = s + ta; links[nl].len = te - ta;
                        links[nl].autolink = links[nl].wiki = true; links[nl].pending = open_tail;
                        t->link = nl++;
                    }
                    i = closed ? e + 2 : n; ts = i;
                    continue;
                }
                if (open_tail) { MD_FLUSH(); i = n; ts = n; continue; }   /* [[ with nothing yet: held back */
            }
        }
        if (c == '[' || (c == '!' && i + 1 < n && s[i + 1] == '[')) {
            int k = c == '!' ? 2 : 1;
            MD_FLUSH();
            md_tok *t = md_add(&T, TK_LOPEN, s + i, k);
            if (t) {
                t->image = c == '!';
                if (nb < 32) stack[nb++] = T.n - 1;
                else t->kind = TK_TXT;
            }
            i += k; ts = i;
            continue;
        }
        if (c == ']') {
            MD_FLUSH();
            const char *url = NULL;
            int ulen = 0, end = i + 1;
            int st = md_link_dest(s, n, i + 1, &url, &ulen, &end);
            int ob = nb > 0 ? stack[nb - 1] : -1;
            bool pending = tail && st < 0;
            if (ob >= 0 && nl < link_cap && (st == 1 || pending)) {
                md_link *L = &links[nl];
                L->url = url; L->len = ulen; L->pending = st != 1;
                T.t[ob].link = nl;
                md_tok *t = md_add(&T, TK_LCLOSE, s + i, 0);
                if (t) t->link = nl;
                nl++;
                bool image = T.t[ob].image;
                nb--;
                if (!image) {   /* links do not nest: earlier [ become text */
                    int keep = 0;
                    for (int q = 0; q < nb; q++) if (T.t[stack[q]].image) stack[keep++] = stack[q];
                    nb = keep;
                }
                i = st == 1 ? end : n;
            } else {
                if (ob >= 0) nb--;
                md_add(&T, TK_TXT, s + i, 1);
                i++;
            }
            ts = i;
            continue;
        }
        if (c == '<') {
            int e = md_br_tag(s, n, i);
            if (e > 0) { MD_FLUSH(); md_add(&T, TK_BR, s + i, 0); i = e; ts = i; continue; }
            e = md_autolink(s, n, i);
            if (e > 0 && nl < link_cap) {
                MD_FLUSH();
                md_tok *t = md_add(&T, TK_AUTO, s + i + 1, e - i - 1);
                if (t) {
                    links[nl].url = s + i + 1; links[nl].len = e - i - 1; links[nl].autolink = true;
                    t->link = nl++;
                }
                i = e + 1; ts = i;
                continue;
            }
        }
        if (c == '&') {
            static const char *const ent[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                                                 { "&#39;", "'" }, { "&nbsp;", "\xc2\xa0" } };
            int hit = -1;
            for (int k = 0; k < CUI_COUNTOF(ent); k++) {
                int el = (int)strlen(ent[k][0]);
                if (n - i >= el && !strncmp(s + i, ent[k][0], (size_t)el)) { hit = k; break; }
            }
            if (hit >= 0) {
                MD_FLUSH();
                md_add(&T, TK_TXT, ent[hit][1], (int)strlen(ent[hit][1]));
                i += (int)strlen(ent[hit][0]); ts = i;
                continue;
            }
        }
        if ((c == 'h' || c == 'w') && (i == 0 || md_url_lead((unsigned char)s[i - 1]))) {
            bool www = false;
            int e = md_bare_url(s, n, i, &www);
            if (e > i && nl < link_cap) {
                MD_FLUSH();
                md_tok *t = md_add(&T, TK_AUTO, s + i, e - i);
                if (t) {
                    links[nl].url = s + i; links[nl].len = e - i; links[nl].www = www; links[nl].autolink = true;
                    t->link = nl++;
                }
                i = e; ts = i;
                continue;
            }
        }
        i++;
    }
    MD_FLUSH();
#undef MD_FLUSH
    /* an unclosed [ at the end may still become a link: hold it back */
    if (tail) for (int q = 0; q < nb; q++) T.t[stack[q]].hidden = 1;

    /* emphasis: each closer looks back for the nearest opener of its kind */
    int *dl = cui__frame_alloc(ui, sizeof(int) * (size_t)(T.n + 1)), nd = 0;
    for (int k = 0; k < T.n; k++) if (T.t[k].kind == TK_DELIM && (T.t[k].can_open || T.t[k].can_close)) dl[nd++] = k;
    for (int ci = 0; ci < nd; ci++) {
        md_tok *C = &T.t[dl[ci]];
        if (!C->can_close) continue;
        while (C->count > 0) {
            int found = -1;
            for (int oi = ci - 1; oi >= 0 && ci - oi <= MD_WINDOW; oi--) {
                md_tok *O = &T.t[dl[oi]];
                if (O->ch != C->ch || !O->can_open || O->count <= 0) continue;
                if (C->ch == '~') { if (O->count < 2 || C->count < 2) continue; }
                else if ((O->can_close || C->can_open) && (O->orig + C->orig) % 3 == 0 && !(O->orig % 3 == 0 && C->orig % 3 == 0)) continue;
                found = oi;
                break;
            }
            if (found < 0) break;
            md_tok *O = &T.t[dl[found]];
            int use = C->ch == '~' ? 2 : (O->count >= 2 && C->count >= 2 ? 2 : 1);
            int st = C->ch == '~' ? 2 : (use == 2 ? 0 : 1);
            O->post[st]++; C->pre[st]++;
            O->count = (int16_t)(O->count - use); C->count = (int16_t)(C->count - use);
            for (int k = found + 1; k < ci; k++) T.t[dl[k]].can_open = T.t[dl[k]].can_close = 0;
        }
    }
    if (tail) {   /* openers still waiting for their closer style the rest of the text */
        for (int k = 0; k < nd; k++) {
            md_tok *O = &T.t[dl[k]];
            if (!O->can_open || O->count <= 0) continue;
            if (O->ch == '~') { if (O->count >= 2) O->post[2]++; }
            else { if (O->count >= 2) O->post[0]++; if (O->count % 2 == 1) O->post[1]++; }
            O->count = 0;
        }
    }

    /* runs */
    md_run *runs = cui__frame_alloc(ui, sizeof(md_run) * (size_t)(T.n + 1));
    int nr = 0, cnt[3] = { 0, 0, 0 }, cur = -1;
    for (int k = 0; k < T.n; k++) {
        md_tok *t = &T.t[k];
        for (int q = 0; q < 3; q++) cnt[q] -= t->pre[q];
        int style = (cnt[0] > 0 ? MS_STRONG : 0) | (cnt[1] > 0 ? MS_EM : 0) | (cnt[2] > 0 ? MS_STRIKE : 0);
        switch (t->kind) {
        case TK_TXT: md_emit(runs, &nr, t->p, t->len, style, cur); break;
        case TK_DELIM: if (!t->hidden && t->count > 0) md_emit(runs, &nr, t->p, t->count, style, cur); break;
        case TK_CODE: md_emit(runs, &nr, t->p, t->len, style | MS_CODE, cur); break;
        case TK_BR: md_emit(runs, &nr, t->p, 0, MS_BREAK, -1); break;
        case TK_LOPEN:
            if (t->link >= 0) cur = t->link;
            else if (!t->hidden) md_emit(runs, &nr, t->p, t->len, style, cur);
            break;
        case TK_LCLOSE: cur = -1; break;
        case TK_AUTO: md_emit(runs, &nr, t->p, t->len, style, cur >= 0 ? cur : t->link); break;
        default: break;
        }
        for (int q = 0; q < 3; q++) cnt[q] += t->post[q];
    }
    for (int k = 0, at = 0; k < nr; k++) { runs[k].at = at; at += (runs[k].style & MS_BREAK) ? 1 : runs[k].len; }
    out.runs = runs; out.nruns = nr; out.links = links; out.nlinks = nl;
    return out;
}

static char *md_plain(cui_ctx *ui, const md_inline *in) {
    int total = 0;
    for (int k = 0; k < in->nruns; k++) total += (in->runs[k].style & MS_BREAK) ? 1 : in->runs[k].len;
    char *out = cui__frame_alloc(ui, (size_t)total + 1);
    int o = 0;
    for (int k = 0; k < in->nruns; k++) {
        if (in->runs[k].style & MS_BREAK) { out[o++] = ' '; continue; }
        memcpy(out + o, in->runs[k].p, (size_t)in->runs[k].len);
        o += in->runs[k].len;
    }
    out[o] = 0;
    return out;
}

/* ------------------------------------------------------------------------ */
/* Inline layout                                                            */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui_font reg, bold, mono;
    float size, mono_size, lh;   /* points, scaled */
    float pad;                   /* inline code: padding inside its tint */
} md_font;

typedef struct { const char *p; int len; float x, w; int line, run, link, at; uint8_t style, lead; } md_seg;   /* at: offset in the block's text */
typedef struct { md_seg *seg; int n, cap; int lines; float widest; } md_lay;

static cui_font md_font_of(const md_font *f, int style) { return (style & MS_CODE) ? f->mono : (style & MS_STRONG) ? f->bold : f->reg; }
static float md_size_of(const md_font *f, int style) { return (style & MS_CODE) ? f->mono_size : f->size; }
static bool md_space_char(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static float md_piece_w(cui_ctx *ui, const md_font *f, const md_run *R, int b, int e) {
    float w = cui__text_width(ui, md_font_of(f, R->style), md_size_of(f, R->style), R->p + b, e - b, false);
    if (R->style & MS_CODE) { if (b == 0) w += f->pad; if (e == R->len) w += f->pad; }
    return w;
}

/* Tabs go to the next stop of MD_TAB_COLS space widths, counted from the line's left edge. */
#define MD_TAB_COLS 4
static float md_tab_to(float x, float space_w) {
    float stop = space_w * MD_TAB_COLS;
    return stop > 0 ? (floorf(x / stop + 0.001f) + 1.0f) * stop : x;
}

/* One line width per hard break, nothing wrapped: the natural width. */
static float md_runs_width(cui_ctx *ui, const md_inline *in, const md_font *f) {
    float w = 0, best = 0;
    for (int k = 0; k < in->nruns; k++) {
        const md_run *R = &in->runs[k];
        if (R->style & MS_BREAK) { best = CUI_MAX(best, w); w = 0; continue; }
        if (in->ws) {   /* a tab is not a glyph's width */
            float spw = cui__text_width(ui, md_font_of(f, R->style), md_size_of(f, R->style), " ", 1, false);
            for (int a = 0; a < R->len;) {
                int e = a;
                while (e < R->len && R->p[e] != '	') e++;
                if (e > a) w += md_piece_w(ui, f, R, a, e);
                if (e < R->len) w = md_tab_to(w, spw);
                a = e + 1;
            }
            continue;
        }
        w += md_piece_w(ui, f, R, 0, R->len);
    }
    return CUI_MAX(best, w);
}

static void md_seg_add(cui_ctx *ui, md_lay *L, const md_inline *in, int run, int off, const char *p, int len, float x, float w) {
    const md_run *R = &in->runs[run];
    if (L->n > 0) {
        md_seg *q = &L->seg[L->n - 1];
        if (q->line == L->lines && q->run == run && q->p + q->len == p) { q->len += len; q->w = x + w - q->x; return; }
    }
    if (L->n == L->cap) {
        int ncap = L->cap ? L->cap * 2 : 32;
        md_seg *ns = cui__frame_alloc(ui, sizeof(md_seg) * (size_t)ncap);
        if (L->n) memcpy(ns, L->seg, sizeof(md_seg) * (size_t)L->n);
        L->seg = ns; L->cap = ncap;
    }
    md_seg *s = &L->seg[L->n++];
    s->p = p; s->len = len; s->x = x; s->w = w; s->line = L->lines; s->run = run; s->link = R->link; s->at = R->at + off;
    s->style = R->style; s->lead = (R->style & MS_CODE) && p == R->p;
}

static void md_finish_line(md_lay *L, int *first, float x, float W, int align) {
    if ((align == CUI_TEXT_CENTER || align == CUI_TEXT_RIGHT) && W > x) {   /* left and start stay put */
        float dx = (W - x) * (align == CUI_TEXT_CENTER ? 0.5f : 1.0f);
        for (int k = *first; k < L->n; k++) L->seg[k].x += dx;
    }
    L->widest = CUI_MAX(L->widest, x);
    L->lines++;
    *first = L->n;
}

/* Greedy word wrap across runs. Words wider than the line break between characters. */
static void md_layout(cui_ctx *ui, const md_inline *in, const md_font *f, float W, int align, md_lay *L) {
    memset(L, 0, sizeof *L);
    float x = 0;
    int first = 0, ri = 0, bi = 0, sp_run = -1;
    const char *sp_p = NULL;
    while (ri < in->nruns) {
        const md_run *R = &in->runs[ri];
        if (R->style & MS_BREAK) { md_finish_line(L, &first, x, W, align); x = 0; sp_run = -1; ri++; bi = 0; continue; }
        if (bi >= R->len) { ri++; bi = 0; continue; }
        if (md_space_char(R->p[bi])) {
            if (sp_run < 0) { sp_run = ri; sp_p = R->p + bi; }
            while (bi < R->len && md_space_char(R->p[bi])) bi++;
            continue;
        }
        float ww = 0;
        for (int rj = ri, bj = bi; rj < in->nruns; rj++, bj = 0) {
            const md_run *Q = &in->runs[rj];
            if (Q->style & MS_BREAK) break;
            int e = bj;
            while (e < Q->len && !md_space_char(Q->p[e])) e++;
            if (e > bj) ww += md_piece_w(ui, f, Q, bj, e);
            if (e < Q->len) break;
        }
        float sw = 0, spw = 0;
        const char *sp_end = in->runs[ri].p + bi;   /* the blank run is sp_p up to here (one run: plain text has no styles) */
        if (sp_run >= 0 && (x > 0 || in->ws)) {
            const md_run *S = &in->runs[sp_run];
            spw = cui__text_width(ui, md_font_of(f, S->style), md_size_of(f, S->style), " ", 1, false);
            sw = spw;
            if (in->ws) {   /* the blanks as written: a space each, a tab to its stop */
                float ex = x;
                for (const char *q = sp_p; q < sp_end; q++) ex = *q == '	' ? md_tab_to(ex, spw) : *q == ' ' ? ex + spw : ex;
                sw = ex - x;
            }
        }
        if (x > 0 && x + sw + ww > W) { md_finish_line(L, &first, x, W, align); x = 0; sw = 0; }
        if (in->ws && sw > 0 && sp_run == ri) {
            const md_run *S = &in->runs[sp_run];
            for (const char *q = sp_p; q < sp_end; q++) {
                int off = (int)(q - S->p);
                if (*q == ' ') { md_seg_add(ui, L, in, sp_run, off, q, 1, x, spw); x += spw; }
                else if (*q == '	') { float nx = md_tab_to(x, spw); md_seg_add(ui, L, in, sp_run, off, " ", 1, x, nx - x); x = nx; }
            }
        } else if (sw > 0) { md_seg_add(ui, L, in, sp_run, (int)(sp_p - in->runs[sp_run].p), *sp_p == ' ' ? sp_p : " ", 1, x, sw); x += sw; }
        sp_run = -1;
        bool split = ww > W - x;
        while (ri < in->nruns) {
            const md_run *Q = &in->runs[ri];
            if (Q->style & MS_BREAK) break;
            int e = bi;
            while (e < Q->len && !md_space_char(Q->p[e])) e++;
            if (e > bi) {
                if (!split) {
                    float pw = md_piece_w(ui, f, Q, bi, e);
                    md_seg_add(ui, L, in, ri, bi, Q->p + bi, e - bi, x, pw);
                    x += pw;
                } else {
                    cui_font qf = md_font_of(f, Q->style);
                    float qs = md_size_of(f, Q->style);
                    bool code = (Q->style & MS_CODE) != 0;
                    float x0 = x;
                    if (code && bi == 0) x += f->pad;
                    int chunk = bi, k = bi;
                    while (k < e) {
                        int nk = cui__utf8_next(Q->p, e, k);
                        if (nk <= k) nk = k + 1;
                        float cw = cui__text_width(ui, qf, qs, Q->p + k, nk - k, false);
                        if (x + cw > W && x > 0 && k > chunk) {
                            md_seg_add(ui, L, in, ri, chunk, Q->p + chunk, k - chunk, x0, x - x0);
                            md_finish_line(L, &first, x, W, align);
                            x = 0; x0 = 0; chunk = k;
                        }
                        x += cw;
                        k = nk;
                    }
                    if (code && e == Q->len) x += f->pad;
                    md_seg_add(ui, L, in, ri, chunk, Q->p + chunk, e - chunk, x0, x - x0);
                }
            }
            if (e < Q->len) { bi = e; break; }
            ri++; bi = 0;
        }
    }
    if (L->n > first) md_finish_line(L, &first, x, W, align);
}

/* ------------------------------------------------------------------------ */
/* Selection                                                                */
/* ------------------------------------------------------------------------ */

#define MD_SEL_END (1 << 30)   /* a block number past every block: the end of the text */
#define MD_DRAG_START 4.0f     /* points a press on a link travels before it selects instead */

typedef struct { int block, off; } md_pos;
static int md_pos_cmp(md_pos a, md_pos b) {
    if (a.block != b.block) return a.block < b.block ? -1 : 1;
    return (a.off > b.off) - (a.off < b.off);
}

enum { MD_BY_CHAR, MD_BY_WORD, MD_BY_BLOCK };
typedef struct {
    md_pos anchor, head;        /* equal: nothing selected */
    md_pos unit0, unit1;        /* the word or block a double or triple click took, kept while dragging */
    uint32_t text_hash;         /* the text the positions belong to */
    int text_len;
    uint8_t unit;               /* MD_BY_* */
    bool dragging, focused;
    bool anchored;              /* a click left the anchor in this text: Shift+click extends from it */
    bool link_drag;             /* a press on a link moved: select from where it went down, next frame */
} md_sel_state;

typedef struct md_code_sel { cui__code_sel cs; int block; struct md_code_sel *next; } md_code_sel;
typedef struct md_diag_sel { cui__mermaid_sel ms; int block; struct md_diag_sel *next; } md_diag_sel;

/* One frame of selection work, shared by the blocks as they are built and read by their paint. */
typedef struct {
    md_sel_state *st;           /* NULL: the view does not select */
    int seq;                    /* selectable blocks so far, in document order */
    md_pos lo, hi;              /* the selection in order; final by the time anything paints */
    cui_color color;
    bool hover;                 /* the pointer is on the view: look for text under it (I-beam) */
    bool over_text;
    /* the probe: the position under (x, y), from each block's latest rect */
    bool probe, pressing, extend;
    float x, y;
    bool found, band;           /* band: the candidate's rows span y (else it is the last block above y) */
    float dx;                   /* how far the band candidate is to the side of x */
    md_pos at;
    const md_run *runs; int nruns;   /* the candidate's text, for words and blocks */
    const char *code; int code_len;
    bool diag;                  /* the candidate is a diagram (its text is code) */
    int lab_a, lab_b;           /* then the label under the pointer, as bytes; lab_a < 0 when none */
    /* Ctrl+C: the selected text, gathered as the blocks go by */
    bool copying, copy_started;
    char *copy; int copy_len, copy_cap;
    char sep;                   /* goes before the next block's text: '\n', or '\t' between table cells */
    int gap;                    /* blank lines (plain text) copied after sep, before the next block's text */
    md_code_sel *codes;
    md_diag_sel *diags;
    uint32_t hash; bool hashed;  /* the text's hash, once taken this frame */
} md_sel;

/* x of byte k of a segment, from the segment's left edge */
static float md_seg_x(cui_ctx *ui, const md_font *f, const md_seg *s, int k) {
    if (k <= 0) return 0;
    if (k >= s->len) return s->w;
    return (s->lead ? f->pad : 0) + cui__text_width(ui, md_font_of(f, s->style), md_size_of(f, s->style), s->p, k, false);
}

/* The offset in the block's text nearest (lx, ly), relative to the block's top left. */
static int md_hit_inline(cui_ctx *ui, const md_lay *L, const md_font *f, float lx, float ly) {
    if (L->n == 0) return 0;
    int line = (int)floorf(ly / f->lh), first = -1, last = -1;
    line = CUI_CLAMP(line, 0, L->lines - 1);
    for (int k = 0; k < L->n; k++) {
        if (L->seg[k].line < line) continue;
        if (L->seg[k].line > line) {   /* an empty line between hard breaks sits just before the next text */
            if (first < 0) return CUI_MAX(0, L->seg[k].at - 1);
            break;
        }
        if (first < 0) first = k;
        last = k;
    }
    if (first < 0) return L->seg[L->n - 1].at + L->seg[L->n - 1].len;
    if (lx <= L->seg[first].x) return L->seg[first].at;
    for (int k = first; k <= last; k++) {
        const md_seg *s = &L->seg[k];
        if (lx >= s->x + s->w) continue;
        float acc = 0, dx = lx - s->x - (s->lead ? f->pad : 0);
        cui_font font = md_font_of(f, s->style);
        float size = md_size_of(f, s->style);
        for (int b = 0; b < s->len;) {
            int nb = cui__utf8_next(s->p, s->len, b);
            if (nb <= b) nb = b + 1;
            float cw = cui__text_width(ui, font, size, s->p + b, nb - b, false);
            if (dx < acc + cw * 0.5f) return s->at + b;
            acc += cw;
            b = nb;
        }
        return s->at + s->len;
    }
    return L->seg[last].at + L->seg[last].len;
}

/* Offers the block laid out in r to the probe; true when it becomes the candidate. The
 * block whose rows span the pointer wins, the nearest sideways (table cells share rows);
 * failing that, the last block above the pointer. */
static bool md_sel_offer(md_sel *S, cui_rect r) {
    bool band = S->y >= r.y && S->y < r.y + r.h;
    float dx = S->x < r.x ? r.x - S->x : S->x >= r.x + r.w ? S->x - (r.x + r.w) : 0;
    if (S->hover && cui__rect_contains(r, S->x, S->y)) S->over_text = true;
    if (!S->probe) return false;
    if (band) {
        if (S->band && dx >= S->dx) return false;
        S->band = true; S->dx = dx;
    } else if (S->band || r.y + r.h > S->y) return false;
    S->found = true;
    return true;
}

/* The part of block blk inside the selection, as [*a, *b); false when none. */
static bool md_sel_range(const md_sel *S, int blk, int *a, int *b) {
    if (md_pos_cmp(S->lo, S->hi) >= 0 || blk < S->lo.block || blk > S->hi.block) return false;
    *a = blk == S->lo.block ? S->lo.off : 0;
    *b = blk == S->hi.block ? S->hi.off : 0x7FFFFFFF;
    return *b > *a;
}

static void md_sel_put(cui_ctx *ui, md_sel *S, const char *p, int n) {
    if (n <= 0) return;
    if (S->copy_len + n + 1 > S->copy_cap) {
        int ncap = CUI_MAX(256, S->copy_cap * 2);
        while (ncap < S->copy_len + n + 1) ncap *= 2;
        char *nb = cui__frame_alloc(ui, (size_t)ncap);
        if (S->copy_len) memcpy(nb, S->copy, (size_t)S->copy_len);
        S->copy = nb; S->copy_cap = ncap;
    }
    memcpy(S->copy + S->copy_len, p, (size_t)n);
    S->copy_len += n;
    S->copy[S->copy_len] = 0;
}

/* Ctrl+C: appends the selected part of block blk (its runs, or plain code). */
static void md_sel_copy(cui_ctx *ui, md_sel *S, int blk, const md_run *runs, int nruns, const char *code, int code_len) {
    int a, b;
    char sep = S->sep;
    int gap = S->gap;
    S->sep = '\n';
    S->gap = 0;
    if (!S->copying || !md_sel_range(S, blk, &a, &b)) return;
    if (S->copy_started) {   /* also after a selection that starts at the end of a block */
        md_sel_put(ui, S, &sep, 1);
        for (; gap > 0; gap--) md_sel_put(ui, S, "\n", 1);
    }
    S->copy_started = true;
    if (code) { md_sel_put(ui, S, code + a, CUI_MIN(b, code_len) - a); return; }
    for (int k = 0; k < nruns; k++) {
        const md_run *R = &runs[k];
        int n = (R->style & MS_BREAK) ? 1 : R->len;
        int x0 = CUI_MAX(a, R->at), x1 = CUI_MIN(b, R->at + n);
        if (x1 <= x0) continue;
        if (R->style & MS_BREAK) md_sel_put(ui, S, "\n", 1);
        else md_sel_put(ui, S, R->p + (x0 - R->at), x1 - x0);
    }
}

/* ------------------------------------------------------------------------ */
/* Painting                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    const md_seg *seg; int n;
    md_font f;
    cui_color fg, link_c, code_bg;
    const float *hover, *focus;   /* per link of the block */
    bool underline_rest;          /* links keep a full underline (text on a colored surface) */
    const md_sel *sel;            /* the view's selection, final when this paints */
    int block;                    /* this block's number in it */
} md_paint;

/* Slants the glyphs drawn since v0 around the baseline: a synthesized italic. */
static void md_shear(cui_ctx *ui, int v0, float base) {
    cui_xform x = ui->dl.xform_stack[ui->dl.xform_depth - 1];
    float by = base * ui->scale * x.scale + x.oy;
    for (int v = v0; v < ui->dl.vtx_count; v++) ui->dl.vtx[v].x += MD_SHEAR * (by - ui->dl.vtx[v].y);
}

static void md_paint_inline(cui_ctx *ui, cui_node *n, void *data) {
    md_paint *p = data;
    const md_font *f = &p->f;
    float fh = cui__font_line_height(ui, f->reg, f->size), asc = cui__font_ascent(ui, f->reg, f->size);
    float th = CUI_MAX(1.0f, f->size / 13.0f);
    int sa = 0, sb = 0;
    if (p->sel && !md_sel_range(p->sel, p->block, &sa, &sb)) sb = 0;
    for (int k = 0; k < p->n; k++) {
        const md_seg *s = &p->seg[k];
        float top = n->rect.y + (float)s->line * f->lh;
        float base = top + (f->lh - fh) * 0.5f + asc;
        float x = n->rect.x + s->x;
        cui_font font = md_font_of(f, s->style);
        float size = md_size_of(f, s->style);
        cui_color col = s->link >= 0 ? p->link_c : p->fg;
        float hv = s->link >= 0 ? p->hover[s->link] : 0;
        if (s->style & MS_CODE) {
            float ch = f->mono_size * 1.5f;
            cui_draw_rect(ui, (cui_rect){ x, top + (f->lh - ch) * 0.5f, s->w, ch }, CUI_CORNERS(4), p->code_bg);
        }
        if (s->at < sb && s->at + s->len > sa) {   /* selected, under the glyphs */
            float x0 = md_seg_x(ui, f, s, sa - s->at), x1 = md_seg_x(ui, f, s, sb - s->at);
            if (x1 > x0) cui_draw_rect(ui, (cui_rect){ x + x0, top, x1 - x0, f->lh }, CUI_CORNERS(0), p->sel->color);
        }
        float tx = x + (s->lead ? f->pad : 0);
        int v0 = ui->dl.vtx_count;
        cui__draw_text_line(ui, font, size, tx, base - cui__font_ascent(ui, font, size), col, s->p, s->len, false);
        if (s->style & MS_EM) md_shear(ui, v0, base);
        if (s->style & MS_STRIKE) cui_draw_rect(ui, (cui_rect){ x, base - f->size * 0.3f, s->w, th }, CUI_CORNERS(0), col);
        if (s->link >= 0) {
            float a = p->underline_rest ? 0.8f + 0.2f * hv : 0.35f + 0.65f * hv;
            cui_draw_rect(ui, (cui_rect){ x, base + CUI_MAX(1.5f, size * 0.14f), s->w, th }, CUI_CORNERS(0), cui_alpha(col, a));
            float fo = p->focus[s->link];
            if (fo > 0.01f) cui__draw_focus_ring(ui, (cui_rect){ x - 2, top + 1, s->w + 4, f->lh - 2 }, CUI_CORNERS(4), fo);
        }
    }
}

typedef struct { int kind; int depth; cui_color c; const char *num; md_font f; } md_mark_paint;   /* kind 0 bullet, 1 number */

static void md_paint_marker(cui_ctx *ui, cui_node *n, void *data) {
    md_mark_paint *p = data;
    cui_rect r = n->rect;
    float u = p->f.size / 13.0f;
    if (p->kind == 1) {
        float w = cui__text_width(ui, p->f.reg, p->f.size, p->num, -1, true);
        float fh = cui__font_line_height(ui, p->f.reg, p->f.size);
        cui__draw_text_line(ui, p->f.reg, p->f.size, r.x + r.w - 7 * u - w, r.y + (p->f.lh - fh) * 0.5f, p->c, p->num, -1, true);
        return;
    }
    cui_vec2 c = { r.x + 6 * u, r.y + p->f.lh * 0.5f };
    int shape = p->depth % 3;
    if (shape == 0) cui_draw_circle(ui, c, 2.5f * u, p->c);
    else if (shape == 1) {
        float rr = 2.75f * u;
        cui_draw_rect_border(ui, (cui_rect){ c.x - rr, c.y - rr, rr * 2, rr * 2 }, CUI_CORNERS(rr), cui__rgba(0, 0, 0, 0), p->c, 1.1f * u);
    } else {
        float rr = 2.25f * u;
        cui_draw_rect(ui, (cui_rect){ c.x - rr, c.y - rr, rr * 2, rr * 2 }, CUI_CORNERS(0.75f), p->c);
    }
}

static void md_paint_rail(cui_ctx *ui, cui_node *n, void *data) {
    cui_color *c = data;
    float w = 3.0f * CUI_MAX(0.8f, n->pad[3] / 14.0f);
    cui_draw_rect(ui, (cui_rect){ n->rect.x, n->rect.y, w, n->rect.h }, CUI_CORNERS(w * 0.5f), *c);
}

typedef struct { float w; } md_root_paint;
static void md_paint_root(cui_ctx *ui, cui_node *n, void *data) {
    md_root_paint *p = data;
    /* laid out for another width than it got: build once more at this one */
    if (fabsf(n->rect.w - p->w) > 0.5f) ui->animating = true;
}

/* ------------------------------------------------------------------------ */
/* Blocks                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui_ctx *ui;
    cui_id id;
    const char *src;
    int len, content_end;
    bool streaming, compact, fit, custom_color, keep_ws;
    float body;                  /* unscaled body size */
    float u;                     /* spacing unit: body / 13 */
    cui_color fg, fg2, link_c, code_bg, marker_c, rule_c, rail_c;
    int link_seq, code_seq, table_seq;
    cui_id callout_keys[128];    /* content hashes of this frame's foldable callouts, to number identical ones */
    int callout_n;
    const char *base_dir;
    const char *(*resolve_image)(const char *src, void *user);
    void *resolve_user;
    cui_id leaf_id;              /* id for the next inline block's node (headings: their position) */
    cui_id root_id;
    const md_line *lines;        /* every line of the text, for line numbers */
    int nlines;
    cui_markdown_heading *heads; int nheads;
    cui_markdown_result *res;
    md_sel *sel;
} md_ctx;

typedef struct { float w; int depth; bool tight; cui_color fg; } md_box;

enum { BK_NONE, BK_PARA, BK_HEADING, BK_CODE, BK_LIST, BK_QUOTE, BK_HR, BK_TABLE };

static md_font md_make_font(md_ctx *c, float size, cui_font reg, cui_font bold, float lh_mul) {
    md_font f;
    f.reg = reg; f.bold = bold; f.mono = CUI_FONT_MONO;
    f.size = cui__fs(c->ui, size);
    f.mono_size = f.size * (12.0f / 13.0f);
    f.lh = f.size * lh_mul;
    f.pad = 4.0f * f.size / 13.0f;
    return f;
}
static md_font md_body(md_ctx *c) { return md_make_font(c, c->body, CUI_FONT_BODY, CUI_FONT_MEDIUM, c->compact ? 1.5f : 1.6f); }

static bool md_tail(md_ctx *c, md_line last) { return c->streaming && last.off + last.len >= c->content_end; }
static bool md_open_line(md_ctx *c, md_line L) { return c->streaming && L.off + L.len >= c->len; }

static void md_space(md_ctx *c, float h) {
    if (h > 0) cui__leaf(c->ui, 0, CUI_PX(1), CUI_PX(h));
}

static float md_gap_before(md_ctx *c, const md_box *b, int prev, int kind, int level) {
    if (prev == BK_NONE) return 0;
    float g = b->tight ? 4 : (c->compact ? 8 : 12);
    if (kind == BK_HEADING) g = c->compact ? 12 : (level <= 2 ? 24 : 18);
    else if (prev == BK_HEADING) g = c->compact ? 4 : 8;
    if (kind == BK_HR || prev == BK_HR) g = c->compact ? 10 : 16;
    return g * c->u;
}

static cui_id md_link_id(md_ctx *c, int seq) { return cui__hash(&seq, sizeof seq, c->id ^ 0x3D11Au); }

/* A press on a link or a fold header that travels past MD_DRAG_START is a selection from
 * where it went down, not a click: the view takes the press (md_sel_begin goes on next frame). */
static void md_sel_drag_off(md_ctx *c, cui_interact r) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    if (!r.active || !S->st || !ui->mdown[CUI_MOUSE_LEFT]) return;
    float dx = ui->mx - ui->last_click_x, dy = ui->my - ui->last_click_y;
    if (dx * dx + dy * dy > MD_DRAG_START * MD_DRAG_START) { ui->active = c->id; S->st->link_drag = true; ui->animating = true; }
}

typedef struct { const md_sel *sel; int block; } md_text_paint;

/* x of byte k of a one line text node w wide, from its left edge, as cui__paint_text places it. */
static float md_text_node_x(cui_ctx *ui, const cui_node *n, float w, int k) {
    bool tab = (n->flags & CUI_NF_TABULAR) != 0;
    float tw = cui__text_width(ui, n->font, n->font_size, n->text, n->text_len, tab), x = 0;
    if (tw < w && n->text_align == CUI_TEXT_CENTER) x = (w - tw) * 0.5f;
    else if (tw < w && (n->text_align == CUI_TEXT_RIGHT || n->text_align == CUI_TEXT_END)) x = w - tw;
    return x + (k > 0 ? cui__text_width(ui, n->font, n->font_size, n->text, CUI_MIN(k, n->text_len), tab) : 0);
}

static void md_paint_text_sel(cui_ctx *ui, cui_node *n, void *data) {
    const md_text_paint *p = data;
    int a, b;
    if (!md_sel_range(p->sel, p->block, &a, &b)) return;
    float x0 = md_text_node_x(ui, n, n->rect.w, CUI_MIN(a, n->text_len)), x1 = md_text_node_x(ui, n, n->rect.w, CUI_MIN(b, n->text_len));
    if (x1 > x0) cui_draw_rect(ui, (cui_rect){ n->rect.x + x0, n->rect.y, x1 - x0, n->rect.h }, CUI_CORNERS(0), p->sel->color);
}

/* A one line text node another component built (a table's header cell) as a selectable block;
 * in holds the same text as runs, byte for byte. The node gets an id for its rect and paints
 * the selection under its text. */
static void md_sel_text_node(md_ctx *c, int node, const md_inline *in) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    int blk = S->seq++;
    cui_id lid = cui__hash(&blk, sizeof blk, c->id ^ 0x5E1B1u);
    cui_node *n = &ui->nodes[node];
    md_text_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->sel = S; p->block = blk;
    n->id = lid;
    n->paint = md_paint_text_sel;
    n->paint_data = p;
    if ((S->hover || S->probe) && cui__has_prev_rect(ui, lid)) {
        cui_rect r = cui__prev_rect(ui, lid);
        if (md_sel_offer(S, r)) {
            int best = 0;
            float bd = 1e9f;
            for (int k = 0; k <= n->text_len; k = k < n->text_len ? CUI_MAX(k + 1, cui__utf8_next(n->text, n->text_len, k)) : k + 1) {
                float d = fabsf(r.x + md_text_node_x(ui, n, r.w, k) - S->x);
                if (d < bd) { bd = d; best = k; }
            }
            S->at.block = blk; S->at.off = best;
            S->runs = in->runs; S->nruns = in->nruns; S->code = NULL; S->diag = false;
        }
    }
    md_sel_copy(ui, S, blk, in->runs, in->nruns, NULL, 0);
}

/* A diagram is one block whose text is its labels, a line each (cui__mermaid_sel). The probe
 * finds the label under the pointer; a double click on a label selects it, also in a diagram
 * that takes the press itself to pan it. */
static void md_sel_diagram(md_ctx *c, md_diag_sel *ds, cui_id panel) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    md_sel_state *st = S->st;
    const cui__mermaid_sel *m = &ds->ms;
    ds->block = S->seq++;
    ds->next = S->diags; S->diags = ds;
    if (cui__has_prev_rect(ui, panel)) {
        cui_rect r = cui__prev_rect(ui, panel);
        int lab = -1;
        for (int k = 0; k < m->count && lab < 0; k++)
            if (cui__rect_contains(m->rect[k], S->x, S->y) && cui__rect_contains(r, S->x, S->y)) lab = k;
        bool over = S->over_text;
        if ((S->hover || S->probe) && md_sel_offer(S, r)) {
            S->at.block = ds->block;
            S->at.off = lab >= 0 ? m->at[lab] : S->y < r.y + r.h * 0.5f ? 0 : m->len;
            S->code = m->text; S->code_len = m->len; S->runs = NULL;
            S->diag = true;
            S->lab_a = lab >= 0 ? m->at[lab] : -1; S->lab_b = lab >= 0 ? m->at[lab + 1] - 1 : -1;
        }
        if (lab < 0) S->over_text = over;   /* the I-beam only over a label */
        if (lab >= 0 && !S->probe && st->focused && ui->mpressed[CUI_MOUSE_LEFT] && ui->click_count == 2 && ui->hot == panel) {
            st->anchor = st->unit0 = (md_pos){ ds->block, m->at[lab] };
            st->head = st->unit1 = (md_pos){ ds->block, m->at[lab + 1] - 1 };
            st->anchored = true; st->dragging = false;
        }
    }
    md_sel_copy(ui, S, ds->block, NULL, 0, m->text, m->len);
}

/* Builds a laid out inline block: one painted node, a hit box per link piece. */
static float md_inline_emit(md_ctx *c, const md_inline *in, const md_font *f, cui_color fg, int align, float W) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    md_lay L;
    md_layout(ui, in, f, CUI_MAX(W, 24.0f), align, &L);
    int blk = S->seq++;
    int base = c->link_seq;
    c->link_seq += in->nlinks;
    float *hover = cui__frame_alloc(ui, sizeof(float) * (size_t)(in->nlinks * 2 + 1)), *focus = hover + in->nlinks;
    cui_id *ids = cui__frame_alloc(ui, sizeof(cui_id) * (size_t)(in->nlinks + 1));
    for (int k = 0; k < in->nlinks; k++) {
        const md_link *lk = &in->links[k];
        bool shown = false;
        for (int q = 0; q < L.n && !shown; q++) shown = L.seg[q].link == k;
        ids[k] = 0;
        if (!shown || lk->pending) continue;   /* nothing to click yet */
        cui_id id = md_link_id(c, base + k);
        ids[k] = id;
        cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE);
        float *st = cui_state(ui, id, sizeof(float) * 2);
        cui__tween(ui, &st[0], r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
        cui__tween(ui, &st[1], r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
        hover[k] = st[0]; focus[k] = st[1];
        const char *url = NULL;
        if (r.hovered || r.clicked) url = lk->wiki ? cui__frame_strdup(ui, lk->url, lk->len) : lk->www ? cui__frame_printf(ui, "http://%.*s", lk->len, lk->url) : cui__frame_strdup(ui, lk->url, lk->len);
        if (r.hovered) {
            cui__set_cursor(ui, CUI_CURSOR_HAND);
            c->res->hovered_link = url; c->res->hovered_is_wiki = lk->wiki;
            c->res->hovered_rect = cui__prev_rect(ui, id);
        }
        if (r.clicked) { c->res->link = url; c->res->link_is_wiki = lk->wiki; }
        md_sel_drag_off(c, r);
        if (!lk->autolink) { ui->last_id = id; cui_tooltip(ui, r.hovered && url ? url : ""); }
    }
    cui_id lid = c->leaf_id;
    if (!lid && S->st) lid = cui__hash(&blk, sizeof blk, c->id ^ 0x5E1B1u);   /* its rect, to find text under the pointer */
    int leaf = cui__leaf(ui, lid, CUI_FILL, CUI_PX((float)L.lines * f->lh));
    c->leaf_id = 0;
    md_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->seg = L.seg; p->n = L.n; p->f = *f;
    p->fg = fg; p->link_c = c->custom_color ? fg : c->link_c; p->code_bg = c->code_bg;
    p->hover = hover; p->focus = focus;
    p->underline_rest = c->custom_color;
    p->sel = S; p->block = blk;
    if (S->st && (S->hover || S->probe) && cui__has_prev_rect(ui, lid)) {
        cui_rect r = cui__prev_rect(ui, lid);
        if (md_sel_offer(S, r)) {
            S->at.block = blk; S->at.off = md_hit_inline(ui, &L, f, S->x - r.x, S->y - r.y);
            S->runs = in->runs; S->nruns = in->nruns; S->code = NULL; S->diag = false;
        }
    }
    md_sel_copy(ui, S, blk, in->runs, in->nruns, NULL, 0);
    ui->nodes[leaf].paint = md_paint_inline;
    ui->nodes[leaf].paint_data = p;
    if (in->nlinks > 0) {
        cui__open(ui, leaf);
        for (int q = 0; q < L.n; q++) {
            const md_seg *s = &L.seg[q];
            if (s->link < 0 || !ids[s->link]) continue;
            float x0 = s->x, x1 = s->x + s->w;
            while (q + 1 < L.n && L.seg[q + 1].link == s->link && L.seg[q + 1].line == s->line) { q++; x1 = L.seg[q].x + L.seg[q].w; }
            int a = cui__absolute(ui, ids[s->link], x0, (float)s->line * f->lh, CUI_MAX(1.0f, x1 - x0), f->lh);
            cui__mark_interactive(ui, a, CUI_IF_FOCUSABLE);
        }
        cui__close(ui);
    }
    return c->fit ? md_runs_width(ui, in, f) : 0;
}

static float md_inline_block(md_ctx *c, const char *s, int n, const md_font *f, cui_color fg, bool tail, float W) {
    md_inline in = md_parse_inline(c->ui, s, n, tail);
    return md_inline_emit(c, &in, f, fg, CUI_TEXT_LEFT, W);
}

/* Joins paragraph lines: soft breaks become spaces, hard breaks '\n'. */
static const char *md_join(md_ctx *c, const md_line *L, int n, int *out_len) {
    if (n == 1) {
        md_line T = md_trim(L[0]);
        *out_len = T.len;
        return T.s;
    }
    int total = n;
    for (int k = 0; k < n; k++) total += L[k].len;
    char *buf = cui__frame_alloc(c->ui, (size_t)total + 1);
    int o = 0;
    for (int k = 0; k < n; k++) {
        int b;
        md_indent(L[k], &b);
        md_line T = md_skip(L[k], b);
        int e = T.len, sp = 0;
        while (e > 0 && md_sp(T.s[e - 1])) { e--; sp++; }
        bool last = k == n - 1, hard = false;
        if (!last) {
            if (sp >= 2) hard = true;
            else if (sp == 0 && e > 0 && T.s[e - 1] == '\\') { hard = true; e--; }
        }
        memcpy(buf + o, T.s, (size_t)e);
        o += e;
        if (!last) buf[o++] = hard ? '\n' : ' ';
    }
    buf[o] = 0;
    *out_len = o;
    return buf;
}

static char md_lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static bool md_alnum(char c) { return (c >= '0' && c <= '9') || (md_lc(c) >= 'a' && md_lc(c) <= 'z'); }

/* ---- images: ![alt](src), ![alt|300](src), ![[file.png|300]], [![alt](src)](url) ---- */

typedef struct {
    const char *alt, *src, *link;
    int alt_len, src_len, link_len;   /* link_len -1: not wrapped in a link */
    float width;                      /* from the |300 size syntax, 0 for the natural size */
} md_img;

static bool md_image_ext(const char *s, int n) {
    static const char *const ext[] = { "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "tga" };
    int dot = n;
    while (dot > 0 && s[dot - 1] != '.' && s[dot - 1] != '/' && s[dot - 1] != '\\') dot--;
    if (dot <= 0 || s[dot - 1] != '.') return false;
    for (int k = 0; k < CUI_COUNTOF(ext); k++) {
        int el = (int)strlen(ext[k]);
        if (n - dot != el) continue;
        bool same = true;
        for (int q = 0; q < el && same; q++) same = md_lc(s[dot + q]) == ext[k][q];
        if (same) return true;
    }
    return false;
}

/* "300": the width an image is shown at */
static bool md_img_width(const char *s, int n, float *w) {
    if (n <= 0 || n > 6) return false;
    float v = 0;
    for (int k = 0; k < n; k++) {
        if (s[k] < '0' || s[k] > '9') return false;
        v = v * 10 + (float)(s[k] - '0');
    }
    *w = v;
    return v > 0;
}

/* The image starting at s[i]: the index after it, or 0. */
static int md_img_at(const char *s, int n, int i, md_img *m) {
    memset(m, 0, sizeof *m);
    m->link_len = -1;
    bool wrap = false;
    if (s[i] == '[') {
        if (!(i + 2 < n && s[i + 1] == '!' && s[i + 2] == '[')) return 0;
        wrap = true; i++;
    }
    if (s[i] != '!' || i + 2 >= n || s[i + 1] != '[') return 0;
    if (s[i + 2] == '[') {   /* ![[name|alt|300]] */
        if (wrap) return 0;
        int os = i + 3, q = os;
        while (q + 1 < n && s[q] != '\n' && !(s[q] == ']' && s[q + 1] == ']')) q++;
        if (q + 1 >= n || s[q] != ']' || s[q + 1] != ']') return 0;
        int seg = os;
        for (int k = os; k <= q; k++) {
            if (k < q && s[k] != '|') continue;
            int a = seg, e = k;
            while (a < e && md_sp(s[a])) a++;
            while (e > a && md_sp(s[e - 1])) e--;
            if (seg == os) { m->src = s + a; m->src_len = e - a; }
            else if (!md_img_width(s + a, e - a, &m->width) && !m->alt) { m->alt = s + a; m->alt_len = e - a; }
            seg = k + 1;
        }
        return md_image_ext(m->src, m->src_len) ? q + 2 : 0;
    }
    int a = i + 2, q = a, depth = 0;
    while (q < n) {
        if (s[q] == '\\' && q + 1 < n) { q += 2; continue; }
        if (s[q] == '[') depth++;
        else if (s[q] == ']') { if (depth == 0) break; depth--; }
        q++;
    }
    if (q >= n) return 0;
    const char *url;
    int ulen, end;
    if (md_link_dest(s, n, q + 1, &url, &ulen, &end) != 1 || ulen <= 0) return 0;
    m->alt = s + a; m->alt_len = q - a;
    for (int k = m->alt_len - 1; k >= 0; k--) {   /* alt|300 */
        if (m->alt[k] != '|') continue;
        if (md_img_width(m->alt + k + 1, m->alt_len - k - 1, &m->width)) m->alt_len = k;
        break;
    }
    m->src = url; m->src_len = ulen;
    if (wrap) {
        const char *lu;
        int ll, le;
        if (end >= n || s[end] != ']' || md_link_dest(s, n, end + 1, &lu, &ll, &le) != 1) return 0;
        m->link = lu; m->link_len = ll;
        end = le;
    }
    return end;
}

static bool md_is_abs_path(const char *p) {
    return p[0] == '/' || p[0] == '\\' || (md_alnum(p[0]) && !(p[0] >= '0' && p[0] <= '9') && p[1] == ':');
}
/* scheme:// (http, https, file...) or data: : never fetched */
static bool md_is_remote(const char *p, int n) {
    for (int k = 0; k < n; k++) {
        if (p[k] == ':') return k >= 2 && ((k + 2 < n && p[k + 1] == '/' && p[k + 2] == '/') || (k == 4 && !strncmp(p, "data", 4)));
        if (!md_alnum(p[k]) && p[k] != '+' && p[k] != '-' && p[k] != '.') return false;
    }
    return false;
}
static int md_hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : md_lc(c) >= 'a' && md_lc(c) <= 'f' ? md_lc(c) - 'a' + 10 : -1;
}

/* The file an image source names, or NULL (remote images are never fetched here; the resolver may map them to a local copy). */
static const char *md_image_path(md_ctx *c, const md_img *m) {
    if (md_is_remote(m->src, m->src_len)) {   /* only the app can fetch it: its resolver may return a downloaded copy */
        if (!c->resolve_image) return NULL;
        const char *src = cui__frame_strdup(c->ui, m->src, m->src_len);
        const char *r = c->resolve_image(src, c->resolve_user);
        return r && r[0] ? cui__frame_strdup(c->ui, r, (int)strlen(r)) : NULL;
    }
    char *src = cui__frame_alloc(c->ui, (size_t)m->src_len + 1);
    int o = 0;
    for (int k = 0; k < m->src_len; k++) {   /* %20 and friends */
        if (m->src[k] == '%' && k + 2 < m->src_len && md_hexval(m->src[k + 1]) >= 0 && md_hexval(m->src[k + 2]) >= 0) {
            src[o++] = (char)(md_hexval(m->src[k + 1]) * 16 + md_hexval(m->src[k + 2]));
            k += 2;
        } else src[o++] = m->src[k];
    }
    src[o] = 0;
    if (c->resolve_image) {
        const char *r = c->resolve_image(src, c->resolve_user);
        if (r && r[0]) return cui__frame_strdup(c->ui, r, (int)strlen(r));
    }
    if (c->base_dir && c->base_dir[0] && !md_is_abs_path(src)) {
        size_t bl = strlen(c->base_dir);
        bool sep = c->base_dir[bl - 1] == '/' || c->base_dir[bl - 1] == '\\';
        return cui__frame_printf(c->ui, sep ? "%s%s" : "%s/%s", c->base_dir, src);
    }
    return src;
}

/* One decoded picture per file, kept in CUI state: decoded once and freed with
 * the state when nothing draws it for a few seconds. A file that failed is tried
 * again about every two seconds, in case it appears later. */
typedef struct { bool init; cui_texture tex; int w, h; uint64_t retry; } md_img_state;
static void md_img_free(cui_ctx *ui, void *data) {
    md_img_state *t = data;
    cui_image_destroy(ui, t->tex);
    t->tex = 0;
}
static md_img_state *md_img_tex(cui_ctx *ui, const char *path) {
    cui_id id = cui__hash(path, strlen(path), 0x1A6E5u);
    md_img_state *t = cui_state(ui, id, sizeof *t);
    cui__state_set_dtor(ui, id, md_img_free);
    if (!t->tex && (!t->init || ui->frame >= t->retry)) {
        t->init = true;
        t->retry = ui->frame + 120;
        t->tex = cui_image_load(ui, path, &t->w, &t->h);
        if (t->tex && (t->w <= 0 || t->h <= 0)) { cui_image_destroy(ui, t->tex); t->tex = 0; }
    }
    return t;
}

typedef struct { cui_texture tex; } md_img_paint;
static void md_paint_img(cui_ctx *ui, cui_node *n, void *data) {
    md_img_paint *p = data;
    cui_rect uv = { 0, 0, 1, 1 };
    cui_draw_image(ui, p->tex, n->rect, uv, n->radius, cui__rgba(255, 255, 255, 255));
}

/* The picture as a block of its own, or a quiet line with an icon and the alt text. */
static float md_image_block(md_ctx *c, const md_img *m, float W) {
    cui_ctx *ui = c->ui;
    const char *path = md_image_path(c, m);
    md_img_state *t = path ? md_img_tex(ui, path) : NULL;
    bool ok = t && t->tex;
    cui_id id = 0;
    float *st = NULL;
    if (m->link_len > 0) {
        id = md_link_id(c, c->link_seq++);
        cui_interact r = cui__interact(ui, id, CUI_IF_FOCUSABLE);
        st = cui_state(ui, id, sizeof(float) * 2);
        cui__tween(ui, &st[0], r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
        cui__tween(ui, &st[1], r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
        const char *url = r.hovered || r.clicked ? cui__frame_strdup(ui, m->link, m->link_len) : NULL;
        if (r.hovered) {
            cui__set_cursor(ui, CUI_CURSOR_HAND);
            c->res->hovered_link = url; c->res->hovered_is_wiki = false;
            c->res->hovered_rect = cui__prev_rect(ui, id);
        }
        if (r.clicked) { c->res->link = url; c->res->link_is_wiki = false; }
    }
    int node;
    float w = 0;
    if (ok) {
        w = m->width > 0 ? m->width : (float)t->w;
        w = CUI_MAX(1.0f, CUI_MIN(w, W));
        node = cui__leaf(ui, id, CUI_PX(w), CUI_PX(w * (float)t->h / (float)t->w));
        cui_node *n = &ui->nodes[node];
        n->radius = CUI_CORNERS(ui->theme.radius_control);
        n->self_align = CUI__ALIGN_NOSTRETCH;
        md_img_paint *p = cui__frame_alloc(ui, sizeof *p);
        p->tex = t->tex;
        n->paint = md_paint_img;
        n->paint_data = p;
    } else {
        md_font f = md_body(c);
        const char *label = m->alt;
        int ll = m->alt_len;
        if (ll <= 0) {   /* the file name */
            label = m->src; ll = m->src_len;
            for (int k = 0; k < m->src_len; k++) if (m->src[k] == '/' || m->src[k] == '\\') { label = m->src + k + 1; ll = m->src_len - k - 1; }
        }
        cui_color mute = c->custom_color ? c->fg2 : CUI_C(ui, TEXT_MUTED);
        node = cui__node_new(ui, id);
        cui_node *rn = &ui->nodes[node];
        rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->gap = 6 * c->u; rn->w = CUI_FILL;
        cui__open(ui, node);
        cui__icon_node(ui, CUI_ICON_IMAGE, f.size * 1.1f, mute);
        int tn = cui__text_node(ui, label, ll, CUI_FONT_BODY, f.size * 0.95f, mute, true);
        ui->nodes[tn].w = CUI_FILL;
        cui__close(ui);
    }
    if (id) {
        cui__mark_interactive(ui, node, CUI_IF_FOCUSABLE);
        if (st[1] > 0.01f) cui__focus_ring(ui, node, st[1]);
    }
    return c->fit ? w : 0;
}

/* Text around images stays inline text; each image is a block of its own. */
static float md_paragraph(md_ctx *c, const md_box *b, const md_line *L, int n) {
    int len;
    const char *s = md_join(c, L, n, &len);
    md_font f = md_body(c);
    bool tail = md_tail(c, L[n - 1]);
    if (!memchr(s, '!', (size_t)len)) return md_inline_block(c, s, len, &f, b->fg, tail, b->w);
    float best = 0;
    int ts = 0, i = 0;
    bool first = true;
    for (;;) {
        md_img m = { 0 };
        int e = 0;
        while (i < len && !(e = (s[i] == '!' || s[i] == '[') ? md_img_at(s, len, i, &m) : 0)) {
            if (s[i] == '\\') i++;
            else if (s[i] == '`') {   /* a code span hides what looks like an image */
                int k = 0;
                while (i + k < len && s[i + k] == '`') k++;
                int j = i + k, close = -1;
                while (j < len && close < 0) {
                    if (s[j] != '`') { j++; continue; }
                    int q = j;
                    while (q < len && s[q] == '`') q++;
                    if (q - j == k) close = q;
                    j = q;
                }
                i = close >= 0 ? close - 1 : i + k - 1;
            }
            i++;
        }
        int a = ts, z = i;   /* the text before the image, or what is left */
        while (a < z && (md_sp(s[a]) || s[a] == '\n')) a++;
        while (z > a && (md_sp(s[z - 1]) || s[z - 1] == '\n')) z--;
        if (z > a) {
            if (!first) md_space(c, 6.0f * c->u);
            float tw = md_inline_block(c, s + a, z - a, &f, b->fg, tail && e == 0, b->w);
            best = CUI_MAX(best, tw);
            first = false;
        }
        if (!e) break;
        if (!first) md_space(c, 6.0f * c->u);
        float iw = md_image_block(c, &m, b->w);
        best = CUI_MAX(best, iw);
        first = false;
        i = ts = e;
    }
    return best;
}

/* Records a heading for the result: its line and its top in the latest layout. */
static void md_note_heading(md_ctx *c, int level, md_line src) {
    if (!c->heads || c->nheads >= c->nlines) return;
    int lo = 0, hi = c->nlines - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if (c->lines[mid].off <= src.off) lo = mid; else hi = mid - 1; }
    cui_markdown_heading *h = &c->heads[c->nheads++];
    h->level = level; h->line = lo + 1; h->text = src.s; h->text_len = src.len;
    c->leaf_id = cui__hash(&c->nheads, sizeof c->nheads, c->id ^ 0x4EAD1u);
    cui_rect hr = cui__prev_rect(c->ui, c->leaf_id), rr = cui__prev_rect(c->ui, c->root_id);
    h->y = cui__has_prev_rect(c->ui, c->leaf_id) ? hr.y - rr.y : 0;
}

static float md_heading(md_ctx *c, const md_box *b, int level, md_line src, const char *s, int len, bool tail) {
    cui_theme *t = &c->ui->theme;
    md_note_heading(c, level, src);
    float scale = c->body / t->text_body;
    float size;
    if (c->compact) size = level == 1 ? t->heading[3] : level == 2 ? t->text_lg : t->text_body;
    else size = level == 1 ? t->heading[1] : level == 2 ? t->heading[2] : level == 3 ? t->heading[3] : level == 4 ? t->text_lg : t->text_body;
    size *= scale;
    cui_font font = size >= 26 ? CUI_FONT_DISPLAY : CUI_FONT_MEDIUM;
    md_font f = md_make_font(c, size, font, font, size >= 26 ? 1.2f : 1.35f);
    return md_inline_block(c, s, len, &f, level >= 6 ? c->fg2 : b->fg, tail, b->w);
}

static void md_lower(char *d, const char *s, int n) {
    for (int k = 0; k < n; k++) d[k] = (s[k] >= 'A' && s[k] <= 'Z') ? (char)(s[k] - 'A' + 'a') : s[k];
    d[n] = 0;
}

static float md_code(md_ctx *c, const md_line *L, int n, int strip, md_line lang) {
    cui_ctx *ui = c->ui;
    int total = n + 1;
    for (int k = 0; k < n; k++) total += L[k].len;
    char *buf = cui__frame_alloc(ui, (size_t)total);
    int o = 0;
    float widest = 0;
    for (int k = 0; k < n; k++) {
        md_line T = md_strip_cols(L[k], strip);
        memcpy(buf + o, T.s, (size_t)T.len);
        o += T.len;
        if (k < n - 1) buf[o++] = '\n';
        if (c->fit) widest = CUI_MAX(widest, cui__text_width(ui, CUI_FONT_MONO, cui__fs(ui, c->body), T.s, T.len, true));
    }
    buf[o] = 0;
    char *lg = cui__frame_alloc(ui, 24);
    if (lang.len > 0 && lang.len < 24) md_lower(lg, lang.s, lang.len);
    else memcpy(lg, "text", 5);
    cui_code_opts co;
    memset(&co, 0, sizeof co);
    co.lang = lg;
    co.size = c->body;
    const char *cid = cui__frame_printf(ui, "code%d", c->code_seq++);
    float mw;
    md_sel *S = c->sel;
    if (!strcmp(lg, "mermaid")) {
        md_diag_sel *ds = S->st ? cui__frame_alloc(ui, sizeof *ds) : NULL;
        if (ds) ds->ms.color = S->color;
        if (cui__mermaid_block(ui, cid, buf, o, c->body, &mw, ds ? &ds->ms : NULL)) {   /* a diagram */
            if (ds) md_sel_diagram(c, ds, ui->last_id);
            return c->fit ? mw : 0;
        }
    }
    if (!S->st) {
        cui_code_block_ex(ui, cid, buf, &co);
        return c->fit ? widest + 42 : 0;
    }
    md_code_sel *cs = cui__frame_alloc(ui, sizeof *cs);
    cs->block = S->seq++;
    cs->cs.color = S->color;
    cs->next = S->codes; S->codes = cs;
    cui__code_block_sel(ui, cid, buf, &co, &cs->cs);
    if ((S->hover || S->probe) && cui__has_prev_rect(ui, cs->cs.canvas)) {
        if (md_sel_offer(S, cui__prev_rect(ui, cs->cs.canvas))) {
            S->at.block = cs->block; S->at.off = cui__code_sel_hit(ui, &cs->cs, buf, o, S->x, S->y);
            S->code = buf; S->code_len = o; S->runs = NULL; S->diag = false;
        }
    }
    md_sel_copy(ui, S, cs->block, NULL, 0, buf, o);
    return c->fit ? widest + 42 : 0;
}

/* ---- block starts ---- */

typedef struct { bool ordered; char ch; int num, w; } md_marker;

static bool md_list_marker(md_line T, md_marker *m) {
    if (T.len < 1) return false;
    char c0 = T.s[0];
    if (c0 == '-' || c0 == '*' || c0 == '+') {
        if (T.len > 1 && !md_sp(T.s[1])) return false;
        m->ordered = false; m->ch = c0; m->num = 0; m->w = 1;
        return true;
    }
    int i = 0, num = 0;
    while (i < T.len && i < 9 && T.s[i] >= '0' && T.s[i] <= '9') { num = num * 10 + (T.s[i] - '0'); i++; }
    if (i == 0 || i >= T.len || (T.s[i] != '.' && T.s[i] != ')')) return false;
    if (i + 1 < T.len && !md_sp(T.s[i + 1])) return false;
    m->ordered = true; m->ch = T.s[i]; m->num = num; m->w = i + 1;
    return true;
}

static bool md_hrule(md_line T) {
    char ch = 0;
    int k = 0;
    for (int i = 0; i < T.len; i++) {
        char x = T.s[i];
        if (md_sp(x)) continue;
        if (!ch) { if (x != '-' && x != '*' && x != '_') return false; ch = x; }
        else if (x != ch) return false;
        k++;
    }
    return k >= 3;
}

/* setext underline: 1 for ===, 2 for ---, else 0 */
static int md_setext(md_line T) {
    T = md_trim(T);
    if (T.len < 1 || (T.s[0] != '=' && T.s[0] != '-')) return 0;
    for (int i = 0; i < T.len; i++) if (T.s[i] != T.s[0]) return 0;
    return T.s[0] == '=' ? 1 : 2;
}

static bool md_atx(md_line T, int *level, md_line *content) {
    int k = 0;
    while (k < T.len && T.s[k] == '#') k++;
    if (k < 1 || k > 6 || (k < T.len && !md_sp(T.s[k]))) return false;
    md_line C = md_trim(md_skip(T, k));
    int h = C.len;
    while (h > 0 && C.s[h - 1] == '#') h--;
    if (h < C.len && (h == 0 || md_sp(C.s[h - 1]))) { C.len = h; while (C.len > 0 && md_sp(C.s[C.len - 1])) C.len--; }
    *level = k; *content = C;
    return true;
}

static bool md_fence(md_line T, char *ch, int *flen, md_line *info) {
    if (T.len < 3 || (T.s[0] != '`' && T.s[0] != '~')) return false;
    int k = 0;
    while (k < T.len && T.s[k] == T.s[0]) k++;
    if (k < 3) return false;
    md_line I = md_trim(md_skip(T, k));
    if (T.s[0] == '`' && md_has(I, '`')) return false;
    int w = 0;
    while (w < I.len && !md_sp(I.s[w])) w++;
    I.len = w;
    *ch = T.s[0]; *flen = k; *info = I;
    return true;
}

static bool md_fence_close(md_line L, char ch, int flen) {
    int b;
    if (md_indent(L, &b) >= 4) return false;
    md_line T = md_skip(L, b);
    int k = 0;
    while (k < T.len && T.s[k] == ch) k++;
    return k >= flen && md_blank(md_skip(T, k));
}

/* Does line L start a block that ends a paragraph? */
static bool md_interrupts(md_line L) {
    int b;
    if (md_blank(L) || md_indent(L, &b) >= 4) return false;
    md_line T = md_skip(L, b), I;
    char ch;
    int k;
    if (md_fence(T, &ch, &k, &I) || md_atx(T, &k, &I) || T.s[0] == '>' || md_hrule(T)) return true;
    md_marker m;
    if (md_list_marker(T, &m)) return !md_blank(md_skip(T, m.w)) && (!m.ordered || m.num == 1);
    return false;
}

static int md_cells(md_line T, md_line *cells, int max) {
    T = md_trim(T);
    int i = 0, n = T.len;
    if (i < n && T.s[i] == '|') i++;
    if (n > i && T.s[n - 1] == '|' && !(n - 2 >= i && T.s[n - 2] == '\\')) n--;
    int count = 0, st = i;
    for (int k = i; k <= n; k++) {
        if (k == n || (T.s[k] == '|' && !(k > i && T.s[k - 1] == '\\'))) {
            if (count < max) {
                md_line C = { T.s + st, k - st, T.off + st };
                cells[count] = md_trim(C);
            }
            count++;
            st = k + 1;
        }
    }
    return count;
}

/* Alignment row: column count, or 0. partial accepts an unfinished last cell. */
static int md_delim_row(md_line T, uint8_t *align, int max, bool partial) {
    md_line cells[MD_MAX_COLS];
    if (!md_has(T, '|') || (!md_has(T, '-') && !partial)) return 0;
    int n = md_cells(T, cells, MD_MAX_COLS);
    for (int k = 0; k < n && k < MD_MAX_COLS; k++) {
        md_line C = cells[k];
        bool left = C.len > 0 && C.s[0] == ':', right = C.len > 1 && C.s[C.len - 1] == ':';
        int dashes = 0;
        for (int q = left ? 1 : 0; q < C.len - (right ? 1 : 0); q++) { if (C.s[q] != '-') return 0; dashes++; }
        if (!dashes && !(partial && k == n - 1)) return 0;
        if (k < max) align[k] = (uint8_t)(left && right ? CUI_TEXT_CENTER : right ? CUI_TEXT_RIGHT : CUI_TEXT_LEFT);
    }
    return n;
}

/* A table starts at line i: header cells and their alignments. */
static int md_table_at(md_ctx *c, const md_line *L, int n, int i, uint8_t *align) {
    md_line H = L[i];
    if (md_blank(H) || md_indent(H, NULL) >= 4 || !md_has(H, '|')) return 0;
    md_line cells[MD_MAX_COLS];
    int nh = md_cells(H, cells, MD_MAX_COLS);
    for (int k = 0; k < MD_MAX_COLS; k++) align[k] = CUI_TEXT_LEFT;
    if (i + 1 < n && md_indent(L[i + 1], NULL) < 4) {
        bool partial = md_open_line(c, L[i + 1]);
        int nd = md_delim_row(L[i + 1], align, MD_MAX_COLS, partial);
        if (nd == nh || (partial && nd > 0 && nd <= nh)) return nh;
    }
    /* still arriving: a header row that waits for its alignment row */
    if (md_open_line(c, H) && md_trim(H).s[0] == '|') return nh;
    return 0;
}

static float md_blocks(md_ctx *c, const md_line *L, int n, const md_box *b);

static int md_list(md_ctx *c, const md_box *b, const md_line *L, int n, int i, float *nat) {
    cui_ctx *ui = c->ui;
    typedef struct { int start, end, content_col; md_line first; bool task, checked; int task_off; } md_item;
    md_item *items = cui__frame_alloc(ui, sizeof(md_item) * (size_t)(n - i + 1));
    int count = 0;
    bool loose = false;
    md_marker m0;
    memset(&m0, 0, sizeof m0);
    int j = i;
    while (j < n) {
        int bytes, ind = md_indent(L[j], &bytes);
        md_line T = md_skip(L[j], bytes);
        md_marker m;
        if (ind >= 4 || !md_list_marker(T, &m)) break;
        if (count > 0 && (m.ordered != m0.ordered || m.ch != m0.ch)) break;
        if (count == 0) m0 = m;
        md_item *it = &items[count++];
        md_line R = md_skip(T, m.w);
        int sb, sp = md_indent(R, &sb);
        if (md_blank(R)) { it->content_col = ind + m.w + 1; R = md_skip(R, R.len); }
        else if (sp > 4) { it->content_col = ind + m.w + 1; R = md_skip(R, 1); }
        else { it->content_col = ind + m.w + sp; R = md_skip(R, sb); }
        /* task item: [ ], [x] or [X] then a space or the end */
        it->task = false;
        if (R.len >= 3 && R.s[0] == '[' && R.s[2] == ']' && (R.s[1] == ' ' || R.s[1] == 'x' || R.s[1] == 'X') && (R.len == 3 || md_sp(R.s[3]))) {
            it->task = true; it->checked = R.s[1] != ' '; it->task_off = R.off + 1;
            R = md_skip(R, R.len > 3 ? 4 : 3);
        }
        it->first = R;
        it->start = j;
        int last = j;
        j++;
        while (j < n) {
            if (md_blank(L[j])) { j++; continue; }
            int ib, indj = md_indent(L[j], &ib);
            md_line Tj = md_skip(L[j], ib);
            md_marker mj;
            bool marker = indj < 4 + ind && md_list_marker(Tj, &mj);
            if (indj >= it->content_col) {}
            else if (marker && indj > ind) {}                 /* nested a little short of the content column */
            else if (j == last + 1 && !md_interrupts(L[j]) && !marker) {}   /* lazy continuation */
            else break;
            last = j;
            j++;
        }
        it->end = last + 1;
        for (int q = it->start + 1; q < it->end; q++) if (md_blank(L[q])) loose = true;
        /* the next item follows, maybe after blank lines */
        int k = it->end;
        while (k < n && md_blank(L[k])) k++;
        if (k >= n) { j = it->end; break; }
        int kb, indk = md_indent(L[k], &kb);
        md_marker mk;
        if (indk >= 4 || !md_list_marker(md_skip(L[k], kb), &mk) || mk.ordered != m0.ordered || mk.ch != m0.ch) { j = it->end; break; }
        if (k > it->end) loose = true;
        j = k;
    }
    if (count == 0) return i + 1;
    md_font f = md_body(c);
    float u = c->u;
    bool any_task = false;
    for (int k = 0; k < count; k++) any_task |= items[k].task;
    float colw = 18 * u;
    const char *last_num = NULL;
    if (m0.ordered) {
        last_num = cui__frame_printf(ui, "%d%c", m0.num + count - 1, m0.ch);
        colw = CUI_MAX(colw, cui__text_width(ui, f.reg, f.size, last_num, -1, true) + 9 * u);
    }
    if (any_task) colw = CUI_MAX(colw, 26.0f * CUI_MAX(1.0f, u));
    int list = cui__node_new(ui, 0);
    ui->nodes[list].w = CUI_FILL;
    ui->nodes[list].gap = (loose ? 10 : (c->compact ? 3 : 4)) * u;
    cui__open(ui, list);
    float best = 0;
    for (int k = 0; k < count; k++) {
        md_item *it = &items[k];
        int row = cui__node_new(ui, 0);
        cui_node *rn = &ui->nodes[row];
        rn->dir = CUI_ROW; rn->align = CUI_ALIGN_START; rn->w = CUI_FILL;
        cui__open(ui, row);
        int mk = cui__leaf(ui, 0, CUI_PX(colw), CUI_PX(f.lh));
        if (it->task) {
            cui_node *mn = &ui->nodes[mk];
            mn->dir = CUI_ROW; mn->align = CUI_ALIGN_CENTER;
            cui__open(ui, mk);
            bool v = it->checked;
            if (cui_checkbox_ex(ui, cui__frame_printf(ui, "##task%d", it->task_off), &v, NULL)) {
                c->res->task_offset = it->task_off;
                c->res->task_checked = v;
                int line = 1;
                for (int q = 0; q < it->task_off && q < c->len; q++) if (c->src[q] == '\n') line++;
                c->res->task_line = line;
            }
            cui__close(ui);
        } else {
            md_mark_paint *mp = cui__frame_alloc(ui, sizeof *mp);
            mp->kind = m0.ordered ? 1 : 0;
            mp->depth = b->depth;
            mp->c = c->marker_c;
            mp->f = f;
            if (m0.ordered) mp->num = cui__frame_printf(ui, "%d%c", m0.num + k, m0.ch);
            ui->nodes[mk].paint = md_paint_marker;
            ui->nodes[mk].paint_data = mp;
        }
        /* the item's lines with the list prefix removed */
        int nl = it->end - it->start;
        md_line *sub = cui__frame_alloc(ui, sizeof(md_line) * (size_t)nl);
        sub[0] = it->first;
        for (int q = 1; q < nl; q++) {
            md_line Lq = L[it->start + q];
            int qi = md_indent(Lq, NULL);
            sub[q] = md_blank(Lq) ? md_skip(Lq, Lq.len) : md_strip_cols(Lq, CUI_MIN(qi, it->content_col));
        }
        int col = cui__node_new(ui, 0);
        ui->nodes[col].w = CUI_FILL;
        cui__open(ui, col);
        md_box cb = { b->w - colw, b->depth + 1, !loose, it->task && it->checked ? c->fg2 : b->fg };
        float w = md_blocks(c, sub, nl, &cb);
        cui__close(ui);
        cui__close(ui);
        best = CUI_MAX(best, w + colw);
    }
    cui__close(ui);
    if (nat) *nat = best;
    return j;
}

/* ---- callouts: > [!type]+- Title ---- */

typedef struct { cui_color c; float w, inset; } md_bar_paint;
static void md_paint_bar(cui_ctx *ui, cui_node *n, void *data) {
    md_bar_paint *p = data;
    cui_draw_rect(ui, (cui_rect){ n->rect.x + p->inset * 0.5f, n->rect.y + p->inset, p->w, n->rect.h - p->inset * 2 }, CUI_CORNERS(p->w * 0.5f), p->c);
}

typedef struct { const char *name; cui_tone tone; const char *icon; } md_callout_kind;
static const md_callout_kind md_callout_kinds[] = {
    { "note", CUI_TONE_ACCENT, CUI_ICON_PENCIL },
    { "abstract", CUI_TONE_TEAL, CUI_ICON_CLIPBOARD }, { "summary", CUI_TONE_TEAL, CUI_ICON_CLIPBOARD }, { "tldr", CUI_TONE_TEAL, CUI_ICON_CLIPBOARD },
    { "info", CUI_TONE_INFO, CUI_ICON_INFO },
    { "todo", CUI_TONE_INFO, CUI_ICON_CIRCLE_CHECK },
    { "tip", CUI_TONE_TEAL, CUI_ICON_ZAP }, { "hint", CUI_TONE_TEAL, CUI_ICON_ZAP },
    { "important", CUI_TONE_VIOLET, CUI_ICON_SPARKLES },
    { "success", CUI_TONE_SUCCESS, CUI_ICON_CHECK }, { "check", CUI_TONE_SUCCESS, CUI_ICON_CHECK }, { "done", CUI_TONE_SUCCESS, CUI_ICON_CHECK },
    { "question", CUI_TONE_WARNING, CUI_ICON_CIRCLE_HELP }, { "help", CUI_TONE_WARNING, CUI_ICON_CIRCLE_HELP }, { "faq", CUI_TONE_WARNING, CUI_ICON_CIRCLE_HELP },
    { "warning", CUI_TONE_WARNING, CUI_ICON_TRIANGLE_ALERT }, { "caution", CUI_TONE_WARNING, CUI_ICON_TRIANGLE_ALERT },
    { "attention", CUI_TONE_WARNING, CUI_ICON_TRIANGLE_ALERT },
    { "failure", CUI_TONE_DANGER, CUI_ICON_X }, { "fail", CUI_TONE_DANGER, CUI_ICON_X }, { "missing", CUI_TONE_DANGER, CUI_ICON_X },
    { "danger", CUI_TONE_DANGER, CUI_ICON_ZAP }, { "error", CUI_TONE_DANGER, CUI_ICON_ZAP },
    { "bug", CUI_TONE_DANGER, CUI_ICON_BUG },
    { "example", CUI_TONE_VIOLET, CUI_ICON_LIST },
    { "quote", CUI_TONE_NEUTRAL, CUI_ICON_QUOTE }, { "cite", CUI_TONE_NEUTRAL, CUI_ICON_QUOTE },
};

typedef struct {
    const char *type; int type_len;
    char fold;                 /* 0, '+' (open) or '-' (collapsed) */
    md_line title;
} md_callout_head;

typedef struct { bool init, open; float hover, focus; } md_callout_state;

/* [!type], [!type]+ or [!type]- then an optional title, as the first line of a quote. */
static bool md_callout_parse(md_line L, md_callout_head *h) {
    L = md_trim(L);
    if (L.len < 4 || L.s[0] != '[' || L.s[1] != '!') return false;
    int e = 2;
    while (e < L.len && (md_alnum(L.s[e]) || L.s[e] == '-' || L.s[e] == '_')) e++;
    if (e == 2 || e >= L.len || L.s[e] != ']') return false;
    h->type = L.s + 2; h->type_len = e - 2;
    h->fold = 0;
    e++;
    if (e < L.len && (L.s[e] == '+' || L.s[e] == '-')) h->fold = L.s[e++];
    h->title = md_trim(md_skip(L, e));
    return true;
}

/* The fold state of a callout follows its content, not its position, so adding
 * a callout above another keeps what the reader opened. Identical callouts are
 * told apart by how many came before them. */
static cui_id md_callout_id(md_ctx *c, const md_callout_head *h, const md_line *body, int nbody) {
    uint32_t key = 0xCA110u + (uint32_t)(unsigned char)h->fold;
    char *lc = cui__frame_alloc(c->ui, (size_t)h->type_len + 1);
    for (int q = 0; q < h->type_len; q++) lc[q] = md_lc(h->type[q]);
    key = cui__hash(lc, (size_t)h->type_len, key);
    key = cui__hash(h->title.s, (size_t)h->title.len, key ^ 1u);
    for (int k = 0; k < nbody; k++) key = cui__hash(body[k].s, (size_t)body[k].len, key + 2u);
    int occ = 0;
    for (int k = 0; k < c->callout_n; k++) if (c->callout_keys[k] == key) occ++;
    if (c->callout_n < CUI_COUNTOF(c->callout_keys)) c->callout_keys[c->callout_n++] = key;
    return cui__hash(&occ, sizeof occ, key ^ c->id);
}

static void md_callout(md_ctx *c, const md_box *b, const md_line *sub, int count, const md_callout_head *h, float *nat) {
    cui_ctx *ui = c->ui;
    const md_callout_kind *kind = &md_callout_kinds[0];   /* unknown types are notes */
    for (int k = 0; k < CUI_COUNTOF(md_callout_kinds); k++) {
        const char *nm = md_callout_kinds[k].name;
        if ((int)strlen(nm) != h->type_len) continue;
        bool same = true;
        for (int q = 0; q < h->type_len && same; q++) same = md_lc(h->type[q]) == nm[q];
        if (same) { kind = &md_callout_kinds[k]; break; }
    }
    const char *title = NULL;
    int title_len = h->title.len;
    if (title_len > 0) title = h->title.s;
    else {   /* the type name, capitalized */
        char *t = cui__frame_alloc(ui, (size_t)h->type_len + 1);
        for (int q = 0; q < h->type_len; q++) t[q] = (char)(q == 0 && md_lc(h->type[q]) >= 'a' ? md_lc(h->type[q]) - 32 : md_lc(h->type[q]));
        t[h->type_len] = 0;
        title = t; title_len = h->type_len;
    }
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color tone, bg;
    if (c->custom_color) { tone = c->fg; bg = cui_alpha(c->fg, 0.08f); }
    else {
        tone = kind->tone == CUI_TONE_NEUTRAL ? CUI_C(ui, TEXT_SECONDARY) : cui__tone_color(ui, kind->tone);
        bg = cui_mix(CUI_C(ui, SURFACE), kind->tone == CUI_TONE_NEUTRAL ? CUI_C(ui, FOREGROUND) : tone, dark ? 0.11f : 0.07f);
    }
    float u = c->u;
    bool foldable = h->fold != 0;
    cui_id hid = md_callout_id(c, h, sub + 1, count - 1);
    md_callout_state *st = NULL;
    cui_interact r = { 0 };
    if (foldable) {
        r = cui__interact(ui, hid, CUI_IF_FOCUSABLE);
        st = cui_state(ui, hid, sizeof *st);
        if (!st->init) { st->init = true; st->open = h->fold == '+'; }
        if (r.clicked) st->open = !st->open;
        md_sel_drag_off(c, r);
        cui__tween(ui, &st->hover, r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
        cui__tween(ui, &st->focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
        if (r.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    }
    bool open = !foldable || st->open;
    float padx = 14 * u, pady = 10 * u;
    int box = cui__node_new(ui, 0);
    cui_node *bn = &ui->nodes[box];
    bn->w = CUI_FILL;
    bn->pad[0] = pady; bn->pad[2] = pady; bn->pad[1] = padx; bn->pad[3] = padx + 3 * u;
    bn->radius = CUI_CORNERS(ui->theme.radius_control);
    bn->bg = bg;
    md_bar_paint *bar = cui__frame_alloc(ui, sizeof *bar);
    bar->c = cui_alpha(tone, 0.85f); bar->w = 3 * u; bar->inset = ui->theme.radius_control * 0.55f;
    bn->paint = md_paint_bar;
    bn->paint_data = bar;
    cui__open(ui, box);
    md_font f = md_body(c);
    int row = cui__node_new(ui, foldable ? hid : 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->align = CUI_ALIGN_CENTER; rn->gap = 8 * u; rn->w = CUI_FILL;
    cui__open(ui, row);
    cui__icon_node(ui, kind->icon, f.size * 1.15f, tone);
    {   /* the title as written, no markdown in it: a block of its own for the selection */
        md_font tf = md_make_font(c, c->body, CUI_FONT_MEDIUM, CUI_FONT_MEDIUM, 1.5f);
        md_run run = { title, title_len, 0, -1, 0 };
        md_inline tin = { &run, 1, NULL, 0 };
        float tw = b->w - padx * 2 - 3 * u - f.size * 1.15f - 8 * u - (foldable ? f.size * 1.2f + 8 * u : 0);
        md_inline_emit(c, &tin, &tf, tone, CUI_TEXT_LEFT, tw);
    }
    if (foldable) {
        cui__chevron_node(ui, open ? 0.0f : -1.5708f, f.size * 1.2f, cui_alpha(tone, 0.6f + 0.4f * st->hover));
    }
    cui__close(ui);
    if (foldable) {
        cui__mark_interactive(ui, row, CUI_IF_FOCUSABLE);
        if (st->focus > 0.01f) cui__focus_ring(ui, row, st->focus);
    }
    float w = 0;
    if (open && count > 1) {
        md_space(c, 2 * u);
        md_box cb = { b->w - padx * 2 - 3 * u, b->depth + 1, false, c->fg };
        w = md_blocks(c, sub + 1, count - 1, &cb);
    }
    cui__close(ui);
    if (nat) *nat = w + padx * 2 + 3 * u;
}

static int md_quote(md_ctx *c, const md_box *b, const md_line *L, int n, int i, float *nat) {
    cui_ctx *ui = c->ui;
    md_line *sub = cui__frame_alloc(ui, sizeof(md_line) * (size_t)(n - i));
    int count = 0, j = i;
    bool prev_text = false;
    while (j < n) {
        int bytes, ind = md_indent(L[j], &bytes);
        md_line T = md_skip(L[j], bytes);
        if (ind < 4 && T.len > 0 && T.s[0] == '>') {
            T = md_skip(T, 1);
            if (T.len > 0 && md_sp(T.s[0])) T = md_skip(T, 1);
            sub[count++] = T;
            prev_text = !md_blank(T);
        } else if (prev_text && !md_blank(L[j]) && !md_interrupts(L[j])) {
            sub[count++] = L[j];   /* lazy continuation of a quoted paragraph */
        } else break;
        j++;
    }
    md_callout_head ch;
    if (count > 0 && md_callout_parse(sub[0], &ch)) {
        md_callout(c, b, sub, count, &ch, nat);
        return j;
    }
    float u = c->u;
    int q = cui__node_new(ui, 0);
    cui_node *qn = &ui->nodes[q];
    qn->w = CUI_FILL;
    qn->pad[3] = 14 * u;
    cui_color *rc = cui__frame_alloc(ui, sizeof *rc);
    *rc = c->rail_c;
    qn->paint = md_paint_rail;
    qn->paint_data = rc;
    cui__open(ui, q);
    md_box qb = { b->w - 14 * u, b->depth + 1, false, c->fg2 };
    float w = md_blocks(c, sub, count, &qb);
    cui__close(ui);
    if (nat) *nat = w + 14 * u;
    return j;
}

static int md_table(md_ctx *c, const md_box *b, const md_line *L, int n, int i, int ncols, const uint8_t *align, float *nat) {
    cui_ctx *ui = c->ui;
    if (ncols > MD_MAX_COLS) ncols = MD_MAX_COLS;
    bool has_delim = i + 1 < n && md_delim_row(L[i + 1], (uint8_t[MD_MAX_COLS]){ 0 }, MD_MAX_COLS, md_open_line(c, L[i + 1])) > 0;
    int j = i + (has_delim ? 2 : 1);
    while (j < n && !md_blank(L[j]) && md_has(L[j], '|')) j++;
    int nrows = j - i - (has_delim ? 2 : 1);
    md_font f = md_body(c);
    md_font hf = md_make_font(c, c->ui->theme.text_sm * c->body / c->ui->theme.text_body, CUI_FONT_MEDIUM, CUI_FONT_MEDIUM, 1.3f);
    md_inline *cells = cui__frame_alloc(ui, sizeof(md_inline) * (size_t)((nrows + 1) * ncols));
    int *rowc = cui__frame_alloc(ui, sizeof(int) * (size_t)(nrows + 1));
    float *colw = cui__frame_alloc(ui, sizeof(float) * (size_t)ncols * 2), *floor_w = colw + ncols;
    md_line raw[MD_MAX_COLS];
    for (int r = 0; r <= nrows; r++) {
        md_line R = L[r == 0 ? i : i + (has_delim ? 2 : 1) + r - 1];
        int nc = md_cells(R, raw, MD_MAX_COLS);
        rowc[r] = CUI_MIN(nc, ncols);
        bool tail = r == nrows && md_tail(c, R);
        for (int k = 0; k < rowc[r]; k++) {
            md_inline *in = &cells[r * ncols + k];
            *in = md_parse_inline(ui, raw[k].s, raw[k].len, tail && k == rowc[r] - 1);
            float w = md_runs_width(ui, in, r == 0 ? &hf : &f);
            if (r == 0) { floor_w[k] = w + 16; colw[k] = w + 16; }
            else colw[k] = CUI_MAX(colw[k], w + 16);
        }
    }
    float total = 18, u = c->u;
    for (int k = 0; k < ncols; k++) {
        floor_w[k] = CUI_MAX(floor_w[k], 48.0f * u);
        colw[k] = cui__clampf(colw[k], 48.0f * u, 360.0f * u);
        total += colw[k];
    }
    float natural = total;
    float avail = b->w;
    if (total < avail) {   /* fill the width, wider columns taking more */
        float extra = avail - total, sum = total - 18;
        for (int k = 0; k < ncols; k++) colw[k] += extra * colw[k] / sum;
    } else if (total > avail) {   /* narrow: long cells wrap first, then the table scrolls */
        for (int pass = 0; pass < 4 && total > avail + 0.5f; pass++) {
            float give = 0;
            for (int k = 0; k < ncols; k++) give += CUI_MAX(0.0f, colw[k] - CUI_MAX(floor_w[k], 120.0f * u));
            if (give <= 0) break;
            float need = CUI_MIN(give, total - avail);
            total = 18;
            for (int k = 0; k < ncols; k++) {
                float g = CUI_MAX(0.0f, colw[k] - CUI_MAX(floor_w[k], 120.0f * u));
                colw[k] -= need * g / give;
                total += colw[k];
            }
        }
    }
    cui_table_column *cols = cui__frame_alloc(ui, sizeof(cui_table_column) * (size_t)ncols);
    for (int k = 0; k < ncols; k++) {
        cols[k].title = k < rowc[0] ? md_plain(ui, &cells[k]) : "";
        cols[k].width = CUI_PX(colw[k]);
        cols[k].align = (cui_text_align)align[k];
    }
    cui_table_opts to;
    memset(&to, 0, sizeof to);
    to.compact = true;
    int head_from = ui->node_count;
    cui_table_begin_ex(ui, cui__frame_printf(ui, "table%d", c->table_seq++), cols, ncols, &to);
    if (c->sel->st)   /* the header the table drew, cell by cell */
        for (int k = 0; k < rowc[0]; k++) {
            int tn = cui__find_text_node(ui, head_from, cols[k].title, -1);
            if (tn < 0) break;
            c->sel->sep = k > 0 ? '\t' : '\n';
            md_sel_text_node(c, tn, &cells[k]);
            head_from = tn + 1;
        }
    for (int r = 1; r <= nrows; r++) {
        cui_table_row(ui, false);
        for (int k = 0; k < ncols; k++) {
            cui_table_cell(ui);
            c->sel->sep = k > 0 ? '\t' : '\n';   /* copied cells keep their rows */
            if (k < rowc[r]) md_inline_emit(c, &cells[r * ncols + k], &f, b->fg, align[k], colw[k] - 16);
        }
    }
    c->sel->sep = '\n';
    cui_table_end(ui);
    if (nat) *nat = natural;
    return j;
}

static float md_blocks(md_ctx *c, const md_line *L, int n, const md_box *b) {
    cui_ctx *ui = c->ui;
    float best = 0;
    if (b->depth > MD_MAX_DEPTH) {   /* too deep: the rest is text */
        md_line *keep = cui__frame_alloc(ui, sizeof(md_line) * (size_t)(n + 1));
        int k = 0;
        for (int q = 0; q < n; q++) if (!md_blank(L[q])) keep[k++] = L[q];
        if (k) best = md_paragraph(c, b, keep, k);
        return best;
    }
    int i = 0, prev = BK_NONE;
    while (i < n) {
        md_line Li = L[i];
        if (md_blank(Li)) { i++; continue; }
        int bytes, ind = md_indent(Li, &bytes);
        md_line T = md_skip(Li, bytes), info, content;
        float w = 0;
        char fch;
        int flen, level;
        uint8_t align[MD_MAX_COLS];
        md_marker m;
        if (ind >= 4) {   /* indented code */
            int j = i, last = i;
            while (j < n && (md_blank(L[j]) || md_indent(L[j], NULL) >= 4)) { if (!md_blank(L[j])) last = j; j++; }
            md_space(c, md_gap_before(c, b, prev, BK_CODE, 0));
            md_line none = { "", 0, 0 };
            w = md_code(c, L + i, last - i + 1, 4, none);
            prev = BK_CODE;
            i = last + 1;
        } else if (md_fence(T, &fch, &flen, &info)) {
            int j = i + 1;
            while (j < n && !md_fence_close(L[j], fch, flen)) j++;
            md_space(c, md_gap_before(c, b, prev, BK_CODE, 0));
            w = md_code(c, L + i + 1, j - i - 1, ind, info);
            prev = BK_CODE;
            i = j < n ? j + 1 : n;
        } else if (md_atx(T, &level, &content)) {
            md_space(c, md_gap_before(c, b, prev, BK_HEADING, level));
            w = md_heading(c, b, level, content, content.s, content.len, md_tail(c, Li));
            prev = BK_HEADING;
            i++;
        } else if (md_hrule(T)) {
            md_space(c, md_gap_before(c, b, prev, BK_HR, 0));
            int r = cui__leaf(ui, 0, CUI_FILL, CUI_PX(1));
            ui->nodes[r].bg = c->rule_c;
            prev = BK_HR;
            i++;
        } else if (T.s[0] == '>') {
            md_space(c, md_gap_before(c, b, prev, BK_QUOTE, 0));
            i = md_quote(c, b, L, n, i, &w);
            prev = BK_QUOTE;
        } else if (md_list_marker(T, &m)) {
            md_space(c, md_gap_before(c, b, prev, BK_LIST, 0));
            i = md_list(c, b, L, n, i, &w);
            prev = BK_LIST;
        } else if ((level = md_table_at(c, L, n, i, align)) > 0) {
            md_space(c, md_gap_before(c, b, prev, BK_TABLE, 0));
            i = md_table(c, b, L, n, i, level, align, &w);
            prev = BK_TABLE;
        } else {   /* paragraph, or a setext heading */
            int j = i + 1, setext = 0;
            while (j < n && !md_blank(L[j])) {
                if (md_indent(L[j], NULL) < 4 && (setext = md_setext(L[j])) > 0) break;
                if (md_interrupts(L[j]) || md_table_at(c, L, n, j, align)) break;
                j++;
            }
            if (setext && md_open_line(c, L[j])) {   /* the underline may still be a list or a rule: wait for its line */
                md_space(c, md_gap_before(c, b, prev, BK_PARA, 0));
                w = md_paragraph(c, b, L + i, j - i);
                prev = BK_PARA;
                i = j + 1;
            } else if (setext) {
                int len;
                const char *s = md_join(c, L + i, j - i, &len);
                md_space(c, md_gap_before(c, b, prev, BK_HEADING, setext));
                w = md_heading(c, b, setext, md_trim(L[i]), s, len, false);
                prev = BK_HEADING;
                i = j + 1;
            } else {
                md_space(c, md_gap_before(c, b, prev, BK_PARA, 0));
                w = md_paragraph(c, b, L + i, j - i);
                prev = BK_PARA;
                i = j;
            }
        }
        best = CUI_MAX(best, w);
    }
    return best;
}

/* Plain text (cui_markdown_opts.plain): every line is shown and copied as written. A
 * paragraph is the lines up to a blank one, with a hard break after each line; blank lines
 * leave a blank line's height, and copy puts them back. Nothing is parsed, so no markers,
 * links or images, and spaces do not hold their width. */
static float md_plain_text(md_ctx *c, const md_line *L, int n, float W) {
    md_font f = md_body(c);
    float best = 0;
    int i = 0, blanks = 0;
    bool any = false;
    while (i < n) {
        if (md_blank(L[i])) { blanks++; i++; continue; }
        int j = i;
        while (j < n && !md_blank(L[j])) j++;
        md_space(c, (float)blanks * f.lh);
        if (any) c->sel->gap = blanks;
        blanks = 0;
        md_run *runs = cui__frame_alloc(c->ui, sizeof(md_run) * (size_t)((j - i) * 2));
        int nr = 0;
        for (int k = i; k < j; k++) {
            md_line T = L[k];
            while (T.len > 0 && md_sp(T.s[T.len - 1])) T.len--;
            if (k > i) { runs[nr].p = NULL; runs[nr].len = 0; runs[nr].style = MS_BREAK; runs[nr].link = -1; nr++; }
            md_emit(runs, &nr, T.s, T.len, 0, -1);
        }
        int at = 0;
        for (int k = 0; k < nr; k++) { runs[k].at = at; at += (runs[k].style & MS_BREAK) ? 1 : runs[k].len; }
        md_inline in = { runs, nr, NULL, 0, c->keep_ws };
        float bw = md_inline_emit(c, &in, &f, c->fg, CUI_TEXT_LEFT, W);   /* not inside CUI_MAX: it evaluates twice */
        best = CUI_MAX(best, bw);
        any = true;
        i = j;
    }
    return best;
}

/* ------------------------------------------------------------------------ */
/* Front matter                                                             */
/* ------------------------------------------------------------------------ */

typedef struct { const char *k, *v; int klen, vlen; } md_prop;
typedef struct { bool open; float hover, focus; } md_fm_state;
typedef struct {
    const md_prop *p; int n;
    md_font f;
    float keyw, hover, focus, pad, row_h, head_h;
    bool open, rows;               /* rows: this node is the expanded list under the header */
    cui_color surface, key_c, val_c, mute_c;
    char head[32];
} md_fm_paint;

/* A line that is exactly --- (or ... when closing), spaces after it allowed. */
static bool md_fm_line(md_line L, char ch) {
    if (L.len < 3 || L.s[0] != ch || L.s[1] != ch || L.s[2] != ch) return false;
    for (int i = 3; i < L.len; i++) if (L.s[i] != ' ') return false;
    return true;
}

/* Index of the closing line when the text opens with front matter, else -1. */
static int md_fm_end(const md_line *L, int n) {
    if (n < 2 || !md_fm_line(L[0], '-')) return -1;
    for (int k = 1; k < n; k++) if (md_fm_line(L[k], '-') || md_fm_line(L[k], '.')) return k;
    return -1;
}

static md_line md_unquote(md_line T) {
    if (T.len >= 2 && (T.s[0] == '"' || T.s[0] == '\'') && T.s[T.len - 1] == T.s[0]) { T = md_skip(T, 1); T.len--; }
    return T;
}

/* Appends T (trimmed, unquoted) to the value being built in pool, after ", " when it is not the first. */
static void md_fm_add(char *pool, int *o, int *vlen, md_line T) {
    T = md_unquote(md_trim(T));
    if (T.len <= 0) return;
    if (*vlen > 0) { pool[(*o)++] = ','; pool[(*o)++] = ' '; *vlen += 2; }
    memcpy(pool + *o, T.s, (size_t)T.len);
    *o += T.len; *vlen += T.len;
}

/* key: value lines and simple lists become rows; anything else is skipped. */
static int md_fm_parse(cui_ctx *ui, const md_line *L, int n, md_prop **out) {
    md_prop *props = cui__frame_alloc(ui, sizeof(md_prop) * (size_t)(n + 1));
    int total = 0;
    for (int k = 0; k < n; k++) total += L[k].len + 4;
    char *pool = cui__frame_alloc(ui, (size_t)total + 8);
    int np = 0, o = 0, vlen = 0;
    for (int k = 0; k < n; k++) {
        md_line T = md_trim(L[k]);
        if (T.len == 0 || T.s[0] == '#') continue;
        bool indented = md_indent(L[k], NULL) > 0;
        if (T.s[0] == '-' && (T.len == 1 || md_sp(T.s[1]))) {   /* - item: belongs to the key above */
            if (np > 0) { md_fm_add(pool, &o, &vlen, md_skip(T, 1)); props[np - 1].vlen = vlen; }
            continue;
        }
        int colon = -1;
        for (int q = 0; q < T.len; q++) if (T.s[q] == ':' && (q + 1 == T.len || md_sp(T.s[q + 1]))) { colon = q; break; }
        if (colon <= 0) {
            if (indented && np > 0) { md_fm_add(pool, &o, &vlen, T); props[np - 1].vlen = vlen; }   /* continuation */
            continue;
        }
        md_line kl = T, key, val;
        kl.len = colon;
        key = md_trim(kl);
        val = md_trim(md_skip(T, colon + 1));
        md_prop *pr = &props[np++];
        pr->k = key.s; pr->klen = key.len;
        pr->v = pool + o; vlen = 0; pr->vlen = 0;
        if (val.len >= 2 && val.s[0] == '[' && val.s[val.len - 1] == ']') {   /* [a, b] */
            md_line in = md_skip(val, 1);
            in.len--;
            int st = 0;
            for (int q = 0; q <= in.len; q++) {
                if (q < in.len && in.s[q] != ',') continue;
                md_line item = md_skip(in, st);
                item.len = q - st;
                md_fm_add(pool, &o, &vlen, item);
                st = q + 1;
            }
        } else md_fm_add(pool, &o, &vlen, val);
        pr->vlen = vlen;
    }
    *out = props;
    return np;
}

static void md_paint_fm(cui_ctx *ui, cui_node *n, void *data) {
    md_fm_paint *p = data;
    cui_rect r = n->rect;
    float rad = ui->theme.radius_control;
    cui_corners cr = p->rows ? (cui_corners){ 0, 0, rad, rad } : p->open ? (cui_corners){ rad, rad, 0, 0 } : CUI_CORNERS(rad);
    cui_draw_rect(ui, r, cr, p->surface);
    const md_font *f = &p->f;
    float fh = cui__font_line_height(ui, f->reg, f->size);
    if (!p->rows) {
        if (p->hover > 0.01f) cui_draw_rect(ui, r, cr, cui_alpha(p->mute_c, 0.08f * p->hover));
        float ty = r.y + (r.h - fh) * 0.5f;
        cui__draw_text_line(ui, f->reg, f->size, r.x + p->pad, ty, p->mute_c, p->head, -1, false);
        float cx = r.x + r.w - p->pad - f->size * 0.5f, cy = r.y + r.h * 0.5f, s = f->size * 0.22f;
        float ang = p->open ? 0.0f : -1.5708f, ca = cosf(ang), sa = sinf(ang);
        cui_vec2 pts[3] = { { -s * 1.6f, -s * 0.8f }, { 0, s * 0.8f }, { s * 1.6f, -s * 0.8f } };
        for (int k = 0; k < 3; k++) { float x = pts[k].x, y = pts[k].y; pts[k].x = cx + x * ca - y * sa; pts[k].y = cy + x * sa + y * ca; }
        cui_draw_polyline(ui, pts, 3, 1.4f, p->mute_c, false);
        if (p->focus > 0.01f) cui__draw_focus_ring(ui, r, cr, p->focus);
        return;
    }
    float vx = r.x + p->pad + p->keyw + p->pad;
    float vw = CUI_MAX(8.0f, r.x + r.w - p->pad - vx);
    for (int k = 0; k < p->n; k++) {
        float y = r.y + (float)k * p->row_h + (p->row_h - fh) * 0.5f;
        cui__draw_text_ellipsis(ui, f->bold, f->size, r.x + p->pad, y, p->keyw, p->key_c, p->p[k].k, p->p[k].klen, false);
        cui__draw_text_ellipsis(ui, f->reg, f->size, vx, y, vw, p->val_c, p->p[k].v, p->p[k].vlen, false);
    }
}

/* The collapsed "Properties" block above the document. */
static void md_front_matter(md_ctx *c, const md_prop *props, int np, float W) {
    cui_ctx *ui = c->ui;
    cui_id hid = cui__hash(&np, sizeof np, c->id ^ 0xF407A77u);
    cui_interact r = cui__interact(ui, hid, CUI_IF_FOCUSABLE);
    md_fm_state *st = cui_state(ui, hid, sizeof *st);
    if (r.clicked) st->open = !st->open;
    cui__tween(ui, &st->hover, r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
    cui__tween(ui, &st->focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    if (r.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    md_font f = md_make_font(c, c->body * 12.0f / 13.0f, CUI_FONT_BODY, CUI_FONT_MEDIUM, 1.5f);
    md_fm_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->p = props; p->n = np; p->f = f; p->open = st->open;
    p->pad = 10.0f * c->u; p->row_h = f.size * 1.5f + 4.0f * c->u; p->head_h = f.size * 1.5f + 12.0f * c->u;
    p->hover = st->hover; p->focus = st->focus;
    p->surface = c->custom_color ? cui_alpha(c->fg, 0.06f) : cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, FOREGROUND), 0.045f);
    p->key_c = c->fg2; p->val_c = c->fg; p->mute_c = c->custom_color ? c->fg2 : CUI_C(ui, TEXT_MUTED);
    snprintf(p->head, sizeof p->head, "Properties \xc2\xb7 %d", np);
    for (int k = 0; k < np; k++) p->keyw = CUI_MAX(p->keyw, cui__text_width(ui, f.bold, f.size, props[k].k, props[k].klen, false));
    p->keyw = CUI_MIN(p->keyw, W * 0.4f);
    int head = cui__leaf(ui, hid, CUI_FILL, CUI_PX(p->head_h));
    ui->nodes[head].paint = md_paint_fm;
    ui->nodes[head].paint_data = p;
    cui__mark_interactive(ui, head, CUI_IF_FOCUSABLE);
    if (st->open) {
        md_fm_paint *q = cui__frame_alloc(ui, sizeof *q);
        *q = *p;
        q->rows = true;
        int body = cui__leaf(ui, 0, CUI_FILL, CUI_PX((float)np * p->row_h + 6.0f * c->u));
        ui->nodes[body].paint = md_paint_fm;
        ui->nodes[body].paint_data = q;
    }
}

/* ------------------------------------------------------------------------ */
/* Selection input                                                          */
/* ------------------------------------------------------------------------ */

static int md_char_class(unsigned char ch) {   /* 0 space, 1 word, 2 punctuation */
    if (ch == ' ' || ch == '\t' || ch == '\n') return 0;
    return (ch >= 0x80 || ch == '_' || md_alnum((char)ch)) ? 1 : 2;
}

/* The word (or whole block) around the probe's position, from the candidate block's text. */
static void md_sel_unit(cui_ctx *ui, const md_sel *S, int unit, md_pos *w0, md_pos *w1) {
    if (S->diag && unit == MD_BY_WORD) {   /* in a diagram a word is a whole label */
        w0->block = w1->block = S->at.block;
        w0->off = S->lab_a >= 0 ? S->lab_a : S->at.off;
        w1->off = S->lab_a >= 0 ? S->lab_b : S->at.off;
        return;
    }
    const char *t = S->code;
    int len = S->code_len;
    if (!t) {
        len = 0;
        for (int k = 0; k < S->nruns; k++) len += (S->runs[k].style & MS_BREAK) ? 1 : S->runs[k].len;
        char *buf = cui__frame_alloc(ui, (size_t)len + 1);
        for (int k = 0; k < S->nruns; k++) {
            const md_run *R = &S->runs[k];
            if (R->style & MS_BREAK) buf[R->at] = '\n';
            else memcpy(buf + R->at, R->p, (size_t)R->len);
        }
        t = buf;
    }
    w0->block = w1->block = S->at.block;
    if (unit == MD_BY_BLOCK) { w0->off = 0; w1->off = len; return; }
    int off = CUI_CLAMP(S->at.off, 0, len), k = off;
    if (len == 0) { w0->off = w1->off = 0; return; }
    /* just past a word's end still takes that word */
    if (k >= len || (md_char_class((unsigned char)t[k]) != 1 && k > 0 && md_char_class((unsigned char)t[k - 1]) == 1)) k--;
    int cls = md_char_class((unsigned char)t[k]), a = k, b = k + 1;
    while (a > 0 && md_char_class((unsigned char)t[a - 1]) == cls) a--;
    while (b < len && md_char_class((unsigned char)t[b]) == cls) b++;
    w0->off = a; w1->off = b;
}

static uint32_t md_text_hash(md_ctx *c) { return cui__hash(c->src, (size_t)c->len, 0x5E1EC7u); }

static void md_sel_order(md_sel *S) {
    bool fwd = md_pos_cmp(S->st->anchor, S->st->head) <= 0;
    S->lo = fwd ? S->st->anchor : S->st->head;
    S->hi = fwd ? S->st->head : S->st->anchor;
}

/* Before the blocks: the press, drag and keys, and the selection they paint with. */
static void md_sel_begin(md_ctx *c, int root) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    md_sel_state *st = cui_state(ui, c->id ^ 0x5E1EC7u, sizeof *st);
    S->st = st;
    S->color = cui__selection_color(ui);
    cui__mark_interactive(ui, root, 0);   /* gets the presses the links and boxes on top leave */
    cui_interact r = cui__interact(ui, c->id, CUI_IF_DRAG);
    bool has = md_pos_cmp(st->anchor, st->head) != 0;
    if (has || st->dragging || st->anchored) { S->hash = md_text_hash(c); S->hashed = true; }
    if (S->hashed && (st->text_len != c->len || st->text_hash != S->hash)) {
        st->head = st->anchor;   /* the text changed under it */
        st->dragging = st->anchored = false;
        has = false;
    }
    S->x = ui->mx; S->y = ui->my;
    S->hover = r.hovered;
    if (ui->mpressed[CUI_MOUSE_LEFT]) {   /* a press in the view takes the keys and starts over; one elsewhere lets go */
        bool in_view = cui__has_prev_rect(ui, c->id) && cui__rect_contains(cui__prev_rect(ui, c->id), ui->mx, ui->my);
        st->focused = r.active || (in_view && ui->hot_layer == ui->current_layer);
        S->extend = r.active && (ui->mods & CUI_MOD_SHIFT) && st->anchored;
        if (st->focused && !S->extend) { st->head = st->anchor; has = false; }
    }
    if (r.active && ui->mpressed[CUI_MOUSE_LEFT]) {
        S->probe = S->pressing = true;
        st->dragging = true;
        st->unit = S->extend ? MD_BY_CHAR : ui->click_count >= 3 ? MD_BY_BLOCK : ui->click_count == 2 ? MD_BY_WORD : MD_BY_CHAR;
    } else if (st->link_drag) {
        st->link_drag = false;
        if (r.active && ui->mdown[CUI_MOUSE_LEFT]) {
            S->probe = S->pressing = true;
            S->x = ui->last_click_x; S->y = ui->last_click_y;
            st->dragging = true;
            st->unit = MD_BY_CHAR;
        }
    } else if (st->dragging) {
        if (r.active && ui->mdown[CUI_MOUSE_LEFT]) S->probe = true;
        else st->dragging = false;
    }
    if (st->focused && !ui->edit_prev) {
        if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_A)) {
            st->anchor.block = st->anchor.off = 0;
            st->head.block = MD_SEL_END; st->head.off = 0;
            st->dragging = false;
            S->probe = false;
            has = true;
        }
        if (has && cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_C)) S->copying = true;
    }
    md_sel_order(S);
}

/* After the blocks: the probe's answer moves the selection; copy, cursor, code blocks. */
static void md_sel_end(md_ctx *c) {
    cui_ctx *ui = c->ui;
    md_sel *S = c->sel;
    md_sel_state *st = S->st;
    if (!st) return;
    if (S->probe) {
        md_pos at = { 0, 0 }, w0, w1;   /* above every block: the start */
        if (S->found) at = S->at;
        if (S->found && S->diag && S->lab_a >= 0 && st->unit == MD_BY_CHAR) {   /* labels select whole */
            md_pos a0 = { at.block, S->lab_a }, a1 = { at.block, S->lab_b };
            at = (S->pressing && !S->extend) || md_pos_cmp(a0, st->anchor) < 0 ? a0 : a1;
        }
        w0 = w1 = at;
        if (st->unit != MD_BY_CHAR && S->found) md_sel_unit(ui, S, st->unit, &w0, &w1);
        if (S->pressing && !S->extend) { st->anchor = st->unit0 = w0; st->head = st->unit1 = w1; st->anchored = true; }
        else if (st->unit == MD_BY_CHAR) st->head = at;
        else if (md_pos_cmp(at, st->unit0) >= 0) { st->anchor = st->unit0; st->head = w1; }
        else { st->anchor = st->unit1; st->head = w0; }
    }
    md_sel_order(S);
    if (md_pos_cmp(S->lo, S->hi) != 0 || st->dragging || st->anchored) {   /* the text the positions belong to */
        st->text_len = c->len;
        st->text_hash = S->hashed ? S->hash : md_text_hash(c);
    }
    for (md_code_sel *cs = S->codes; cs; cs = cs->next) {
        int a, b;
        if (md_sel_range(S, cs->block, &a, &b)) { cs->cs.from = a; cs->cs.to = b; }
    }
    for (md_diag_sel *ds = S->diags; ds; ds = ds->next) {
        int a, b;
        if (md_sel_range(S, ds->block, &a, &b)) { ds->ms.from = a; ds->ms.to = b; }
    }
    if (S->copying && S->copy_len > 0) { cui_set_clipboard_text(ui, S->copy); c->res->copied = true; }
    if ((S->hover && S->over_text) || (st->dragging && ui->active == c->id)) cui__set_cursor(ui, CUI_CURSOR_TEXT);
}

/* ------------------------------------------------------------------------ */
/* Public                                                                   */
/* ------------------------------------------------------------------------ */

cui_markdown_result cui_markdown_ex(cui_ctx *ui, const char *id_str, const char *text, const cui_markdown_opts *o) {
    cui_markdown_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_markdown_result res;
    memset(&res, 0, sizeof res);
    res.task_offset = -1;
    if (!text) text = "";
    if (!id_str) id_str = "markdown";
    int len = o->length > 0 ? o->length : (int)strlen(text);
    cui_id id = cui_make_id(ui, id_str);
    cui__emoji_text_on(ui);   /* notes and chat are full of emoji */

    md_ctx c;
    memset(&c, 0, sizeof c);
    c.ui = ui; c.id = id; c.src = text; c.len = len; c.res = &res;
    c.streaming = o->streaming; c.compact = o->compact; c.fit = o->fit_content; c.keep_ws = o->plain && o->preserve_whitespace;
    c.base_dir = o->base_dir; c.resolve_image = o->resolve_image; c.resolve_user = o->resolve_user;
    c.body = o->size > 0 ? o->size : ui->theme.text_body;
    c.u = c.body / 13.0f;
    c.content_end = len;
    while (c.content_end > 0 && md_ws((unsigned char)text[c.content_end - 1])) c.content_end--;
    c.custom_color = cui__color_set(o->color);
    if (c.custom_color) {
        c.fg = cui_color_of(ui, o->color);
        c.fg2 = cui_alpha(c.fg, 0.78f);
        c.link_c = c.fg;
        c.code_bg = cui_alpha(c.fg, 0.14f);
        c.marker_c = cui_alpha(c.fg, 0.7f);
        c.rule_c = cui_alpha(c.fg, 0.22f);
        c.rail_c = cui_alpha(c.fg, 0.35f);
    } else {
        c.fg = CUI_C(ui, FOREGROUND);
        c.fg2 = CUI_C(ui, TEXT_SECONDARY);
        c.link_c = cui_mix(CUI_C(ui, ACCENT_STRONG), CUI_C(ui, FOREGROUND), ui->accent == CUI_ACCENT_NEUTRAL ? 1.0f : 0.0f);
        c.code_bg = CUI_C(ui, ACTIVE);
        c.marker_c = CUI_C(ui, TEXT_SECONDARY);
        c.rule_c = CUI_C(ui, BORDER);
        c.rail_c = cui_mix(CUI_C(ui, BORDER), CUI_C(ui, BORDER_STRONG), 0.45f);
    }

    /* wrap at the width of the latest layout (exact when the width is fixed) */
    float W;
    cui_rect pr = cui__prev_rect(ui, id);
    if (o->width.kind == CUI_SIZE_FIXED) W = o->width.value;
    else if (cui__has_prev_rect(ui, id) && pr.w > 0) W = pr.w;
    else { W = CUI_MIN(ui->vw, 640.0f); ui->animating = true; }
    res.height = pr.h;

    int root = cui__node_new(ui, id);
    cui_node *rn = &ui->nodes[root];
    if (c.fit) { rn->w = CUI_FIT; rn->self_align = CUI__ALIGN_NOSTRETCH; }
    else rn->w = o->width.kind != CUI_SIZE_FIT ? o->width : CUI_FILL;
    md_root_paint *rp = cui__frame_alloc(ui, sizeof *rp);
    rp->w = W;
    rn->paint = md_paint_root;
    rn->paint_data = rp;
    c.sel = cui__frame_alloc(ui, sizeof *c.sel);
    c.sel->sep = '\n';
    if (!o->no_select) md_sel_begin(&c, root);
    cui__open(ui, root);
    cui_push_id(ui, id_str);

    int nlines = 1;
    for (int k = 0; k < len; k++) if (text[k] == '\n') nlines++;
    md_line *lines = cui__frame_alloc(ui, sizeof(md_line) * (size_t)nlines);
    int n = 0, st = 0;
    for (int k = 0; k <= len; k++) {
        if (k < len && text[k] != '\n') continue;
        int e = k;
        if (e > st && text[e - 1] == '\r') e--;
        if (!(k == len && st == len && n > 0)) { lines[n].s = text + st; lines[n].len = e - st; lines[n].off = st; n++; }
        st = k + 1;
    }
    int total = len > 0 ? n : 0, first = 0;
    c.lines = lines; c.nlines = total; c.root_id = id;
    c.heads = total > 0 ? cui__frame_alloc(ui, sizeof(cui_markdown_heading) * (size_t)total) : NULL;
    int fe = o->plain ? -1 : md_fm_end(lines, total);
    if (fe >= 0) {   /* front matter: a properties block, not markdown */
        first = fe + 1;
        md_prop *props;
        int np = md_fm_parse(ui, lines + 1, fe - 1, &props);
        if (np > 0 && !o->no_front_matter) {
            md_front_matter(&c, props, np, W);
            bool more = false;
            for (int k = first; k < total && !more; k++) more = !md_blank(lines[k]);
            if (more) md_space(&c, (c.compact ? 8.0f : 16.0f) * c.u);
        }
    }
    md_box box = { W, 0, false, c.fg };
    float natural = o->plain ? md_plain_text(&c, lines, total, W) : md_blocks(&c, lines + first, total - first, &box);
    res.headings = c.heads; res.heading_count = c.nheads;
    md_sel_end(&c);

    cui_pop_id(ui);
    cui__close(ui);
    if (c.fit) ui->nodes[root].pref_hint = CUI_MAX(1.0f, CUI_MIN(natural, 1e6f));
    ui->last_id = id;
    return res;
}

bool cui_markdown_apply_task(char *text, const cui_markdown_result *r) {
    if (!text || !r || r->task_offset < 1) return false;
    int off = r->task_offset;
    for (int k = 0; k <= off; k++) if (!text[k]) return false;   /* past the end */
    if (text[off - 1] != '[' || text[off + 1] != ']') return false;
    if (text[off] != ' ' && text[off] != 'x' && text[off] != 'X') return false;
    text[off] = r->task_checked ? 'x' : ' ';
    return true;
}
