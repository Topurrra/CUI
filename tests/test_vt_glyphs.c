/* Tests for the VT screen's hand-drawn characters (src/cui_vt_glyphs.c): the tables (every box character has the arms the
 * Unicode chart gives, every sextant maps to the right six cells) and the pixels (frames are one connected line at every
 * scale, blocks fill their cell exactly and meet without seams, dashes have even gaps, braille dots stay apart).
 * Runs headless with the software renderer. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static cui_sw_renderer *g_sw;

/* ---- the tables ---- */

/* the arms of U+2500..U+257F from the character names in the Unicode chart (left, right, up, down; 2 bits each): 0 none,
 * 1 light, 2 heavy, 3 double; 0 for the three diagonals */
static const uint8_t k_chart[128] = {
    0x05,0x0A,0x50,0xA0,0x05,0x0A,0x50,0xA0,0x05,0x0A,0x50,0xA0,0x44,0x48,0x84,0x88,
    0x41,0x42,0x81,0x82,0x14,0x18,0x24,0x28,0x11,0x12,0x21,0x22,0x54,0x58,0x64,0x94,
    0xA4,0x68,0x98,0xA8,0x51,0x52,0x61,0x91,0xA1,0x62,0x92,0xA2,0x45,0x46,0x49,0x4A,
    0x85,0x86,0x89,0x8A,0x15,0x16,0x19,0x1A,0x25,0x26,0x29,0x2A,0x55,0x56,0x59,0x5A,
    0x65,0x95,0xA5,0x66,0x69,0x96,0x99,0x6A,0x9A,0xA6,0xA9,0xAA,0x05,0x0A,0x50,0xA0,
    0x0F,0xF0,0x4C,0xC4,0xCC,0x43,0xC1,0xC3,0x1C,0x34,0x3C,0x13,0x31,0x33,0x5C,0xF4,
    0xFC,0x53,0xF1,0xF3,0x4F,0xC5,0xCF,0x1F,0x35,0x3F,0x5F,0xF5,0xFF,0x44,0x41,0x11,
    0x14,0x00,0x00,0x00,0x01,0x10,0x04,0x40,0x02,0x20,0x08,0x80,0x09,0x90,0x06,0x60,
};

/* U+1FB00..U+1FB3B: the cells named in "BLOCK SEXTANT-135" as bits (cell 1 is bit 0) */
static const uint8_t k_sextants[60] = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,22,23,24,25,26,27,28,29,30,31,
    32,33,34,35,36,37,38,39,40,41,43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62,
};

static void test_tables(void) {
    for (uint32_t c = 0x2500; c <= 0x257F; c++) {
        if (cui__vt_box_arms(c) != k_chart[c - 0x2500]) { printf("  U+%04X: arms %02X, chart %02X\n", c, cui__vt_box_arms(c), k_chart[c - 0x2500]); g_failed++; }
        g_checks++;
    }
    CHECK_EQ(cui__vt_box_arms(0x24FF), 0);
    CHECK_EQ(cui__vt_box_arms(0x2580), 0);
    for (uint32_t c = 0x1FB00; c <= 0x1FB3B; c++) CHECK_EQ(cui__vt_sextant_bits(c), k_sextants[c - 0x1FB00]);
    CHECK_EQ(cui__vt_sextant_bits(0x1FAFF), 0);
    CHECK_EQ(cui__vt_sextant_bits(0x1FB3C), 0);
    /* the 60 patterns are all different and are none of: empty, left half (21), right half (42), full (63) */
    int seen[64] = { 0 };
    for (uint32_t c = 0x1FB00; c <= 0x1FB3B; c++) seen[cui__vt_sextant_bits(c)]++;
    CHECK_EQ(seen[0] + seen[21] + seen[42] + seen[63], 0);
    int distinct = 0;
    for (int i = 0; i < 64; i++) distinct += seen[i] == 1;
    CHECK_EQ(distinct, 60);
}

/* ---- pixels ---- */

typedef struct { const uint32_t *cps; int cols, rows; float cw, lh, ox, oy; } sheet;
typedef struct { int w, h; unsigned char *cov; } image;      /* coverage 0..255 of white on black, from the red channel */

static cui_rect g_canvas;       /* where the sheet was painted, in logical pixels */

static void paint_sheet(cui_ctx *ui, cui_rect r, void *user) {
    g_canvas = r;
    const sheet *s = (const sheet *)user;
    cui_draw_rect(ui, r, CUI_CORNERS(0), cui__rgba(0, 0, 0, 255));
    for (int y = 0; y < s->rows; y++)
        for (int x = 0; x < s->cols; x++) {
            uint32_t cp = s->cps[y * s->cols + x];
            if (cp && cp != ' ') cui__vt_glyph(ui, cp, (cui_rect){ r.x + s->ox + (float)x * s->cw, r.y + s->oy + (float)y * s->lh, s->cw, s->lh }, cui__rgba(255, 255, 255, 255));
        }
}

static void render(const sheet *s, float scale, image *im) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    float W = s->ox * 2 + (float)s->cols * s->cw + 4, H = s->oy * 2 + (float)s->rows * s->lh + 4;
    cui_set_viewport(ui, W, H, scale);
    cui_set_reduced_motion(ui, true);
    for (int i = 0; i < 2; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) cui_canvas(ui, "sheet", CUI_FILL, CUI_FILL, paint_sheet, s, sizeof *s);
        cui_end_frame(ui);
    }
    const cui_draw_data *dd = cui_get_draw_data(ui);
    int w = dd->width_px, h = dd->height_px;
    unsigned char *px = (unsigned char *)malloc((size_t)w * (size_t)h * 4);
    cui_sw_render(g_sw, dd, px, w, h, w * 4, CUI_SW_RGBA);
    im->w = w; im->h = h;
    im->cov = (unsigned char *)malloc((size_t)w * (size_t)h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {          /* only the sheet itself: what is round it is the theme's background */
        bool in = x >= (int)(g_canvas.x * scale + 0.5f) && x < (int)((g_canvas.x + g_canvas.w) * scale + 0.5f) && y >= (int)(g_canvas.y * scale + 0.5f) && y < (int)((g_canvas.y + g_canvas.h) * scale + 0.5f);
        im->cov[y * w + x] = in ? px[(y * w + x) * 4] : 0;
    }
    free(px);
    cui_destroy(ui);
}

static int cov(const image *im, int x, int y) { return x < 0 || y < 0 || x >= im->w || y >= im->h ? 0 : im->cov[y * im->w + x]; }

/* decode UTF-8 rows into code points */
static uint32_t *decode(const char *const *rows, int n, int *cols) {
    int maxc = 0;
    uint32_t *cp = (uint32_t *)calloc((size_t)n * 64, sizeof(uint32_t));
    for (int y = 0; y < n; y++) {
        const unsigned char *s = (const unsigned char *)rows[y];
        int x = 0;
        while (*s) {
            uint32_t c = *s++;
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            if (extra) c &= (0x3Fu >> extra);
            while (extra--) c = (c << 6) | (*s++ & 0x3F);
            cp[y * 64 + x++] = c;
        }
        if (x > maxc) maxc = x;
    }
    uint32_t *out = (uint32_t *)calloc((size_t)n * (size_t)maxc, sizeof(uint32_t));
    for (int y = 0; y < n; y++) memcpy(out + y * maxc, cp + y * 64, (size_t)maxc * sizeof(uint32_t));
    free(cp);
    *cols = maxc;
    return out;
}

static void render_rows(const char *const *rows, int n, float cw, float lh, float scale, image *im, sheet *sh) {
    int cols;
    uint32_t *cps = decode(rows, n, &cols);
    *sh = (sheet){ cps, cols, n, cw, lh, 3, 2 };
    render(sh, scale, im);
}

static void free_all(image *im, sheet *sh) { free(im->cov); free((void *)sh->cps); }

/* connected pieces of pixels at least `thr` covered (4-neighbours), or of the pixels below it when `bg` */
static int components(const image *im, int thr, bool bg) {
    int n = im->w * im->h, count = 0;
    unsigned char *seen = (unsigned char *)calloc((size_t)n, 1);
    int *stack = (int *)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) {
        bool on = bg ? im->cov[i] < thr : im->cov[i] >= thr;
        if (!on || seen[i]) continue;
        count++;
        int sp = 0;
        stack[sp++] = i; seen[i] = 1;
        while (sp) {
            int p = stack[--sp], x = p % im->w, y = p / im->w;
            int nb[4] = { x > 0 ? p - 1 : -1, x + 1 < im->w ? p + 1 : -1, y > 0 ? p - im->w : -1, y + 1 < im->h ? p + im->w : -1 };
            for (int k = 0; k < 4; k++) {
                int q = nb[k];
                if (q < 0 || seen[q]) continue;
                bool qon = bg ? im->cov[q] < thr : im->cov[q] >= thr;
                if (qon) { seen[q] = 1; stack[sp++] = q; }
            }
        }
    }
    free(seen); free(stack);
    return count;
}

static void bbox(const image *im, int thr, int *x0, int *y0, int *x1, int *y1) {
    *x0 = im->w; *y0 = im->h; *x1 = -1; *y1 = -1;
    for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) if (im->cov[y * im->w + x] >= thr) {
        if (x < *x0) *x0 = x;
        if (x > *x1) *x1 = x;
        if (y < *y0) *y0 = y;
        if (y > *y1) *y1 = y;
    }
}

/* every pixel inside the box of the lit pixels is lit fully: a solid rectangle, no seam, no gap */
static bool solid_rect(const image *im, int *bw, int *bh) {
    int x0, y0, x1, y1;
    bbox(im, 64, &x0, &y0, &x1, &y1);
    if (x1 < 0) return false;
    if (getenv("GLYPH_DEBUG")) { for (int y = 0; y < im->h; y++) for (int x = 0; x < im->w; x++) if ((x == x0 || y == y0) && cov(im, x, y) >= 1) { printf("  lit at %d,%d = %d (image %d x %d, canvas %g,%g %gx%g)\n", x, y, cov(im, x, y), im->w, im->h, g_canvas.x, g_canvas.y, g_canvas.w, g_canvas.h); y = im->h; break; } }
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) if (cov(im, x, y) != 255) {
        printf("  not solid: box %d,%d..%d,%d, pixel %d,%d is %d\n", x0, y0, x1, y1, x, y, cov(im, x, y));
        return false;
    }
    if (bw) *bw = x1 - x0 + 1;
    if (bh) *bh = y1 - y0 + 1;
    return true;
}

static const float k_scales[] = { 1.0f, 1.25f, 1.5f, 2.0f };
static const float k_cw[] = { 7.8f, 8.0f, 9.1f, 7.0f };
static const float k_lh[] = { 19.0f, 20.0f, 23.0f, 17.0f };

static void test_solid_blocks(void) {
    for (int si = 0; si < 4; si++) for (int ci = 0; ci < 4; ci++) {
        float s = k_scales[si], cw = k_cw[ci], lh = k_lh[ci];
        image im; sheet sh; int bw, bh;
        /* a field of full blocks is one solid rectangle */
        const char *full[] = { "██████████", "██████████", "██████████" };
        render_rows(full, 3, cw, lh, s, &im, &sh);
        CHECK(solid_rect(&im, &bw, &bh));
        CHECK(bw >= (int)(10 * cw * s) - 1 && bw <= (int)(10 * cw * s) + 2);
        CHECK(bh >= (int)(3 * lh * s) - 1 && bh <= (int)(3 * lh * s) + 2);
        free_all(&im, &sh);
        /* lower half over upper half, upper half over lower half: no line between the cells (rows) */
        const char *rows[] = { "▄▄▄▄▄▄", "▀▀▀▀▀▀" };
        render_rows(rows, 2, cw, lh, s, &im, &sh);
        CHECK(solid_rect(&im, &bw, &bh));
        CHECK(bh >= (int)(lh * s) - 1 && bh <= (int)(lh * s) + 2);
        free_all(&im, &sh);
        /* right half then left half across columns */
        const char *cols[] = { "▐▌", "▐▌" };
        render_rows(cols, 2, cw, lh, s, &im, &sh);
        CHECK(solid_rect(&im, &bw, &bh));
        CHECK(bw >= (int)(cw * s) - 1 && bw <= (int)(cw * s) + 2);
        CHECK(bh >= (int)(2 * lh * s) - 1 && bh <= (int)(2 * lh * s) + 2);
        free_all(&im, &sh);
        int x0, y0, x1, y1;
        /* a column of lower blocks 7/8 stacked with the full block is solid too */
        const char *mix[] = { "▇▇▇", "███", "▀▀▀" };
        render_rows(mix, 3, cw, lh, s, &im, &sh);
        bbox(&im, 64, &x0, &y0, &x1, &y1);
        CHECK(y1 - y0 + 1 >= (int)(2 * lh * s));
        free_all(&im, &sh);
    }
}

static void test_blocks(void) {
    /* the shades are 25, 50 and 75 percent of the text colour over the cell */
    {
        const char *rows[] = { "░▒▓" };
        image im; sheet sh;
        render_rows(rows, 1, 8, 16, 1, &im, &sh);
        int base = 3 + 4, top = 2 + 4;
        CHECK(abs(cov(&im, base, top) - 64) <= 1);
        CHECK(abs(cov(&im, base + 8, top) - 128) <= 1);
        CHECK(abs(cov(&im, base + 16, top) - 191) <= 1);
        free_all(&im, &sh);
    }
    /* eighths: the covered fraction of a 8 by 16 cell is exact */
    for (uint32_t c = 0x2581; c <= 0x2588; c++) {
        char b[5] = { (char)0xE2, (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
        const char *rows[] = { b };
        image im; sheet sh;
        render_rows(rows, 1, 8, 16, 1, &im, &sh);
        long sum = 0;
        for (int i = 0; i < im.w * im.h; i++) sum += im.cov[i];
        CHECK_EQ(sum, 255L * 8 * 2 * (long)(c - 0x2580));
        free_all(&im, &sh);
    }
    for (uint32_t c = 0x2589; c <= 0x258F; c++) {
        char b[5] = { (char)0xE2, (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
        const char *rows[] = { b };
        image im; sheet sh;
        render_rows(rows, 1, 8, 16, 1, &im, &sh);
        long sum = 0;
        for (int i = 0; i < im.w * im.h; i++) sum += im.cov[i];
        CHECK_EQ(sum, 255L * 16 * (long)(0x2590 - c));
        free_all(&im, &sh);
    }
    /* the quadrants: the cell's four quarters are lit as the name says (upper left, upper right, lower left, lower right) */
    {
        static const int want[10] = { 0x4, 0x8, 0x1, 0xD, 0x9, 0x7, 0xB, 0x2, 0x6, 0xE };
        for (int i = 0; i < 10; i++) {
            uint32_t c = 0x2596 + (uint32_t)i;
            char b[5] = { (char)0xE2, (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
            const char *rows[] = { b };
            image im; sheet sh;
            render_rows(rows, 1, 8, 16, 1, &im, &sh);
            int m = (cov(&im, 3 + 2, 2 + 4) > 128) | ((cov(&im, 3 + 6, 2 + 4) > 128) << 1) | ((cov(&im, 3 + 2, 2 + 12) > 128) << 2) | ((cov(&im, 3 + 6, 2 + 12) > 128) << 3);
            CHECK_EQ(m, want[i]);
            free_all(&im, &sh);
        }
    }
    /* upper and right one eighth */
    {
        const char *rows[] = { "▔▕" };
        image im; sheet sh;
        render_rows(rows, 1, 8, 16, 1, &im, &sh);
        CHECK_EQ(cov(&im, 3 + 2, 2 + 0), 255); CHECK_EQ(cov(&im, 3 + 2, 2 + 1), 255); CHECK_EQ(cov(&im, 3 + 2, 2 + 2), 0);
        CHECK_EQ(cov(&im, 3 + 15, 2 + 8), 255); CHECK_EQ(cov(&im, 3 + 14, 2 + 8), 0);
        free_all(&im, &sh);
    }
}

/* frames: every line of the picture is joined to the next, at every scale and cell size */
static void test_frames(void) {
    static const char *light[] = { "┌─┬─┐", "│ │ │", "├─┼─┤", "│ │ │", "└─┴─┘" };
    static const char *heavy[] = { "┏━┳━┓", "┃ ┃ ┃", "┣━╋━┫", "┃ ┃ ┃", "┗━┻━┛" };
    static const char *dbl[] = { "╔═╦═╗", "║ ║ ║", "╠═╬═╣", "║ ║ ║", "╚═╩═╝" };
    static const char *round[] = { "╭───╮", "│   │", "│   │", "╰───╯" };
    static const char *mixed[] = { "┍━┯━┑", "│ │ │", "┝━┿━┥", "│ │ │", "┕━┷━┙" };
    static const char *dsingle[] = { "╒═╤═╕", "│ │ │", "╞═╪═╡", "│ │ │", "╘═╧═╛" };
    static const char *sdouble[] = { "╓─╥─╖", "║ ║ ║", "╟─╫─╢", "║ ║ ║", "╙─╨─╜" };
    static const char *halves[] = { "┎─┰─┒", "┃ ┃ ┃", "┠─╂─┨", "┃ ┃ ┃", "┖─┸─┚" };
    struct { const char *const *rows; int n; const char *name; int pieces; } cases[] = {
        { light, 5, "light", 1 }, { heavy, 5, "heavy", 1 }, { dbl, 5, "double", 5 }, { round, 4, "round", 1 }, { mixed, 5, "mixed", 1 },
        { dsingle, 5, "double/single", 1 }, { sdouble, 5, "single/double", 1 }, { halves, 5, "heavy/light", 1 },
    };
    for (int c = 0; c < 8; c++) for (int si = 0; si < 4; si++) for (int ci = 0; ci < 4; ci++) {
        image im; sheet sh;
        render_rows(cases[c].rows, cases[c].n, k_cw[ci], k_lh[ci], k_scales[si], &im, &sh);
        int n = components(&im, 64, false);
        if (n != cases[c].pieces) printf("  frame %s at scale %.2f, cell %.1f x %.0f: %d pieces\n", cases[c].name, k_scales[si], k_cw[ci], k_lh[ci], n);
        CHECK_EQ(n, cases[c].pieces);
        free_all(&im, &sh);
    }
    /* a double frame keeps its two lines apart all the way round: outside, inside and the channel between the lines */
    for (int si = 0; si < 4; si++) {
        static const char *box[] = { "╔═══╗", "║   ║", "║   ║", "╚═══╝" };
        image im; sheet sh;
        render_rows(box, 4, 8, 20, k_scales[si], &im, &sh);
        int bgc = components(&im, 128, true);
        if (bgc != 3) printf("  double frame at scale %.2f: %d background pieces\n", k_scales[si], bgc);
        CHECK_EQ(bgc, 3);
        free_all(&im, &sh);
    }
    /* a double grid: the junctions open the channels the same way on every side (outside, 4 rooms, the channels in one piece) */
    {
        static const char *grid[] = { "╔═╦═╗", "║ ║ ║", "╠═╬═╣", "║ ║ ║", "╚═╩═╝" };
        image im; sheet sh;
        render_rows(grid, 5, 8, 20, 1, &im, &sh);
        int bgc = components(&im, 128, true);
        CHECK_EQ(bgc, 1 + 4 + 1);
        free_all(&im, &sh);
    }
}

static void test_lines(void) {
    /* at 8 by 16 pixels: a light line is one pixel, centred, and the same pixels in every cell of a row */
    const char *rows[] = { "────", "│││││" };
    image im; sheet sh;
    render_rows(rows, 2, 8, 16, 1, &im, &sh);
    for (int x = 3; x < 3 + 32; x++) { CHECK_EQ(cov(&im, x, 2 + 7), 255); CHECK_EQ(cov(&im, x, 2 + 6), 0); CHECK_EQ(cov(&im, x, 2 + 8), 0); }
    for (int y = 2 + 16; y < 2 + 32; y++) { CHECK_EQ(cov(&im, 3 + 3, y), 255); CHECK_EQ(cov(&im, 3 + 2, y), 0); CHECK_EQ(cov(&im, 3 + 4, y), 0); }
    free_all(&im, &sh);
    /* a round corner reaches the middle of both edges where the straight lines are */
    const char *arc[] = { "╭─", "│ " };
    render_rows(arc, 2, 8, 16, 1, &im, &sh);
    CHECK(cov(&im, 3 + 3, 2 + 15) >= 200);       /* the bottom edge, the vertical line's pixel */
    CHECK(cov(&im, 3 + 7, 2 + 7) >= 200);        /* the right edge, the horizontal line's pixel */
    CHECK(cov(&im, 3 + 0, 2 + 0) < 40);          /* the cell's outer corner stays empty */
    free_all(&im, &sh);
}

/* runs of lit pixels along one row or column starting at (x, y) */
static int runs(const image *im, int x, int y, int dx, int dy, int len, int *gaps, int maxg, int *dashes) {
    int n = 0, ng = 0, run = 0, gap = 0;
    for (int i = 0; i < len; i++) {
        if (cov(im, x + dx * i, y + dy * i) >= 128) {
            if (run == 0) { if (n > 0 && ng < maxg) gaps[ng++] = gap; n++; }
            run++; dashes[n - 1] = run; gap = 0;
        } else { run = 0; gap++; }
    }
    return n;
}

static void test_dashes(void) {
    static const struct { const char *s; int per; bool vert; } cases[] = {
        { "┄┄┄┄┄┄", 3, false }, { "┅┅┅┅┅┅", 3, false }, { "┈┈┈┈┈┈", 4, false }, { "┉┉┉┉┉┉", 4, false }, { "╌╌╌╌╌╌", 2, false }, { "╍╍╍╍╍╍", 2, false },
        { "┆┆┆┆┆┆", 3, true }, { "┇┇┇┇┇┇", 3, true }, { "┊┊┊┊┊┊", 4, true }, { "┋┋┋┋┋┋", 4, true }, { "╎╎╎╎╎╎", 2, true }, { "╏╏╏╏╏╏", 2, true },
    };
    for (int c = 0; c < 12; c++) for (int si = 0; si < 4; si++) for (int ci = 0; ci < 4; ci++) {
        float s = k_scales[si], cw = k_cw[ci], lh = k_lh[ci];
        image im; sheet sh;
        int cells = 6;
        const char *col[6] = { "┆", "┆", "┆", "┆", "┆", "┆" };
        char one[8][4];
        for (int i = 0; i < 6; i++) {
            const unsigned char *p = (const unsigned char *)cases[c].s;
            memcpy(one[i], p, 3); one[i][3] = 0; col[i] = one[i];
        }
        if (cases[c].vert) render_rows(col, 6, cw, lh, s, &im, &sh);
        else { const char *rows[] = { cases[c].s }; render_rows(rows, 1, cw, lh, s, &im, &sh); }
        /* scan along the middle of the line: find it first */
        int gaps[64], dash[64], n;
        int span = cases[c].vert ? (int)(lh * s * 6) : (int)(cw * s * cells);
        if (cases[c].vert) {
            int px = -1;
            for (int x = 0; x < im.w && px < 0; x++) for (int y = 0; y < im.h; y++) if (cov(&im, x, y) >= 128) { px = x; break; }
            n = runs(&im, px, (int)(2 * s), 0, 1, span, gaps, 64, dash);
        } else {
            int py = -1;
            for (int y = 0; y < im.h && py < 0; y++) for (int x = 0; x < im.w; x++) if (cov(&im, x, y) >= 128) { py = y; break; }
            n = runs(&im, (int)(3 * s), py, 1, 0, span, gaps, 64, dash);
        }
if ((int)((cases[c].vert ? lh : cw) * s) >= 2 * cases[c].per) CHECK_EQ(n, cases[c].per * cells);       /* a cell too small for the dashes shows fewer */        else CHECK(n >= (cases[c].per - 1) * cells && n <= cases[c].per * cells);
        int gmin = 1 << 30, gmax = 0, dmin = 1 << 30, dmax = 0;
        for (int i = 0; i + 1 < n && i < 63; i++) { if (gaps[i] < gmin) gmin = gaps[i]; if (gaps[i] > gmax) gmax = gaps[i]; }
        for (int i = 0; i < n && i < 64; i++) { if (dash[i] < dmin) dmin = dash[i]; if (dash[i] > dmax) dmax = dash[i]; }
        if (gmax != gmin || dmax - dmin > 2) printf("  dashes %s scale %.2f cell %.1f x %.0f: gaps %d..%d dashes %d..%d\n", cases[c].s, s, cw, lh, gmin, gmax, dmin, dmax);
        CHECK(gmax == gmin);
        CHECK(dmax - dmin <= 2);
        free_all(&im, &sh);
    }
}

static void test_braille(void) {
    for (int si = 0; si < 4; si++) for (int ci = 0; ci < 4; ci++) {
        /* every pattern: one separate dot per bit, none outside its cell */
        uint32_t cps[16 * 16];
        for (int i = 0; i < 256; i++) cps[i] = 0x2800 + (uint32_t)i;
        sheet sh = { cps, 16, 16, k_cw[ci], k_lh[ci], 3, 2 };
        image im;
        render(&sh, k_scales[si], &im);
        int bits = 0;
        for (int i = 0; i < 256; i++) for (int b = 0; b < 8; b++) bits += (i >> b) & 1;
        int n = components(&im, 40, false);
        if (n != bits) printf("  braille at scale %.2f, cell %.1f x %.0f: %d dots, expected %d\n", k_scales[si], k_cw[ci], k_lh[ci], n, bits);
        CHECK_EQ(n, bits);
        free(im.cov);
    }
    /* the full cell: 8 dots of one size, evenly placed */
    const char *rows[] = { "⣿⣿⣿" };
    image im; sheet sh;
    render_rows(rows, 1, 8, 16, 1, &im, &sh);
    CHECK_EQ(components(&im, 128, false), 24);
    free_all(&im, &sh);
    /* an 8 by 16 cell has dots of 3 pixels (a plus), at 1 or 5 across and 1, 5, 9 or 13 down; the texture they come from is exact:
     * the centre of every dot is fully lit, its corners and the pixels between the dots are not */
    {
        const char *one[] = { "⣿" };
        render_rows(one, 1, 8, 16, 1, &im, &sh);
        static const int dx[8] = { 1, 1, 1, 5, 5, 5, 1, 5 }, dy[8] = { 1, 5, 9, 1, 5, 9, 13, 13 };
        for (int i = 0; i < 8; i++) {
            CHECK_EQ(cov(&im, 3 + dx[i] + 1, 2 + dy[i] + 1), 255);
            CHECK_EQ(cov(&im, 3 + dx[i], 2 + dy[i]), 0);
            CHECK_EQ(cov(&im, 3 + dx[i] + 2, 2 + dy[i] + 2), 0);
            CHECK_EQ(cov(&im, 3 + dx[i] + 1, 2 + dy[i] + 3), 0);
        }
        long lit = 0;
        for (int i = 0; i < im.w * im.h; i++) lit += im.cov[i] == 255;
        CHECK_EQ(lit, 8 * 5);
        free_all(&im, &sh);
    }
}

static void test_sextants_and_wedges(void) {
    /* sextants: the six areas are lit as the code point's pattern says (cell 8 by 18) */
    for (uint32_t c = 0x1FB00; c <= 0x1FB3B; c++) {
        char b[5] = { (char)0xF0, (char)(0x80 | ((c >> 12) & 63)), (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
        const char *rows[] = { b };
        image im; sheet sh;
        render_rows(rows, 1, 8, 18, 1, &im, &sh);
        int m = 0;
        for (int r = 0; r < 3; r++) for (int k = 0; k < 2; k++) if (cov(&im, 3 + 2 + 4 * k, 2 + 3 + 6 * r) > 128) m |= 1 << (2 * r + k);
        CHECK_EQ(m, k_sextants[c - 0x1FB00]);
        free_all(&im, &sh);
    }
    /* wedges: the corner the name gives is lit, the opposite corner is not; every one draws something */
    for (uint32_t c = 0x1FB3C; c <= 0x1FB67; c++) {
        char b[5] = { (char)0xF0, (char)(0x80 | ((c >> 12) & 63)), (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
        const char *rows[] = { b };
        image im; sheet sh;
        render_rows(rows, 1, 12, 24, 1, &im, &sh);
        long sum = 0;
        for (int i = 0; i < im.w * im.h; i++) sum += im.cov[i];
        CHECK(sum > 255L * 12 * 24 / 16 && sum < 255L * 12 * 24 * 15 / 16);        /* the smallest wedge is a twelfth of the cell */
        free_all(&im, &sh);
    }
    /* the three quarters have the area of three quarters, the quarters one quarter */
    for (uint32_t c = 0x1FB68; c <= 0x1FB6F; c++) {
        char b[5] = { (char)0xF0, (char)(0x80 | ((c >> 12) & 63)), (char)(0x80 | ((c >> 6) & 63)), (char)(0x80 | (c & 63)), 0 };
        const char *rows[] = { b };
        image im; sheet sh;
        render_rows(rows, 1, 12, 24, 1, &im, &sh);
        long sum = 0;
        for (int i = 0; i < im.w * im.h; i++) sum += im.cov[i];
        double frac = (double)sum / (255.0 * 12 * 24), want = c < 0x1FB6C ? 0.75 : 0.25;
        CHECK(frac > want - 0.03 && frac < want + 0.03);
        free_all(&im, &sh);
    }
}

static void test_status_line(void) {
    /* a coloured cell then the triangle: no seam; the triangle's base fills the cell's edge from top to bottom */
    const char *rows[] = { "█\xEE\x82\xB0█\xEE\x82\xB2" };
    for (int si = 0; si < 4; si++) {
        image im; sheet sh;
        render_rows(rows, 1, 8, 17, k_scales[si], &im, &sh);
        float s = k_scales[si];
        int x_tri = (int)floorf((3 + 8) * s + 0.5f), x_tip = (int)floorf((3 + 16) * s + 0.5f) - 1;
        int y0 = (int)floorf(2 * s + 0.5f), y1 = (int)floorf((2 + 17) * s + 0.5f) - 1;
        for (int y = y0; y <= y1; y++) CHECK(cov(&im, x_tri - 1, y) == 255);
        for (int y = y0 + 4; y <= y1 - 4; y++) CHECK(cov(&im, x_tri, y) >= 250);      /* the base fills the edge (the corners are cut by the slopes) */
        CHECK(cov(&im, x_tip, (y0 + y1) / 2) >= 100);
        CHECK(cov(&im, x_tip, y0) < 60);
        free_all(&im, &sh);
    }
}

int main(void) {
    g_sw = cui_sw_create();
    test_tables();
    test_solid_blocks();
    test_blocks();
    test_frames();
    test_lines();
    test_dashes();
    test_braille();
    test_sextants_and_wedges();
    test_status_line();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
