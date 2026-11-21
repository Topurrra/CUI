/* The shared app loop: renderer selection, frame pacing, event driven
 * sleeping and screenshots. Backends supply windows and events. */
#include "cui_platform.h"

static cui_app *cui__current_app;
/* A thread may keep one OpenGL context: the renderer's calls go to whichever context is current, and
 * nothing here makes the right one current before each. So the windows a thread makes after its first
 * GL window draw in software (see cui_app_frame_poll in cui_app.h). The thread that holds GL windows is
 * remembered while it has any; windows of other threads are not counted (a thread local would pull
 * emulated thread locals into every app on some toolchains: 70 KB). */
#ifdef _WIN32
__declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#define CUI__THREAD_ID() ((uintptr_t)GetCurrentThreadId())
#else
#include <pthread.h>
#define CUI__THREAD_ID() ((uintptr_t)pthread_self())
#endif
static uintptr_t cui__gl_thread;
static int cui__gl_apps;
static void cui__app_set_clip(void *user, const char *utf8);
static const char *cui__app_get_clip(void *user);

static bool cui__env_is(const char *name, const char *value) {
    const char *v = getenv(name);
    return v && strcmp(v, value) == 0;
}

static void cui__app_make_software(cui_app *app) {
    if (!app->sw) app->sw = cui_sw_create();
    cui_sw_set_incremental(app->sw, true);   /* only what changed is drawn again */
    app->software = true;
    cui_renderer_iface iface = cui_sw_iface(app->sw);
    cui_set_renderer(app->ui, &iface);
}

/* OpenGL that was being set up on another thread is ready: from the next frame on it draws.
 * The textures made so far move to it under the same handles. False while it is not ready. */
static bool cui__app_take_gl(cui_app *app, bool wait) {
    cui_gl_renderer *gl = NULL;
    if (!cui__plat_gl_end_async(app, wait, !app->desc.no_vsync, &gl)) return false;
    app->gl_starting = false;
    if (!gl) return true;   /* no usable OpenGL: software goes on */
    cui_renderer_iface iface = cui_gl_iface(gl);
    cui__sw_move_textures(app->sw, &iface);
    app->ui->renderer = iface;   /* not cui_set_renderer: that would drop the atlas the handles point to */
    app->gl = gl;
    app->software = false;
    cui_sw_destroy(app->sw);
    app->sw = NULL;
    free(app->sw_pixels);
    app->sw_pixels = NULL;
    app->sw_w = app->sw_h = 0;
    app->pace_frames = app->pace_fast = 0;
    cui_request_redraw(app->ui);
    return true;
}

/* An app that could not be made: the OpenGL thread is waited for and its work dropped. */
static void cui__app_drop_gl_start(cui_app *app) {
    cui_gl_renderer *gl = NULL;
    if (app->gl_starting && cui__plat_gl_end_async(app, true, false, &gl)) cui_gl_destroy(gl);
    app->gl_starting = false;
}

cui_app *cui_app_create(const cui_app_desc *desc) {
    cui_app *app = calloc(1, sizeof *app);
    if (!app) return NULL;
    if (desc) app->desc = *desc;
    cui_app_desc *d = &app->desc;
    if (!d->title) d->title = "CUI";
    snprintf(app->title, sizeof app->title, "%s", d->title);
    d->title = app->title;
    if (d->app_id && !d->app_id[0]) d->app_id = NULL;
    if (d->width <= 0) d->width = 1080;
    if (d->height <= 0) d->height = 720;
    if (d->min_width <= 0) d->min_width = 360;
    if (d->min_height <= 0) d->min_height = 280;
    if (d->placement) {
        /* backends read the size from width and height, and only a maximized state at opening */
        app->placement = *d->placement;
        d->placement = &app->placement;
        cui_window_placement *p = &app->placement;
        if (p->width > 0) d->width = p->width;
        if (p->height > 0) d->height = p->height;
        if (d->width < d->min_width) d->width = d->min_width;
        if (d->height < d->min_height) d->height = d->min_height;
        p->width = d->width;
        p->height = d->height;
        if (p->state != CUI_WINDOW_MAXIMIZED) p->state = CUI_WINDOW_NORMAL;
    } else if (d->has_pos || d->start_maximized) {
        /* x, y, has_pos and start_maximized are a placement at width x height, so every backend
         * opens a window in place or maximized the one way */
        cui_window_placement *p = &app->placement;
        p->width = d->width;
        p->height = d->height;
        d->placement = p;
    }
    if (d->placement) {
        cui_window_placement *p = &app->placement;
        if (d->has_pos && !p->has_position) { p->x = d->x; p->y = d->y; p->has_position = true; }
        if (d->start_maximized) p->state = CUI_WINDOW_MAXIMIZED;
        if (d->fixed_size) p->state = CUI_WINDOW_NORMAL;
    }
    cui_theme_mode mode = d->theme;
    if (d->follow_system_theme) {
        int dark = cui__plat_system_dark();
        if (dark >= 0) mode = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT;
    }
    /* CUI_THEME=dark|light and CUI_ACCENT=<name> override the app's choice
     * (handy for screenshots and for trying an app in the other theme) */
    if (cui__env_is("CUI_THEME", "dark")) mode = CUI_THEME_DARK;
    if (cui__env_is("CUI_THEME", "light")) mode = CUI_THEME_LIGHT;
    static const char *accents[CUI_ACCENT_COUNT] = { "neutral", "violet", "blue", "green", "amber", "orange", "coral", "rose" };
    for (int i = 0; i < CUI_ACCENT_COUNT; i++)
        if (cui__env_is("CUI_ACCENT", accents[i])) d->accent = (cui_accent)i;
    d->theme = mode;
    /* Auto: OpenGL is set up on another thread while the first pictures are drawn in software,
     * so the window shows without waiting for the driver. Tests and screenshots, and an app or
     * user that asks for a renderer, get it from the first frame. */
    bool want_gl = d->renderer != CUI_RENDERER_SOFTWARE && !cui__env_is("CUI_RENDERER", "software");
    if (want_gl && cui__gl_apps > 0 && cui__gl_thread == CUI__THREAD_ID()) {   /* this thread already has a GL window */
        if (d->renderer == CUI_RENDERER_OPENGL) fprintf(stderr, "cui: one OpenGL window per thread, using the software renderer\n");
        want_gl = false;
    }
    if (want_gl && (cui__gl_apps == 0 || cui__gl_thread == CUI__THREAD_ID())) {
        cui__gl_thread = CUI__THREAD_ID();
        app->gl_counted = true;
        cui__gl_apps++;
    }
    if (want_gl && d->renderer == CUI_RENDERER_AUTO && !getenv("CUI_RENDERER") && !getenv("CUI_SCREENSHOT") && !d->hidden)
        app->gl_starting = cui__plat_gl_begin_async(app);
    cui_ctx_desc cd;
    memset(&cd, 0, sizeof cd);
    cd.theme = mode;
    cd.accent = d->accent;
    app->ui = cui_create(&cd);
    if (!app->ui) { cui__app_drop_gl_start(app); if (app->gl_counted) cui__gl_apps--; free(app); return NULL; }
    if (!cui__plat_init(app)) {
        cui__app_drop_gl_start(app);
        if (app->gl_counted) cui__gl_apps--;
        fprintf(stderr, "cui: could not create a window (%s backend)\n", cui__plat_name());
        cui_destroy(app->ui);
        free(app);
        return NULL;
    }
    if (!app->gl_starting && want_gl && cui__plat_gl_create(app, !d->no_vsync)) {
        app->gl = cui_gl_create(cui__plat_gl_proc);
        /* OpenGL drawn on the CPU (a virtual machine, a remote session) costs several times
         * what the software renderer does, as that one redraws only what changed. Where the
         * window can take a software picture as well, Auto goes software. */
        bool can_switch = !strcmp(cui__plat_name(), "x11") || !strcmp(cui__plat_name(), "win32");
        if (app->gl && cui_gl_is_software(app->gl) && d->renderer == CUI_RENDERER_AUTO && can_switch && !cui__env_is("CUI_RENDERER", "opengl")) {
            cui_gl_destroy(app->gl);
            app->gl = NULL;
            cui__plat_gl_release(app);
        }
    }
    if (app->gl) {
        cui_renderer_iface iface = cui_gl_iface(app->gl);
        cui_set_renderer(app->ui, &iface);
    } else {
        if (d->renderer == CUI_RENDERER_OPENGL) fprintf(stderr, "cui: OpenGL unavailable, using the software renderer\n");
        cui__app_make_software(app);
    }
    cui_platform_iface pi;
    memset(&pi, 0, sizeof pi);
    pi.user = app;
    /* backends may already have set clipboard/cursor callbacks */
    cui_ctx *ui = app->ui;
    if (!ui->platform.set_clipboard) { pi.set_clipboard = cui__app_set_clip; pi.get_clipboard = cui__app_get_clip; }
    else pi = ui->platform;
    cui_set_platform(ui, &pi);
    app->running = true;
    app->last_time = cui__plat_now();
    app->screenshot_path = getenv("CUI_SCREENSHOT");
    if (app->screenshot_path) {
        const char *f = getenv("CUI_SCREENSHOT_FRAMES");
        app->screenshot_frames = f ? atoi(f) : 90;
        d->continuous = true;
    }
    if (!d->hidden) cui__plat_show(app);
    cui__current_app = app;
    return app;
}

void cui_app_destroy(cui_app *app) {
    if (!app) return;
    if (cui__current_app == app) cui__current_app = NULL;
    if (app->in_frame) { cui_end_frame(app->ui); app->in_frame = false; }
    if (app->gl_starting) cui__app_take_gl(app, true);   /* then the thread is done and the context is this thread's */
    /* release textures while the GL context still exists */
    cui_set_renderer(app->ui, NULL);
    if (app->gl) cui_gl_destroy(app->gl);
    app->gl = NULL;
    /* close the window before the UI context goes away: the OS may still
     * deliver focus and size messages while the window is destroyed */
    cui__plat_shutdown(app);
    if (app->sw) cui_sw_destroy(app->sw);
    free(app->sw_pixels);
    cui_destroy(app->ui);
    free(app->clipboard);
    if (app->gl_counted) cui__gl_apps--;
    free(app);
}

/* NULL means the app that is running: a cui_app_run callback has no other way to name it */
static cui_app *cui__app_or_current(cui_app *app) { return app ? app : cui__current_app; }

cui_ctx *cui_app_ui(cui_app *app) { app = cui__app_or_current(app); return app ? app->ui : NULL; }
void cui_app_quit(cui_app *app) { app = cui__app_or_current(app); if (app) app->running = false; }
bool cui_app_is_software(cui_app *app) { app = cui__app_or_current(app); return app && app->software; }
void *cui_app_native_window(cui_app *app) { app = cui__app_or_current(app); return app ? cui__plat_native(app) : NULL; }
const char *cui_app_backend(cui_app *app) { (void)app; return cui__plat_name(); }
bool cui_app_has_custom_titlebar(cui_app *app) { app = cui__app_or_current(app); return app && cui__plat_window_op(app, 4); }
void cui_app_drag_window(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_window_op(app, 2); }

void cui_app_set_title(cui_app *app, const char *title) {
    app = cui__app_or_current(app);
    if (!app) return;
    snprintf(app->title, sizeof app->title, "%s", title ? title : "");
    cui__plat_set_title(app, app->title);
}

void cui_app_set_size(cui_app *app, int w, int h) { app = cui__app_or_current(app); if (app) cui__plat_set_size(app, w, h); }
bool cui_app_position(cui_app *app, int *x, int *y) {
    int px = 0, py = 0;
    app = cui__app_or_current(app);
    bool ok = app && cui__plat_position(app, &px, &py);
    if (x) *x = ok ? px : 0;
    if (y) *y = ok ? py : 0;
    return ok;
}

void cui_app_follow_system_theme(cui_app *app, bool follow) {
    app = cui__app_or_current(app);
    if (!app) return;
    app->desc.follow_system_theme = follow;    /* the backends read it when the OS setting changes */
    if (!follow) return;
    int dark = cui__plat_system_dark();
    if (dark >= 0) cui_set_theme(app->ui, dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
}

cui_vec2 cui_app_size(cui_app *app) {
    float w, h, s;
    int fw, fh;
    app = cui__app_or_current(app);
    if (!app) { cui_vec2 none = { 0, 0 }; return none; }
    cui__plat_metrics(app, &w, &h, &s, &fw, &fh);
    cui_vec2 v = { w, h };
    return v;
}

static void cui__app_set_clip(void *user, const char *utf8) { cui__app_set_clipboard_cache(user, utf8); }
static const char *cui__app_get_clip(void *user) { cui_app *app = user; return app->clipboard; }

void cui__app_set_clipboard_cache(cui_app *app, const char *utf8) {
    free(app->clipboard);
    app->clipboard = NULL;
    if (!utf8) return;
    size_t n = strlen(utf8);
    app->clipboard = malloc(n + 1);
    if (app->clipboard) memcpy(app->clipboard, utf8, n + 1);
}

void cui__app_render(cui_app *app) {
    if (app->in_frame) {
        cui_end_frame(app->ui);
        app->in_frame = false;
    }
    const cui_draw_data *dd = cui_get_draw_data(app->ui);
    int w = dd->width_px, h = dd->height_px;
    if (w <= 0 || h <= 0) return;
    CUI__PROBE(CUI_PROBE_RENDER);
    if (app->gl) {
        cui_gl_render(app->gl, dd, true);
        CUI__PROBE(CUI_PROBE_PRESENT);
        cui__plat_gl_swap(app);
    } else {
        if (w != app->sw_w || h != app->sw_h) {
            free(app->sw_pixels);
            app->sw_pixels = malloc((size_t)w * (size_t)h * 4);
            app->sw_w = w; app->sw_h = h;
        }
        if (!app->sw_pixels) return;
        cui_sw_render(app->sw, dd, app->sw_pixels, w, h, w * 4, CUI_SW_BGRA);
        CUI__PROBE(CUI_PROBE_PRESENT);
        int dx, dy, dw, dh;
        bool changed = cui_sw_last_damage(app->sw, &dx, &dy, &dw, &dh);
        if (app->force_present || !app->presented_once || (changed && !cui__plat_present_part(app, app->sw_pixels, w, h, w * 4, dx, dy, dw, dh)))
            cui__plat_present(app, app->sw_pixels, w, h, w * 4);
        app->force_present = false;
    }
    app->presented_once = true;
    app->last_present = cui__plat_now();
    CUI__PROBE(CUI_PROBE_DONE);
}

static void cui__app_begin(cui_app *app) {
    float w, h, s;
    int fw, fh;
    cui__plat_metrics(app, &w, &h, &s, &fw, &fh);
    cui_set_viewport(app->ui, w, h, s);
    double now = cui__plat_now();
    float dt = (float)(now - app->last_time);
    app->last_time = now;
    if (dt <= 0) dt = 1.0f / 120.0f;
    cui_begin_frame(app->ui, dt);
    app->in_frame = true;
    app->woken = false;
}

/* Presents the frame that was drawn (and takes the screenshot a test asked for). */
static void cui__app_finish(cui_app *app) {
    if (!app->in_frame) return;
    cui__app_render(app);
    app->idle_since = cui__plat_now();
    app->wake_seen = -2;   /* the timers of the frame just drawn count from now */
    if (app->screenshot_path && ++app->frames >= app->screenshot_frames) {
        const cui_draw_data *dd = cui_get_draw_data(app->ui);
        int w = dd->width_px, h = dd->height_px;
        unsigned char *px = malloc((size_t)w * (size_t)h * 4);
        if (px && cui_app_screenshot(app, px, w, h)) {
            bool ok = cui_write_png(app->screenshot_path, px, w, h, w * 4);
            fprintf(stderr, "cui: %s %s\n", ok ? "saved" : "could not save", app->screenshot_path);
        }
        free(px);
        app->running = false;
    }
}

/* How long the app may sleep before its next frame, in seconds: 0 when it animates (then *pace_until, when
 * not 0, is the time before which that frame should not start, for the pacing of software drawing), a
 * delay for a timer, < 0 for input only. */
static double cui__app_plan(cui_app *app, double *pace_until) {
    cui_ctx *ui = app->ui;
    double timeout;
    *pace_until = 0;
    if (app->desc.continuous || cui_needs_redraw(ui) || !app->presented_once) {
        /* Animation frames come at the display's rate. With vsync the swap waits for it; the
         * software renderer, and drivers that ignore the swap interval (software GL, settings
         * that force vsync off), are paced by the clock instead of drawing frames nobody sees.
         * The first picture waits for nothing (asking for the refresh rate can take 15 ms). */
        timeout = 0;
        double now = cui__plat_now();
        if (app->presented_once && (app->hz <= 0 || now - app->hz_checked > 2.0)) {   /* some clocks start at 0 */
            double hz = cui__plat_refresh_hz(app);
            app->hz = hz >= 24 && hz <= 1000 ? hz : 60;
            app->hz_checked = now;
        }
        if (app->gl && !app->desc.no_vsync && !app->vsync_off) {
            /* two runs of 20 frames in a row faster than the display: the swap does not wait
             * (one is not enough: after a pause a driver may queue a few frames without waiting) */
            if (app->pace_frames++ == 0) app->pace_start = app->last_present;
            else if (app->pace_frames == 21) {
                double span = app->last_present - app->pace_start;
                app->pace_fast = span > 0 && 20.0 / span > app->hz * 1.3 ? app->pace_fast + 1 : 0;
                if (app->pace_fast >= 2) app->vsync_off = true;
                app->pace_frames = 0;
            }
        }
        if (app->presented_once && (!app->gl || app->desc.no_vsync || app->vsync_off)) {
            /* one frame per period, counted from when the last one began; events that come
             * in the meantime are taken in but do not start the frame early */
            *pace_until = app->last_time + 1.0 / (app->desc.no_vsync && app->hz < 120 ? 120 : app->hz);
        }
    } else {
        app->pace_frames = 0;
        app->pace_fast = 0;
        float wake = cui_next_wakeup(ui);
        timeout = wake >= 0 ? wake : -1;
    }
    if (app->gl_starting && (timeout < 0 || timeout > 0.1)) timeout = 0.1;   /* the thread wakes the loop too; this is in case it cannot */
    return timeout;
}

bool cui_app_frame(cui_app *app) {
    cui__app_finish(app);
    if (!app->running) return false;
    double until, timeout = cui__app_plan(app, &until);
    if (until > 0)
        for (double left = until - cui__plat_now(); left > 0.0005 && app->running; left = until - cui__plat_now())
            cui__plat_wait(app, left);
    CUI__PROBE(CUI_PROBE_SLEEP);
    cui__plat_wait(app, timeout);
    CUI__PROBE(CUI_PROBE_WAKE);
    if (!app->running) return false;
    if (app->gl_starting) cui__app_take_gl(app, false);
    cui__app_begin(app);
    return true;
}

bool cui_app_frame_poll(cui_app *app) {
    app = cui__app_or_current(app);
    if (!app) return false;
    cui__app_finish(app);
    if (!app->running) return false;
    cui__current_app = app;
    cui__plat_wait(app, 0);   /* this thread's messages, every window's, without sleeping */
    if (!app->running) return false;
    if (app->gl_starting) cui__app_take_gl(app, false);
    cui__app_begin(app);
    return true;
}

/* When the app's next timer is due, in plat_now time, or < 0. Wake ups the frame asked for count from the end
 * of the frame; one asked for between frames (cui_request_wakeup) from when it is first seen here. The time stays
 * put while the loop wakes for other windows, as the frame's own wait would have kept it. */
static double cui__app_deadline(cui_app *app) {
    float w = cui_next_wakeup(app->ui);
    if (w != app->wake_seen) {
        app->wake_seen = w;
        app->wake_at = w >= 0 ? cui__plat_now() + w : -1;
    }
    return app->wake_at;
}

bool cui_app_pending(cui_app *app) {
    app = cui__app_or_current(app);
    if (!app) return false;
    if (!app->running || app->woken || app->desc.continuous || !app->presented_once || cui_needs_redraw(app->ui)) return true;
    double now = cui__plat_now(), due = cui__app_deadline(app);
    if (due >= 0 && now >= due) return true;
    return app->gl_starting && now - app->idle_since >= 0.1;   /* as the plan: the OpenGL thread may not be able to wake us */
}

void cui_app_wait(cui_app *const *apps, int n) {
    cui_app *first = NULL;
    double limit = -1, pace = 0, now;
    for (int i = 0; i < n; i++) if (apps[i]) { cui__app_finish(apps[i]); if (!first) first = apps[i]; }
    if (!first) return;
    now = cui__plat_now();
    for (int i = 0; i < n; i++) {
        cui_app *a = apps[i];
        if (!a) continue;
        double until = 0, t = 0;
        if (a->running && !a->woken) {
            t = cui__app_plan(a, &until);
            if (t != 0 || until == 0) {   /* not animating: the timer's deadline, not its delay (see cui__app_deadline) */
                bool animating = a->desc.continuous || cui_needs_redraw(a->ui) || !a->presented_once;
                if (!animating) {
                    double due = cui__app_deadline(a);
                    t = due >= 0 ? (due > now ? due - now : 0) : -1;
                    if (a->gl_starting && (t < 0 || t > 0.1)) t = 0.1;
                }
            }
        }
        if (t == 0 && until > now) { if (!pace || until < pace) pace = until; continue; }   /* animating: due at the next period */
        if (t >= 0 && (limit < 0 || t < limit)) limit = t;
    }
    if (pace > 0 && (limit < 0 || pace - now < limit)) {
        /* the nearest frame period: events on the way are taken in, the frame still waits for it */
        for (double left = pace - now; left > 0.0005; left = pace - cui__plat_now()) cui__plat_wait(first, left);
        limit = 0;
    }
    CUI__PROBE(CUI_PROBE_SLEEP);
    cui__plat_wait(first, limit);
    CUI__PROBE(CUI_PROBE_WAKE);
    for (int i = 0; i < n; i++)
        if (apps[i] && !apps[i]->wake_tracked) apps[i]->woken = true;   /* the backend cannot tell which window it was */
}

void cui__app_live_frame(cui_app *app) {
    /* called by backends while the OS runs a modal resize loop */
    if (!app->frame_cb || app->in_live_resize) {
        if (!app->in_frame) {
            const cui_draw_data *dd = cui_get_draw_data(app->ui);
            if (dd->vertex_count) cui__app_render(app);
        }
        return;
    }
    app->in_live_resize = true;
    if (app->in_frame) { cui_end_frame(app->ui); app->in_frame = false; }
    cui__app_begin(app);
    app->frame_cb(app->ui, app->frame_user);
    cui__app_render(app);
    app->in_live_resize = false;
}

int cui_app_run(const cui_app_desc *desc, void (*frame)(cui_ctx *ui, void *user), void *user) {
    cui_app *app = cui_app_create(desc);
    if (!app) return 1;
    app->frame_cb = frame;
    app->frame_user = user;
    while (cui_app_frame(app)) {
        if (frame) frame(app->ui, user);
    }
    cui_app_destroy(app);
    return 0;
}

bool cui_app_screenshot(cui_app *app, void *rgba, int width, int height) {
    const cui_draw_data *dd = cui_get_draw_data(app->ui);
    if (width < dd->width_px || height < dd->height_px || !rgba) return false;
    if (app->in_frame) cui__app_render(app);
    dd = cui_get_draw_data(app->ui);
    int w = dd->width_px, h = dd->height_px;
    if (app->gl) {
        /* re-render without swapping so the back buffer holds the frame */
        cui_gl_render(app->gl, dd, true);
        cui_gl_read_pixels(app->gl, w, h, rgba);
    } else {
        cui_sw_render(app->sw, dd, rgba, w, h, width * 4, CUI_SW_RGBA);
    }
    return true;
}

bool cui_app_file_dialog(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap) {
    cui_file_dialog_opts d;
    memset(&d, 0, sizeof d);
    if (!opts) opts = &d;
    if (!app) app = cui__current_app;
    if (!out || cap == 0 || !app) return false;
    out[0] = 0;
    bool ok = cui__plat_file_dialog(app, opts, out, cap);
    /* the dialog ran its own loop: input state is stale, and time jumped */
    cui_input_focus(app->ui, false);   /* releases keys, buttons and modifiers */
    cui_input_focus(app->ui, true);
    cui_input_mouse_leave(app->ui);
    app->last_time = cui__plat_now();
    cui_request_redraw(app->ui);
    return ok && out[0];
}

int cui_app_file_dialog_multi(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap) {
    cui_file_dialog_opts d;
    memset(&d, 0, sizeof d);
    if (opts) d = *opts;
    d.kind = CUI_FILE_OPEN;
    d.multi = true;
    if (!cui_app_file_dialog(app, &d, out, cap)) return 0;
    int n = 0;
    for (const char *p = out; *p; p += strlen(p) + 1) n++;
    return n;
}

bool cui_app_open_url(const char *url) { return url && *url && cui__plat_open_url(url); }

void cui_app_maximize(cui_app *app) {
    app = cui__app_or_current(app);
    /* a fixed-size window has no maximized state: on Win32 its borders are gone, and other
     * backends would still grow it */
    if (app && !app->desc.fixed_size) cui__plat_maximize(app);
}

void cui_app_minimize(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_minimize(app); }
void cui_app_restore(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_restore(app); }
void cui_app_set_position(cui_app *app, int x, int y) { app = cui__app_or_current(app); if (app) cui__plat_set_position(app, x, y); }

cui_window_state cui_app_window_state(cui_app *app) {
    app = cui__app_or_current(app);
    return app ? cui__plat_window_state(app) : CUI_WINDOW_NORMAL;
}

bool cui_app_placement(cui_app *app, cui_window_placement *out) {
    app = cui__app_or_current(app);
    if (!app || !out) return false;
    memset(out, 0, sizeof *out);
    cui__plat_placement(app, out);
    return true;
}

/* the custom title bar's controls, on top of the calls above */
bool cui_app_is_maximized(cui_app *app) { return cui_app_window_state(app) == CUI_WINDOW_MAXIMIZED; }
void cui_app_toggle_maximize(cui_app *app) {
    app = cui__app_or_current(app);
    if (!app) return;
    if (cui__plat_window_state(app) == CUI_WINDOW_MAXIMIZED) cui__plat_restore(app);
    else cui_app_maximize(app);
}

/* ---- the caret and idle frames (cui_app.h) ---- */

float cui_system_caret_blink(void) { return cui__plat_caret_blink(); }

bool cui_app_frames_on_change(cui_app *app, bool on) { app = cui__app_or_current(app); return app && cui__plat_frames_on_change(app, on); }
void cui_app_arm(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_arm(app); }
bool cui_app_frame_due(cui_app *app) { app = cui__app_or_current(app); return !app || cui__plat_frame_due(app); }
void cui_app_request_frame(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_request_frame(app); }
long cui_app_frames_skipped(cui_app *app) { app = cui__app_or_current(app); return app ? cui__plat_frames_skipped(app) : 0; }
bool cui_app_schedule(cui_app *app, int64_t ms, void (*tick)(void *user), void *user) {
    app = cui__app_or_current(app);
    return app && cui__plat_schedule(app, ms, tick, user);
}

/* ---- fullscreen (cui_app.h) ---- */

bool cui_app_set_fullscreen(cui_app *app, bool on) {
    app = cui__app_or_current(app);
    if (!app) return false;
    bool was = cui__plat_is_fullscreen(app);
    bool ok = cui__plat_set_fullscreen(app, on);
    if (ok && on && !was) app->fs_hint = true;
    return ok;
}
bool cui_app_is_fullscreen(cui_app *app) { app = cui__app_or_current(app); return app && cui__plat_is_fullscreen(app); }
float cui_app_fullscreen_strip(cui_app *app) { app = cui__app_or_current(app); return app ? cui__plat_fullscreen_strip(app) : 0; }
bool cui_app_fullscreen_peek(cui_app *app, float chrome_h, bool hold) {
    app = cui__app_or_current(app);
    return app && cui__plat_fullscreen_peek(app, chrome_h, hold);
}
void cui_app_fullscreen_end_peek(cui_app *app) { app = cui__app_or_current(app); if (app) cui__plat_fullscreen_end_peek(app); }
bool cui_app_fullscreen_hint(cui_app *app, const char *text) {
    app = cui__app_or_current(app);
    if (!app || !app->fs_hint || !cui__plat_is_fullscreen(app)) return false;
    app->fs_hint = false;
    cui_toast(app->ui, text && *text ? text : "Press F11 to exit full screen", .duration = 2.5f);
    return true;
}
void cui_app_fullscreen_sim_pointer(cui_app *app, bool on, int sx, int sy, int cx, int cy) {
    app = cui__app_or_current(app);
    if (app) cui__plat_fullscreen_sim_pointer(app, on, sx, sy, cx, cy);
}

#ifndef _WIN32
/* Only the Windows backend has these: elsewhere the caret blinks as it always did, every wake draws a frame, and the window
 * is not made fullscreen (cui_app.h). */
float cui__plat_caret_blink(void) { return 5.0f; }
bool cui__plat_frames_on_change(cui_app *app, bool on) { (void)app; (void)on; return false; }
void cui__plat_arm(cui_app *app) { (void)app; }
bool cui__plat_frame_due(cui_app *app) { (void)app; return true; }
void cui__plat_request_frame(cui_app *app) { (void)app; }
long cui__plat_frames_skipped(cui_app *app) { (void)app; return 0; }
bool cui__plat_schedule(cui_app *app, int64_t ms, void (*tick)(void *), void *user) { (void)app; (void)ms; (void)tick; (void)user; return false; }
bool cui__plat_set_fullscreen(cui_app *app, bool on) { (void)app; (void)on; return false; }
bool cui__plat_is_fullscreen(cui_app *app) { (void)app; return false; }
float cui__plat_fullscreen_strip(cui_app *app) { (void)app; return 0; }
bool cui__plat_fullscreen_peek(cui_app *app, float chrome_h, bool hold) { (void)app; (void)chrome_h; (void)hold; return false; }
void cui__plat_fullscreen_end_peek(cui_app *app) { (void)app; }
void cui__plat_fullscreen_sim_pointer(cui_app *app, bool on, int sx, int sy, int cx, int cy) { (void)app; (void)on; (void)sx; (void)sy; (void)cx; (void)cy; }
#endif
