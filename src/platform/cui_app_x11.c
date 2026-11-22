/* X11 backend (Linux, BSD). Uses Xlib for windows and input, XIM for text
 * input with the candidate window at the caret, GLX (loaded at runtime) for
 * OpenGL and XPutImage for the software renderer. Links only against libX11. */
#include "cui_platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/Xresource.h>
#include <dlfcn.h>
#include <locale.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

typedef struct __GLXFBConfigRec *GLXFBConfig_;
typedef struct __GLXcontextRec *GLXContext_;
typedef XID GLXDrawable_;

typedef GLXFBConfig_ *(*PFN_glXChooseFBConfig)(Display *, int, const int *, int *);
typedef XVisualInfo *(*PFN_glXGetVisualFromFBConfig)(Display *, GLXFBConfig_);
typedef void *(*PFN_glXGetProcAddress)(const unsigned char *);
typedef int (*PFN_glXMakeCurrent)(Display *, GLXDrawable_, GLXContext_);
typedef void (*PFN_glXSwapBuffers)(Display *, GLXDrawable_);
typedef void (*PFN_glXDestroyContext)(Display *, GLXContext_);
typedef GLXContext_ (*PFN_glXCreateContextAttribsARB)(Display *, GLXFBConfig_, GLXContext_, int, const int *);
typedef void (*PFN_glXSwapIntervalEXT)(Display *, GLXDrawable_, int);
typedef int (*PFN_glXSwapIntervalMESA)(unsigned int);
typedef Bool (*PFN_glXGetMscRateOML)(Display *, GLXDrawable_, int32_t *, int32_t *);
typedef const char *(*PFN_glXQueryExtensionsString)(Display *, int);

#define GLX_X_RENDERABLE_ 0x8012
#define GLX_DRAWABLE_TYPE_ 0x8010
#define GLX_WINDOW_BIT_ 0x0001
#define GLX_RENDER_TYPE_ 0x8011
#define GLX_RGBA_BIT_ 0x0001
#define GLX_X_VISUAL_TYPE_ 0x22
#define GLX_TRUE_COLOR_ 0x8002
#define GLX_RED_SIZE_ 8
#define GLX_GREEN_SIZE_ 9
#define GLX_BLUE_SIZE_ 10
#define GLX_ALPHA_SIZE_ 11
#define GLX_DOUBLEBUFFER_ 5
#define GLX_CONTEXT_MAJOR_VERSION_ARB_ 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB_ 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB_ 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB_ 0x0001
#define GLX_CONTEXT_FLAGS_ARB_ 0x2094
#define GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB_ 0x0002

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    Colormap cmap;
    Visual *visual;
    int depth;
    XIM im;
    XIC ic;
    XFontSet ic_fontset;       /* core font set; XIMPreeditPosition needs one for the preedit */
    XVaNestedList ime_attrs;   /* XNSpotLocation (+ XNFontSet) handed to the IC, updated in place */
    XPoint ime_spot;           /* caret, in window pixels */
    bool ime_spot_ok;          /* the IC takes a spot, so the IME opens at the caret */
    bool ime_spot_set;
    unsigned long event_mask;
    Atom wm_delete, wm_protocols, net_wm_name, utf8_string, clipboard, targets, cui_sel, net_wm_icon_name;
    Atom wm_state, net_wm_state, net_wm_maximized_horz, net_wm_maximized_vert, net_wm_hidden, net_active_window;
    Cursor cursors[CUI_CURSOR_COUNT];
    cui_cursor current_cursor;
    int width, height;     /* pixels */
    float scale;
    GC gc;
    XImage *image;
    /* GLX */
    void *libgl;
    GLXContext_ ctx;
    GLXFBConfig_ fbc;
    PFN_glXGetProcAddress GetProcAddress;
    PFN_glXMakeCurrent MakeCurrent;
    PFN_glXSwapBuffers SwapBuffers;
    PFN_glXDestroyContext DestroyContext;
    PFN_glXGetMscRateOML GetMscRate;   /* refresh rate, when GLX_OML_sync_control is there */
    /* clipboard */
    char *clip_owned;
    char *clip_read;
    bool mapped;
    /* X11 keeps no normal size and place for a maximized window: noted while it is normal */
    int normal_x, normal_y, normal_w, normal_h;   /* content origin on the root window, size in pixels */
    bool normal_pos_known;
    bool geometry_changed;                        /* a ConfigureNotify came: note them after the batch */
    /* drag and drop (XDND) */
    Atom xdnd_aware, xdnd_enter, xdnd_position, xdnd_status, xdnd_leave, xdnd_drop, xdnd_finished,
         xdnd_selection, xdnd_action_copy, uri_list, cui_xdnd;
    Window xdnd_source;
    int xdnd_version;
    float xdnd_x, xdnd_y;
} cui__x11;

static cui__x11 *cui__x(cui_app *app) { return app->plat; }
static void *cui__x11_libgl;
static PFN_glXGetProcAddress cui__x11_getproc;

double cui__plat_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char *cui__plat_name(void) { return "x11"; }

int cui__plat_system_dark(void) {
    const char *gtk = getenv("GTK_THEME");
    if (gtk && strstr(gtk, "dark")) return 1;
    return -1;
}

static float cui__x11_dpi_scale(Display *dpy) {
    const char *env = getenv("CUI_SCALE");
    if (env) { float s = (float)atof(env); if (s > 0.25f && s < 8) return s; }
    char *rms = XResourceManagerString(dpy);
    float scale = 1.0f;
    if (rms) {
        XrmInitialize();
        XrmDatabase db = XrmGetStringDatabase(rms);
        if (db) {
            char *type = NULL;
            XrmValue val;
            if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &val) && val.addr) {
                float dpi = (float)atof(val.addr);
                if (dpi > 0) scale = dpi / 96.0f;
            }
            XrmDestroyDatabase(db);
        }
    }
    if (scale < 1.0f) scale = 1.0f;
    return roundf(scale * 4) / 4;
}

static const char *cui__x11_get_clip(void *user);
static void cui__x11_set_clip(void *user, const char *utf8);
static void cui__x11_set_cursor(void *user, cui_cursor c);
static void cui__x11_text_input_rect(void *user, cui_rect caret);

static bool cui__x11_load_glx(cui__x11 *x) {
    if (!cui__x11_libgl) cui__x11_libgl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!cui__x11_libgl) cui__x11_libgl = dlopen("libGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!cui__x11_libgl) return false;
    x->libgl = cui__x11_libgl;
    cui__x11_getproc = (PFN_glXGetProcAddress)dlsym(x->libgl, "glXGetProcAddressARB");
    x->GetProcAddress = cui__x11_getproc;
    x->MakeCurrent = (PFN_glXMakeCurrent)dlsym(x->libgl, "glXMakeCurrent");
    x->SwapBuffers = (PFN_glXSwapBuffers)dlsym(x->libgl, "glXSwapBuffers");
    x->DestroyContext = (PFN_glXDestroyContext)dlsym(x->libgl, "glXDestroyContext");
    return x->GetProcAddress && x->MakeCurrent && x->SwapBuffers && x->DestroyContext;
}

/* Input styles to ask for, best first. XIMPreeditPosition is the only one that
 * gives the input method a spot (XNSpotLocation) to open its candidate list and
 * its preedit at; XIMPreeditNothing leaves it guessing. A spot needs an
 * XFontSet, which XIM uses to draw the preedit: CUI draws its own text, so a
 * font set of the server's default font is enough. Status is never drawn by
 * CUI. The best style without a spot comes back through `fallback`; the return
 * value is 0 when the input method lists no style at all. */
static unsigned long cui__x11_style(XIM im, bool have_fontset, unsigned long *fallback) {
    unsigned long want[4];
    int count = 0;
    if (have_fontset) {
        want[count++] = XIMPreeditPosition | XIMStatusNothing;
        want[count++] = XIMPreeditPosition | XIMStatusNone;
    }
    want[count++] = XIMPreeditNothing | XIMStatusNothing;
    want[count++] = XIMPreeditNothing | XIMStatusNone;

    unsigned long style = 0, plain = 0;
    XIMStyles *styles = NULL;
    if (XGetIMValues(im, XNQueryInputStyle, &styles, NULL) != NULL || !styles) return 0;
    for (int w = 0; w < count && !style; w++)
        for (int i = 0; i < styles->count_styles && !style; i++)
            if (styles->supported_styles[i] == want[w]) style = want[w];
    for (int w = 0; w < count && !plain; w++) {
        if (want[w] & XIMPreeditPosition) continue;
        for (int i = 0; i < styles->count_styles && !plain; i++)
            if (styles->supported_styles[i] == want[w]) plain = want[w];
    }
    XFree(styles);
    *fallback = plain;
    return style;
}

/* An XIC for the text fields. With a spot the input method is told where the
 * caret is whenever it moves (see set_text_input_rect below), so the candidate
 * window opens there instead of at the pointer. */
static XIC cui__x11_create_ic(cui__x11 *x) {
    unsigned long plain = 0;
    unsigned long style = cui__x11_style(x->im, x->ic_fontset != NULL, &plain);
    if (style & XIMPreeditPosition) {
        x->ime_attrs = XVaCreateNestedList(0, XNSpotLocation, &x->ime_spot, XNFontSet, x->ic_fontset, NULL);
        XIC ic = XCreateIC(x->im, XNInputStyle, style, XNClientWindow, x->win, XNFocusWindow, x->win,
                           XNPreeditAttributes, x->ime_attrs, NULL);
        if (ic) { x->ime_spot_ok = true; return ic; }
        /* the input method offered a spot but would not create the IC: old style */
        XFree(x->ime_attrs);
        x->ime_attrs = NULL;
        style = plain;
    }
    if (!style) style = XIMPreeditNothing | XIMStatusNothing;   /* the input method is silent; try the old default */
    return XCreateIC(x->im, XNInputStyle, style, XNClientWindow, x->win, XNFocusWindow, x->win, NULL);
}

bool cui__plat_init(cui_app *app) {
    cui__x11 *x = calloc(1, sizeof *x);
    if (!x) return false;
    app->plat = x;
    setlocale(LC_CTYPE, "");
    XInitThreads();
    x->dpy = XOpenDisplay(NULL);
    if (!x->dpy) { free(x); app->plat = NULL; return false; }
    x->screen = DefaultScreen(x->dpy);
    x->scale = cui__x11_dpi_scale(x->dpy);
    Window root = RootWindow(x->dpy, x->screen);
    x->visual = DefaultVisual(x->dpy, x->screen);
    x->depth = DefaultDepth(x->dpy, x->screen);
    bool want_gl = app->desc.renderer != CUI_RENDERER_SOFTWARE && !(getenv("CUI_RENDERER") && strcmp(getenv("CUI_RENDERER"), "software") == 0);
    if (want_gl && cui__x11_load_glx(x)) {
        PFN_glXChooseFBConfig choose = (PFN_glXChooseFBConfig)dlsym(x->libgl, "glXChooseFBConfig");
        PFN_glXGetVisualFromFBConfig getvis = (PFN_glXGetVisualFromFBConfig)dlsym(x->libgl, "glXGetVisualFromFBConfig");
        if (choose && getvis) {
            int attrs[] = { GLX_X_RENDERABLE_, True, GLX_DRAWABLE_TYPE_, GLX_WINDOW_BIT_, GLX_RENDER_TYPE_, GLX_RGBA_BIT_,
                            GLX_X_VISUAL_TYPE_, GLX_TRUE_COLOR_, GLX_RED_SIZE_, 8, GLX_GREEN_SIZE_, 8, GLX_BLUE_SIZE_, 8,
                            GLX_ALPHA_SIZE_, 8, GLX_DOUBLEBUFFER_, True, None };
            int n = 0;
            GLXFBConfig_ *cfgs = choose(x->dpy, x->screen, attrs, &n);
            if (cfgs && n > 0) {
                XVisualInfo *vi = getvis(x->dpy, cfgs[0]);
                if (vi) {
                    x->fbc = cfgs[0];
                    x->visual = vi->visual;
                    x->depth = vi->depth;
                    XFree(vi);
                }
            }
            if (cfgs) XFree(cfgs);
        }
    }
    x->cmap = XCreateColormap(x->dpy, root, x->visual, AllocNone);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof swa);
    swa.colormap = x->cmap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    cui_color bg = app->ui->theme.colors[CUI_TOKEN_BACKGROUND];
    swa.background_pixel = ((unsigned long)bg.r << 16) | ((unsigned long)bg.g << 8) | bg.b;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                     PointerMotionMask | EnterWindowMask | LeaveWindowMask | StructureNotifyMask | FocusChangeMask;
    x->event_mask = swa.event_mask;
    x->width = (int)(app->desc.width * x->scale);
    x->height = (int)(app->desc.height * x->scale);
    x->normal_w = x->width;
    x->normal_h = x->height;
    const cui_window_placement *pl = app->desc.placement;
    if (pl && pl->has_position && pl->y >= 0 && pl->y < DisplayHeight(x->dpy, x->screen) &&
        pl->x < DisplayWidth(x->dpy, x->screen) && pl->x + x->width > 0) {
        x->normal_x = pl->x;   /* on the screen: a place on a monitor that was unplugged is left to the window manager */
        x->normal_y = pl->y;
        x->normal_pos_known = true;
    }
    x->win = XCreateWindow(x->dpy, root, x->normal_x, x->normal_y, (unsigned)x->width, (unsigned)x->height, 0, x->depth, InputOutput,
                           x->visual, CWColormap | CWBorderPixel | CWBackPixel | CWEventMask, &swa);
    if (!x->win) { XCloseDisplay(x->dpy); free(x); app->plat = NULL; return false; }
    x->wm_protocols = XInternAtom(x->dpy, "WM_PROTOCOLS", False);
    x->wm_delete = XInternAtom(x->dpy, "WM_DELETE_WINDOW", False);
    x->net_wm_name = XInternAtom(x->dpy, "_NET_WM_NAME", False);
    x->net_wm_icon_name = XInternAtom(x->dpy, "_NET_WM_ICON_NAME", False);
    x->utf8_string = XInternAtom(x->dpy, "UTF8_STRING", False);
    x->clipboard = XInternAtom(x->dpy, "CLIPBOARD", False);
    x->targets = XInternAtom(x->dpy, "TARGETS", False);
    x->cui_sel = XInternAtom(x->dpy, "CUI_SELECTION", False);
    x->wm_state = XInternAtom(x->dpy, "WM_STATE", False);
    x->net_wm_state = XInternAtom(x->dpy, "_NET_WM_STATE", False);
    x->net_wm_maximized_horz = XInternAtom(x->dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    x->net_wm_maximized_vert = XInternAtom(x->dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    x->net_wm_hidden = XInternAtom(x->dpy, "_NET_WM_STATE_HIDDEN", False);
    x->net_active_window = XInternAtom(x->dpy, "_NET_ACTIVE_WINDOW", False);
    XSetWMProtocols(x->dpy, x->win, &x->wm_delete, 1);
    /* accept files dragged from file managers (XDND version 5) */
    x->xdnd_aware = XInternAtom(x->dpy, "XdndAware", False);
    x->xdnd_enter = XInternAtom(x->dpy, "XdndEnter", False);
    x->xdnd_position = XInternAtom(x->dpy, "XdndPosition", False);
    x->xdnd_status = XInternAtom(x->dpy, "XdndStatus", False);
    x->xdnd_leave = XInternAtom(x->dpy, "XdndLeave", False);
    x->xdnd_drop = XInternAtom(x->dpy, "XdndDrop", False);
    x->xdnd_finished = XInternAtom(x->dpy, "XdndFinished", False);
    x->xdnd_selection = XInternAtom(x->dpy, "XdndSelection", False);
    x->xdnd_action_copy = XInternAtom(x->dpy, "XdndActionCopy", False);
    x->uri_list = XInternAtom(x->dpy, "text/uri-list", False);
    x->cui_xdnd = XInternAtom(x->dpy, "CUI_XDND", False);
    {
        Atom version = 5;
        XChangeProperty(x->dpy, x->win, x->xdnd_aware, XA_ATOM, 32, PropModeReplace, (unsigned char *)&version, 1);
    }
    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        /* static gravity: positions name the content's corner, not the window manager's frame */
        hints->flags = PMinSize | PWinGravity;
        hints->win_gravity = StaticGravity;
        if (x->normal_pos_known) {
            hints->flags |= USPosition;
            hints->x = x->normal_x;
            hints->y = x->normal_y;
        }
        hints->min_width = (int)(app->desc.min_width * x->scale);
        hints->min_height = (int)(app->desc.min_height * x->scale);
        if (app->desc.fixed_size) {
            hints->flags |= PMaxSize;
            hints->min_width = hints->max_width = x->width;
            hints->min_height = hints->max_height = x->height;
        }
        XSetWMNormalHints(x->dpy, x->win, hints);
        XFree(hints);
    }
    /* Desktops find the window's .desktop file (name, icon, dock entry) by
     * matching WM_CLASS against its file name, so both parts carry the app ID */
    XClassHint *ch = XAllocClassHint();
    if (ch) {
        ch->res_name = (char *)(app->desc.app_id ? app->desc.app_id : app->title);
        ch->res_class = (char *)(app->desc.app_id ? app->desc.app_id : "CUI");
        XSetClassHint(x->dpy, x->win, ch);
        XFree(ch);
    }
    cui__plat_set_title(app, app->title);
    /* text input */
    XSetLocaleModifiers("");
    x->im = XOpenIM(x->dpy, NULL, NULL, NULL);
    if (!x->im) { XSetLocaleModifiers("@im=none"); x->im = XOpenIM(x->dpy, NULL, NULL, NULL); }
    if (x->im) {
        char **missing = NULL;
        int missing_count = 0;
        char *missing_default = NULL;
        x->ic_fontset = XCreateFontSet(x->dpy, "-*-*-*-*-*-*-*-*-*-*-*-*-*", &missing, &missing_count, &missing_default);
        if (missing) XFreeStringList(missing);
        x->ic = cui__x11_create_ic(x);
        if (x->ic) {
            /* events the input method wants while it composes, on top of ours */
            unsigned long filter = 0;
            if (XGetICValues(x->ic, XNFilterEvents, &filter, NULL) == NULL && filter)
                XSelectInput(x->dpy, x->win, x->event_mask | filter);
        }
    }
    unsigned int shapes[CUI_CURSOR_COUNT] = { XC_left_ptr, XC_hand2, XC_xterm, XC_sb_h_double_arrow, XC_sb_v_double_arrow, XC_fleur, XC_X_cursor };
    for (int i = 0; i < CUI_CURSOR_COUNT; i++) x->cursors[i] = XCreateFontCursor(x->dpy, shapes[i]);
    x->gc = XCreateGC(x->dpy, x->win, 0, NULL);
    cui_platform_iface pi;
    memset(&pi, 0, sizeof pi);
    pi.user = app;
    pi.get_clipboard = cui__x11_get_clip;
    pi.set_clipboard = cui__x11_set_clip;
    pi.set_cursor = cui__x11_set_cursor;
    pi.set_text_input_rect = cui__x11_text_input_rect;
    cui_set_platform(app->ui, &pi);
    return true;
}

/* True while an EWMH window manager runs: it names its check window on the root. */
static bool cui__x11_has_ewmh(cui__x11 *x) {
    Atom check = XInternAtom(x->dpy, "_NET_SUPPORTING_WM_CHECK", False), type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    bool found = XGetWindowProperty(x->dpy, RootWindow(x->dpy, x->screen), check, 0, 1, False, XA_WINDOW, &type, &format,
                                    &count, &after, &data) == Success && data && count == 1;
    if (data) XFree(data);
    return found;
}

void cui__plat_show(cui_app *app) {
    cui__x11 *x = cui__x(app);
    const cui_window_placement *pl = app->desc.placement;
    if (pl && pl->state == CUI_WINDOW_MAXIMIZED && cui__x11_has_ewmh(x)) {
        /* EWMH: a window that is not mapped yet asks for its state with the property. Without a
         * window manager to carry it out, the property would only claim a maximized window. */
        Atom maximized[2] = { x->net_wm_maximized_horz, x->net_wm_maximized_vert };
        XChangeProperty(x->dpy, x->win, x->net_wm_state, XA_ATOM, 32, PropModeReplace, (unsigned char *)maximized, 2);
    }
    XMapWindow(x->dpy, x->win);
    XFlush(x->dpy);
    x->mapped = true;
}

void cui__plat_shutdown(cui_app *app) {
    cui__x11 *x = cui__x(app);
    if (!x) return;
    if (x->ctx) { x->MakeCurrent(x->dpy, None, NULL); x->DestroyContext(x->dpy, x->ctx); }
    if (x->image) { x->image->data = NULL; XDestroyImage(x->image); }
    if (x->ime_attrs) XFree(x->ime_attrs);
    if (x->ic) XDestroyIC(x->ic);
    if (x->im) XCloseIM(x->im);
    if (x->ic_fontset) XFreeFontSet(x->dpy, x->ic_fontset);
    for (int i = 0; i < CUI_CURSOR_COUNT; i++) if (x->cursors[i]) XFreeCursor(x->dpy, x->cursors[i]);
    if (x->gc) XFreeGC(x->dpy, x->gc);
    XDestroyWindow(x->dpy, x->win);
    XFreeColormap(x->dpy, x->cmap);
    XCloseDisplay(x->dpy);
    free(x->clip_owned);
    free(x->clip_read);
    free(x);
    app->plat = NULL;
}

static int cui__x11_err_flag;
static int cui__x11_error_handler(Display *d, XErrorEvent *e) { (void)d; (void)e; cui__x11_err_flag = 1; return 0; }

bool cui__plat_gl_create(cui_app *app, bool vsync) {
    cui__x11 *x = cui__x(app);
    if (!x->libgl || !x->fbc) return false;
    PFN_glXCreateContextAttribsARB create = (PFN_glXCreateContextAttribsARB)x->GetProcAddress((const unsigned char *)"glXCreateContextAttribsARB");
    if (!create) return false;
    int attrs[] = { GLX_CONTEXT_MAJOR_VERSION_ARB_, 3, GLX_CONTEXT_MINOR_VERSION_ARB_, 3,
                    GLX_CONTEXT_PROFILE_MASK_ARB_, GLX_CONTEXT_CORE_PROFILE_BIT_ARB_,
                    GLX_CONTEXT_FLAGS_ARB_, GLX_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB_, None };
    int (*old)(Display *, XErrorEvent *) = XSetErrorHandler(cui__x11_error_handler);
    cui__x11_err_flag = 0;
    x->ctx = create(x->dpy, x->fbc, NULL, True, attrs);
    XSync(x->dpy, False);
    XSetErrorHandler(old);
    if (!x->ctx || cui__x11_err_flag) { x->ctx = NULL; return false; }
    if (!x->MakeCurrent(x->dpy, x->win, x->ctx)) { x->DestroyContext(x->dpy, x->ctx); x->ctx = NULL; return false; }
    PFN_glXSwapIntervalEXT sie = (PFN_glXSwapIntervalEXT)x->GetProcAddress((const unsigned char *)"glXSwapIntervalEXT");
    PFN_glXSwapIntervalMESA sim = (PFN_glXSwapIntervalMESA)x->GetProcAddress((const unsigned char *)"glXSwapIntervalMESA");
    if (sie) sie(x->dpy, x->win, vsync ? 1 : 0);
    else if (sim) sim(vsync ? 1 : 0);
    PFN_glXQueryExtensionsString qes = (PFN_glXQueryExtensionsString)dlsym(x->libgl, "glXQueryExtensionsString");
    const char *ext = qes ? qes(x->dpy, DefaultScreen(x->dpy)) : NULL;
    if (ext && strstr(ext, "GLX_OML_sync_control"))
        x->GetMscRate = (PFN_glXGetMscRateOML)x->GetProcAddress((const unsigned char *)"glXGetMscRateOML");
    return true;
}

void *cui__plat_gl_proc(const char *name) {
    void *p = NULL;
    if (cui__x11_getproc) p = cui__x11_getproc((const unsigned char *)name);
    if (!p && cui__x11_libgl) p = dlsym(cui__x11_libgl, name);
    return p;
}

double cui__plat_refresh_hz(cui_app *app) {
    cui__x11 *x = cui__x(app);
    int32_t num = 0, den = 0;
    if (x->ctx && x->GetMscRate && x->GetMscRate(x->dpy, x->win, &num, &den) && den > 0) return (double)num / den;
    return 0;
}

void cui__plat_gl_release(cui_app *app) {
    cui__x11 *x = cui__x(app);
    x->GetMscRate = NULL;
    if (x->ctx) { x->MakeCurrent(x->dpy, None, NULL); x->DestroyContext(x->dpy, x->ctx); x->ctx = NULL; }
}

void cui__plat_gl_swap(cui_app *app) {
    cui__x11 *x = cui__x(app);
    if (x->ctx) x->SwapBuffers(x->dpy, x->win);
}

void cui__plat_present(cui_app *app, const void *bgra, int w, int h, int stride) {
    cui__x11 *x = cui__x(app);
    if (!x->image || x->image->width != w || x->image->height != h) {
        if (x->image) { x->image->data = NULL; XDestroyImage(x->image); }
        x->image = XCreateImage(x->dpy, x->visual, (unsigned)x->depth, ZPixmap, 0, NULL, (unsigned)w, (unsigned)h, 32, stride);
    }
    if (!x->image) return;
    x->image->data = (char *)bgra;
    XPutImage(x->dpy, x->win, x->gc, x->image, 0, 0, 0, 0, (unsigned)w, (unsigned)h);
    XFlush(x->dpy);
}

bool cui__plat_present_part(cui_app *app, const void *bgra, int w, int h, int stride, int px, int py, int pw, int ph) {
    cui__x11 *x = cui__x(app);
    if (!x->image || x->image->width != w || x->image->height != h) return false;
    x->image->data = (char *)bgra;
    (void)stride;
    XPutImage(x->dpy, x->win, x->gc, x->image, px, py, px, py, (unsigned)pw, (unsigned)ph);
    XFlush(x->dpy);
    return true;
}

void cui__plat_metrics(cui_app *app, float *w, float *h, float *scale, int *fb_w, int *fb_h) {
    cui__x11 *x = cui__x(app);
    *scale = x->scale;
    *fb_w = x->width; *fb_h = x->height;
    *w = (float)x->width / x->scale;
    *h = (float)x->height / x->scale;
}

void cui__plat_set_title(cui_app *app, const char *title) {
    cui__x11 *x = cui__x(app);
    XStoreName(x->dpy, x->win, title);
    XChangeProperty(x->dpy, x->win, x->net_wm_name, x->utf8_string, 8, PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XChangeProperty(x->dpy, x->win, x->net_wm_icon_name, x->utf8_string, 8, PropModeReplace, (const unsigned char *)title, (int)strlen(title));
    XFlush(x->dpy);
}

/* window positions are not implemented here yet (cui_app_position returns false) */

void cui__plat_set_size(cui_app *app, int w, int h) {
    cui__x11 *x = cui__x(app);
    XResizeWindow(x->dpy, x->win, (unsigned)(w * x->scale), (unsigned)(h * x->scale));
    XFlush(x->dpy);
}

void *cui__plat_native(cui_app *app) { return (void *)(uintptr_t)cui__x(app)->win; }
bool cui__plat_window_op(cui_app *app, int op) { (void)app; (void)op; return false; }   /* system title bar only */

/* Where the content's top-left corner is on the root window. */
static void cui__x11_content_origin(cui__x11 *x, int *px, int *py) {
    Window child;
    if (!XTranslateCoordinates(x->dpy, x->win, RootWindow(x->dpy, x->screen), 0, 0, px, py, &child)) *px = *py = 0;
}

static void cui__x11_set_cursor(void *user, cui_cursor c) {
    cui_app *app = user;
    cui__x11 *x = cui__x(app);
    if (c == x->current_cursor || (int)c < 0 || c >= CUI_CURSOR_COUNT) return;
    x->current_cursor = c;
    XDefineCursor(x->dpy, x->win, x->cursors[c]);
    XFlush(x->dpy);
}

/* CUI calls this every frame a text field has focus, with the caret in window
 * points; the input method hears about it only when the caret moved. XIM takes
 * the spot in window pixels, as xterm and the other XIM clients send it, and
 * opens the candidate list under it. */
static void cui__x11_text_input_rect(void *user, cui_rect caret) {
    cui_app *app = user;
    cui__x11 *x = cui__x(app);
    if (!x || !x->ic || !x->ime_spot_ok) return;
    float s = x->scale;
    XPoint spot;
    spot.x = (short)(caret.x * s + 0.5f);
    spot.y = (short)((caret.y + caret.h) * s + 0.5f);   /* bottom of the line */
    if (x->ime_spot_set && spot.x == x->ime_spot.x && spot.y == x->ime_spot.y) return;
    x->ime_spot = spot;
    x->ime_spot_set = true;
    XSetICValues(x->ic, XNPreeditAttributes, x->ime_attrs, NULL);
}

static void cui__x11_set_clip(void *user, const char *utf8) {
    cui_app *app = user;
    cui__x11 *x = cui__x(app);
    free(x->clip_owned);
    size_t n = strlen(utf8);
    x->clip_owned = malloc(n + 1);
    if (x->clip_owned) memcpy(x->clip_owned, utf8, n + 1);
    XSetSelectionOwner(x->dpy, x->clipboard, x->win, CurrentTime);
    XFlush(x->dpy);
}

static void cui__x11_handle_selection_request(cui__x11 *x, XSelectionRequestEvent *req) {
    XSelectionEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SelectionNotify;
    ev.requestor = req->requestor;
    ev.selection = req->selection;
    ev.target = req->target;
    ev.time = req->time;
    ev.property = None;
    if (x->clip_owned) {
        if (req->target == x->targets) {
            Atom list[3] = { x->targets, x->utf8_string, XA_STRING };
            XChangeProperty(x->dpy, req->requestor, req->property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
            ev.property = req->property;
        } else if (req->target == x->utf8_string || req->target == XA_STRING) {
            XChangeProperty(x->dpy, req->requestor, req->property, req->target, 8, PropModeReplace,
                            (unsigned char *)x->clip_owned, (int)strlen(x->clip_owned));
            ev.property = req->property;
        }
    }
    XSendEvent(x->dpy, req->requestor, False, 0, (XEvent *)&ev);
    XFlush(x->dpy);
}

static const char *cui__x11_get_clip(void *user) {
    cui_app *app = user;
    cui__x11 *x = cui__x(app);
    Window owner = XGetSelectionOwner(x->dpy, x->clipboard);
    if (owner == x->win) return x->clip_owned;
    if (owner == None) return NULL;
    XConvertSelection(x->dpy, x->clipboard, x->utf8_string, x->cui_sel, x->win, CurrentTime);
    XFlush(x->dpy);
    double start = cui__plat_now();
    while (cui__plat_now() - start < 0.5) {
        XEvent ev;
        if (XCheckTypedWindowEvent(x->dpy, x->win, SelectionNotify, &ev)) {
            if (ev.xselection.property == None) return NULL;
            Atom type;
            int fmt;
            unsigned long n, rest;
            unsigned char *data = NULL;
            XGetWindowProperty(x->dpy, x->win, x->cui_sel, 0, 1 << 24, True, AnyPropertyType, &type, &fmt, &n, &rest, &data);
            free(x->clip_read);
            x->clip_read = NULL;
            if (data) {
                x->clip_read = malloc(n + 1);
                if (x->clip_read) { memcpy(x->clip_read, data, n); x->clip_read[n] = 0; }
                XFree(data);
            }
            return x->clip_read;
        }
        if (XCheckTypedWindowEvent(x->dpy, x->win, SelectionRequest, &ev)) cui__x11_handle_selection_request(x, &ev.xselectionrequest);
        { struct timespec ts = { 0, 1000000 }; nanosleep(&ts, NULL); }
    }
    return NULL;
}

static cui_key cui__x11_key(KeySym ks) {
    if (ks >= XK_a && ks <= XK_z) return (cui_key)(CUI_KEY_A + (int)(ks - XK_a));
    if (ks >= XK_A && ks <= XK_Z) return (cui_key)(CUI_KEY_A + (int)(ks - XK_A));
    if (ks >= XK_0 && ks <= XK_9) return (cui_key)(CUI_KEY_0 + (int)(ks - XK_0));
    if (ks >= XK_F1 && ks <= XK_F12) return (cui_key)(CUI_KEY_F1 + (int)(ks - XK_F1));
    switch (ks) {
    case XK_Tab: case XK_ISO_Left_Tab: return CUI_KEY_TAB;
    case XK_Return: case XK_KP_Enter: return CUI_KEY_ENTER;
    case XK_Escape: return CUI_KEY_ESCAPE;
    case XK_BackSpace: return CUI_KEY_BACKSPACE;
    case XK_Delete: case XK_KP_Delete: return CUI_KEY_DELETE;
    case XK_space: return CUI_KEY_SPACE;
    case XK_Left: case XK_KP_Left: return CUI_KEY_LEFT;
    case XK_Right: case XK_KP_Right: return CUI_KEY_RIGHT;
    case XK_Up: case XK_KP_Up: return CUI_KEY_UP;
    case XK_Down: case XK_KP_Down: return CUI_KEY_DOWN;
    case XK_Home: case XK_KP_Home: return CUI_KEY_HOME;
    case XK_End: case XK_KP_End: return CUI_KEY_END;
    case XK_Page_Up: return CUI_KEY_PAGE_UP;
    case XK_Page_Down: return CUI_KEY_PAGE_DOWN;
    case XK_minus: return CUI_KEY_MINUS;
    case XK_equal: return CUI_KEY_EQUAL;
    case XK_comma: return CUI_KEY_COMMA;
    case XK_period: return CUI_KEY_PERIOD;
    case XK_slash: return CUI_KEY_SLASH;
    case XK_backslash: return CUI_KEY_BACKSLASH;
    case XK_grave: return CUI_KEY_GRAVE;
    case XK_Insert: return CUI_KEY_INSERT;
    default: return CUI_KEY_NONE;
    }
}

static int cui__x11_mods(unsigned int state) {
    int m = 0;
    if (state & ShiftMask) m |= CUI_MOD_SHIFT;
    if (state & ControlMask) m |= CUI_MOD_CTRL;
    if (state & Mod1Mask) m |= CUI_MOD_ALT;
    if (state & Mod4Mask) m |= CUI_MOD_SUPER;
    return m;
}

/* ---- drag and drop ---- */

static void cui__x11_xdnd_send(cui__x11 *x, Window to, Atom type, long l1, long l2, long l3, long l4) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.display = x->dpy;
    e.xclient.window = to;
    e.xclient.message_type = type;
    e.xclient.format = 32;
    e.xclient.data.l[0] = (long)x->win;
    e.xclient.data.l[1] = l1;
    e.xclient.data.l[2] = l2;
    e.xclient.data.l[3] = l3;
    e.xclient.data.l[4] = l4;
    XSendEvent(x->dpy, to, False, NoEventMask, &e);
    XFlush(x->dpy);
}

static int cui__hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* text/uri-list: one URI per line, "file://host/path" with %XX escapes */
static void cui__x11_xdnd_deliver(cui_app *app, const char *data, size_t len) {
    cui__x11 *x = cui__x(app);
    char *paths[64];
    int n = 0;
    size_t i = 0;
    while (i < len && n < 64) {
        size_t e = i;
        while (e < len && data[e] != '\n' && data[e] != '\r') e++;
        if (e > i && data[i] != '#' && e - i > 7 && strncmp(data + i, "file://", 7) == 0) {
            size_t p = i + 7;
            while (p < e && data[p] != '/') p++;   /* skip the host */
            char *out = malloc(e - p + 1);
            if (out) {
                size_t o = 0;
                for (size_t k = p; k < e; k++) {
                    if (data[k] == '%' && k + 2 < e && cui__hexv(data[k + 1]) >= 0 && cui__hexv(data[k + 2]) >= 0) {
                        out[o++] = (char)(cui__hexv(data[k + 1]) * 16 + cui__hexv(data[k + 2]));
                        k += 2;
                    } else out[o++] = data[k];
                }
                out[o] = 0;
                paths[n++] = out;
            }
        }
        i = e;
        while (i < len && (data[i] == '\n' || data[i] == '\r')) i++;
    }
    cui_input_file_drop(app->ui, (const char *const *)paths, n, x->xdnd_x, x->xdnd_y);
    for (int k = 0; k < n; k++) free(paths[k]);
}

static bool cui__x11_xdnd_message(cui_app *app, XClientMessageEvent *m) {
    cui__x11 *x = cui__x(app);
    Atom t = m->message_type;
    if (t == x->xdnd_enter) {
        x->xdnd_source = (Window)m->data.l[0];
        x->xdnd_version = (int)((unsigned long)m->data.l[1] >> 24);
        return true;
    }
    if (t == x->xdnd_position) {
        x->xdnd_source = (Window)m->data.l[0];
        int rx = (int)(((unsigned long)m->data.l[2] >> 16) & 0xFFFF), ry = (int)((unsigned long)m->data.l[2] & 0xFFFF);
        int wx = 0, wy = 0;
        Window child;
        XTranslateCoordinates(x->dpy, RootWindow(x->dpy, x->screen), x->win, rx, ry, &wx, &wy, &child);
        x->xdnd_x = wx / x->scale;
        x->xdnd_y = wy / x->scale;
        cui_input_file_hover(app->ui, true, x->xdnd_x, x->xdnd_y);
        /* accept, keep sending positions, and ask for a copy */
        cui__x11_xdnd_send(x, x->xdnd_source, x->xdnd_status, 1 | 2, 0, 0, (long)x->xdnd_action_copy);
        return true;
    }
    if (t == x->xdnd_leave) {
        cui_input_file_hover(app->ui, false, 0, 0);
        return true;
    }
    if (t == x->xdnd_drop) {
        x->xdnd_source = (Window)m->data.l[0];
        Time when = x->xdnd_version >= 1 ? (Time)m->data.l[2] : CurrentTime;
        XConvertSelection(x->dpy, x->xdnd_selection, x->uri_list, x->cui_xdnd, x->win, when);
        return true;
    }
    return false;
}

static void cui__x11_xdnd_selection(cui_app *app, XSelectionEvent *se) {
    cui__x11 *x = cui__x(app);
    if (se->property == None) {
        cui_input_file_hover(app->ui, false, 0, 0);
    } else {
        Atom type;
        int format;
        unsigned long count = 0, after = 0;
        unsigned char *data = NULL;
        if (XGetWindowProperty(x->dpy, x->win, x->cui_xdnd, 0, 1 << 22, True, AnyPropertyType, &type, &format, &count, &after, &data) == Success && data) {
            cui__x11_xdnd_deliver(app, (const char *)data, count);
            XFree(data);
        }
    }
    if (x->xdnd_source && x->xdnd_version >= 2)
        cui__x11_xdnd_send(x, x->xdnd_source, x->xdnd_finished, se->property != None ? 1 : 0, (long)x->xdnd_action_copy, 0, 0);
    x->xdnd_source = 0;
}

static void cui__x11_event(cui_app *app, XEvent *ev) {
    cui__x11 *x = cui__x(app);
    cui_ctx *ui = app->ui;
    if (XFilterEvent(ev, None)) return;
    float s = x->scale;
    switch (ev->type) {
    case ClientMessage:
        if (cui__x11_xdnd_message(app, &ev->xclient)) break;
        if ((Atom)ev->xclient.data.l[0] == x->wm_delete) app->running = false;
        break;
    case SelectionNotify:
        if (ev->xselection.property == x->cui_xdnd || (ev->xselection.selection == x->xdnd_selection)) cui__x11_xdnd_selection(app, &ev->xselection);
        break;
    case ConfigureNotify:
        x->geometry_changed = true;
        if (ev->xconfigure.width != x->width || ev->xconfigure.height != x->height) {
            x->width = ev->xconfigure.width;
            x->height = ev->xconfigure.height;
            cui_request_redraw(ui);
        }
        break;
    case Expose:
        app->force_present = true;
        cui_request_redraw(ui);
        break;
    case MotionNotify:
        cui_input_mouse_move(ui, ev->xmotion.x / s, ev->xmotion.y / s);
        break;
    case EnterNotify:
        cui_input_mouse_move(ui, ev->xcrossing.x / s, ev->xcrossing.y / s);
        break;
    case LeaveNotify:
        if (!cui_mouse_down(ui, CUI_MOUSE_LEFT)) cui_input_mouse_leave(ui);
        break;
    case ButtonPress:
    case ButtonRelease: {
        bool down = ev->type == ButtonPress;
        unsigned int b = ev->xbutton.button;
        cui_input_mouse_move(ui, ev->xbutton.x / s, ev->xbutton.y / s);
        if (b == Button1) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, down);
        else if (b == Button2) cui_input_mouse_button(ui, CUI_MOUSE_MIDDLE, down);
        else if (b == Button3) cui_input_mouse_button(ui, CUI_MOUSE_RIGHT, down);
        else if (down && b == Button4) cui_input_scroll(ui, 0, 1);
        else if (down && b == Button5) cui_input_scroll(ui, 0, -1);
        else if (down && b == 6) cui_input_scroll(ui, 1, 0);
        else if (down && b == 7) cui_input_scroll(ui, -1, 0);
        break;
    }
    case KeyPress:
    case KeyRelease: {
        bool down = ev->type == KeyPress;
        KeySym ks = XLookupKeysym(&ev->xkey, 0);
        int mods = cui__x11_mods(ev->xkey.state);
        cui_key k = cui__x11_key(ks);
        if (k) cui_input_key(ui, k, down, mods);
        else {
            /* a modifier on its own; X reports the state from before this event, so apply the change */
            int bit = (ks == XK_Shift_L || ks == XK_Shift_R) ? CUI_MOD_SHIFT : (ks == XK_Control_L || ks == XK_Control_R) ? CUI_MOD_CTRL
                    : (ks == XK_Alt_L || ks == XK_Alt_R || ks == XK_Meta_L || ks == XK_Meta_R) ? CUI_MOD_ALT : (ks == XK_Super_L || ks == XK_Super_R) ? CUI_MOD_SUPER : 0;
            if (bit) cui_input_key(ui, CUI_KEY_NONE, down, down ? (mods | bit) : (mods & ~bit));
        }
        if (down) {
            char buf[64];
            KeySym sym = 0;
            int n;
            if (x->ic) {
                Status st;
                n = Xutf8LookupString(x->ic, &ev->xkey, buf, (int)sizeof buf - 1, &sym, &st);
                if (st != XLookupChars && st != XLookupBoth) n = 0;
            } else {
                n = XLookupString(&ev->xkey, buf, (int)sizeof buf - 1, &sym, NULL);
            }
            if (n > 0 && !(mods & CUI_MOD_CTRL)) {
                buf[n] = 0;
                cui_input_text(ui, buf);
            }
        }
        break;
    }
    case FocusIn:
        if (x->ic) XSetICFocus(x->ic);
        cui_input_focus(ui, true);
        break;
    case FocusOut:
        if (x->ic) XUnsetICFocus(x->ic);
        cui_input_focus(ui, false);
        break;
    case SelectionRequest:
        cui__x11_handle_selection_request(x, &ev->xselectionrequest);
        break;
    case SelectionClear:
        break;
    default:
        break;
    }
}

void cui__plat_wait(cui_app *app, double timeout) {
    cui__x11 *x = cui__x(app);
    XFlush(x->dpy);
    if (timeout != 0 && !XPending(x->dpy)) {
        int fd = ConnectionNumber(x->dpy);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        struct timeval tv, *ptv = NULL;
        if (timeout > 0) {
            tv.tv_sec = (long)timeout;
            tv.tv_usec = (long)((timeout - (double)tv.tv_sec) * 1e6);
            ptv = &tv;
        }
        select(fd + 1, &set, NULL, NULL, ptv);
    }
    while (XPending(x->dpy)) {
        XEvent ev;
        XNextEvent(x->dpy, &ev);
        cui__x11_event(app, &ev);
    }
    /* after the whole batch, which also carries a state change that came with the move */
    if (x->geometry_changed) {
        x->geometry_changed = false;
        if (x->mapped && cui__plat_window_state(app) == CUI_WINDOW_NORMAL) {
            int px, py;
            cui__x11_content_origin(x, &px, &py);
            /* another client can maximize the window between the two queries: keep only a place
             * read while the window stayed normal, and only with the size it had then */
            if (cui__plat_window_state(app) == CUI_WINDOW_NORMAL && !XPending(x->dpy)) {
                x->normal_x = px;
                x->normal_y = py;
                x->normal_pos_known = true;
                x->normal_w = x->width;
                x->normal_h = x->height;
            } else {
                x->geometry_changed = true;   /* try again after the next batch */
            }
        }
    }
}

/* Asks the window manager to add (1) or remove (0) two _NET_WM_STATE properties. */
static void cui__x11_send_state(cui__x11 *x, long action, Atom a, Atom b) {
    XEvent e;
    memset(&e, 0, sizeof e);
    e.xclient.type = ClientMessage;
    e.xclient.window = x->win;
    e.xclient.message_type = x->net_wm_state;
    e.xclient.format = 32;
    e.xclient.data.l[0] = action;
    e.xclient.data.l[1] = (long)a;
    e.xclient.data.l[2] = (long)b;
    e.xclient.data.l[3] = 1; /* source: application */
    XSendEvent(x->dpy, DefaultRootWindow(x->dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
    XFlush(x->dpy);
}

void cui__plat_maximize(cui_app *app) {
    cui__x11 *x = cui__x(app);
    if (x && x->dpy && x->win) cui__x11_send_state(x, 1, x->net_wm_maximized_horz, x->net_wm_maximized_vert);
}

void cui__plat_minimize(cui_app *app) {
    cui__x11 *x = cui__x(app);
    XIconifyWindow(x->dpy, x->win, x->screen);
    XFlush(x->dpy);
}

void cui__plat_restore(cui_app *app) {
    cui__x11 *x = cui__x(app);
    cui_window_state state = cui__plat_window_state(app);
    if (state == CUI_WINDOW_MAXIMIZED) {
        cui__x11_send_state(x, 0, x->net_wm_maximized_horz, x->net_wm_maximized_vert);
    } else if (state == CUI_WINDOW_MINIMIZED) {
        /* ICCCM: mapping an iconic window makes it normal again; EWMH: activating raises it */
        XMapWindow(x->dpy, x->win);
        XEvent e;
        memset(&e, 0, sizeof e);
        e.xclient.type = ClientMessage;
        e.xclient.window = x->win;
        e.xclient.message_type = x->net_active_window;
        e.xclient.format = 32;
        e.xclient.data.l[0] = 1; /* source: application */
        XSendEvent(x->dpy, DefaultRootWindow(x->dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
        XFlush(x->dpy);
    }
}

/* Reads _NET_WM_STATE and WM_STATE. A minimized window keeps its maximized properties. */
static void cui__x11_read_state(cui__x11 *x, bool *maximized, bool *minimized) {
    bool horz = false, vert = false, hidden = false;
    Atom type;
    int format;
    unsigned long count, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(x->dpy, x->win, x->net_wm_state, 0, 64, False, XA_ATOM, &type, &format, &count, &after, &data) == Success &&
        data && format == 32) {
        const Atom *atoms = (const Atom *)data;   /* format 32 arrives as an array of longs */
        for (unsigned long i = 0; i < count; i++) {
            if (atoms[i] == x->net_wm_maximized_horz) horz = true;
            if (atoms[i] == x->net_wm_maximized_vert) vert = true;
            if (atoms[i] == x->net_wm_hidden) hidden = true;
        }
    }
    if (data) XFree(data);
    /* ICCCM WM_STATE, for window managers that minimize without _NET_WM_STATE_HIDDEN */
    data = NULL;
    if (XGetWindowProperty(x->dpy, x->win, x->wm_state, 0, 2, False, x->wm_state, &type, &format, &count, &after, &data) == Success &&
        data && format == 32 && count >= 1 && ((const long *)data)[0] == IconicState)
        hidden = true;
    if (data) XFree(data);
    *maximized = horz && vert;
    *minimized = hidden;
}

cui_window_state cui__plat_window_state(cui_app *app) {
    bool maximized, minimized;
    cui__x11_read_state(cui__x(app), &maximized, &minimized);
    return minimized ? CUI_WINDOW_MINIMIZED : maximized ? CUI_WINDOW_MAXIMIZED : CUI_WINDOW_NORMAL;
}

void cui__plat_set_position(cui_app *app, int px, int py) {
    cui__x11 *x = cui__x(app);
    XMoveWindow(x->dpy, x->win, px, py);   /* static gravity: the content's corner */
    XFlush(x->dpy);
}

bool cui__plat_position(cui_app *app, int *px, int *py) {
    cui__x11 *x = cui__x(app);
    bool maximized, minimized;
    cui__x11_read_state(x, &maximized, &minimized);
    if (minimized) {   /* where it comes back */
        if (!x->normal_pos_known) return false;
        *px = x->normal_x;
        *py = x->normal_y;
        return true;
    }
    cui__x11_content_origin(x, px, py);
    return true;
}

void cui__plat_placement(cui_app *app, cui_window_placement *out) {
    cui__x11 *x = cui__x(app);
    /* the bounds noted after the last event batch: a live read can catch the window manager
     * between placing the frame and reporting a new state */
    bool maximized, minimized;
    cui__x11_read_state(x, &maximized, &minimized);
    out->x = x->normal_x;
    out->y = x->normal_y;
    out->width = (int)((float)x->normal_w / x->scale + 0.5f);
    out->height = (int)((float)x->normal_h / x->scale + 0.5f);
    out->has_position = x->normal_pos_known;
    out->state = maximized ? CUI_WINDOW_MAXIMIZED : CUI_WINDOW_NORMAL;
}

/* OpenGL is set up on the app's thread before the first frame (see cui_app_win32.c for the other way) */
bool cui__plat_gl_begin_async(cui_app *app) { (void)app; return false; }
bool cui__plat_gl_end_async(cui_app *app, bool wait, bool vsync, cui_gl_renderer **out) { (void)app; (void)wait; (void)vsync; *out = NULL; return true; }
