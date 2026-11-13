/* Tests for the ordered dither primitive and the components drawn with it. Runs headless with
 * the software renderer. The primitive is checked cell by cell against the 8x8 Bayer matrix;
 * the components through what they return and edit, the keyboard, and their pixels. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static cui_sw_renderer *g_sw;
static float g_dt = 1.0f / 60.0f;
static bool g_rtl;

static cui_ctx *new_ctx(float w, float h, bool reduced) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK, .accent = CUI_ACCENT_NEUTRAL });
    cui_set_viewport(ui, w, h, 1);
    cui_set_reduced_motion(ui, reduced);
    cui_input_mouse_leave(ui);
    return ui;
}

typedef void (*build_fn)(cui_ctx *ui, void *user);
static void run(cui_ctx *ui, build_fn fn, void *user, int frames) {
    for (int i = 0; i < frames; i++) {
        cui_begin_frame(ui, g_dt);
        cui_column(ui, .pad = 20, .gap = 12, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND,
                   .direction = g_rtl ? CUI_DIRECTION_RTL : CUI_DIRECTION_LTR) fn(ui, user);
        cui_end_frame(ui);
    }
}
static void move(cui_ctx *ui, build_fn fn, void *u, float x, float y) { cui_input_mouse_move(ui, x, y); run(ui, fn, u, 1); }
static void click(cui_ctx *ui, build_fn fn, void *u, float x, float y) {
    move(ui, fn, u, x, y);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); run(ui, fn, u, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); run(ui, fn, u, 1);
}
static void key(cui_ctx *ui, build_fn fn, void *u, cui_key k) {
    cui_input_key(ui, k, true, 0); cui_input_key(ui, k, false, 0);
    run(ui, fn, u, 1);
}

/* the last frame, rendered at scale 1 */
typedef struct { unsigned char *px; int w, h; } image;
static image render(cui_ctx *ui) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    image im = { malloc((size_t)dd->width_px * dd->height_px * 4), dd->width_px, dd->height_px };
    cui_sw_render(g_sw, dd, im.px, im.w, im.h, im.w * 4, CUI_SW_RGBA);
    return im;
}
static const unsigned char *at(image im, float x, float y) { return im.px + ((size_t)(int)y * im.w + (size_t)(int)x) * 4; }
static bool same(image a, image b) { return a.w == b.w && a.h == b.h && memcmp(a.px, b.px, (size_t)a.w * a.h * 4) == 0; }

/* ------------------------------------------------------------------------ */
/* the primitive                                                            */

static int bayer(int x, int y) {
    static const int m[64] = { 0, 32, 8, 40, 2, 34, 10, 42, 48, 16, 56, 24, 50, 18, 58, 26, 12, 44, 4, 36, 14, 46, 6, 38,
                               60, 28, 52, 20, 62, 30, 54, 22, 3, 35, 11, 43, 1, 33, 9, 41, 51, 19, 59, 27, 49, 17, 57, 25,
                               15, 47, 7, 39, 13, 45, 5, 37, 63, 31, 55, 23, 61, 29, 53, 21 };
    return m[((y & 7) << 3) | (x & 7)];
}

/* five 64 point squares at 2 point cells: densities, a floor, a phase and a rounded cut */
typedef struct { float density, floor, phase; float radius; } prim_case;
static const prim_case g_prim[5] = { { 0.5f, 0, 0, 0 }, { 0.25f, 0, 0, 0 }, { 1, 0.5f, 0, 0 }, { 0.5f, 0, 3, 0 }, { 1, 0, 0, 16 } };
static cui_rect g_canvas;
static void paint_prim(cui_ctx *ui, cui_rect r, void *u) {
    (void)u;
    g_canvas = r;
    cui_draw_rect(ui, r, CUI_CORNERS(0), cui__rgba(0, 0, 0, 255));
    for (int k = 0; k < 5; k++) {
        cui_rect sq = { r.x + 80 * (float)k, r.y, 64, 64 };
        cui__dither d;
        memset(&d, 0, sizeof d);
        d.color = cui__rgba(255, 255, 255, 255);
        d.origin = (cui_vec2){ sq.x, sq.y };
        d.floor = g_prim[k].floor;
        d.phase.x = g_prim[k].phase;
        d.shape = sq;
        d.radius = CUI_CORNERS(g_prim[k].radius);
        cui__draw_dither_rect(ui, &d, sq, g_prim[k].density, g_prim[k].density, false);
    }
    /* a gradient, 1 at the left to 0 at the right */
    cui__dither d;
    memset(&d, 0, sizeof d);
    d.color = cui__rgba(255, 255, 255, 255);
    d.origin = (cui_vec2){ r.x, r.y + 80 };
    cui__draw_dither_rect(ui, &d, (cui_rect){ r.x, r.y + 80, 256, 16 }, 1, 0, false);
}
static void build_prim(cui_ctx *ui, void *u) { (void)u; cui_canvas(ui, "prim", CUI_PX(420), CUI_PX(100), paint_prim, NULL, 0); }

static void test_primitive(void) {
    printf("primitive\n");
    cui_ctx *ui = new_ctx(480, 160, true);
    run(ui, build_prim, NULL, 2);
    image im = render(ui);
    int lit[5] = { 0 }, wrong[5] = { 0 }, subset_bad = 0;
    for (int k = 0; k < 4; k++)
        for (int cy = 0; cy < 32; cy++)
            for (int cx = 0; cx < 32; cx++) {
                /* the center pixel of the cell */
                bool on = at(im, g_canvas.x + 80 * k + cx * 2 + 1, g_canvas.y + cy * 2 + 1)[0] > 128;
                int sx = cx + (int)g_prim[k].phase;
                float t = (bayer(sx, cy) + 0.5f) / 64.0f;
                bool want = t >= g_prim[k].floor && t < g_prim[k].density;
                lit[k] += on;
                wrong[k] += on != want;
                /* lower densities light a subset of the cells of higher ones */
                if (k == 1 && on && !(at(im, g_canvas.x + cx * 2 + 1, g_canvas.y + cy * 2 + 1)[0] > 128)) subset_bad++;
            }
    for (int k = 0; k < 4; k++) CHECK_EQ(wrong[k], 0);
    CHECK_EQ(lit[0], 512);   /* half of 1024 cells */
    CHECK_EQ(lit[1], 256);
    CHECK_EQ(lit[2], 512);   /* a floor of 0.5 turns off the lower half */
    CHECK_EQ(subset_bad, 0);
    /* every pixel of a cell agrees, so a dot is a 2 by 2 square */
    int split = 0;
    for (int cy = 0; cy < 32; cy++)
        for (int cx = 0; cx < 32; cx++) {
            unsigned char v = at(im, g_canvas.x + cx * 2, g_canvas.y + cy * 2)[0];
            if (at(im, g_canvas.x + cx * 2 + 1, g_canvas.y + cy * 2 + 1)[0] != v || at(im, g_canvas.x + cx * 2 + 1, g_canvas.y + cy * 2)[0] != v) split++;
        }
    CHECK_EQ(split, 0);
    /* the rounded cut: the corner is empty, the middle full */
    CHECK(at(im, g_canvas.x + 320 + 1, g_canvas.y + 1)[0] < 30);
    CHECK(at(im, g_canvas.x + 320 + 32, g_canvas.y + 32)[0] > 200);
    /* the gradient: dense at the left, empty at the right, thinning in between */
    int left = 0, mid = 0, right = 0;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 32; x++) {
            left += at(im, g_canvas.x + x, g_canvas.y + 80 + y)[0] > 128;
            mid += at(im, g_canvas.x + 112 + x, g_canvas.y + 80 + y)[0] > 128;
            right += at(im, g_canvas.x + 224 + x, g_canvas.y + 80 + y)[0] > 128;
        }
    CHECK(left > mid && mid > right);
    CHECK(left > 420);
    CHECK(right < 60);
    free(im.px);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_primitive();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
