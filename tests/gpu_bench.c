#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
/* Renderer bench: the component gallery in a real window, driven fast, so that every renderer
 * draws the same busy mix of hovers, presses, scrolling, page changes and window resizes.
 *
 * Each page is four slots: the pointer moves onto a control (a hover), moves
 * onto another and clicks it, scrolls the content, and moves onto a third and clicks it. One
 * idle frame follows each action. A pass over all
 * pages is 17 frames a page, about 8 seconds at 60 frames a second.
 *
 * The window is resized as well. Every other page it enters jumps to the next of six sizes
 * (desktop, tablet and phone widths). Four pages (every seventh, from the fourth) instead
 * drag to the next size over one second of wall time, a step every frame, as if the corner
 * were held: that is 4 seconds more a pass.
 *
 *   cui_gpu_bench [options]
 *     --renderer R    gpu (default), opengl, software or auto: what cui_app_desc.renderer asks for
 *     --fps N         frames a second the script is held to, default 60; 0 for as fast as the display allows
 *     --uncapped      do not wait for the display (cui_app_desc.no_vsync): the time a frame takes
 *     --size WxH      window size to start at in points, default 1240x820
 *     --repeat N      N passes over the pages, default 1
 *     --seconds S     stop the script after S seconds, default 20
 *     --quiet         only the totals
 *
 * It reports, for the run: wall time, frames, the time cui_app_frame takes (which is where the
 * frame before is drawn and presented, and the wait for the display), the time the gallery takes
 * to build a frame, the process's CPU time and its peak memory. Those are what one process can
 * read about itself. The GPU's load and the system compositor's are not, and they are most of
 * what a renderer changes: sample them from outside while this runs. It also counts the moves
 * that ended over a control, the clicks that pressed one, the resizes that reached the size
 * asked for, and times the frames drawn right after a resize, and the resize calls, on their own.
 *
 * The same pages get the same moves and sizes every run (the targets come from a fixed seed
 * and the layout), so runs with different renderers do the same work. Not run by ctest: it
 * needs a window. */
#include "../src/cui_internal.h"
#include <cui/cui_app.h>
#include "../examples/gallery_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#include <time.h>
#endif

#define GB_SLOTS 4              /* what the pointer does on a page, in turn: see gb_step */
#define GB_IDLE_FRAMES 1        /* idle frames after each action */
#define GB_PAGE_FRAMES (2 + 3 + 4 + 3 + GB_SLOTS * GB_IDLE_FRAMES)
#define GB_WARMUP 10            /* frames before the script, not timed: pipelines and glyph atlas */
#define GB_SIDEBAR 240          /* the gallery's page list: moves stay out of it, or a click changes the page */
#define GB_NARROW 760           /* below this width the gallery has no page list, only a drawer */
#define GB_DRAG_SECONDS 1.0     /* how long a dragged resize takes */
#define GB_MAX_PAGES 128
#define GB_MAX_TARGETS 512

/* the window sizes the pages are shown at in turn, in points: desktop, tablet and phone widths */
static const int gb_sizes[][2] = { { 1240, 820 }, { 960, 700 }, { 700, 860 }, { 1400, 900 }, { 800, 600 }, { 1100, 780 } };
#define GB_SIZE_COUNT ((int)(sizeof gb_sizes / sizeof gb_sizes[0]))

/* ---- what the process can tell about itself ---- */

static double gb_wall(void) {
#if defined(_WIN32)
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

/* Sleeps until the wall time t (no spinning, so the wait costs no CPU). */
static void gb_sleep_until(double t) {
    double left = t - gb_wall();
    if (left <= 0) return;
#if defined(_WIN32)
    static HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(left * 1e7);
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)(left * 1000));
#else
    struct timespec ts = { (time_t)left, (long)((left - (double)(time_t)left) * 1e9) };
    nanosleep(&ts, NULL);
#endif
}

/* CPU seconds of the whole process, all threads */
static double gb_cpu(void) {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    ULONGLONG k = ((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ULONGLONG u = ((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return (double)(k + u) * 1e-7;
#else
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

static double gb_peak_mb(void) {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    return GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc) ? (double)pmc.PeakWorkingSetSize / 1048576.0 : 0;
#else
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#if defined(__APPLE__)
    return (double)ru.ru_maxrss / 1048576.0;   /* bytes */
#else
    return (double)ru.ru_maxrss / 1024.0;      /* kilobytes */
#endif
#endif
}

/* ---- the run: one frame at a time, with the numbers kept ---- */

typedef struct gb_samples {
    double *v;
    int n, cap;
} gb_samples;

static void gb_add(gb_samples *s, double x) {
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 1024;
        s->v = realloc(s->v, sizeof(double) * (size_t)s->cap);
        if (!s->v) exit(1);
    }
    s->v[s->n++] = x;
}

typedef struct gb_page_stats {
    double call_sum, call_max, build_sum;
    int frames;
    int moves, hovered, clicks, pressed;   /* what the pointer did: moves onto a control, ended over one, clicks, pressed one */
} gb_page_stats;

typedef struct gb_run {
    cui_app *app;
    gallery *g;
    double seconds, wall0, cpu0;
    double period, next;                   /* the time a frame is given (0: as fast as the display allows) and when the next one starts */
    bool timing, cut;                      /* timing: the script has started, the numbers count */
    double call_ms;                        /* what cui_app_frame took this frame */
    gb_samples calls, builds, after_resize, set_sizes;   /* set_sizes: what each cui_app_set_size call took */
    gb_page_stats page[GB_MAX_PAGES];
    bool resized_now, resized_before;      /* the window was resized this frame, or on the one before */
    int size_index;                        /* the next of gb_sizes */
    int jumps, jumps_ok, drags, drag_steps;
    int expect_w, expect_h;                /* the size a jump asked for, until the next frame has checked it */
} gb_run;

static cui_rect gb_targets[GB_MAX_TARGETS];
static int gb_target_count;
static uint32_t gb_rng;

static uint32_t gb_rand(void) {
    gb_rng ^= gb_rng << 13;
    gb_rng ^= gb_rng >> 17;
    gb_rng ^= gb_rng << 5;
    return gb_rng;
}

/* The width of the page list at the left of the gallery, which is a drawer when narrow. */
static float gb_side(float vw) {
    return vw < GB_NARROW ? 0 : GB_SIDEBAR;
}

/* The controls of the last frame that are worth a visit: a button's size, wholly inside the
 * content, and not a scroll area. */
static void gb_collect(cui_ctx *ui, float vw, float vh) {
    gb_target_count = 0;
    for (int i = 0; i < ui->prev_hit_count && gb_target_count < GB_MAX_TARGETS; i++) {
        const cui_hit *h = &ui->prev_hits[i];
        if (h->rect.w < 8 || h->rect.h < 8 || h->rect.w > vw * 0.6f || h->rect.h > vh * 0.5f) continue;
        if (h->flags & (CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y)) continue;
        if (h->rect.x < (gb_side(vw) ? GB_SIDEBAR + 4 : 0) || h->rect.x + h->rect.w > vw || h->rect.y < 0 || h->rect.y + h->rect.h > vh) continue;
        gb_targets[gb_target_count++] = h->rect;
    }
}

/* The start of a frame: waits for its slot (which is where the frame before is drawn and
 * presented), checks the size a jump asked for, and finds the controls to visit. NULL when the
 * window closed or the time is up. */
static cui_ctx *gb_begin(gb_run *r) {
    if (r->period > 0) {   /* a fixed rate, so every renderer does the same work in the same time */
        double now = gb_wall();
        if (r->next == 0 || now > r->next + r->period) r->next = now;
        gb_sleep_until(r->next);
        r->next += r->period;
    }
    double a = gb_wall();
    if (!cui_app_frame(r->app)) return NULL;
    r->call_ms = (gb_wall() - a) * 1000;
    if (r->timing && gb_wall() - r->wall0 > r->seconds) { r->cut = true; return NULL; }
    cui_ctx *ui = cui_app_ui(r->app);
    cui_vec2 size = cui_app_size(r->app);
    if (r->expect_w) {
        if ((int)(size.x + 0.5f) == r->expect_w && (int)(size.y + 0.5f) == r->expect_h) r->jumps_ok++;
        r->expect_w = 0;
    }
    gb_collect(ui, size.x, size.y);
    return ui;
}

/* The end of a frame: builds it, and keeps the numbers of the page it was on. */
static void gb_end(gb_run *r, cui_ctx *ui, int page) {
    double b = gb_wall();
    gallery_frame(ui, r->g);
    double build_ms = (gb_wall() - b) * 1000;
    if (r->timing) {
        gb_page_stats *p = &r->page[page];
        gb_add(&r->calls, r->call_ms);
        gb_add(&r->builds, build_ms);
        if (r->resized_before) gb_add(&r->after_resize, r->call_ms);   /* the frame resized before is drawn in this call */
        p->call_sum += r->call_ms; p->build_sum += build_ms; p->frames++;
        if (r->call_ms > p->call_max) p->call_max = r->call_ms;
    }
    r->resized_before = r->resized_now;
    r->resized_now = false;
}

/* ---- the script ---- */

/* Moves the pointer to the middle of a random control. */
static void gb_move_to_control(gb_run *r, cui_ctx *ui, int page) {
    r->page[page].moves++;
    if (!gb_target_count) return;
    cui_rect c = gb_targets[gb_rand() % (uint32_t)gb_target_count];
    cui_input_mouse_move(ui, c.x + c.w * 0.5f, c.y + c.h * 0.5f);
}

/* A control is hot once the pointer has rested over it for a frame, so this reads where the
 * last move left the pointer. */
static void gb_note_hover(gb_run *r, cui_ctx *ui, int page) {
    if (ui->hot) r->page[page].hovered++;
}

static void gb_new_page(cui_ctx *ui, gallery *g, int page) {
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);   /* lets go of anything held, and closes what a click opened */
    cui_input_key(ui, CUI_KEY_ESCAPE, true, 0);
    cui_input_key(ui, CUI_KEY_ESCAPE, false, 0);
    g->page = page;
    gb_rng = 2463534242u + (uint32_t)page * 7919u;
}

/* Frame f (1 to GB_PAGE_FRAMES) of a page, in four slots, each starting with a move, then
 * the frames its action needs, then GB_IDLE_FRAMES idle frames:
 *   0  hover: onto a control (2 frames)
 *   1  click: onto a control, press on the next frame, release on the one after (3 frames)
 *   2  scroll: onto the middle of the content, three wheel steps on the next three frames (4 frames)
 *   3  click: as slot 1, on what the scroll brought into view (3 frames)
 * A press needs the control to be hot already, which it is the frame after the move. */
static void gb_step(gb_run *r, cui_ctx *ui, int page, int f) {
    static const int len[GB_SLOTS] = {2, 3, 4, 3};
    cui_vec2 size = cui_app_size(r->app);
    int slot = 0, k = f - 1;
    while (k >= len[slot] + GB_IDLE_FRAMES) k -= len[slot++] + GB_IDLE_FRAMES;
    if (k >= len[slot]) return;   /* idle frame */
    if (k == 0) {
        if (slot == 2) cui_input_mouse_move(ui, gb_side(size.x) + (size.x - gb_side(size.x)) * 0.5f, size.y * 0.5f);   /* (not onto a control: not counted) */
        else gb_move_to_control(r, ui, page);
        return;
    }
    if (slot == 0 && k == 1) gb_note_hover(r, ui, page);
    if (slot == 2 && k >= 1 && k <= 3) cui_input_scroll(ui, 0, -3);
    if (slot == 1 || slot == 3) {
        if (k == 1) {
            gb_note_hover(r, ui, page);
            r->page[page].clicks++;
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        } else if (k == 2) {
            if (ui->active) r->page[page].pressed++;
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        }
    }
}

/* cui_app_set_size, timed: a window resize is done inside this call, and what the renderers
 * have to do for it (new swapchain or context size) is part of its cost. */
static void gb_set_size(gb_run *r, int w, int h) {
    double a = gb_wall();
    cui_app_set_size(r->app, w, h);
    if (r->timing) gb_add(&r->set_sizes, (gb_wall() - a) * 1000);
}

static void gb_next_size(gb_run *r, int *w, int *h) {
    r->size_index = (r->size_index + 1) % GB_SIZE_COUNT;
    *w = gb_sizes[r->size_index][0];
    *h = gb_sizes[r->size_index][1];
}

/* The window jumps to the next size; the next frame checks that it got there. */
static void gb_jump(gb_run *r) {
    gb_next_size(r, &r->expect_w, &r->expect_h);
    gb_set_size(r, r->expect_w, r->expect_h);
    r->resized_now = true;
    r->jumps++;
}

/* The window is dragged to the next size over GB_DRAG_SECONDS: a size every frame, easing in
 * and out as a hand would. A frame of the page is drawn at each. False when the run is over. */
static bool gb_drag(gb_run *r, int page) {
    cui_vec2 from = cui_app_size(r->app);
    int to_w, to_h, last_w = (int)(from.x + 0.5f), last_h = (int)(from.y + 0.5f);
    gb_next_size(r, &to_w, &to_h);
    r->drags++;
    double t0 = gb_wall();
    for (;;) {
        cui_ctx *ui = gb_begin(r);
        if (!ui) return false;
        double t = (gb_wall() - t0) / GB_DRAG_SECONDS;
        bool last = t >= 1;
        double e = last ? 1 : t * t * (3 - 2 * t);
        int w = (int)(from.x + (to_w - from.x) * e + 0.5), h = (int)(from.y + (to_h - from.y) * e + 0.5);
        if (w != last_w || h != last_h) {
            gb_set_size(r, w, h);
            last_w = w; last_h = h;
            r->resized_now = true;
            r->drag_steps++;
        }
        gb_end(r, ui, page);
        if (last) return true;
    }
}

/* ---- the numbers ---- */

static int gb_compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct gb_summary {
    double mean, p50, p99, max;
} gb_summary;

static gb_summary gb_summarize(gb_samples *s) {
    gb_summary out = { 0, 0, 0, 0 };
    if (!s->n) return out;
    qsort(s->v, (size_t)s->n, sizeof(double), gb_compare);
    double sum = 0;
    for (int i = 0; i < s->n; i++) sum += s->v[i];
    out.mean = sum / s->n;
    out.p50 = s->v[(int)(0.5 * (s->n - 1) + 0.5)];
    out.p99 = s->v[(int)(0.99 * (s->n - 1) + 0.5)];
    out.max = s->v[s->n - 1];
    return out;
}

typedef struct gb_options {
    const char *renderer;
    bool uncapped, quiet;
    double fps;
    int width, height, repeat;
    double seconds;
} gb_options;

static bool gb_parse(int argc, char **argv, gb_options *o) {
    o->renderer = "gpu"; o->width = gb_sizes[0][0]; o->height = gb_sizes[0][1]; o->repeat = 1; o->seconds = 20; o->fps = 60;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--renderer") && i + 1 < argc) o->renderer = argv[++i];
        else if (!strcmp(a, "--uncapped")) o->uncapped = true;
        else if (!strcmp(a, "--fps") && i + 1 < argc) o->fps = atof(argv[++i]);
        else if (!strcmp(a, "--quiet")) o->quiet = true;
        else if (!strcmp(a, "--size") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &o->width, &o->height) != 2) return false; }
        else if (!strcmp(a, "--repeat") && i + 1 < argc) o->repeat = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) o->seconds = atof(argv[++i]);
        else return false;
    }
    return o->width > 0 && o->height > 0 && o->repeat > 0 && o->seconds > 0;
}

static bool gb_renderer(const char *name, cui_renderer_kind *kind) {
    if (!strcmp(name, "gpu")) *kind = CUI_RENDERER_GPU;
    else if (!strcmp(name, "opengl")) *kind = CUI_RENDERER_OPENGL;
    else if (!strcmp(name, "software")) *kind = CUI_RENDERER_SOFTWARE;
    else if (!strcmp(name, "auto")) *kind = CUI_RENDERER_AUTO;
    else return false;
    return true;
}

static void gb_report(const gb_run *r, gb_samples *calls, gb_samples *builds, gb_samples *after, gb_samples *set_sizes, const gb_options *o, int pages,
                      double startup_ms, double wall, double cpu, cui_vec2 start_size) {
    if (!o->quiet) {
        printf("%-24s %6s %9s %9s %9s %6s %8s %7s %8s\n", "page", "frames", "call ms", "max ms", "build ms", "moves", "hovered", "clicks", "pressed");
        for (int p = 0; p < pages; p++) {
            const gb_page_stats *s = &r->page[p];
            if (s->frames) printf("%-24s %6d %9.2f %9.2f %9.3f %6d %8d %7d %8d\n", gallery_page_name(p), s->frames, s->call_sum / s->frames, s->call_max,
                                  s->build_sum / s->frames, s->moves, s->hovered, s->clicks, s->pressed);
        }
    }
    int moves = 0, hovered = 0, clicks = 0, pressed = 0;
    for (int p = 0; p < pages; p++) { moves += r->page[p].moves; hovered += r->page[p].hovered; clicks += r->page[p].clicks; pressed += r->page[p].pressed; }
    gb_summary c = gb_summarize(calls), b = gb_summarize(builds), a = gb_summarize(after), z = gb_summarize(set_sizes);
    printf("\nrenderer asked for: %s%s, window from %.0fx%.0f points, startup %.0f ms\n", o->renderer, o->uncapped ? ", uncapped" : "", start_size.x, start_size.y, startup_ms);
    printf("%d frames in %.2f s (%.1f frames/s)%s\n", calls->n, wall, wall > 0 ? calls->n / wall : 0, r->cut ? ", cut short by --seconds" : "");
    printf("cui_app_frame ms: mean %.2f  p50 %.2f  p99 %.2f  max %.2f\n", c.mean, c.p50, c.p99, c.max);
    printf("build ms: mean %.3f\n", b.mean);
    printf("resizes: %d jumps (%d reached the size asked for), %d drags of %d steps\n", r->jumps, r->jumps_ok, r->drags, r->drag_steps);
    printf("frames drawn right after a resize: %d, ms mean %.2f  p50 %.2f  p99 %.2f  max %.2f\n", after->n, a.mean, a.p50, a.p99, a.max);
    printf("cui_app_set_size ms (the window resize itself): %d calls, mean %.2f  p50 %.2f  p99 %.2f  max %.2f\n", set_sizes->n, z.mean, z.p50, z.p99, z.max);
    printf("pointer: %d moves, %d ended over a control; %d clicks, %d pressed one\n", moves, hovered, clicks, pressed);
    printf("process CPU %.2f s = %.1f%% of one core; peak memory %.0f MB\n", cpu, wall > 0 ? cpu / wall * 100 : 0, gb_peak_mb());
    printf("not measured here: the GPU's load and the system compositor's (sample them from outside)\n");
}

int main(int argc, char **argv) {
    gb_options o = { 0 };
    cui_renderer_kind kind;
    if (!gb_parse(argc, argv, &o) || !gb_renderer(o.renderer, &kind)) {
        fprintf(stderr, "usage: cui_gpu_bench [--renderer gpu|opengl|software|auto] [--fps N] [--uncapped] [--size WxH] [--repeat N] [--seconds S] [--quiet]\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    double created = gb_wall();
    cui_app *app = cui_app_create(&(cui_app_desc){ .title = "CUI GPU bench", .width = o.width, .height = o.height, .continuous = true,
                                                   .no_vsync = o.uncapped, .renderer = kind });
    if (!app) { fprintf(stderr, "could not create app\n"); return 1; }
    double startup_ms = (gb_wall() - created) * 1000;
    static gallery g;
    gallery_init(&g);
    static gb_run r;
    r.app = app; r.g = &g; r.seconds = o.seconds; r.period = o.fps > 0 ? 1.0 / o.fps : 0;
    cui_vec2 start_size = cui_app_size(app);
    int pages = gallery_page_total();
    if (pages > GB_MAX_PAGES) pages = GB_MAX_PAGES;

    for (int i = 0; i < GB_WARMUP; i++) {   /* pipelines and the glyph atlas, not counted */
        cui_ctx *ui = gb_begin(&r);
        if (!ui) return 1;
        gb_end(&r, ui, 0);
    }
    r.timing = true;
    r.wall0 = gb_wall();
    double cpu0 = gb_cpu();
    bool running = true;
    for (int n = 0; n < pages * o.repeat && running; n++) {
        int page = n % pages;
        bool drag = page % 7 == 3;   /* four pages of the gallery's thirty */
        cui_ctx *ui = gb_begin(&r);
        if (!ui) break;
        gb_new_page(ui, &g, page);
        if (!drag && n % 2 == 1) gb_jump(&r);   /* every other page */
        gb_end(&r, ui, page);
        if (drag) running = gb_drag(&r, page);
        for (int f = 1; f <= GB_PAGE_FRAMES && running; f++) {
            ui = gb_begin(&r);
            if (!ui) { running = false; break; }
            gb_step(&r, ui, page, f);
            gb_end(&r, ui, page);
        }
    }
    double wall = gb_wall() - r.wall0, cpu = gb_cpu() - cpu0;
    cui_app_destroy(app);
    gb_report(&r, &r.calls, &r.builds, &r.after_resize, &r.set_sizes, &o, pages, startup_ms, wall, cpu, start_size);
    free(r.calls.v);
    free(r.builds.v);
    free(r.after_resize.v);
    free(r.set_sizes.v);
    return 0;
}
