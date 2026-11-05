/* Opens a real window with the native backend, runs the gallery for a few
 * frames, injects input, saves a screenshot and exits.
 *   cui_app_smoke [out.png] */
#include <cui/cui_app.h>
#include <cui/cui_render.h>
#include "../examples/gallery_ui.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "app_smoke.png";
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "CUI smoke test", .width = 1000, .height = 700, .continuous = true });
    if (!app) { fprintf(stderr, "could not create app\n"); return 1; }
    printf("backend=%s renderer=%s\n", cui_app_backend(app), cui_app_is_software(app) ? "software" : "opengl");
    static gallery g;
    gallery_init(&g);
    g.page = PAGE_SELECTION;
    int frame = 0;
    bool switched = false;
    while (cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        /* click the Bluetooth switch after the layout settled */
        if (frame == 20) { cui_input_mouse_move(ui, 700, 198); }
        if (frame == 22) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        if (frame == 24) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        bool before = g.bluetooth;
        gallery_frame(ui, &g);
        if (g.bluetooth != before) switched = true;
        if (++frame >= 80) break;
    }
    cui_vec2 sz = cui_app_size(app);
    const cui_draw_data *dd = cui_get_draw_data(cui_app_ui(app));
    int w = dd->width_px, h = dd->height_px;
    unsigned char *px = malloc((size_t)w * h * 4);
    bool ok = cui_app_screenshot(app, px, w, h) && cui_write_png(out, px, w, h, w * 4);
    printf("size=%.0fx%.0f screenshot=%s switch_clicked=%s\n", sz.x, sz.y, ok ? out : "failed", switched ? "yes" : "no");
    free(px);
    cui_app_destroy(app);
    return (ok && switched) ? 0 : 1;
}
