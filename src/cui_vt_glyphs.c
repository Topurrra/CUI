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
 * strokes (the same pixels) as the lines of the neighbouring cells. */
static void cui__vg_arc(cui__vg *g, bool right, bool down) {
    int vs[2][2], hs[2][2];
    cui__vg_strokes(1, g->w, g->light, g->light, vs);
    cui__vg_strokes(1, g->h, g->light, g->light, hs);
    float xc = (float)(vs[0][0] + vs[0][1]) * 0.5f, yc = (float)(hs[0][0] + hs[0][1]) * 0.5f;
    float sx = right ? 1.0f : -1.0f, sy = down ? 1.0f : -1.0f;
    float rx = right ? (float)g->w - xc : xc, ry = down ? (float)g->h - yc : yc;
    float rr = rx < ry ? rx : ry;                                    /* a circle: as wide as the half cell, then straight on */
    float cx = xc + sx * rr, cy = yc + sy * rr;
    float p[2 * 17];
    int n = (int)(rr * 0.8f) + 3;                                      /* segments: enough for the radius, few for the cost */
    if (n > 16) n = 16;
    for (int i = 0; i <= n; i++) {
        float a = (float)i * (CUI_PI * 0.5f) / (float)n;
        p[2 * i] = cx - sx * rr * cosf(a);
        p[2 * i + 1] = cy - sy * rr * sinf(a);
    }
    cui__vg_line(g, p, n + 1, (float)g->light + 0.35f);       /* a little wider: one anti-aliased pixel looks thin next to the exact lines */
    /* the straight parts to the cell's edges: the same pixels as the lines of the neighbouring cells */
    int ey = (int)floorf(cy + 0.5f), ex = (int)floorf(cx + 0.5f);
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
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

bool cui__vt_glyph(cui_ctx *ui, uint32_t cp, cui_rect cell, cui_color col) {
    if (cp < 0x2500) return false;
    bool ours = (cp <= 0x257F) || (cp >= 0x2580 && cp <= 0x2593);
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
    else { cui__vg_flush(&g); cui__tm_block(ui, (int)(cp - 0x2580), cell, col); }      /* the block elements: the shared ones until they have their own */
    cui__vg_flush(&g);
    return drawn;
}
