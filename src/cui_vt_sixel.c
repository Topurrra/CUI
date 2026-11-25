/* The VT screen's opt-in parts (cui_vt_set_features): answers to the queries a console host forwards, and sixel pictures.
 *
 * Windows Terminal is the reference (github.com/microsoft/terminal, MIT; read for behaviour, no code copied):
 *   - sixel pixels sit on a virtual VT340 grid of 10 x 20 pixels a cell (SixelParser::CellSizeForLevel), ESC [ 16 t and
 *     ESC [ 14 t report that virtual size (adaptDispatch.cpp, "the virtual pixel size, as used in the Sixel graphics
 *     emulation"), and the picture is drawn scaled to the real cells. Its console host (OpenConsole.exe), which sits between
 *     the program and us, keeps its own copy of the screen with the same arithmetic, so doing the same keeps the cursor
 *     where both of them think it is.
 *   - placement (SixelParser.cpp): the picture starts at the cursor (at the top left of the page in display mode, DECSDM);
 *     a band that would not fit above the bottom margin scrolls the text first, one cell row at a time, until it fits with
 *     a pixel to spare; at the end the cursor goes back to the column it started in, on the row holding the top of the last
 *     band (origin row + image y / 20), so a picture ending with "-" leaves the cursor on the row below it.
 *   - text written over a picture's cells removes those cells of it (TextBuffer::Replace -> ImageSlice::EraseCells); erasing
 *     does too; pictures move with their rows.
 *
 * Storage. A picture is kept as slices, one per screen row it touches (as Windows Terminal's ImageSlice): 10 x 20 RGBA pixels
 * per cell for the columns c0..c1-1, a byte per cell saying it holds picture, and an id and version for the view's textures.
 * The two screens keep a slice table indexed by row slot (the row map rotates slots when the screen scrolls, so slices move
 * with their rows for nothing); a slice whose row leaves into the scrollback moves to a ring keyed by the line's number since
 * the first line ever kept. Every slice counts against the budget; over it the slice drawn into longest ago goes first, never
 * one of the picture being read (that picture is cut off instead). Nothing here is allocated before the first picture, and
 * the text paths of cui_vt.c only test a byte of flags until then.
 *
 * Decoding reads a band (6 sixel rows, each `ratio` pixels high) into a buffer of 6 rows as wide as the picture may be, then
 * copies it into the slices at the next "-" or at the end: the inner loop only sets pixels in one small array. */
#include "cui_internal.h"

#include <limits.h>
#if defined(_MSC_VER)
#include <intrin.h>   /* _InterlockedIncrement */
#endif

#define CUI__VTP_CW 10                  /* the virtual cell sixel pixels are placed on (its width with SIXEL_ASPECT: cui__vtp_cw) */
#define CUI__VTP_CH 20
#define CUI__VTP_PARAM_MAX 32767
#define CUI__VTP_BUDGET ((size_t)8 << 20)
#define CUI__VTP_REPLY_BIG 4096         /* replies past the screen's own 256 bytes, kept until cui_vt_reply takes them */

typedef struct cui__vtp_slice {
    uint32_t id, ver, seq;              /* seq: when it was last drawn into (the budget evicts the smallest) */
    int c0, c1, live;                   /* columns c0..c1-1; live: how many of them hold picture */
    int cw;                             /* pixels a cell is wide here: 10, or the W it was drawn with (SIXEL_ASPECT) */
    size_t bytes;
    uint8_t *mask;                      /* c1 - c0 bytes: 1 where the cell holds picture */
    uint32_t *px;                       /* (c1 - c0) * cw x 20 pixels, RGBA bytes in memory order; 0 is transparent */
} cui__vtp_slice;

typedef struct { uint64_t line; cui__vtp_slice *s; } cui__vtp_sbent;

enum { CUI__SX_DATA, CUI__SX_COLOR, CUI__SX_REPEAT, CUI__SX_ATTR };

typedef struct {                        /* the picture being read */
    int st, prm[6], nprm;
    int ratio, six_h, seg_h, max_ratio;
    int ox, oy, text_x;                 /* origin cell (oy goes negative once its top scrolled away), the cursor's column */
    int cw;                             /* the virtual cell's width for the whole picture (fixed when it starts) */
    int ix, iy, max_w, avail_h, marg_h; /* image cursor and limits in virtual pixels (Windows Terminal's names) */
    bool sdm, dead, full;               /* display mode; nothing is drawn (origin outside the margins); over the budget */
    bool bg_need, bg_done;              /* P2 != 1: the picture's area is filled with color 0 at the first sixel */
    int bg_w, bg_h, bg_y0, bg_end, bg_filled;
    uint32_t col, bgc, reg[256];
    uint32_t *band; int band_w, band_cap;
    int bx0, bx1;                       /* columns of the band that were drawn */
    int done_y;                         /* image rows below this were copied out */
    uint32_t seq;
} cui__vtp_six;

struct cui__vt_x {
    unsigned features;
    uint32_t nonce;                     /* tells this screen's slices from those of an earlier screen at the same address */
    size_t budget, used;
    uint32_t next_id, next_seq;
    bool ever;                          /* a picture was kept once: the view looks after textures from now on */
    bool sdm;                           /* DECSDM */
    cui__vtp_slice **tab[2]; int tab_rows[2], tab_count[2];
    cui__vtp_sbent *sb; int sb_cap, sb_head, sb_n;      /* ring, oldest first */
    cui__vtp_six *six; bool reading;
    bool has_fg, has_bg, has_cur; cui_color fg, bg, cur;
    char *rbig; size_t rbig_n;
};

static uint32_t cui__vtp_rgba(int r, int g, int b) {
    uint8_t q[4] = { (uint8_t)r, (uint8_t)g, (uint8_t)b, 255 };
    uint32_t c;
    memcpy(&c, q, 4);
    return c;
}

/* ------------------------------------------------------------------------ */
/* The extension                                                            */
/* ------------------------------------------------------------------------ */

/* A number no other screen of this process has had: a view's textures are keyed by it with the slice ids, and a screen
 * created after another was destroyed often lands at the same address with the same ids. The one process-wide counter
 * here, taken atomically because screens may be made on any thread. */
static volatile long cui__vtp_serial;
static uint32_t cui__vtp_next_serial(void) {
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedIncrement(&cui__vtp_serial);
#else
    return (uint32_t)__atomic_add_fetch(&cui__vtp_serial, 1, __ATOMIC_RELAXED);
#endif
}

static cui__vt_x *cui__vtp_get(cui_vt *v) {
    cui__vt_x *x = cui__vt_ext(v);
    if (x) return x;
    x = (cui__vt_x *)calloc(1, sizeof *x);
    if (!x) return NULL;
    x->budget = CUI__VTP_BUDGET;
    x->nonce = cui__vtp_next_serial();
    cui__vt_set_ext(v, x);
    return x;
}

static void cui__vtp_bits(cui_vt *v, cui__vt_x *x) {
    uint8_t b = 0;
    if (x->tab_count[0]) b |= CUI__VT_PIC_MAIN;
    if (x->tab_count[1]) b |= CUI__VT_PIC_ALT;
    if (x->sb_n) b |= CUI__VT_PIC_SB;
    cui__vt_set_pic_bits(v, b);
}

static void cui__vtp_slice_free(cui__vt_x *x, cui__vtp_slice *s) {
    if (!s) return;
    x->used -= s->bytes;
    free(s);
}

static void cui__vtp_drop_all(cui_vt *v, cui__vt_x *x) {
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < x->tab_rows[k]; i++) cui__vtp_slice_free(x, x->tab[k][i]);
        free(x->tab[k]); x->tab[k] = NULL; x->tab_rows[k] = 0; x->tab_count[k] = 0;
    }
    for (int i = 0; i < x->sb_n; i++) cui__vtp_slice_free(x, x->sb[(x->sb_head + i) % x->sb_cap].s);
    free(x->sb); x->sb = NULL; x->sb_cap = x->sb_head = x->sb_n = 0;
    cui__vtp_bits(v, x);
}

static void cui__vtp_six_free(cui__vt_x *x) {
    if (x->six) free(x->six->band);
    free(x->six); x->six = NULL; x->reading = false;
}

void cui__vt_x_free(cui_vt *v) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x) return;
    cui__vtp_drop_all(v, x);
    cui__vtp_six_free(x);
    free(x->rbig);
    free(x);
    cui__vt_set_ext(v, NULL);
}

size_t cui__vt_x_memory(const cui_vt *v) {
    const cui__vt_x *x = cui__vt_ext(v);
    if (!x) return 0;
    size_t n = sizeof *x + x->used + (size_t)x->sb_cap * sizeof(cui__vtp_sbent) + (x->rbig ? CUI__VTP_REPLY_BIG : 0);
    n += ((size_t)x->tab_rows[0] + (size_t)x->tab_rows[1]) * sizeof(cui__vtp_slice *);
    if (x->six) n += sizeof *x->six + (size_t)x->six->band_cap * sizeof(uint32_t);
    return n;
}

void cui__vt_x_reset(cui_vt *v) {
    cui__vt_x *x = cui__vt_ext(v);
    x->sdm = false;
    if (x->six) x->six->dead = true;
}

/* The virtual cell's width: 10 as Windows Terminal, or with SIXEL_ASPECT the width that keeps the view's cell shape at a
 * height of 20 (10 until a view has drawn and told the screen its cell). */
static int cui__vtp_cw(const cui__vt_x *x, const cui__vt_info *in) {
    if (!(x->features & CUI_VT_FEATURE_SIXEL_ASPECT) || in->cell_w <= 0 || in->cell_h <= 0) return CUI__VTP_CW;
    int cw16 = in->cell_w16 > 0 ? in->cell_w16 : in->cell_w * 16;   /* the unrounded width: 6.65 px must not count as 7 */
    int w = (2 * CUI__VTP_CH * cw16 + 16 * in->cell_h) / (32 * in->cell_h);
    return CUI_CLAMP(w, 6, 14);
}

void cui_vt_set_features(cui_vt *v, unsigned features) {
    if (!v) return;
    cui__vt_reflow_set(v, (features & CUI_VT_FEATURE_REFLOW) != 0);     /* the screen's own (cui_vt.c): needs no extension */
    cui__vt_lk_feature(v, (features & CUI_VT_FEATURE_LINKS) != 0);     /* hyperlinks live in cui_vt_links.c */
    cui__vt_mk_feature(v, (features & CUI_VT_FEATURE_MARKS) != 0);     /* marks, progress, copy requests: cui_vt_marks.c */
    features &= CUI_VT_FEATURE_QUERIES | CUI_VT_FEATURE_SIXEL | CUI_VT_FEATURE_SIXEL_ASPECT;
    cui__vt_x *x = cui__vt_ext(v);
    if (!features && !x) return;
    if (!features && x && !x->has_fg && !x->has_bg && !x->has_cur && !x->rbig_n) { cui__vt_x_free(v); return; }
    x = cui__vtp_get(v);
    if (!x) return;
    x->features = features;
    if (!(features & CUI_VT_FEATURE_SIXEL)) {          /* pictures go with the feature */
        cui__vtp_drop_all(v, x);
        cui__vtp_six_free(x);
        x->sdm = false;
    }
}

void cui_vt_set_colors(cui_vt *v, cui_color fg, cui_color bg, cui_color cursor) {
    if (!v) return;
    cui__vt_x *x = cui__vt_ext(v);
    bool any = cui__color_set(fg) || cui__color_set(bg) || cui__color_set(cursor);
    if (!x && !any) return;
    x = cui__vtp_get(v);
    if (!x) return;
    x->has_fg = fg.a != 0 && !cui__is_token(fg); x->fg = fg;
    x->has_bg = bg.a != 0 && !cui__is_token(bg); x->bg = bg;
    x->has_cur = cursor.a != 0 && !cui__is_token(cursor); x->cur = cursor;
}

/* ------------------------------------------------------------------------ */
/* Replies past the screen's own buffer                                     */
/* ------------------------------------------------------------------------ */

bool cui__vt_x_reply(cui_vt *v, char *inl, int *inl_n, size_t inl_cap, const char *s, size_t n) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x->rbig_n && (size_t)*inl_n + n <= inl_cap) return false;     /* the usual case: it fits where it always went */
    if (!x->rbig && !(x->rbig = (char *)malloc(CUI__VTP_REPLY_BIG))) return false;
    if (!x->rbig_n && *inl_n) { memcpy(x->rbig, inl, (size_t)*inl_n); x->rbig_n = (size_t)*inl_n; *inl_n = 0; }   /* keep the order */
    if (x->rbig_n + n <= CUI__VTP_REPLY_BIG) { memcpy(x->rbig + x->rbig_n, s, n); x->rbig_n += n; }
    return true;                        /* a reply that does not fit is dropped whole, never cut (4 KB of answers to one write) */
}

size_t cui__vt_x_take(cui_vt *v, char *out, size_t cap) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x->rbig_n) return 0;
    size_t n = CUI_MIN(x->rbig_n, cap);
    memcpy(out, x->rbig, n);
    memmove(x->rbig, x->rbig + n, x->rbig_n - n);
    x->rbig_n -= n;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Queries                                                                  */
/* ------------------------------------------------------------------------ */

bool cui__vt_query_csi(cui_vt *v, char f, char priv, char inter, const int *p, int np) {
    cui__vt_x *x = cui__vt_ext(v);
    unsigned fe = x->features;
    int p0 = np ? p[0] : 0;
    char b[64];
    cui__vt_info in;
    if (fe & CUI_VT_FEATURE_QUERIES) {
        if (f == 'c' && !inter && p0 == 0) {
            /* VT220 class, ANSI color (22), sixel (4) only when it is drawn: what is really there. Windows Terminal says
             * 61;4;6;7;14;21;22;23;24;28;32;42 (selective erase, soft fonts, rectangles... which this screen does not have). */
            if (!priv) { cui__vt_reply(v, (fe & CUI_VT_FEATURE_SIXEL) ? "\x1b[?62;4;22c" : "\x1b[?62;22c"); return true; }
            if (priv == '>') { cui__vt_reply(v, "\x1b[>1;10;0c"); return true; }              /* VT220, firmware 10, no options */
            if (priv == '=') { cui__vt_reply(v, "\x1bP!|00000000\x1b\\"); return true; }      /* unit id, as Windows Terminal */
        }
        if (f == 'n' && priv == '?' && !inter && p0 == 6) {
            cui__vt_get_info(v, &in);
            snprintf(b, sizeof b, "\x1b[?%d;%d;1R", in.cy + 1, in.cx + 1);
            cui__vt_reply(v, b);
            return true;
        }
        if (f == 'p' && inter == '$' && (!priv || priv == '?')) {          /* DECRQM */
            int st = cui__vt_mode_report(v, p0, priv == '?');
            if (priv == '?' && p0 == 80 && (fe & CUI_VT_FEATURE_SIXEL)) st = x->sdm ? 1 : 2;
            snprintf(b, sizeof b, priv ? "\x1b[?%d;%d$y" : "\x1b[%d;%d$y", p0, st);
            cui__vt_reply(v, b);
            return true;
        }
    }
    if (fe & CUI_VT_FEATURE_SIXEL) {
        if (f == 't' && !priv && !inter && (p0 == 14 || p0 == 16)) {     /* the virtual cell, as Windows Terminal */
            cui__vt_get_info(v, &in);
            int cw = cui__vtp_cw(x, &in);
            if (p0 == 16) snprintf(b, sizeof b, "\x1b[6;%d;%dt", CUI__VTP_CH, cw);
            else snprintf(b, sizeof b, "\x1b[4;%d;%dt", in.rows * CUI__VTP_CH, in.cols * cw);
            cui__vt_reply(v, b);
            return true;
        }
        if (f == 'S' && priv == '?' && !inter && np >= 2) {               /* XTSMGRAPHICS: color registers, sixel geometry */
            int pi = p[0], pa = p[1];
            cui__vt_get_info(v, &in);
            if (pa < 1 || pa > 4) snprintf(b, sizeof b, "\x1b[?%d;2;0S", pi);
            else if (pi == 1) snprintf(b, sizeof b, "\x1b[?1;%d;256S", pa == 3 ? 3 : 0);        /* the count cannot be changed */
            else if (pi == 2) snprintf(b, sizeof b, "\x1b[?2;%d;%d;%dS", pa == 3 ? 3 : 0, in.cols * cui__vtp_cw(x, &in), in.rows * CUI__VTP_CH);
            else snprintf(b, sizeof b, "\x1b[?%d;1;0S", pi);
            cui__vt_reply(v, b);
            return true;
        }
    }
    return false;
}

void cui__vt_query_osc(cui_vt *v, int code, const char *arg, int n, bool bel) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!(x->features & CUI_VT_FEATURE_QUERIES) || n != 1 || arg[0] != '?') return;
    cui_color c;
    if (code == 10 && x->has_fg) c = x->fg;
    else if (code == 11 && x->has_bg) c = x->bg;
    else if (code == 12 && x->has_cur) c = x->cur;
    else return;
    char b[64];
    snprintf(b, sizeof b, "\x1b]%d;rgb:%04x/%04x/%04x%s", code, c.r * 0x101u, c.g * 0x101u, c.b * 0x101u, bel ? "\x07" : "\x1b\\");
    cui__vt_reply(v, b);
}

/* ------------------------------------------------------------------------ */
/* Slices                                                                   */
/* ------------------------------------------------------------------------ */

static cui__vtp_slice *cui__vtp_sb_at(const cui__vt_x *x, int i) { return x->sb[(x->sb_head + i) % x->sb_cap].s; }

/* The slice drawn into longest ago, outside the picture being read (seq >= keep), freed. False when there is none. */
static bool cui__vtp_evict_one(cui_vt *v, cui__vt_x *x, uint32_t keep) {
    int best_k = -1, best_i = -1;
    uint32_t best = UINT32_MAX;
    if (x->sb_n) {                                          /* the oldest line of the scrollback stands for the ring */
        cui__vtp_slice *s = cui__vtp_sb_at(x, 0);
        if (s->seq < keep) { best = s->seq; best_k = 2; best_i = 0; }
    }
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    for (int k = 0; k < 2; k++) {
        if (!x->tab_count[k]) continue;
        for (int y = 0; y < CUI_MIN(x->tab_rows[k], in.rows); y++) {      /* top rows first: the oldest rows of a picture go first */
            int slot = cui__vt_slot(v, k, y);
            if (slot >= x->tab_rows[k]) continue;
            cui__vtp_slice *s = x->tab[k][slot];
            if (s && s->seq < keep && s->seq < best) { best = s->seq; best_k = k; best_i = slot; }
        }
    }
    if (best_k < 0) return false;
    if (best_k == 2) {
        cui__vtp_slice_free(x, cui__vtp_sb_at(x, 0));
        x->sb_head = (x->sb_head + 1) % x->sb_cap; x->sb_n--;
    } else {
        cui__vtp_slice_free(x, x->tab[best_k][best_i]);
        x->tab[best_k][best_i] = NULL; x->tab_count[best_k]--;
    }
    cui__vtp_bits(v, x);
    return true;
}

static bool cui__vtp_room(cui_vt *v, cui__vt_x *x, size_t need, uint32_t keep) {
    while (x->used + need > x->budget) if (!cui__vtp_evict_one(v, x, keep)) return false;
    return true;
}

void cui_vt_set_picture_budget(cui_vt *v, size_t bytes) {
    if (!v) return;
    cui__vt_x *x = cui__vtp_get(v);
    if (!x) return;
    x->budget = bytes;
    cui__vtp_room(v, x, 0, x->reading && x->six ? x->six->seq : UINT32_MAX);
}

static size_t cui__vtp_slice_bytes(int nc, int cw) { return sizeof(cui__vtp_slice) + (size_t)nc * ((size_t)cw * CUI__VTP_CH * 4 + 1); }

static cui__vtp_slice *cui__vtp_slice_new(int c0, int c1, int cw) {
    int nc = c1 - c0;
    size_t bytes = cui__vtp_slice_bytes(nc, cw);
    cui__vtp_slice *s = (cui__vtp_slice *)calloc(1, bytes);
    if (!s) return NULL;
    s->c0 = c0; s->c1 = c1; s->cw = cw; s->bytes = bytes;
    s->px = (uint32_t *)(void *)(s + 1);                    /* the header's size is a multiple of its pointer alignment */
    s->mask = (uint8_t *)(s->px + (size_t)nc * (size_t)cw * CUI__VTP_CH);
    return s;
}

/* The slice of the row in `slot` of screen k, made or widened to cover columns c0..c1-1 at cw pixels a cell; NULL over the
 * budget. A row drawn with another cell width first has its old cells scaled to this one (nearest pixel, in place: the
 * cells keep their columns, so nothing shifts). */
static cui__vtp_slice *cui__vtp_slice_for(cui_vt *v, cui__vt_x *x, int k, int slot, int c0, int c1, int cw, uint32_t keep) {
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    if (!x->tab[k] || x->tab_rows[k] != in.rows) {
        cui__vtp_slice **t = (cui__vtp_slice **)realloc(x->tab[k], sizeof *t * (size_t)in.rows);
        if (!t) return NULL;
        for (int i = x->tab_rows[k]; i < in.rows; i++) t[i] = NULL;
        x->tab[k] = t; x->tab_rows[k] = in.rows;
    }
    cui__vtp_slice *s = x->tab[k][slot];
    if (s && s->c0 <= c0 && s->c1 >= c1 && s->cw == cw) return s;
    int n0 = s ? CUI_MIN(s->c0, c0) : c0, n1 = s ? CUI_MAX(s->c1, c1) : c1;
    if (s) s->seq = keep;                                   /* being drawn into: not a candidate for eviction */
    if (!cui__vtp_room(v, x, cui__vtp_slice_bytes(n1 - n0, cw), keep)) return NULL;
    s = x->tab[k][slot];                                    /* (eviction never takes it, but read it again) */
    cui__vtp_slice *n = cui__vtp_slice_new(n0, n1, cw);
    if (!n) return NULL;
    x->used += n->bytes;
    if (s) {                                                /* widened or rescaled: the old cells go to their place in the new one */
        int ow = (s->c1 - s->c0) * s->cw, nw = (n1 - n0) * cw;
        for (int r = 0; r < CUI__VTP_CH; r++) {
            const uint32_t *src = s->px + (size_t)r * (size_t)ow;
            uint32_t *dst = n->px + (size_t)r * (size_t)nw;
            if (s->cw == cw) memcpy(dst + (size_t)(s->c0 - n0) * (size_t)cw, src, (size_t)ow * 4);
            else for (int c = s->c0; c < s->c1; c++)
                for (int i = 0; i < cw; i++) dst[(c - n0) * cw + i] = src[(c - s->c0) * s->cw + (2 * i + 1) * s->cw / (2 * cw)];
        }
        memcpy(n->mask + (s->c0 - n0), s->mask, (size_t)(s->c1 - s->c0));
        n->live = s->live; n->id = s->id; n->ver = s->ver + 1;
        cui__vtp_slice_free(x, s);
    } else {
        n->id = ++x->next_id;
        x->tab_count[k]++;
    }
    n->seq = keep;
    x->tab[k][slot] = n;
    x->ever = true;
    cui__vtp_bits(v, x);
    return n;
}

static void cui__vtp_slice_erase(cui__vtp_slice *s, int x0, int x1) {
    int w = (s->c1 - s->c0) * s->cw;
    for (int c = x0; c < x1; c++) {
        if (!s->mask[c - s->c0]) continue;
        s->mask[c - s->c0] = 0; s->live--;
        for (int r = 0; r < CUI__VTP_CH; r++) memset(s->px + (size_t)r * (size_t)w + (size_t)(c - s->c0) * (size_t)s->cw, 0, (size_t)s->cw * 4);
    }
    s->ver++;
}

void cui__vt_pic_erase(cui_vt *v, int scr, int slot, int x0, int x1) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x || slot >= x->tab_rows[scr]) return;
    cui__vtp_slice *s = x->tab[scr][slot];
    if (!s || x1 <= s->c0 || x0 >= s->c1) return;
    x0 = CUI_MAX(x0, s->c0); x1 = CUI_MIN(x1, s->c1);
    cui__vtp_slice_erase(s, x0, x1);
    if (s->live <= 0) {
        cui__vtp_slice_free(x, s);
        x->tab[scr][slot] = NULL; x->tab_count[scr]--;
        cui__vtp_bits(v, x);
    }
}

/* Insert (n > 0) or delete (n < 0) cells at column `at`: the picture cells right of it move with the text, within the
 * columns the slice has (cells pushed past its right end are lost, as the text past the screen's is). */
void cui__vt_pic_shift(cui_vt *v, int slot, int at, int n) {
    cui__vt_x *x = cui__vt_ext(v);
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    if (!x || slot >= x->tab_rows[in.cur] || n == 0) return;
    cui__vtp_slice *s = x->tab[in.cur][slot];
    if (!s || at >= s->c1) return;
    const int cw = s->cw, w = (s->c1 - s->c0) * cw;
    int a = CUI_MAX(at, s->c0);
    for (int pass = 0; pass < s->c1 - a; pass++) {
        int c = n > 0 ? s->c1 - 1 - pass : a + pass;       /* destination cell, walked so sources are read before they are written */
        int from = c - n;
        bool has = from >= at && from >= s->c0 && from < s->c1 && s->mask[from - s->c0];
        bool had = s->mask[c - s->c0] != 0;
        for (int r = 0; r < CUI__VTP_CH; r++) {
            uint32_t *dst = s->px + (size_t)r * (size_t)w + (size_t)(c - s->c0) * (size_t)cw;
            if (has) memcpy(dst, s->px + (size_t)r * (size_t)w + (size_t)(from - s->c0) * (size_t)cw, (size_t)cw * 4);
            else memset(dst, 0, (size_t)cw * 4);
        }
        s->mask[c - s->c0] = has;
        s->live += (int)has - (int)had;
    }
    s->ver++;
    if (s->live <= 0) {
        cui__vtp_slice_free(x, s);
        x->tab[in.cur][slot] = NULL; x->tab_count[in.cur]--;
        cui__vtp_bits(v, x);
    }
}

void cui__vt_pic_trim_sb(cui_vt *v) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x) return;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    uint64_t first = in.sb_total - (uint64_t)in.sb_count;  /* the oldest line still kept */
    while (x->sb_n && x->sb[x->sb_head].line < first) {
        cui__vtp_slice_free(x, x->sb[x->sb_head].s);
        x->sb_head = (x->sb_head + 1) % x->sb_cap; x->sb_n--;
    }
    cui__vtp_bits(v, x);
}

void cui__vt_pic_pushed(cui_vt *v, int slot) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x) return;
    cui__vtp_slice *s = slot < x->tab_rows[0] ? x->tab[0][slot] : NULL;
    if (s) {
        x->tab[0][slot] = NULL; x->tab_count[0]--;
        cui__vt_info in;
        cui__vt_get_info(v, &in);
        if (in.sb_max <= 0) cui__vtp_slice_free(x, s);
        else {
            if (x->sb_n == x->sb_cap) {                     /* grow the ring, oldest first again */
                int cap = x->sb_cap ? x->sb_cap * 2 : 64;
                cui__vtp_sbent *r = (cui__vtp_sbent *)malloc(sizeof *r * (size_t)cap);
                if (!r) { cui__vtp_slice_free(x, s); cui__vtp_bits(v, x); return; }
                for (int i = 0; i < x->sb_n; i++) r[i] = x->sb[(x->sb_head + i) % x->sb_cap];
                free(x->sb); x->sb = r; x->sb_cap = cap; x->sb_head = 0;
            }
            x->sb[(x->sb_head + x->sb_n) % x->sb_cap] = (cui__vtp_sbent){ in.sb_total - 1, s };
            x->sb_n++;
            cui__vt_sb_mark_pic(v);                         /* a reflow keeps this line as it is */
        }
    }
    cui__vt_pic_trim_sb(v);
}

/* A reflow renumbered the scrollback: the line first + i is now first + map[i], or gone (-1). The ring stays oldest first,
 * since a reflow keeps the order of lines. */
void cui__vt_pic_sb_remap(cui_vt *v, uint64_t first, const int32_t *map, int n) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x || !x->sb_n) return;
    int w = 0;
    for (int i = 0; i < x->sb_n; i++) {
        cui__vtp_sbent e = x->sb[(x->sb_head + i) % x->sb_cap];
        int64_t k = e.line >= first ? (int64_t)(e.line - first) : -1;
        if (k < 0 || k >= n || !map || map[k] < 0) { cui__vtp_slice_free(x, e.s); continue; }
        e.line = first + (uint64_t)map[k];
        x->sb[(x->sb_head + w) % x->sb_cap] = e;
        w++;
    }
    x->sb_n = w;
    cui__vt_pic_trim_sb(v);
}

void cui__vt_pic_drop_screen(cui_vt *v, int scr) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!x) return;
    for (int i = 0; i < x->tab_rows[scr]; i++) { cui__vtp_slice_free(x, x->tab[scr][i]); x->tab[scr][i] = NULL; }
    x->tab_count[scr] = 0;
    cui__vtp_bits(v, x);
}

/* The screen changed size: slices follow their rows (old slot old_map[y] becomes slot y), rows that are gone take their
 * slices with them, and cells past the new right edge are erased. A picture being read stops being drawn. */
void cui__vt_x_resize(cui_vt *v, int scr, const uint16_t *old_map, int old_rows, int cols, int rows) {
    cui__vt_x *x = cui__vt_ext(v);
    if (x->six) x->six->dead = true;
    if (!x->tab[scr]) return;
    cui__vtp_slice **t = (cui__vtp_slice **)calloc((size_t)rows, sizeof *t);
    int count = 0;
    for (int y = 0; y < old_rows; y++) {
        cui__vtp_slice *s = old_map[y] < x->tab_rows[scr] ? x->tab[scr][old_map[y]] : NULL;
        if (!s) continue;
        x->tab[scr][old_map[y]] = NULL;
        if (!t || y >= rows) { cui__vtp_slice_free(x, s); continue; }
        if (s->c1 > cols) {
            if (s->c0 < cols) cui__vtp_slice_erase(s, cols, s->c1);
            if (s->c0 >= cols || s->live <= 0) { cui__vtp_slice_free(x, s); continue; }
        }
        t[y] = s; count++;
    }
    for (int i = 0; i < x->tab_rows[scr]; i++) cui__vtp_slice_free(x, x->tab[scr][i]);     /* none should be left */
    free(x->tab[scr]);
    x->tab[scr] = t; x->tab_rows[scr] = t ? rows : 0; x->tab_count[scr] = count;
    cui__vtp_bits(v, x);
}

/* ------------------------------------------------------------------------ */
/* Sixel: colors                                                            */
/* ------------------------------------------------------------------------ */

/* DEC's HLS: hue 0 is blue, 120 red, 240 green; lightness and saturation 0..100. */
static uint32_t cui__vtp_hls(int h, int l, int s) {
    h = ((h % 360) + 360 + 240) % 360;                      /* to the usual hue, where 0 is red */
    float L = (float)CUI_CLAMP(l, 0, 100) / 100.0f, S = (float)CUI_CLAMP(s, 0, 100) / 100.0f;
    float c = (1.0f - fabsf(2.0f * L - 1.0f)) * S, hp = (float)h / 60.0f;
    float xx = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f)), m = L - c * 0.5f;
    float r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = xx; } else if (hp < 2) { r = xx; g = c; } else if (hp < 3) { g = c; b = xx; }
    else if (hp < 4) { g = xx; b = c; } else if (hp < 5) { r = xx; b = c; } else { r = c; b = xx; }
    return cui__vtp_rgba((int)lroundf((r + m) * 255.0f), (int)lroundf((g + m) * 255.0f), (int)lroundf((b + m) * 255.0f));
}

static uint32_t cui__vtp_rgb100(int r, int g, int b) {
    r = CUI_CLAMP(r, 0, 100); g = CUI_CLAMP(g, 0, 100); b = CUI_CLAMP(b, 0, 100);
    return cui__vtp_rgba((r * 255 + 50) / 100, (g * 255 + 50) / 100, (b * 255 + 50) / 100);
}

/* The registers a picture starts with: the VT340's 16 colors, then the 256 color palette of xterm. */
static void cui__vtp_palette(uint32_t *reg) {
    static const uint8_t vt340[16][3] = {   /* percent */
        { 0, 0, 0 }, { 20, 20, 80 }, { 80, 13, 13 }, { 20, 80, 20 }, { 80, 20, 80 }, { 20, 80, 80 }, { 80, 80, 20 }, { 53, 53, 53 },
        { 26, 26, 26 }, { 33, 33, 60 }, { 60, 26, 26 }, { 33, 60, 33 }, { 60, 33, 60 }, { 33, 60, 60 }, { 60, 60, 33 }, { 80, 80, 80 } };
    for (int i = 0; i < 16; i++) reg[i] = cui__vtp_rgb100(vt340[i][0], vt340[i][1], vt340[i][2]);
    for (int i = 16; i < 232; i++) {
        static const int lv[6] = { 0, 95, 135, 175, 215, 255 };
        int k = i - 16;
        reg[i] = cui__vtp_rgba(lv[k / 36], lv[(k / 6) % 6], lv[k % 6]);
    }
    for (int i = 232; i < 256; i++) { int g = 8 + (i - 232) * 10; reg[i] = cui__vtp_rgba(g, g, g); }
}

/* ------------------------------------------------------------------------ */
/* Sixel: reading                                                           */
/* ------------------------------------------------------------------------ */

void cui__vt_sixel_mode(cui_vt *v, bool set) {
    cui__vt_x *x = cui__vt_ext(v);
    if (x->features & CUI_VT_FEATURE_SIXEL) x->sdm = set;
}

bool cui__vt_sixel_begin(cui_vt *v, const int *p, int np) {
    cui__vt_x *x = cui__vt_ext(v);
    if (!(x->features & CUI_VT_FEATURE_SIXEL)) return false;
    if (!x->six && !(x->six = (cui__vtp_six *)calloc(1, sizeof *x->six))) return false;
    cui__vtp_six *z = x->six;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    uint32_t *band = z->band; int cap = z->band_cap;
    memset(z, 0, sizeof *z);
    z->band = band; z->band_cap = cap;
    z->sdm = x->sdm;
    z->text_x = in.cx;
    z->cw = cui__vtp_cw(x, &in);                            /* kept for the whole picture, even if the view's cell changes */
    if (z->sdm) {                                           /* display mode: the whole page, from its top left, no scrolling */
        z->ox = 0; z->oy = 0;
        z->max_w = in.cols * z->cw; z->avail_h = in.rows * CUI__VTP_CH; z->marg_h = in.rows * CUI__VTP_CH;
    } else {
        z->ox = in.cx; z->oy = in.cy;
        z->max_w = (in.cols - in.cx) * z->cw;
        z->avail_h = (in.sbot + 1 - in.cy) * CUI__VTP_CH;
        z->marg_h = (in.sbot - in.stop + 1) * CUI__VTP_CH;
        if (in.cy > in.sbot) z->dead = true;               /* below the scroll region: Windows Terminal draws nothing */
    }
    z->max_ratio = CUI_MAX(1, z->marg_h / 6);
    int p1 = np > 0 ? p[0] : 0, p2 = np > 1 ? p[1] : 0;
    z->ratio = (p1 <= 1 || p1 == 5 || p1 == 6) ? 2 : p1 == 2 ? 5 : (p1 == 3 || p1 == 4) ? 3 : 1;   /* P1: the pixel's height */
    z->six_h = z->seg_h = 6 * z->ratio;
    z->bg_need = p2 != 1;                                   /* P2 = 1: pixels no sixel sets stay as they are */
    z->bg_w = z->bg_h = INT_MAX; z->bg_filled = -1;
    cui__vtp_palette(z->reg);
    z->col = z->reg[15];
    z->bgc = z->reg[0];
    z->bx0 = INT_MAX; z->bx1 = 0;
    z->seq = ++x->next_seq;
    if (!z->dead && z->band_cap < 6 * z->max_w) {
        free(z->band);
        z->band_cap = 6 * z->max_w;
        z->band = (uint32_t *)calloc((size_t)z->band_cap, sizeof(uint32_t));
        if (!z->band) { z->band_cap = 0; z->dead = true; }
    }
    z->band_w = z->max_w;
    x->reading = true;
    return true;                                            /* a dead picture is still read, to its end, and drawn nowhere */
}

/* Image rows y0..y1-1 into the slices: the band where it covers them, color 0 inside the background area elsewhere. */
static void cui__vtp_commit(cui_vt *v, cui__vt_x *x, cui__vtp_six *z, int y0, int y1) {
    if (z->dead || z->full) return;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    int bgw = z->bg_done ? CUI_MIN(z->bg_w, z->max_w) : 0;
    for (int y = y0; y < y1; y++) {
        int row = z->oy + y / CUI__VTP_CH;
        if (row < 0) continue;
        if (row >= in.rows) break;
        bool in_band = y >= z->iy && y < z->iy + z->six_h && z->bx1 > z->bx0;
        bool in_bg = bgw > 0 && y >= z->bg_y0 && y < z->bg_end;
        int a = in_bg ? 0 : (in_band ? z->bx0 : 0), b = in_bg ? CUI_MAX(bgw, in_band ? z->bx1 : 0) : (in_band ? z->bx1 : 0);
        const uint32_t *src = in_band ? z->band + (size_t)((y - z->iy) / z->ratio) * (size_t)z->band_w : NULL;
        if (!in_bg && src) {                                /* only the pixels this row really has: an empty row makes no slice */
            while (a < b && !src[a]) a++;
            while (b > a && !src[b - 1]) b--;
        }
        if (b <= a) continue;
        const int cw = z->cw;
        int c0 = z->ox + a / cw, c1 = z->ox + (b + cw - 1) / cw;
        c1 = CUI_MIN(c1, in.cols);
        if (c1 <= c0) continue;
        cui__vtp_slice *s = cui__vtp_slice_for(v, x, in.cur, cui__vt_slot(v, in.cur, row), c0, c1, cw, z->seq);
        if (!s) { z->full = true; return; }                 /* over the budget: the rest of this picture is cut off */
        int sw = (s->c1 - s->c0) * cw, base = z->ox * cw - s->c0 * cw;
        uint32_t *dst = s->px + (size_t)(y % CUI__VTP_CH) * (size_t)sw;
        for (int px = a; px < b; px++) {
            uint32_t c = src && px >= z->bx0 && px < z->bx1 ? src[px] : 0;
            if (!c && in_bg && px < bgw) c = z->bgc;
            if (!c) continue;
            int dx = base + px;
            if (dx < 0 || dx >= sw) continue;
            dst[dx] = c;
            uint8_t *m = &s->mask[dx / cw];
            if (!*m) { *m = 1; s->live++; }
        }
        s->ver++; s->seq = z->seq;
    }
    if (y1 > z->done_y) z->done_y = y1;
}

static void cui__vtp_band_clear(cui__vtp_six *z) {
    if (z->bx1 > z->bx0 && z->band)
        for (int r = 0; r < 6; r++) memset(z->band + (size_t)r * (size_t)z->band_w + (size_t)z->bx0, 0, (size_t)(z->bx1 - z->bx0) * 4);
    z->bx0 = INT_MAX; z->bx1 = 0;
}

static void cui__vtp_cr(cui__vtp_six *z) { z->ix = 0; }

/* Windows Terminal's scroll before a flush: a segment taller than the room left scrolls the text a cell row at a time
 * until it fits with a pixel to spare (never more than the scroll region). */
static void cui__vtp_scroll_check(cui_vt *v, cui__vtp_six *z) {
    if (!z->sdm && !z->dead && z->seg_h > z->avail_h) {
        int n = 0;
        while (z->avail_h < z->marg_h && z->seg_h >= z->avail_h) { n++; z->avail_h += CUI__VTP_CH; }
        if (n) { cui__vt_scroll_lines(v, n); z->oy -= n; }
    }
    z->seg_h = z->six_h;
}

static void cui__vtp_bg_extend(cui__vtp_six *z) {          /* the background grows with a picture that scrolled */
    int bottom = z->iy + z->six_h;
    if (z->bg_filled >= 0 && bottom > z->bg_filled) {
        int nf = (bottom + CUI__VTP_CH - 1) / CUI__VTP_CH * CUI__VTP_CH;
        z->bg_filled = nf;
        if (nf > z->bg_end) z->bg_end = nf;
    }
}

static void cui__vtp_bg_fill(cui__vtp_six *z) {
    if (!z->bg_need || z->bg_done) return;
    z->bg_done = true;
    int h = CUI_MIN(z->bg_h, CUI_MAX(z->avail_h, 0));
    z->bg_y0 = z->iy; z->bg_end = z->iy + h;
    z->bg_filled = z->iy + CUI_MAX(z->avail_h, 0);
    cui__vtp_bg_extend(z);
}

static void cui__vtp_next_line(cui_vt *v, cui__vt_x *x, cui__vtp_six *z) {
    cui__vtp_cr(z);
    cui__vtp_scroll_check(v, z);
    cui__vtp_commit(v, x, z, z->iy, z->iy + z->six_h);
    cui__vtp_band_clear(z);
    if (z->avail_h > 0) {
        z->iy += z->six_h;
        z->avail_h -= z->six_h;
        cui__vtp_bg_extend(z);
    }
}

static void cui__vtp_raster(cui__vtp_six *z) {
    int pan = z->nprm > 0 ? z->prm[0] : 0, pad = z->nprm > 1 ? z->prm[1] : 0;
    if (pad > 0) {
        z->ratio = CUI_CLAMP((pan + pad - 1) / pad, 1, z->max_ratio);
        z->six_h = 6 * z->ratio;
        z->seg_h = CUI_MAX(z->seg_h, z->six_h);
    }
    if (z->nprm > 2 && z->prm[2] > 0) z->bg_w = z->prm[2];
    if (z->nprm > 3 && z->prm[3] > 0) z->bg_h = z->prm[3];
    cui__vtp_bg_extend(z);
    cui__vtp_cr(z);
}

static void cui__vtp_color(cui__vtp_six *z) {
    int n = (z->nprm > 0 ? z->prm[0] : 0) & 255;            /* register numbers wrap at 256 */
    if (z->nprm > 1) {
        int a = z->nprm > 2 ? z->prm[2] : 0, b = z->nprm > 3 ? z->prm[3] : 0, c = z->nprm > 4 ? z->prm[4] : 0;
        if (z->prm[1] == 1) z->reg[n] = cui__vtp_hls(a, b, c);
        else if (z->prm[1] == 2) z->reg[n] = cui__vtp_rgb100(a, b, c);
    }
    z->col = z->reg[n];
}

static int cui__vtp_pending(cui__vtp_six *z) {             /* the command whose parameters were being read is applied */
    int count = 1;
    if (z->st == CUI__SX_COLOR) cui__vtp_color(z);
    else if (z->st == CUI__SX_REPEAT) count = z->nprm > 0 && z->prm[0] > 0 ? z->prm[0] : 1;
    else if (z->st == CUI__SX_ATTR) cui__vtp_raster(z);
    z->st = CUI__SX_DATA;
    return count;
}

size_t cui__vt_sixel_feed(cui_vt *v, const unsigned char *d, size_t n) {
    cui__vt_x *x = cui__vt_ext(v);
    cui__vtp_six *z = x->six;
    uint32_t *band = z->band;
    const int bw = z->band_w;
    const bool draw = !z->dead && band;
    for (size_t i = 0; i < n; i++) {
        if (z->st == CUI__SX_DATA && (!z->bg_need || z->bg_done)) {
            /* The hot loop: a run of plain sixels. The image cursor and the band's extent live in locals while it runs (the
             * band's pixels are uint32_t, which could alias the decoder's ints, so fields would be reloaded each store). */
            int ix = z->ix, x0 = z->bx0, x1 = z->bx1;
            const int mw = z->max_w;
            const uint32_t col = z->col;
            for (; i < n; i++) {
                unsigned bits = (unsigned)d[i] - 0x3Fu;
                if (bits > 0x3Fu) break;
                if (ix >= mw) continue;
                if (bits && draw) {
                    uint32_t *p = band + ix;
                    if (bits & 1u) p[0] = col;
                    if (bits & 2u) p[bw] = col;
                    if (bits & 4u) p[2 * bw] = col;
                    if (bits & 8u) p[3 * bw] = col;
                    if (bits & 16u) p[4 * bw] = col;
                    if (bits & 32u) p[5 * bw] = col;
                    if (ix < x0) x0 = ix;
                    if (ix >= x1) x1 = ix + 1;
                }
                ix++;
            }
            z->ix = ix; z->bx0 = x0; z->bx1 = x1;
            if (i >= n) break;
        }
        unsigned c = d[i];
        if (c - 0x3Fu <= 0x3Fu) {                           /* a sixel after a command: '?' .. '~', six pixels of a column */
            int count = z->st == CUI__SX_DATA ? 1 : cui__vtp_pending(z);
            if (z->bg_need && !z->bg_done) cui__vtp_bg_fill(z);
            int room = z->max_w - z->ix;
            if (count > room) count = room;
            if (count <= 0) continue;
            unsigned bits = c - 0x3Fu;
            if (bits && draw) {
                uint32_t col = z->col;
                uint32_t *p = band + z->ix;
                if (count == 1) {
                    for (int b = 0; b < 6; b++, p += bw) if (bits & (1u << b)) *p = col;
                } else {
                    for (int b = 0; b < 6; b++, p += bw) if (bits & (1u << b)) for (int k = 0; k < count; k++) p[k] = col;
                }
                if (z->ix < z->bx0) z->bx0 = z->ix;
                if (z->ix + count > z->bx1) z->bx1 = z->ix + count;
            }
            z->ix += count;
            continue;
        }
        if (c - '0' <= 9u) {
            if (z->st == CUI__SX_DATA) continue;
            if (z->nprm == 0) { z->nprm = 1; z->prm[0] = 0; }
            if (z->nprm <= 6) { int *q = &z->prm[z->nprm - 1]; *q = CUI_MIN(*q * 10 + (int)(c - '0'), CUI__VTP_PARAM_MAX); }
            continue;
        }
        switch (c) {
        case ';':
            if (z->st != CUI__SX_DATA) { if (z->nprm == 0) { z->nprm = 1; z->prm[0] = 0; } if (z->nprm < 7) { z->nprm++; if (z->nprm <= 6) z->prm[z->nprm - 1] = 0; } }
            break;
        case '#': cui__vtp_pending(z); z->st = CUI__SX_COLOR; z->nprm = 0; break;
        case '!': cui__vtp_pending(z); z->st = CUI__SX_REPEAT; z->nprm = 0; break;
        case '"': cui__vtp_pending(z); z->st = CUI__SX_ATTR; z->nprm = 0; break;
        case '$': cui__vtp_pending(z); cui__vtp_cr(z); break;
        case '-': cui__vtp_pending(z); cui__vtp_next_line(v, x, z); break;
        case 0x1B: case 0x18: case 0x1A: return i;          /* the end: the screen takes it from here */
        default: break;                                     /* spaces, line breaks and other controls are ignored */
        }
    }
    return n;
}

void cui__vt_sixel_end(cui_vt *v) {
    cui__vt_x *x = cui__vt_ext(v);
    cui__vtp_six *z = x->six;
    x->reading = false;
    if (!z) return;
    if (z->st == CUI__SX_COLOR) cui__vtp_pending(z);        /* as Windows Terminal: only a pending color is applied at the end */
    z->st = CUI__SX_DATA;
    if (z->dead) return;
    if (z->bg_need && !z->bg_done) cui__vtp_bg_fill(z);
    cui__vtp_cr(z);
    cui__vtp_scroll_check(v, z);
    cui__vtp_commit(v, x, z, z->iy, z->iy + z->six_h);
    cui__vtp_band_clear(z);
    if (z->bg_done && z->bg_end > z->done_y) cui__vtp_commit(v, x, z, z->done_y, z->bg_end);
    if (!z->sdm) {                                          /* the cursor: the column it started in, the row of the last band's top */
        cui__vt_info in;
        cui__vt_get_info(v, &in);
        int row = z->oy + z->iy / CUI__VTP_CH;
        cui__vt_set_cursor(v, z->text_x, CUI_CLAMP(row, 0, in.rows - 1));
    }
}

/* For the tests: the pixel at virtual pixel (px, py) of a row (0 the top of the live screen, -1 the newest scrollback line)
 * as r | g << 8 | b << 16 | a << 24, 0 where there is no picture; and how many slices are kept. */
uint32_t cui__vt_pic_px(const cui_vt *v, int row, int px, int py) {
    const cui__vt_x *x = v ? cui__vt_ext(v) : NULL;
    if (!x || py < 0 || py >= CUI__VTP_CH || px < 0) return 0;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    const cui__vtp_slice *s = NULL;
    if (row >= 0 && row < in.rows && x->tab[in.cur]) {
        int slot = cui__vt_slot(v, in.cur, row);
        s = slot < x->tab_rows[in.cur] ? x->tab[in.cur][slot] : NULL;
    } else if (row < 0 && in.cur == 0) {
        for (int i = 0; i < x->sb_n; i++) if (x->sb[(x->sb_head + i) % x->sb_cap].line == in.sb_total + (uint64_t)(int64_t)row) s = cui__vtp_sb_at(x, i);
    }
    if (!s) return 0;
    int c = px / s->cw;                                     /* (px counts pixels of the row's own cell width) */
    if (c < s->c0 || c >= s->c1) return 0;
    const uint8_t *q = (const uint8_t *)&s->px[(size_t)py * (size_t)((s->c1 - s->c0) * s->cw) + (size_t)(px - s->c0 * s->cw)];
    return (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
}

int cui__vt_pic_count(const cui_vt *v) {
    const cui__vt_x *x = v ? cui__vt_ext(v) : NULL;
    return x ? x->tab_count[0] + x->tab_count[1] + x->sb_n : 0;
}

/* ------------------------------------------------------------------------ */
/* View: textures for the rows of picture on screen                         */
/* ------------------------------------------------------------------------ */

typedef struct { uint32_t id, ver; cui_texture tex; int w; bool used; } cui__vtp_tex;
typedef struct {
    const cui_vt *owner; uint32_t nonce;
    cui__vtp_tex *t; int n, cap;
    cui__vt_pic_draw *last; int last_rows;     /* the last frame's draws, shown again while a synchronized update is open */
} cui__vtp_view;

static void cui__vtp_view_clear(cui_ctx *ui, cui__vtp_view *w) {
    for (int i = 0; i < w->n; i++) cui_image_destroy(ui, w->t[i].tex);
    w->n = 0;
    if (w->last) memset(w->last, 0, sizeof *w->last * (size_t)w->last_rows);
}

static void cui__vtp_view_free(cui_ctx *ui, void *data) {
    cui__vtp_view *w = (cui__vtp_view *)data;
    cui__vtp_view_clear(ui, w);
    cui__free(ui, w->t); w->t = NULL; w->cap = 0;
    cui__free(ui, w->last); w->last = NULL; w->last_rows = 0;
    w->owner = NULL;
}

static const cui__vtp_slice *cui__vtp_row_slice(const cui_vt *v, const cui__vt_x *x, const cui__vt_info *in, int64_t arow) {
    if (arow >= (int64_t)in->sb_total) {
        int y = (int)(arow - (int64_t)in->sb_total);
        if (y >= in->rows || !x->tab[in->cur]) return NULL;
        int slot = cui__vt_slot(v, in->cur, y);
        return slot < x->tab_rows[in->cur] ? x->tab[in->cur][slot] : NULL;
    }
    if (in->cur != 0 || !x->sb_n || arow < 0) return NULL;
    int lo = 0, hi = x->sb_n - 1;                           /* the ring is ordered by line number */
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        uint64_t ln = x->sb[(x->sb_head + mid) % x->sb_cap].line;
        if (ln == (uint64_t)arow) return cui__vtp_sb_at(x, mid);
        if (ln < (uint64_t)arow) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

const cui__vt_pic_draw *cui__vt_pic_view(cui_ctx *ui, cui_id id, const cui_vt *v, int off, bool frozen) {
    const cui__vt_x *x = cui__vt_ext(v);
    if (!x->ever) return NULL;                              /* never a picture: no state, no work */
    cui_id sid = id ^ 0x51C5E1u;
    cui__vtp_view *w = (cui__vtp_view *)cui_state(ui, sid, sizeof *w);
    cui__state_set_dtor(ui, sid, cui__vtp_view_free);
    if (w->owner != v || w->nonce != x->nonce) { cui__vtp_view_clear(ui, w); w->owner = v; w->nonce = x->nonce; }
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    if (w->last_rows != in.rows) {
        cui__free(ui, w->last);
        w->last = (cui__vt_pic_draw *)cui__alloc(ui, sizeof *w->last * (size_t)in.rows);
        w->last_rows = w->last ? in.rows : 0;
        if (w->last) memset(w->last, 0, sizeof *w->last * (size_t)in.rows);
        frozen = false;
    }
    if (frozen) return w->last;                             /* textures are neither made nor freed while the old screen shows */
    if (!w->last) return NULL;
    for (int i = 0; i < w->n; i++) w->t[i].used = false;
    bool any = false;
    for (int y = 0; y < in.rows; y++) {
        cui__vt_pic_draw *d = &w->last[y];
        memset(d, 0, sizeof *d);
        const cui__vtp_slice *s = cui__vtp_row_slice(v, x, &in, (int64_t)in.sb_total - off + y);
        if (!s) continue;
        int tw = (s->c1 - s->c0) * s->cw, k = 0;
        while (k < w->n && w->t[k].id != s->id) k++;
        if (k == w->n) {
            if (w->n == w->cap) {
                int cap = w->cap ? w->cap * 2 : 32;
                cui__vtp_tex *t = (cui__vtp_tex *)cui__realloc(ui, w->t, sizeof *t * (size_t)cap);
                if (!t) continue;
                w->t = t; w->cap = cap;
            }
            cui_texture tex = cui_image_create(ui, tw, CUI__VTP_CH, s->px);
            if (!tex) continue;
            w->t[w->n++] = (cui__vtp_tex){ s->id, s->ver, tex, tw, false };
        } else if (w->t[k].ver != s->ver) {                 /* changed: updated in place, or made again when it widened */
            if (w->t[k].w == tw && ui->renderer.texture_update) ui->renderer.texture_update(ui->renderer.user, w->t[k].tex, 0, 0, tw, CUI__VTP_CH, s->px, tw * 4);
            else {
                cui_image_destroy(ui, w->t[k].tex);
                w->t[k].tex = cui_image_create(ui, tw, CUI__VTP_CH, s->px);
                w->t[k].w = tw;
            }
            w->t[k].ver = s->ver;
        }
        w->t[k].used = true;
        if (!w->t[k].tex) continue;
        d->tex = w->t[k].tex; d->c0 = s->c0; d->c1 = s->c1;
        any = true;
    }
    int keep = 0;                                           /* rows that left the screen take their textures with them */
    for (int i = 0; i < w->n; i++) {
        if (w->t[i].used) w->t[keep++] = w->t[i];
        else cui_image_destroy(ui, w->t[i].tex);
    }
    w->n = keep;
    return any ? w->last : NULL;
}
