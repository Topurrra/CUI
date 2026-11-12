/* Mermaid diagrams in the markdown view: a fenced ```mermaid block is drawn as a native diagram
 * instead of code. Flowcharts (graph, flowchart) are laid out with the dependency graph's layered
 * layout (cui_graph.c) and drawn here; pie charts become a donut chart with its legend; gantt
 * charts become the gantt component, read only. Any other diagram, and text that does not
 * parse, makes cui__mermaid_block return false, and the markdown view shows the code.
 *
 * Parsing is bounded (sizes are capped, nothing recurses) and runs only when the text changes:
 * the parsed and laid out diagram is kept in state keyed by a hash of the text. */
#include "cui_internal.h"

#define CUI__MM_MAX_SRC    65536
#define CUI__MM_MAX_NODES  300
#define CUI__MM_MAX_EDGES  600
#define CUI__MM_MAX_GROUPS 64
#define CUI__MM_MAX_NEST   12      /* nested subgraphs */
#define CUI__MM_MAX_FAN    16      /* nodes joined with & */
#define CUI__MM_MAX_LABEL  240     /* bytes of a label */
#define CUI__MM_MAX_LINES  6       /* lines of a label */
#define CUI__MM_MAX_BOXES  4000    /* nodes, edge labels and waypoints */
#define CUI__MM_MAX_SLICES 48
#define CUI__MM_MAX_TASKS  200
#define CUI__MM_MAX_DAYS   3660
#define CUI__MM_MAX_H      520.0f  /* taller diagrams scroll inside this height */
#define CUI__MM_PAD        16.0f
#define CUI__MM_MIN_ZOOM   0.55f   /* below this a wide diagram scrolls sideways instead */

enum { CUI__MM_NONE, CUI__MM_FLOW, CUI__MM_PIE, CUI__MM_GANTT };
enum { CUI__MMS_RECT, CUI__MMS_ROUND, CUI__MMS_STADIUM, CUI__MMS_SUB, CUI__MMS_DB, CUI__MMS_DIAMOND, CUI__MMS_HEX, CUI__MMS_CIRCLE, CUI__MMS_FLAG };
enum { CUI__MML_SOLID, CUI__MML_DOTTED, CUI__MML_THICK, CUI__MML_HIDDEN };
enum { CUI__MMH_NONE, CUI__MMH_ARROW, CUI__MMH_CIRCLE, CUI__MMH_CROSS };

typedef struct { int id, label, group; uint8_t shape; } cui__mm_node;      /* id, label: pool offsets; label lines split by '\n' */
typedef struct { int from, to, label; uint8_t style, head, tail; } cui__mm_edge;
typedef struct { int title, parent; float x, y, w, h; bool shown; } cui__mm_group;

/* strings of one diagram, NUL terminated; offset 0 is the empty string */
typedef struct { char *s; int used, cap; } cui__mm_pool;

typedef struct {
    bool init;
    uint32_t key;
    int kind;                    /* CUI__MM_*; NONE: not a diagram drawn here, show the code */
    char *pool;                  /* heap: every string below is an offset into it */
    int title;                   /* pie and gantt title, or -1 */
    /* flowchart */
    cui__layered g;              /* boxes: nodes, then edge labels, then waypoints */
    cui__mm_node *node;
    int nn;
    int *lab;                    /* label of each edge label box */
    int nlab;
    uint8_t *seg;                /* per laid out edge: style, head << 2, tail << 4 */
    cui__mm_group *group;
    int ng;
    bool flip;                   /* BT or RL: the layers run the other way */
    float x0, y0, bw, bh;        /* the drawn area, in layout units */
    float fs, lfs;               /* node and edge label text sizes */
    float fit_w, zoom, px, py, panel_h;
    bool pan, wheel;
    int mode;
    float last_x, last_y;
    /* pie */
    float *values;
    const char **names;
    int nval;
    /* gantt */
    cui_gantt_task *tasks;
    int ntasks, days;
    cui_date origin;
} cui__mm_state;

static void cui__mm_release(cui_ctx *ui, cui__mm_state *s) {
    cui__layered_free(ui, &s->g);
    cui__free(ui, s->pool); cui__free(ui, s->node); cui__free(ui, s->lab); cui__free(ui, s->seg);
    cui__free(ui, s->group); cui__free(ui, s->values); cui__free(ui, (void *)s->names); cui__free(ui, s->tasks);
    s->pool = NULL; s->node = NULL; s->lab = NULL; s->seg = NULL; s->group = NULL;
    s->values = NULL; s->names = NULL; s->tasks = NULL;
    s->nn = s->nlab = s->ng = s->nval = s->ntasks = 0;
    s->kind = CUI__MM_NONE;
}
static void cui__mm_dtor(cui_ctx *ui, void *data) { cui__mm_release(ui, (cui__mm_state *)data); }

/* ------------------------------------------------------------------------ */
/* Text helpers                                                             */
/* ------------------------------------------------------------------------ */

static bool cui__mm_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static bool cui__mm_alnum(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80; }
static int cui__mm_skip(const char *s, int i, int n) { while (i < n && cui__mm_sp(s[i])) i++; return i; }
static void cui__mm_trim(const char *s, int *a, int *b) {
    while (*a < *b && cui__mm_sp(s[*a])) (*a)++;
    while (*b > *a && cui__mm_sp(s[*b - 1])) (*b)--;
}
static int cui__mm_find(const char *s, int i, int n, const char *what) {
    int k = (int)strlen(what);
    for (; i + k <= n; i++) if (!memcmp(s + i, what, (size_t)k)) return i;
    return -1;
}
/* s[i..n) starts with the word w (case insensitive), followed by an end, a space or a non word character */
static bool cui__mm_word(const char *s, int i, int n, const char *w) {
    int k = (int)strlen(w);
    if (n - i < k) return false;
    for (int j = 0; j < k; j++) {
        char c = s[i + j];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        char d = w[j];
        if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
        if (c != d) return false;
    }
    return i + k >= n || !cui__mm_alnum((unsigned char)s[i + k]);
}
/* the next line of s from *pos, trimmed to [*a, *b); false at the end */
static bool cui__mm_line(const char *s, int n, int *pos, int *a, int *b) {
    if (*pos >= n) return false;
    int e = *pos;
    while (e < n && s[e] != '\n') e++;
    *a = *pos; *b = e;
    *pos = e + 1;
    cui__mm_trim(s, a, b);
    return true;
}

static int cui__mm_put(cui__mm_pool *P, const char *s, int n) {
    if (n < 0 || P->used + n + 1 > P->cap) return 0;
    int at = P->used;
    memcpy(P->s + at, s, (size_t)n);
    P->s[at + n] = 0;
    P->used += n + 1;
    return at;
}
/* a label as shown: quotes and markdown backticks taken off, <br> as a line break, capped */
static int cui__mm_put_label(cui__mm_pool *P, const char *s, int n) {
    int a = 0, b = n;
    cui__mm_trim(s, &a, &b);
    if (b - a >= 2 && s[a] == '"' && s[b - 1] == '"') { a++; b--; }
    if (b - a >= 2 && s[a] == '`' && s[b - 1] == '`') { a++; b--; }
    cui__mm_trim(s, &a, &b);
    if (P->used + (b - a) + 1 > P->cap) return 0;
    int at = P->used, o = at, lines = 1;
    for (int i = a; i < b && o - at < CUI__MM_MAX_LABEL; ) {
        if (s[i] == '<' && i + 3 < b + 1 && (s[i + 1] == 'b' || s[i + 1] == 'B') && (s[i + 2] == 'r' || s[i + 2] == 'R')) {
            int j = i + 3;
            while (j < b && (cui__mm_sp(s[j]) || s[j] == '/')) j++;
            if (j < b && s[j] == '>') {
                if (lines < CUI__MM_MAX_LINES) { P->s[o++] = '\n'; lines++; } else P->s[o++] = ' ';
                i = j + 1;
                while (i < b && cui__mm_sp(s[i])) i++;
                continue;
            }
        }
        char c = s[i++];
        P->s[o++] = c == '\t' ? ' ' : c;
    }
    while (o > at && ((unsigned char)P->s[o - 1] & 0xC0) == 0x80) o--;   /* never end inside a UTF-8 sequence */
    if (o > at && (unsigned char)P->s[o - 1] >= 0xC0) o--;
    P->s[o++] = 0;
    P->used = o;
    return at;
}

/* Front matter (--- ... ---), blank lines and %% comments come before the diagram's first word.
 * Returns its kind, and where the rest of that line starts. */
static int cui__mm_kind(const char *s, int n, int *rest, int *next) {
    int pos = 0, a, b;
    bool front = false, first = true;
    while (cui__mm_line(s, n, &pos, &a, &b)) {
        if (first && b - a == 3 && !memcmp(s + a, "---", 3)) { front = true; first = false; continue; }
        first = false;
        if (front) { if (b - a == 3 && !memcmp(s + a, "---", 3)) front = false; continue; }
        if (a == b || (b - a >= 2 && s[a] == '%' && s[a + 1] == '%')) continue;
        int kind = CUI__MM_NONE, w = 0;
        if (cui__mm_word(s, a, b, "graph")) { kind = CUI__MM_FLOW; w = 5; }
        else if (cui__mm_word(s, a, b, "flowchart")) { kind = CUI__MM_FLOW; w = 9; }
        else if (cui__mm_word(s, a, b, "pie")) { kind = CUI__MM_PIE; w = 3; }
        else if (cui__mm_word(s, a, b, "gantt")) { kind = CUI__MM_GANTT; w = 5; }
        *rest = a + w;
        *next = pos;
        return kind;
    }
    return CUI__MM_NONE;
}

/* ------------------------------------------------------------------------ */
/* Flowchart parser                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui__mm_pool P;
    cui__mm_node *node;
    int nn;
    cui__mm_edge *edge;
    int ne;
    cui__mm_group *group;
    int ng;
    int nest[CUI__MM_MAX_NEST], depth;
    int dir;                     /* 0 top down, 1 bottom up, 2 left to right, 3 right to left */
    bool bad;
} cui__mm_flow;

/* a node id goes on through - and . only when a word character follows (A-->B ends at A) */
static bool cui__mm_idch(const char *s, int i, int n) {
    unsigned char c = (unsigned char)s[i];
    if (cui__mm_alnum(c)) return true;
    if (c == '-' || c == '.') return i + 1 < n && cui__mm_alnum((unsigned char)s[i + 1]);
    return false;
}

static int cui__mm_node_at(cui__mm_flow *f, const char *s, int *pi, int n) {
    int i = *pi, st = i;
    while (i < n && cui__mm_idch(s, i, n)) i++;
    if (i == st || i - st > 120) return -1;
    int idlen = i - st, shape = -1, open = 0;
    const char *close = NULL, *close2 = NULL;
    char c0 = i < n ? s[i] : 0, c1 = i + 1 < n ? s[i + 1] : 0, c2 = i + 2 < n ? s[i + 2] : 0;
    if (c0 == '[') {
        if (c1 == '[') { shape = CUI__MMS_SUB; open = 2; close = "]]"; }
        else if (c1 == '(') { shape = CUI__MMS_DB; open = 2; close = ")]"; }
        else if (c1 == '/' || c1 == '\\') { shape = CUI__MMS_RECT; open = 2; close = "/]"; close2 = "\\]"; }
        else { shape = CUI__MMS_RECT; open = 1; close = "]"; }
    } else if (c0 == '(') {
        if (c1 == '(' && c2 == '(') { shape = CUI__MMS_CIRCLE; open = 3; close = ")))"; }
        else if (c1 == '(') { shape = CUI__MMS_CIRCLE; open = 2; close = "))"; }
        else if (c1 == '[') { shape = CUI__MMS_STADIUM; open = 2; close = "])"; }
        else { shape = CUI__MMS_ROUND; open = 1; close = ")"; }
    } else if (c0 == '{') {
        if (c1 == '{') { shape = CUI__MMS_HEX; open = 2; close = "}}"; }
        else { shape = CUI__MMS_DIAMOND; open = 1; close = "}"; }
    } else if (c0 == '>') { shape = CUI__MMS_FLAG; open = 1; close = "]"; }
    int label = -1;
    if (shape >= 0) {
        i += open;
        int ts, te, at;
        int q = cui__mm_skip(s, i, n);
        if (q < n && s[q] == '"') {
            ts = q + 1;
            te = ts;
            while (te < n && s[te] != '"') te++;
            if (te >= n) return -1;
            at = cui__mm_skip(s, te + 1, n);
            int kl = (int)strlen(close);
            bool ok = at + kl <= n && !memcmp(s + at, close, (size_t)kl);
            if (!ok && close2) { ok = at + 2 <= n && !memcmp(s + at, close2, 2); }
            if (!ok) return -1;
            i = at + kl;
        } else {
            at = cui__mm_find(s, i, n, close);
            if (close2) { int a2 = cui__mm_find(s, i, n, close2); if (a2 >= 0 && (at < 0 || a2 < at)) at = a2; }
            if (at < 0) return -1;
            ts = i; te = at;
            i = at + (int)strlen(close);
        }
        label = cui__mm_put_label(&f->P, s + ts, te - ts);
    }
    if (i + 2 < n && s[i] == ':' && s[i + 1] == ':' && s[i + 2] == ':') {   /* :::class */
        i += 3;
        while (i < n && cui__mm_idch(s, i, n)) i++;
    }
    *pi = i;
    int v = -1;
    for (int k = 0; k < f->nn; k++) {
        const char *id = f->P.s + f->node[k].id;
        if (!strncmp(id, s + st, (size_t)idlen) && id[idlen] == 0) { v = k; break; }
    }
    if (v < 0) {
        if (f->nn >= CUI__MM_MAX_NODES) return -1;
        v = f->nn++;
        f->node[v].id = cui__mm_put(&f->P, s + st, idlen);
        f->node[v].label = -1;
        f->node[v].shape = CUI__MMS_RECT;
        f->node[v].group = -1;
    }
    if (shape >= 0) { f->node[v].label = label; f->node[v].shape = (uint8_t)shape; }
    if (f->depth > 0) f->node[v].group = f->nest[f->depth - 1];
    return v;
}

/* nodes joined with &: A & B */
static int cui__mm_fan(cui__mm_flow *f, const char *s, int *pi, int n, int *out) {
    int c = 0, i = *pi;
    for (;;) {
        int v = cui__mm_node_at(f, s, &i, n);
        if (v < 0 || c >= CUI__MM_MAX_FAN) return 0;
        out[c++] = v;
        int j = cui__mm_skip(s, i, n);
        if (j < n && s[j] == '&') { i = cui__mm_skip(s, j + 1, n); continue; }
        break;
    }
    *pi = i;
    return c;
}

/* the end of a link: > arrow, o circle, x cross (o and x only when no word goes on) */
static int cui__mm_head(const char *s, int *pi, int n) {
    int i = *pi;
    if (i < n && s[i] == '>') { *pi = i + 1; return CUI__MMH_ARROW; }
    if (i < n && (s[i] == 'o' || s[i] == 'x') && (i + 1 >= n || !cui__mm_alnum((unsigned char)s[i + 1]))) {
        *pi = i + 1;
        return s[i] == 'o' ? CUI__MMH_CIRCLE : CUI__MMH_CROSS;
    }
    return CUI__MMH_NONE;
}

/* A link at s[*pi]: --> --- -.-> ==> ~~~, with < o x ends, "-- text -->" and "-->|text|" labels. */
static bool cui__mm_link(cui__mm_flow *f, const char *s, int *pi, int n, cui__mm_edge *e) {
    int i = *pi, ts = -1, te = -1;
    e->tail = CUI__MMH_NONE; e->head = CUI__MMH_NONE; e->label = -1; e->style = CUI__MML_SOLID;
    if (i < n && s[i] == '<') { e->tail = CUI__MMH_ARROW; i++; }
    else if (i + 2 < n && (s[i] == 'o' || s[i] == 'x') && (s[i + 1] == '-' || s[i + 1] == '=') && s[i + 2] == s[i + 1]) {
        e->tail = s[i] == 'o' ? CUI__MMH_CIRCLE : CUI__MMH_CROSS;
        i++;
    }
    if (i >= n) return false;
    char c = s[i];
    if (c == '~') {
        int k = 0;
        while (i < n && s[i] == '~') { i++; k++; }
        if (k < 3) return false;
        e->style = CUI__MML_HIDDEN;
    } else if (c == '-' && i + 1 < n && s[i + 1] == '.') {   /* -.-  -.->  -. text .-> */
        e->style = CUI__MML_DOTTED;
        i += 2;
        if (i < n && cui__mm_sp(s[i])) {
            int q = cui__mm_find(s, i, n, ".-");
            if (q < 0) return false;
            ts = i; te = q;
            i = q + 2;
        } else {
            while (i < n && s[i] == '.') i++;
            if (i >= n || s[i] != '-') return false;
            i++;
        }
        while (i < n && s[i] == '-') i++;
        e->head = (uint8_t)cui__mm_head(s, &i, n);
    } else if ((c == '-' || c == '=') && i + 1 < n && s[i + 1] == c) {
        e->style = c == '=' ? CUI__MML_THICK : CUI__MML_SOLID;
        int k = 0;
        while (i < n && s[i] == c) { i++; k++; }
        int h = cui__mm_head(s, &i, n);
        if (h == CUI__MMH_NONE && k == 2) {   /* -- text --> */
            if (i >= n || !cui__mm_sp(s[i])) return false;
            int q = cui__mm_find(s, i, n, c == '-' ? "--" : "==");
            if (q < 0) return false;
            ts = i; te = q;
            i = q; k = 0;
            while (i < n && s[i] == c) { i++; k++; }
            h = cui__mm_head(s, &i, n);
            if (h == CUI__MMH_NONE && k < 3) return false;
        }
        e->head = (uint8_t)h;
    } else return false;
    int j = cui__mm_skip(s, i, n);
    if (j < n && s[j] == '|') {
        int q = j + 1;
        while (q < n && s[q] != '|') q++;
        if (q >= n) return false;
        ts = j + 1; te = q;
        i = q + 1;
    }
    if (ts >= 0) {
        int a = ts, b = te;
        cui__mm_trim(s, &a, &b);
        if (b > a) e->label = cui__mm_put_label(&f->P, s + a, b - a);
    }
    *pi = i;
    return true;
}

/* A statement: a keyword line, or a chain of nodes and links. */
static void cui__mm_statement(cui__mm_flow *f, const char *s, int a, int b) {
    cui__mm_trim(s, &a, &b);
    if (a >= b) return;
    if (b - a >= 2 && s[a] == '%' && s[a + 1] == '%') return;
    static const char *const skip[] = { "classDef", "class", "style", "linkStyle", "click", "direction", "accTitle", "accDescr" };
    for (int k = 0; k < CUI_COUNTOF(skip); k++) if (cui__mm_word(s, a, b, skip[k])) return;
    if (cui__mm_word(s, a, b, "end")) {
        if (b - a != 3 || f->depth <= 0) { f->bad = true; return; }
        f->depth--;
        return;
    }
    if (cui__mm_word(s, a, b, "subgraph")) {
        if (f->depth >= CUI__MM_MAX_NEST || f->ng >= CUI__MM_MAX_GROUPS) { f->bad = true; return; }
        int i = cui__mm_skip(s, a + 8, b), ts = i, te = b;
        int br = cui__mm_find(s, i, b, "[");
        if (br >= 0 && s[b - 1] == ']') { ts = br + 1; te = b - 1; }
        cui__mm_group *g = &f->group[f->ng];
        memset(g, 0, sizeof *g);
        g->title = cui__mm_put_label(&f->P, s + ts, te - ts);
        g->parent = f->depth > 0 ? f->nest[f->depth - 1] : -1;
        f->nest[f->depth++] = f->ng++;
        return;
    }
    int i = a, left[CUI__MM_MAX_FAN], right[CUI__MM_MAX_FAN];
    int nl = cui__mm_fan(f, s, &i, b, left);
    if (!nl) { f->bad = true; return; }
    for (;;) {
        i = cui__mm_skip(s, i, b);
        if (i >= b) break;
        cui__mm_edge e;
        if (!cui__mm_link(f, s, &i, b, &e)) { f->bad = true; return; }
        i = cui__mm_skip(s, i, b);
        int nr = cui__mm_fan(f, s, &i, b, right);
        if (!nr) { f->bad = true; return; }
        for (int x = 0; x < nl; x++)
            for (int y = 0; y < nr; y++) {
                if (f->ne >= CUI__MM_MAX_EDGES) { f->bad = true; return; }
                e.from = left[x]; e.to = right[y];
                f->edge[f->ne++] = e;
            }
        memcpy(left, right, sizeof(int) * (size_t)nr);
        nl = nr;
    }
}

static bool cui__mm_parse_flow(cui_ctx *ui, cui__mm_flow *f, const char *s, int n, int rest, int next) {
    memset(f, 0, sizeof *f);
    f->P.cap = n * 2 + 8 * (CUI__MM_MAX_NODES + CUI__MM_MAX_EDGES) + 16;
    f->P.s = cui__frame_alloc(ui, (size_t)f->P.cap);
    f->node = cui__frame_alloc(ui, sizeof(cui__mm_node) * CUI__MM_MAX_NODES);
    f->edge = cui__frame_alloc(ui, sizeof(cui__mm_edge) * CUI__MM_MAX_EDGES);
    f->group = cui__frame_alloc(ui, sizeof(cui__mm_group) * CUI__MM_MAX_GROUPS);
    if (!f->P.s || !f->node || !f->edge || !f->group) return false;
    f->P.s[0] = 0; f->P.used = 1;
    /* the header's direction, and maybe statements after a ; */
    int e = rest;
    while (e < n && s[e] != '\n') e++;
    int i = cui__mm_skip(s, rest, e);
    if (cui__mm_word(s, i, e, "TD") || cui__mm_word(s, i, e, "TB")) { f->dir = 0; i += 2; }
    else if (cui__mm_word(s, i, e, "BT")) { f->dir = 1; i += 2; }
    else if (cui__mm_word(s, i, e, "LR")) { f->dir = 2; i += 2; }
    else if (cui__mm_word(s, i, e, "RL")) { f->dir = 3; i += 2; }
    i = cui__mm_skip(s, i, e);
    if (i < e && s[i] == ';') i++;
    else if (i < e) return false;
    /* statements end at a line end or a ; outside quotes and brackets */
    int pos = i, depth = 0, st = i;
    bool quote = false;
    for (; pos <= n && !f->bad; pos++) {
        char c = pos < n ? s[pos] : '\n';
        if (c == '%' && !quote && pos + 1 < n && s[pos + 1] == '%') {   /* a comment to the line's end */
            cui__mm_statement(f, s, st, pos);
            while (pos < n && s[pos] != '\n') pos++;
            st = pos + 1; quote = false; depth = 0;
            continue;
        }
        if (c == '"') quote = !quote;
        else if (!quote && (c == '[' || c == '(' || c == '{')) depth++;
        else if (!quote && (c == ']' || c == ')' || c == '}')) depth = CUI_MAX(0, depth - 1);
        if (c == '\n' || (c == ';' && !quote && depth == 0)) {
            cui__mm_statement(f, s, st, pos);
            st = pos + 1;
            if (c == '\n') { quote = false; depth = 0; }
        }
    }
    (void)next;
    return !f->bad && f->nn > 0 && f->depth == 0;
}

/* ------------------------------------------------------------------------ */
/* Flowchart layout                                                         */
/* ------------------------------------------------------------------------ */

static float cui__mm_label_size(cui_ctx *ui, const char *t, float fs, float *th) {
    float lh = cui__font_line_height(ui, CUI_FONT_BODY, fs), w = 0;
    int lines = 0;
    for (const char *p = t;; ) {
        const char *q = strchr(p, '\n');
        int len = q ? (int)(q - p) : (int)strlen(p);
        w = CUI_MAX(w, cui__text_width(ui, CUI_FONT_BODY, fs, p, len, false));
        lines++;
        if (!q) break;
        p = q + 1;
    }
    *th = lines * lh;
    return w;
}

static void cui__mm_node_size(cui_ctx *ui, const cui__mm_state *s, int v, float *w, float *h) {
    const cui__mm_node *nd = &s->node[v];
    float th, tw = cui__mm_label_size(ui, s->pool + nd->label, s->fs, &th);
    float bw = CUI_MAX(tw + 28, 56.0f), bh = CUI_MAX(th + 18, 38.0f);
    switch (nd->shape) {
    case CUI__MMS_STADIUM: bw += bh * 0.4f; break;
    case CUI__MMS_SUB: bw += 14; break;
    case CUI__MMS_DB: bh += 12; break;
    case CUI__MMS_HEX: bw += bh * 0.5f; break;
    case CUI__MMS_FLAG: bw += bh * 0.35f; break;
    case CUI__MMS_DIAMOND: {   /* the text's box fits inside: half diagonals a and a / 2 */
        float a = CUI_MAX(tw * 0.5f + 10 + 2 * (th * 0.5f + 7), 30.0f);
        bw = 2 * a; bh = a;
        break;
    }
    case CUI__MMS_CIRCLE: bw = bh = CUI_MAX(sqrtf(tw * tw + th * th) + 16, 44.0f); break;
    default: break;
    }
    *w = bw; *h = bh;
}

static bool cui__mm_overlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

/* Group boxes around their nodes (inner groups first), each with room for its title. A box that
 * would cover a node of another group, or another group's box, is not drawn. */
static void cui__mm_groups(cui_ctx *ui, cui__mm_state *s) {
    if (!s->ng) return;
    size_t ng = (size_t)s->ng;
    int *depth = cui__frame_alloc(ui, sizeof(int) * ng), *order = cui__frame_alloc(ui, sizeof(int) * ng);
    float title_h = cui__font_line_height(ui, CUI_FONT_MEDIUM, s->lfs) + 6;
    for (int k = 0; k < s->ng; k++) {
        int d = 0;
        for (int p = s->group[k].parent; p >= 0 && d < CUI__MM_MAX_NEST + 1; p = s->group[p].parent) d++;
        depth[k] = d; order[k] = k;
        s->group[k].shown = false;
    }
    for (int a = 1; a < s->ng; a++) { int x = order[a], b = a - 1; while (b >= 0 && depth[order[b]] < depth[x]) { order[b + 1] = order[b]; b--; } order[b + 1] = x; }
    for (int o = 0; o < s->ng; o++) {
        int k = order[o];
        cui__mm_group *G = &s->group[k];
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int v = 0; v < s->nn; v++) {
            bool in = false;
            for (int p = s->node[v].group, d = 0; p >= 0 && d <= CUI__MM_MAX_NEST; p = s->group[p].parent, d++) if (p == k) { in = true; break; }
            if (!in) continue;
            const cui__layer_box *b = &s->g.box[v];
            x0 = CUI_MIN(x0, b->x); y0 = CUI_MIN(y0, b->y); x1 = CUI_MAX(x1, b->x + b->w); y1 = CUI_MAX(y1, b->y + b->h);
        }
        for (int c = 0; c < s->ng; c++) {
            if (!s->group[c].shown || s->group[c].parent != k) continue;
            x0 = CUI_MIN(x0, s->group[c].x); y0 = CUI_MIN(y0, s->group[c].y);
            x1 = CUI_MAX(x1, s->group[c].x + s->group[c].w); y1 = CUI_MAX(y1, s->group[c].y + s->group[c].h);
        }
        if (x0 > x1) continue;
        G->x = x0 - 12; G->y = y0 - 10 - title_h; G->w = x1 - x0 + 24; G->h = y1 - y0 + 20 + title_h;
        float tw = cui__text_width(ui, CUI_FONT_MEDIUM, s->lfs, s->pool + G->title, -1, false) + 20;
        if (tw > G->w) { G->x -= (tw - G->w) * 0.5f; G->w = tw; }   /* wide enough for its title */
        bool clash = false;
        for (int v = 0; v < s->nn + s->nlab && !clash; v++) {
            const cui__layer_box *b = &s->g.box[v];
            bool inside = b->x >= x0 && b->y >= y0 && b->x + b->w <= x1 && b->y + b->h <= y1;
            if (!inside && cui__mm_overlap(G->x, G->y, G->w, G->h, b->x, b->y, b->w, b->h)) clash = true;
        }
        for (int c = 0; c < s->ng && !clash; c++) {
            if (c == k || !s->group[c].shown) continue;
            const cui__mm_group *H = &s->group[c];
            bool nested = H->x >= G->x && H->y >= G->y && H->x + H->w <= G->x + G->w && H->y + H->h <= G->y + G->h;
            if (!nested && cui__mm_overlap(G->x, G->y, G->w, G->h, H->x, H->y, H->w, H->h)) clash = true;
        }
        G->shown = !clash;
    }
}

/* Copies the parsed chart into state and lays it out: an edge with a label runs through a box
 * of its own for the label, so labels get their place in a layer like nodes. */
static bool cui__mm_build_flow(cui_ctx *ui, cui__mm_state *s, const cui__mm_flow *f) {
    s->pool = cui__alloc(ui, (size_t)f->P.used);
    s->node = cui__alloc(ui, sizeof(cui__mm_node) * (size_t)f->nn);
    if (!s->pool || !s->node) return false;
    memcpy(s->pool, f->P.s, (size_t)f->P.used);
    memcpy(s->node, f->node, sizeof(cui__mm_node) * (size_t)f->nn);
    s->nn = f->nn;
    for (int v = 0; v < s->nn; v++) if (s->node[v].label < 0) s->node[v].label = s->node[v].id;
    if (f->ng) {
        s->group = cui__alloc(ui, sizeof(cui__mm_group) * (size_t)f->ng);
        if (!s->group) return false;
        memcpy(s->group, f->group, sizeof(cui__mm_group) * (size_t)f->ng);
        s->ng = f->ng;
    }
    int nlab = 0, nseg = 0;
    for (int k = 0; k < f->ne; k++) {
        if (f->edge[k].from == f->edge[k].to) continue;   /* a loop on one node is not drawn */
        if (f->edge[k].label > 0) { nlab++; nseg += 2; } else nseg++;
    }
    s->lab = cui__alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, nlab));
    s->seg = cui__alloc(ui, (size_t)CUI_MAX(1, nseg));
    if (!s->lab || !s->seg) return false;
    int nb = s->nn + nlab;
    float *bw = cui__frame_alloc(ui, sizeof(float) * (size_t)nb), *bh = cui__frame_alloc(ui, sizeof(float) * (size_t)nb);
    cui_graph_edge *ge = cui__frame_alloc(ui, sizeof(cui_graph_edge) * (size_t)CUI_MAX(1, nseg));
    for (int v = 0; v < s->nn; v++) cui__mm_node_size(ui, s, v, &bw[v], &bh[v]);
    int m = 0;
    for (int k = 0; k < f->ne; k++) {
        const cui__mm_edge *e = &f->edge[k];
        if (e->from == e->to) continue;
        uint8_t st = e->style;
        if (e->label > 0) {
            int L = s->nn + s->nlab;
            s->lab[s->nlab++] = e->label;
            float th, tw = cui__mm_label_size(ui, s->pool + e->label, s->lfs, &th);
            bw[L] = tw + 12; bh[L] = th + 6;
            ge[m] = (cui_graph_edge){ e->from, L }; s->seg[m++] = (uint8_t)(st | e->tail << 4);
            ge[m] = (cui_graph_edge){ L, e->to }; s->seg[m++] = (uint8_t)(st | e->head << 2);
        } else {
            ge[m] = (cui_graph_edge){ e->from, e->to }; s->seg[m++] = (uint8_t)(st | e->head << 2 | e->tail << 4);
        }
    }
    bool td = f->dir < 2;
    s->flip = f->dir == 1 || f->dir == 3;
    float gap_layer = td ? 34.0f / 0.6f : 48.0f;   /* the layout takes 0.6 of it top down */
    if (!cui__layered_layout(ui, &s->g, bw, bh, nb, ge, m, td, gap_layer, 24.0f, CUI__MM_MAX_BOXES) || s->g.ne != m) return false;
    if (s->flip)
        for (int v = 0; v < s->g.nv; v++) {
            cui__layer_box *b = &s->g.box[v];
            if (td) b->y = s->g.gh - b->y - b->h; else b->x = s->g.gw - b->x - b->w;
        }
    cui__mm_groups(ui, s);
    float x0 = 0, y0 = 0, x1 = s->g.gw, y1 = s->g.gh;
    for (int k = 0; k < s->ng; k++) {
        const cui__mm_group *G = &s->group[k];
        if (!G->shown) continue;
        x0 = CUI_MIN(x0, G->x); y0 = CUI_MIN(y0, G->y); x1 = CUI_MAX(x1, G->x + G->w); y1 = CUI_MAX(y1, G->y + G->h);
    }
    s->x0 = x0; s->y0 = y0; s->bw = x1 - x0; s->bh = y1 - y0;
    return true;
}

/* ------------------------------------------------------------------------ */
/* Pie and gantt parsers                                                    */
/* ------------------------------------------------------------------------ */

/* a plain decimal number: digits, optionally a point and more digits */
static bool cui__mm_number(const char *s, int a, int b, double *out) {
    double v = 0, scale = 1;
    bool any = false, frac = false;
    for (int i = a; i < b; i++) {
        char c = s[i];
        if (c == '.' && !frac) { frac = true; continue; }
        if (c < '0' || c > '9') return false;
        any = true;
        if (frac) { scale *= 0.1; v += (c - '0') * scale; }
        else v = v * 10 + (c - '0');
        if (v > 1e15) return false;
    }
    *out = v;
    return any;
}

static bool cui__mm_parse_pie(cui_ctx *ui, cui__mm_state *s, const char *src, int n, int rest, int next) {
    cui__mm_pool P;
    P.cap = n + 8 * CUI__MM_MAX_SLICES + 16;
    P.s = cui__frame_alloc(ui, (size_t)P.cap);
    float *vals = cui__frame_alloc(ui, sizeof(float) * CUI__MM_MAX_SLICES);
    int *names = cui__frame_alloc(ui, sizeof(int) * CUI__MM_MAX_SLICES), nv = 0, title = -1;
    bool show = false;
    if (!P.s || !vals || !names) return false;
    P.s[0] = 0; P.used = 1;
    int e = rest;
    while (e < n && src[e] != '\n') e++;
    int pos = next, a = rest, b = e;
    cui__mm_trim(src, &a, &b);
    for (bool header = true;; header = false) {
        if (!header && !cui__mm_line(src, n, &pos, &a, &b)) break;
        if (cui__mm_word(src, a, b, "showData")) { show = true; a = cui__mm_skip(src, a + 8, b); }
        if (a >= b || (b - a >= 2 && src[a] == '%' && src[a + 1] == '%')) continue;
        if (cui__mm_word(src, a, b, "title")) { title = cui__mm_put_label(&P, src + a + 5, b - a - 5); continue; }
        if (cui__mm_word(src, a, b, "accTitle") || cui__mm_word(src, a, b, "accDescr")) continue;
        if (header || src[a] != '"') return false;
        int q = a + 1;
        while (q < b && src[q] != '"') q++;
        if (q >= b) return false;
        int c = cui__mm_skip(src, q + 1, b);
        if (c >= b || src[c] != ':' || nv >= CUI__MM_MAX_SLICES) return false;
        int va = c + 1, vb = b;
        cui__mm_trim(src, &va, &vb);
        double v;
        if (!cui__mm_number(src, va, vb, &v)) return false;
        names[nv] = cui__mm_put(&P, src + a + 1, q - a - 1);
        if (show) {   /* the value after the name, as mermaid's showData does */
            int at = P.used - 1, room = P.cap - P.used;
            int k = snprintf(P.s + at, (size_t)CUI_MAX(0, room + 1), " [%.*s]", vb - va, src + va);
            if (k > 0 && k <= room) P.used += k;
        }
        vals[nv++] = (float)v;
    }
    if (nv == 0) return false;
    s->pool = cui__alloc(ui, (size_t)P.used);
    s->values = cui__alloc(ui, sizeof(float) * (size_t)nv);
    s->names = cui__alloc(ui, sizeof(char *) * (size_t)nv);
    if (!s->pool || !s->values || !s->names) return false;
    memcpy(s->pool, P.s, (size_t)P.used);
    for (int k = 0; k < nv; k++) { s->values[k] = vals[k]; s->names[k] = s->pool + names[k]; }
    s->nval = nv;
    s->title = title;
    return true;
}

/* YYYY-MM-DD as a day number */
static bool cui__mm_date(const char *s, int a, int b, int *day) {
    if (b - a != 10 || s[a + 4] != '-' || s[a + 7] != '-') return false;
    int v[3] = { 0, 0, 0 }, at[3] = { 0, 5, 8 }, len[3] = { 4, 2, 2 };
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < len[k]; j++) {
            char c = s[a + at[k] + j];
            if (c < '0' || c > '9') return false;
            v[k] = v[k] * 10 + (c - '0');
        }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) return false;
    cui_date d = { v[0], v[1], v[2] };
    *day = cui_date_to_days(d);
    return true;
}

typedef struct { int name, id, start, end, after; uint8_t done, active, crit, milestone; } cui__mm_task;

static int cui__mm_task_id(const cui__mm_pool *P, const cui__mm_task *t, int nt, const char *s, int len) {
    for (int k = 0; k < nt; k++)
        if (t[k].id > 0 && !strncmp(P->s + t[k].id, s, (size_t)len) && P->s[t[k].id + len] == 0) return k;
    return -1;
}

/* "after a b": the latest end of those tasks; *first the first of them */
static bool cui__mm_after(const cui__mm_pool *P, const cui__mm_task *t, int nt, const char *s, int a, int b, int *day, int *first) {
    int i = a + 5, best = -2147483647;
    *first = -1;
    for (;;) {
        i = cui__mm_skip(s, i, b);
        if (i >= b) break;
        int e = i;
        while (e < b && !cui__mm_sp(s[e])) e++;
        int k = cui__mm_task_id(P, t, nt, s + i, e - i);
        if (k < 0) return false;
        if (*first < 0) *first = k;
        best = CUI_MAX(best, t[k].end);
        i = e;
    }
    if (*first < 0) return false;
    *day = best;
    return true;
}

/* a task's start: a date or "after ids" */
static bool cui__mm_task_start(const cui__mm_pool *P, const cui__mm_task *t, int nt, const char *s, int a, int b, int *day, int *dep) {
    *dep = -1;
    if (cui__mm_word(s, a, b, "after")) return cui__mm_after(P, t, nt, s, a, b, day, dep);
    return cui__mm_date(s, a, b, day);
}

/* a task's end: a date, a length (3d, 2w, 12h) or "until id" */
static bool cui__mm_task_end(const cui__mm_pool *P, const cui__mm_task *t, int nt, const char *s, int a, int b, int start, int *day) {
    if (cui__mm_date(s, a, b, day)) return true;
    if (cui__mm_word(s, a, b, "until")) {
        int i = cui__mm_skip(s, a + 5, b);
        int k = cui__mm_task_id(P, t, nt, s + i, b - i);
        if (k < 0) return false;
        *day = t[k].start;
        return true;
    }
    if (b - a < 2) return false;
    char unit = s[b - 1];
    double v;
    if (!cui__mm_number(s, a, b - 1, &v) || v > CUI__MM_MAX_DAYS * 24.0) return false;
    double days = unit == 'd' ? v : unit == 'w' ? v * 7 : unit == 'h' ? v / 24 : -1;
    if (days < 0) return false;
    *day = start + (int)ceil(days);
    return true;
}

static bool cui__mm_parse_gantt(cui_ctx *ui, cui__mm_state *s, const char *src, int n, int rest, int next) {
    cui__mm_pool P;
    P.cap = n * 2 + 16;
    P.s = cui__frame_alloc(ui, (size_t)P.cap);
    cui__mm_task *t = cui__frame_alloc(ui, sizeof(cui__mm_task) * CUI__MM_MAX_TASKS);
    if (!P.s || !t) return false;
    P.s[0] = 0; P.used = 1;
    int nt = 0, title = -1, pos = next, a, b;
    {   /* nothing may follow the header word */
        int e = rest;
        while (e < n && src[e] != '\n') e++;
        if (cui__mm_skip(src, rest, e) < e) return false;
    }
    static const char *const ignore[] = { "axisFormat", "tickInterval", "excludes", "includes", "todayMarker", "weekday", "weekend",
                                          "inclusiveEndDates", "topAxis", "displayMode", "accTitle", "accDescr", "section", "click" };
    while (cui__mm_line(src, n, &pos, &a, &b)) {
        if (a >= b || (b - a >= 2 && src[a] == '%' && src[a + 1] == '%')) continue;
        if (cui__mm_word(src, a, b, "title")) { title = cui__mm_put_label(&P, src + a + 5, b - a - 5); continue; }
        if (cui__mm_word(src, a, b, "dateFormat")) {
            int i = cui__mm_skip(src, a + 10, b);
            if (b - i != 10 || !cui__mm_word(src, i, b, "YYYY-MM-DD")) return false;
            continue;
        }
        bool skip = false;
        for (int k = 0; k < CUI_COUNTOF(ignore) && !skip; k++) skip = cui__mm_word(src, a, b, ignore[k]);
        if (skip) continue;
        /* a task: name : tags, id, start, end */
        int colon = a;
        while (colon < b && src[colon] != ':') colon++;
        if (colon >= b || nt >= CUI__MM_MAX_TASKS) return false;
        cui__mm_task *T = &t[nt];
        memset(T, 0, sizeof *T);
        T->name = cui__mm_put_label(&P, src + a, colon - a);
        int ia[8], ib[8], items = 0;
        for (int i = colon + 1; i <= b; ) {
            int e = i;
            while (e < b && src[e] != ',') e++;
            if (items >= 8) return false;
            ia[items] = i; ib[items] = e;
            cui__mm_trim(src, &ia[items], &ib[items]);
            items++;
            i = e + 1;
        }
        int it = 0;
        for (; it < items; it++) {
            int x = ia[it], y = ib[it];
            if (cui__mm_word(src, x, y, "done") && y - x == 4) T->done = 1;
            else if (cui__mm_word(src, x, y, "active") && y - x == 6) T->active = 1;
            else if (cui__mm_word(src, x, y, "crit") && y - x == 4) T->crit = 1;
            else if (cui__mm_word(src, x, y, "milestone") && y - x == 9) T->milestone = 1;
            else break;
        }
        int r = items - it, start = 0, end = 0, dep = -1, prev_end = nt > 0 ? t[nt - 1].end : 0;
        int *A = ia + it, *B = ib + it;
        int si = -1, ei;
        if (r == 3) { T->id = cui__mm_put(&P, src + A[0], B[0] - A[0]); si = 1; ei = 2; }
        else if (r == 2) {
            int d;
            if (cui__mm_date(src, A[0], B[0], &d) || cui__mm_word(src, A[0], B[0], "after")) { si = 0; ei = 1; }
            else { T->id = cui__mm_put(&P, src + A[0], B[0] - A[0]); ei = 1; }
        } else if (r == 1) ei = 0;
        else return false;
        if (si >= 0) { if (!cui__mm_task_start(&P, t, nt, src, A[si], B[si], &start, &dep)) return false; }
        else if (nt > 0) start = prev_end;
        else return false;
        if (!cui__mm_task_end(&P, t, nt, src, A[ei], B[ei], start, &end) || end < start) return false;
        if (T->milestone) end = start;
        else if (end == start) end = start + 1;
        T->start = start; T->end = end; T->after = dep;
        nt++;
    }
    if (nt == 0) return false;
    int lo = t[0].start, hi = t[0].end;
    for (int k = 1; k < nt; k++) { lo = CUI_MIN(lo, t[k].start); hi = CUI_MAX(hi, t[k].end); }
    if (hi - lo > CUI__MM_MAX_DAYS) return false;
    s->pool = cui__alloc(ui, (size_t)P.used);
    s->tasks = cui__alloc(ui, sizeof(cui_gantt_task) * (size_t)nt);
    if (!s->pool || !s->tasks) return false;
    memcpy(s->pool, P.s, (size_t)P.used);
    for (int k = 0; k < nt; k++) {
        cui_gantt_task *g = &s->tasks[k];
        memset(g, 0, sizeof *g);
        g->name = s->pool + t[k].name;
        g->start = t[k].start - lo;
        g->end = t[k].end - lo;
        g->progress = t[k].done ? 1.0f : 0.0f;
        g->state = t[k].done ? CUI_GANTT_DONE : t[k].active ? CUI_GANTT_ACTIVE : CUI_GANTT_PLANNED;
        g->tone = t[k].crit ? CUI_TONE_DANGER : CUI_TONE_NEUTRAL;
        g->milestone = t[k].milestone != 0;
        g->depends_on = t[k].after;
    }
    s->ntasks = nt;
    s->origin = cui_date_from_days(lo);
    s->days = hi - lo + 1;
    s->title = title;
    return true;
}

static void cui__mm_parse(cui_ctx *ui, cui__mm_state *s, const char *src, int n) {
    cui__mm_release(ui, s);
    s->title = -1;
    int rest = 0, next = 0, kind = cui__mm_kind(src, n, &rest, &next);
    bool ok = false;
    if (kind == CUI__MM_FLOW) {
        cui__mm_flow f;
        ok = cui__mm_parse_flow(ui, &f, src, n, rest, next) && cui__mm_build_flow(ui, s, &f);
    } else if (kind == CUI__MM_PIE) ok = cui__mm_parse_pie(ui, s, src, n, rest, next);
    else if (kind == CUI__MM_GANTT) ok = cui__mm_parse_gantt(ui, s, src, n, rest, next);
    if (!ok) { cui__mm_release(ui, s); return; }
    s->kind = kind;
}

/* ------------------------------------------------------------------------ */
/* Flowchart view                                                           */
/* ------------------------------------------------------------------------ */

typedef struct {
    const cui__mm_state *s;
    float px, py, zoom;
    cui_color edge, node_fill, node_edge, text, label_text, panel, group_fill, group_edge, group_text;
} cui__mm_paint;

static cui_vec2 cui__mm_pt(const cui__mm_paint *p, cui_rect r, float x, float y) {
    return (cui_vec2){ r.x + p->px + (x - p->s->x0) * p->zoom, r.y + p->py + (y - p->s->y0) * p->zoom };
}

/* where an edge leaves (out) or enters a box: the middle of the side along the layers */
static cui_vec2 cui__mm_port(const cui__mm_paint *p, cui_rect r, int v, bool out) {
    const cui__mm_state *s = p->s;
    const cui__layer_box *b = &s->g.box[v];
    bool far_side = out != s->flip, td = s->g.top_down;
    float x = b->x + (td ? b->w * 0.5f : far_side ? b->w : 0);
    float y = b->y + (td ? (far_side ? b->h : 0) : b->h * 0.5f);
    return cui__mm_pt(p, r, x, y);
}

static void cui__mm_dashed(cui_ctx *ui, const cui_vec2 *pt, int n, float th, cui_color c, float dash, float gap) {
    bool on = true;
    float left = dash;
    for (int i = 0; i + 1 < n; i++) {
        float dx = pt[i + 1].x - pt[i].x, dy = pt[i + 1].y - pt[i].y, len = sqrtf(dx * dx + dy * dy), t = 0;
        if (len <= 1e-4f) continue;
        while (len - t > 1e-3f) {
            float step = CUI_MIN(left, len - t);
            if (on) cui_draw_line(ui, (cui_vec2){ pt[i].x + dx * t / len, pt[i].y + dy * t / len },
                                  (cui_vec2){ pt[i].x + dx * (t + step) / len, pt[i].y + dy * (t + step) / len }, th, c);
            t += step; left -= step;
            if (left <= 1e-3f) { on = !on; left = on ? dash : gap; }
        }
    }
}

static void cui__mm_stroke(cui_ctx *ui, const cui_vec2 *pt, int n, float th, cui_color c, int style, float z) {
    if (style == CUI__MML_DOTTED) cui__mm_dashed(ui, pt, n, th, c, 4 * z + 1, 4 * z + 1);
    else cui_draw_polyline(ui, pt, n, th, c, false);
}

/* an end of an edge at tip, pointing along (dx, dy) */
static void cui__mm_end(cui_ctx *ui, int head, cui_vec2 tip, float dx, float dy, float a, cui_color c, cui_color fill, float th) {
    float nx = -dy, ny = dx;
    if (head == CUI__MMH_ARROW) {
        cui_vec2 tri[3] = { tip, { tip.x - dx * a + nx * a * 0.55f, tip.y - dy * a + ny * a * 0.55f }, { tip.x - dx * a - nx * a * 0.55f, tip.y - dy * a - ny * a * 0.55f } };
        cui_draw_polygon(ui, tri, 3, c);
    } else if (head == CUI__MMH_CIRCLE) {
        cui_vec2 m = { tip.x - dx * a * 0.5f, tip.y - dy * a * 0.5f };
        cui_draw_circle(ui, m, a * 0.5f, c);
        cui_draw_circle(ui, m, a * 0.5f - th, fill);
    } else if (head == CUI__MMH_CROSS) {
        cui_vec2 m = { tip.x - dx * a * 0.6f, tip.y - dy * a * 0.6f };
        float k = a * 0.4f;
        cui_draw_line(ui, (cui_vec2){ m.x - k * (dx + nx), m.y - k * (dy + ny) }, (cui_vec2){ m.x + k * (dx + nx), m.y + k * (dy + ny) }, th, c);
        cui_draw_line(ui, (cui_vec2){ m.x - k * (dx - nx), m.y - k * (dy - ny) }, (cui_vec2){ m.x + k * (dx - nx), m.y + k * (dy - ny) }, th, c);
    }
}

static void cui__mm_paint_edges(cui_ctx *ui, const cui__mm_paint *p, cui_rect r) {
    const cui__mm_state *s = p->s;
    bool td = s->g.top_down;
    float z = p->zoom, sign = s->flip ? -1.0f : 1.0f;
    float ax = td ? 0 : sign, ay = td ? sign : 0;   /* the layers' direction on screen */
    float a = 8 * CUI_MAX(z, 0.6f);
    for (int k = 0; k < s->g.ne; k++) {
        int style = s->seg[k] & 3, head = (s->seg[k] >> 2) & 3, tail = (s->seg[k] >> 4) & 3;
        if (style == CUI__MML_HIDDEN) continue;
        float th = style == CUI__MML_THICK ? 2.75f : 1.35f;
        int from = s->g.from[k], to = s->g.to[k], prev = from;
        for (int w = 0; w <= s->g.rlen[k]; w++) {
            int nxt = w < s->g.rlen[k] ? s->g.route[s->g.rstart[k] + w] : to;
            cui_vec2 s0 = cui__mm_port(p, r, prev, true), e0 = cui__mm_port(p, r, nxt, false);
            /* the line stops where an end's mark starts */
            if (prev == from && tail) { s0.x += ax * (a - 1); s0.y += ay * (a - 1); }
            if (nxt == to && head) { e0.x -= ax * (a - 1); e0.y -= ay * (a - 1); }
            float bend = (td ? fabsf(e0.y - s0.y) : fabsf(e0.x - s0.x)) * 0.5f + 4 * z;
            cui_vec2 c1 = { s0.x + ax * bend, s0.y + ay * bend }, c2 = { e0.x - ax * bend, e0.y - ay * bend }, pts[17];
            for (int i = 0; i <= 16; i++) {
                float t = i / 16.0f, u = 1 - t;
                pts[i].x = u * u * u * s0.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * e0.x;
                pts[i].y = u * u * u * s0.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * e0.y;
            }
            cui__mm_stroke(ui, pts, 17, th, p->edge, style, z);
            if (nxt != to) {   /* straight across a waypoint's layer */
                cui_vec2 across[2] = { e0, cui__mm_port(p, r, nxt, true) };
                cui__mm_stroke(ui, across, 2, th, p->edge, style, z);
            }
            prev = nxt;
        }
        if (head) cui__mm_end(ui, head, cui__mm_port(p, r, to, false), ax, ay, a, p->edge, p->panel, 1.35f);
        if (tail) cui__mm_end(ui, tail, cui__mm_port(p, r, from, true), -ax, -ay, a, p->edge, p->panel, 1.35f);
    }
}

/* lines of t centered on (cx, cy) */
static void cui__mm_text(cui_ctx *ui, const char *t, float cx, float cy, float fs, cui_color c) {
    if (fs < 4) return;
    float lh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
    int lines = 1;
    for (const char *q = t; *q; q++) lines += *q == '\n';
    float y = cy - lines * lh * 0.5f;
    for (const char *q = t;; ) {
        const char *e = strchr(q, '\n');
        int len = e ? (int)(e - q) : (int)strlen(q);
        float w = cui__text_width(ui, CUI_FONT_BODY, fs, q, len, false);
        cui__draw_text_line(ui, CUI_FONT_BODY, fs, cx - w * 0.5f, y, c, q, len, false);
        y += lh;
        if (!e) break;
        q = e + 1;
    }
}

static void cui__mm_shape(cui_ctx *ui, const cui__mm_paint *p, int shape, cui_rect b) {
    cui_color fill = p->node_fill, edge = p->node_edge;
    float z = p->zoom, cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
    cui_vec2 pt[24];
    int np = 0;
    switch (shape) {
    case CUI__MMS_ROUND: cui_draw_rect_border(ui, b, CUI_CORNERS(10 * z), fill, edge, 1.25f); return;
    case CUI__MMS_STADIUM: cui_draw_rect_border(ui, b, CUI_CORNERS(b.h * 0.5f), fill, edge, 1.25f); return;
    case CUI__MMS_CIRCLE: cui_draw_circle(ui, (cui_vec2){ cx, cy }, b.w * 0.5f, edge); cui_draw_circle(ui, (cui_vec2){ cx, cy }, b.w * 0.5f - 1.25f, fill); return;
    case CUI__MMS_SUB:
        cui_draw_rect_border(ui, b, CUI_CORNERS(3 * z), fill, edge, 1.25f);
        cui_draw_line(ui, (cui_vec2){ b.x + 7 * z, b.y }, (cui_vec2){ b.x + 7 * z, b.y + b.h }, 1.25f, edge);
        cui_draw_line(ui, (cui_vec2){ b.x + b.w - 7 * z, b.y }, (cui_vec2){ b.x + b.w - 7 * z, b.y + b.h }, 1.25f, edge);
        return;
    case CUI__MMS_DB: {   /* a cylinder: a body between two ellipses, the top one whole */
        float ry = 6 * z, rx = b.w * 0.5f;
        cui_vec2 body[24], top[24];
        int nb = 0;
        for (int i = 0; i <= 10; i++) { float t = CUI_PI * i / 10; body[nb++] = (cui_vec2){ cx + cosf(t) * rx, b.y + ry - sinf(t) * ry }; }
        for (int i = 0; i <= 10; i++) { float t = CUI_PI * i / 10; body[nb++] = (cui_vec2){ cx - cosf(t) * rx, b.y + b.h - ry + sinf(t) * ry }; }
        cui_draw_polygon(ui, body, nb, fill);
        cui_draw_polyline(ui, body, nb, 1.25f, edge, true);
        for (int i = 0; i < 20; i++) { float t = 2 * CUI_PI * i / 20; top[i] = (cui_vec2){ cx + cosf(t) * rx, b.y + ry + sinf(t) * ry }; }
        cui_draw_polyline(ui, top, 20, 1.25f, edge, true);
        return;
    }
    case CUI__MMS_DIAMOND:
        pt[0] = (cui_vec2){ cx, b.y }; pt[1] = (cui_vec2){ b.x + b.w, cy }; pt[2] = (cui_vec2){ cx, b.y + b.h }; pt[3] = (cui_vec2){ b.x, cy }; np = 4;
        break;
    case CUI__MMS_HEX: {
        float k = b.h * 0.25f;
        pt[0] = (cui_vec2){ b.x + k, b.y }; pt[1] = (cui_vec2){ b.x + b.w - k, b.y }; pt[2] = (cui_vec2){ b.x + b.w, cy };
        pt[3] = (cui_vec2){ b.x + b.w - k, b.y + b.h }; pt[4] = (cui_vec2){ b.x + k, b.y + b.h }; pt[5] = (cui_vec2){ b.x, cy }; np = 6;
        break;
    }
    case CUI__MMS_FLAG: {
        float k = b.h * 0.35f;
        pt[0] = (cui_vec2){ b.x, b.y }; pt[1] = (cui_vec2){ b.x + b.w - k, b.y }; pt[2] = (cui_vec2){ b.x + b.w, cy };
        pt[3] = (cui_vec2){ b.x + b.w - k, b.y + b.h }; pt[4] = (cui_vec2){ b.x, b.y + b.h }; np = 5;
        break;
    }
    default: cui_draw_rect_border(ui, b, CUI_CORNERS(4 * z), fill, edge, 1.25f); return;
    }
    cui_draw_polygon(ui, pt, np, fill);
    cui_draw_polyline(ui, pt, np, 1.25f, edge, true);
}

static void cui__paint_mermaid(cui_ctx *ui, cui_node *n, void *data) {
    const cui__mm_paint *p = data;
    const cui__mm_state *s = p->s;
    cui_rect r = n->rect;
    float z = p->zoom;
    cui_push_clip(ui, cui__rect_inset(r, 1));
    float gfs = s->lfs * z;
    for (int k = 0; k < s->ng; k++) {
        const cui__mm_group *G = &s->group[k];
        if (!G->shown) continue;
        cui_vec2 o = cui__mm_pt(p, r, G->x, G->y);
        cui_rect gr = { o.x, o.y, G->w * z, G->h * z };
        cui_draw_rect_border(ui, gr, CUI_CORNERS(8 * z), p->group_fill, p->group_edge, 1);
        if (gfs >= 4) cui__draw_text_ellipsis(ui, CUI_FONT_MEDIUM, gfs, gr.x + 10 * z, gr.y + 6 * z, gr.w - 20 * z, p->group_text, s->pool + G->title, -1, false);
    }
    cui__mm_paint_edges(ui, p, r);
    for (int v = 0; v < s->nn + s->nlab; v++) {
        const cui__layer_box *b = &s->g.box[v];
        cui_vec2 o = cui__mm_pt(p, r, b->x, b->y);
        cui_rect br = { o.x, o.y, b->w * z, b->h * z };
        if (br.x > r.x + r.w || br.y > r.y + r.h || br.x + br.w < r.x || br.y + br.h < r.y) continue;
        if (v < s->nn) {
            cui__mm_shape(ui, p, s->node[v].shape, br);
            float dy = s->node[v].shape == CUI__MMS_DB ? 4 * z : 0;
            cui__mm_text(ui, s->pool + s->node[v].label, br.x + br.w * 0.5f, br.y + br.h * 0.5f + dy, s->fs * z, p->text);
        } else {
            cui_draw_rect(ui, br, CUI_CORNERS(4 * z), p->panel);
            cui__mm_text(ui, s->pool + s->lab[v - s->nn], br.x + br.w * 0.5f, br.y + br.h * 0.5f, s->lfs * z, p->label_text);
        }
    }
    cui_pop_clip(ui);
}

/* the scale that fits the width (but not below CUI__MM_MIN_ZOOM), and a height up to CUI__MM_MAX_H */
static void cui__mm_fit(cui__mm_state *s, float W) {
    s->fit_w = W;
    float z = s->bw > 0 ? (W - 2 * CUI__MM_PAD) / s->bw : 1;
    s->zoom = CUI_CLAMP(z, CUI__MM_MIN_ZOOM, 1.0f);
    s->panel_h = CUI_MIN(s->bh * s->zoom + 2 * CUI__MM_PAD, CUI__MM_MAX_H);
    s->wheel = s->bh * s->zoom + 2 * CUI__MM_PAD > CUI__MM_MAX_H + 0.5f;
    s->pan = s->wheel || s->bw * s->zoom + 2 * CUI__MM_PAD > W + 0.5f;
    s->px = s->py = 1e9f;   /* the top left corner, once clamped */
}
static void cui__mm_clamp(cui__mm_state *s, float W) {
    float cw = s->bw * s->zoom, ch = s->bh * s->zoom, P = CUI__MM_PAD;
    s->px = cw + 2 * P <= W ? (W - cw) * 0.5f : CUI_CLAMP(s->px, W - P - cw, P);
    s->py = ch + 2 * P <= s->panel_h ? (s->panel_h - ch) * 0.5f : CUI_CLAMP(s->py, s->panel_h - P - ch, P);
}

static void cui__paint_mm_wheel(cui_ctx *ui, cui_node *n, void *data) { cui__paint_wheel_hit(ui, *(cui_id *)data, n->rect); }

static void cui__mm_flow_view(cui_ctx *ui, cui_id id, cui__mm_state *s, float *natural) {
    bool has = cui__has_prev_rect(ui, id);
    cui_rect R = cui__prev_rect(ui, id);
    float W = has && R.w > 0 ? R.w : 600;
    if (!has) ui->animating = true;   /* fit once the width is known */
    if (fabsf(W - s->fit_w) > 0.5f) cui__mm_fit(s, W);
    *natural = s->bw + 2 * CUI__MM_PAD;
    cui_id wid = id ^ 0x3E7B2u;
    /* a diagram bigger than its panel pans with a drag, and with the wheel when taller */
    if (s->pan && has) {
        cui_interact bi = cui__interact(ui, id, CUI_IF_DRAG);
        if (s->wheel && ui->scroll_target == wid && (ui->scroll_dy != 0 || ui->scroll_dx != 0)) {
            if (ui->mods & CUI_MOD_PRIMARY) {
                float nz = CUI_CLAMP(s->zoom * powf(1.12f, ui->scroll_dy), 0.3f, 2.0f);
                float mx = ui->mx - R.x, my = ui->my - R.y;
                float gx = (mx - s->px) / s->zoom, gy = (my - s->py) / s->zoom;
                s->zoom = nz; s->px = mx - gx * nz; s->py = my - gy * nz;
            } else if (ui->scroll_dx != 0 || (ui->mods & CUI_MOD_SHIFT)) s->px += (ui->scroll_dx != 0 ? ui->scroll_dx : ui->scroll_dy) * 40;
            else s->py += ui->scroll_dy * 40;
            ui->scroll_dx = ui->scroll_dy = 0;
        }
        if (bi.hovered && ui->mpressed[0] && s->mode == 0) { s->mode = 1; s->last_x = ui->mx; s->last_y = ui->my; }
        if (s->mode) {
            if (ui->mdown[0]) {
                s->px += ui->mx - s->last_x; s->py += ui->my - s->last_y;
                s->last_x = ui->mx; s->last_y = ui->my;
                cui__set_cursor(ui, CUI_CURSOR_MOVE);
            } else s->mode = 0;
        }
    }
    cui__mm_clamp(s, W);
    int en = cui__node_new(ui, id);
    cui_node *nd = &ui->nodes[en];
    nd->w = CUI_FILL;
    nd->h = CUI_PX(s->panel_h);
    cui_color panel = ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.45f) : CUI_C(ui, SURFACE_MUTED);
    nd->bg = panel;
    nd->border = 1; nd->border_color = CUI_C(ui, BORDER);
    nd->radius = CUI_CORNERS(ui->theme.radius_panel);
    nd->flags |= CUI_NF_CLIP;
    nd->direction = CUI_DIRECTION_LTR;
    if (s->pan) cui__mark_interactive(ui, en, CUI_IF_DRAG);
    cui__mm_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->s = s;
    p->px = s->px; p->py = s->py; p->zoom = s->zoom;
    p->panel = panel;
    p->edge = cui_mix(CUI_C(ui, TEXT_MUTED), CUI_C(ui, BORDER_STRONG), 0.35f);
    p->node_fill = CUI_C(ui, SURFACE);
    p->node_edge = cui_mix(CUI_C(ui, BORDER_STRONG), cui__emphasis(ui), 0.3f);
    p->text = CUI_C(ui, FOREGROUND);
    p->label_text = CUI_C(ui, TEXT_SECONDARY);
    p->group_fill = cui_alpha(CUI_C(ui, FOREGROUND), 0.035f);
    p->group_edge = CUI_C(ui, BORDER);
    p->group_text = CUI_C(ui, TEXT_SECONDARY);
    nd->paint = cui__paint_mermaid;
    nd->paint_data = p;
    if (s->wheel) {
        cui__open(ui, en);
        int wheel = cui__node_new(ui, 0);
        ui->nodes[wheel].w = CUI_FILL; ui->nodes[wheel].h = CUI_FILL;
        ui->nodes[wheel].paint = cui__paint_mm_wheel;
        cui_id *wp = cui__frame_alloc(ui, sizeof *wp);
        *wp = wid;
        ui->nodes[wheel].paint_data = wp;
        cui__close(ui);
    }
}

/* ------------------------------------------------------------------------ */
/* Pie and gantt views                                                      */
/* ------------------------------------------------------------------------ */

static void cui__mm_title(cui_ctx *ui, const cui__mm_state *s, float body) {
    if (s->title > 0 && s->pool[s->title]) cui__label_text(ui, s->pool + s->title, CUI_FONT_MEDIUM, cui__fs(ui, body), CUI_C(ui, FOREGROUND));
}

static void cui__mm_pie_view(cui_ctx *ui, cui_id id, const char *id_str, cui__mm_state *s, float body, float *natural) {
    int col = cui__node_new(ui, id);
    ui->nodes[col].dir = CUI_COLUMN; ui->nodes[col].w = CUI_FILL; ui->nodes[col].gap = 10;
    cui__open(ui, col);
    cui__mm_title(ui, s, body);
    cui_chart_opts co;
    memset(&co, 0, sizeof co);
    co.height = 180;
    cui_push_id(ui, id_str);
    cui_donut_chart_ex(ui, "pie", s->values, s->names, s->nval, &co);
    cui_pop_id(ui);
    cui__close(ui);
    *natural = 420;
}

static void cui__mm_gantt_view(cui_ctx *ui, cui_id id, const char *id_str, cui__mm_state *s, float body, float *natural) {
    cui_rect R = cui__prev_rect(ui, id);
    float W = cui__has_prev_rect(ui, id) && R.w > 0 ? R.w : 600;
    bool slim = W < 480;
    float list = slim ? 129 : 231;
    /* a milestone's label (name and date) runs past its day: room for it, at the scale that results */
    int days = s->days;
    float dw = 30, fs = cui__fs(ui, ui->theme.text_sm);
    for (int pass = 0; pass < 2; pass++) {
        dw = CUI_CLAMP((W - list - 2) / (float)days, 8.0f, 30.0f);
        for (int k = 0; k < s->ntasks; k++)
            if (s->tasks[k].milestone) {
                float lw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, s->tasks[k].name, -1, false) + 80;
                days = CUI_MAX(days, s->tasks[k].start + 1 + (int)ceilf(lw / dw));
            }
    }
    int col = cui__node_new(ui, id);
    ui->nodes[col].dir = CUI_COLUMN; ui->nodes[col].w = CUI_FILL; ui->nodes[col].gap = 10;
    cui__open(ui, col);
    cui__mm_title(ui, s, body);
    /* the gantt may write to its tasks; it gets a copy */
    cui_gantt_task *t = cui__frame_alloc(ui, sizeof(cui_gantt_task) * (size_t)s->ntasks);
    memcpy(t, s->tasks, sizeof(cui_gantt_task) * (size_t)s->ntasks);
    cui_gantt_opts go;
    memset(&go, 0, sizeof go);
    go.day_width = dw;
    go.row_height = 34;
    go.read_only = true;
    cui_push_id(ui, id_str);
    cui_gantt_ex(ui, "gantt", t, s->ntasks, s->origin, days, &go);
    cui_pop_id(ui);
    cui__close(ui);
    *natural = 231 + 2 + days * 30.0f;
}

/* ------------------------------------------------------------------------ */
/* Entry from the markdown view                                             */
/* ------------------------------------------------------------------------ */

bool cui__mermaid_block(cui_ctx *ui, const char *id_str, const char *src, int len, float body, float *natural) {
    *natural = 0;
    if (!src || len <= 0 || len > CUI__MM_MAX_SRC) return false;
    cui_id id = cui_make_id(ui, id_str), sid = id ^ 0x3E7A1u;
    cui__mm_state *s = cui_state(ui, sid, sizeof *s);
    cui__state_set_dtor(ui, sid, cui__mm_dtor);
    float fs = cui__fs(ui, body * 0.95f), lfs = cui__fs(ui, body * 0.85f);
    uint32_t key = cui__hash(src, (size_t)len, 0x3E7Au);
    key = cui__hash(&fs, sizeof fs, key);
    if (!s->init || key != s->key) {
        s->init = true; s->key = key;
        s->fs = fs; s->lfs = lfs;
        cui__mm_parse(ui, s, src, len);
        s->fit_w = -1;
    }
    switch (s->kind) {
    case CUI__MM_FLOW: cui__mm_flow_view(ui, id, s, natural); break;
    case CUI__MM_PIE: cui__mm_pie_view(ui, id, id_str, s, body, natural); break;
    case CUI__MM_GANTT: cui__mm_gantt_view(ui, id, id_str, s, body, natural); break;
    default: return false;
    }
    ui->last_id = id;
    return true;
}
