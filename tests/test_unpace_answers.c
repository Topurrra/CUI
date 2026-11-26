/* cui_app_unpace_answers (Windows): with the software renderer, frames that follow input are paced one display period
 * apart. A worker's wake (WM_APP) that comes right after a frame that took a key starts the next frame at once when
 * the app opted in; without the opt in, and for a wake with no key before it (a flood), the frame waits for its period
 * as before. Measured as the time from one frame's start to the next, median of 15 tries each.
 * Needs a window (soft skip when none can be made). */
#include <cui/cui_app.h>

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

/* kind 0: a key and a wake in the frame; 1: a wake alone; returns the median gap to the next frame's start, ms */
static double gaps(cui_app *app, int kind, int tries) {
    double g[64], t0 = 0;
    int n = 0, step = 0;
    HWND hw = (HWND)cui_app_native_window(app);
    while (n < tries && cui_app_frame(app)) {
        cui_ctx *ui = cui_app_ui(app);
        double t = now_ms();
        if (step == 1) g[n++] = t - t0;                     /* the frame after the one that took the key or the wake */
        cui_column(ui, .pad = 8) { cui_text(ui, "unpace test"); }
        step = (step + 1) % 3;                              /* a quiet frame between tries: each starts from the pace */
        if (step == 1) {
            if (kind == 0) { cui_input_key(ui, CUI_KEY_A, true, 0); cui_input_key(ui, CUI_KEY_A, false, 0); }
            PostMessageW(hw, WM_APP + 0x70, 0, 0);          /* a worker's wake, as a terminal's output thread posts it */
            t0 = t;
        }
    }
    qsort(g, (size_t)n, sizeof g[0], cmp_d);
    return n ? g[n / 2] : -1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "CUI unpace test", .width = 320, .height = 200,
                                                   .renderer = CUI_RENDERER_SOFTWARE, .continuous = true });
    if (!app) { fprintf(stderr, "skip: could not create app (no display?)\n"); return 0; }
    for (int i = 0; i < 5 && cui_app_frame(app); i++) cui_text(cui_app_ui(app), "warm up");
    double off_key = gaps(app, 0, 15);
    cui_app_unpace_answers(app, 50);
    double on_key = gaps(app, 0, 15), on_wake = gaps(app, 1, 15);
    cui_app_unpace_answers(app, 0);
    double off_again = gaps(app, 0, 15);
    cui_app_destroy(app);
    printf("next frame after a key and a wake: off %.2f ms, on %.2f ms, off again %.2f ms; a wake alone (on) %.2f ms\n",
           off_key, on_key, off_again, on_wake);
    /* paced frames are a display period apart (8 ms at most at 120 Hz); an answer to a key comes well before */
    if (off_key < 6.0 || off_again < 6.0) { fprintf(stderr, "fail: without the opt in the frame after a key should keep its pace\n"); return 1; }
    if (on_wake < 6.0) { fprintf(stderr, "fail: a wake with no key before it should keep its pace (a flood)\n"); return 1; }
    if (on_key > off_key * 0.5) { fprintf(stderr, "fail: with the opt in the answer to a key should not wait for the pace\n"); return 1; }
    return 0;
}
