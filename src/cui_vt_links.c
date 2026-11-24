/* The VT screen's links: OSC 8 hyperlinks kept with the cells (CUI_VT_FEATURE_LINKS), links found in the text under the
 * mouse while Ctrl is held (`.links`: URLs, Windows paths, the positions compilers, git and shells print after a path),
 * and the smart double and triple click (`.smart_select`).
 *
 * Storage. A hyperlink is a 16-bit id per cell into a table of the URIs (id 0: none). The two screens get a grid of ids
 * indexed like their cells (by row slot, so scrolling moves ids with the rows for nothing) when a link is first written
 * into them; a row that leaves into the scrollback keeps its ids only if it holds a link, in a ring keyed by the line's
 * number since the first line ever kept (sb_total), and that entry goes when the line is dropped. The table interns each
 * (id parameter, URI) once; when it is full, ids no cell holds any more are freed and reused. A resize frees it all (the
 * cells move). cui_vt.c calls the hooks here only while a table exists, so a screen without links tests one pointer.
 *
 * Detection works on the logical line under the mouse (wrapped rows joined) as UTF-8, with a cell for every byte, and runs
 * only while Ctrl is held, when the cell under the mouse, Ctrl, the screen's text or its scroll position changed. */
#include "cui_internal.h"

#define CUI__VTL_MAX 8192            /* links in the table at once */
#define CUI__VTL_URI 2048            /* the longest URI kept (the OSC buffer holds a little more) */
#define CUI__VTL_HASH 1024
#define CUI__VTL_GROUP 16            /* rows of one wrapped line looked at above and below the mouse */
#define CUI__VTL_TEXT 4096           /* the longest link text the view hands out */

typedef struct { char *uri, *key; uint32_t hash; uint8_t used, mark; } cui__vtl_ent;
typedef struct { uint64_t line; int n; uint16_t *ids; } cui__vtl_sbl;

struct cui__vt_lx {
    uint16_t cur;                    /* the open link (the pen's), 0 none */
    int cols, rows;                  /* the screen's size when the table was made */
    uint16_t *grid[2];               /* ids of the main and alternate screen, by slot; NULL until a link lands there */
    cui__vtl_ent *ent; int ent_n, ent_cap, live, scan;
    uint16_t bucket[CUI__VTL_HASH];  /* the newest id with this hash */
    cui__vtl_sbl *sb; int sb_cap, sb_head, sb_n;   /* scrollback lines with links, oldest first */
    size_t bytes;                    /* URIs and scrollback ids */
};

static int cui__vtl_detections;

/* ------------------------------------------------------------------------ */
/* The table                                                                */
/* ------------------------------------------------------------------------ */

void cui__vt_lk_free(cui_vt *v) {
    cui__vt_lx *x = cui__vt_lk(v);
    if (!x) return;
    for (int i = 0; i < x->ent_n; i++) if (x->ent[i].used) { free(x->ent[i].uri); free(x->ent[i].key); }
    for (int i = 0; i < x->sb_n; i++) free(x->sb[(x->sb_head + i) % x->sb_cap].ids);
    free(x->ent); free(x->sb); free(x->grid[0]); free(x->grid[1]);
    free(x);
    cui__vt_set_lk(v, NULL);
}

void cui__vt_lk_feature(cui_vt *v, bool on) {
    cui__vt_set_lk_on(v, on);
    if (!on) cui__vt_lk_free(v);
}

size_t cui__vt_lk_memory(const cui_vt *v) {
    const cui__vt_lx *x = cui__vt_lk(v);
    if (!x) return 0;
    size_t n = sizeof *x + x->bytes + (size_t)x->ent_cap * sizeof(cui__vtl_ent) + (size_t)x->sb_cap * sizeof(cui__vtl_sbl);
    for (int k = 0; k < 2; k++) if (x->grid[k]) n += (size_t)x->cols * (size_t)x->rows * sizeof(uint16_t);
    return n;
}

static cui__vt_lx *cui__vtl_make(cui_vt *v) {
    cui__vt_lx *x = cui__vt_lk(v);
    if (x) return x;
    x = (cui__vt_lx *)calloc(1, sizeof *x);
    if (!x) return NULL;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    x->cols = in.cols; x->rows = in.rows;
    cui__vt_set_lk(v, x);
    return x;
}

static uint32_t cui__vtl_hash(const char *k, int kn, const char *u, int un) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < kn; i++) h = (h ^ (uint8_t)k[i]) * 16777619u;
    h = (h ^ 1u) * 16777619u;
    for (int i = 0; i < un; i++) h = (h ^ (uint8_t)u[i]) * 16777619u;
    return h;
}

static bool cui__vtl_line_live(const cui__vt_info *in, uint64_t line) { return line < in->sb_total && line + (uint64_t)in->sb_count >= in->sb_total; }

/* scrollback lines that are gone (dropped from the ring, or the history cleared) take their ids with them */
static void cui__vtl_trim(cui__vt_lx *x, const cui__vt_info *in) {
    while (x->sb_n > 0) {
        cui__vtl_sbl *l = &x->sb[x->sb_head];
        if (cui__vtl_line_live(in, l->line)) break;
        x->bytes -= (size_t)l->n * sizeof(uint16_t);
        free(l->ids);
        x->sb_head = (x->sb_head + 1) % x->sb_cap;
        x->sb_n--;
    }
}

/* frees the links no cell holds any more */
static void cui__vtl_gc(cui_vt *v, cui__vt_lx *x) {
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    cui__vtl_trim(x, &in);
    for (int i = 0; i < x->ent_n; i++) x->ent[i].mark = 0;
    size_t cells = (size_t)x->cols * (size_t)x->rows;
    for (int k = 0; k < 2; k++)
        for (size_t i = 0; x->grid[k] && i < cells; i++) if (x->grid[k][i]) x->ent[x->grid[k][i] - 1].mark = 1;
    for (int j = 0; j < x->sb_n; j++) {
        const cui__vtl_sbl *l = &x->sb[(x->sb_head + j) % x->sb_cap];
        for (int i = 0; i < l->n; i++) if (l->ids[i]) x->ent[l->ids[i] - 1].mark = 1;
    }
    if (x->cur) x->ent[x->cur - 1].mark = 1;
    for (int i = 0; i < x->ent_n; i++) {
        cui__vtl_ent *e = &x->ent[i];
        if (!e->used || e->mark) continue;
        x->bytes -= strlen(e->uri) + 1 + (e->key ? strlen(e->key) + 1 : 0);
        free(e->uri); free(e->key);
        e->uri = e->key = NULL; e->used = 0;
        x->live--;
    }
}

static char *cui__vtl_dup(const char *s, int n) {
    char *d = (char *)malloc((size_t)n + 1);
    if (d) { memcpy(d, s, (size_t)n); d[n] = 0; }
    return d;
}

/* the id of (key, uri): the one already kept, or a new one; 0 when the table is full of links still on screen */
static uint16_t cui__vtl_intern(cui_vt *v, cui__vt_lx *x, const char *key, int kn, const char *uri, int un) {
    uint32_t h = cui__vtl_hash(key, kn, uri, un);
    uint16_t id = x->bucket[h % CUI__VTL_HASH];
    if (id && id <= x->ent_n) {
        const cui__vtl_ent *e = &x->ent[id - 1];
        if (e->used && e->hash == h && (int)strlen(e->uri) == un && !memcmp(e->uri, uri, (size_t)un) &&
            (e->key ? (int)strlen(e->key) == kn && !memcmp(e->key, key, (size_t)kn) : kn == 0)) return id;
    }
    int slot = -1;
    if (x->live < x->ent_n) {                               /* a freed entry */
        for (int k = 0; k < x->ent_n; k++) { int i = (x->scan + k) % x->ent_n; if (!x->ent[i].used) { slot = i; break; } }
    }
    if (slot < 0 && x->ent_n == x->ent_cap && x->ent_n >= 64) {       /* full: free what no cell holds before growing */
        int before = x->live;
        cui__vtl_gc(v, x);
        if (before - x->live >= x->ent_n / 4 || x->ent_n == CUI__VTL_MAX)
            for (int i = 0; i < x->ent_n && slot < 0; i++) if (!x->ent[i].used) slot = i;
        if (slot < 0 && x->ent_n == CUI__VTL_MAX) return 0;
    }
    if (slot < 0) {
        if (x->ent_n == x->ent_cap) {
            int cap = x->ent_cap ? x->ent_cap * 2 : 64;
            if (cap > CUI__VTL_MAX) cap = CUI__VTL_MAX;
            cui__vtl_ent *ne = (cui__vtl_ent *)realloc(x->ent, (size_t)cap * sizeof *ne);
            if (!ne) return 0;
            x->ent = ne; x->ent_cap = cap;
        }
        slot = x->ent_n++;
        memset(&x->ent[slot], 0, sizeof x->ent[slot]);
    }
    cui__vtl_ent *e = &x->ent[slot];
    e->uri = cui__vtl_dup(uri, un);
    e->key = kn ? cui__vtl_dup(key, kn) : NULL;
    if (!e->uri || (kn && !e->key)) { free(e->uri); free(e->key); e->uri = e->key = NULL; return 0; }
    e->hash = h; e->used = 1;
    x->live++;
    x->scan = slot + 1;
    x->bytes += (size_t)un + 1 + (kn ? (size_t)kn + 1 : 0);
    id = (uint16_t)(slot + 1);
    x->bucket[h % CUI__VTL_HASH] = id;
    return id;
}

/* ESC ] 8 ; params ; URI ST: the cells written from now on take the link; an empty URI (or one that did not fit) closes it */
void cui__vt_lk_osc(cui_vt *v, const char *arg, int n, bool cut) {
    const char *semi = (const char *)memchr(arg, ';', (size_t)n);
    cui__vt_lx *x = cui__vt_lk(v);
    int un = semi ? n - (int)(semi + 1 - arg) : 0;
    bool ok = semi && un > 0 && un <= CUI__VTL_URI && !cut;
    for (int i = 0; ok && i < un; i++) if ((uint8_t)semi[1 + i] < 0x20 || semi[1 + i] == 0x7F) ok = false;
    if (!ok) { if (x) x->cur = 0; return; }
    const char *key = NULL; int kn = 0;                     /* id=...: cells of one link split by other text (a redrawn prompt) */
    for (const char *p = arg; p < semi;) {
        const char *e = p;
        while (e < semi && *e != ':') e++;
        if (e - p > 3 && !memcmp(p, "id=", 3)) { key = p + 3; kn = (int)(e - p - 3); }
        p = e + 1;
    }
    x = cui__vtl_make(v);
    if (!x) return;
    x->cur = cui__vtl_intern(v, x, key, kn, semi + 1, un);
}

void cui__vt_lk_reset(cui_vt *v) { cui__vt_lx *x = cui__vt_lk(v); if (x) x->cur = 0; }

/* ------------------------------------------------------------------------ */
/* Cells                                                                    */
/* ------------------------------------------------------------------------ */

void cui__vt_lk_put(cui_vt *v, int scr, int slot, int x0, int w) {
    cui__vt_lx *x = cui__vt_lk(v);
    uint16_t *g = x->grid[scr];
    if (!g) {
        if (!x->cur) return;                                /* no link on this screen yet, and none is being written */
        g = x->grid[scr] = (uint16_t *)calloc((size_t)x->cols * (size_t)x->rows, sizeof(uint16_t));
        if (!g) return;
    }
    if (slot >= x->rows || x0 + w > x->cols) return;
    uint16_t *c = g + (size_t)slot * (size_t)x->cols + x0;
    c[0] = x->cur;
    if (w == 2) c[1] = x->cur;
}

void cui__vt_lk_erase(cui_vt *v, int scr, int slot, int x0, int x1) {
    cui__vt_lx *x = cui__vt_lk(v);
    if (!x->grid[scr] || slot >= x->rows) return;
    x1 = CUI_MIN(x1, x->cols);
    if (x1 > x0) memset(x->grid[scr] + (size_t)slot * (size_t)x->cols + x0, 0, sizeof(uint16_t) * (size_t)(x1 - x0));
}

void cui__vt_lk_shift(cui_vt *v, int scr, int slot, int x0, int n) {
    cui__vt_lx *x = cui__vt_lk(v);
    if (!x->grid[scr] || slot >= x->rows || x0 >= x->cols) return;
    uint16_t *r = x->grid[scr] + (size_t)slot * (size_t)x->cols;
    int cols = x->cols;
    if (n > 0) {
        n = CUI_MIN(n, cols - x0);
        memmove(r + x0 + n, r + x0, sizeof *r * (size_t)(cols - x0 - n));
        memset(r + x0, 0, sizeof *r * (size_t)n);
    } else if (n < 0) {
        n = CUI_MIN(-n, cols - x0);
        memmove(r + x0, r + x0 + n, sizeof *r * (size_t)(cols - x0 - n));
        memset(r + cols - n, 0, sizeof *r * (size_t)n);
    }
}

void cui__vt_lk_pushed(cui_vt *v, int slot) {
    cui__vt_lx *x = cui__vt_lk(v);
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    if (in.sb_max == 0 || in.sb_total == 0) return;
    cui__vtl_trim(x, &in);
    if (!x->grid[0] || slot >= x->rows) return;
    const uint16_t *r = x->grid[0] + (size_t)slot * (size_t)x->cols;
    int n = x->cols;
    while (n > 0 && !r[n - 1]) n--;
    if (n == 0) return;
    if (x->sb_n == x->sb_cap) {                              /* grow the ring, oldest first again */
        int cap = x->sb_cap ? x->sb_cap * 2 : 32;
        cui__vtl_sbl *ns = (cui__vtl_sbl *)malloc((size_t)cap * sizeof *ns);
        if (!ns) return;
        for (int i = 0; i < x->sb_n; i++) ns[i] = x->sb[(x->sb_head + i) % x->sb_cap];
        free(x->sb);
        x->sb = ns; x->sb_cap = cap; x->sb_head = 0;
    }
    uint16_t *ids = (uint16_t *)malloc(sizeof(uint16_t) * (size_t)n);
    if (!ids) return;
    memcpy(ids, r, sizeof(uint16_t) * (size_t)n);
    cui__vtl_sbl *l = &x->sb[(x->sb_head + x->sb_n) % x->sb_cap];
    l->line = in.sb_total - 1; l->n = n; l->ids = ids;
    x->sb_n++;
    x->bytes += sizeof(uint16_t) * (size_t)n;
}

/* the link ids of row `abs` (sb_total + y) and how many there are, or NULL */
static const uint16_t *cui__vtl_row_ids(const cui_vt *v, const cui__vt_info *in, int64_t abs, int *n) {
    const cui__vt_lx *x = cui__vt_lk(v);
    *n = 0;
    if (!x) return NULL;
    if (abs >= (int64_t)in->sb_total) {
        int64_t y = abs - (int64_t)in->sb_total;
        if (y >= in->rows || y >= x->rows || !x->grid[in->cur]) return NULL;
        *n = x->cols;
        return x->grid[in->cur] + (size_t)cui__vt_slot(v, in->cur, (int)y) * (size_t)x->cols;
    }
    if (abs < 0 || !cui__vtl_line_live(in, (uint64_t)abs)) return NULL;
    int lo = 0, hi = x->sb_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const cui__vtl_sbl *l = &x->sb[(x->sb_head + mid) % x->sb_cap];
        if (l->line == (uint64_t)abs) { *n = l->n; return l->ids; }
        if (l->line < (uint64_t)abs) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static uint16_t cui__vtl_id_at(const cui_vt *v, const cui__vt_info *in, int64_t abs, int col) {
    int n;
    const uint16_t *ids = cui__vtl_row_ids(v, in, abs, &n);
    return ids && col >= 0 && col < n ? ids[col] : 0;
}

static const char *cui__vtl_uri(const cui_vt *v, uint16_t id) {
    const cui__vt_lx *x = cui__vt_lk(v);
    return x && id && id <= x->ent_n && x->ent[id - 1].used ? x->ent[id - 1].uri : NULL;
}

const char *cui_vt_hyperlink(const cui_vt *v, int col, int row) {
    if (!v || !cui__vt_lk(v)) return NULL;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    return cui__vtl_uri(v, cui__vtl_id_at(v, &in, (int64_t)in.sb_total + row, col));
}

/* ------------------------------------------------------------------------ */
/* Finding links in text                                                    */
/* ------------------------------------------------------------------------ */

static bool cui__vtl_digit(int c) { return c >= '0' && c <= '9'; }
static bool cui__vtl_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool cui__vtl_alnum(int c) { return cui__vtl_alpha(c) || cui__vtl_digit(c); }
static bool cui__vtl_sep(int c) { return c == '\\' || c == '/'; }
static bool cui__vtl_bad(int c) { return c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (c >= 0 && c < 32) || c == 0x7F; }
static bool cui__vtl_tok(int c) { return c > ' ' && c != 0x7F && c != '"' && c != '<' && c != '>' && c != '|' && c != '`'; }
static bool cui__vtl_drive(const char *s, int n) { return n >= 3 && cui__vtl_alpha((uint8_t)s[0]) && s[1] == ':' && cui__vtl_sep(s[2]); }

/* the last path segment ends in .ext: 1 to 10 letters and digits, a letter among them, a name before the dot */
static bool cui__vtl_ext(const char *s, int n) {
    int seg = 0;
    for (int i = 0; i < n; i++) if (cui__vtl_sep(s[i])) seg = i + 1;
    int dot = -1;
    for (int i = n - 1; i > seg; i--) if (s[i] == '.') { dot = i; break; }
    if (dot < 0 || n - dot - 1 < 1 || n - dot - 1 > 10) return false;
    bool letter = false;
    for (int i = dot + 1; i < n; i++) { if (!cui__vtl_alnum((uint8_t)s[i]) && s[i] != '_') return false; letter |= cui__vtl_alpha((uint8_t)s[i]); }
    return letter;
}

/* does s look like a path? `pos`: a compiler position followed it, `quoted`: it stood in quotes */
static bool cui__vtl_pathy(const char *s, int n, bool pos, bool quoted) {
    if (n < 2) return false;
    bool sep = false, bs = false, letter = false;
    for (int i = 0; i < n; i++) {
        int c = (uint8_t)s[i];
        if (cui__vtl_bad(c) && !(quoted && c == ' ')) return false;
        if (c == ':' && i + 2 < n && s[i + 1] == '/' && s[i + 2] == '/') return false;      /* a URL */
        sep |= cui__vtl_sep(c); bs |= c == '\\';
        letter |= cui__vtl_alpha(c) || c >= 128;
    }
    if (cui__vtl_drive(s, n)) return true;                                              /* C:\x, C:/x */
    if (n >= 4 && s[0] == '\\' && s[1] == '\\' && s[2] != '\\') return memchr(s + 3, '\\', (size_t)(n - 3)) != NULL;   /* \\server\share */
    if (s[0] == '/') {                                                                  /* /usr/bin/x, /c/Users/x */
        int k = 0;
        for (int i = 0; i < n; i++) k += s[i] == '/';
        return s[1] != '/' && k >= 2 && cui__vtl_alnum((uint8_t)s[1]);
    }
    if ((s[0] == '.' && cui__vtl_sep(s[1])) || (s[0] == '~' && cui__vtl_sep(s[1])) || (n >= 3 && s[0] == '.' && s[1] == '.' && cui__vtl_sep(s[2]))) return n >= 3;
    if (s[0] == '-' || s[0] == ':' || !letter) return false;                            /* options, dates, versions */
    if (sep) {
        if (cui__vtl_ext(s, n) || cui__vtl_sep(s[n - 1]) || pos) return true;
        if (bs) { int segs = 0; for (int i = 0; i < n; i++) if (!cui__vtl_sep(s[i]) && (i == 0 || cui__vtl_sep(s[i - 1]))) segs++; return segs >= 2; }
        return false;
    }
    return (pos || quoted) && cui__vtl_ext(s, n);
}

static int cui__vtl_num(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n && i < 9; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* `:line`, `:line:col` (and gcc's colon after it) at the end of t[a..e): returns where the path ends (e: no position) */
static int cui__vtl_pos_colon(const char *t, int a, int e, int *line, int *col) {
    int p = e;
    if (p - 1 > a && t[p - 1] == ':') p--;
    int nums[2], nn = 0, q = p;
    while (nn < 2) {
        int d = q;
        while (d > a && cui__vtl_digit((uint8_t)t[d - 1])) d--;
        if (d == q || d - 1 <= a || t[d - 1] != ':' || q - d > 9) break;
        if (d - 1 == a + 1 && cui__vtl_alpha((uint8_t)t[a])) break;                     /* the drive's colon */
        nums[nn++] = cui__vtl_num(t + d, q - d);
        q = d - 1;
    }
    if (!nn) return e;
    if (nn == 2) { *line = nums[1]; *col = nums[0]; } else { *line = nums[0]; *col = 0; }
    return q;
}

/* `(line)`, `(line,col)`, `(line,col,line2,col2)`, `(line,col-col2)` ending at e - 1: returns the index of '(' or -1 */
static int cui__vtl_pos_paren(const char *t, int a, int e, int *line, int *col) {
    if (e - a < 3 || t[e - 1] != ')') return -1;
    int i = e - 2;
    while (i > a && (cui__vtl_digit((uint8_t)t[i]) || t[i] == ',' || t[i] == '-')) i--;
    if (t[i] != '(' || i <= a || !cui__vtl_digit((uint8_t)t[i + 1])) return -1;
    int k = i + 1, d = k;
    while (cui__vtl_digit((uint8_t)t[d])) d++;
    *line = cui__vtl_num(t + k, d - k); *col = 0;
    if (t[d] == ',' && cui__vtl_digit((uint8_t)t[d + 1])) { int d2 = d + 1; while (cui__vtl_digit((uint8_t)t[d2])) d2++; *col = cui__vtl_num(t + d + 1, d2 - d - 1); }
    return i;
}

/* after a path: ` char:N` (PowerShell's "At C:\x.ps1:12 char:5") and `, line N` (Python's File "x.py", line 12) */
static void cui__vtl_pos_after(const char *t, int len, int e, int *line, int *col) {
    if (*line && !*col && e + 6 < len && !memcmp(t + e, " char:", 6) && cui__vtl_digit((uint8_t)t[e + 6])) {
        int d = e + 6; while (d < len && cui__vtl_digit((uint8_t)t[d])) d++;
        *col = cui__vtl_num(t + e + 6, d - e - 6);
    }
    if (!*line && e + 7 < len && !memcmp(t + e, ", line ", 7) && cui__vtl_digit((uint8_t)t[e + 7])) {
        int d = e + 7; while (d < len && cui__vtl_digit((uint8_t)t[d])) d++;
        *line = cui__vtl_num(t + e + 7, d - e - 7);
    }
}

static bool cui__vtl_put(char *buf, int cap, const char *s, int n) {
    if (n <= 0 || n >= cap) return false;
    memcpy(buf, s, (size_t)n); buf[n] = 0;
    return true;
}

static int cui__vtl_hex(int c) { return cui__vtl_digit(c) ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

/* file:///C:/a%20b/c.txt, file://host/share/x, file://localhost/C:/x, file:///home/x: the path, decoded, with its position */
static bool cui__vtl_file_url(const char *u, int n, char *buf, int cap, int *line, int *col) {
    const char *p = u + 7, *end = u + n;
    char host[256]; int hn = 0;
    if (p < end && *p != '/') {
        const char *h = p;
        while (p < end && *p != '/') p++;
        hn = (int)(p - h);
        if (hn >= (int)sizeof host) return false;
        memcpy(host, h, (size_t)hn); host[hn] = 0;
        if (!strcmp(host, "localhost") || !strcmp(host, "LOCALHOST")) hn = 0;
    }
    if (end - p >= 3 && p[0] == '/' && cui__vtl_alpha((uint8_t)p[1]) && p[2] == ':') { p++; hn = 0; }   /* /C:/x: a drive, whatever the host */
    int o = 0;
    if (hn) { if (hn + 3 >= cap) return false; buf[o++] = '\\'; buf[o++] = '\\'; memcpy(buf + o, host, (size_t)hn); o += hn; }
    for (; p < end; p++) {
        int c = (uint8_t)*p;
        if (c == '%' && end - p >= 3 && cui__vtl_hex((uint8_t)p[1]) >= 0 && cui__vtl_hex((uint8_t)p[2]) >= 0) { c = cui__vtl_hex((uint8_t)p[1]) * 16 + cui__vtl_hex((uint8_t)p[2]); p += 2; }
        if (c == 0 || o + 1 >= cap) return false;
        buf[o++] = (char)c;
    }
    buf[o] = 0;
    if (o == 0) return false;
    if (hn || cui__vtl_drive(buf, o)) for (int i = 0; i < o; i++) if (buf[i] == '/') buf[i] = '\\';
    int pe = cui__vtl_pos_colon(buf, 0, o, line, col);
    buf[pe] = 0;
    return pe > 0;
}

static bool cui__vtl_link(cui_vt_link *out, int kind, char *buf, int line, int col) {
    out->kind = kind; out->text = buf; out->line = line; out->col = col; out->osc8 = false;
    return true;
}

static int cui__vtl_url_end(const char *t, int len, int s, int sl) {
    int e = s + sl;
    while (e < len) { int c = (uint8_t)t[e]; if (c <= ' ' || c == 0x7F || c == '"' || c == '<' || c == '>' || c == '`') break; e++; }
    while (e > s + sl) {                                     /* punctuation that ends a sentence, and brackets the URL did not open */
        char c = t[e - 1];
        if (strchr(".,:;!?'*", c)) { e--; continue; }
        if (c == ')' || c == ']' || c == '}') {
            char op = c == ')' ? '(' : c == ']' ? '[' : '{';
            int no = 0, nc = 0;
            for (int k = s; k < e; k++) { no += t[k] == op; nc += t[k] == c; }
            if (nc > no) { e--; continue; }
        }
        break;
    }
    return e;
}

static bool cui__vtl_find_url(const char *t, int len, int at, cui_vt_link *out, char *buf, int cap, int *b0, int *b1) {
    static const char *const sch[] = { "https://", "http://", "file://" };
    for (int i = 0; i <= at && i < len; i++) {
        if (i > 0 && cui__vtl_alnum((uint8_t)t[i - 1])) continue;
        for (int k = 0; k < 3; k++) {
            int sl = (int)strlen(sch[k]);
            if (len - i < sl) continue;
            int m = 0;
            while (m < sl && (t[i + m] | (cui__vtl_alpha((uint8_t)t[i + m]) ? 32 : 0)) == sch[k][m]) m++;
            if (m < sl) continue;
            int e = cui__vtl_url_end(t, len, i, sl);
            if (e <= i + sl || at >= e) break;
            *b0 = i; *b1 = e;
            if (k == 2) { int line = 0, col = 0; if (!cui__vtl_file_url(t + i, e - i, buf, cap, &line, &col)) return false; return cui__vtl_link(out, CUI_VT_LINK_PATH, buf, line, col); }
            if (!cui__vtl_put(buf, cap, t + i, e - i)) return false;
            return cui__vtl_link(out, CUI_VT_LINK_URL, buf, 0, 0);
        }
    }
    return false;
}

/* a path in quotes: "C:\Program Files\x\y.txt", 'C:\a b\c.c', with a position inside or after the quotes */
static bool cui__vtl_find_quoted(const char *t, int len, int at, char q, cui_vt_link *out, char *buf, int cap, int *b0, int *b1) {
    for (int i = 0; i < len;) {
        int q0 = -1, q1 = -1;
        for (; i < len; i++)
            if (t[i] == q && (q == '"' || ((i == 0 || (t[i - 1] && strchr(" \t(=:,[", t[i - 1]))) && i + 1 < len && t[i + 1] != ' '))) { q0 = i; break; }
        if (q0 < 0 || q0 > at) return false;
        for (i = q0 + 1; i < len; i++)
            if (t[i] == q && (q == '"' || i + 1 == len || !cui__vtl_alnum((uint8_t)t[i + 1]))) { q1 = i; break; }
        if (q1 < 0) return false;
        i = q1 + 1;
        if (at > q1) continue;
        int a = q0 + 1, line = 0, col = 0;
        int pe = cui__vtl_pos_colon(t, a, q1, &line, &col);
        if (!line) {
            int lp = 0, cp = 0;
            if (q1 + 1 < len && t[q1 + 1] == '(') {
                int e = q1 + 2;
                while (e < len && t[e] != ')' && e - q1 < 40) e++;
                if (e < len && cui__vtl_pos_paren(t, q1, e + 1, &lp, &cp) == q1 + 1) { line = lp; col = cp; }
            } else if (q1 + 1 < len && t[q1 + 1] == ':') {
                int e = q1 + 2;
                while (e < len && (cui__vtl_digit((uint8_t)t[e]) || t[e] == ':')) e++;
                int p2 = cui__vtl_pos_colon(t, q1, e, &lp, &cp);
                if (p2 == q1 + 1) { line = lp; col = cp; }
            }
            cui__vtl_pos_after(t, len, q1 + 1, &line, &col);
        }
        if (pe - a < 2 || !cui__vtl_pathy(t + a, pe - a, line > 0, true)) continue;
        if (!cui__vtl_put(buf, cap, t + a, pe - a)) return false;
        *b0 = a; *b1 = q1;
        return cui__vtl_link(out, CUI_VT_LINK_PATH, buf, line, col);
    }
    return false;
}

/* An absolute path with spaces that a compiler's position ends (C:\Program Files\x\y.cpp(12): error), or that runs to a
 * prompt's '>' or the end of the line (PS C:\Program Files> , Directory: C:\Program Files\x). The pieces between the
 * spaces may not end in a file extension (that would be a whole path already) and hold no other colon. */
static bool cui__vtl_find_spaced(const char *t, int len, int at, cui_vt_link *out, char *buf, int cap, int *b0, int *b1) {
    int d = -1;
    for (int i = at; i >= 0 && at - i < 400; i--)
        if (cui__vtl_drive(t + i, len - i) && (i == 0 || strchr(" \t'(", t[i - 1]))) { d = i; break; }
    if (d < 0) return false;
    int piece = d, end = -1, sfx = -1, line = 0, col = 0;
    bool space = false;
    for (int i = d + 3; i <= len && i - d < 400; i++) {
        int c = i < len ? (uint8_t)t[i] : 0;
        if (c == '(' && cui__vtl_ext(t + d, i - d)) {
            int e = i + 1;
            while (e < len && t[e] != ')' && e - i < 40) e++;
            if (e < len && cui__vtl_pos_paren(t, i - 1, e + 1, &line, &col) == i) { end = i; sfx = e + 1; break; }
        }
        if (c == ':' && i + 1 < len && cui__vtl_digit((uint8_t)t[i + 1]) && cui__vtl_ext(t + d, i - d)) {
            int e = i + 1;
            while (e < len && (cui__vtl_digit((uint8_t)t[e]) || t[e] == ':')) e++;
            if (cui__vtl_pos_colon(t, d, e, &line, &col) == i) { end = i; sfx = e; while (sfx > i && t[sfx - 1] == ':') sfx--; break; }
        }
        bool prompt = c == '>' && (i + 1 == len || t[i + 1] == ' '), last = i == len || prompt;
        if (!last) for (int k = i; k < len && t[k] == ' '; k++) if (k + 1 == len) last = true;   /* only spaces after it */
        if (last) {                                          /* a prompt's folder, or a header's (Directory: C:\x y) */
            if (!prompt && !(d >= 2 && t[d - 1] == ' ' && t[d - 2] == ':')) return false;
            end = i; while (end > d && t[end - 1] == ' ') end--;
            sfx = end;
            break;
        }
        if (c == ' ') {
            if (i + 1 < len && t[i + 1] == ' ') return false;
            if (cui__vtl_ext(t + piece, i - piece)) return false;
            space = true; piece = i + 1;
        } else if (c == ':' || cui__vtl_bad(c) || c == '\t') return false;
    }
    if (end < 0 || !space || at >= sfx) return false;
    cui__vtl_pos_after(t, len, sfx, &line, &col);
    if (!cui__vtl_put(buf, cap, t + d, end - d)) return false;
    *b0 = d; *b1 = sfx;
    return cui__vtl_link(out, CUI_VT_LINK_PATH, buf, line, col);
}

/* the word around `at` (no spaces, quotes or <>|): src/x.c:12:5, C:\x\y.cpp(12):, (C:\x\app.js:12:5), --file=a/b.txt */
static bool cui__vtl_find_token(const char *t, int len, int at, cui_vt_link *out, char *buf, int cap, int *b0, int *b1) {
    if (!cui__vtl_tok((uint8_t)t[at])) return false;
    int a = at, b = at;
    while (a > 0 && cui__vtl_tok((uint8_t)t[a - 1])) a--;
    while (b < len && cui__vtl_tok((uint8_t)t[b])) b++;
    for (int k = b - 1; k > a; k--) if (t[k] == '=' && k < at) { a = k + 1; break; }
    while (a < b && strchr("([{'", t[a])) a++;
    int line = 0, col = 0, pe = -1;
    for (int pass = 0; pass < 2 && pe < 0; pass++) {
        while (b > a && (strchr(".,;!?'", t[b - 1]) || (t[b - 1] == ':' && b - 2 > a && t[b - 2] == ')'))) b--;   /* x.cpp(12): */
        int k = cui__vtl_pos_paren(t, a, b, &line, &col);
        if (k >= 0) { pe = k; break; }
        if (b > a && strchr(")]}", t[b - 1])) {               /* a bracket it did not open: (C:\x\app.js:12:5) */
            char op = t[b - 1] == ')' ? '(' : t[b - 1] == ']' ? '[' : '{';
            int no = 0, nc = 0;
            for (int i = a; i < b; i++) { no += t[i] == op; nc += t[i] == t[b - 1]; }
            if (nc > no) { b--; continue; }
        }
        break;
    }
    int sfx = b;
    if (pe < 0) {
        pe = cui__vtl_pos_colon(t, a, b, &line, &col);
        if (pe == b) { while (pe > a && strchr(":.", t[pe - 1])) pe--; sfx = pe; }
        else { sfx = b; if (sfx > a && t[sfx - 1] == ':') sfx--; }
    }
    while (pe > a && t[pe - 1] == '.') pe--;
    if (at < a || at >= sfx || !cui__vtl_pathy(t + a, pe - a, line > 0, false)) return false;
    cui__vtl_pos_after(t, len, sfx, &line, &col);
    if (!cui__vtl_put(buf, cap, t + a, pe - a)) return false;
    *b0 = a; *b1 = sfx;
    return cui__vtl_link(out, CUI_VT_LINK_PATH, buf, line, col);
}

bool cui_vt_link_find(const char *text, int len, int at, cui_vt_link *out, char *buf, int cap, int *start, int *end) {
    int b0 = 0, b1 = 0;
    if (!text || !out || !buf || cap < 2 || at < 0 || at >= len) return false;
    memset(out, 0, sizeof *out);
    bool ok = cui__vtl_find_url(text, len, at, out, buf, cap, &b0, &b1) ||
              cui__vtl_find_quoted(text, len, at, '"', out, buf, cap, &b0, &b1) ||
              cui__vtl_find_quoted(text, len, at, '\'', out, buf, cap, &b0, &b1) ||
              cui__vtl_find_spaced(text, len, at, out, buf, cap, &b0, &b1) ||
              cui__vtl_find_token(text, len, at, out, buf, cap, &b0, &b1);
    if (!ok) { memset(out, 0, sizeof *out); return false; }
    if (start) *start = b0;
    if (end) *end = b1;
    return true;
}

/* ------------------------------------------------------------------------ */
/* The text under the mouse                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    int64_t r0; int nrows, cols;     /* the wrapped line: rows r0 .. r0 + nrows - 1 */
    char *text; int len;
    int32_t *cell;                   /* per byte: (row - r0) << 11 | col */
    int at;                          /* the first byte of the cell asked for, -1 when it holds no text */
} cui__vtl_text;

static bool cui__vtl_row_wrapped(const cui_vt *v, int64_t abs, bool *exists) {
    uint32_t dummy; bool w = false;
    int n = cui__vt_row_chars(v, abs, &dummy, 0, &w);
    *exists = n >= 0;
    return n >= 0 && w;
}

static void cui__vtl_text_free(cui__vtl_text *g) { free(g->text); free(g->cell); g->text = NULL; g->cell = NULL; }

/* the logical line around cell (row, col), at most CUI__VTL_GROUP rows above and below */
static bool cui__vtl_gather(const cui_vt *v, int64_t row, int col, cui__vtl_text *g) {
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    memset(g, 0, sizeof *g);
    g->at = -1; g->cols = in.cols;
    bool ex;
    cui__vtl_row_wrapped(v, row, &ex);
    if (!ex) return false;
    int64_t r0 = row, r1 = row;
    while (row - r0 < CUI__VTL_GROUP && cui__vtl_row_wrapped(v, r0 - 1, &ex) && ex) r0--;
    while (r1 - row < CUI__VTL_GROUP && cui__vtl_row_wrapped(v, r1, &ex)) { bool e2; cui__vtl_row_wrapped(v, r1 + 1, &e2); if (!e2) break; r1++; }
    g->r0 = r0; g->nrows = (int)(r1 - r0 + 1);
    size_t cap = (size_t)g->nrows * (size_t)in.cols * 4 + 4;
    g->text = (char *)malloc(cap);
    g->cell = (int32_t *)malloc(cap * sizeof(int32_t));
    uint32_t *cps = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)in.cols);
    if (!g->text || !g->cell || !cps) { free(cps); cui__vtl_text_free(g); return false; }
    for (int k = 0; k < g->nrows; k++) {
        bool w = false;
        int n = cui__vt_row_chars(v, r0 + k, cps, in.cols, &w);
        if (n < 0) n = 0;
        int m = n;
        if (w) m = in.cols;                                  /* a wrapped row runs to its last column */
        else while (m > 0 && (cps[m - 1] == 0 || cps[m - 1] == ' ')) m--;
        for (int x = 0; x < m; x++) {
            uint32_t c = x < n ? cps[x] : 0;
            if (c == CUI__VT_FIND_SPACER) continue;
            if (r0 + k == row && x == col) g->at = g->len;
            char u[4];
            int b = c && c != CUI__VT_FIND_SPACER ? cui__utf8_encode(c, u) : (u[0] = ' ', 1);
            for (int i = 0; i < b; i++) { g->text[g->len] = u[i]; g->cell[g->len] = (int32_t)(k << 11 | x); g->len++; }
        }
        if (r0 + k == row && g->at < 0 && col < m && col > 0 && col < n && cps[col] == CUI__VT_FIND_SPACER) {   /* the right half of a wide character */
            for (int i = g->len - 1; i >= 0; i--) if (g->cell[i] == (int32_t)(k << 11 | (col - 1))) { g->at = i; while (g->at > 0 && g->cell[g->at - 1] == g->cell[i]) g->at--; break; }
        }
    }
    free(cps);
    return true;
}

/* bytes [b0, b1) of the gathered text as cells r0:c0 .. r1:c1 (inclusive) */
static void cui__vtl_span(const cui_vt *v, const cui__vtl_text *g, int b0, int b1, int64_t *r0, int *c0, int64_t *r1, int *c1) {
    int32_t a = g->cell[b0], b = g->cell[b1 - 1];
    *r0 = g->r0 + (a >> 11); *c0 = a & 2047;
    *r1 = g->r0 + (b >> 11); *c1 = b & 2047;
    uint32_t cps[1000];
    bool w;
    int n = cui__vt_row_chars(v, *r1, cps, CUI_MIN(g->cols, 1000), &w);
    if (*c1 + 1 < n && cps[*c1 + 1] == CUI__VT_FIND_SPACER) (*c1)++;
}

/* the cells around (row, col) of the wrapped line that hold the same OSC 8 link: false when the cell holds none */
static bool cui__vtl_osc8_span(const cui_vt *v, const cui__vtl_text *g, int64_t row, int col, uint16_t *id, int64_t *r0, int *c0, int64_t *r1, int *c1) {
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    *id = cui__vtl_id_at(v, &in, row, col);
    if (!*id) return false;
    int64_t ra = row, rb = row; int ca = col, cb = col;
    for (;;) {                                               /* left, into the rows above while they wrap */
        int64_t r = ra; int c = ca - 1;
        if (c < 0) { if (r <= g->r0) break; r--; c = in.cols - 1; }
        if (cui__vtl_id_at(v, &in, r, c) != *id) break;
        ra = r; ca = c;
    }
    for (;;) {
        int64_t r = rb; int c = cb + 1;
        if (c >= in.cols) { if (r >= g->r0 + g->nrows - 1) break; r++; c = 0; }
        if (cui__vtl_id_at(v, &in, r, c) != *id) break;
        rb = r; cb = c;
    }
    *r0 = ra; *c0 = ca; *r1 = rb; *c1 = cb;
    return true;
}

/* an OSC 8 URI as a link: file:// is a path, anything else is handed out as it is */
static void cui__vtl_from_uri(const char *uri, cui_vt_link *out, char *buf, int cap) {
    int n = (int)strlen(uri), line = 0, col = 0;
    memset(out, 0, sizeof *out);
    if (n > 7 && (uri[0] | 32) == 'f' && (uri[1] | 32) == 'i' && (uri[2] | 32) == 'l' && (uri[3] | 32) == 'e' && !memcmp(uri + 4, "://", 3) &&
        cui__vtl_file_url(uri, n, buf, cap, &line, &col)) cui__vtl_link(out, CUI_VT_LINK_PATH, buf, line, col);
    else { if (n >= cap) n = cap - 1; memcpy(buf, uri, (size_t)n); buf[n] = 0; cui__vtl_link(out, CUI_VT_LINK_URL, buf, 0, 0); }
    out->osc8 = true;
}

/* ------------------------------------------------------------------------ */
/* The view                                                                 */
/* ------------------------------------------------------------------------ */

typedef struct {
    bool valid, kctrl;               /* what the last detection was for */
    int64_t krow; int kcol, kcols; uint32_t kseq; uint64_t ktotal;
    cui__vt_lpaint lp;
    cui_vt_link link;
    char text[CUI__VTL_TEXT];
} cui__vtl_state;

static cui__vtl_state *cui__vtl_st(cui_ctx *ui, cui_id id) { return (cui__vtl_state *)cui_state(ui, id ^ 0x6C696E6Bu, sizeof(cui__vtl_state)); }

/* finds the link at (row, col): OSC 8 first, then the text */
static bool cui__vtl_detect(const cui_vt *v, int64_t row, int col, cui__vtl_state *st) {
    cui__vtl_text g;
    cui__vtl_detections++;
    if (!cui__vtl_gather(v, row, col, &g)) return false;
    bool ok = false;
    uint16_t id;
    if (cui__vtl_osc8_span(v, &g, row, col, &id, &st->lp.r0, &st->lp.c0, &st->lp.r1, &st->lp.c1)) {
        const char *uri = cui__vtl_uri(v, id);
        if (uri) { cui__vtl_from_uri(uri, &st->link, st->text, sizeof st->text); ok = true; }
    }
    int b0, b1;
    if (!ok && g.at >= 0 && cui_vt_link_find(g.text, g.len, g.at, &st->link, st->text, sizeof st->text, &b0, &b1) && b1 > b0) {
        cui__vtl_span(v, &g, b0, b1, &st->lp.r0, &st->lp.c0, &st->lp.r1, &st->lp.c1);
        ok = true;
    }
    cui__vtl_text_free(&g);
    return ok;
}

const cui__vt_lpaint *cui__vt_links_hover(cui_ctx *ui, cui_id id, const cui_vt *v, bool hovered, int64_t row, int col, bool ctrl) {
    cui__vtl_state *st = cui__vtl_st(ui, id);
    if (!ctrl || !hovered) { st->lp.on = false; st->valid = false; return &st->lp; }       /* nothing is looked for without Ctrl */
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    uint32_t seq = cui__vt_wseq(v);
    if (!st->valid || st->krow != row || st->kcol != col || st->kseq != seq || st->ktotal != in.sb_total || st->kcols != in.cols || !st->kctrl) {
        st->valid = true; st->kctrl = true; st->krow = row; st->kcol = col; st->kseq = seq; st->ktotal = in.sb_total; st->kcols = in.cols;
        st->lp.on = cui__vtl_detect(v, row, col, st);
    }
    if (st->lp.on) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return &st->lp;
}

const cui_vt_link *cui__vt_links_take(cui_ctx *ui, cui_id id) {
    cui__vtl_state *st = cui__vtl_st(ui, id);
    if (!st->lp.on) return NULL;
    size_t n = strlen(st->text);
    cui_vt_link *l = (cui_vt_link *)cui__frame_alloc(ui, sizeof *l + n + 1);
    if (!l) return NULL;
    *l = st->link;
    char *t = (char *)(l + 1);
    memcpy(t, st->text, n + 1);
    l->text = t;
    return l;
}

static bool cui__vtl_word_byte(uint8_t c) { return c > 32 && c != 0x7F && (c >= 128 || !strchr("()[]{}<>\"'`,;|", (int)c)); }

void cui__vt_smart_word(const cui_vt *v, int64_t row, int col, int64_t *r0, int *c0, int64_t *r1, int *c1) {
    *r0 = *r1 = row; *c0 = *c1 = col;
    cui__vtl_text g;
    if (!cui__vtl_gather(v, row, col, &g)) return;
    uint16_t id;
    if (g.at < 0) { cui__vtl_text_free(&g); return; }
    if (!cui__vtl_osc8_span(v, &g, row, col, &id, r0, c0, r1, c1)) {
        cui_vt_link l;
        char buf[CUI__VTL_TEXT];
        int b0, b1;
        if (cui_vt_link_find(g.text, g.len, g.at, &l, buf, sizeof buf, &b0, &b1) && b1 > b0) cui__vtl_span(v, &g, b0, b1, r0, c0, r1, c1);
        else if (cui__vtl_word_byte((uint8_t)g.text[g.at])) {
            b0 = g.at; b1 = g.at + 1;
            while (b0 > 0 && cui__vtl_word_byte((uint8_t)g.text[b0 - 1])) b0--;
            while (b1 < g.len && cui__vtl_word_byte((uint8_t)g.text[b1])) b1++;
            cui__vtl_span(v, &g, b0, b1, r0, c0, r1, c1);
        }
    }
    cui__vtl_text_free(&g);
}

void cui__vt_smart_line(const cui_vt *v, int64_t row, int64_t *r0, int64_t *r1) {
    bool ex;
    *r0 = *r1 = row;
    cui__vtl_row_wrapped(v, row, &ex);
    if (!ex) return;
    while (row - *r0 < 4096 && cui__vtl_row_wrapped(v, *r0 - 1, &ex) && ex) (*r0)--;
    while (*r1 - row < 4096 && cui__vtl_row_wrapped(v, *r1, &ex)) { bool e2; cui__vtl_row_wrapped(v, *r1 + 1, &e2); if (!e2) break; (*r1)++; }
}

void cui__vt_trim_copy(char *s) {
    size_t o = 0, sp = 0;                                    /* sp: spaces held back until something other than a line break follows */
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t') { sp++; continue; }
        if (c != '\n' && c != '\r') { memmove(s + o, s + i - sp, sp); o += sp; }
        sp = 0;
        s[o++] = c;
    }
    s[o] = 0;
}

static void cui__vtl_rect(cui_ctx *ui, float x0, float y0, float x1, float y1, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    x0 = floorf(x0 * s + 0.5f) / s; x1 = floorf(x1 * s + 0.5f) / s;
    y0 = floorf(y0 * s + 0.5f) / s; y1 = floorf(y1 * s + 0.5f) / s;
    if (x1 > x0 && y1 > y0) cui_draw_rect(ui, (cui_rect){ x0, y0, x1 - x0, y1 - y0 }, CUI_CORNERS(0), col);
}

void cui__vt_links_paint(cui_ctx *ui, const cui_vt *v, const cui__vt_lpaint *lp, int64_t top, int rows, float x0, float y0, float cw, float lh, cui_color col) {
    float s = ui->scale > 0 ? ui->scale : 1;
    float t = fmaxf(1.0f, floorf(lh / 16.0f * s + 0.5f)) / s;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    const cui__vt_lx *x = cui__vt_lk(v);
    for (int y = 0; y < rows; y++) {
        int64_t r = top + y;
        float yb = y0 + (float)y * lh + lh - 1.0f;
        int h0 = 1, h1 = 0;                                  /* the hovered link's cells on this row */
        if (lp && lp->on && r >= lp->r0 && r <= lp->r1) {
            h0 = r == lp->r0 ? lp->c0 : 0; h1 = r == lp->r1 ? lp->c1 : in.cols - 1;
            cui__vtl_rect(ui, x0 + (float)h0 * cw, yb - t, x0 + (float)(h1 + 1) * cw, yb, col);
        }
        int n;
        const uint16_t *ids = x ? cui__vtl_row_ids(v, &in, r, &n) : NULL;
        if (!ids) continue;
        cui_color dim = cui_alpha(col, 0.55f);
        for (int c = 0; c < n && c < in.cols; c++) {         /* OSC 8 cells: dotted, a dot of the line's width every two */
            if ((c >= h0 && c <= h1) || !ids[c]) continue;
            float cx = x0 + (float)c * cw;
            for (float dx = ceilf(cx / (2 * t)) * 2 * t; dx < cx + cw; dx += 2 * t) cui__vtl_rect(ui, dx, yb - t, fminf(dx + t, cx + cw), yb, dim);
        }
    }
}

int cui__vt_links_detections(void) { return cui__vtl_detections; }
