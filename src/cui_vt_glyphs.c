/* The VT screen's hand-drawn characters (cui_vt.c calls cui__vt_glyph for every cell with a character):
 *
 *   U+2500..U+257F   box drawing: light, heavy and double lines meeting exactly across cells, dashes, round corners, diagonals
 *   U+2580..U+259F   block elements: halves, eighths, quadrants, the three shades
 *   U+2800..U+28FF   braille: the 2 by 4 dot grid
 *   U+1FB00..U+1FB3B sextants; U+1FB3C..U+1FB6F smooth wedges and triangles; U+1FB70..U+1FB94 eighth and shade blocks;
 *                    U+1FB9A..U+1FB9F triangle halves and shades
 *   U+E0B0..U+E0BE   status line shapes (the Powerline triangles, chevrons and half circles)
 *
 * Fonts draw these differently from each other and rarely so that neighbouring cells meet, so they are drawn here. Everything is
 * laid out on whole device pixels: the cell's edges are rounded once, and every part is an integer number of pixels from them, so
 * cells next to each other join without seams or overlaps at any scale and font size. Rectangles are used wherever the shape is
 * axis aligned; only round corners, diagonals, wedges and the status line shapes use polygons and lines (anti-aliased). */
#include "cui_internal.h"

typedef struct {
    cui_ctx *ui;
    float s;             /* device pixels per logical pixel */
    int x, y;            /* the cell's top left in device pixels */
    int w, h;            /* its size in device pixels */
    int light;           /* line width of light lines, from the font's cell height (the same in every row) */
    float cw, ch;        /* the cell size in device pixels before rounding (the same for every cell) */
    cui_color col;
    float rc[4 * 32];    /* rectangles waiting to be drawn as one batch (device pixels) */
    int nrc;
    cui_color bcol;      /* their color */
} cui__vg;

/* the waiting rectangles, drawn as one batch */
static void cui__vg_flush(cui__vg *g) {
    if (g->nrc) cui__draw_rects_px(g->ui, g->rc, g->nrc, g->bcol);
    g->nrc = 0;
}

/* a rectangle of the cell, in device pixels relative to its corner; collected and drawn with the others of its color */
static void cui__vg_fillc(cui__vg *g, int x0, int y0, int x1, int y1, cui_color col) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->w) x1 = g->w;
    if (y1 > g->h) y1 = g->h;
    if (x1 <= x0 || y1 <= y0) return;
    if (g->nrc == 32 || (g->nrc && memcmp(&col, &g->bcol, sizeof col))) cui__vg_flush(g);
    g->bcol = col;
    float *r = &g->rc[4 * g->nrc++];
    r[0] = (float)(g->x + x0); r[1] = (float)(g->y + y0); r[2] = (float)(g->x + x1); r[3] = (float)(g->y + y1);
}

static void cui__vg_fill(cui__vg *g, int x0, int y0, int x1, int y1) { cui__vg_fillc(g, x0, y0, x1, y1, g->col); }

static void cui__vg_line(cui__vg *g, const float *xy, int n, float thick) {
    cui_vec2 p[20];
    float k = 1.0f / g->s;
    if (n > 20) n = 20;
    for (int i = 0; i < n; i++) p[i] = (cui_vec2){ ((float)g->x + xy[2 * i]) * k, ((float)g->y + xy[2 * i + 1]) * k };
    cui_draw_polyline(g->ui, p, n, thick * k, g->col, false);
}

/* ------------------------------------------------------------------------ */
/* Box drawing                                                              */
/* ------------------------------------------------------------------------ */

/* U+2500..U+257F as four arms (left, right, up, down), each 0 none, 1 light, 2 heavy, 3 double. The dashed lines are the straight
 * lines here (they are drawn by cui__vg_dashes), the round corners are the plain corners (cui__vg_arc), the diagonals have no arms. */
#define CUI__VT_ARMS(l, r, u, d) ((uint8_t)((l) | ((r) << 2) | ((u) << 4) | ((d) << 6)))
uint8_t cui__vt_box_arms(uint32_t c) {
    static const uint8_t t[] = {
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2), CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(0,1,0,1), CUI__VT_ARMS(0,2,0,1), CUI__VT_ARMS(0,1,0,2), CUI__VT_ARMS(0,2,0,2), CUI__VT_ARMS(1,0,0,1), CUI__VT_ARMS(2,0,0,1), CUI__VT_ARMS(1,0,0,2), CUI__VT_ARMS(2,0,0,2),
        CUI__VT_ARMS(0,1,1,0), CUI__VT_ARMS(0,2,1,0), CUI__VT_ARMS(0,1,2,0), CUI__VT_ARMS(0,2,2,0), CUI__VT_ARMS(1,0,1,0), CUI__VT_ARMS(2,0,1,0), CUI__VT_ARMS(1,0,2,0), CUI__VT_ARMS(2,0,2,0),
        CUI__VT_ARMS(0,1,1,1), CUI__VT_ARMS(0,2,1,1), CUI__VT_ARMS(0,1,2,1), CUI__VT_ARMS(0,1,1,2), CUI__VT_ARMS(0,1,2,2), CUI__VT_ARMS(0,2,2,1), CUI__VT_ARMS(0,2,1,2), CUI__VT_ARMS(0,2,2,2),
        CUI__VT_ARMS(1,0,1,1), CUI__VT_ARMS(2,0,1,1), CUI__VT_ARMS(1,0,2,1), CUI__VT_ARMS(1,0,1,2), CUI__VT_ARMS(1,0,2,2), CUI__VT_ARMS(2,0,2,1), CUI__VT_ARMS(2,0,1,2), CUI__VT_ARMS(2,0,2,2),
        CUI__VT_ARMS(1,1,0,1), CUI__VT_ARMS(2,1,0,1), CUI__VT_ARMS(1,2,0,1), CUI__VT_ARMS(2,2,0,1), CUI__VT_ARMS(1,1,0,2), CUI__VT_ARMS(2,1,0,2), CUI__VT_ARMS(1,2,0,2), CUI__VT_ARMS(2,2,0,2),
        CUI__VT_ARMS(1,1,1,0), CUI__VT_ARMS(2,1,1,0), CUI__VT_ARMS(1,2,1,0), CUI__VT_ARMS(2,2,1,0), CUI__VT_ARMS(1,1,2,0), CUI__VT_ARMS(2,1,2,0), CUI__VT_ARMS(1,2,2,0), CUI__VT_ARMS(2,2,2,0),
        CUI__VT_ARMS(1,1,1,1), CUI__VT_ARMS(2,1,1,1), CUI__VT_ARMS(1,2,1,1), CUI__VT_ARMS(2,2,1,1), CUI__VT_ARMS(1,1,2,1), CUI__VT_ARMS(1,1,1,2), CUI__VT_ARMS(1,1,2,2), CUI__VT_ARMS(2,1,2,1),
        CUI__VT_ARMS(1,2,2,1), CUI__VT_ARMS(2,1,1,2), CUI__VT_ARMS(1,2,1,2), CUI__VT_ARMS(2,2,2,1), CUI__VT_ARMS(2,2,1,2), CUI__VT_ARMS(2,1,2,2), CUI__VT_ARMS(1,2,2,2), CUI__VT_ARMS(2,2,2,2),
        CUI__VT_ARMS(1,1,0,0), CUI__VT_ARMS(2,2,0,0), CUI__VT_ARMS(0,0,1,1), CUI__VT_ARMS(0,0,2,2),
        CUI__VT_ARMS(3,3,0,0), CUI__VT_ARMS(0,0,3,3), CUI__VT_ARMS(0,3,0,1), CUI__VT_ARMS(0,1,0,3), CUI__VT_ARMS(0,3,0,3), CUI__VT_ARMS(3,0,0,1), CUI__VT_ARMS(1,0,0,3), CUI__VT_ARMS(3,0,0,3),
        CUI__VT_ARMS(0,3,1,0), CUI__VT_ARMS(0,1,3,0), CUI__VT_ARMS(0,3,3,0), CUI__VT_ARMS(3,0,1,0), CUI__VT_ARMS(1,0,3,0), CUI__VT_ARMS(3,0,3,0),
        CUI__VT_ARMS(0,3,1,1), CUI__VT_ARMS(0,1,3,3), CUI__VT_ARMS(0,3,3,3), CUI__VT_ARMS(3,0,1,1), CUI__VT_ARMS(1,0,3,3), CUI__VT_ARMS(3,0,3,3),
        CUI__VT_ARMS(3,3,0,1), CUI__VT_ARMS(1,1,0,3), CUI__VT_ARMS(3,3,0,3), CUI__VT_ARMS(3,3,1,0), CUI__VT_ARMS(1,1,3,0), CUI__VT_ARMS(3,3,3,0),
        CUI__VT_ARMS(3,3,1,1), CUI__VT_ARMS(1,1,3,3), CUI__VT_ARMS(3,3,3,3),
        CUI__VT_ARMS(0,1,0,1), CUI__VT_ARMS(1,0,0,1), CUI__VT_ARMS(1,0,1,0), CUI__VT_ARMS(0,1,1,0),
    };
    if (c >= 0x2574 && c <= 0x257F) {
        static const uint8_t h[] = { CUI__VT_ARMS(1,0,0,0), CUI__VT_ARMS(0,0,1,0), CUI__VT_ARMS(0,1,0,0), CUI__VT_ARMS(0,0,0,1), CUI__VT_ARMS(2,0,0,0), CUI__VT_ARMS(0,0,2,0),
                                     CUI__VT_ARMS(0,2,0,0), CUI__VT_ARMS(0,0,0,2), CUI__VT_ARMS(1,2,0,0), CUI__VT_ARMS(0,0,1,2), CUI__VT_ARMS(2,1,0,0), CUI__VT_ARMS(0,0,2,1) };
        return h[c - 0x2574];
    }
    return c >= 0x2500 && c < 0x2500 + sizeof t ? t[c - 0x2500] : 0;
}

/* The strokes of one arm across an axis of `extent` pixels: one stroke (light or heavy) or two (double), centred. Returns their
 * count; out[k] is {start, end}. Strokes of different weight in one cell nest (the thin one sits inside the thick one). */
static int cui__vg_strokes(int style, int extent, int light, int heavy, int out[2][2]) {
    int t = style == 2 ? heavy : light, total = style == 3 ? light * 3 : t;
    int o = (extent - total) / 2;
    if (o < 0) o = 0;
    if (style == 3) { out[0][0] = o; out[0][1] = o + light; out[1][0] = o + 2 * light; out[1][1] = o + 3 * light; return 2; }
    out[0][0] = o; out[0][1] = o + t;
    return 1;
}

static void cui__vg_band(int style, int extent, int light, int heavy, int *lo, int *hi) {
    int s[2][2], n = cui__vg_strokes(style, extent, light, heavy, s);
    *lo = s[0][0]; *hi = s[n - 1][1];
}

/* Horizontal and vertical double lines in one cell: each of the two lines of an arm either runs on to the far edge of the
 * crossing pair (a corner closes, a line passes a single crossing) or stops at the line next to it (the gap in the double
 * junction). Where an arm ends depends on which other arms exist; the rules are derived from the chart. */
static void cui__vg_box_dd(cui__vg *g, bool l, bool r, bool u, bool d) {
    int h[2][2], v[2][2], lt = g->light;
    cui__vg_strokes(3, g->h, lt, lt, h);
    cui__vg_strokes(3, g->w, lt, lt, v);
    for (int k = 0; k < 2; k++) {
        bool broken = k == 0 ? u : d;                                  /* a vertical arm meets this line */
        bool outer = k == 0 ? (d && !u) : (u && !d);                   /* the corner's outer line */
        if (r) cui__vg_fill(g, l ? (broken ? v[1][0] : v[0][0]) : (outer ? v[0][0] : v[1][0]), h[k][0], g->w, h[k][1]);
        if (l) cui__vg_fill(g, 0, h[k][0], r ? (broken ? v[0][1] : v[1][1]) : (outer ? v[1][1] : v[0][1]), h[k][1]);
    }
    for (int j = 0; j < 2; j++) {
        bool broken = j == 0 ? l : r;
        bool outer = j == 0 ? (r && !l) : (l && !r);
        if (u) cui__vg_fill(g, v[j][0], 0, v[j][1], d ? (broken ? h[0][1] : h[1][1]) : (outer ? h[1][1] : h[0][1]));
        if (d) cui__vg_fill(g, v[j][0], u ? (broken ? h[1][0] : h[0][0]) : (outer ? h[0][0] : h[1][0]), v[j][1], g->h);
    }
}

/* Lines: each arm runs from the cell's edge to the far side of the arms that cross it, so corners and junctions are solid. */
static void cui__vg_box_lines(cui__vg *g, const int arm[4]) {
    int lt = g->light, hv = g->light * 2;
    if ((arm[0] == 3 || arm[1] == 3) && (arm[2] == 3 || arm[3] == 3)) { cui__vg_box_dd(g, arm[0] != 0, arm[1] != 0, arm[2] != 0, arm[3] != 0); return; }
    int hs[2][2][2], vs[2][2][2], hn[2] = { 0, 0 }, vn[2] = { 0, 0 };     /* strokes of left/right across y, of up/down across x */
    int xlo = 1 << 30, xhi = -1, ylo = 1 << 30, yhi = -1;
    for (int i = 0; i < 2; i++) {
        int lo, hi;
        if (arm[i]) { hn[i] = cui__vg_strokes(arm[i], g->h, lt, hv, hs[i]); cui__vg_band(arm[i], g->h, lt, hv, &lo, &hi); if (lo < ylo) ylo = lo; if (hi > yhi) yhi = hi; }
        if (arm[2 + i]) { vn[i] = cui__vg_strokes(arm[2 + i], g->w, lt, hv, vs[i]); cui__vg_band(arm[2 + i], g->w, lt, hv, &lo, &hi); if (lo < xlo) xlo = lo; if (hi > xhi) xhi = hi; }
    }
    bool hasv = vn[0] || vn[1], hash = hn[0] || hn[1];
    bool one_h = arm[0] && arm[0] == arm[1], one_v = arm[2] && arm[2] == arm[3];       /* both arms alike: one line from edge to edge */
    if (one_h) for (int k = 0; k < hn[0]; k++) cui__vg_fill(g, 0, hs[0][k][0], g->w, hs[0][k][1]);
    if (one_v) for (int k = 0; k < vn[0]; k++) cui__vg_fill(g, vs[0][k][0], 0, vs[0][k][1], g->h);
    for (int i = 0; i < 2; i++) {
        int lo, hi;
        if (arm[i] && !one_h) {                                                /* left (0) ends past the crossing, right (1) starts before it */
            cui__vg_band(arm[i], g->w, lt, hv, &lo, &hi);
            int e = hasv ? (i == 0 ? xhi : xlo) : (i == 0 ? hi : lo);
            for (int k = 0; k < hn[i]; k++) { if (i == 0) cui__vg_fill(g, 0, hs[i][k][0], e, hs[i][k][1]); else cui__vg_fill(g, e, hs[i][k][0], g->w, hs[i][k][1]); }
        }
        if (arm[2 + i] && !one_v) {
            cui__vg_band(arm[2 + i], g->h, lt, hv, &lo, &hi);
            int e = hash ? (i == 0 ? yhi : ylo) : (i == 0 ? hi : lo);
            for (int k = 0; k < vn[i]; k++) { if (i == 0) cui__vg_fill(g, vs[i][k][0], 0, vs[i][k][1], e); else cui__vg_fill(g, vs[i][k][0], e, vs[i][k][1], g->h); }
        }
    }
}

/* Dashed lines: n dashes with the same gap between all of them, and half a gap at each end of the cell, so the pattern
 * carries on evenly into the next cell. The dashes differ by at most one pixel in length. */
static void cui__vg_dashes(cui__vg *g, bool vertical, int n, bool heavy) {
    int len = vertical ? g->h : g->w, across = vertical ? g->w : g->h, s[2][2];
    cui__vg_strokes(heavy ? 2 : 1, across, g->light, g->light * 2, s);
    if (n > len / 2) n = len / 2 > 1 ? len / 2 : 1;                       /* a cell too small for that many dashes with a gap each */
    float lenf = vertical ? g->ch : g->cw;                              /* the gap from the cell size before rounding: the same in every cell */
    int gap = (int)(lenf / (3.0f * (float)n) + 0.5f);
    if (gap < 1) gap = 1;
    while (gap > 0 && n * gap > len - n) gap--;
    int avail = len - n * gap, base = avail / n, rem = avail % n, pos = gap / 2;
    for (int i = 0; i < n; i++) {
        int dl = base + ((i + 1) * rem / n - i * rem / n);
        if (vertical) cui__vg_fill(g, s[0][0], pos, s[0][1], pos + dl);
        else cui__vg_fill(g, pos, s[0][0], pos + dl, s[0][1]);
        pos += dl + gap;
    }
}

/* A round corner: a quarter circle as wide as half the cell, tangent to straight lines that run on to the cell's edges, the same
 * strokes (the same pixels) as the lines of the neighbouring cells. The quarter ring is laid out pixel by pixel: how much of a
 * stroke as wide as a light line covers each pixel, in four steps of alpha, a run of one step in a row as one rectangle. So the
 * curve is exactly as thick as the lines it joins and meets them on whole pixels, and a corner costs a handful of rectangles. */
static void cui__vg_arc(cui__vg *g, bool right, bool down) {
    int vs[2][2], hs[2][2];
    cui__vg_strokes(1, g->w, g->light, g->light, vs);
    cui__vg_strokes(1, g->h, g->light, g->light, hs);
    float xc = (float)(vs[0][0] + vs[0][1]) * 0.5f, yc = (float)(hs[0][0] + hs[0][1]) * 0.5f;
    float sx = right ? 1.0f : -1.0f, sy = down ? 1.0f : -1.0f;
    float rx = right ? (float)g->w - xc : xc, ry = down ? (float)g->h - yc : yc;
    float rr = floorf(rx < ry ? rx : ry);                             /* a circle: as wide as the half cell, then straight on */
    float cx = xc + sx * rr, cy = yc + sy * rr, half = (float)g->light * 0.5f;
    int ey = (int)floorf(cy + 0.5f), ex = (int)floorf(cx + 0.5f);
    int bx0 = right ? vs[0][0] : ex, bx1 = right ? ex : vs[0][1];
    int by0 = down ? hs[0][0] : ey, by1 = down ? ey : hs[0][1];
    for (int py = by0; py < by1; py++) {
        int run0 = bx0, lev = 0;
        for (int px = bx0; px <= bx1; px++) {
            int l = 0;
            if (px < bx1) {
                float dx = (float)px + 0.5f - cx, dy = (float)py + 0.5f - cy;
                float c = half + 0.5f - fabsf(sqrtf(dx * dx + dy * dy) - rr);
                l = c >= 1.0f ? 4 : c <= 0.0f ? 0 : (int)(c * 4.0f + 0.5f);
            }
            if (l != lev) {
                if (lev) cui__vg_fillc(g, run0, py, px, py + 1, lev == 4 ? g->col : cui_alpha(g->col, (float)lev * 0.25f));
                run0 = px; lev = l;
            }
        }
    }
    /* the straight parts to the cell's edges: the same pixels as the lines of the neighbouring cells */
    if (down) cui__vg_fill(g, vs[0][0], ey, vs[0][1], g->h); else cui__vg_fill(g, vs[0][0], 0, vs[0][1], ey);
    if (right) cui__vg_fill(g, ex, hs[0][0], g->w, hs[0][1]); else cui__vg_fill(g, 0, hs[0][0], ex, hs[0][1]);
}

static void cui__vg_diagonals(cui__vg *g, uint32_t cp) {
    float w = (float)g->w, h = (float)g->h;
    if (cp != 0x2572) { float p[4] = { 0, h, w, 0 }; cui__vg_line(g, p, 2, (float)g->light); }
    if (cp != 0x2571) { float p[4] = { 0, 0, w, h }; cui__vg_line(g, p, 2, (float)g->light); }
}

static bool cui__vg_box(cui__vg *g, uint32_t cp) {
    if ((cp >= 0x2504 && cp <= 0x250B) || (cp >= 0x254C && cp <= 0x254F)) {
        uint32_t i = cp >= 0x254C ? cp - 0x254C : cp - 0x2504;
        cui__vg_dashes(g, ((i >> 1) & 1) != 0, cp >= 0x254C ? 2 : (i < 4 ? 3 : 4), (i & 1) != 0);
        return true;
    }
    if (cp >= 0x256D && cp <= 0x2570) {
        cui__vg_arc(g, cp == 0x256D || cp == 0x2570, cp == 0x256D || cp == 0x256E);
        return true;
    }
    if (cp >= 0x2571 && cp <= 0x2573) { cui__vg_diagonals(g, cp); return true; }
    uint8_t a = cui__vt_box_arms(cp);
    if (!a) return false;
    int arm[4] = { a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3 };
    cui__vg_box_lines(g, arm);
    return true;
}

/* ------------------------------------------------------------------------ */
/* Block elements                                                           */
/* ------------------------------------------------------------------------ */

/* the position of the n-th eighth of a length: rounded, so eighths of cells in a row add up */
static int cui__vg_eighth(int len, int n) { return (len * n + 4) / 8; }

/* quadrants: bit 0 upper left, 1 upper right, 2 lower left, 3 lower right */
static void cui__vg_quadrants(cui__vg *g, int mask) {
    int xm = cui__vg_eighth(g->w, 4), ym = g->h / 2;
    if ((mask & 3) == 3) cui__vg_fill(g, 0, 0, g->w, ym);
    else if (mask & 1) cui__vg_fill(g, 0, 0, xm, ym);
    else if (mask & 2) cui__vg_fill(g, xm, 0, g->w, ym);
    if ((mask & 12) == 12) cui__vg_fill(g, 0, ym, g->w, g->h);
    else if (mask & 4) cui__vg_fill(g, 0, ym, xm, g->h);
    else if (mask & 8) cui__vg_fill(g, xm, ym, g->w, g->h);
}

static bool cui__vg_block(cui__vg *g, uint32_t cp) {
    int w = g->w, h = g->h, xm = cui__vg_eighth(w, 4), ym = h / 2;
    if (cp == 0x2580) cui__vg_fill(g, 0, 0, w, ym);
    else if (cp <= 0x2588) cui__vg_fill(g, 0, h - cui__vg_eighth(h, (int)(cp - 0x2580)), w, h);        /* lower 1/8 .. full */
    else if (cp <= 0x258F) cui__vg_fill(g, 0, 0, cui__vg_eighth(w, (int)(0x2590 - cp)), h);         /* left 7/8 .. 1/8 */
    else if (cp == 0x2590) cui__vg_fill(g, xm, 0, w, h);
    else if (cp <= 0x2593) cui__vg_fillc(g, 0, 0, w, h, cui_alpha(g->col, 0.25f * (float)(cp - 0x2590)));
    else if (cp == 0x2594) { int e = cui__vg_eighth(h, 1); cui__vg_fill(g, 0, 0, w, e < 1 ? 1 : e); }
    else if (cp == 0x2595) { int e = cui__vg_eighth(w, 1); cui__vg_fill(g, w - (e < 1 ? 1 : e), 0, w, h); }
    else {
        static const uint8_t q[10] = { 4, 8, 1, 13, 9, 7, 11, 2, 6, 14 };        /* U+2596..U+259F */
        cui__vg_quadrants(g, q[cp - 0x2596]);
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Braille                                                                  */
/* ------------------------------------------------------------------------ */

/* Dots on a 2 by 4 grid, each as large as fits with a clear pixel between neighbours (also to the next cell), centred in its
 * quarter or eighth of the cell. The size is the same in every cell. A screenful of braille is thousands of dots, so the 256
 * patterns are drawn once into a small texture (per cell size, kept with the context) and each cell is one textured rectangle. */

static int cui__vg_dot_size(const cui__vg *g) {
    int dw = (int)g->cw / 2 - 1, dh = (int)g->ch / 4 - 1;
    int d = dw < dh ? dw : dh;
    int cap = (int)(0.8f * (g->cw / 2 < g->ch / 4 ? g->cw / 2 : g->ch / 4));
    if (d > cap) d = cap;
    return d < 1 ? 1 : d;
}

/* where dot i (bit i of the pattern: 0..2 down the left column, 3..5 down the right, 6 and 7 the bottom row) starts in a cell of w by h */
static void cui__vg_dot_pos(int w, int h, int d, int i, int *x0, int *y0) {
    static const uint8_t col_of[8] = { 0, 0, 0, 1, 1, 1, 0, 1 }, row_of[8] = { 0, 1, 2, 0, 1, 2, 3, 3 };
    *x0 = (int)floorf((float)w * (0.25f + 0.5f * (float)col_of[i]) - (float)d * 0.5f + 0.5f);
    *y0 = (int)floorf((float)h * (0.125f + 0.25f * (float)row_of[i]) - (float)d * 0.5f + 0.5f);
}

/* the pixels of one dot of d by d as rectangles (x0, y0, x1, y1 relative to the dot): a square, a plus at 3 pixels, a rounded
 * square at 4; from 5 pixels the dot is a circle (anti-aliased, only in the texture) */
static int cui__vg_dot_rects(int d, int r[3][4]) {
    if (d == 3) { int a[2][4] = { { 0, 1, 3, 2 }, { 1, 0, 2, 3 } }; memcpy(r, a, sizeof a); return 2; }
    if (d == 4) { int a[3][4] = { { 1, 0, 3, 4 }, { 0, 1, 1, 3 }, { 3, 1, 4, 3 } }; memcpy(r, a, sizeof a); return 3; }
    r[0][0] = 0; r[0][1] = 0; r[0][2] = d; r[0][3] = d;
    return 1;
}

typedef struct {
    int w, h, d;               /* the cell size and the dot size this texture is for */
    cui_texture tex;
    int aw, ah;                /* the texture's size in pixels: 16 by 16 cells, each with a transparent pixel round it */
    uint32_t used;
} cui__vg_atlas;

typedef struct { cui__vg_atlas a[6]; uint32_t tick; } cui__vg_cache;

static void cui__vg_cache_free(cui_ctx *ui, void *data) {
    cui__vg_cache *c = (cui__vg_cache *)data;
    for (int i = 0; i < 6; i++) if (c->a[i].tex) cui_image_destroy(ui, c->a[i].tex);
}

static void cui__vg_atlas_build(cui_ctx *ui, cui__vg_atlas *at, int w, int h, int d) {
    int sw = w + 2, sh = h + 2, aw = sw * 16, ah = sh * 16;
    unsigned char *px = (unsigned char *)calloc((size_t)aw * (size_t)ah, 4);
    if (!px) return;
    for (int p = 0; p < 256; p++) {
        int ox = (p & 15) * sw + 1, oy = (p >> 4) * sh + 1;
        for (int i = 0; i < 8; i++) {
            if (!(p & (1 << i))) continue;
            int x0, y0;
            cui__vg_dot_pos(w, h, d, i, &x0, &y0);
            for (int y = 0; y < d; y++) for (int x = 0; x < d; x++) {
                int xx = x0 + x, yy = y0 + y;
                if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                float a;
                if (d >= 5) {                                  /* a circle: 4 by 4 samples per pixel */
                    int in = 0;
                    float rad = (float)d * 0.5f;
                    for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                        float dx = (float)x + ((float)sx + 0.5f) * 0.25f - rad, dy = (float)y + ((float)sy + 0.5f) * 0.25f - rad;
                        in += dx * dx + dy * dy <= rad * rad;
                    }
                    a = (float)in / 16.0f;
                } else {
                    int r[3][4], n = cui__vg_dot_rects(d, r);
                    a = 0;
                    for (int k = 0; k < n; k++) if (x >= r[k][0] && x < r[k][2] && y >= r[k][1] && y < r[k][3]) a = 1;
                }
                unsigned char *q = &px[((size_t)(oy + yy) * (size_t)aw + (size_t)(ox + xx)) * 4];
                q[0] = q[1] = q[2] = 255;
                q[3] = (unsigned char)(a * 255.0f + 0.5f);
            }
        }
    }
    at->tex = cui_image_create(ui, aw, ah, px);
    at->w = w; at->h = h; at->d = d; at->aw = aw; at->ah = ah;
    free(px);
}

static bool cui__vg_braille_texture(cui__vg *g, unsigned bits, int d) {
    cui__vg_cache *c = (cui__vg_cache *)cui_state(g->ui, 0xB7A11E01u, sizeof *c);
    if (c->tick == 0) { c->tick = 1; cui_state_on_free(g->ui, 0xB7A11E01u, cui__vg_cache_free); }
    cui__vg_atlas *at = NULL, *victim = &c->a[0];
    for (int i = 0; i < 6; i++) {
        cui__vg_atlas *a = &c->a[i];
        if (a->tex && a->w == g->w && a->h == g->h && a->d == d) { at = a; break; }
        if (!a->tex || a->used < victim->used) victim = a;
    }
    if (!at) {
        if (victim->tex) { cui_image_destroy(g->ui, victim->tex); victim->tex = 0; }
        cui__vg_atlas_build(g->ui, victim, g->w, g->h, d);
        if (!victim->tex) return false;
        at = victim;
    }
    at->used = ++c->tick;
    float k = 1.0f / g->s, fw = (float)at->aw, fh = (float)at->ah;
    cui_rect uv = { (float)((bits & 15) * (g->w + 2) + 1) / fw, (float)((bits >> 4) * (g->h + 2) + 1) / fh, (float)g->w / fw, (float)g->h / fh };
    cui_draw_image(g->ui, at->tex, (cui_rect){ (float)g->x * k, (float)g->y * k, (float)g->w * k, (float)g->h * k }, uv, CUI_CORNERS(0), g->col);
    return true;
}

static void cui__vg_braille(cui__vg *g, uint32_t cp) {
    unsigned bits = cp - 0x2800;
    if (!bits) return;
    int d = cui__vg_dot_size(g);
    if (cui__vg_braille_texture(g, bits, d)) return;
    /* no texture (or no memory): the dots as rectangles */
    for (int i = 0; i < 8; i++) {
        if (!(bits & (1u << i))) continue;
        int x0, y0, r[3][4], n = cui__vg_dot_rects(d, r);
        cui__vg_dot_pos(g->w, g->h, d, i, &x0, &y0);
        for (int k = 0; k < n; k++) cui__vg_fill(g, x0 + r[k][0], y0 + r[k][1], x0 + r[k][2], y0 + r[k][3]);
    }
}

/* ------------------------------------------------------------------------ */
/* Sextants, wedges and the other symbols for legacy computing               */
/* ------------------------------------------------------------------------ */

/* a polygon of the cell, points in device pixels relative to its corner (anti-aliased) */
static void cui__vg_poly(cui__vg *g, const float *xy, int n, cui_color col) {
    cui_vec2 p[20];
    float k = 1.0f / g->s;
    if (n > 20) n = 20;
    for (int i = 0; i < n; i++) p[i] = (cui_vec2){ ((float)g->x + xy[2 * i]) * k, ((float)g->y + xy[2 * i + 1]) * k };
    cui_draw_polygon(g->ui, p, n, col);
}

/* U+1FB00..U+1FB3B: the 60 patterns of six cells (bit 0 upper left, 1 upper right, 2 middle left, 3 middle right, 4 lower left,
 * 5 lower right), in order of their value, without the empty one, the left half (21), the right half (42) and the full one. 0: not one. */
int cui__vt_sextant_bits(uint32_t cp) {
    if (cp < 0x1FB00 || cp > 0x1FB3B) return 0;
    int b = (int)(cp - 0x1FB00) + 1;
    if (b >= 21) b++;
    if (b >= 42) b++;
    return b;
}

static void cui__vg_sextant(cui__vg *g, int bits) {
    int xm = cui__vg_eighth(g->w, 4), ye[4] = { 0, (2 * g->h + 3) / 6, (4 * g->h + 3) / 6, g->h };
    for (int r = 0; r < 3; r++) {
        int l = (bits >> (2 * r)) & 1, rt = (bits >> (2 * r + 1)) & 1;
        if (l && rt) cui__vg_fill(g, 0, ye[r], g->w, ye[r + 1]);
        else if (l) cui__vg_fill(g, 0, ye[r], xm, ye[r + 1]);
        else if (rt) cui__vg_fill(g, xm, ye[r], g->w, ye[r + 1]);
    }
}

/* U+1FB3C..U+1FB67 "X block diagonal P to Q": the side of the line P-Q that holds corner X. Points on the cell's edge:
 * 0 upper left, 1 upper middle left (1/3 down), 2 lower middle left (2/3), 3 lower left, 4 upper centre, 5 lower centre,
 * 6 upper right, 7 upper middle right, 8 lower middle right, 9 lower right. Corners: 0 lower left, 1 lower right, 2 upper left, 3 upper right. */
static const uint8_t cui__vg_wedges[44][3] = {
    {0,2,5},{0,2,9},{0,1,5},{0,1,9},{0,0,5},{1,1,4},{1,1,6},{1,2,4},
    {1,2,6},{1,3,4},{1,2,7},{1,5,8},{1,3,8},{1,5,7},{1,3,7},{1,5,6},
    {0,4,7},{0,0,7},{0,4,8},{0,0,8},{0,4,9},{0,1,8},{3,2,5},{3,2,9},
    {3,1,5},{3,1,9},{3,0,5},{2,1,4},{2,1,6},{2,2,4},{2,2,6},{2,3,4},
    {2,2,7},{2,5,8},{2,3,8},{2,5,7},{2,3,7},{2,5,6},{3,4,7},{3,0,7},
    {3,4,8},{3,0,8},{3,4,9},{3,1,8},
};

static void cui__vg_wedge(cui__vg *g, int idx) {
    float w = (float)g->w, h = (float)g->h, xm = (float)cui__vg_eighth(g->w, 4);
    float y1 = (float)((2 * g->h + 3) / 6), y2 = (float)((4 * g->h + 3) / 6);
    const float pt[10][2] = { { 0, 0 }, { 0, y1 }, { 0, y2 }, { 0, h }, { xm, 0 }, { xm, h }, { w, 0 }, { w, y1 }, { w, y2 }, { w, h } };
    const float cn[4][2] = { { 0, h }, { w, h }, { 0, 0 }, { w, 0 } };
    const uint8_t *e = cui__vg_wedges[idx];
    float ax = pt[e[1]][0], ay = pt[e[1]][1], dx = pt[e[2]][0] - ax, dy = pt[e[2]][1] - ay;
    float sc = dx * (cn[e[0]][1] - ay) - dy * (cn[e[0]][0] - ax);          /* which side of the line holds the corner */
    const float ring[4][2] = { { 0, 0 }, { w, 0 }, { w, h }, { 0, h } };
    float out[2 * 8];
    int n = 0;
    /* clip the cell's rectangle to that side of the line (Sutherland-Hodgman, one edge) */
    for (int i = 0; i < 4; i++) {
        const float *a = ring[i], *b = ring[(i + 1) & 3];
        float sa = (dx * (a[1] - ay) - dy * (a[0] - ax)) * sc, sb = (dx * (b[1] - ay) - dy * (b[0] - ax)) * sc;
        if (sa >= 0) { out[2 * n] = a[0]; out[2 * n + 1] = a[1]; n++; }
        if ((sa > 0 && sb < 0) || (sa < 0 && sb > 0)) {
            float t = sa / (sa - sb);
            out[2 * n] = a[0] + (b[0] - a[0]) * t; out[2 * n + 1] = a[1] + (b[1] - a[1]) * t; n++;
        }
    }
    if (n >= 3) cui__vg_poly(g, out, n, g->col);
}

/* U+1FB68..U+1FB6F: the three quarters left when one quarter triangle is missing, and the quarter triangles (left, upper, right, lower) */
static void cui__vg_triangles(cui__vg *g, uint32_t cp) {
    float w = (float)g->w, h = (float)g->h, xm = (float)cui__vg_eighth(g->w, 4), ym = (float)(g->h / 2);
    int xi = cui__vg_eighth(g->w, 4), yi = g->h / 2;
    if (cp >= 0x1FB6C) {
        int side = (int)(cp - 0x1FB6C);
        float t[4][6] = { { 0, 0, 0, h, xm, ym }, { 0, 0, w, 0, xm, ym }, { w, 0, w, h, xm, ym }, { 0, h, w, h, xm, ym } };
        cui__vg_poly(g, t[side], 3, g->col);
        return;
    }
    /* the missing quarter: 0x1FB68 left, 0x1FB69 upper, 0x1FB6A right, 0x1FB6B lower; a rectangle of the far half and two triangles */
    if (cp == 0x1FB68) {        /* upper, right, lower: the left quarter is missing */
        cui__vg_fill(g, xi, 0, g->w, g->h);
        float a[6] = { 0, 0, xm, 0, xm, ym }, b[6] = { 0, h, xm, h, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    } else if (cp == 0x1FB69) { /* left, lower, right: the upper quarter is missing */
        cui__vg_fill(g, 0, yi, g->w, g->h);
        float a[6] = { 0, 0, 0, ym, xm, ym }, b[6] = { w, 0, w, ym, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    } else if (cp == 0x1FB6A) { /* upper, left, lower: the right quarter is missing */
        cui__vg_fill(g, 0, 0, xi, g->h);
        float a[6] = { w, 0, xm, 0, xm, ym }, b[6] = { w, h, xm, h, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    } else {                    /* left, upper, right: the lower quarter is missing */
        cui__vg_fill(g, 0, 0, g->w, yi);
        float a[6] = { 0, h, 0, ym, xm, ym }, b[6] = { w, h, w, ym, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    }
}

/* U+1FB70..U+1FB94: eighth columns and rows, quarter-steps, medium shades of halves */
static bool cui__vg_legacy(cui__vg *g, uint32_t cp) {
    int w = g->w, h = g->h, xm = cui__vg_eighth(w, 4), ym = h / 2;
    cui_color med = cui_alpha(g->col, 0.5f);
    if (cp <= 0x1FB75) { int n = (int)(cp - 0x1FB70) + 1; cui__vg_fill(g, cui__vg_eighth(w, n), 0, cui__vg_eighth(w, n + 1), h); }
    else if (cp <= 0x1FB7B) { int n = (int)(cp - 0x1FB76) + 1; cui__vg_fill(g, 0, cui__vg_eighth(h, n), w, cui__vg_eighth(h, n + 1)); }
    else if (cp <= 0x1FB7F) {
        int e = cui__vg_eighth(w, 1), f = cui__vg_eighth(h, 1);
        if (e < 1) e = 1;
        if (f < 1) f = 1;
        bool left = cp == 0x1FB7C || cp == 0x1FB7D, lower = cp == 0x1FB7C || cp == 0x1FB7F;
        if (left) cui__vg_fill(g, 0, 0, e, h); else cui__vg_fill(g, w - e, 0, w, h);
        if (lower) cui__vg_fill(g, 0, h - f, w, h); else cui__vg_fill(g, 0, 0, w, f);
    }
    else if (cp == 0x1FB80) { int f = cui__vg_eighth(h, 1); if (f < 1) f = 1; cui__vg_fill(g, 0, 0, w, f); cui__vg_fill(g, 0, h - f, w, h); }
    else if (cp == 0x1FB81) { static const uint8_t rows[4] = { 0, 2, 4, 7 }; for (int i = 0; i < 4; i++) cui__vg_fill(g, 0, cui__vg_eighth(h, rows[i]), w, cui__vg_eighth(h, rows[i] + 1)); }
    else if (cp <= 0x1FB86) { static const uint8_t n[5] = { 2, 3, 5, 6, 7 }; cui__vg_fill(g, 0, 0, w, cui__vg_eighth(h, n[cp - 0x1FB82])); }
    else if (cp <= 0x1FB8B) { static const uint8_t n[5] = { 2, 3, 5, 6, 7 }; cui__vg_fill(g, w - cui__vg_eighth(w, n[cp - 0x1FB87]), 0, w, h); }
    else if (cp == 0x1FB8C) cui__vg_fillc(g, 0, 0, xm, h, med);
    else if (cp == 0x1FB8D) cui__vg_fillc(g, xm, 0, w, h, med);
    else if (cp == 0x1FB8E) cui__vg_fillc(g, 0, 0, w, ym, med);
    else if (cp == 0x1FB8F) cui__vg_fillc(g, 0, ym, w, h, med);
    else if (cp == 0x1FB90) cui__vg_fillc(g, 0, 0, w, h, med);
    else if (cp == 0x1FB91) { cui__vg_fill(g, 0, 0, w, ym); cui__vg_fillc(g, 0, ym, w, h, med); }
    else if (cp == 0x1FB92) { cui__vg_fillc(g, 0, 0, w, ym, med); cui__vg_fill(g, 0, ym, w, h); }
    else if (cp == 0x1FB94) { cui__vg_fillc(g, 0, 0, xm, h, med); cui__vg_fill(g, xm, 0, w, h); }
    else return false;
    return true;
}

/* U+1FB9A..U+1FB9F: the bow ties and the triangles in medium shade */
static bool cui__vg_halves(cui__vg *g, uint32_t cp) {
    float w = (float)g->w, h = (float)g->h, xm = (float)cui__vg_eighth(g->w, 4), ym = (float)(g->h / 2);
    if (cp == 0x1FB9A) {
        float a[6] = { 0, 0, w, 0, xm, ym }, b[6] = { 0, h, w, h, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    } else if (cp == 0x1FB9B) {
        float a[6] = { 0, 0, 0, h, xm, ym }, b[6] = { w, 0, w, h, xm, ym };
        cui__vg_poly(g, a, 3, g->col); cui__vg_poly(g, b, 3, g->col);
    } else {
        float t[4][6] = { { 0, 0, w, 0, 0, h }, { 0, 0, w, 0, w, h }, { w, 0, w, h, 0, h }, { 0, 0, w, h, 0, h } };
        cui__vg_poly(g, t[cp - 0x1FB9C], 3, cui_alpha(g->col, 0.5f));
    }
    return true;
}

/* ------------------------------------------------------------------------ */
/* Status line shapes                                                       */
/* ------------------------------------------------------------------------ */

/* U+E0B0..U+E0BE: solid triangles filling the cell edge to edge (so they join the coloured cell next to them without a
 * seam), thin chevrons, half circles, and the corner triangles */
static bool cui__vg_powerline(cui__vg *g, uint32_t cp) {
    float w = (float)g->w, h = (float)g->h, t = (float)g->light;
    switch (cp) {
    case 0xE0B0: { float p[6] = { 0, 0, w, h * 0.5f, 0, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    case 0xE0B2: { float p[6] = { w, 0, 0, h * 0.5f, w, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    case 0xE0B1: case 0xE0B3: {
        float ang = atan2f(h * 0.5f, w), tip = w - t * 0.5f / sinf(ang);
        ang = atan2f(h * 0.5f, tip > 1 ? tip : 1); tip = w - t * 0.5f / sinf(ang);
        if (tip < 1) tip = 1;
        if (cp == 0xE0B1) { float p[6] = { 0, 0, tip, h * 0.5f, 0, h }; cui__vg_line(g, p, 3, t); }
        else { float p[6] = { w, 0, w - tip, h * 0.5f, w, h }; cui__vg_line(g, p, 3, t); }
        return true;
    }
    case 0xE0B4: case 0xE0B6: {              /* half circles: the flat side on the cell's edge, as wide as the cell */
        float p[2 * 17];
        const int n = 16;
        for (int i = 0; i <= n; i++) {
            float a = -CUI_PI * 0.5f + (float)i * CUI_PI / (float)n, c = cosf(a), s = sinf(a);
            p[2 * i] = cp == 0xE0B4 ? w * c : w - w * c;
            p[2 * i + 1] = h * 0.5f + h * 0.5f * s;
        }
        cui__vg_poly(g, p, n + 1, g->col);
        return true;
    }
    case 0xE0B5: case 0xE0B7: {              /* the outlines of the half circles */
        float p[2 * 17], rx = w - t * 0.5f, ry = h * 0.5f - t * 0.5f;
        const int n = 16;
        for (int i = 0; i <= n; i++) {
            float a = -CUI_PI * 0.5f + (float)i * CUI_PI / (float)n, c = cosf(a), s = sinf(a);
            p[2 * i] = cp == 0xE0B5 ? rx * c : w - rx * c;
            p[2 * i + 1] = h * 0.5f + ry * s;
        }
        cui__vg_line(g, p, n + 1, t);
        return true;
    }
    case 0xE0B8: { float p[6] = { 0, 0, w, h, 0, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    case 0xE0BA: { float p[6] = { w, 0, w, h, 0, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    case 0xE0BC: { float p[6] = { 0, 0, w, 0, 0, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    case 0xE0BE: { float p[6] = { 0, 0, w, 0, w, h }; cui__vg_poly(g, p, 3, g->col); return true; }
    default: return false;
    }
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

bool cui__vt_glyph(cui_ctx *ui, uint32_t cp, cui_rect cell, cui_color col) {
    if (cp < 0x2500) return false;
    bool ours = (cp <= 0x259F) || (cp >= 0x2800 && cp <= 0x28FF) || (cp >= 0xE0B0 && cp <= 0xE0BE) || (cp >= 0x1FB00 && cp <= 0x1FB9F);
    if (!ours) return false;
    cui__vg g;
    g.ui = ui;
    g.s = ui->scale > 0 ? ui->scale : 1.0f;
    float x0 = floorf(cell.x * g.s + 0.5f), y0 = floorf(cell.y * g.s + 0.5f);
    float x1 = floorf((cell.x + cell.w) * g.s + 0.5f), y1 = floorf((cell.y + cell.h) * g.s + 0.5f);
    g.x = (int)x0; g.y = (int)y0; g.w = (int)(x1 - x0); g.h = (int)(y1 - y0);
    g.cw = cell.w * g.s; g.ch = cell.h * g.s;
    g.col = col;
    int lt = (int)(g.ch / 16.0f + 0.5f);
    g.light = lt < 1 ? 1 : lt;
    g.nrc = 0;
    if (g.w <= 0 || g.h <= 0) return true;
    bool drawn = true;
    if (cp <= 0x257F) drawn = cui__vg_box(&g, cp);
    else if (cp <= 0x259F) drawn = cui__vg_block(&g, cp);
    else if (cp >= 0x2800 && cp <= 0x28FF) cui__vg_braille(&g, cp);
    else if (cp >= 0xE0B0 && cp <= 0xE0BE) drawn = cui__vg_powerline(&g, cp);
    else if (cp <= 0x1FB3B) cui__vg_sextant(&g, cui__vt_sextant_bits(cp));
    else if (cp <= 0x1FB67) cui__vg_wedge(&g, (int)(cp - 0x1FB3C));
    else if (cp <= 0x1FB6F) cui__vg_triangles(&g, cp);
    else if (cp <= 0x1FB94) drawn = cui__vg_legacy(&g, cp);
    else if (cp >= 0x1FB9A) drawn = cui__vg_halves(&g, cp);
    else drawn = false;
    cui__vg_flush(&g);
    return drawn;
}
