/* Window state and placement: opens at a saved place, moves, maximizes, minimizes and restores
 * the window, and reads its state and placement back after each step.
 *   cui_test_window_state
 * Needs a display; without one it skips. A step the window manager does not carry out within
 * two seconds fails on Windows and macOS and is reported as skipped elsewhere (an X11 window
 * manager without EWMH, or one that cannot minimize). */
#include <cui/cui_app.h>

#include <stdio.h>
#include <stdlib.h>

static int failures;
static bool strict;   /* the system always carries out state changes */

static const char *state_name(cui_window_state s) {
    return s == CUI_WINDOW_MAXIMIZED ? "maximized" : s == CUI_WINDOW_MINIMIZED ? "minimized" : "normal";
}

/* Draws one frame of the test window, without spinning while the window is minimized. */
static bool step(cui_app *app) {
    if (!cui_app_frame(app)) return false;
    cui_ctx *ui = cui_app_ui(app);
    cui_column(ui, .pad = 16) {
        cui_text(ui, "window state test");
    }
    cui_request_wakeup(ui, 0.02f);
    return true;
}

/* Runs frames for the given time. */
static void settle(cui_app *app, double seconds) {
    double until = cui_time(cui_app_ui(app)) + seconds;
    while (cui_time(cui_app_ui(app)) < until && step(app)) {}
}

/* Runs frames until the window is in the state, then lets its geometry settle. */
static bool reach(cui_app *app, cui_window_state want, const char *what) {
    double until = cui_time(cui_app_ui(app)) + 2.0;
    while (cui_app_window_state(app) != want && cui_time(cui_app_ui(app)) < until && step(app)) {}
    bool ok = cui_app_window_state(app) == want;
    if (ok) settle(app, 0.3);
    if (ok) printf("ok: %s: %s\n", what, state_name(want));
    else if (strict) { printf("fail: %s: state is %s, not %s\n", what, state_name(cui_app_window_state(app)), state_name(want)); failures++; }
    else printf("skip: %s: the window manager left the window %s\n", what, state_name(cui_app_window_state(app)));
    return ok;
}

static void expect(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "fail", what);
    if (!ok) failures++;
}

static bool close_to(float a, float b) { return a - b <= 1.0f && b - a <= 1.0f; }

/* Checks the saved normal size, the place where the system reports one, and the state the
 * window opens in again: maximized also for a window minimized from maximized. */
static void expect_placement(cui_app *app, int x, int y, int w, int h, cui_window_state state, const char *what) {
    cui_window_placement p;
    expect(cui_app_placement(app, &p), "cui_app_placement");
    printf("  placement %s: %d,%d %dx%d %s%s\n", what, p.x, p.y, p.width, p.height, state_name(p.state),
           p.has_position ? "" : " (no position)");
    char line[160];
    snprintf(line, sizeof line, "%s: saved state %s", what, state_name(state));
    expect(p.state == state, line);
    snprintf(line, sizeof line, "%s: normal size %dx%d", what, w, h);
    expect(close_to((float)p.width, (float)w) && close_to((float)p.height, (float)h), line);
    if (p.has_position) {
        snprintf(line, sizeof line, "%s: normal place %d,%d", what, x, y);
        expect(close_to((float)p.x, (float)x) && close_to((float)p.y, (float)y), line);
    }
}

static void expect_size(cui_app *app, int w, int h, const char *what) {
    cui_vec2 s = cui_app_size(app);
    char line[160];
    snprintf(line, sizeof line, "%s: content %.0fx%.0f, expected %dx%d", what, s.x, s.y, w, h);
    expect(close_to(s.x, (float)w) && close_to(s.y, (float)h), line);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const int x = 120, y = 140, w = 520, h = 380;
    cui_window_placement at = { .x = x, .y = y, .width = w, .height = h, .has_position = true };
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "CUI window state test", .placement = &at });
    if (!app) {
        fprintf(stderr, "skip: could not create app (no display?)\n");
        return 0;
    }
    const char *backend = cui_app_backend(app);
#if defined(_WIN32) || defined(__APPLE__)
    strict = true;   /* the system carries out state changes with every backend */
#endif
    printf("backend=%s strict=%d\n", backend, strict);
    /* before the window manager has placed the window, the placement is still the saved one */
    expect_placement(app, x, y, w, h, CUI_WINDOW_NORMAL, "right after opening");

    settle(app, 0.5);
    if (reach(app, CUI_WINDOW_NORMAL, "open at a saved place")) {
        expect_size(app, w, h, "open at a saved place");
        expect_placement(app, x, y, w, h, CUI_WINDOW_NORMAL, "open at a saved place");
    }

    const int x2 = 200, y2 = 180;
    cui_app_set_position(app, x2, y2);
    settle(app, 0.5);
    expect_placement(app, x2, y2, w, h, CUI_WINDOW_NORMAL, "set_position");

    cui_app_maximize(app);
    if (reach(app, CUI_WINDOW_MAXIMIZED, "maximize")) {
        cui_vec2 s = cui_app_size(app);
        bool grew = s.x > (float)w + 1 || s.y > (float)h + 1;
        /* SDL on X11 reports a maximize as soon as it asks for one, also when no window manager
         * runs to carry it out (Xvfb without one) */
        if (grew || strict) expect(grew, "maximize: the content grew");
        else printf("skip: maximize: reported, but the content did not grow (no window manager?)\n");
        expect_placement(app, x2, y2, w, h, CUI_WINDOW_MAXIMIZED, "maximized");
        cui_app_restore(app);
        if (reach(app, CUI_WINDOW_NORMAL, "restore from maximized")) {
            expect_size(app, w, h, "restore from maximized");
            expect_placement(app, x2, y2, w, h, CUI_WINDOW_NORMAL, "restore from maximized");
        }
    }

    cui_app_minimize(app);
    if (reach(app, CUI_WINDOW_MINIMIZED, "minimize")) {
        expect_placement(app, x2, y2, w, h, CUI_WINDOW_NORMAL, "minimized");
        cui_app_restore(app);
        if (reach(app, CUI_WINDOW_NORMAL, "restore from minimized")) {
            expect_size(app, w, h, "restore from minimized");
            expect_placement(app, x2, y2, w, h, CUI_WINDOW_NORMAL, "restore from minimized");
        }
    }

    /* restoring a window that was minimized from maximized brings it back maximized */
    cui_app_maximize(app);
    if (reach(app, CUI_WINDOW_MAXIMIZED, "maximize again")) {
        cui_app_minimize(app);
        if (reach(app, CUI_WINDOW_MINIMIZED, "minimize a maximized window")) {
            expect_placement(app, x2, y2, w, h, CUI_WINDOW_MAXIMIZED, "minimized from maximized");
            cui_app_restore(app);
            reach(app, CUI_WINDOW_MAXIMIZED, "restore to maximized");
        }
        cui_app_restore(app);
        reach(app, CUI_WINDOW_NORMAL, "restore to normal");
    }
    cui_app_destroy(app);

    /* a window saved maximized opens maximized and restores to its saved size, also when the app
     * maximizes it as well right after cui_app_create, as an app that opens maximized does */
    at.state = CUI_WINDOW_MAXIMIZED;
    app = cui_app_create(&(cui_app_desc){ .title = "CUI window state test", .placement = &at });
    expect(app != NULL, "open maximized: window created");
    if (app) {
        cui_app_maximize(app);
        if (reach(app, CUI_WINDOW_MAXIMIZED, "open maximized")) {
            expect_placement(app, x, y, w, h, CUI_WINDOW_MAXIMIZED, "open maximized");
            cui_app_restore(app);
            if (reach(app, CUI_WINDOW_NORMAL, "restore after opening maximized")) {
                expect_size(app, w, h, "restore after opening maximized");
                expect_placement(app, x, y, w, h, CUI_WINDOW_NORMAL, "restore after opening maximized");
            }
        }
        cui_app_destroy(app);
    }

#ifdef _WIN32
    /* a custom title bar has no system frame: desc.x/.y/.has_pos, cui_app_position,
     * cui_app_set_position and cui_app_placement all name the same corner */
    app = cui_app_create(&(cui_app_desc){ .title = "CUI window state test", .custom_titlebar = true,
                                          .width = w, .height = h, .x = 150, .y = 160, .has_pos = true });
    expect(app != NULL, "custom title bar: window created");
    if (app) {
        int px = -1, py = -1;
        if (reach(app, CUI_WINDOW_NORMAL, "custom title bar: open at x, y")) {
            expect(cui_app_position(app, &px, &py) && px == 150 && py == 160, "custom title bar: cui_app_position 150,160");
            expect_size(app, w, h, "custom title bar");
            expect_placement(app, 150, 160, w, h, CUI_WINDOW_NORMAL, "custom title bar");
        }
        cui_app_set_position(app, 230, 210);
        settle(app, 0.3);
        expect(cui_app_position(app, &px, &py) && px == 230 && py == 210, "custom title bar: set_position then position 230,210");
        expect_placement(app, 230, 210, w, h, CUI_WINDOW_NORMAL, "custom title bar moved");
        cui_app_toggle_maximize(app);
        if (reach(app, CUI_WINDOW_MAXIMIZED, "custom title bar: toggle to maximized")) {
            expect(cui_app_is_maximized(app), "custom title bar: cui_app_is_maximized");
            expect_placement(app, 230, 210, w, h, CUI_WINDOW_MAXIMIZED, "custom title bar maximized");
            cui_app_toggle_maximize(app);
            if (reach(app, CUI_WINDOW_NORMAL, "custom title bar: toggle back"))
                expect_size(app, w, h, "custom title bar restored");
        }
        cui_app_destroy(app);
    }
#endif

    printf("%s: %d failed\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
