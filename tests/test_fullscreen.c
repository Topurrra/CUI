/* The Windows opt-ins that save work at rest: the caret that follows the system's timeout, frames only on change
 * (cui_app_frames_on_change, cui_app_arm, cui_app_frame_due, cui_app_schedule) and borderless fullscreen with the edge
 * strip peek and the one-off hint. The window is hidden. Other platforms have no such calls to run: the test prints a
 * skip line and passes.
 *   cui_test_fullscreen */
#include <cui/cui_app.h>

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
    cui_column(ui, .pad = 16) { cui_text(ui, "fullscreen test"); }
}

/* Takes in the system's messages and runs n frames. */
static void pump(cui_app *app, int n) {
    for (int i = 0; i < n; i++) {
        if (!cui_app_frame_poll(app)) return;
        draw(app);
        Sleep(4);
    }
}

static int ticks;
static void tick(void *user) { (void)user; ticks++; }

/* A focused text field and frames a second apart (driven here, not by the clock) for `seconds`: the wakeup CUI asks for
 * afterwards (< 0: none). */
static float caret_wakeup_after(cui_app *app, int seconds) {
    cui_ctx *ui = cui_app_ui(app);
    static char buf[32];
    for (int i = 0; i <= seconds; i++) {
        cui_begin_frame(ui, 1.0f);
        cui_column(ui, .pad = 16) { cui_input(ui, "field", buf, sizeof buf, .autofocus = true); }
        cui_end_frame(ui);
    }
    return cui_next_wakeup(ui);
}

int main(void) {
    float sys = cui_system_caret_blink();
    expect(sys != 0.0f, "cui_system_caret_blink is a time or -1, never 0");
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "cui fullscreen test", .width = 480, .height = 320, .hidden = true,
                                                   .renderer = CUI_RENDERER_SOFTWARE, .follow_system_caret = true });
    if (!app) { printf("skip: no window\n"); return 0; }
    HWND hwnd = (HWND)cui_app_native_window(app);
    pump(app, 4);

    /* ---- frames only on change ---- */
    expect(cui_app_frame_due(app), "without frames_on_change a frame is always due");
    expect(cui_app_frames_on_change(app, true), "frames_on_change(true)");
    for (int i = 0; i < 10 && cui_app_pending(app); i++) pump(app, 1);
    expect(!cui_app_pending(app), "the app settles: nothing pending");
    cui_app_arm(app);
    SendMessageW(hwnd, WM_GETICON, 0, 0);   /* a question that changes nothing */
    expect(!cui_app_frame_due(app), "a question that changes nothing brings no frame");
    expect(cui_app_frames_skipped(app) == 1, "and counts as skipped");
    cui_app_arm(app);
    SendMessageW(hwnd, WM_USER + 5, 0, 0);   /* a message of the app's own */
    expect(cui_app_frame_due(app), "a message for the window brings a frame");
    cui_app_arm(app);
    cui_app_request_frame(app);
    expect(cui_app_frame_due(app), "cui_app_request_frame brings a frame");
    cui_app_arm(app);
    cui_request_redraw(cui_app_ui(app));
    expect(cui_app_frame_due(app), "CUI's own redraw request brings a frame");
    pump(app, 4);
    for (int i = 0; i < 10 && cui_app_pending(app); i++) pump(app, 1);
    cui_app_arm(app);
    expect(cui_app_schedule(app, 20, tick, NULL), "cui_app_schedule");
    cui_app_wait(&app, 1);   /* the timer runs the tick between frames, then nothing is due */
    expect(ticks == 1, "the scheduled tick ran while waiting");
    expect(!cui_app_frame_due(app), "and drew no frame");
    expect(cui_app_schedule(app, -1, NULL, NULL), "cui_app_schedule(-1) clears it");
    expect(cui_app_frames_on_change(app, false), "frames_on_change(false)");
    cui_app_arm(app);
    SendMessageW(hwnd, WM_GETICON, 0, 0);
    expect(cui_app_frame_due(app), "off again: every frame is due");

    /* ---- fullscreen ---- */
    RECT before, mr;
    GetWindowRect(hwnd, &before);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    mr = mi.rcMonitor;
    expect(!cui_app_is_fullscreen(app) && cui_app_fullscreen_strip(app) == 0, "not fullscreen: no strip");
    expect(!cui_app_fullscreen_hint(app, NULL), "no hint before");
    expect(cui_app_set_fullscreen(app, true), "set_fullscreen(true)");
    pump(app, 6);
    expect(cui_app_is_fullscreen(app), "fullscreen");
    RECT now;
    GetWindowRect(hwnd, &now);
    expect(now.left == mr.left && now.top == mr.top && now.right == mr.right && now.bottom == mr.bottom, "the window covers its monitor");
    expect(!(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION), "no caption");
    expect(cui_app_fullscreen_strip(app) > 0, "the edge strip exists");
    expect(SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM(mr.left + 1, mr.top + 1)) == HTCLIENT, "everything is the client area");
    expect(cui_app_fullscreen_hint(app, NULL), "the hint is due once");
    expect(!cui_app_fullscreen_hint(app, NULL), "and only once");
    expect(!cui_app_fullscreen_peek(app, 80, false), "no peek at rest");
    cui_app_fullscreen_sim_pointer(app, true, mr.left + 50, mr.top + 1, 50, 1);   /* the pointer on the strip */
    expect(cui_app_fullscreen_peek(app, 80, false), "the pointer on the strip shows the chrome");
    pump(app, 2);
    expect(cui_app_fullscreen_peek(app, 80, true), "held: stays");
    cui_app_fullscreen_sim_pointer(app, true, mr.left + 50, mr.top + 400, 50, 400);   /* far below the chrome */
    expect(cui_app_fullscreen_peek(app, 80, true), "held (a menu is open): stays although the pointer left");
    expect(!cui_app_fullscreen_peek(app, 80, false), "not held: the pointer below the chrome hides it");
    cui_app_fullscreen_sim_pointer(app, false, 0, 0, 0, 0);
    SendMessageW(hwnd, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    expect(cui_app_window_state(app) != CUI_WINDOW_MAXIMIZED, "the maximize command is ignored");
    expect(cui_app_set_fullscreen(app, false), "set_fullscreen(false)");
    pump(app, 6);
    expect(!cui_app_is_fullscreen(app), "left");
    GetWindowRect(hwnd, &now);
    expect(now.left == before.left && now.top == before.top && now.right == before.right && now.bottom == before.bottom,
           "the window is back at its rectangle");
    expect(cui_app_fullscreen_strip(app) == 0, "no strip");
    expect(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION, "the caption is back");

    /* ---- follow_system_caret: a focused field at rest asks for no frame once the system's timeout went by ---- */
    expect(caret_wakeup_after(app, 12) < 0 || sys > 12.0f, "follow_system_caret: no caret wakeup 12 s after the last input");
    cui_app_destroy(app);
    cui_app *plain = cui_app_create(&(cui_app_desc){ .title = "cui caret test", .width = 320, .height = 200, .hidden = true,
                                                     .renderer = CUI_RENDERER_SOFTWARE });
    if (plain) {
        expect(caret_wakeup_after(plain, 12) >= 0, "without it the caret still blinks 12 s later");
        cui_app_destroy(plain);
    }
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
#else
int main(void) {
    printf("skip: Windows only\n");
    return 0;
}
#endif
