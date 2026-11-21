/* cui_app_desc.present_exposed_part (Windows, software renderer): a part of the window the system asks to repaint
 * (WM_PAINT for a rectangle, as when a child window's region shrinks) is copied with the next frame's change, not
 * the whole picture; a whole-window repaint still copies all of it; off, any repaint copies all of it, as before.
 * The window is shown (hidden windows get no WM_PAINT). Other platforms: a skip line, and it passes.
 *   cui_test_present_part */
#include <cui/cui_app.h>
#include "../src/platform/cui_platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int failures;

static void expect(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "fail", what);
    if (!ok) failures++;
}

static void draw(cui_app *app) {
    cui_ctx *ui = cui_app_ui(app);
    cui_column(ui, .pad = 16) { cui_text(ui, "present part test"); }
}

static void pump(cui_app *app, int n) {
    for (int i = 0; i < n; i++) {
        if (!cui_app_frame_poll(app)) return;
        draw(app);
        Sleep(4);
    }
}

/* the presents of the frames after the system asks to repaint r (NULL: all of the window) */
static void after_repaint(cui_app *app, const RECT *r, int *full, int *part) {
    HWND hwnd = (HWND)cui_app_native_window(app);
    pump(app, 2);
    app->presents_full = app->presents_part = 0;
    InvalidateRect(hwnd, r, FALSE);
    pump(app, 4);
    *full = app->presents_full;
    *part = app->presents_part;
}

int main(void) {
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "cui present part test", .width = 480, .height = 320,
                                                   .renderer = CUI_RENDERER_SOFTWARE, .present_exposed_part = true });
    if (!app) { printf("skip: no window\n"); return 0; }
    if (!app->sw) { printf("skip: no software renderer\n"); cui_app_destroy(app); return 0; }
    pump(app, 6);
    int full, part;
    RECT r = { 20, 30, 140, 90 };
    after_repaint(app, &r, &full, &part);
    printf("  a part to repaint: %d whole, %d part presents\n", full, part);
    expect(full == 0 && part >= 1, "a part to repaint is copied as a part");
    after_repaint(app, NULL, &full, &part);
    printf("  the whole window to repaint: %d whole, %d part presents\n", full, part);
    expect(full >= 1, "the whole window to repaint is copied whole");
    app->desc.present_exposed_part = false;
    after_repaint(app, &r, &full, &part);
    printf("  off, a part to repaint: %d whole, %d part presents\n", full, part);
    expect(full >= 1, "off: a part to repaint copies the whole picture, as before");
    cui_app_destroy(app);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
#else
int main(void) {
    printf("skip: Windows only\n");
    return 0;
}
#endif
