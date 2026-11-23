/*
 * CUI app layer: a window, an event loop and a renderer in a few lines.
 *
 * Native backends: Win32 (Windows), Cocoa (macOS), X11 (Linux/BSD).
 * Optional backends: SDL2 and GLFW (Wayland, consoles, anything SDL runs on).
 * Rendering uses OpenGL 3.3 when available and falls back to the built-in
 * software renderer, so apps also run in VMs and on remote desktops.
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
    /* Where to open, for an app that reopens where it was closed: the size and place from
     * cui_app_placement, and whether the window was maximized. Its size replaces width and
     * height. A place that is on no screen is left to the system, and a fixed-size window is
     * not maximized. Copied when the window is created.
     * Default: none, the system picks the place. */
    const cui_window_placement *placement;
} cui_app_desc;

CUI_API cui_app *cui_app_create(const cui_app_desc *desc);
CUI_API void cui_app_destroy(cui_app *app);

/* Presents the previous frame, waits for input or animation, then begins a
 * new frame. Returns false once the window was closed or cui_app_quit was
 * called. */
CUI_API bool cui_app_frame(cui_app *app);
CUI_API cui_ctx *cui_app_ui(cui_app *app);

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
/* True when the software renderer is drawing (no usable OpenGL, or it was asked for). */
CUI_API bool cui_app_is_software(cui_app *app);
/* HWND, NSWindow*, X11 Window (cast to uintptr_t), SDL_Window* or GLFWwindow*. */
CUI_API void *cui_app_native_window(cui_app *app);
/* Name of the active backend: "win32", "cocoa", "x11", "sdl2", "glfw". */
CUI_API const char *cui_app_backend(cui_app *app);

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
} cui_file_dialog_opts;
CUI_API bool cui_app_file_dialog(cui_app *app, const cui_file_dialog_opts *opts, char *out, size_t cap);

/* Opens a URL or file with the system's default handler. */
CUI_API bool cui_app_open_url(const char *url);

/* Renders the entire current UI into an RGBA buffer (any backend).
 * The buffer can be empty or reused. Returns false when it is too small. */
CUI_API bool cui_app_screenshot(cui_app *app, void *rgba, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* CUI_APP_H */
