/*
 * CUI app layer: a window, an event loop and a renderer in a few lines.
 *
 * Native backends: Win32 (Windows), Cocoa (macOS), X11 (Linux/BSD).
 * Optional backends: SDL2 and GLFW (Wayland, consoles, anything SDL runs on).
 * Rendering uses OpenGL 3.3 when available and falls back to the built-in
 * software renderer, so apps also run in VMs and on remote desktops. On Windows
 * the first frames are drawn in software while OpenGL starts on another thread,
 * and the window shows with its first frame, so it appears without waiting for
 * the GPU driver.
 *
 * Loop style:
 *
 *     cui_app *app = cui_app_create(&(cui_app_desc){ .title = "Notes" });
 *     while (cui_app_frame(app)) {
 *         cui_ctx *ui = cui_app_ui(app);
 *         ... components ...
 *     }
 *     cui_app_destroy(app);
 *
 * Callback style (also gives smooth live window resizing on Windows/macOS):
 *
 *     static void frame(cui_ctx *ui, void *user) { ... }
 *     int main(void) { return cui_app_run(&(cui_app_desc){ .title = "Notes" }, frame, NULL); }
 *
 * The loop is event driven: when nothing animates and no input arrives the
 * app sleeps and uses no CPU.
 */
#ifndef CUI_APP_H
#define CUI_APP_H

#include "cui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cui_app cui_app;

typedef enum cui_renderer_kind {
    CUI_RENDERER_AUTO = 0,     /* OpenGL, or software when there is no GPU (CPU-drawn OpenGL, X11 and Win32) */
    CUI_RENDERER_OPENGL,
    CUI_RENDERER_SOFTWARE,
    CUI_RENDERER_GPU           /* the platform's GPU renderer, in builds that have it: Direct3D 12 on Windows (CUI_D3D12), Vulkan on Linux (CUI_VULKAN), Metal on macOS (CUI_METAL). Also CUI_RENDERER=gpu, or its name */
} cui_renderer_kind;

/* How the window is shown. */
typedef enum cui_window_state {
    CUI_WINDOW_NORMAL = 0,
    CUI_WINDOW_MAXIMIZED,
    CUI_WINDOW_MINIMIZED
} cui_window_state;

/* Where the window is when it is neither maximized nor minimized, and whether it is maximized:
 * what an app saves when it closes (cui_app_placement) and hands back as cui_app_desc.placement
 * to open where it was. x and y are the top-left corner of the window's content in desktop
 * coordinates, y growing down: pixels on Windows and X11, points on macOS. Keep them only to
 * pass back. width and height are logical points, the units of cui_app_size. state is
 * CUI_WINDOW_NORMAL or CUI_WINDOW_MAXIMIZED; for a minimized window it is the state it comes
 * back in, so a window closed from the taskbar reopens as it was. */
typedef struct cui_window_placement {
    int x, y;
    int width, height;
    cui_window_state state;
    bool has_position;   /* false: the system picks the place (Wayland tells a window nothing about it) */
} cui_window_placement;

typedef struct cui_app_desc {
    const char *title;          /* default "CUI" */
    int width, height;          /* logical size, default 1080 x 720 */
    int min_width, min_height;  /* default 360 x 280 */
    bool fixed_size;            /* not resizable */
    bool continuous;            /* render every frame even when idle (games, video) */
    bool no_vsync;              /* OpenGL: do not wait for the display; frames are capped at 120 Hz or the refresh rate */
    bool hidden;                /* create the window hidden (tests) */
    cui_renderer_kind renderer;
    cui_theme_mode theme;
    cui_accent accent;
    bool follow_system_theme;   /* use the OS light/dark setting where available */
    /* Linux: the app's ID, which is the name of its .desktop file without
     * ".desktop", e.g. "org.example.Notes". The desktop uses it to show the
     * app's name and icon for the window (taskbar, dock, Alt+Tab) and to group
     * its windows. Sets WM_CLASS on X11 and the app_id on Wayland (SDL2, and
     * GLFW 3.4). Read when the window is created; ignored on Windows and
     * macOS. Default: none (the X11 backend then sets WM_CLASS to the title
     * and "CUI"). */
    const char *app_id;
    /* Windows: no system title bar or frame; the app draws its own bar with
     * cui_app_minimize, cui_app_toggle_maximize, cui_app_quit and cui_app_drag_window.
     * The edges still resize, and the shadow, snapping and Alt+Space menu stay.
     * Ignored elsewhere (check cui_app_has_custom_titlebar). */
    bool custom_titlebar;
    /* Windows, with custom_titlebar: tells the system what is under a point of the window, so
     * the empty part of your bar drags and double-clicks like a title bar and the maximize
     * button gets the Windows 11 Snap Layouts flyout. x and y are in logical points, as
     * the cui_app_desc.width, from the window's top left corner. Return one of
     * CUI_WINDOW_HIT_*; CUI_WINDOW_HIT_NONE (0) is the app's own surface, as without a
     * callback. The window edges that resize come first and are not asked.
     *
     * It runs while Windows processes the pointer, between frames, not inside one: answer from
     * the rectangles of the last frame (cui_get_rect, or cui_anchor) and do not build UI.
     * The system handles clicks on the three button codes: it minimizes, maximizes or restores,
     * and closes, so do not act on a click there yourself. CUI still gets the pointer position
     * over them (hover) but no click. Unset: today's behavior, and ignored without
     * custom_titlebar or off Windows. */
    int (*hit_test)(void *user, float x, float y);
    void *hit_test_user;        /* passed to hit_test */
    /* Open the window maximized, as a placement with state CUI_WINDOW_MAXIMIZED does (it is
     * that, without the size and place). A shortcut set to start minimized or maximized still
     * wins on Windows, and a fixed-size window is not maximized. Unset: a normal window at
     * width x height, unless placement says maximized. */
    bool start_maximized;
    /* Windows: take files dragged from Explorer through OLE drag and drop instead of WM_DROPFILES, which also
     * reports them while they are over the window (cui_file_drag_active, cui_input_file_hover) so a tree or
     * dropzone can highlight its target. Falls back to WM_DROPFILES when OLE cannot start. Ignored elsewhere. */
    bool file_hover;
    /* Where the top-left corner of the window's content is created, in the desktop coordinates of
     * cui_window_placement (as cui_app_position reports them), so a window opens in place without a
     * flash at the default spot. Used only when has_pos is set, and only without placement (whose
     * place comes first when it has one). It is a placement with this place and width x height: a
     * place on no screen is left to the system. With custom_titlebar on Windows the content is the
     * whole window, so this is also its outer corner. */
    int x, y;
    bool has_pos;
    /* Where to open, for an app that reopens where it was closed: the size and place from
     * cui_app_placement, and whether the window was maximized. Its size replaces width and
     * height. A place that is on no screen is left to the system, and a fixed-size window is
     * not maximized. Copied when the window is created.
     * Default: none, the system picks the place. */
    const cui_window_placement *placement;
    /* The text caret stops blinking as the system's does: after the system's caret timeout (Windows: SPI_GETCARETTIMEOUT,
     * 5 seconds by default), and never when the system's caret does not blink. A focused field at rest then asks for no
     * frames. The same as calling cui_set_caret_blink(ui, cui_system_caret_blink()) after creating the window. Off:
     * the caret blinks for as long as a field has the focus, as before. */
    bool follow_system_caret;
    /* Software renderer: when the system asks to repaint only a part of the window (Windows: WM_PAINT's rectangle,
     * for example where a child window's region shrank), the next frame copies that part and what changed instead
     * of the whole picture. Helps an app whose child window changes shape while something animates (a browser
     * cutting popovers out of its page). Off: an exposed part makes the next frame copy the whole picture, as
     * before. Windows only; ignored elsewhere and with OpenGL. */
    bool present_exposed_part;
    /* The dark theme's style from the first frame, as cui_set_dark_style: CUI_DARK_BLACK is a true black
     * dark theme. Default gray. CUI_DARK=black (or gray) in the environment overrides it, as CUI_THEME does. */
    cui_dark_style dark_style;
    /* Windows: a window opened at width x height (no saved placement) that would not fit the work area of
     * the monitor it opens on (the screen less the taskbar) is made smaller to fit, never below min_width x
     * min_height, and placed inside it: a 1200 x 780 window on a 1366 x 768 screen at 125% opens at about
     * 1093 x 545. Off: the size is used as given, as before. Ignored elsewhere. */
    bool fit_work_area;
} cui_app_desc;

/* What cui_app_desc.hit_test returns. */
typedef enum cui_window_hit {
    CUI_WINDOW_HIT_NONE = 0,    /* the app's own surface (clicks and hover reach CUI) */
    CUI_WINDOW_HIT_CAPTION,     /* the title bar: drag to move, double-click to maximize, right-click for the system menu */
    CUI_WINDOW_HIT_MIN,         /* the minimize button */
    CUI_WINDOW_HIT_MAX,         /* the maximize / restore button (Snap Layouts on Windows 11) */
    CUI_WINDOW_HIT_CLOSE        /* the close button */
} cui_window_hit;

CUI_API cui_app *cui_app_create(const cui_app_desc *desc);
CUI_API void cui_app_destroy(cui_app *app);

/* Presents the previous frame, waits for input or animation, then begins a
 * new frame. Returns false once the window was closed or cui_app_quit was
 * called. */
CUI_API bool cui_app_frame(cui_app *app);
CUI_API cui_ctx *cui_app_ui(cui_app *app);

/* Several windows on one thread. cui_app_frame blocks, so one thread cannot serve two apps with it;
 * these steps do not. A loop:
 *
 *     cui_app *apps[2] = { a, b };
 *     while (a_open || b_open) {
 *         for (int i = 0; i < 2; i++)
 *             if (apps[i] && cui_app_pending(apps[i])) {
 *                 if (!cui_app_frame_poll(apps[i])) { cui_app_destroy(apps[i]); apps[i] = NULL; continue; }
 *                 ... draw into cui_app_ui(apps[i]) ...
 *             }
 *         cui_app_wait(apps, 2);
 *     }
 *
 * cui_app_frame_poll presents the app's previous frame, takes in the operating system's messages for the
 * thread (every window of it) without waiting, and begins a frame: draw it, as after cui_app_frame, and
 * poll again later. It returns false once the window was closed or cui_app_quit was called (then no frame
 * was begun). NULL-app calls (cui_app_quit(NULL), file dialogs) name the app that was last polled, so
 * they keep working inside the draw code of each window.
 * cui_app_pending says whether an app needs a frame now: input arrived, an animation or redraw is
 * running, a cui_request_wakeup is due, a worker woke the window (Windows: a WM_APP message), the first
 * frame is not out yet, or it was closed. An app that is not pending would draw the same picture again,
 * so skip it; polling it anyway is allowed (you then draw it each time) but costs a frame.
 * cui_app_wait presents the frames that were drawn since the last wait, then sleeps until some app of the
 * list is pending: input, a timer or a wakeup on any window of the thread. It does not poll: an idle
 * set of windows uses no CPU. While an app animates it paces the loop to the display as cui_app_frame does.
 * Secondary windows: the first window of a thread may use OpenGL; a window made on the same thread while
 * another one holds a GL context uses the software renderer (a second GL context per thread is not
 * supported), whatever cui_app_desc.renderer says. Ask for it explicitly with CUI_RENDERER_SOFTWARE to
 * avoid the message an explicit CUI_RENDERER_OPENGL prints. Threads of their own, each with its own
 * apps, work as before.
 * The windows' positions: cui_app_desc.x, .y and .has_pos create one in place, cui_app_set_position
 * moves one and cui_app_position reads it, all in the same frame: the top-left corner of the content
 * in the desktop coordinates of cui_window_placement. */
CUI_API bool cui_app_frame_poll(cui_app *app);
CUI_API bool cui_app_pending(cui_app *app);
CUI_API void cui_app_wait(cui_app *const *apps, int n);
/* Where the top-left corner of the window's content is now, in the desktop coordinates of
 * cui_window_placement, so cui_app_set_position(app, x, y) with what it reads leaves a normal
 * window where it is. A maximized window reports where its content is; a minimized one where
 * it comes back. Returns false, and 0, 0, where windows cannot tell their place (Wayland). */
CUI_API bool cui_app_position(cui_app *app, int *x, int *y);

/* Creates the app, calls frame() until the window closes, destroys it. */
CUI_API int cui_app_run(const cui_app_desc *desc, void (*frame)(cui_ctx *ui, void *user), void *user);

/* The functions below take NULL for "the app that is running", which is how a
 * cui_app_run callback reaches its window: cui_app_quit(NULL). */
/* Ends the loop after the current frame: cui_app_frame returns false, cui_app_run returns. */
CUI_API void cui_app_quit(cui_app *app);
CUI_API void cui_app_set_title(cui_app *app, const char *title);
/* The window's size in logical points (the same units as cui_app_desc.width). */
CUI_API void cui_app_set_size(cui_app *app, int width, int height);
CUI_API cui_vec2 cui_app_size(cui_app *app);
/* Maximizes the window on its display, as the title bar's maximize button does. NULL means the
 * running app, so a cui_app_run callback can call it on its first frame; with the loop form it is
 * fine right after cui_app_create. Nothing happens for a fixed-size window or where the window
 * manager does not support it (an X11 window manager without EWMH). */
CUI_API void cui_app_maximize(cui_app *app);
/* Minimizes the window to the taskbar or the dock. Nothing happens where the window manager
 * does not support it. */
CUI_API void cui_app_minimize(cui_app *app);
/* Undoes a maximize or a minimize: a maximized window gets its normal size and place back, a
 * minimized one comes back as it was before, maximized or not. */
CUI_API void cui_app_restore(cui_app *app);
/* Normal, maximized or minimized, as the system reports it now. On X11 the window manager
 * carries out what cui_app_maximize, cui_app_minimize and cui_app_restore ask for, so the new
 * state can take a few frames to show here. */
CUI_API cui_window_state cui_app_window_state(cui_app *app);
/* Moves the window so the top-left corner of its content is at x, y, in the desktop
 * coordinates of cui_window_placement. Meant for a normal window: on Windows a maximized or
 * minimized one takes the new place when it is restored; elsewhere the window manager decides.
 * Nothing happens where windows cannot place themselves (Wayland). */
CUI_API void cui_app_set_position(cui_app *app, int x, int y);
/* Writes the window's normal size and place, and whether it is or comes back maximized, to
 * out, also while it is maximized or minimized, so an app can save them when it closes.
 * Returns false when there is no app. */
CUI_API bool cui_app_placement(cui_app *app, cui_window_placement *out);
/* Starts or stops following the OS light/dark setting, as cui_app_desc.follow_system_theme
 * does at creation, for a theme switcher with Light, Dark and System. Starting applies the
 * OS setting at once, where the platform reports one. Stopping keeps the current theme, so
 * a cui_set_theme after it lasts. Later OS changes are followed on Windows and macOS. */
CUI_API void cui_app_follow_system_theme(cui_app *app, bool follow);
/* True when the software renderer is drawing (no usable OpenGL, or it was asked for; on
 * Windows also for the first frames of a CUI_RENDERER_AUTO app, until OpenGL is ready). */
CUI_API bool cui_app_is_software(cui_app *app);
/* HWND, NSWindow*, X11 Window (cast to uintptr_t), SDL_Window* or GLFWwindow*. */
CUI_API void *cui_app_native_window(cui_app *app);
/* Name of the active backend: "win32", "cocoa", "x11", "sdl2", "glfw". */
CUI_API const char *cui_app_backend(cui_app *app);

/* Window controls, for an app that draws its own title bar (cui_app_desc.custom_titlebar), with
 * cui_app_minimize above. cui_app_toggle_maximize restores a maximized window and maximizes any
 * other (cui_app_restore or cui_app_maximize); cui_app_is_maximized is cui_app_window_state ==
 * CUI_WINDOW_MAXIMIZED. Both work on every backend. */
CUI_API bool cui_app_has_custom_titlebar(cui_app *app);   /* the option took effect on this platform */
CUI_API void cui_app_toggle_maximize(cui_app *app);
CUI_API bool cui_app_is_maximized(cui_app *app);
/* Starts moving the window with the mouse, as the system title bar does (with snapping).
 * Call it on the frame the left button goes down on your bar's empty space. */
CUI_API void cui_app_drag_window(cui_app *app);

/* Native file dialogs. Blocking; returns true and writes the chosen path
 * (UTF-8) to out when the user confirms. On Linux this uses zenity or
 * kdialog when installed. Pass NULL as app to use the current app (handy in
 * cui_app_run callbacks). On Windows the folder chooser also works when the
 * calling thread already uses a multithreaded COM apartment. */
typedef enum cui_file_dialog_kind { CUI_FILE_OPEN = 0, CUI_FILE_SAVE, CUI_FILE_FOLDER } cui_file_dialog_kind;
typedef struct cui_file_dialog_opts {
    cui_file_dialog_kind kind;
    const char *title;
    const char *filter_name;    /* e.g. "Images" */
    const char *filter;         /* extensions separated by ';', e.g. "png;jpg" */
    const char *default_name;   /* save dialogs: suggested name, optionally with its directory on Windows */
    bool multi;                 /* open dialogs: allow choosing several files (see below) */
} cui_file_dialog_opts;
CUI_API bool cui_app_file_dialog(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap);
/* The same dialog with several files allowed (it sets .multi itself, and only CUI_FILE_OPEN
 * honors it). Returns how many paths were chosen, 0 when cancelled or when they do not fit
 * in cap bytes. The paths are UTF-8 and packed into out, each ending with a NUL, after the
 * last one an extra NUL: "a.txt\0b.txt\0\0". Step through them:
 *     for (const char *p = out; *p; p += strlen(p) + 1) use(p);
 * Calling cui_app_file_dialog with .multi set fills out the same way and returns true, so
 * out also reads as a plain string holding the first path. */
CUI_API int cui_app_file_dialog_multi(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap);

/* Opens a URL or file with the system's default handler. */
CUI_API bool cui_app_open_url(const char *url);

/* Renders the entire current UI into an RGBA buffer (any backend).
 * The buffer can be empty or reused. Returns false when it is too small. */
CUI_API bool cui_app_screenshot(cui_app *app, void *rgba, int width, int height);

/* ---- The caret and idle frames: opt-in costs saved at rest ---- */

/* The value cui_set_caret_blink takes for the system's own caret: seconds the caret blinks after it last moved (Windows:
 * the caret timeout), -1 when the system's caret does not blink (Windows: a caret blink time of INFINITE), 5 where the
 * system cannot tell. Read when called; see also cui_app_desc.follow_system_caret. */
CUI_API float cui_system_caret_blink(void);

/* Draw a frame only when something changed. Without it a message for the window (an embedded engine's pump, an input
 * method, COM's apartment window) wakes the loop and runs a frame that draws the same picture. With it, the loop built
 * on cui_app_frame_poll and cui_app_wait skips such frames:
 *
 *     cui_app_frames_on_change(app, true);          // after the app's own window subclasses (see below)
 *     while (running) {
 *         cui_app_arm(app);                         // forget what came before
 *         cui_app_wait(&app, 1);
 *         if (!cui_app_frame_due(app)) continue;    // nothing changed: sleep again
 *         if (!cui_app_frame_poll(app)) break;
 *         ... draw ...
 *     }
 *
 * A frame is due when CUI wants one (cui_app_pending: input, an animation, a cui_request_wakeup that is due, the first
 * frame), when a message other than a question that changes nothing (WM_SETCURSOR, WM_NCHITTEST, WM_GETOBJECT, WM_GETTEXT,
 * WM_GETTEXTLENGTH, WM_GETICON) reached the window, or when the app said so with cui_app_request_frame (an engine callback
 * or a worker finished: the app's own events that CUI cannot see). Windows only; elsewhere cui_app_frames_on_change
 * returns false and cui_app_frame_due is always true, so the same loop runs unchanged.
 *
 * cui_app_frames_on_change(app, true) subclasses the window and must be called after the app's own subclasses (it
 * must see each message first, also those a later subclass handles). Calling it with false takes the subclass out.
 * Thread: the one that made the window. */
CUI_API bool cui_app_frames_on_change(cui_app *app, bool on);
CUI_API void cui_app_arm(cui_app *app);          /* before cui_app_wait */
CUI_API bool cui_app_frame_due(cui_app *app);    /* after cui_app_wait: whether to draw the next frame */
CUI_API void cui_app_request_frame(cui_app *app);   /* the next cui_app_frame_due is true (call from the UI thread) */
CUI_API long cui_app_frames_skipped(cui_app *app);  /* wakes that brought no frame, for a trace */
/* Runs tick(user) from a timer in ms milliseconds (replacing the one set before, whose tick then never runs; ms < 0:
 * none), between frames and without drawing one, while the loop waits. When another message loop runs (a dialog, the
 * size loop) it asks for a frame instead, which is when the app should run its timers by hand. For deadlines the app
 * keeps itself (the next sleep of a tab, a list refresh) so that nothing runs at rest. One timer per thread. Windows
 * only (returns false elsewhere: use cui_request_wakeup in a frame). Without cui_app_frames_on_change (or cui_app_arm) the
 * timer only wakes the loop and the frame it draws is the app's chance to run its deadlines. */
CUI_API bool cui_app_schedule(cui_app *app, int64_t ms, void (*tick)(void *user), void *user);

/* ---- Fullscreen (Windows) ---- */

/* Borderless fullscreen: the window covers its monitor, over the taskbar, with no caption or frame; leaving puts back what
 * was there (the normal rectangle, maximized, or minimized with the right state to come back to). The change is made
 * between frames (posted to the window), so the size change it causes runs the next frame at the new size: build the
 * chrome (tab strip, toolbar, menu) only while !cui_app_is_fullscreen, and the content fills the window. While fullscreen
 * the window ignores the system's move, size and maximize commands (the whole client area is the app's, HTCLIENT), and
 * minimize and restore work as usual; a minimized window enters fullscreen when restored. Returns false where the
 * platform cannot (other backends: nothing happens, cui_app_is_fullscreen stays false) and while minimized.
 * Call it again with the wanted state each frame if you like: a change already under way is not posted twice.
 *
 * Peek: the top 2 pixels of the window (cui_app_fullscreen_strip) are an edge strip. Reserve it when you cover the window
 * with your own child window (an embedded engine); the pointer on it asks for the chrome. cui_app_fullscreen_peek, called
 * each frame while fullscreen, says whether the chrome should be shown over the content now (slid in): it turns on when
 * the pointer touches the edge strip and off when the pointer is below chrome_h points or outside the window and hold is
 * false (hold: a menu or popover is open, a field has the focus). While it is shown it asks for a frame every 0.1 s and
 * reads the pointer from the system (no mouse messages reach the window over a child window); at rest nothing runs.
 *
 * Hint: cui_app_set_fullscreen(on = true) arms a one-off toast; cui_app_fullscreen_hint shows it ("Press F11 to exit full
 * screen" unless text is given) the first frame the window is fullscreen and returns whether it did. */
CUI_API bool cui_app_set_fullscreen(cui_app *app, bool on);
CUI_API bool cui_app_is_fullscreen(cui_app *app);   /* the window is fullscreen now (the change has been made) */
CUI_API float cui_app_fullscreen_strip(cui_app *app);   /* points of the edge strip, 0 when not fullscreen */
CUI_API bool cui_app_fullscreen_peek(cui_app *app, float chrome_h, bool hold);
CUI_API void cui_app_fullscreen_end_peek(cui_app *app);   /* hide the peeked chrome now (a page took over the window) */
CUI_API bool cui_app_fullscreen_hint(cui_app *app, const char *text);
/* Tests: the pointer is at this screen position from now on (on = true), or the real pointer again (on = false); a
 * WM_MOUSEMOVE at the client position (cx, cy) is sent as the system does when the pointer crosses the edge strip. */
CUI_API void cui_app_fullscreen_sim_pointer(cui_app *app, bool on, int screen_x, int screen_y, int client_x, int client_y);

#ifdef __cplusplus
}
#endif

#endif /* CUI_APP_H */
