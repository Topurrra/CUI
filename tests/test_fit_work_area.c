/* cui_app_desc.fit_work_area (Windows): a window asked for at a size larger than the screen opens smaller, inside the
 * work area and never below its minimum; without the option it opens at the size asked for. Opens hidden windows. */
#include <cui/cui_app.h>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int g_failed;
#define CHECK(c) do { if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static float g_scale = 1;

static cui_vec2 open_size(bool fit, int w, int h, int min_w, int min_h, RECT *outer) {
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "CUI fit test", .width = w, .height = h,
                                                   .min_width = min_w, .min_height = min_h, .hidden = true, .fit_work_area = fit,
                                                   .continuous = true });   /* frames come without input */
    cui_vec2 s = { -1, -1 };
    if (!app) return s;
    for (int i = 0; i < 3 && cui_app_frame(app); i++) cui_text(cui_app_ui(app), "fit");
    s = cui_app_size(app);
    g_scale = cui_dpi_scale(cui_app_ui(app));
    GetWindowRect((HWND)cui_app_native_window(app), outer);
    cui_app_destroy(app);
    return s;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    RECT outer;
    /* far too large: made to fit the work area (the first window also makes the process DPI aware) */
    cui_vec2 s = open_size(true, 9000, 9000, 300, 200, &outer);
    if (s.x < 0) { printf("skip: no window\n"); return 0; }
    MONITORINFO mi;
    memset(&mi, 0, sizeof mi);
    mi.cbSize = sizeof mi;
    POINT origin = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &mi);
    RECT work = mi.rcWork;
    float ww = (float)(work.right - work.left) / g_scale, wh = (float)(work.bottom - work.top) / g_scale;
    printf("work area %.0f x %.0f points at %.2fx\n", ww, wh, g_scale);
    printf("fit: %.0f x %.0f, window %ld,%ld - %ld,%ld\n", s.x, s.y, outer.left, outer.top, outer.right, outer.bottom);
    CHECK(s.x <= ww + 0.5f && s.y <= wh + 0.5f);
    CHECK(s.x >= ww * 0.8f && s.y >= wh * 0.7f);       /* not shrunk more than the frame needs */
    CHECK(outer.left >= work.left - 16 && outer.top >= work.top - 1);   /* placed inside (the frame's invisible edge aside) */
    CHECK(outer.right <= work.right + 16 && outer.bottom <= work.bottom + 16);

    /* a size that fits is used as asked */
    s = open_size(true, 400, 300, 300, 200, &outer);
    CHECK(s.x == 400 && s.y == 300);

    /* content that fits the work area but not with its title bar: made shorter by about the frame */
    int h = (int)wh - 10;
    s = open_size(true, 400, h, 300, 200, &outer);
    printf("fit %d tall: %.0f\n", h, s.y);
    CHECK(s.y < h && s.y >= h - 80);

    /* the minimum wins over the work area */
    s = open_size(true, 400, h, 300, h, &outer);
    CHECK(s.y >= h);

    /* without the option: as before, the size asked for (Windows itself only keeps a window within the screen) */
    s = open_size(false, 400, h, 300, 200, &outer);
    CHECK(s.y == h);

    printf("%s\n", g_failed ? "FAILED" : "ok");
    return g_failed ? 1 : 0;
}
#else
int main(void) { printf("skip: Windows only\n"); return 0; }
#endif
