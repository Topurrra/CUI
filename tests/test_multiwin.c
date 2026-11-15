/* Several windows on one thread: cui_app_frame_poll, cui_app_pending, cui_app_wait, window positions
 * and the one-GL-context-per-thread rule. Windows are created hidden. Skips (exit 0) where no
 * window can be made (a machine without a display). The timing checks are loose: the machine may be busy. */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static double now_s(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static double cpu_s(void) {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    return (double)(((unsigned long long)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((unsigned long long)u.dwHighDateTime << 32 | u.dwLowDateTime)) * 1e-7;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static cui_app *make(const char *title, bool software) {
    cui_app_desc d;
    memset(&d, 0, sizeof d);
    d.title = title;
    d.width = 320; d.height = 240;
    d.hidden = true;
    d.renderer = software ? CUI_RENDERER_SOFTWARE : CUI_RENDERER_AUTO;
    return cui_app_create(&d);
}

static void draw(cui_app *a, const char *text) {
    cui_ctx *ui = cui_app_ui(a);
    cui_column(ui, .pad = 8) { cui_text(ui, text); }
}

/* One pass of the documented loop. A message that is no wakeup (WM_NULL) is posted first, so the
 * wait ends at once whether or not anything is pending: a test must not block for good. */
static void pass(cui_app *const *apps, int n, int *frames) {
    for (int i = 0; i < n; i++)
        if (apps[i] && cui_app_pending(apps[i]) && cui_app_frame_poll(apps[i])) { draw(apps[i], "x"); frames[i]++; }
#ifdef _WIN32
    PostMessageW((HWND)cui_app_native_window(apps[0]), WM_NULL, 0, 0);
#endif
    cui_app_wait(apps, n);
}
static void settle(cui_app *const *apps, int n, int *frames) {
    for (int k = 0; k < 40; k++) {
        pass(apps, n, frames);
        bool any = false;
        for (int i = 0; i < n; i++) any = any || (apps[i] && cui_app_pending(apps[i]));
        if (!any) return;
    }
}

#ifdef _WIN32
static DWORD WINAPI poster(LPVOID p) {
    Sleep(250);
    PostMessageW((HWND)p, WM_APP + 0x70, 0, 0);   /* what a worker thread does to wake a window */
    return 0;
}
#endif

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cui_app *a = make("multi A", false);
    if (!a) { printf("no window can be made here, skipped\n"); return 0; }
    cui_app *b = make("multi B", false);
    CHECK(b != NULL);
    if (!b) return 1;
    printf("backend=%s a=%s b=%s\n", cui_app_backend(a), cui_app_is_software(a) ? "software" : "opengl", cui_app_is_software(b) ? "software" : "opengl");
    /* a second window on the thread never takes a second GL context */
    CHECK(cui_app_is_software(b));

    /* a polled app is the one NULL-app calls name */
    CHECK(cui_app_frame_poll(a));
    CHECK(cui_app_ui(NULL) == cui_app_ui(a));
    draw(a, "a");
    CHECK(cui_app_frame_poll(b));
    CHECK(cui_app_ui(NULL) == cui_app_ui(b));
    draw(b, "b");

    /* both draw their first frames, then nothing is pending: idle */
    cui_app *apps[2] = { a, b };
    int frames[2] = { 0, 0 };
#ifdef _WIN32
    settle(apps, 2, frames);
#else
    (void)pass; (void)settle;
#endif
    int settled0 = frames[0], settled1 = frames[1];
#ifdef _WIN32
    CHECK(settled0 >= 1 && settled1 >= 1);
    CHECK(!cui_app_pending(a) && !cui_app_pending(b));

    /* a timer: a wakeup requested for 0.25 s makes the wait block about that long, then the app is pending, the other not */
    cui_request_wakeup(cui_app_ui(a), 0.25f);
    double t0 = now_s();
    cui_app_wait(apps, 2);
    double dt = now_s() - t0;
    printf("timer wait: %.3f s\n", dt);
    CHECK(dt > 0.15 && dt < 0.6);
    CHECK(cui_app_pending(a) && !cui_app_pending(b));
    if (cui_app_frame_poll(a)) draw(a, "a");
    settle(apps, 2, frames);

    /* a worker's message to the second window wakes the wait, and only that window is pending */
    CHECK(!cui_app_pending(b));
    HANDLE th = CreateThread(NULL, 0, poster, cui_app_native_window(b), 0, NULL);
    t0 = now_s();
    cui_app_wait(apps, 2);
    dt = now_s() - t0;
    printf("message wait: %.3f s\n", dt);
    CHECK(dt > 0.15 && dt < 0.8);
    CHECK(cui_app_pending(b) && !cui_app_pending(a));
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    if (cui_app_frame_poll(b)) draw(b, "b");
    settle(apps, 2, frames);
    CHECK(!cui_app_pending(a) && !cui_app_pending(b));

    /* idle costs no CPU: a wait of 1.2 s for a timer uses next to no processor time */
    cui_request_wakeup(cui_app_ui(a), 1.2f);
    double c0 = cpu_s();
    t0 = now_s();
    cui_app_wait(apps, 2);
    dt = now_s() - t0;
    double cpu = cpu_s() - c0;
    printf("idle wait: %.3f s wall, %.4f s cpu\n", dt, cpu);
    CHECK(dt > 1.0 && cpu < 0.05);
    if (cui_app_frame_poll(a)) draw(a, "a");
    settle(apps, 2, frames);
#endif

    /* positions */
    cui_app_set_position(a, 120, 90);
    int x = -1, y = -1;
    bool ok = cui_app_position(a, &x, &y);
#ifdef _WIN32
    CHECK(ok && x == 120 && y == 90);
#else
    (void)ok;
#endif
    cui_app *c = NULL;
    {
        cui_app_desc d;
        memset(&d, 0, sizeof d);
        d.title = "multi C"; d.width = 200; d.height = 150; d.hidden = true; d.renderer = CUI_RENDERER_SOFTWARE;
        d.x = 210; d.y = 160; d.has_pos = true;
        c = cui_app_create(&d);
    }
    CHECK(c != NULL);
    if (c) {
        ok = cui_app_position(c, &x, &y);
#ifdef _WIN32
        CHECK(ok && x == 210 && y == 160);
#endif
        cui_app_destroy(c);
    }

    /* closing: poll returns false once, and the others go on */
    cui_app_quit(b);
    CHECK(cui_app_pending(b));
    CHECK(!cui_app_frame_poll(b));
    CHECK(cui_app_frame_poll(a));
    draw(a, "a");
    cui_app_destroy(b);
    apps[1] = NULL;
#ifdef _WIN32
    for (int i = 0; i < 4; i++) pass(apps, 2, frames);
#endif
    cui_app_destroy(a);

    /* with every window gone, a new first window may use GL again */
    cui_app *d2 = make("multi D", false);
    CHECK(d2 != NULL);
    if (d2) cui_app_destroy(d2);

    printf("multiwin: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
