/* The VT screen's kitty and iTerm2 pictures (opt in: CUI_VT_FEATURE_KITTY, CUI_VT_FEATURE_ITERM, CUI_VT_FEATURE_PICS_STILL).
 *
 * Both protocols end in the slices of cui_vt_sixel.c: a placed picture is scaled to the cells it covers on the virtual cell
 * sixel pictures use (cui__vt_pic_cw x 20 pixels), so scrolling, the scrollback, reflow, text written over it, ESC [ 3 J, the
 * picture budget and the view's textures are the ones sixel pictures have. A slice cell carries a tag: 1 for sixel and iTerm2
 * pictures, 2..255 for a kitty placement, which kitty's delete commands find the cells by.
 *
 * Kitty (sw.kovidgoyal.net/kitty/graphics-protocol, read for behaviour): ESC _ G <key=value,...> [; <base64>] ESC \.
 *   - transmit (a=t), transmit and place (a=T), place (a=p), delete (a=d), query (a=q); direct data only (t=d): files,
 *     temporary files and shared memory are refused with EBADF and never opened.
 *   - formats f=24 (RGB), 32 (RGBA, the default), 100 (PNG through stb_image); o=z zlib; chunks with m=1.
 *   - stored images (by i, or I which gets an i) are kept decoded within cui_vt_set_image_budget, the oldest dropped first.
 *   - a placement fills c x r cells (one given: the other from the picture's shape; none: its pixels over the virtual cell),
 *     from the source rectangle x, y, w, h. X, Y and z are read and not used (pictures are drawn as sixel ones are: over the
 *     text of their cells, and text written into a cell takes the cell's piece).
 *   - the cursor goes to the column after the picture on its last row, the screen scrolling up so the picture fits, unless
 *     C=1 or CUI_VT_FEATURE_PICS_STILL (then it stays, and the picture is cut at the screen's bottom).
 *   - replies ESC _ G i=<id>[,I=<n>][,p=<p>] ; OK|ECODE:text ESC \ when i or I was given, as q allows.
 *   - Unicode placeholders: a placement with U=1 is virtual; cells holding U+10EEEE (written by the program as text, with
 *     the image id in their foreground color and row, column and the id's high byte as diacritics) get the piece of the
 *     picture they name, painted when the cell is written and again as each diacritic joins it (CUI_VT_FEATURE_CLUSTERS
 *     keeps the diacritics in the cell; without it only the ones a cell takes from its left neighbour work).
 *
 * iTerm2 (iterm2.com/documentation-images.html): ESC ] 1337 ; File = args : <base64> BEL (or ST), and iTerm2 3.5's
 * MultipartFile=args, FilePart=<base64>..., FileEnd. inline=1 draws the file (PNG and JPEG through stb_image); width and
 * height in cells, Npx, N% of the screen or auto; preserveAspectRatio (default 1) fits the picture inside that box; the cursor
 * as kitty's, unless doNotMoveCursor=1 or PICS_STILL. Anything without inline=1 is a download and is dropped. */
#include "cui_internal.h"

#include <limits.h>

#define CUI__VTK_B64_MAX ((size_t)64 << 20)     /* base64 bytes of one picture */
#define CUI__VTK_BUDGET ((size_t)32 << 20)      /* stored kitty images, decoded */
#define CUI__VTK_DIM 16384                      /* a picture's side, in pixels */
#define CUI__VTK_ROWS 4000                      /* rows a placement may take */

typedef struct { uint8_t *p; size_t n, cap; uint32_t q; int qn; bool bad, over; } cui__vtk_blob;   /* base64 decoded as it comes */
typedef struct { uint32_t id, num; int w, h; uint8_t *px; size_t bytes; uint32_t seq; } cui__vtk_img;
typedef struct { bool used, virt; uint32_t id, pid; int cols, rows; uint32_t seq; } cui__vtk_tag;
typedef struct {
    char a, t, o, d; int f, s, v, x, y, w, h, c, r, C, q, m, U;
    uint32_t i, I, p; bool has_i, has_I;
} cui__vtk_cmd;
typedef struct { bool ok; int scr, x, y; uint32_t id; int row, col, msb; } cui__vtk_ph;

struct cui__vtk {
    size_t budget, used;
    cui__vtk_img *img; int nimg, cimg;
    uint32_t seq, next_id;
    cui__vtk_tag tag[256];
    /* kitty, being read */
    bool in_data;                        /* past the ';' */
    char ctl[512]; int ctl_n;
    cui__vtk_cmd pend; bool chunking;    /* the first chunk's keys while m=1 chunks come */
    cui__vtk_blob data;
    /* iTerm2, being read */
    int it_mode;                         /* 0 the header, 1 base64, 2 skipped */
    char it_hdr[4096]; int it_hdr_n;
    bool it_multi; char it_args[4096];
    cui__vtk_blob it;
    cui__vtk_ph ph_l, ph_c;              /* the placeholder cell painted before the last one, and the last one */
};

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static void cui__vtk_blob_free(cui__vtk_blob *b) { free(b->p); memset(b, 0, sizeof *b); }

void cui__vtk_free(cui__vtk *k) {
    if (!k) return;
    for (int i = 0; i < k->nimg; i++) free(k->img[i].px);
    free(k->img);
    cui__vtk_blob_free(&k->data);
    cui__vtk_blob_free(&k->it);
    free(k);
}

size_t cui__vtk_memory(const cui__vtk *k) {
    if (!k) return 0;
    return sizeof *k + k->used + (size_t)k->cimg * sizeof(cui__vtk_img) + k->data.cap + k->it.cap;
}

void cui__vtk_reset(cui__vtk *k) {
    if (!k) return;
    k->in_data = false; k->ctl_n = 0; k->chunking = false;
    cui__vtk_blob_free(&k->data);
    k->it_mode = 0; k->it_hdr_n = 0; k->it_multi = false;
    cui__vtk_blob_free(&k->it);
    k->ph_l.ok = k->ph_c.ok = false;
}

static cui__vtk *cui__vtk_get(cui_vt *v) {
    cui__vtk **slot = cui__vt_x_kitty(v);
    if (!slot) return NULL;
    if (!*slot) {
        *slot = (cui__vtk *)calloc(1, sizeof **slot);
        if (*slot) { (*slot)->budget = CUI__VTK_BUDGET; (*slot)->next_id = 0x80000000u; }
    }
    return *slot;
}

void cui_vt_set_image_budget(cui_vt *v, size_t bytes) {
    if (!v) return;
    cui__vtk *k = cui__vtk_get(v);
    if (!k) return;
    k->budget = bytes;
    while (k->used > k->budget && k->nimg) {             /* the oldest go */
        int o = 0;
        for (int i = 1; i < k->nimg; i++) if (k->img[i].seq < k->img[o].seq) o = i;
        k->used -= k->img[o].bytes; free(k->img[o].px);
        k->img[o] = k->img[--k->nimg];
    }
}

/* ------------------------------------------------------------------------ */
/* Base64, as it comes                                                      */
/* ------------------------------------------------------------------------ */

static int cui__vtk_b64(unsigned c) {
    if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
    if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
    if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static void cui__vtk_put(cui__vtk_blob *b, const uint8_t *s, int n) {
    if (b->over) return;
    if (b->n + (size_t)n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + (size_t)n) cap *= 2;
        if (cap > CUI__VTK_B64_MAX) cap = CUI__VTK_B64_MAX;
        if (b->n + (size_t)n > cap) { b->over = true; return; }
        uint8_t *p = (uint8_t *)realloc(b->p, cap);
        if (!p) { b->over = true; return; }
        b->p = p; b->cap = cap;
    }
    memcpy(b->p + b->n, s, (size_t)n);
    b->n += (size_t)n;
}

static void cui__vtk_b64_feed(cui__vtk_blob *b, const unsigned char *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned c = d[i];
        int v = cui__vtk_b64(c);
        if (v < 0) {
            if (c == '=') {                                  /* padding: what the quad holds goes out */
                if (b->qn == 2) { uint8_t o = (uint8_t)(b->q >> 4); cui__vtk_put(b, &o, 1); }
                else if (b->qn == 3) { uint8_t o[2] = { (uint8_t)(b->q >> 10), (uint8_t)(b->q >> 2) }; cui__vtk_put(b, o, 2); }
                b->q = 0; b->qn = 0;
            } else if (c != ' ' && c != '\r' && c != '\n' && c != '\t') b->bad = true;
            continue;
        }
        b->q = b->q << 6 | (uint32_t)v;
        if (++b->qn == 4) {
            uint8_t o[3] = { (uint8_t)(b->q >> 16), (uint8_t)(b->q >> 8), (uint8_t)b->q };
            cui__vtk_put(b, o, 3);
            b->q = 0; b->qn = 0;
        }
    }
}

static void cui__vtk_b64_flush(cui__vtk_blob *b) {          /* an unpadded end */
    const unsigned char pad = '=';
    if (b->qn >= 2) cui__vtk_b64_feed(b, &pad, 1);
    else if (b->qn == 1) b->bad = true;
    b->q = 0; b->qn = 0;
}

/* ------------------------------------------------------------------------ */
/* Pictures                                                                 */
/* ------------------------------------------------------------------------ */

static bool cui__vtk_still(const cui_vt *v) { return (cui__vt_x_features(v) & CUI_VT_FEATURE_PICS_STILL) != 0; }

/* rgba (w x h, stride pixels a row) on cols x rows cells at the cursor, with tag: the cursor as the protocols move it unless
 * still. False when the picture budget cut it off. */
static bool cui__vtk_draw(cui_vt *v, const uint8_t *rgba, int w, int h, int stride, int cols, int rows, uint8_t tag, bool still) {
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    int x0 = in.cx, y = in.cy;
    if (still || cui__vtk_still(v)) return cui__vt_pic_put(v, rgba, w, h, stride, cols, rows, 0, 0, cols, rows, x0, y, tag, 0);
    bool ok = true;
    uint32_t seq = cui__vt_pic_seq(v);
    for (int r = 0; r < rows; r++) {
        int yy = y + r;
        if (yy > in.sbot && in.cy <= in.sbot) {           /* the picture scrolls the region up a row at a time, as text would */
            cui__vt_scroll_lines(v, 1);
            y--; yy--;
        }
        if (ok) ok = cui__vt_pic_put(v, rgba, w, h, stride, cols, rows, 0, r, cols, 1, x0, yy, tag, seq);
    }
    cui__vt_set_cursor(v, CUI_MIN(x0 + cols, in.cols - 1), CUI_CLAMP(y + rows - 1, 0, in.rows - 1));
    return ok;
}

/* the cells a picture of sw x sh pixels takes: c, r as given, one from the other, or from its pixels */
static void cui__vtk_cells(const cui_vt *v, int sw, int sh, int c, int r, int *cols, int *rows) {
    int cw = cui__vt_pic_cw(v);
    if (c > 0 && r > 0) { *cols = c; *rows = r; }
    else if (c > 0) { *cols = c; *rows = (int)CUI_MAX(1, ((int64_t)c * cw * sh * 2 / sw / 20 + 1) / 2); }
    else if (r > 0) { *rows = r; *cols = (int)CUI_MAX(1, ((int64_t)r * 20 * sw * 2 / sh / cw + 1) / 2); }
    else { *cols = (sw + cw - 1) / cw; *rows = (sh + 19) / 20; }
    *cols = CUI_CLAMP(*cols, 1, 4096);
    *rows = CUI_CLAMP(*rows, 1, CUI__VTK_ROWS);
}

/* ------------------------------------------------------------------------ */
/* Kitty: images, tags                                                      */
/* ------------------------------------------------------------------------ */

static cui__vtk_img *cui__vtk_find(cui__vtk *k, uint32_t id) {
    for (int i = 0; i < k->nimg; i++) if (k->img[i].id == id) return &k->img[i];
    return NULL;
}

static cui__vtk_img *cui__vtk_find_num(cui__vtk *k, uint32_t num) {
    cui__vtk_img *best = NULL;                           /* the newest image with that number */
    for (int i = 0; i < k->nimg; i++) if (k->img[i].num == num && (!best || k->img[i].seq > best->seq)) best = &k->img[i];
    return best;
}

static void cui__vtk_drop_img(cui__vtk *k, cui__vtk_img *m) {
    k->used -= m->bytes; free(m->px);
    *m = k->img[--k->nimg];
}

/* keeps px (w x h RGBA, malloc'd) as image id; false when it cannot (then px is still the caller's) */
static bool cui__vtk_store(cui__vtk *k, uint32_t id, uint32_t num, uint8_t *px, int w, int h) {
    size_t bytes = (size_t)w * (size_t)h * 4;
    cui__vtk_img *old = cui__vtk_find(k, id);
    if (old) cui__vtk_drop_img(k, old);
    if (bytes > k->budget) return false;
    while (k->used + bytes > k->budget && k->nimg) {
        int o = 0;
        for (int i = 1; i < k->nimg; i++) if (k->img[i].seq < k->img[o].seq) o = i;
        cui__vtk_drop_img(k, &k->img[o]);
    }
    if (k->nimg == k->cimg) {
        int cap = k->cimg ? k->cimg * 2 : 8;
        cui__vtk_img *n = (cui__vtk_img *)realloc(k->img, sizeof *n * (size_t)cap);
        if (!n) return false;
        k->img = n; k->cimg = cap;
    }
    k->img[k->nimg++] = (cui__vtk_img){ id, num, w, h, px, bytes, ++k->seq };
    k->used += bytes;
    return true;
}

static bool cui__vtk_is(void *u, uint8_t t, int row, int col) { (void)row; (void)col; return ((const uint8_t *)u)[t] != 0; }

/* a tag for a placement of image id (pid: its placement id, 0 none). The placement id's old placement is taken off first; with
 * every tag in use, the tags whose cells are gone are freed, else the oldest placement's cells become anonymous (tag 1). */
static uint8_t cui__vtk_tag_new(cui_vt *v, cui__vtk *k, uint32_t id, uint32_t pid, bool virt, int cols, int rows) {
    int t = 0;
    if (pid) for (int i = 2; i < 256; i++) if (k->tag[i].used && k->tag[i].id == id && k->tag[i].pid == pid) {
        uint8_t set[256] = { 0 };
        set[i] = 1;
        cui__vt_pic_erase_if(v, cui__vtk_is, set, false);
        t = i;
        break;
    }
    if (!t) for (int i = 2; i < 256 && !t; i++) if (!k->tag[i].used) t = i;
    if (!t) {
        uint8_t used[256];
        cui__vt_pic_tags(v, used);
        for (int i = 2; i < 256; i++) if (k->tag[i].used && !k->tag[i].virt && !used[i]) { k->tag[i].used = false; if (!t) t = i; }
    }
    if (!t) {
        t = 2;
        for (int i = 3; i < 256; i++) if (k->tag[i].seq < k->tag[t].seq) t = i;
        cui__vt_pic_retag(v, (uint8_t)t, 1);
    }
    k->tag[t] = (cui__vtk_tag){ true, virt, id, pid, cols, rows, ++k->seq };
    return (uint8_t)t;
}

/* ------------------------------------------------------------------------ */
/* Kitty: commands                                                          */
/* ------------------------------------------------------------------------ */

static void cui__vtk_parse(const char *s, int n, cui__vtk_cmd *c) {
    memset(c, 0, sizeof *c);
    c->a = 't'; c->t = 'd'; c->f = 32;
    for (int i = 0; i < n;) {
        char key = s[i];
        int e = i;
        while (e < n && s[e] != ',') e++;
        if (e - i >= 2 && s[i + 1] == '=') {
            const char *val = s + i + 2;
            int vl = e - i - 2;
            long long num = 0;
            bool neg = vl > 0 && val[0] == '-';
            for (int j = neg ? 1 : 0; j < vl && val[j] >= '0' && val[j] <= '9'; j++) if (num < 0xFFFFFFFFLL) num = num * 10 + (val[j] - '0');
            if (neg) num = -num;
            int iv = (int)CUI_CLAMP(num, (long long)INT_MIN, (long long)INT_MAX);
            char ch = vl > 0 ? val[0] : 0;
            switch (key) {
            case 'a': c->a = ch; break;
            case 't': c->t = ch; break;
            case 'o': c->o = ch; break;
            case 'd': c->d = ch; break;
            case 'f': c->f = iv; break;
            case 's': c->s = iv; break;
            case 'v': c->v = iv; break;
            case 'x': c->x = iv; break;
            case 'y': c->y = iv; break;
            case 'w': c->w = iv; break;
            case 'h': c->h = iv; break;
            case 'c': c->c = iv; break;
            case 'r': c->r = iv; break;
            case 'C': c->C = iv; break;
            case 'q': c->q = iv; break;
            case 'm': c->m = iv; break;
            case 'U': c->U = iv; break;
            case 'i': c->i = (uint32_t)(num < 0 ? 0 : num); c->has_i = true; break;
            case 'I': c->I = (uint32_t)(num < 0 ? 0 : num); c->has_I = true; break;
            case 'p': c->p = (uint32_t)(num < 0 ? 0 : num); break;
            default: break;                              /* X, Y, z, S, O, P, Q, H, V ... read and not used */
            }
        }
        i = e + 1;
    }
}

static void cui__vtk_reply(cui_vt *v, const cui__vtk_cmd *c, const char *msg) {
    bool ok = !strcmp(msg, "OK");
    if ((!c->has_i && !c->has_I) || c->q >= 2 || (ok && c->q == 1)) return;
    char b[300];
    int n = snprintf(b, sizeof b, "\x1b_Gi=%u", (unsigned)c->i);
    if (c->has_I) n += snprintf(b + n, sizeof b - (size_t)n, ",I=%u", (unsigned)c->I);
    if (c->p) n += snprintf(b + n, sizeof b - (size_t)n, ",p=%u", (unsigned)c->p);
    snprintf(b + n, sizeof b - (size_t)n, ";%s\x1b\\", msg);
    cui__vt_reply(v, b);
}

/* the data of a transmission as RGBA (malloc'd), or NULL with *err set */
static uint8_t *cui__vtk_decode(const cui__vtk_cmd *c, cui__vtk_blob *b, int *w, int *h, const char **err) {
    *err = NULL;
    if (c->t != 'd') { *err = "EBADF:only direct data is read"; return NULL; }
    if (b->over) { *err = "EFBIG:the picture is too big"; return NULL; }
    if (b->bad) { *err = "EINVAL:bad base64 data"; return NULL; }
    const uint8_t *src = b->p;
    size_t n = b->n;
    uint8_t *raw = NULL;
    if (c->f == 24 || c->f == 32) {
        int bpp = c->f / 8;
        if (c->s <= 0 || c->v <= 0 || c->s > CUI__VTK_DIM || c->v > CUI__VTK_DIM) { *err = "EINVAL:the width and height are needed"; return NULL; }
        size_t need = (size_t)c->s * (size_t)c->v * (size_t)bpp;
        if (c->o == 'z') {
            raw = (uint8_t *)malloc(need);
            if (!raw) { *err = "ENOMEM:out of memory"; return NULL; }
            cui_inflate_result r = cui_inflate(src, n, raw, need, CUI_INFLATE_ZLIB);
            if (r.status != CUI_INFLATE_OK) { free(raw); *err = "EINVAL:the zlib data does not inflate"; return NULL; }
            if (r.out_len < need) { free(raw); *err = "ENODATA:insufficient image data"; return NULL; }
            src = raw; n = need;
        }
        if (n < need) { *err = "ENODATA:insufficient image data"; return NULL; }
        uint8_t *px = (uint8_t *)malloc((size_t)c->s * (size_t)c->v * 4);
        if (!px) { free(raw); *err = "ENOMEM:out of memory"; return NULL; }
        size_t np = (size_t)c->s * (size_t)c->v;
        if (bpp == 4) memcpy(px, src, np * 4);
        else for (size_t i = 0; i < np; i++) { px[i * 4] = src[i * 3]; px[i * 4 + 1] = src[i * 3 + 1]; px[i * 4 + 2] = src[i * 3 + 2]; px[i * 4 + 3] = 255; }
        free(raw);
        *w = c->s; *h = c->v;
        return px;
    }
    if (c->f != 100) { *err = "EINVAL:unknown format"; return NULL; }
    if (c->o == 'z') {
        cui_inflate_result r;
        raw = (uint8_t *)cui_inflate_alloc(src, n, CUI_INFLATE_ZLIB, 0, CUI__VTK_B64_MAX, &r);
        if (!raw) { *err = "EINVAL:the zlib data does not inflate"; return NULL; }
        src = raw; n = r.out_len;
    }
    int iw = 0, ih = 0;
    unsigned char *dec = n > 0 && n < (size_t)INT_MAX ? cui__png_decode(src, (int)n, &iw, &ih) : NULL;
    free(raw);
    if (!dec || iw <= 0 || ih <= 0 || iw > CUI__VTK_DIM || ih > CUI__VTK_DIM) { if (dec) cui__png_free(dec); *err = "EBADPNG:the PNG does not decode"; return NULL; }
    uint8_t *px = (uint8_t *)malloc((size_t)iw * (size_t)ih * 4);
    if (px) memcpy(px, dec, (size_t)iw * (size_t)ih * 4);
    cui__png_free(dec);
    if (!px) { *err = "ENOMEM:out of memory"; return NULL; }
    *w = iw; *h = ih;
    return px;
}

/* a placement of px (w x h) by the keys of c: drawn at the cursor, or kept as a virtual one (U=1) */
static const char *cui__vtk_place(cui_vt *v, cui__vtk *k, const cui__vtk_cmd *c, uint32_t id, const uint8_t *px, int w, int h) {
    int sx = CUI_CLAMP(c->x, 0, w - 1), sy = CUI_CLAMP(c->y, 0, h - 1);
    int sw = c->w > 0 ? CUI_MIN(c->w, w - sx) : w - sx, sh = c->h > 0 ? CUI_MIN(c->h, h - sy) : h - sy;
    if (sw <= 0 || sh <= 0) return "EINVAL:the source rectangle is empty";
    int cols, rows;
    cui__vtk_cells(v, sw, sh, c->c, c->r, &cols, &rows);
    if (c->U == 1) {
        if (!id) return "EINVAL:a virtual placement needs an image id";
        cui__vtk_tag_new(v, k, id, c->p, true, cols, rows);
        return "OK";
    }
    uint8_t tag = cui__vtk_tag_new(v, k, id, c->p, false, cols, rows);
    if (!cui__vtk_draw(v, px + ((size_t)sy * (size_t)w + (size_t)sx) * 4, sw, sh, w, cols, rows, tag, c->C == 1)) return "ENOSPC:the picture budget is full";
    return "OK";
}

/* which placements a delete takes: set[t] for each tag */
typedef struct { const cui__vtk *k; char d; int x, y; uint32_t id, lo, hi, pid; uint8_t *set; } cui__vtk_del;

static bool cui__vtk_del_pick(void *u, uint8_t t, int row, int col) {
    cui__vtk_del *q = (cui__vtk_del *)u;
    if (t < 2 || !q->k->tag[t].used) return false;
    switch (q->d) {
    case 'x': if (col == q->x) q->set[t] = 1; break;
    case 'y': if (row == q->y) q->set[t] = 1; break;
    case 'p': if (col == q->x && row == q->y) q->set[t] = 1; break;
    default: break;
    }
    return false;
}

static void cui__vtk_delete(cui_vt *v, cui__vtk *k, const cui__vtk_cmd *c) {
    char d = c->d ? c->d : 'a';
    bool up = d >= 'A' && d <= 'Z';
    char lo = up ? (char)(d + 32) : d;
    uint8_t set[256] = { 0 };
    bool screen_only = false, any_virt = false;
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    switch (lo) {
    case 'a': case 'z':                                 /* every placement on the screen (z: as a, z-indexes are not kept) */
        screen_only = true;
        for (int t = 2; t < 256; t++) if (k->tag[t].used) { set[t] = 1; any_virt |= k->tag[t].virt; }
        break;
    case 'i': case 'n': case 'r': {
        uint32_t a = c->i, b = c->i;
        if (lo == 'n') { cui__vtk_img *m = cui__vtk_find_num(k, c->I); if (!m) return; a = b = m->id; }
        if (lo == 'r') { a = (uint32_t)CUI_MAX(c->x, 0); b = (uint32_t)CUI_MAX(c->y, 0); }
        for (int t = 2; t < 256; t++)
            if (k->tag[t].used && k->tag[t].id >= a && k->tag[t].id <= b && (lo != 'i' || !c->p || k->tag[t].pid == c->p)) set[t] = 1;
        break;
    }
    case 'c': case 'p': case 'x': case 'y': {
        cui__vtk_del q = { k, lo == 'c' ? 'p' : lo, lo == 'c' ? in.cx : c->x - 1, lo == 'c' ? in.cy : c->y - 1, 0, 0, 0, 0, set };
        cui__vt_pic_erase_if(v, cui__vtk_del_pick, &q, true);   /* finds the tags (erases nothing) */
        break;
    }
    default: return;                                     /* f (animation frames) and the rest: nothing here */
    }
    uint32_t ids[256]; int nid = 0;
    for (int t = 2; t < 256; t++) if (set[t]) ids[nid++] = k->tag[t].id;
    cui__vt_pic_erase_if(v, cui__vtk_is, set, screen_only);
    uint8_t used[256];
    cui__vt_pic_tags(v, used);
    for (int t = 2; t < 256; t++) {
        if (!set[t]) continue;
        if (k->tag[t].virt || !used[t]) k->tag[t].used = false;   /* placements whose cells all went (a virtual one goes when named) */
    }
    (void)any_virt;
    if (!up) return;
    for (int j = 0; j < nid; j++) {                      /* capital: the image data goes too when no placement shows it */
        bool ref = false;
        for (int t = 2; t < 256 && !ref; t++) ref = k->tag[t].used && k->tag[t].id == ids[j];
        cui__vtk_img *m = ref ? NULL : cui__vtk_find(k, ids[j]);
        if (m) cui__vtk_drop_img(k, m);
    }
    if (lo == 'a') for (int i = k->nimg - 1; i >= 0; i--) {
        bool ref = false;
        for (int t = 2; t < 256 && !ref; t++) ref = k->tag[t].used && k->tag[t].id == k->img[i].id;
        if (!ref) cui__vtk_drop_img(k, &k->img[i]);
    }
}

static void cui__vtk_exec(cui_vt *v, cui__vtk *k, cui__vtk_cmd *c) {
    if (c->a == 'd') { cui__vtk_delete(v, k, c); return; }
    if (c->a == 'q' || c->a == 't' || c->a == 'T') {
        int w = 0, h = 0;
        const char *err;
        uint8_t *px = cui__vtk_decode(c, &k->data, &w, &h, &err);
        if (!px) { cui__vtk_reply(v, c, err); return; }
        if (c->a == 'q') { free(px); cui__vtk_reply(v, c, "OK"); return; }
        if (c->has_I && !c->has_i) { c->i = k->next_id++; if (!k->next_id) k->next_id = 0x80000000u; }
        uint32_t id = c->i;
        const char *res = "OK";
        bool kept = id && cui__vtk_store(k, id, c->I, px, w, h);
        if (c->a == 'T') res = cui__vtk_place(v, k, c, id, px, w, h);
        if (!kept) free(px);
        cui__vtk_reply(v, c, res);
        return;
    }
    if (c->a == 'p') {
        cui__vtk_img *m = c->has_i && c->i ? cui__vtk_find(k, c->i) : c->has_I ? cui__vtk_find_num(k, c->I) : NULL;
        if (!m) { cui__vtk_reply(v, c, "ENOENT:no such image"); return; }
        if (!c->has_i) c->i = m->id;
        m->seq = ++k->seq;                               /* used: the last to be dropped */
        cui__vtk_reply(v, c, cui__vtk_place(v, k, c, m->id, m->px, m->w, m->h));
        return;
    }
    /* a=f, a=a, a=c (animation) and others: not drawn */
}

bool cui__vt_kitty_begin(cui_vt *v) {
    if (!(cui__vt_x_features(v) & CUI_VT_FEATURE_KITTY)) return false;
    cui__vtk *k = cui__vtk_get(v);
    if (!k) return false;
    k->in_data = false; k->ctl_n = 0;
    if (!k->chunking) { cui__vtk_blob_free(&k->data); }
    return true;
}

size_t cui__vt_kitty_feed(cui_vt *v, const unsigned char *d, size_t n) {
    cui__vtk *k = *cui__vt_x_kitty(v);
    size_t i = 0;
    if (!k->in_data) {
        for (; i < n; i++) {
            unsigned char c = d[i];
            if (c == 0x1B || c == 0x07 || c == 0x18 || c == 0x1A) return i;
            if (c == ';') { k->in_data = true; i++; break; }
            if (k->ctl_n + 1 < (int)sizeof k->ctl) k->ctl[k->ctl_n++] = (char)c;
        }
    }
    size_t s = i;
    while (i < n && d[i] != 0x1B && d[i] != 0x07 && d[i] != 0x18 && d[i] != 0x1A) i++;
    if (i > s) cui__vtk_b64_feed(&k->data, d + s, i - s);
    return i;
}

void cui__vt_kitty_end(cui_vt *v) {
    cui__vtk *k = *cui__vt_x_kitty(v);
    cui__vtk_cmd c;
    cui__vtk_parse(k->ctl, k->ctl_n, &c);
    if (k->chunking) {                                   /* a later chunk: only m and q count */
        if (c.q) k->pend.q = c.q;
        if (c.m == 1) return;
        k->chunking = false;
        c = k->pend;
    } else if (c.m == 1) {
        k->pend = c;
        k->chunking = true;
        return;
    }
    cui__vtk_b64_flush(&k->data);
    cui__vtk_exec(v, k, &c);
    cui__vtk_blob_free(&k->data);
}

/* ------------------------------------------------------------------------ */
/* Kitty: Unicode placeholders                                              */
/* ------------------------------------------------------------------------ */

/* kitty's rowcolumn-diacritics: the nth combining character stands for the number n (297 of them), as ranges */
static const uint32_t cui__vtk_diac[][2] = {
    { 0x0305, 0x0305 }, { 0x030D, 0x030E }, { 0x0310, 0x0310 }, { 0x0312, 0x0312 }, { 0x033D, 0x033F }, { 0x0346, 0x0346 },
    { 0x034A, 0x034C }, { 0x0350, 0x0352 }, { 0x0357, 0x0357 }, { 0x035B, 0x035B }, { 0x0363, 0x036F }, { 0x0483, 0x0487 },
    { 0x0592, 0x0595 }, { 0x0597, 0x0599 }, { 0x059C, 0x05A1 }, { 0x05A8, 0x05A9 }, { 0x05AB, 0x05AC }, { 0x05AF, 0x05AF },
    { 0x05C4, 0x05C4 }, { 0x0610, 0x0617 }, { 0x0657, 0x065B }, { 0x065D, 0x065E }, { 0x06D6, 0x06DC }, { 0x06DF, 0x06E2 },
    { 0x06E4, 0x06E4 }, { 0x06E7, 0x06E8 }, { 0x06EB, 0x06EC }, { 0x0730, 0x0730 }, { 0x0732, 0x0733 }, { 0x0735, 0x0736 },
    { 0x073A, 0x073A }, { 0x073D, 0x073D }, { 0x073F, 0x0741 }, { 0x0743, 0x0743 }, { 0x0745, 0x0745 }, { 0x0747, 0x0747 },
    { 0x0749, 0x074A }, { 0x07EB, 0x07F1 }, { 0x07F3, 0x07F3 }, { 0x0816, 0x0819 }, { 0x081B, 0x0823 }, { 0x0825, 0x0827 },
    { 0x0829, 0x082D }, { 0x0951, 0x0951 }, { 0x0953, 0x0954 }, { 0x0F82, 0x0F83 }, { 0x0F86, 0x0F87 }, { 0x135D, 0x135F },
    { 0x17DD, 0x17DD }, { 0x193A, 0x193A }, { 0x1A17, 0x1A17 }, { 0x1A75, 0x1A7C }, { 0x1B6B, 0x1B6B }, { 0x1B6D, 0x1B73 },
    { 0x1CD0, 0x1CD2 }, { 0x1CDA, 0x1CDB }, { 0x1CE0, 0x1CE0 }, { 0x1DC0, 0x1DC1 }, { 0x1DC3, 0x1DC9 }, { 0x1DCB, 0x1DCC },
    { 0x1DD1, 0x1DE6 }, { 0x1DFE, 0x1DFE }, { 0x20D0, 0x20D1 }, { 0x20D4, 0x20D7 }, { 0x20DB, 0x20DC }, { 0x20E1, 0x20E1 },
    { 0x20E7, 0x20E7 }, { 0x20E9, 0x20E9 }, { 0x20F0, 0x20F0 }, { 0x2CEF, 0x2CF1 }, { 0x2DE0, 0x2DFF }, { 0xA66F, 0xA66F },
    { 0xA67C, 0xA67D }, { 0xA6F0, 0xA6F1 }, { 0xA8E0, 0xA8F1 }, { 0xAAB0, 0xAAB0 }, { 0xAAB2, 0xAAB3 }, { 0xAAB7, 0xAAB8 },
    { 0xAABE, 0xAABF }, { 0xAAC1, 0xAAC1 }, { 0xFE20, 0xFE26 }, { 0x10A0F, 0x10A0F }, { 0x10A38, 0x10A38 },
    { 0x1D185, 0x1D189 }, { 0x1D1AA, 0x1D1AD }, { 0x1D242, 0x1D244 },
};

int cui__vtk_diacritic(uint32_t cp);
int cui__vtk_diacritic(uint32_t cp) {                  /* its number, -1 for another character */
    int n = 0;
    for (size_t i = 0; i < sizeof cui__vtk_diac / sizeof cui__vtk_diac[0]; i++) {
        if (cp < cui__vtk_diac[i][0]) return -1;
        if (cp <= cui__vtk_diac[i][1]) return n + (int)(cp - cui__vtk_diac[i][0]);
        n += (int)(cui__vtk_diac[i][1] - cui__vtk_diac[i][0] + 1);
    }
    return -1;
}

void cui__vt_kitty_ph(cui_vt *v, int x, int y, uint32_t id24, const uint32_t *cps, int n) {
    cui__vtk **slot = cui__vt_x_kitty(v);
    cui__vtk *k = slot ? *slot : NULL;
    if (!k) return;                                      /* no picture ever came: nothing to show */
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    int row = n > 1 ? cui__vtk_diacritic(cps[1]) : -1, col = n > 2 ? cui__vtk_diacritic(cps[2]) : -1, msb = n > 3 ? cui__vtk_diacritic(cps[3]) : -1;
    if (!(k->ph_c.ok && k->ph_c.scr == in.cur && k->ph_c.x == x && k->ph_c.y == y)) k->ph_l = k->ph_c;   /* a new cell (not this one again) */
    const cui__vtk_ph *l = &k->ph_l;
    bool left = l->ok && l->scr == in.cur && l->y == y && l->x == x - 1 && l->id == id24;
    if (row < 0) { row = left ? l->row : 0; if (col < 0) col = left ? l->col + 1 : 0; if (msb < 0) msb = left ? l->msb : 0; }
    else if (col < 0) { col = left && l->row == row ? l->col + 1 : 0; if (msb < 0) msb = left && l->row == row ? l->msb : 0; }
    else if (msb < 0) msb = left && l->row == row && l->col + 1 == col ? l->msb : 0;
    k->ph_c = (cui__vtk_ph){ true, in.cur, x, y, id24, row, col, msb };
    uint32_t id = (id24 & 0xFFFFFFu) | (uint32_t)(msb & 255) << 24;
    int t = 0;
    for (int i = 2; i < 256; i++) if (k->tag[i].used && k->tag[i].virt && k->tag[i].id == id && (!t || k->tag[i].seq > k->tag[t].seq)) t = i;
    cui__vtk_img *m = t ? cui__vtk_find(k, id) : NULL;
    if (!m || row >= k->tag[t].rows || col >= k->tag[t].cols) {          /* nothing there: a piece painted before for this cell goes */
        cui__vt_pic_erase(v, in.cur, cui__vt_slot(v, in.cur, y), x, x + 1);
        return;
    }
    cui__vt_pic_put(v, m->px, m->w, m->h, m->w, k->tag[t].cols, k->tag[t].rows, col, row, 1, 1, x, y, (uint8_t)t, 0);
}

/* ------------------------------------------------------------------------ */
/* iTerm2                                                                   */
/* ------------------------------------------------------------------------ */

bool cui__vt_iterm_begin(cui_vt *v) {
    if (!(cui__vt_x_features(v) & CUI_VT_FEATURE_ITERM)) return false;
    cui__vtk *k = cui__vtk_get(v);
    if (!k) return false;
    k->it_mode = 0; k->it_hdr_n = 0;
    return true;
}

size_t cui__vt_iterm_feed(cui_vt *v, const unsigned char *d, size_t n) {
    cui__vtk *k = *cui__vt_x_kitty(v);
    size_t i = 0;
    if (k->it_mode == 0) {
        for (; i < n; i++) {
            unsigned char c = d[i];
            if (c == 0x1B || c == 0x07 || c == 0x18 || c == 0x1A) return i;
            if (k->it_hdr_n + 1 >= (int)sizeof k->it_hdr) { k->it_mode = 2; break; }
            k->it_hdr[k->it_hdr_n++] = (char)c;
            k->it_hdr[k->it_hdr_n] = 0;
            if (c == ':' && !strncmp(k->it_hdr, "File=", 5)) {        /* the file's base64 follows */
                cui__vtk_blob_free(&k->it);
                k->it_multi = false;
                k->it_mode = 1; i++; break;
            }
            if (k->it_hdr_n == 9 && !strncmp(k->it_hdr, "FilePart=", 9)) { k->it_mode = k->it_multi ? 1 : 2; i++; break; }
        }
    }
    size_t s = i;
    while (i < n && d[i] != 0x1B && d[i] != 0x07 && d[i] != 0x18 && d[i] != 0x1A) i++;
    if (k->it_mode == 1 && i > s) cui__vtk_b64_feed(&k->it, d + s, i - s);
    return i;
}

/* an argument's value in "a=1;b=2" (NULL when it is not there) */
static const char *cui__vtk_arg(const char *args, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = args; p && *p;) {
        const char *e = strchr(p, ';');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
            size_t vl = CUI_MIN(len - kl - 1, cap - 1);
            memcpy(out, p + kl + 1, vl);
            out[vl] = 0;
            return out;
        }
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* a width or height: cells, Npx, N% of `screen` cells, or auto (0); `cell` pixels a cell */
static int cui__vtk_dim(const char *s, int screen, int cell, bool *pixels) {
    *pixels = false;
    if (!s || !*s || !strcmp(s, "auto")) return 0;
    long long n = 0;
    const char *p = s;
    while (*p >= '0' && *p <= '9') { if (n < 1000000) n = n * 10 + (*p - '0'); p++; }
    if (!strcmp(p, "px")) { *pixels = true; return (int)CUI_CLAMP(n, 0, (long long)CUI__VTK_DIM); }
    if (!strcmp(p, "%")) return (int)CUI_MAX(1, (n * screen + 50) / 100);
    (void)cell;
    return (int)CUI_CLAMP(n, 0, 4096);
}

static void cui__vtk_iterm_show(cui_vt *v, cui__vtk *k, const char *args) {
    char b[64];
    cui__vtk_b64_flush(&k->it);
    if (!cui__vtk_arg(args, "inline", b, sizeof b) || strcmp(b, "1")) return;       /* a download: not shown */
    if (k->it.bad || k->it.over || k->it.n == 0 || k->it.n >= (size_t)INT_MAX) return;
    int w = 0, h = 0;
    unsigned char *px = cui__png_decode(k->it.p, (int)k->it.n, &w, &h);
    if (!px) return;
    if (w <= 0 || h <= 0 || w > CUI__VTK_DIM || h > CUI__VTK_DIM) { cui__png_free(px); return; }
    cui__vt_info in;
    cui__vt_get_info(v, &in);
    const int cw = cui__vt_pic_cw(v);
    bool wpx, hpx;
    int W = cui__vtk_dim(cui__vtk_arg(args, "width", b, sizeof b), in.cols, cw, &wpx);
    int H = cui__vtk_dim(cui__vtk_arg(args, "height", b, sizeof b), in.rows, 20, &hpx);
    bool keep = !cui__vtk_arg(args, "preserveAspectRatio", b, sizeof b) || strcmp(b, "0");
    bool still = cui__vtk_arg(args, "doNotMoveCursor", b, sizeof b) && !strcmp(b, "1");
    /* the box in virtual pixels (0: auto) */
    int64_t bw = wpx ? W : (int64_t)W * cw, bh = hpx ? H : (int64_t)H * 20, dw, dh;
    if (!bw && !bh) {                                    /* its own size, made smaller to fit what is left of the row */
        dw = w; dh = h;
        int64_t room = (int64_t)CUI_MAX(1, in.cols - in.cx) * cw;
        if (dw > room) { dh = CUI_MAX(1, dh * room / dw); dw = room; }
    } else if (!bh) { dw = bw; dh = keep ? CUI_MAX(1, bw * h / w) : h; }
    else if (!bw) { dh = bh; dw = keep ? CUI_MAX(1, bh * w / h) : w; }
    else if (keep) {                                     /* inside the box, the shape kept */
        if (bw * h <= bh * w) { dw = bw; dh = CUI_MAX(1, bw * h / w); } else { dh = bh; dw = CUI_MAX(1, bh * w / h); }
    } else { dw = bw; dh = bh; }
    int cols = (int)CUI_CLAMP((dw + cw - 1) / cw, 1, 4096), rows = (int)CUI_CLAMP((dh + 19) / 20, 1, CUI__VTK_ROWS);
    cui__vtk_draw(v, px, w, h, w, cols, rows, 1, still);
    cui__png_free(px);
}

void cui__vt_iterm_end(cui_vt *v) {
    cui__vtk *k = *cui__vt_x_kitty(v);
    if (k->it_mode == 1 && !strncmp(k->it_hdr, "File=", 5)) {
        k->it_hdr[k->it_hdr_n - 1] = 0;                 /* the ':' */
        cui__vtk_iterm_show(v, k, k->it_hdr + 5);
        cui__vtk_blob_free(&k->it);
    } else if (k->it_mode == 0 && !strncmp(k->it_hdr, "MultipartFile=", 14)) {
        snprintf(k->it_args, sizeof k->it_args, "%s", k->it_hdr + 14);
        cui__vtk_blob_free(&k->it);
        k->it_multi = true;
    } else if (k->it_mode == 0 && !strcmp(k->it_hdr, "FileEnd") && k->it_multi) {
        cui__vtk_iterm_show(v, k, k->it_args);
        cui__vtk_blob_free(&k->it);
        k->it_multi = false;
    }
    k->it_mode = 0; k->it_hdr_n = 0; k->it_hdr[0] = 0;
}
