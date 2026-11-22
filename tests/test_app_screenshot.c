/* [Documentation](../docs/getting-started.md#8-screenshots-and-tests) */
#include <cui/cui_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Checks that every pixel in the screenshot contains the drawn color. */
static bool is_solid_rgba(const unsigned char *pixels, size_t len, cui_color color) {
    for (size_t i = 0; i < len; i += 4) {
        if (pixels[i] != color.r || pixels[i + 1] != color.g ||
            pixels[i + 2] != color.b || pixels[i + 3] != color.a) return false;
    }
    return true;
}

/** Captures the same static software frame twice into one cleared buffer. */
int main(void) {
#if !defined(_WIN32) && !defined(__APPLE__)
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        puts("skip: screenshot test needs a display");
        return 77;
    }
#endif
    cui_app *app = cui_app_create(&(cui_app_desc){
        .title = "CUI screenshot test", .width = 128, .height = 96, .hidden = true,
        .renderer = CUI_RENDERER_SOFTWARE
    });
    if (!app) return 1;
    if (!cui_app_is_software(app) || !cui_app_frame(app)) {
        fprintf(stderr, "could not start a software frame\n");
        cui_app_destroy(app);
        return 1;
    }

    cui_ctx *ui = cui_app_ui(app);
    cui_vec2 size = cui_viewport_size(ui);
    cui_color color = { 255, 0, 255, 255 };
    cui_draw_rect(ui, (cui_rect){ -2, -2, size.x + 4, size.y + 4 }, (cui_corners){ 0 }, color);
    cui_app_quit(app);
    cui_app_frame(app);

    const cui_draw_data *dd = cui_get_draw_data(ui);
    int w = dd->width_px, h = dd->height_px;
    size_t len = w > 0 && h > 0 ? (size_t)w * h * 4 : 0;
    unsigned char *pixels = len ? calloc(len, 1) : NULL;
    bool first = pixels && cui_app_screenshot(app, pixels, w, h) && is_solid_rgba(pixels, len, color);
    bool second = false;
    if (first) {
        memset(pixels, 0, len);
        second = cui_app_screenshot(app, pixels, w, h) && is_solid_rgba(pixels, len, color);
    }
    printf("%s software screenshot (%dx%d): first capture %s, capture after clearing buffer %s\n",
           cui_app_backend(app), w, h, first ? "ok" : "FAILED", second ? "ok" : "FAILED");
    free(pixels);
    cui_app_destroy(app);
    return first && second ? 0 : 1;
}
