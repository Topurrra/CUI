/* Code folding for the code editor (cui_code_editor_opts.folding): which lines start a block and where it ends (by
 * indentation, or in markdown by headings), the set of folded blocks, and the rows on screen with the folded lines
 * taken out. The editor (cui_editor.c) reads its text through a cui__lines, tells the folds about its edits
 * (cui__fold_splice) and asks only about the lines in view; only cui__fold_all reads every line, once. */
#include "cui_internal.h"

#include <limits.h>

#define CUI__FOLD_LOOK 256   /* blank lines looked past to see whether a line starts a block */

/* ---- reading lines ---- */

static bool cui__fold_blank(const char *q, int ls, int le) {
    for (int i = ls; i < le; i++) if (q[i] != ' ' && q[i] != '\t' && q[i] != '\r') return false;
    return true;
}

/* the columns line i is indented by; -1 for a blank line (or past the end) */
static int cui__fold_indent(const cui__lines *L, int i) {
    int ls, le;
    const char *q = i < L->nl ? L->line(L->u, i, &ls, &le) : NULL;
    if (!q) return -1;
    int c = 0, tab = L->tab > 0 ? L->tab : 4;
    for (int k = ls; k < le; k++) {
        char ch = q[k];
        if (ch == ' ') c++;
        else if (ch == '\t') c = (c / tab + 1) * tab;
        else if (ch != '\r') return c;
    }
    return -1;
}

/* a markdown ATX heading's level (1 to 6), else 0 */
static int cui__fold_md_level(const char *q, int ls, int le) {
    int i = ls, sp = 0, n = 0;
    while (i < le && q[i] == ' ') { i++; sp++; }
    if (sp > 3) return 0;
    while (i < le && q[i] == '#') { i++; n++; }
    if (n < 1 || n > 6 || (i < le && q[i] != ' ' && q[i] != '\t' && q[i] != '\r')) return 0;
    return n;
}

/* fenced code in markdown: a line of ``` or ~~~ (three or more, at most three spaces in) opens it, one of the same
 * character at least as long closes it; headings inside are code */
typedef struct { char ch; int len; } cui__fold_fz;
static void cui__fold_fence(cui__fold_fz *z, const char *q, int ls, int le) {
    int i = ls, sp = 0;
    while (i < le && q[i] == ' ') { i++; sp++; }
    if (sp > 3 || i >= le || (q[i] != '`' && q[i] != '~')) return;
    int k = i;
    while (k < le && q[k] == q[i]) k++;
    if (k - i < 3) return;
    if (!z->len) { z->ch = q[i]; z->len = k - i; }
    else if (q[i] == z->ch && k - i >= z->len && cui__fold_blank(q, k, le)) z->len = 0;
}

/* line i's heading level, outside fenced code (z follows the fences, line by line); 0 for any other line, -1 blank */
static int cui__fold_md_line(const cui__lines *L, int i, cui__fold_fz *z) {
    int ls, le;
    const char *q = L->line(L->u, i, &ls, &le);
    if (!q) return -1;
    bool in = z->len > 0;
    cui__fold_fence(z, q, ls, le);
    if (cui__fold_blank(q, ls, le)) return -1;
    return in || z->len ? 0 : cui__fold_md_level(q, ls, le);
}

/* not in a fence, front matter or comment by the editor's line states (unknown: assume so) */
static bool cui__fold_md_free(const cui__lines *L, int h) {
    int s = L->state ? L->state(L->u, h) : -1;
    return s <= 0 || s == CUI__MD_START;
}

bool cui__fold_starts(const cui__lines *L, bool md, int h) {
    if (h < 0 || h >= L->nl - 1) return false;
    if (md) {
        int ls, le;
        const char *q = L->line(L->u, h, &ls, &le);
        int lv = q ? cui__fold_md_level(q, ls, le) : 0;
        if (!lv || !cui__fold_md_free(L, h)) return false;
        cui__fold_fz z = { 0, 0 };
        for (int i = h + 1, n = 0; i < L->nl && n < CUI__FOLD_LOOK; i++, n++) {
            int l2 = cui__fold_md_line(L, i, &z);
            if (l2 < 0) continue;
            return !(l2 > 0 && l2 <= lv);   /* text or a deeper heading below it */
        }
        return false;
    }
    int ind = cui__fold_indent(L, h);
    if (ind < 0) return false;
    for (int i = h + 1, n = 0; i < L->nl && n < CUI__FOLD_LOOK; i++, n++) {
        int d = cui__fold_indent(L, i);
        if (d >= 0) return d > ind;
    }
    return false;
}

int cui__fold_end(const cui__lines *L, bool md, int h) {
    if (h < 0 || h >= L->nl - 1) return -1;
    int last = -1;
    if (md) {
        int ls, le;
        const char *q = L->line(L->u, h, &ls, &le);
        int lv = q ? cui__fold_md_level(q, ls, le) : 0;
        if (!lv) return -1;
        cui__fold_fz z = { 0, 0 };
        for (int i = h + 1; i < L->nl; i++) {
            int l2 = cui__fold_md_line(L, i, &z);
            if (l2 > 0 && l2 <= lv) break;
            if (l2 >= 0) last = i;
        }
    } else {
        int ind = cui__fold_indent(L, h);
        if (ind < 0) return -1;
        for (int i = h + 1; i < L->nl; i++) {
            int d = cui__fold_indent(L, i);
            if (d < 0) continue;
            if (d <= ind) break;
            last = i;
        }
    }
    return last > h ? last : -1;
}

/* a heading's level on line i outside a fence by the line states; 0 none */
static int cui__fold_md_head(const cui__lines *L, int i) {
    int ls, le;
    const char *q = L->line(L->u, i, &ls, &le);
    return q && cui__fold_md_free(L, i) ? cui__fold_md_level(q, ls, le) : 0;
}

/* The innermost block holding line `line` that is not folded yet: the line's own when it starts one, else the nearest
 * one above that holds it (a heading of a higher level, a line indented less). Its header, and its last line in *end;
 * -1 none. Reads up from the line as far as the block's header. */
int cui__fold_around(const cui__lines *L, bool md, const cui__fold *F, int line, int *end) {
    if (cui__fold_at(F, line) < 0) {
        int e = cui__fold_end(L, md, line);
        if (e > line) { *end = e; return line; }
    }
    int ref = md ? cui__fold_md_head(L, line) : cui__fold_indent(L, line);
    if (ref <= 0 && md) ref = 7;          /* not a heading: any heading above holds it */
    if (ref < 0) ref = INT_MAX;           /* a blank line: the nearest line above */
    for (int h = line - 1; h >= 0; h--) {
        int v = md ? cui__fold_md_head(L, h) : cui__fold_indent(L, h);
        if ((md && v == 0) || v < 0 || v >= ref) continue;
        int e = cui__fold_end(L, md, h);
        if (e >= line && cui__fold_at(F, h) < 0) { *end = e; return h; }
        ref = v;   /* folded already, or it does not reach the line: the block around it */
    }
    return -1;
}

/* ---- the folded set ---- */

void cui__fold_free(cui_ctx *ui, cui__fold *F) {
    if (!F) return;
    cui__free(ui, F->h); cui__free(ui, F->e);
    cui__free(ui, F->va); cui__free(ui, F->vb); cui__free(ui, F->vp);
    cui__free(ui, F);
}

static void cui__fold_chg(cui__fold *F, int a, int b) {
    if (b <= a) return;
    if (F->chg0 >= F->chg1) { F->chg0 = a; F->chg1 = b; return; }
    F->chg0 = CUI_MIN(F->chg0, a); F->chg1 = CUI_MAX(F->chg1, b);
}

/* the hidden runs again: the union of the folds' lines, in order, with the hidden lines above each */
static void cui__fold_runs(cui_ctx *ui, cui__fold *F) {
    if (F->vcap < F->n + 1) {
        F->vcap = F->n + 16;
        F->va = cui__realloc(ui, F->va, sizeof(int) * (size_t)F->vcap);
        F->vb = cui__realloc(ui, F->vb, sizeof(int) * (size_t)F->vcap);
        F->vp = cui__realloc(ui, F->vp, sizeof(int) * (size_t)F->vcap);
    }
    int vn = 0, hid = 0;
    for (int i = 0; i < F->n; i++) {
        int a = F->h[i] + 1, b = F->e[i];
        if (vn && a <= F->vb[vn - 1] + 1) { F->vb[vn - 1] = CUI_MAX(F->vb[vn - 1], b); continue; }
        F->va[vn] = a; F->vb[vn] = b; vn++;
    }
    for (int j = 0; j < vn; j++) { F->vp[j] = hid; hid += F->vb[j] - F->va[j] + 1; }
    F->vp[vn] = hid;
    F->vn = vn; F->hidden = hid;
}

void cui__fold_clear(cui__fold *F) {
    if (!F) return;
    if (F->vn) cui__fold_chg(F, F->va[0], F->vb[F->vn - 1] + 1);
    F->n = F->vn = F->hidden = 0;
}

/* the first fold whose header is at or after h */
static int cui__fold_lower(const cui__fold *F, int h) {
    int lo = 0, hi = F->n;
    while (lo < hi) { int m = (lo + hi) / 2; if (F->h[m] < h) lo = m + 1; else hi = m; }
    return lo;
}

int cui__fold_at(const cui__fold *F, int h) {
    int i = cui__fold_lower(F, h);
    return i < F->n && F->h[i] == h ? i : -1;
}

bool cui__fold_set(cui_ctx *ui, cui__fold *F, int h, int e) {
    if (h < 0 || e <= h) return false;
    int i = cui__fold_lower(F, h);
    if (i < F->n && F->h[i] == h) {
        if (F->e[i] == e) return false;
        cui__fold_chg(F, h + 1, CUI_MAX(F->e[i], e) + 1);
        F->e[i] = e;
    } else {
        if (F->n == F->cap) {
            F->cap = F->cap ? F->cap * 2 : 16;
            F->h = cui__realloc(ui, F->h, sizeof(int) * (size_t)F->cap);
            F->e = cui__realloc(ui, F->e, sizeof(int) * (size_t)F->cap);
        }
        memmove(F->h + i + 1, F->h + i, sizeof(int) * (size_t)(F->n - i));
        memmove(F->e + i + 1, F->e + i, sizeof(int) * (size_t)(F->n - i));
        F->h[i] = h; F->e[i] = e; F->n++;
        cui__fold_chg(F, h + 1, e + 1);
    }
    cui__fold_runs(ui, F);
    return true;
}

bool cui__fold_unset(cui_ctx *ui, cui__fold *F, int h) {
    int i = cui__fold_at(F, h);
    if (i < 0) return false;
    cui__fold_chg(F, h + 1, F->e[i] + 1);
    memmove(F->h + i, F->h + i + 1, sizeof(int) * (size_t)(F->n - i - 1));
    memmove(F->e + i, F->e + i + 1, sizeof(int) * (size_t)(F->n - i - 1));
    F->n--;
    cui__fold_runs(ui, F);
    return true;
}

bool cui__fold_reveal(cui_ctx *ui, cui__fold *F, int line) {
    if (!cui__fold_hidden(F, line)) return false;
    int w = 0;
    for (int i = 0; i < F->n; i++) {
        if (F->h[i] < line && line <= F->e[i]) { cui__fold_chg(F, F->h[i] + 1, F->e[i] + 1); continue; }
        F->h[w] = F->h[i]; F->e[w] = F->e[i]; w++;
    }
    F->n = w;
    cui__fold_runs(ui, F);
    return true;
}

/* Lines [la, lb] became [la, la + k]. Folds above move not, folds below move with the text, and a fold whose lines the
 * edit touched opens (typing on a folded line itself keeps it). */
void cui__fold_splice(cui_ctx *ui, cui__fold *F, int la, int lb, int k) {
    int d = k - (lb - la);
    if (F->chg0 < F->chg1) {   /* the lines still to look at move too */
        if (F->chg1 > lb) F->chg1 += d; else F->chg1 = CUI_MAX(F->chg1, la + k + 1);
        if (F->chg0 > lb) F->chg0 += d; else F->chg0 = CUI_MIN(F->chg0, la);
    }
    if (!F->n) return;
    int w = 0, lim = F->n;
    if (d == 0) {   /* nothing moves: only a fold whose lines the edit touched (headers up to lb) changes */
        lim = cui__fold_lower(F, lb + 1);
        bool hit = false;
        for (int i = 0; i < lim && !hit; i++) hit = F->e[i] >= la && !(la == lb && F->h[i] == la && k == 0);
        if (!hit) return;
    }
    for (int i = 0; i < F->n; i++) {
        int h = F->h[i], e = F->e[i];
        if (h > lb) { h += d; e += d; }
        else if (e >= la && !(la == lb && h == la && k == 0)) {   /* touched: it opens */
            cui__fold_chg(F, CUI_MIN(h + 1, la), CUI_MAX(e + d, la + k) + 1);
            continue;
        }
        F->h[w] = h; F->e[w] = e; w++;
    }
    F->n = w;
    cui__fold_runs(ui, F);
}

/* Every block folded, in one pass over the text; returns how many folds there are. A block is noted when it opens
 * (headers come in order, so the list is sorted as it grows) and gets its last line when it closes. */
int cui__fold_all(cui_ctx *ui, cui__fold *F, const cui__lines *L, bool md) {
    int cap = 256, n = 0, sn = 0, scap = 64;
    int *pr = cui__alloc(ui, sizeof(int) * 2 * (size_t)cap);         /* (header, last line) */
    int *sk = cui__alloc(ui, sizeof(int) * 2 * (size_t)scap);        /* the open blocks: (their pr, indent or level) */
#define CUI__FOLD_PUSH(arr, cnt, cp, x, y) do { if ((cnt) == (cp)) { (cp) *= 2; (arr) = cui__realloc(ui, (arr), sizeof(int) * 2 * (size_t)(cp)); } \
        (arr)[2 * (cnt)] = (x); (arr)[2 * (cnt) + 1] = (y); (cnt)++; } while (0)
#define CUI__FOLD_OPEN(h, key) do { CUI__FOLD_PUSH(sk, sn, scap, n, (key)); CUI__FOLD_PUSH(pr, n, cap, (h), -1); } while (0)
#define CUI__FOLD_SHUT(last) do { sn--; pr[2 * sk[2 * sn] + 1] = (last); } while (0)
    int p = -1, ip = 0;   /* the last line that is not blank, and its indentation */
    cui__fold_fz z = { 0, 0 };
    for (int i = 0; i < L->nl; i++) {
        if (md) {
            int lv = cui__fold_md_line(L, i, &z);
            if (lv < 0) continue;
            if (lv > 0) {
                while (sn && sk[2 * (sn - 1) + 1] >= lv) CUI__FOLD_SHUT(p);
                CUI__FOLD_OPEN(i, lv);
            }
            p = i;
            continue;
        }
        int ind = cui__fold_indent(L, i);
        if (ind < 0) continue;
        if (p >= 0 && ind > ip) CUI__FOLD_OPEN(p, ip);
        else while (sn && sk[2 * (sn - 1) + 1] >= ind) CUI__FOLD_SHUT(p);
        p = i; ip = ind;
    }
    while (sn) CUI__FOLD_SHUT(p);
#undef CUI__FOLD_PUSH
#undef CUI__FOLD_OPEN
#undef CUI__FOLD_SHUT
    cui__free(ui, sk);
    /* merged with the folds there were (the new one wins on a header both have) */
    int m = 0, room = n + F->n + 1, *h = cui__alloc(ui, sizeof(int) * (size_t)room), *e = cui__alloc(ui, sizeof(int) * (size_t)room);
    for (int a = 0, b = 0; a < n || b < F->n;) {
        if (a < n && pr[2 * a + 1] <= pr[2 * a]) { a++; continue; }   /* (a heading with nothing under it) */
        bool take_new = b >= F->n || (a < n && pr[2 * a] <= F->h[b]);
        if (take_new) {
            if (b < F->n && F->h[b] == pr[2 * a]) b++;
            h[m] = pr[2 * a]; e[m] = pr[2 * a + 1]; a++;
        } else { h[m] = F->h[b]; e[m] = F->e[b]; b++; }
        m++;
    }
    cui__free(ui, pr);
    cui__free(ui, F->h); cui__free(ui, F->e);
    F->h = h; F->e = e; F->n = m; F->cap = room;
    cui__fold_runs(ui, F);
    if (F->vn) cui__fold_chg(F, F->va[0], F->vb[F->vn - 1] + 1);
    return m;
}

/* ---- rows with the folded lines taken out ---- */

/* the first hidden run that ends at or after line */
static int cui__fold_run(const cui__fold *F, int line) {
    int lo = 0, hi = F->vn;
    while (lo < hi) { int m = (lo + hi) / 2; if (F->vb[m] < line) lo = m + 1; else hi = m; }
    return lo;
}

bool cui__fold_hidden(const cui__fold *F, int line) {
    if (!F || !F->vn) return false;
    int j = cui__fold_run(F, line);
    return j < F->vn && F->va[j] <= line;
}

int cui__fold_row(const cui__fold *F, int line) {
    if (!F || !F->vn) return line;
    int j = cui__fold_run(F, line), hb = F->vp[j];
    if (j < F->vn && F->va[j] <= line) hb += line - F->va[j];
    return line - hb;
}

int cui__fold_line(const cui__fold *F, int row) {
    if (!F || !F->vn || row < 0) return row;
    int lo = 0, hi = F->vn;   /* the runs whose line below them is at or above row */
    while (lo < hi) { int m = (lo + hi) / 2; if (F->va[m] - F->vp[m] <= row) lo = m + 1; else hi = m; }
    return row + F->vp[lo];
}

int cui__fold_next(const cui__fold *F, int line) {
    if (!F || !F->vn) return line;
    int j = cui__fold_run(F, line);
    return j < F->vn && F->va[j] <= line ? F->vb[j] + 1 : line;
}

int cui__fold_prev(const cui__fold *F, int line) {
    if (!F || !F->vn) return line;
    int j = cui__fold_run(F, line);
    return j < F->vn && F->va[j] <= line ? F->va[j] - 1 : line;
}

int cui__fold_gap(const cui__fold *F, int line) {
    if (!F || !F->vn) return INT_MAX;
    int j = cui__fold_run(F, line);
    return j < F->vn ? CUI_MAX(F->va[j], line) : INT_MAX;
}

/* ---- painting: the markers in the gutter, and the box after a folded line ---- */

void cui__fold_draw(cui_ctx *ui, const cui__fold_paint *fp, cui_rect r, float gx, float x0, int part) {
    float hs = CUI_MIN(fp->lh * 0.2f, 4.0f);   /* half the chevron's size */
    for (int i = 0; i < fp->n; i++) {
        int kind = fp->kind[i];
        float y = r.y + (part == 0 ? fp->dy[i] : fp->by[i]);
        if (y > r.y + r.h || y + fp->lh < r.y) continue;
        float cy = y + fp->lh * 0.5f;
        if (part == 0) {
            if (kind == 1 && !fp->hover) continue;
            float cx = gx - fp->fw * 0.5f - 1;
            cui_vec2 pt[3];
            if (kind == 2) { pt[0] = (cui_vec2){ cx - hs * 0.5f, cy - hs }; pt[1] = (cui_vec2){ cx + hs * 0.5f, cy }; pt[2] = (cui_vec2){ cx - hs * 0.5f, cy + hs }; }
            else { pt[0] = (cui_vec2){ cx - hs, cy - hs * 0.5f }; pt[1] = (cui_vec2){ cx, cy + hs * 0.5f }; pt[2] = (cui_vec2){ cx + hs, cy - hs * 0.5f }; }
            cui_draw_polyline(ui, pt, 3, 1.4f, kind == 2 ? fp->mark_on : fp->mark, false);
        } else if (kind == 2) {
            float bx = x0 + (float)(fp->col[i] - fp->cb) * fp->cw + fp->cw * 0.6f, bw = CUI__FOLD_BOX_COLS * fp->cw;
            cui_draw_rect_border(ui, (cui_rect){ bx, y + 2, bw, fp->lh - 4 }, CUI_CORNERS(3), fp->box, fp->box_edge, 1);
            float dr = CUI_MAX(1.0f, fp->lh * 0.055f);
            for (int k = -1; k <= 1; k++) cui_draw_circle(ui, (cui_vec2){ bx + bw * 0.5f + (float)k * bw * 0.24f, cy }, dr, fp->dots);
        }
    }
}
