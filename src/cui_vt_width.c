/* The VT screen's characters by Unicode 17.0.0 (CUI_VT_FEATURE_CLUSTERS): how many cells each takes, which ones join
 * into one grapheme cluster (UAX #29: combining marks, variation selectors, emoji modifiers, ZWJ sequences, flags, Hangul
 * syllables, Indic conjuncts), and the table of clusters the cells point into.
 *
 * Widths. A character takes 2 cells when its East Asian Width is W or F, when it is Emoji_Presentation, or when it is an
 * Extended_Pictographic code point not assigned yet (a future emoji); otherwise 1. A cluster takes the cells of its first
 * character, except that VS16 (FE0F) right after a character with an emoji style makes it 2 and VS15 (FE0E) right after an
 * Emoji_Presentation character makes it 1, as Windows Terminal measures graphemes. Ambiguous characters are narrow.
 *
 * Storage. A cell keeps a 21-bit code point; code points from 0x110000 on name a cluster in the table below, so a cell
 * stays 8 bytes and the scrollback, reflow and synchronized-output copies carry clusters for nothing. The table interns
 * each sequence once (up to 16 code points, a hash for lookups). It is allocated at the first cluster; when it is full,
 * the clusters no cell holds any more are freed (cui_vt.c walks every cell) and their ids reused. */
#include "cui_internal.h"
#include "cui_vt_width.h"

#define CUI__VTG_MAX 65536            /* clusters at once */

/* ------------------------------------------------------------------------ */
/* The character table                                                      */
/* ------------------------------------------------------------------------ */

static unsigned cui__vtw_find(uint32_t c, uint32_t *lo_cp, uint32_t *hi_cp) {
    int lo = 0, hi = CUI__VTW_N - 1;
    while (lo < hi) {                                              /* the last run that starts at or before c */
        int mid = (lo + hi + 1) >> 1;
        if ((cui__vtw_table[mid] >> 11) <= c) lo = mid; else hi = mid - 1;
    }
    *lo_cp = cui__vtw_table[lo] >> 11;
    *hi_cp = lo + 1 < CUI__VTW_N ? cui__vtw_table[lo + 1] >> 11 : 0x110000u;
    return cui__vtw_table[lo] & 0x7FFu;
}

unsigned cui__vtw_props(uint32_t c) {
    /* printable ASCII: narrow, Other; but # * and the digits have an emoji style (keycaps) */
    if (c >= 0x20 && c < 0x7F && c != '#' && c != '*' && (c < '0' || c > '9')) return 1u;
    uint32_t a, b;
    return cui__vtw_find(c, &a, &b);
}

/* the same, remembering the run it was in: text in one script stays in one run */
unsigned cui__vtw_props_in(cui__vtw_run *r, uint32_t c) {
    if (c - r->lo < r->n) return r->p;
    uint32_t a, b;
    unsigned p = cui__vtw_find(c, &a, &b);
    r->lo = a; r->n = b - a; r->p = (uint16_t)p;
    return p;
}

/* the cells a cluster's first character takes alone: 0 for one that joins or is invisible (a mark, a joiner, a format
 * character) and so has nothing to show without a character before it */
int cui__vtw_cells(unsigned p) {
    unsigned g = CUI__VTW_GCB(p);
    return g == CUI__VTW_CONTROL || g == CUI__VTW_EXTEND || g == CUI__VTW_ZWJ || g == CUI__VTW_CR || g == CUI__VTW_LF ? 0 : (int)(p & 3u);
}

/* ------------------------------------------------------------------------ */
/* Grapheme cluster boundaries (UAX #29, Unicode 17)                        */
/* ------------------------------------------------------------------------ */

static void cui__vtw_after(cui__vtw_gs *s, unsigned p, bool joined) {
    unsigned g = CUI__VTW_GCB(p), ic = CUI__VTW_INCB(p);
    /* GB11: ExtPict Extend* ZWJ x ExtPict */
    if (p & CUI__VTW_PICT) s->pict = 1;
    else if (joined && s->pict == 1 && g == CUI__VTW_EXTEND) s->pict = 1;
    else if (joined && s->pict == 1 && g == CUI__VTW_ZWJ) s->pict = 2;
    else s->pict = 0;
    /* GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x Consonant */
    if (ic == 2) s->incb = 1;
    else if (joined && s->incb && ic == 1) s->incb = 2;
    else if (!(joined && s->incb && ic == 3)) s->incb = 0;
    /* GB12, GB13: regional indicators pair up */
    s->ri = g == CUI__VTW_RI ? (uint8_t)(joined && s->ri ? 0 : 1) : 0;
    s->last = (uint16_t)p;
}

void cui__vtw_begin(cui__vtw_gs *s, unsigned p) {
    memset(s, 0, sizeof *s);
    cui__vtw_after(s, p, false);
}

bool cui__vtw_join(cui__vtw_gs *s, unsigned p) {
    unsigned a = CUI__VTW_GCB(s->last), b = CUI__VTW_GCB(p);
    bool j;
    if (a == CUI__VTW_CR && b == CUI__VTW_LF) j = true;                                                   /* GB3 */
    else if (a == CUI__VTW_CONTROL || a == CUI__VTW_CR || a == CUI__VTW_LF) j = false;                    /* GB4 */
    else if (b == CUI__VTW_CONTROL || b == CUI__VTW_CR || b == CUI__VTW_LF) j = false;                    /* GB5 */
    else if (a == CUI__VTW_L && (b == CUI__VTW_L || b == CUI__VTW_V || b == CUI__VTW_LV || b == CUI__VTW_LVT)) j = true;   /* GB6 */
    else if ((a == CUI__VTW_LV || a == CUI__VTW_V) && (b == CUI__VTW_V || b == CUI__VTW_T)) j = true;     /* GB7 */
    else if ((a == CUI__VTW_LVT || a == CUI__VTW_T) && b == CUI__VTW_T) j = true;                         /* GB8 */
    else if (b == CUI__VTW_EXTEND || b == CUI__VTW_ZWJ || b == CUI__VTW_SPACING) j = true;                /* GB9, GB9a */
    else if (a == CUI__VTW_PREPEND) j = true;                                                             /* GB9b */
    else if (s->incb == 2 && CUI__VTW_INCB(p) == 2) j = true;                                             /* GB9c */
    else if (s->pict == 2 && (p & CUI__VTW_PICT)) j = true;                                               /* GB11 */
    else j = a == CUI__VTW_RI && b == CUI__VTW_RI && s->ri;                                               /* GB12, GB13 */
    if (j) cui__vtw_after(s, p, true);
    else cui__vtw_begin(s, p);
    return j;
}

/* the cells of a cluster: its first character's, with VS16 or VS15 right after it */
static bool cui__vtw_tone(uint32_t c) { return c >= 0x1F3FB && c <= 0x1F3FF; }

/* a pictograph asked for in its emoji style: VS16, or a skin tone (which only an emoji takes) */
static bool cui__vtw_styled(const uint32_t *cps, int n, unsigned p) {
    return n > 1 && ((cps[1] == 0xFE0F && (p & CUI__VTW_VS16)) || (cui__vtw_tone(cps[1]) && (p & CUI__VTW_PICT)));
}

int cui__vtw_cluster_cells(const uint32_t *cps, int n) {
    unsigned p = cui__vtw_props(cps[0]);
    int w = cui__vtw_cells(p);
    if (cui__vtw_styled(cps, n, p)) w = 2;
    else if (n > 1 && cps[1] == 0xFE0E && (p & CUI__VTW_PRES)) w = 1;
    return w;
}

/* drawn by the emoji font: 2 cells of a pictograph or a flag, or a character asked for in its emoji style */
bool cui__vtw_cluster_emoji(const uint32_t *cps, int n, int cells) {
    unsigned p = cui__vtw_props(cps[0]);
    if (cui__vtw_styled(cps, n, p)) return true;
    return cells == 2 && ((p & CUI__VTW_PICT) || CUI__VTW_GCB(p) == CUI__VTW_RI);
}

/* the canonical composite of a and b (a + ring above: U+00E5), 0 none; BMP pairs only, for drawing a mark the fonts lack */
uint32_t cui__vtw_compose(uint32_t a, uint32_t b) {
    if (a > 0xFFFF || b > 0xFFFF) return 0;
    if (a >= 0x1100 && a <= 0x1112 && b >= 0x1161 && b <= 0x1175) return 0xAC00 + ((a - 0x1100) * 21 + (b - 0x1161)) * 28;   /* Hangul: L + V */
    if (a >= 0xAC00 && a <= 0xD7A3 && (a - 0xAC00) % 28 == 0 && b > 0x11A7 && b <= 0x11C2) return a + (b - 0x11A7);           /* LV + T */
    int lo = 0, hi = CUI__VTW_COMP_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t ma = cui__vtw_comp[mid][0], mb = cui__vtw_comp[mid][1];
        if (ma == a && mb == b) return cui__vtw_comp[mid][2];
        if (ma < a || (ma == a && mb < b)) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int cui_vt_grapheme_next(const char *s, int len, int *width) {
    if (width) *width = 0;
    if (!s || len <= 0) return 0;
    uint32_t cps[16], c;
    int n = 0, i = cui__utf8_decode(s, len, &c);
    cps[n++] = c;
    cui__vtw_gs st;
    cui__vtw_begin(&st, cui__vtw_props(c));
    while (i < len) {
        int k = cui__utf8_decode(s + i, len - i, &c);
        if (!cui__vtw_join(&st, cui__vtw_props(c))) break;
        if (n < 16) cps[n++] = c;
        i += k;
    }
    if (width) *width = cui__vtw_cluster_cells(cps, n);
    return i;
}

int cui_vt_text_width(const char *s, int len) {
    if (!s) return 0;
    if (len < 0) len = (int)strlen(s);
    int cols = 0;
    for (int i = 0; i < len;) {
        int w, k = cui_vt_grapheme_next(s + i, len - i, &w);
        if (k <= 0) break;
        cols += w; i += k;
    }
    return cols;
}

/* ------------------------------------------------------------------------ */
/* The cluster table                                                        */
/* ------------------------------------------------------------------------ */

typedef struct { uint32_t off, hash; uint8_t len, flags, live, mark; } cui__vtg_ent;

struct cui__vt_gcl {
    uint32_t *pool; uint32_t pool_n, pool_cap;          /* the code points of every cluster */
    cui__vtg_ent *ent; uint32_t ent_n, ent_cap;         /* by id; live 0: free */
    uint32_t *free_ids; uint32_t free_n;                /* ids to reuse (after a collection) */
    int32_t *slot; uint32_t slot_cap;                   /* open addressing: id + 1, 0 empty */
    uint32_t live;
    uint32_t last_new;                                  /* id + 1 of the entry the last add made (0: it found one), for undo */
};

static uint32_t cui__vtg_hash(const uint32_t *cps, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= cps[i]; h *= 16777619u; h ^= h >> 15; }
    return h ? h : 1;
}

static bool cui__vtg_same(const cui__vt_gcl *g, const cui__vtg_ent *e, const uint32_t *cps, int n) {
    return e->len == n && !memcmp(g->pool + e->off, cps, sizeof(uint32_t) * (size_t)n);
}

static void cui__vtg_rehash(cui__vt_gcl *g) {
    memset(g->slot, 0, sizeof(int32_t) * g->slot_cap);
    for (uint32_t i = 0; i < g->ent_n; i++) {
        if (!g->ent[i].live) continue;
        uint32_t m = g->slot_cap - 1, k = g->ent[i].hash & m;
        while (g->slot[k]) k = (k + 1) & m;
        g->slot[k] = (int32_t)i + 1;
    }
}

void cui__vt_gcl_free(cui__vt_gcl *g) {
    if (!g) return;
    free(g->pool); free(g->ent); free(g->free_ids); free(g->slot); free(g);
}

size_t cui__vt_gcl_memory(const cui__vt_gcl *g) {
    if (!g) return 0;
    return sizeof *g + g->pool_cap * sizeof(uint32_t) + g->ent_cap * (sizeof(cui__vtg_ent) + sizeof(uint32_t)) + g->slot_cap * sizeof(int32_t);
}

uint32_t cui__vt_gcl_count(const cui__vt_gcl *g) { return g ? g->live : 0; }

const uint32_t *cui__vt_gcl_get(const cui__vt_gcl *g, uint32_t cp, int *n, unsigned *flags) {
    uint32_t id = cp - CUI__VT_CLUSTER0;
    if (!g || cp < CUI__VT_CLUSTER0 || id >= g->ent_n || !g->ent[id].live) { *n = 0; if (flags) *flags = 0; return NULL; }
    *n = g->ent[id].len;
    if (flags) *flags = g->ent[id].flags;
    return g->pool + g->ent[id].off;
}

void cui__vt_gcl_mark(cui__vt_gcl *g, uint32_t cp) {
    uint32_t id = cp - CUI__VT_CLUSTER0;
    if (cp >= CUI__VT_CLUSTER0 && id < g->ent_n) g->ent[id].mark = 1;
}

/* frees what no cell holds: cui_vt.c has marked every cell's cluster */
void cui__vt_gcl_sweep(cui__vt_gcl *g) {
    uint32_t w = 0;
    g->free_n = 0;
    for (uint32_t i = 0; i < g->ent_n; i++) {
        cui__vtg_ent *e = &g->ent[i];
        if (e->live && e->mark) {                                  /* kept: its code points move down the pool */
            memmove(g->pool + w, g->pool + e->off, sizeof(uint32_t) * e->len);
            e->off = w; w += e->len;
        } else if (e->live) { e->live = 0; g->live--; }
        e->mark = 0;
    }
    g->pool_n = w;
    while (g->ent_n > 0 && !g->ent[g->ent_n - 1].live) g->ent_n--;  /* free ids at the end are simply not used */
    for (uint32_t i = g->ent_n; i-- > 0;) if (!g->ent[i].live) g->free_ids[g->free_n++] = i;
    g->last_new = 0;
    cui__vtg_rehash(g);
}

/* The id of a cluster (CUI__VT_CLUSTER0 + index), made if new; 0 when the table is full of clusters cells still hold (the
 * caller then keeps the first character) or out of memory. *full is set when a collection would make room. */
uint32_t cui__vt_gcl_add(cui__vt_gcl **pg, const uint32_t *cps, int n, unsigned flags, bool *full) {
    *full = false;
    if (n < 2 || n > 16) return 0;
    cui__vt_gcl *g = *pg;
    if (!g) {
        g = (cui__vt_gcl *)calloc(1, sizeof *g);
        if (!g) return 0;
        g->slot_cap = 256;
        g->slot = (int32_t *)calloc(g->slot_cap, sizeof(int32_t));
        if (!g->slot) { free(g); return 0; }
        *pg = g;
    }
    uint32_t h = cui__vtg_hash(cps, n), m = g->slot_cap - 1, k = h & m;
    for (; g->slot[k]; k = (k + 1) & m) {
        cui__vtg_ent *e = &g->ent[g->slot[k] - 1];
        if (e->hash == h && cui__vtg_same(g, e, cps, n)) { g->last_new = 0; return CUI__VT_CLUSTER0 + (uint32_t)(g->slot[k] - 1); }
    }
    uint32_t id;
    if (g->free_n) id = g->free_ids[--g->free_n];
    else if (g->ent_n < CUI__VTG_MAX) {
        if (g->ent_n == g->ent_cap) {
            uint32_t cap = g->ent_cap ? g->ent_cap * 2 : 64;
            cui__vtg_ent *ne = (cui__vtg_ent *)realloc(g->ent, cap * sizeof *ne);
            if (!ne) return 0;
            g->ent = ne;
            uint32_t *nf = (uint32_t *)realloc(g->free_ids, cap * sizeof *nf);
            if (!nf) return 0;
            g->free_ids = nf;
            g->ent_cap = cap;
        }
        id = g->ent_n++;
    } else { *full = true; return 0; }
    if (g->pool_n + (uint32_t)n > g->pool_cap) {
        uint32_t cap = g->pool_cap ? g->pool_cap * 2 : 1024;
        while (cap < g->pool_n + (uint32_t)n) cap *= 2;
        uint32_t *np = (uint32_t *)realloc(g->pool, cap * sizeof *np);
        if (!np) { if (id == g->ent_n - 1) g->ent_n--; else g->free_ids[g->free_n++] = id; return 0; }
        g->pool = np; g->pool_cap = cap;
    }
    if ((g->live + 1) * 2 > g->slot_cap) {                       /* keep the table at most half full */
        int32_t *ns = (int32_t *)calloc(g->slot_cap * 2, sizeof(int32_t));
        if (ns) {
            free(g->slot); g->slot = ns; g->slot_cap *= 2;
            g->ent[id].live = 0;
            cui__vtg_rehash(g);
            m = g->slot_cap - 1;
            for (k = h & m; g->slot[k]; k = (k + 1) & m) {}
        }
    }
    cui__vtg_ent *e = &g->ent[id];
    e->off = g->pool_n; e->hash = h; e->len = (uint8_t)n; e->flags = (uint8_t)flags; e->live = 1; e->mark = 0;
    memcpy(g->pool + g->pool_n, cps, sizeof(uint32_t) * (size_t)n);
    g->pool_n += (uint32_t)n;
    g->slot[k] = (int32_t)id + 1;
    g->live++;
    g->last_new = id + 1;
    return CUI__VT_CLUSTER0 + id;
}

/* Takes back the cluster the last add made, when only one cell held it and that cell now holds a longer one: a mark at a
 * time grows a cluster without leaving each step behind. Nothing else may have been added since. */
void cui__vt_gcl_undo(cui__vt_gcl *g, uint32_t cp) {
    uint32_t id = cp - CUI__VT_CLUSTER0;
    if (!g || cp < CUI__VT_CLUSTER0 || g->last_new != id + 1 || !g->ent[id].live) return;
    cui__vtg_ent *e = &g->ent[id];
    uint32_t m = g->slot_cap - 1, k = e->hash & m;
    while (g->slot[k] && g->slot[k] != (int32_t)id + 1) k = (k + 1) & m;
    if (!g->slot[k]) return;
    g->slot[k] = 0;                         /* the newest entry: no later one probed past it, so the chain stays whole */
    if (e->off + e->len == g->pool_n) g->pool_n = e->off;
    e->live = 0; g->live--;
    if (id == g->ent_n - 1) g->ent_n--; else g->free_ids[g->free_n++] = id;
    g->last_new = 0;
}
