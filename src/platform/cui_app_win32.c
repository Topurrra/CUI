/* Win32 backend (Windows 7 and later). Per-monitor DPI awareness, WGL
 * OpenGL 3.3 with a GDI software fallback, UTF-16 text input with the IME
 * placed at the caret, clipboard, dark title bars and smooth live resizing. */
#include "cui_platform.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ole2.h>
#include <dwmapi.h>
#include <imm.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL 0x020E
#endif

#define CUI_GLAPI_WIN32 __stdcall
typedef HGLRC (WINAPI *PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int *);
typedef BOOL (WINAPI *PFN_wglSwapIntervalEXT)(int);
typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(HWND, DWORD, LPCVOID, DWORD);
typedef HANDLE (WINAPI *PFN_CreateWaitableTimerExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

#define WGL_CONTEXT_MAJOR_VERSION_ARB_ 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB_ 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB_ 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB_ 0x0001

typedef struct {
    HWND hwnd;
    HDC hdc;
    HGLRC glrc;
    int width, height;     /* client pixels */
    float scale;
    bool tracking;
    WCHAR high_surrogate;
    RECT ime_caret;        /* the focused text field's caret in client pixels, where the IME opens */
    bool ime_caret_set;
    int sys_caret_h;       /* height of our hidden system caret, 0 while there is none */
    HCURSOR cursors[CUI_CURSOR_COUNT];
    cui_cursor cursor;
    bool in_size_move;
    BITMAPINFO bmi;
    HANDLE timer;          /* timed waits: high resolution where Windows has it (10 1803 and later) */
    bool show_pending;     /* shown with the first picture (cui__plat_show) */
    bool ole_drop;         /* the window is an OLE drop target (cui_app_desc.file_hover) */
} cui__win32;

static HMODULE cui__opengl32;
static PFN_GetDpiForWindow cui__GetDpiForWindow;
static PFN_AdjustWindowRectExForDpi cui__AdjustWindowRectExForDpi;

static cui__win32 *cui__w(cui_app *app) { return app->plat; }

/* OpenGL frame pacing. With the swap interval at 1 some drivers (Intel here) block in the
 * first buffer upload of the next frame until the flip, spinning a core: scrolling a big
 * file cost 13 ms and a whole core per frame. So by default the swap interval is 0 and the
 * frame waits for the compositor with DwmFlush() after SwapBuffers: still one frame per
 * refresh and no tearing (DWM counters: composed == refreshes == swaps, nothing dropped),
 * about 1-2 ms per frame and a sixth of the CPU. Only when vsync is wanted.
 * CUI_GL_PACE=flush (default) | timer (sleep to the next vblank from DwmGetCompositionTimingInfo)
 *             | swap (the old way: swap interval 1). CUI_GL_PACE_STATS=1 prints DWM counters at exit. */
typedef HRESULT (WINAPI *PFN_DwmFlush)(void);
typedef HRESULT (WINAPI *PFN_DwmGetCompositionTimingInfo)(HWND, DWM_TIMING_INFO *);
static int cui__w32pace_mode = -1;   /* -1 unread, 0 off, 1 flush, 2 timer */
static PFN_DwmFlush cui__w32pace_flush;
static PFN_DwmGetCompositionTimingInfo cui__w32pace_timing;
static DWM_TIMING_INFO cui__w32pace_t0;
static unsigned long cui__w32pace_swaps;
static bool cui__w32pace_stats;
static bool cui__w32pace_vsync = true;   /* pace only when the app wants vsync */

static int cui__w32pace_get(void) {
    if (cui__w32pace_mode >= 0) return cui__w32pace_mode;
    const char *v = getenv("CUI_GL_PACE");
    cui__w32pace_mode = !v || !strcmp(v, "flush") ? 1 : !strcmp(v, "timer") ? 2 : 0;
    cui__w32pace_stats = getenv("CUI_GL_PACE_STATS") != NULL;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm) {
        cui__w32pace_flush = (PFN_DwmFlush)(void *)GetProcAddress(dwm, "DwmFlush");
        cui__w32pace_timing = (PFN_DwmGetCompositionTimingInfo)(void *)GetProcAddress(dwm, "DwmGetCompositionTimingInfo");
    }
    if (!cui__w32pace_flush || !cui__w32pace_timing) cui__w32pace_mode = 0;
    return cui__w32pace_mode;
}

static void cui__w32pace_wait(cui__win32 *w) {
    if (cui__w32pace_mode == 1) { cui__w32pace_flush(); return; }
    DWM_TIMING_INFO t;
    memset(&t, 0, sizeof t);
    t.cbSize = sizeof t;
    if (FAILED(cui__w32pace_timing(NULL, &t)) || !t.qpcRefreshPeriod) { cui__w32pace_flush(); return; }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    unsigned long long period = t.qpcRefreshPeriod, last = t.qpcVBlank;
    unsigned long long next = last;
    if ((unsigned long long)now.QuadPart >= last) next = last + ((unsigned long long)now.QuadPart - last) / period * period + period;
    long long ticks = (long long)(next - (unsigned long long)now.QuadPart);
    if (ticks <= 0 || !w->timer) return;
    LARGE_INTEGER due;
    due.QuadPart = -(ticks * 10000000LL / freq.QuadPart);   /* 100 ns units, relative */
    if (SetWaitableTimer(w->timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(w->timer, 100);
}

static void cui__w32pace_report(void) {
    DWM_TIMING_INFO t;
    if (!cui__w32pace_stats || !cui__w32pace_timing) return;
    memset(&t, 0, sizeof t);
    t.cbSize = sizeof t;
    if (FAILED(cui__w32pace_timing(NULL, &t))) return;
    fprintf(stderr, "pace=%d swaps=%lu dwm_composed=%llu dwm_refreshes=%llu dwm_dropped=%llu dwm_missed=%llu\n", cui__w32pace_mode, cui__w32pace_swaps,
            (unsigned long long)(t.cFrame - cui__w32pace_t0.cFrame), (unsigned long long)(t.cRefresh - cui__w32pace_t0.cRefresh),
            (unsigned long long)(t.cFramesDropped - cui__w32pace_t0.cFramesDropped), (unsigned long long)(t.cFramesMissed - cui__w32pace_t0.cFramesMissed));
}

double cui__plat_now(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

const char *cui__plat_name(void) { return "win32"; }

int cui__plat_system_dark(void) {
    HKEY key;
    DWORD value = 1, size = sizeof value;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &key) != ERROR_SUCCESS)
        return -1;
    LONG r = RegQueryValueExW(key, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS) return -1;
    return value == 0 ? 1 : 0;
}

static void cui__win32_dpi_awareness(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    PFN_SetProcessDpiAwarenessContext ctx = user32 ? (PFN_SetProcessDpiAwarenessContext)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
    if (user32) {
        cui__GetDpiForWindow = (PFN_GetDpiForWindow)(void *)GetProcAddress(user32, "GetDpiForWindow");
        cui__AdjustWindowRectExForDpi = (PFN_AdjustWindowRectExForDpi)(void *)GetProcAddress(user32, "AdjustWindowRectExForDpi");
    }
    if (ctx && ctx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) return;
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        PFN_SetProcessDpiAwareness f = (PFN_SetProcessDpiAwareness)(void *)GetProcAddress(shcore, "SetProcessDpiAwareness");
        if (f && SUCCEEDED(f(2))) return;
    }
    SetProcessDPIAware();
}

static float cui__win32_window_scale(HWND hwnd) {
    const char *env = getenv("CUI_SCALE");
    if (env) { float s = (float)atof(env); if (s > 0.25f && s < 8) return s; }
    UINT dpi = 96;
    if (cui__GetDpiForWindow && hwnd) dpi = cui__GetDpiForWindow(hwnd);
    else {
        HDC dc = GetDC(NULL);
        dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(NULL, dc);
    }
    return dpi > 0 ? (float)dpi / 96.0f : 1.0f;
}

static void cui__win32_dark_titlebar(HWND hwnd, bool dark) {
    static PFN_DwmSetWindowAttribute set;
    static bool tried;
    if (!tried) {
        tried = true;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) set = (PFN_DwmSetWindowAttribute)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    if (!set) return;
    BOOL v = dark ? TRUE : FALSE;
    if (FAILED(set(hwnd, 20, &v, sizeof v))) set(hwnd, 19, &v, sizeof v);
}

static int cui__win32_mods(void) {
    int m = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) m |= CUI_MOD_SHIFT;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= CUI_MOD_CTRL;
    if (GetKeyState(VK_MENU) & 0x8000) m |= CUI_MOD_ALT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) m |= CUI_MOD_SUPER;
    return m;
}

static cui_key cui__win32_key(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return (cui_key)(CUI_KEY_A + (int)(vk - 'A'));
    if (vk >= '0' && vk <= '9') return (cui_key)(CUI_KEY_0 + (int)(vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return (cui_key)(CUI_KEY_F1 + (int)(vk - VK_F1));
    switch (vk) {
    case VK_TAB: return CUI_KEY_TAB;
    case VK_RETURN: return CUI_KEY_ENTER;
    case VK_ESCAPE: return CUI_KEY_ESCAPE;
    case VK_BACK: return CUI_KEY_BACKSPACE;
    case VK_DELETE: return CUI_KEY_DELETE;
    case VK_SPACE: return CUI_KEY_SPACE;
    case VK_LEFT: return CUI_KEY_LEFT;
    case VK_RIGHT: return CUI_KEY_RIGHT;
    case VK_UP: return CUI_KEY_UP;
    case VK_DOWN: return CUI_KEY_DOWN;
    case VK_HOME: return CUI_KEY_HOME;
    case VK_END: return CUI_KEY_END;
    case VK_PRIOR: return CUI_KEY_PAGE_UP;
    case VK_NEXT: return CUI_KEY_PAGE_DOWN;
    case VK_OEM_MINUS: return CUI_KEY_MINUS;
    case VK_OEM_PLUS: return CUI_KEY_EQUAL;
    case VK_OEM_COMMA: return CUI_KEY_COMMA;
    case VK_OEM_PERIOD: return CUI_KEY_PERIOD;
    case VK_OEM_2: return CUI_KEY_SLASH;
    case VK_OEM_5: return CUI_KEY_BACKSLASH;
    case VK_OEM_3: return CUI_KEY_GRAVE;
    default: return CUI_KEY_NONE;
    }
}

static void cui__win32_set_cursor(void *user, cui_cursor c) {
    cui_app *app = user;
    cui__win32 *w = cui__w(app);
    if ((int)c < 0 || c >= CUI_CURSOR_COUNT) return;
    w->cursor = c;
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(w->hwnd, &p);
    if (p.x >= 0 && p.y >= 0 && p.x < w->width && p.y < w->height) SetCursor(w->cursors[c]);
}

static void cui__win32_set_clip(void *user, const char *utf8) {
    cui_app *app = user;
    cui__win32 *w = cui__w(app);
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0 || !OpenClipboard(w->hwnd)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)n * sizeof(WCHAR));
    if (mem) {
        WCHAR *dst = GlobalLock(mem);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, dst, n);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

static const char *cui__win32_get_clip(void *user) {
    cui_app *app = user;
    cui__win32 *w = cui__w(app);
    if (!OpenClipboard(w->hwnd)) return NULL;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    const char *result = NULL;
    if (h) {
        const WCHAR *src = GlobalLock(h);
        if (src) {
            int n = WideCharToMultiByte(CP_UTF8, 0, src, -1, NULL, 0, NULL, NULL);
            if (n > 0) {
                char *buf = malloc((size_t)n);
                if (buf) {
                    WideCharToMultiByte(CP_UTF8, 0, src, -1, buf, n, NULL, NULL);
                    /* drop carriage returns */
                    char *o = buf;
                    for (char *p = buf; *p; p++) if (*p != '\r') *o++ = *p;
                    *o = 0;
                    cui__app_set_clipboard_cache(app, buf);
                    free(buf);
                    result = app->clipboard;
                }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return result;
}

/* The app's answer for a screen point inside its window, as a WM_NCHITTEST result. */
static LRESULT cui__win32_hit(cui_app *app, HWND hwnd, POINT pt, float scale) {
    if (!app->desc.hit_test) return HTCLIENT;
    ScreenToClient(hwnd, &pt);
    switch (app->desc.hit_test(app->desc.hit_test_user, (float)pt.x / scale, (float)pt.y / scale)) {
    case CUI_WINDOW_HIT_CAPTION: return HTCAPTION;
    case CUI_WINDOW_HIT_MIN: return HTMINBUTTON;
    case CUI_WINDOW_HIT_MAX: return app->desc.fixed_size ? HTCLIENT : HTMAXBUTTON;
    case CUI_WINDOW_HIT_CLOSE: return HTCLOSE;
    default: return HTCLIENT;
    }
}

/* Tells the input method where the caret is: the composition window starts at
 * the caret and the candidate list opens under the line without covering it.
 * IMEs differ in what they read, so each form is set. Chinese IMEs running
 * through TSF take the candidate position, Japanese and Korean IMEs the
 * excluded rectangle, and older IMEs, magnifiers and screen readers follow the
 * system caret, which stays hidden: CUI draws its own. */
static void cui__win32_place_ime(cui__win32 *w) {
    if (!w->ime_caret_set) return;
    RECT c = w->ime_caret;
    if (GetFocus() == w->hwnd) {
        int h = (int)(c.bottom - c.top);
        if (w->sys_caret_h != h) {
            if (w->sys_caret_h) DestroyCaret();
            w->sys_caret_h = CreateCaret(w->hwnd, NULL, 1, h) ? h : 0;
        }
        if (w->sys_caret_h) SetCaretPos(c.left, c.top);
    }
    HIMC imc = ImmGetContext(w->hwnd);
    if (!imc) return;   /* no IME for this window or this keyboard layout */
    COMPOSITIONFORM comp;
    memset(&comp, 0, sizeof comp);
    comp.dwStyle = CFS_POINT;
    comp.ptCurrentPos.x = c.left;
    comp.ptCurrentPos.y = c.top;
    ImmSetCompositionWindow(imc, &comp);
    CANDIDATEFORM cand;
    memset(&cand, 0, sizeof cand);
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos.x = c.left;
    cand.ptCurrentPos.y = c.bottom;
    ImmSetCandidateWindow(imc, &cand);
    cand.dwStyle = CFS_EXCLUDE;
    cand.ptCurrentPos.y = c.top;
    cand.rcArea = c;
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(w->hwnd, imc);
}

/* CUI calls this every frame a text field has focus, with the caret in window
 * points; the IME is told only when the caret moved. */
static void cui__win32_text_input_rect(void *user, cui_rect caret) {
    cui_app *app = user;
    cui__win32 *w = cui__w(app);
    float s = w->scale;
    RECT r;
    r.left = (LONG)floorf(caret.x * s);
    r.top = (LONG)floorf(caret.y * s);
    r.right = r.left + CUI_MAX(1, (LONG)ceilf(caret.w * s));
    r.bottom = r.top + CUI_MAX(1, (LONG)ceilf(caret.h * s));
    if (w->ime_caret_set && EqualRect(&r, &w->ime_caret)) return;
    w->ime_caret = r;
    w->ime_caret_set = true;
    cui__win32_place_ime(w);
}

/* Files dropped from the OS: the paths of a drop (WM_DROPFILES or OLE) go to the frame as one drop at (x, y). */
static void cui__win32_deliver_drop(cui_app *app, HDROP drop, float x, float y) {
    UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
    if (n > 64) n = 64;
    char *paths[64] = { 0 };   /* zeroed: MinGW cannot tell that only the first `got` are read */
    int got = 0;
    for (UINT i = 0; i < n; i++) {
        UINT len = DragQueryFileW(drop, i, NULL, 0);
        WCHAR *wbuf = (WCHAR *)malloc(sizeof(WCHAR) * (len + 1));
        if (!wbuf) continue;
        DragQueryFileW(drop, i, wbuf, len + 1);
        int u = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, NULL, 0, NULL, NULL);
        char *ubuf = u > 0 ? (char *)malloc((size_t)u) : NULL;
        if (ubuf) { WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, ubuf, u, NULL, NULL); paths[got++] = ubuf; }
        free(wbuf);
    }
    cui_input_file_drop(app->ui, (const char *const *)paths, got, x, y);
    for (int i = 0; i < got; i++) free(paths[i]);
}

/* OLE drop target (cui_app_desc.file_hover): the same drop, and the hover before it */
typedef struct cui__win32_dt {
    IDropTarget iface;
    LONG refs;
    cui_app *app;
} cui__win32_dt;

static const GUID cui__iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUID cui__iid_droptarget = { 0x00000122, 0x0000, 0x0000, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };

static bool cui__win32_dt_has_files(IDataObject *obj) {
    FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return obj && obj->lpVtbl->QueryGetData(obj, &fe) == S_OK;
}

static void cui__win32_dt_point(cui_app *app, POINTL pt, float *x, float *y) {
    cui__win32 *w = cui__w(app);
    POINT p = { pt.x, pt.y };
    ScreenToClient(w->hwnd, &p);
    *x = (float)p.x / w->scale;
    *y = (float)p.y / w->scale;
}

static HRESULT STDMETHODCALLTYPE cui__dt_qi(IDropTarget *t, REFIID riid, void **out) {
    if (IsEqualGUID(riid, &cui__iid_unknown) || IsEqualGUID(riid, &cui__iid_droptarget)) {
        *out = t;
        t->lpVtbl->AddRef(t);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cui__dt_addref(IDropTarget *t) { return (ULONG)InterlockedIncrement(&((cui__win32_dt *)t)->refs); }
static ULONG STDMETHODCALLTYPE cui__dt_release(IDropTarget *t) {
    LONG n = InterlockedDecrement(&((cui__win32_dt *)t)->refs);
    if (n == 0) free(t);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE cui__dt_enter(IDropTarget *t, IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect) {
    (void)keys;
    cui_app *app = ((cui__win32_dt *)t)->app;
    if (!cui__win32_dt_has_files(obj)) { *effect = DROPEFFECT_NONE; return S_OK; }
    float x, y;
    cui__win32_dt_point(app, pt, &x, &y);
    cui_input_file_hover(app->ui, true, x, y);
    *effect = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cui__dt_over(IDropTarget *t, DWORD keys, POINTL pt, DWORD *effect) {
    (void)keys;
    cui_app *app = ((cui__win32_dt *)t)->app;
    float x, y;
    cui__win32_dt_point(app, pt, &x, &y);
    cui_input_file_hover(app->ui, true, x, y);
    *effect = DROPEFFECT_COPY;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cui__dt_leave(IDropTarget *t) {
    cui_input_file_hover(((cui__win32_dt *)t)->app->ui, false, 0, 0);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE cui__dt_drop(IDropTarget *t, IDataObject *obj, DWORD keys, POINTL pt, DWORD *effect) {
    (void)keys;
    cui_app *app = ((cui__win32_dt *)t)->app;
    *effect = DROPEFFECT_NONE;
    cui_input_file_hover(app->ui, false, 0, 0);
    FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM sm;
    memset(&sm, 0, sizeof sm);
    if (!obj || obj->lpVtbl->GetData(obj, &fe, &sm) != S_OK) return S_OK;
    float x, y;
    cui__win32_dt_point(app, pt, &x, &y);
    cui__win32_deliver_drop(app, (HDROP)sm.hGlobal, x, y);
    ReleaseStgMedium(&sm);
    *effect = DROPEFFECT_COPY;
    SetForegroundWindow(cui__w(app)->hwnd);
    return S_OK;
}
static IDropTargetVtbl cui__dt_vtbl = { cui__dt_qi, cui__dt_addref, cui__dt_release, cui__dt_enter, cui__dt_over, cui__dt_leave, cui__dt_drop };

/* true when the window takes drops through OLE; false leaves it to WM_DROPFILES */
static bool cui__win32_dt_register(cui_app *app, HWND hwnd) {
    HRESULT hr = OleInitialize(NULL);
    if (hr != S_OK && hr != S_FALSE) return false;
    cui__win32_dt *dt = (cui__win32_dt *)calloc(1, sizeof *dt);
    if (!dt) return false;
    dt->iface.lpVtbl = &cui__dt_vtbl;
    dt->refs = 1;
    dt->app = app;
    if (RegisterDragDrop(hwnd, &dt->iface) != S_OK) { free(dt); return false; }
    return true;   /* the registration holds the other reference; RevokeDragDrop releases it */
}

static LRESULT CALLBACK cui__win32_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    cui_app *app = (cui_app *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!app) {
        if (msg == WM_NCCREATE) {
            CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    cui__win32 *w = cui__w(app);
    cui_ctx *ui = app->ui;
    float s = w ? w->scale : 1.0f;
    switch (msg) {
    case WM_CLOSE:
        app->running = false;
        return 0;
    case WM_NCCALCSIZE:
        /* custom title bar: the client area takes the whole window. Maximized, Windows
         * places the window past the screen by its frame, so keep that much back. */
        if (app->desc.custom_titlebar && wp) {
            if (IsZoomed(hwnd)) {
                NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
                int fx = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                int fy = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        /* custom title bar: the edges still resize; everything else is the app's */
        if (app->desc.custom_titlebar) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (!IsZoomed(hwnd) && !app->desc.fixed_size) {
                RECT wr;
                GetWindowRect(hwnd, &wr);
                int b = (int)(6 * s + 0.5f);
                bool l = pt.x < wr.left + b, r = pt.x >= wr.right - b, t = pt.y < wr.top + b, d = pt.y >= wr.bottom - b;
                if (t && l) return HTTOPLEFT;
                if (t && r) return HTTOPRIGHT;
                if (d && l) return HTBOTTOMLEFT;
                if (d && r) return HTBOTTOMRIGHT;
                if (t) return HTTOP;
                if (d) return HTBOTTOM;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
            }
            return cui__win32_hit(app, hwnd, pt, s);
        }
        break;
    case WM_NCMOUSEMOVE:
        /* over the app's caption or buttons the pointer arrives as a non-client message: pass the
         * position on so hover shows, and ask for the matching leave */
        if (app->desc.custom_titlebar && app->desc.hit_test && w) {
            if (!w->tracking) {
                TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE | TME_NONCLIENT, hwnd, 0 };
                TrackMouseEvent(&tme);
                w->tracking = true;
            }
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd, &pt);
            cui_input_mouse_move(ui, (float)pt.x / s, (float)pt.y / s);
        }
        break;
    case WM_NCMOUSELEAVE:
        if (w) w->tracking = false;
        cui_input_mouse_leave(ui);
        break;
    case WM_NCLBUTTONDOWN:
        /* the buttons are ours to run: the default would track the press and draw its own button */
        if (app->desc.custom_titlebar && app->desc.hit_test && (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE)) return 0;
        break;
    case WM_NCLBUTTONUP:
        if (app->desc.custom_titlebar && app->desc.hit_test) {
            if (wp == HTMINBUTTON) { ShowWindow(hwnd, SW_MINIMIZE); return 0; }
            if (wp == HTMAXBUTTON) { ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE); return 0; }
            if (wp == HTCLOSE) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        }
        break;
    case WM_APP + 0x51:
        /* cui_app_drag_window, posted so the move loop runs outside the frame */
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) {
            POINT pt;
            GetCursorPos(&pt);
            ReleaseCapture();
            SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
        }
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);   /* the move loop ate the release */
        cui_request_redraw(ui);
        return 0;
    case WM_SIZE:
        if (w) {
            w->width = LOWORD(lp);
            w->height = HIWORD(lp);
            cui_request_redraw(ui);
            if (w->in_size_move && w->width > 0 && w->height > 0) cui__app_live_frame(app);
        }
        return 0;
    case WM_ENTERSIZEMOVE: if (w) w->in_size_move = true; return 0;
    case WM_EXITSIZEMOVE: if (w) w->in_size_move = false; return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (app) app->force_present = true;   /* software: the next frame shows the whole picture, not only what changed */
        if (w && w->in_size_move) cui__app_live_frame(app);
        cui_request_redraw(ui);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        if (w) w->scale = (float)HIWORD(wp) / 96.0f;
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        cui_request_redraw(ui);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        float sc = w ? w->scale : cui__win32_window_scale(hwnd);
        RECT r = { 0, 0, (LONG)(app->desc.min_width * sc), (LONG)(app->desc.min_height * sc) };
        if (!app->desc.custom_titlebar) AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(hwnd, GWL_STYLE), FALSE, 0);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w && !w->tracking) {
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            w->tracking = true;
        }
        cui_input_mouse_move(ui, (float)GET_X_LPARAM(lp) / s, (float)GET_Y_LPARAM(lp) / s);
        return 0;
    case WM_MOUSELEAVE:
        if (w) w->tracking = false;
        cui_input_mouse_leave(ui);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
        cui_mouse_button b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? CUI_MOUSE_LEFT
                           : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? CUI_MOUSE_RIGHT : CUI_MOUSE_MIDDLE;
        cui_input_mouse_move(ui, (float)GET_X_LPARAM(lp) / s, (float)GET_Y_LPARAM(lp) / s);
        cui_input_mouse_button(ui, b, down);
        if (down) SetCapture(hwnd);
        else if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        return 0;
    }
    case WM_MOUSEWHEEL:
        cui_input_scroll(ui, 0, (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        POINT pt = { 0, 0 };
        DragQueryPoint(drop, &pt);
        cui__win32_deliver_drop(app, drop, (float)pt.x / s, (float)pt.y / s);
        DragFinish(drop);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        cui_input_scroll(ui, -(float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA, 0);
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        cui_key k = cui__win32_key(wp);
        if (k) cui_input_key(ui, k, down, cui__win32_mods());
        else if (wp == VK_SHIFT || wp == VK_CONTROL || wp == VK_MENU || wp == VK_LWIN || wp == VK_RWIN)
            cui_input_key(ui, CUI_KEY_NONE, down, cui__win32_mods());   /* a modifier on its own: releasing Ctrl after a shortcut must register */
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) return 0;
        return 0;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (c >= 0xD800 && c <= 0xDBFF) { if (w) w->high_surrogate = c; return 0; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!w || !w->high_surrogate) return 0;
            cp = 0x10000 + (((uint32_t)w->high_surrogate - 0xD800) << 10) + ((uint32_t)c - 0xDC00);
            w->high_surrogate = 0;
        }
        if (cp < 32 || cp == 127) return 0;
        char buf[8];
        int n = cui__utf8_encode(cp, buf);
        buf[n] = 0;
        cui_input_text(ui, buf);
        return 0;
    }
    /* Text committed in an IME arrives as WM_CHAR, one UTF-16 unit at a time:
     * DefWindowProcW turns the result string of WM_IME_COMPOSITION into
     * WM_IME_CHAR and those into WM_CHAR, so neither is read here. */
    case WM_IME_STARTCOMPOSITION:
        if (w) cui__win32_place_ime(w);
        break;
    case WM_SETFOCUS:
        if (w) cui__win32_place_ime(w);
        cui_input_focus(ui, true);
        return 0;
    case WM_KILLFOCUS:
        if (w && w->sys_caret_h) { DestroyCaret(); w->sys_caret_h = 0; }
        cui_input_focus(ui, false);
        return 0;
    case WM_SETCURSOR:
        if (w && LOWORD(lp) == HTCLIENT) { SetCursor(w->cursors[w->cursor]); return TRUE; }
        break;
    case WM_SETTINGCHANGE:
        if (app->desc.follow_system_theme && lp && lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
            int dark = cui__plat_system_dark();
            if (dark >= 0) {
                cui_set_theme(ui, dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
                cui__win32_dark_titlebar(hwnd, dark == 1);
            }
        }
        break;
    default:
        break;
    }
    /* a worker's message (the app's own, WM_APP and up, such as a wake up) needs a frame: cui_app_pending */
    if (msg >= WM_APP && msg < 0xC000 && msg != WM_APP + 0x51) app->woken = true;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool cui__plat_init(cui_app *app) {
    cui__win32_dpi_awareness();
    cui__win32 *w = calloc(1, sizeof *w);
    if (!w) return false;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = cui__win32_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wc.lpszClassName = L"CUIWindow";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (app->desc.fixed_size) style &= ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX);
    float scale = cui__win32_window_scale(NULL);
    RECT r = { 0, 0, (LONG)(app->desc.width * scale), (LONG)(app->desc.height * scale) };
    if (!app->desc.custom_titlebar) AdjustWindowRectEx(&r, style, FALSE, 0);
    WCHAR title[256];
    MultiByteToWideChar(CP_UTF8, 0, app->title, -1, title, 256);
    app->plat = w;
    app->wake_tracked = true;   /* WM_APP messages from workers mark the window woken (cui_app_pending) */
    w->hwnd = CreateWindowExW(0, wc.lpszClassName, title, style, app->desc.has_pos ? app->desc.x : CW_USEDEFAULT,
                              app->desc.has_pos ? app->desc.y : CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              NULL, NULL, inst, app);
    if (!w->hwnd) { free(w); app->plat = NULL; return false; }
    if (app->desc.custom_titlebar)   /* recompute the client area: no title bar or frame */
        SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    /* files dropped from Explorer arrive as WM_DROPFILES, or through OLE (with the hover before) when asked for */
    if (app->desc.file_hover && cui__win32_dt_register(app, w->hwnd)) w->ole_drop = true;
    else DragAcceptFiles(w->hwnd, TRUE);
    w->scale = cui__win32_window_scale(w->hwnd);
    if (w->scale != scale) {
        RECT r2 = { 0, 0, (LONG)(app->desc.width * w->scale), (LONG)(app->desc.height * w->scale) };
        if (!app->desc.custom_titlebar) AdjustWindowRectEx(&r2, style, FALSE, 0);
        SetWindowPos(w->hwnd, NULL, 0, 0, r2.right - r2.left, r2.bottom - r2.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RECT cr;
    GetClientRect(w->hwnd, &cr);
    w->width = cr.right; w->height = cr.bottom;
    w->hdc = GetDC(w->hwnd);
    LPCWSTR ids[CUI_CURSOR_COUNT] = { (LPCWSTR)IDC_ARROW, (LPCWSTR)IDC_HAND, (LPCWSTR)IDC_IBEAM, (LPCWSTR)IDC_SIZEWE,
                                      (LPCWSTR)IDC_SIZENS, (LPCWSTR)IDC_SIZEALL, (LPCWSTR)IDC_NO };
    for (int i = 0; i < CUI_CURSOR_COUNT; i++) w->cursors[i] = LoadCursorW(NULL, ids[i]);
    cui__win32_dark_titlebar(w->hwnd, app->desc.theme == CUI_THEME_DARK);
    cui_platform_iface pi;
    memset(&pi, 0, sizeof pi);
    pi.user = app;
    pi.set_clipboard = cui__win32_set_clip;
    pi.get_clipboard = cui__win32_get_clip;
    pi.set_cursor = cui__win32_set_cursor;
    pi.set_text_input_rect = cui__win32_text_input_rect;
    cui_set_platform(app->ui, &pi);
    return true;
}

/* Activating a new window costs 50 to 80 ms (Windows sets up text input for it), and showing
 * it takes the first picture's place on screen before there is one. So a window opened the
 * usual way is shown when its first picture is ready, without activating, and activated right
 * after that picture is out. One that starts minimized or maximized (a shortcut's setting)
 * is shown now, as Windows says, since that changes its size before the first picture. */
void cui__plat_show(cui_app *app) {
    cui__win32 *w = cui__w(app);
    STARTUPINFOW si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    GetStartupInfoW(&si);
    WORD how = (si.dwFlags & STARTF_USESHOWWINDOW) ? si.wShowWindow : (WORD)SW_SHOWNORMAL;
    bool plain = how == SW_SHOWNORMAL || how == SW_SHOW || how == SW_SHOWDEFAULT;
    if (plain && app->desc.start_maximized) {
        /* maximized changes the size before the first picture, so it is shown now, like a
         * shortcut set to maximized */
        ShowWindow(w->hwnd, SW_SHOWMAXIMIZED);
        UpdateWindow(w->hwnd);
        return;
    }
    if (plain) { w->show_pending = true; return; }
    ShowWindow(w->hwnd, SW_SHOWDEFAULT);
    UpdateWindow(w->hwnd);
}

static void cui__win32_show_now(cui__win32 *w) {
    if (!w->show_pending) return;
    w->show_pending = false;
    ShowWindow(w->hwnd, SW_SHOWNA);
}

static void cui__win32_activate(cui__win32 *w) {
    if (GetForegroundWindow() != w->hwnd) SetForegroundWindow(w->hwnd);
}

void cui__plat_shutdown(cui_app *app) {
    cui__win32 *w = cui__w(app);
    if (!w) return;
    cui__w32pace_report();
    if (w->glrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(w->glrc); }
    if (w->hdc) ReleaseDC(w->hwnd, w->hdc);
    if (w->timer) CloseHandle(w->timer);
    if (w->ole_drop) RevokeDragDrop(w->hwnd);   /* before the window goes: the target points at the app */
    SetWindowLongPtrW(w->hwnd, GWLP_USERDATA, 0);   /* no more messages to the app */
    DestroyWindow(w->hwnd);
    free(w);
    app->plat = NULL;
}

void *cui__plat_gl_proc(const char *name) {
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) {
        if (!cui__opengl32) cui__opengl32 = LoadLibraryW(L"opengl32.dll");
        p = cui__opengl32 ? (void *)GetProcAddress(cui__opengl32, name) : NULL;
    }
    return p;
}

/* The pixel format OpenGL draws with: 32 bit color with alpha, double buffered. CUI needs no
 * depth or stencil buffer, but ChoosePixelFormat gives one even when none is asked for (24 + 8
 * bits on Intel: 4 bytes a pixel of the window, 7 MB for a 1620 x 1080 window). So the same
 * format without them is taken when the driver has it. */
static int cui__win32_gl_format(HDC dc, PIXELFORMATDESCRIPTOR *pfd) {
    memset(pfd, 0, sizeof *pfd);
    pfd->nSize = sizeof *pfd;
    pfd->nVersion = 1;
    pfd->dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cAlphaBits = 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(dc, pfd);
    PIXELFORMATDESCRIPTOR c, q;
    if (!pf || !DescribePixelFormat(dc, pf, sizeof c, &c)) return pf;
    *pfd = c;
    if (!c.cDepthBits && !c.cStencilBits) return pf;
    int n = DescribePixelFormat(dc, 1, sizeof q, NULL);
    for (int i = 1; i <= n; i++) {
        if (!DescribePixelFormat(dc, i, sizeof q, &q)) break;
        if (q.dwFlags == c.dwFlags && q.iPixelType == c.iPixelType && q.cColorBits == c.cColorBits && q.cAlphaBits == c.cAlphaBits &&
            q.cAccumBits == c.cAccumBits && q.cAuxBuffers == c.cAuxBuffers && !q.cDepthBits && !q.cStencilBits) {
            *pfd = q;
            return i;
        }
    }
    return pf;
}

/* An OpenGL 3.3 core context for dc (whose pixel format is set), current on this thread. */
static HGLRC cui__win32_gl_context(HDC dc) {
    HGLRC legacy = wglCreateContext(dc);
    if (!legacy) return NULL;
    wglMakeCurrent(dc, legacy);
    PFN_wglCreateContextAttribsARB create = (PFN_wglCreateContextAttribsARB)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!create) { wglMakeCurrent(NULL, NULL); wglDeleteContext(legacy); return NULL; }
    int attrs[] = { WGL_CONTEXT_MAJOR_VERSION_ARB_, 3, WGL_CONTEXT_MINOR_VERSION_ARB_, 3,
                    WGL_CONTEXT_PROFILE_MASK_ARB_, WGL_CONTEXT_CORE_PROFILE_BIT_ARB_, 0 };
    HGLRC ctx = create(dc, NULL, attrs);
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(legacy);
    if (!ctx) return NULL;
    if (!wglMakeCurrent(dc, ctx)) { wglDeleteContext(ctx); return NULL; }
    return ctx;
}

static void cui__win32_swap_interval(bool vsync) {
    PFN_wglSwapIntervalEXT swap = (PFN_wglSwapIntervalEXT)(void *)wglGetProcAddress("wglSwapIntervalEXT");
    cui__w32pace_vsync = vsync;
    if (swap) swap(vsync && !cui__w32pace_get() ? 1 : 0);
}

bool cui__plat_gl_create(cui_app *app, bool vsync) {
    cui__win32 *w = cui__w(app);
    PIXELFORMATDESCRIPTOR pfd;
    int pf = cui__win32_gl_format(w->hdc, &pfd);
    if (!pf || !SetPixelFormat(w->hdc, pf, &pfd)) return false;
    HGLRC ctx = cui__win32_gl_context(w->hdc);
    if (!ctx) return false;
    w->glrc = ctx;
    cui__win32_swap_interval(vsync);
    return true;
}

/* OpenGL set up on a thread of its own. The context is made for a small hidden window with
 * the pixel format the app's window gets later, the renderer (its shaders) is made and draws
 * once, which is when drivers finish compiling; then the context is let go, and the app's
 * thread takes it for its window (a context works with any window of the same format). */
typedef struct {
    HANDLE thread;
    volatile LONG done;    /* set by the thread when its work is over (it may still be exiting) */
    DWORD app_thread;
    int pf;
    PIXELFORMATDESCRIPTOR pfd;
    HGLRC ctx;
    cui_gl_renderer *gl;
} cui__win32_gl_start;

static DWORD WINAPI cui__win32_gl_thread(LPVOID user) {
    cui__win32_gl_start *s = user;
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"CUIGLStart";
    RegisterClassExW(&wc);   /* fails harmlessly when a window made it before */
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    HDC dc = hwnd ? GetDC(hwnd) : NULL;
    s->pf = dc ? cui__win32_gl_format(dc, &s->pfd) : 0;
    if (s->pf && SetPixelFormat(dc, s->pf, &s->pfd)) s->ctx = cui__win32_gl_context(dc);
    if (s->ctx) {
        s->gl = cui_gl_create(cui__plat_gl_proc);
        if (s->gl && cui_gl_is_software(s->gl)) {
            /* OpenGL drawn on the CPU: the software renderer, which redraws only what changed, is cheaper */
            cui_gl_destroy(s->gl);
            s->gl = NULL;
        }
        if (s->gl) {
            /* one small draw: drivers compile the shaders for real on the first one */
            cui_vertex v[3];
            memset(v, 0, sizeof v);
            v[1].x = 1; v[2].y = 1;
            uint32_t idx[3] = { 0, 1, 2 };
            cui_draw_cmd cmd;
            memset(&cmd, 0, sizeof cmd);
            cmd.clip.w = 1; cmd.clip.h = 1; cmd.index_count = 3;
            cui_draw_data dd;
            memset(&dd, 0, sizeof dd);
            dd.vertices = v; dd.vertex_count = 3; dd.indices = idx; dd.index_count = 3;
            dd.commands = &cmd; dd.command_count = 1; dd.width_px = 1; dd.height_px = 1;
            cui_gl_render(s->gl, &dd, true);
            void (CUI_GLAPI_WIN32 *finish)(void) = (void (CUI_GLAPI_WIN32 *)(void))cui__plat_gl_proc("glFinish");
            if (finish) finish();
        }
        wglMakeCurrent(NULL, NULL);
        if (!s->gl) { wglDeleteContext(s->ctx); s->ctx = NULL; }
    }
    if (dc) ReleaseDC(hwnd, dc);
    if (hwnd) DestroyWindow(hwnd);
    InterlockedExchange(&s->done, 1);
    PostThreadMessageW(s->app_thread, WM_NULL, 0, 0);   /* wakes the app's loop */
    return 0;
}

bool cui__plat_gl_begin_async(cui_app *app) {
    cui__win32_gl_start *s = calloc(1, sizeof *s);
    if (!s) return false;
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);   /* this thread has a message queue, for the wake up */
    s->app_thread = GetCurrentThreadId();
    s->thread = CreateThread(NULL, 0, cui__win32_gl_thread, s, 0, NULL);
    if (!s->thread) { free(s); return false; }
    app->gl_start = s;
    return true;
}

bool cui__plat_gl_end_async(cui_app *app, bool wait, bool vsync, cui_gl_renderer **out) {
    cui__win32_gl_start *s = app->gl_start;
    *out = NULL;
    if (!s) return true;
    if (!wait && !InterlockedCompareExchange(&s->done, 0, 0)) return false;
    WaitForSingleObject(s->thread, INFINITE);
    CloseHandle(s->thread);
    cui__win32 *w = cui__w(app);
    if (s->ctx) {
        /* the window has no pixel format yet: software pictures are plain GDI */
        bool ok = w && SetPixelFormat(w->hdc, s->pf, &s->pfd) && wglMakeCurrent(w->hdc, s->ctx);
        if (ok) {
            w->glrc = s->ctx;
            cui__win32_swap_interval(vsync);
            *out = s->gl;
        } else {
            wglDeleteContext(s->ctx);   /* frees what the renderer made; its small struct is left (a failure path) */
        }
    }
    free(s);
    app->gl_start = NULL;
    return true;
}

void cui__plat_gl_swap(cui_app *app) {
    cui__win32 *w = cui__w(app);
    bool first = w->show_pending;
    cui__win32_show_now(w);
    if (cui__w32pace_get() && cui__w32pace_stats && !cui__w32pace_swaps) {
        memset(&cui__w32pace_t0, 0, sizeof cui__w32pace_t0);
        cui__w32pace_t0.cbSize = sizeof cui__w32pace_t0;
        cui__w32pace_timing(NULL, &cui__w32pace_t0);
    }
    SwapBuffers(w->hdc);
    cui__w32pace_swaps++;
    if (cui__w32pace_stats && cui__w32pace_timing) {   /* the counters only advance for a process that asks */
        DWM_TIMING_INFO t;
        memset(&t, 0, sizeof t);
        t.cbSize = sizeof t;
        cui__w32pace_timing(NULL, &t);
    }
    if (cui__w32pace_mode > 0 && cui__w32pace_vsync) cui__w32pace_wait(w);
    if (first) cui__win32_activate(w);
}

void cui__plat_present(cui_app *app, const void *bgra, int width, int height, int stride) {
    cui__win32 *w = cui__w(app);
    bool first = w->show_pending;
    cui__win32_show_now(w);
    BITMAPINFO *bmi = &w->bmi;
    memset(bmi, 0, sizeof *bmi);
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth = stride / 4;
    bmi->bmiHeader.biHeight = -height;   /* top down */
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 32;
    bmi->bmiHeader.biCompression = BI_RGB;
    SetDIBitsToDevice(w->hdc, 0, 0, (DWORD)width, (DWORD)height, 0, 0, 0, (UINT)height, bgra, bmi, DIB_RGB_COLORS);
    if (first) cui__win32_activate(w);
}

void cui__plat_metrics(cui_app *app, float *wpt, float *hpt, float *scale, int *fb_w, int *fb_h) {
    cui__win32 *w = cui__w(app);
    *scale = w->scale;
    *fb_w = w->width; *fb_h = w->height;
    *wpt = (float)w->width / w->scale;
    *hpt = (float)w->height / w->scale;
}

void cui__plat_set_title(cui_app *app, const char *title) {
    WCHAR buf[512];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, buf, 512);
    SetWindowTextW(cui__w(app)->hwnd, buf);
}

void cui__plat_set_size(cui_app *app, int width, int height) {
    cui__win32 *w = cui__w(app);
    RECT r = { 0, 0, (LONG)(width * w->scale), (LONG)(height * w->scale) };
    if (!app->desc.custom_titlebar) AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(w->hwnd, GWL_STYLE), FALSE, 0);
    SetWindowPos(w->hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

void cui__plat_set_position(cui_app *app, int x, int y) {
    cui__win32 *w = cui__w(app);
    SetWindowPos(w->hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

bool cui__plat_position(cui_app *app, int *x, int *y) {
    cui__win32 *w = cui__w(app);
    RECT r;
    if (IsIconic(w->hwnd)) {   /* a minimized window sits far off screen: report where it comes back to */
        WINDOWPLACEMENT wp;
        memset(&wp, 0, sizeof wp);
        wp.length = sizeof wp;
        if (!GetWindowPlacement(w->hwnd, &wp)) return false;
        r = wp.rcNormalPosition;
    } else if (!GetWindowRect(w->hwnd, &r)) {
        return false;
    }
    *x = (int)r.left;
    *y = (int)r.top;
    return true;
}

void *cui__plat_native(cui_app *app) { return cui__w(app)->hwnd; }

bool cui__plat_window_op(cui_app *app, int op) {
    cui__win32 *w = cui__w(app);
    if (!w || !w->hwnd) return false;
    switch (op) {
    case 0: ShowWindow(w->hwnd, SW_MINIMIZE); return true;
    case 1: ShowWindow(w->hwnd, IsZoomed(w->hwnd) ? SW_RESTORE : SW_MAXIMIZE); return true;
    case 2: return PostMessageW(w->hwnd, WM_APP + 0x51, 0, 0) != 0;
    case 3: return IsZoomed(w->hwnd) != 0;
    case 4: return app->desc.custom_titlebar;
    default: return false;
    }
}

void cui__plat_wait(cui_app *app, double timeout) {
    if (timeout != 0) {
        /* QS_ALLINPUT without QS_RAWINPUT: CUI does not register for raw input, and Wine
         * leaves that bit set, which made every wait return at once */
        DWORD mask = QS_KEY | QS_MOUSE | QS_POSTMESSAGE | QS_TIMER | QS_PAINT | QS_SENDMESSAGE | QS_HOTKEY | QS_ALLPOSTMESSAGE;
        cui__win32 *w = cui__w(app);
        if (timeout > 0 && !w->timer) {
            /* a plain timed wait rounds up to the 15.6 ms system tick, which turns 60 Hz pacing into 30 to 50 */
            PFN_CreateWaitableTimerExW cwt = (PFN_CreateWaitableTimerExW)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateWaitableTimerExW");
            if (cwt) w->timer = cwt(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
            if (!w->timer) w->timer = CreateWaitableTimerW(NULL, TRUE, NULL);
        }
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(timeout * 1e7);   /* relative, in 100 ns */
        if (timeout > 0 && w->timer && SetWaitableTimer(w->timer, &due, 0, NULL, NULL, FALSE))
            MsgWaitForMultipleObjectsEx(1, &w->timer, INFINITE, mask, MWMO_INPUTAVAILABLE);
        else
            MsgWaitForMultipleObjectsEx(0, NULL, timeout < 0 ? INFINITE : (DWORD)(timeout * 1000.0 + 0.5), mask, MWMO_INPUTAVAILABLE);
    }
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { app->running = false; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool cui__plat_present_part(cui_app *app, const void *bgra, int width, int height, int stride, int x, int y, int pw, int ph) {
    cui__win32 *w = cui__w(app);
    if (w->show_pending || x < 0 || y < 0 || pw <= 0 || ph <= 0 || x + pw > width || y + ph > height) return false;
    /* the rows from y on, as a top down DIB of ph rows: the source offset of a top down DIB is
     * counted differently by different drivers, a pointer to the first row is not */
    BITMAPINFO *bmi = &w->bmi;
    memset(bmi, 0, sizeof *bmi);
    bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi->bmiHeader.biWidth = stride / 4;
    bmi->bmiHeader.biHeight = -ph;
    bmi->bmiHeader.biPlanes = 1;
    bmi->bmiHeader.biBitCount = 32;
    bmi->bmiHeader.biCompression = BI_RGB;
    const uint8_t *rows = (const uint8_t *)bgra + (size_t)y * (size_t)stride;
    return SetDIBitsToDevice(w->hdc, x, y, (DWORD)pw, (DWORD)ph, x, 0, 0, (UINT)ph, rows, bmi, DIB_RGB_COLORS) != 0;
}

double cui__plat_refresh_hz(cui_app *app) {
    /* the compositor's rate is the real one (a 75 Hz panel reported 60 through VREFRESH) */
    cui__w32pace_get();
    if (cui__w32pace_timing) {
        DWM_TIMING_INFO t;
        memset(&t, 0, sizeof t);
        t.cbSize = sizeof t;
        if (SUCCEEDED(cui__w32pace_timing(NULL, &t)) && t.rateRefresh.uiDenominator && t.rateRefresh.uiNumerator)
            return (double)t.rateRefresh.uiNumerator / (double)t.rateRefresh.uiDenominator;
    }
    return GetDeviceCaps(cui__w(app)->hdc, VREFRESH);   /* 0 or 1 means the hardware default: unknown */
}

void cui__plat_gl_release(cui_app *app) {
    cui__win32 *w = cui__w(app);
    if (w->glrc) { wglMakeCurrent(NULL, NULL); wglDeleteContext(w->glrc); w->glrc = NULL; }
}
