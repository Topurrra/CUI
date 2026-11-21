/* test_floating_layers.c - cui_floating_layers for a dialog: the full layer (the scrim) reports its panel as painted
 * (inside the window, centred, growing from CUI__ENTER_SCALE while it fades in, the theme's surface radius when at
 * rest), its opacity and elevation, and the scrim's color with its alpha following the fade; with nothing open there
 * is no layer. Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static bool g_open;

static void frame(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .pad = 20) {
        cui_text(ui, "page");
        cui_dialog(ui, "About", &g_open, .width = 300) { cui_text(ui, "body"); }
    }
    cui_end_frame(ui);
}

static void frame_pop(cui_ctx *ui, bool open) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .pad = 20) {
        cui_button(ui, "Menu");
        if (open) cui_popover_open(ui, "pop");
        if (cui_popover_begin(ui, "pop", NULL)) { cui_text(ui, "A popover"); cui_popover_end(ui); }
    }
    cui_end_frame(ui);
}

static int full_layer(cui_ctx *ui, cui_layer_rect *out) {
    cui_layer_rect l[16];
    int n = cui_floating_layers(ui, l, 16);
    for (int i = 0; i < n && i < 16; i++)
        if (l[i].full) { *out = l[i]; return 1; }
    return 0;
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, 800, 600, 1);
    for (int i = 0; i < 3; i++) frame(ui);
    cui_layer_rect l;
    CHECK(cui_floating_layers(ui, NULL, 0) == 0);

    g_open = true;
    frame(ui);
    frame(ui);
    CHECK(full_layer(ui, &l));
    float a0 = l.panel_opacity, s0 = l.panel.w;
    unsigned scrim0 = l.scrim.a;
    CHECK(a0 > 0 && a0 < 0.9f);
    CHECK(l.panel.w > 0 && l.panel.w < 300);           /* still growing */
    for (int i = 0; i < 90; i++) frame(ui);
    CHECK(full_layer(ui, &l));
    cui_color want = cui_color_of(ui, CUI_COLOR_SCRIM);
    CHECK(l.scrim.r == want.r && l.scrim.g == want.g && l.scrim.b == want.b);
    CHECK(abs((int)l.scrim.a - (int)want.a) <= 1);
    CHECK(scrim0 < l.scrim.a);
    CHECK(l.panel_opacity > 0.99f);
    CHECK(fabsf(l.panel.w - 300) < 0.5f && l.panel.w > s0);
    CHECK(fabsf(l.panel.x + l.panel.w * 0.5f - 400) < 1.0f);    /* centred */
    CHECK(fabsf(l.panel.y + l.panel.h * 0.5f - 300) < 1.0f);
    CHECK(fabsf(l.panel_radius.tl - cui_theme_get(ui)->radius_surface) < 0.01f);
    CHECK(l.panel_elevation == CUI_ELEVATION_FLOATING);
    CHECK(l.rect.w == 800 && l.rect.h == 600);
    /* how the panel is painted (cui_overlay.c): the raised surface, a 1 pt border (black at 6% in the light theme) */
    {
        cui_color sr = cui_color_of(ui, CUI_COLOR_SURFACE_RAISED);
        CHECK(l.fill.r == sr.r && l.fill.g == sr.g && l.fill.b == sr.b && l.fill.a == sr.a);
        CHECK(l.border_w == 1.0f && l.border.r == 0 && l.border.a >= 14 && l.border.a <= 18);
        CHECK(l.opacity > 0.99f);
    }

    /* closing: the scrim fades out, then the layer is gone */
    g_open = false;
    frame(ui);
    frame(ui);
    CHECK(full_layer(ui, &l) && l.scrim.a < want.a);
    for (int i = 0; i < 90; i++) frame(ui);
    CHECK(!full_layer(ui, &l));

    /* a popover reports its own box: fill, border, radius and opacity (the panel style every popover and menu has) */
    for (int i = 0; i < 90; i++) frame_pop(ui, i == 1);
    {
        cui_layer_rect pl[16];
        int n = cui_floating_layers(ui, pl, 16), found = -1;
        for (int i = 0; i < n && i < 16; i++) if (!pl[i].full) found = i;
        CHECK(found >= 0);
        if (found >= 0) {
            cui_color sr = cui_color_of(ui, CUI_COLOR_SURFACE_RAISED);
            CHECK(pl[found].fill.r == sr.r && pl[found].fill.g == sr.g && pl[found].fill.b == sr.b && pl[found].fill.a == sr.a);
            CHECK(pl[found].border_w == 1.0f && pl[found].border.a >= 16 && pl[found].border.a <= 20);   /* black at 7% */
            CHECK(pl[found].opacity > 0.99f);
            CHECK(fabsf(pl[found].radius.tl - cui_theme_get(ui)->radius_panel) < 0.01f);
        }
    }

    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("test_floating_layers: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
