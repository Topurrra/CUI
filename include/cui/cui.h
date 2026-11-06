/*
 * CUI: modern native UI components for C.
 *
 * One header for the whole component library. The windowing layer lives in
 * cui_app.h and the renderers in cui_render.h, so you can use CUI with the
 * built-in window backends or plug it into your own engine.
 *
 *     #include <cui/cui.h>
 *     #include <cui/cui_app.h>
 *
 *     int main(void) {
 *         cui_app *app = cui_app_create(&(cui_app_desc){ .title = "Hello" });
 *         bool on = false;
 *         while (cui_app_frame(app)) {
 *             cui_ctx *ui = cui_app_ui(app);
 *             cui_column(ui, .pad = 24, .gap = 12) {
 *                 cui_heading(ui, "Hello, CUI", 2);
 *                 cui_switch(ui, "Notifications", &on);
 *                 if (cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY))
 *                     cui_toast(ui, "Saved", .tone = CUI_TONE_SUCCESS);
 *             }
 *         }
 *         cui_app_destroy(app);
 *     }
 *
 * Conventions
 *   - Immediate mode: call the component functions every frame. They return
 *     what happened (clicked, changed) and read and write your own variables.
 *   - Every component that takes options has a macro form with named
 *     arguments: cui_button(ui, "Save", .variant = CUI_BUTTON_PRIMARY).
 *     The *_ex function takes a pointer to the options struct (handy from C++).
 *   - Containers use a scope: cui_row(ui, .gap = 8) { ... }. Do not `return`
 *     or `break` out of a scope; use the explicit begin/end functions if you
 *     need early exits.
 *   - Units are logical points. CUI scales everything for high DPI screens.
 *   - Strings are UTF-8. "Label##key" shows "Label" but uses the whole
 *     string as the identity, so two buttons can share a visible label.
 *     "Label###key" uses only "###key", so the label can change (a count,
 *     a translation) while the widget keeps its state and focus.
 *
 * MIT License. See LICENSE.
 */
#ifndef CUI_H
#define CUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#include "cui_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CUI_VERSION_MAJOR 1
#define CUI_VERSION_MINOR 0
#define CUI_VERSION_PATCH 0
#define CUI_VERSION_STRING "1.0.0"

#ifndef CUI_API
#  if defined(_WIN32) && defined(CUI_SHARED) && defined(CUI_BUILDING)
#    define CUI_API __declspec(dllexport)
#  elif defined(_WIN32) && defined(CUI_SHARED)
#    define CUI_API __declspec(dllimport)
#  elif defined(__GNUC__) && defined(CUI_SHARED)
#    define CUI_API __attribute__((visibility("default")))
#  else
#    define CUI_API
#  endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define CUI_PRINTF(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#  define CUI_PRINTF(fmt, args)
#endif

/* ------------------------------------------------------------------------ */
/* Basic types                                                              */
/* ------------------------------------------------------------------------ */

typedef struct cui_ctx cui_ctx;
typedef uint32_t cui_id;

/* Compound literals in C, brace initialization in C++. */
#ifdef __cplusplus
#  define CUI__LIT(T) T
#else
#  define CUI__LIT(T) (T)
#endif

typedef struct cui_vec2 { float x, y; } cui_vec2;
typedef struct cui_rect { float x, y, w, h; } cui_rect;
typedef struct cui_color { uint8_t r, g, b, a; } cui_color;

#define CUI_RGB(hex)  (CUI__LIT(cui_color){ (uint8_t)(((hex) >> 16) & 0xff), (uint8_t)(((hex) >> 8) & 0xff), (uint8_t)((hex) & 0xff), 255 })
#define CUI_RGBA(hex) (CUI__LIT(cui_color){ (uint8_t)(((hex) >> 24) & 0xff), (uint8_t)(((hex) >> 16) & 0xff), (uint8_t)(((hex) >> 8) & 0xff), (uint8_t)((hex) & 0xff) })
#define CUI_TRANSPARENT (CUI__LIT(cui_color){ 0, 0, 0, 0 })

/* Semantic colors. Use these instead of raw colors so your UI follows the
 * theme (light/dark) and the accent. They can go anywhere a cui_color goes. */
typedef enum cui_token {
    CUI_TOKEN_NONE = 0,
    CUI_TOKEN_BACKGROUND, CUI_TOKEN_SURFACE, CUI_TOKEN_SURFACE_RAISED, CUI_TOKEN_SURFACE_MUTED,
    CUI_TOKEN_FOREGROUND, CUI_TOKEN_TEXT_SECONDARY, CUI_TOKEN_TEXT_MUTED,
    CUI_TOKEN_BORDER, CUI_TOKEN_BORDER_SUBTLE, CUI_TOKEN_BORDER_STRONG,
    CUI_TOKEN_ACCENT, CUI_TOKEN_ACCENT_STRONG, CUI_TOKEN_ACCENT_SUBTLE, CUI_TOKEN_ACCENT_FOREGROUND,
    CUI_TOKEN_SUCCESS, CUI_TOKEN_WARNING, CUI_TOKEN_DANGER, CUI_TOKEN_INFO,
    CUI_TOKEN_CONTROL_ON, CUI_TOKEN_CONTROL_GLYPH, CUI_TOKEN_CONTROL_TRACK,
    CUI_TOKEN_CONTROL_TRACK_HOVER, CUI_TOKEN_CONTROL_THUMB, CUI_TOKEN_CONTROL_THUMB_ON,
    CUI_TOKEN_HOVER, CUI_TOKEN_ACTIVE, CUI_TOKEN_SCRIM, CUI_TOKEN_FOCUS_RING,
    CUI_TOKEN_SERIES_1, CUI_TOKEN_SERIES_2, CUI_TOKEN_SERIES_3, CUI_TOKEN_SERIES_4,
    CUI_TOKEN_SERIES_5, CUI_TOKEN_SERIES_6,
    CUI_TOKEN_COUNT
} cui_token;

#define CUI_TOKEN_COLOR(t) (CUI__LIT(cui_color){ (uint8_t)(t), 0xC5, 0x1D, 0 })
#define CUI_COLOR_BACKGROUND      CUI_TOKEN_COLOR(CUI_TOKEN_BACKGROUND)
#define CUI_COLOR_SURFACE         CUI_TOKEN_COLOR(CUI_TOKEN_SURFACE)
#define CUI_COLOR_SURFACE_RAISED  CUI_TOKEN_COLOR(CUI_TOKEN_SURFACE_RAISED)
#define CUI_COLOR_SURFACE_MUTED   CUI_TOKEN_COLOR(CUI_TOKEN_SURFACE_MUTED)
#define CUI_COLOR_FOREGROUND      CUI_TOKEN_COLOR(CUI_TOKEN_FOREGROUND)
#define CUI_COLOR_TEXT_SECONDARY  CUI_TOKEN_COLOR(CUI_TOKEN_TEXT_SECONDARY)
#define CUI_COLOR_TEXT_MUTED      CUI_TOKEN_COLOR(CUI_TOKEN_TEXT_MUTED)
#define CUI_COLOR_BORDER          CUI_TOKEN_COLOR(CUI_TOKEN_BORDER)
#define CUI_COLOR_BORDER_SUBTLE   CUI_TOKEN_COLOR(CUI_TOKEN_BORDER_SUBTLE)
#define CUI_COLOR_BORDER_STRONG   CUI_TOKEN_COLOR(CUI_TOKEN_BORDER_STRONG)
#define CUI_COLOR_ACCENT          CUI_TOKEN_COLOR(CUI_TOKEN_ACCENT)
#define CUI_COLOR_ACCENT_STRONG   CUI_TOKEN_COLOR(CUI_TOKEN_ACCENT_STRONG)
#define CUI_COLOR_ACCENT_SUBTLE   CUI_TOKEN_COLOR(CUI_TOKEN_ACCENT_SUBTLE)
#define CUI_COLOR_ACCENT_FOREGROUND CUI_TOKEN_COLOR(CUI_TOKEN_ACCENT_FOREGROUND)
#define CUI_COLOR_SUCCESS         CUI_TOKEN_COLOR(CUI_TOKEN_SUCCESS)
#define CUI_COLOR_WARNING         CUI_TOKEN_COLOR(CUI_TOKEN_WARNING)
#define CUI_COLOR_DANGER          CUI_TOKEN_COLOR(CUI_TOKEN_DANGER)
#define CUI_COLOR_INFO            CUI_TOKEN_COLOR(CUI_TOKEN_INFO)
#define CUI_COLOR_HOVER           CUI_TOKEN_COLOR(CUI_TOKEN_HOVER)
#define CUI_COLOR_SCRIM           CUI_TOKEN_COLOR(CUI_TOKEN_SCRIM)
#define CUI_COLOR_SERIES(n)       CUI_TOKEN_COLOR(CUI_TOKEN_SERIES_1 + ((n) % 6))

/* ------------------------------------------------------------------------ */
/* Context and frame                                                        */
/* ------------------------------------------------------------------------ */

typedef enum cui_theme_mode { CUI_THEME_LIGHT = 0, CUI_THEME_DARK = 1 } cui_theme_mode;

typedef enum cui_accent {
    CUI_ACCENT_NEUTRAL = 0, CUI_ACCENT_VIOLET, CUI_ACCENT_BLUE, CUI_ACCENT_GREEN,
    CUI_ACCENT_AMBER, CUI_ACCENT_ORANGE, CUI_ACCENT_CORAL, CUI_ACCENT_ROSE,
    CUI_ACCENT_COUNT
} cui_accent;

typedef enum cui_texture_format { CUI_TEXTURE_R8 = 1, CUI_TEXTURE_RGBA8 = 4 } cui_texture_format;
typedef uintptr_t cui_texture;

/* How CUI talks to a renderer. The built-in renderers fill this in for you
 * (cui_render_gl_iface / cui_render_sw_iface). Implement it to use CUI with
 * any graphics API. */
typedef struct cui_renderer_iface {
    void *user;
    cui_texture (*texture_create)(void *user, int w, int h, cui_texture_format fmt, const void *pixels);
    void (*texture_update)(void *user, cui_texture tex, int x, int y, int w, int h, const void *pixels, int stride);
    void (*texture_destroy)(void *user, cui_texture tex);
} cui_renderer_iface;

typedef struct cui_allocator {
    void *(*alloc)(size_t size, void *user);
    void *(*realloc)(void *ptr, size_t size, void *user);
    void (*free)(void *ptr, void *user);
    void *user;
} cui_allocator;

typedef struct cui_ctx_desc {
    cui_renderer_iface renderer;      /* required for drawing; may be set later */
    cui_allocator allocator;          /* optional, defaults to malloc/free */
    cui_theme_mode theme;
    cui_accent accent;
    bool no_default_fonts;            /* skip the embedded Inter/Geist/Lucide fonts */
} cui_ctx_desc;

CUI_API cui_ctx *cui_create(const cui_ctx_desc *desc);
CUI_API void cui_destroy(cui_ctx *ui);
CUI_API void cui_set_renderer(cui_ctx *ui, const cui_renderer_iface *renderer);

/* Size of the drawable area in logical points, and the scale to pixels
 * (2.0 on a typical retina display). */
CUI_API void cui_set_viewport(cui_ctx *ui, float width, float height, float scale);
CUI_API cui_vec2 cui_viewport_size(cui_ctx *ui);
CUI_API float cui_dpi_scale(cui_ctx *ui);

/* dt is the time since the previous frame in seconds. */
CUI_API void cui_begin_frame(cui_ctx *ui, float dt);
CUI_API void cui_end_frame(cui_ctx *ui);

/* True while something animates or input is still settling. Event driven
 * apps can sleep until the next input event when this is false. */
CUI_API bool cui_needs_redraw(cui_ctx *ui);
/* Seconds until CUI wants a frame (for caret blink, tooltips, toasts).
 * Negative means "only on input". */
CUI_API float cui_next_wakeup(cui_ctx *ui);
CUI_API void cui_request_redraw(cui_ctx *ui);
CUI_API double cui_time(cui_ctx *ui);
CUI_API float cui_delta_time(cui_ctx *ui);   /* seconds since the last frame */
CUI_API uint64_t cui_frame_count(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Input (called by the platform layer)                                     */
/* ------------------------------------------------------------------------ */

typedef enum cui_mouse_button { CUI_MOUSE_LEFT = 0, CUI_MOUSE_RIGHT = 1, CUI_MOUSE_MIDDLE = 2 } cui_mouse_button;

typedef enum cui_key {
    CUI_KEY_NONE = 0,
    CUI_KEY_TAB, CUI_KEY_ENTER, CUI_KEY_ESCAPE, CUI_KEY_BACKSPACE, CUI_KEY_DELETE, CUI_KEY_SPACE,
    CUI_KEY_LEFT, CUI_KEY_RIGHT, CUI_KEY_UP, CUI_KEY_DOWN,
    CUI_KEY_HOME, CUI_KEY_END, CUI_KEY_PAGE_UP, CUI_KEY_PAGE_DOWN,
    CUI_KEY_A, CUI_KEY_B, CUI_KEY_C, CUI_KEY_D, CUI_KEY_E, CUI_KEY_F, CUI_KEY_G, CUI_KEY_H, CUI_KEY_I,
    CUI_KEY_J, CUI_KEY_K, CUI_KEY_L, CUI_KEY_M, CUI_KEY_N, CUI_KEY_O, CUI_KEY_P, CUI_KEY_Q, CUI_KEY_R,
    CUI_KEY_S, CUI_KEY_T, CUI_KEY_U, CUI_KEY_V, CUI_KEY_W, CUI_KEY_X, CUI_KEY_Y, CUI_KEY_Z,
    CUI_KEY_0, CUI_KEY_1, CUI_KEY_2, CUI_KEY_3, CUI_KEY_4, CUI_KEY_5, CUI_KEY_6, CUI_KEY_7, CUI_KEY_8, CUI_KEY_9,
    CUI_KEY_MINUS, CUI_KEY_EQUAL, CUI_KEY_COMMA, CUI_KEY_PERIOD, CUI_KEY_SLASH,
    CUI_KEY_F1, CUI_KEY_F2, CUI_KEY_F3, CUI_KEY_F4, CUI_KEY_F5, CUI_KEY_F6,
    CUI_KEY_F7, CUI_KEY_F8, CUI_KEY_F9, CUI_KEY_F10, CUI_KEY_F11, CUI_KEY_F12,
    CUI_KEY_COUNT
} cui_key;

typedef enum cui_mod {
    CUI_MOD_SHIFT = 1 << 0,
    CUI_MOD_CTRL  = 1 << 1,
    CUI_MOD_ALT   = 1 << 2,
    CUI_MOD_SUPER = 1 << 3   /* Command on macOS, Windows key elsewhere */
} cui_mod;

/* The "primary" shortcut modifier: Command on macOS, Ctrl elsewhere. */
#if defined(__APPLE__)
#  define CUI_MOD_PRIMARY CUI_MOD_SUPER
#else
#  define CUI_MOD_PRIMARY CUI_MOD_CTRL
#endif

typedef enum cui_cursor {
    CUI_CURSOR_ARROW = 0, CUI_CURSOR_HAND, CUI_CURSOR_TEXT, CUI_CURSOR_RESIZE_EW,
    CUI_CURSOR_RESIZE_NS, CUI_CURSOR_MOVE, CUI_CURSOR_NOT_ALLOWED, CUI_CURSOR_COUNT
} cui_cursor;

CUI_API void cui_input_mouse_move(cui_ctx *ui, float x, float y);
CUI_API void cui_input_mouse_button(cui_ctx *ui, cui_mouse_button button, bool down);
CUI_API void cui_input_mouse_leave(cui_ctx *ui);
CUI_API void cui_input_scroll(cui_ctx *ui, float dx, float dy);  /* in lines; +dy scrolls up */
CUI_API void cui_input_key(cui_ctx *ui, cui_key key, bool down, int mods);
CUI_API void cui_input_text(cui_ctx *ui, const char *utf8);
CUI_API void cui_input_focus(cui_ctx *ui, bool window_focused);
/* Files dragged from the OS: hover while a drag is over the window (where the
 * platform reports it), then the drop itself. Paths are UTF-8 and copied. */
CUI_API void cui_input_file_hover(cui_ctx *ui, bool hovering, float x, float y);
CUI_API void cui_input_file_drop(cui_ctx *ui, const char *const *paths, int count, float x, float y);
/* Files dropped this frame (dropzones claim theirs first). */
CUI_API int cui_dropped_file_count(cui_ctx *ui);
CUI_API const char *cui_dropped_file(cui_ctx *ui, int index);
CUI_API bool cui_file_drag_active(cui_ctx *ui);   /* files are being dragged over the window */

/* Platform services. The built-in backends set these. */
typedef struct cui_platform_iface {
    void *user;
    void (*set_clipboard)(void *user, const char *utf8);
    const char *(*get_clipboard)(void *user);   /* returned string stays valid until next call */
    void (*set_cursor)(void *user, cui_cursor cursor);
    void (*set_text_input_rect)(void *user, cui_rect caret);  /* IME candidate window position */
} cui_platform_iface;
CUI_API void cui_set_platform(cui_ctx *ui, const cui_platform_iface *platform);
CUI_API cui_cursor cui_get_cursor(cui_ctx *ui);

/* Queries, useful for app level shortcuts. */
CUI_API bool cui_key_pressed(cui_ctx *ui, cui_key key);          /* pressed this frame (with repeat) */
CUI_API bool cui_key_down(cui_ctx *ui, cui_key key);
CUI_API bool cui_shortcut(cui_ctx *ui, int mods, cui_key key);   /* e.g. cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K) */
CUI_API int cui_key_mods(cui_ctx *ui);
CUI_API cui_vec2 cui_mouse_pos(cui_ctx *ui);
CUI_API bool cui_mouse_down(cui_ctx *ui, cui_mouse_button b);
CUI_API bool cui_mouse_clicked(cui_ctx *ui, cui_mouse_button b);
/* True when a text field has keyboard focus (so the app should not treat
 * typing as shortcuts). */
CUI_API bool cui_wants_keyboard(cui_ctx *ui);
CUI_API bool cui_wants_mouse(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Theme                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct cui_shadow { float y, blur, spread; float alpha; } cui_shadow;

typedef struct cui_theme {
    cui_color colors[CUI_TOKEN_COUNT];
    cui_color tone_sky, tone_teal, tone_green, tone_amber, tone_rose, tone_orchid, tone_violet;

    float radius_control;   /* buttons, inputs                (default 10) */
    float radius_panel;     /* menus, popovers, nested panels (default 14) */
    float radius_surface;   /* cards, dialogs                 (default 18) */

    float control_sm, control_md, control_lg;   /* control heights (30/36/44) */
    float text_xs, text_sm, text_body, text_lg; /* 11/12/13/15 */
    float heading[4];                           /* h1..h4: 36/28/22/17 */

    cui_shadow shadow_resting[2], shadow_raised[2], shadow_floating[2];

    float motion;           /* 1 = normal, 0 = reduced motion (no travel), >1 slow motion */
    float font_scale;       /* user text scale multiplier (default 1) */
} cui_theme;

CUI_API void cui_set_theme(cui_ctx *ui, cui_theme_mode mode);
CUI_API cui_theme_mode cui_get_theme_mode(cui_ctx *ui);
CUI_API void cui_set_accent(cui_ctx *ui, cui_accent accent);
CUI_API cui_accent cui_get_accent(cui_ctx *ui);
/* The active theme. Edit it to customize; changes apply next frame. Theme
 * changes through cui_set_theme/cui_set_accent animate smoothly. */
CUI_API cui_theme *cui_theme_get(cui_ctx *ui);
CUI_API void cui_set_reduced_motion(cui_ctx *ui, bool reduced);
CUI_API cui_color cui_color_of(cui_ctx *ui, cui_color c);   /* resolves tokens */
CUI_API cui_color cui_token_color(cui_ctx *ui, cui_token token);
CUI_API const char *cui_accent_name(cui_accent accent);

/* Color helpers. Mixing is done in OKLab, like CSS color-mix(in oklab). */
CUI_API cui_color cui_oklch(float l, float c, float h_degrees, float alpha);
CUI_API cui_color cui_mix(cui_color a, cui_color b, float t);
CUI_API cui_color cui_alpha(cui_color c, float alpha_multiplier);

/* ------------------------------------------------------------------------ */
/* Fonts                                                                    */
/* ------------------------------------------------------------------------ */

typedef int cui_font;
enum {
    CUI_FONT_BODY = 0,     /* Inter Regular  */
    CUI_FONT_MEDIUM = 1,   /* Inter Medium   */
    CUI_FONT_DISPLAY = 2,  /* Geist Medium, for headings 28pt and up */
    CUI_FONT_MONO = 3,     /* Geist Mono     */
    CUI_FONT_ICONS = 4     /* Lucide icons   */
};

/* Load a TrueType/OpenType font. The data must stay alive if copy is false.
 * Returns a font id, or -1 on failure. */
CUI_API cui_font cui_font_load_memory(cui_ctx *ui, const void *data, size_t size, bool copy);
CUI_API cui_font cui_font_load_file(cui_ctx *ui, const char *path);
/* Replace one of the built-in roles (e.g. use your brand font as CUI_FONT_BODY). */
CUI_API void cui_font_set_role(cui_ctx *ui, cui_font role, cui_font font);
/* Glyphs missing from `font` are looked up in `fallback` (e.g. CJK, emoji). */
CUI_API void cui_font_add_fallback(cui_ctx *ui, cui_font font, cui_font fallback);

typedef struct cui_text_metrics { float width, height, ascent, descent, line_height; } cui_text_metrics;
CUI_API cui_text_metrics cui_measure_text(cui_ctx *ui, cui_font font, float size, const char *text, int len);

/* ------------------------------------------------------------------------ */
/* Layout                                                                   */
/* ------------------------------------------------------------------------ */

typedef enum cui_size_kind { CUI_SIZE_FIT = 0, CUI_SIZE_GROW, CUI_SIZE_FIXED, CUI_SIZE_PERCENT } cui_size_kind;
typedef struct cui_size { cui_size_kind kind; float value; } cui_size;

#define CUI_FIT       (CUI__LIT(cui_size){ CUI_SIZE_FIT, 0 })
#define CUI_FILL      (CUI__LIT(cui_size){ CUI_SIZE_GROW, 1 })
#define CUI_GROW(w)   (CUI__LIT(cui_size){ CUI_SIZE_GROW, (float)(w) })
#define CUI_PX(v)     (CUI__LIT(cui_size){ CUI_SIZE_FIXED, (float)(v) })
#define CUI_PCT(p)    (CUI__LIT(cui_size){ CUI_SIZE_PERCENT, (float)(p) })   /* 0..100 of parent */

typedef enum cui_dir { CUI_COLUMN = 0, CUI_ROW, CUI_STACK, CUI_GRID } cui_dir;
/* CUI_ALIGN_DEFAULT: columns stretch their children, rows center them. */
typedef enum cui_align { CUI_ALIGN_DEFAULT = 0, CUI_ALIGN_START, CUI_ALIGN_CENTER, CUI_ALIGN_END, CUI_ALIGN_STRETCH } cui_align;
typedef enum cui_justify {
    CUI_JUSTIFY_START = 0, CUI_JUSTIFY_CENTER, CUI_JUSTIFY_END, CUI_JUSTIFY_BETWEEN, CUI_JUSTIFY_AROUND
} cui_justify;

typedef enum cui_elevation { CUI_ELEVATION_NONE = 0, CUI_ELEVATION_RESTING, CUI_ELEVATION_RAISED, CUI_ELEVATION_FLOATING } cui_elevation;

/* Options for every container. All fields are optional. */
typedef struct cui_box {
    int _;
    const char *id;            /* identity for scroll state etc. (defaults to an automatic id) */
    cui_dir dir;               /* CUI_COLUMN (default), CUI_ROW, CUI_STACK, CUI_GRID */
    cui_size width, height;    /* default: fit content (columns stretch children horizontally) */
    float min_width, max_width, min_height, max_height;
    float pad, pad_x, pad_y, pad_top, pad_right, pad_bottom, pad_left;
    float gap;                 /* space between children (and between wrapped lines) */
    int columns;               /* CUI_GRID: column count (with min_column_width: the most columns) */
    float min_column_width;    /* CUI_GRID: as many columns as fit at this width, at least one */
    bool wrap;                 /* CUI_ROW: children that do not fit move to a new line */
    cui_align align;           /* cross axis alignment of children */
    cui_justify justify;       /* main axis distribution */
    cui_color bg;              /* background */
    cui_color border_color;    /* defaults to CUI_COLOR_BORDER when border > 0 */
    float border;              /* border width */
    float radius;              /* corner radius; use radius_tl.. for single corners */
    float radius_tl, radius_tr, radius_br, radius_bl;
    cui_elevation elevation;
    bool scroll_x, scroll_y;   /* make the box a scroll area (clips content) */
    bool clip;                 /* clip children to the box */
    float opacity;             /* 0 means 1 (fully opaque) */
} cui_box;

CUI_API cui_id cui_box_begin(cui_ctx *ui, const cui_box *opts);
CUI_API void cui_box_end(cui_ctx *ui);
CUI_API cui_id cui_row_begin(cui_ctx *ui, const cui_box *opts);
CUI_API cui_id cui_column_begin(cui_ctx *ui, const cui_box *opts);
CUI_API void cui_spacer(cui_ctx *ui);                         /* flexible space */
CUI_API void cui_gap(cui_ctx *ui, float size);                /* fixed space */
CUI_API void cui_separator(cui_ctx *ui);                      /* hairline, follows the parent direction */

/* Card: a bordered surface with generous padding. */
typedef struct cui_card_opts {
    int _;
    const char *id;
    cui_size width, height;
    float min_width, max_width;  /* e.g. CUI_FILL up to max_width: fills narrow screens, stops on wide ones */
    float pad, pad_x, pad_y, gap;
    cui_dir dir;
    cui_align align;
    bool wrap;                 /* CUI_ROW cards: children that do not fit move to a new line */
    bool hoverable;            /* lifts slightly on hover and returns clicks */
    bool muted;                /* muted surface, no border */
    bool selected;             /* accent border */
} cui_card_opts;
CUI_API bool cui_card_begin(cui_ctx *ui, const cui_card_opts *opts);   /* returns true when clicked (hoverable) */
CUI_API void cui_card_end(cui_ctx *ui);

/* Scroll area with smooth scrolling and overlay scrollbars. */
typedef struct cui_scroll_opts {
    int _;
    const char *id;
    cui_size width, height;
    bool horizontal;            /* also scroll horizontally */
    float pad, gap;
    bool fade_edges;            /* content fades out at an edge with more to scroll */
    cui_color fade_color;       /* what it fades into (default: the nearest opaque box around it) */
} cui_scroll_opts;
CUI_API void cui_scroll_begin(cui_ctx *ui, const cui_scroll_opts *opts);
CUI_API void cui_scroll_end(cui_ctx *ui);
CUI_API void cui_scroll_to(cui_ctx *ui, const char *scroll_id, float y);

/* Resizable split view: two panes with a draggable divider. `ratio` is the
 * size of the first pane in 0..1. Call cui_split_next between the panes. */
typedef struct cui_split_opts { int _; bool vertical; float min_first, min_second; cui_size width, height; } cui_split_opts;
CUI_API void cui_split_begin(cui_ctx *ui, const char *id, float *ratio, const cui_split_opts *opts);
CUI_API void cui_split_next(cui_ctx *ui);
CUI_API void cui_split_end(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Identity                                                                 */
/* ------------------------------------------------------------------------ */

CUI_API cui_id cui_make_id(cui_ctx *ui, const char *str);
CUI_API void cui_push_id(cui_ctx *ui, const char *str);
CUI_API void cui_push_id_int(cui_ctx *ui, int value);
CUI_API void cui_pop_id(cui_ctx *ui);
/* The id and rectangle of the most recent component (for tooltips, popovers). */
CUI_API cui_id cui_last_id(cui_ctx *ui);
CUI_API cui_rect cui_last_rect(cui_ctx *ui);
/* Rectangle of any component or box id from the latest layout. */
CUI_API cui_rect cui_get_rect(cui_ctx *ui, cui_id id);
CUI_API bool cui_last_hovered(cui_ctx *ui);
CUI_API bool cui_is_disabled(cui_ctx *ui);
CUI_API void cui_push_disabled(cui_ctx *ui, bool disabled);
CUI_API void cui_pop_disabled(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Text                                                                     */
/* ------------------------------------------------------------------------ */

typedef enum cui_text_align { CUI_TEXT_LEFT = 0, CUI_TEXT_CENTER, CUI_TEXT_RIGHT } cui_text_align;

typedef struct cui_text_opts {
    int _;
    cui_font font;             /* default CUI_FONT_BODY (0) */
    float size;                /* default theme text_body */
    cui_color color;           /* default foreground */
    cui_text_align align;
    bool nowrap;               /* single line, ellipsis when it does not fit */
    bool tabular;              /* tabular numbers */
    bool selectable;           /* reserved */
    cui_size width;
    float line_height;         /* multiplier, default 1.45 */
} cui_text_opts;

CUI_API void cui_text_ex(cui_ctx *ui, const char *text, const cui_text_opts *opts);
CUI_API void cui_textf(cui_ctx *ui, const char *fmt, ...) CUI_PRINTF(2, 3);
CUI_API void cui_heading(cui_ctx *ui, const char *text, int level);          /* 1..4 */
CUI_API void cui_label(cui_ctx *ui, const char *text);                       /* medium weight */
CUI_API void cui_secondary(cui_ctx *ui, const char *text);                   /* secondary color */
CUI_API void cui_muted(cui_ctx *ui, const char *text);                       /* small, muted */
CUI_API void cui_code(cui_ctx *ui, const char *text);                        /* inline code */
CUI_API void cui_kbd(cui_ctx *ui, const char *keys);                         /* keyboard keycap, e.g. "Ctrl K" */
CUI_API bool cui_link(cui_ctx *ui, const char *text);
CUI_API void cui_icon(cui_ctx *ui, const char *icon, float size, cui_color color);

/* ------------------------------------------------------------------------ */
/* Buttons                                                                  */
/* ------------------------------------------------------------------------ */

typedef enum cui_button_variant {
    CUI_BUTTON_DEFAULT = 0,    /* secondary for buttons, ghost for icon buttons */
    CUI_BUTTON_SECONDARY,      /* neutral filled */
    CUI_BUTTON_PRIMARY,        /* the one main action */
    CUI_BUTTON_OUTLINE,
    CUI_BUTTON_GHOST,
    CUI_BUTTON_DANGER,
    CUI_BUTTON_LINK
} cui_button_variant;

typedef enum cui_control_size { CUI_SIZE_MD = 0, CUI_SIZE_SM, CUI_SIZE_LG } cui_control_size;

typedef struct cui_button_opts {
    int _;
    cui_button_variant variant;
    cui_control_size size;
    const char *icon;          /* leading icon, e.g. CUI_ICON_PLUS */
    const char *icon_right;    /* trailing icon */
    bool disabled;
    bool loading;              /* spinner, ignores clicks */
    bool full_width;
    bool selected;             /* toggled look */
    const char *tooltip;
    const char *shortcut;      /* shown as a hint in the tooltip, e.g. "Ctrl S" */
    cui_size width;
    cui_color color;           /* text and icon color, e.g. CUI_COLOR_ACCENT for a toggled icon */
} cui_button_opts;

CUI_API bool cui_button_ex(cui_ctx *ui, const char *label, const cui_button_opts *opts);
/* Square/round button with only an icon. Give it a tooltip so it has a name. */
CUI_API bool cui_icon_button_ex(cui_ctx *ui, const char *icon, const cui_button_opts *opts);

/* Segmented control: a row of options with a gliding highlight. */
typedef struct cui_segmented_opts { int _; cui_control_size size; bool full_width; const char *const *icons; } cui_segmented_opts;
CUI_API bool cui_segmented_ex(cui_ctx *ui, const char *id, const char *const *items, int count, int *index, const cui_segmented_opts *opts);

/* ------------------------------------------------------------------------ */
/* Selection controls                                                       */
/* ------------------------------------------------------------------------ */

typedef struct cui_check_opts { int _; const char *description; bool disabled; bool indeterminate; } cui_check_opts;
CUI_API bool cui_checkbox_ex(cui_ctx *ui, const char *label, bool *value, const cui_check_opts *opts);
CUI_API bool cui_switch_ex(cui_ctx *ui, const char *label, bool *value, const cui_check_opts *opts);
CUI_API bool cui_radio_ex(cui_ctx *ui, const char *label, int *value, int option, const cui_check_opts *opts);
CUI_API bool cui_radio_group(cui_ctx *ui, const char *id, const char *const *items, int count, int *index);

typedef struct cui_slider_opts {
    int _;
    float step;                /* 0 = continuous */
    bool show_value;
    const char *format;        /* printf format for the value, default "%.0f" */
    bool disabled;
    cui_size width;
} cui_slider_opts;
/* Elastic slider: the track stretches like rubber when dragged past its ends. */
CUI_API bool cui_slider_ex(cui_ctx *ui, const char *label, float *value, float min, float max, const cui_slider_opts *opts);
CUI_API bool cui_range_slider(cui_ctx *ui, const char *label, float *lo, float *hi, float min, float max);
CUI_API bool cui_rating(cui_ctx *ui, const char *id, int *value, int max);

typedef struct cui_chip_opts { int _; const char *icon; bool disabled; } cui_chip_opts;
CUI_API bool cui_chip_ex(cui_ctx *ui, const char *label, bool *selected, const cui_chip_opts *opts);

/* ------------------------------------------------------------------------ */
/* Text input                                                               */
/* ------------------------------------------------------------------------ */

enum { CUI_CHANGED = 1, CUI_SUBMITTED = 2, CUI_FOCUSED = 4, CUI_BLURRED = 8 };

typedef struct cui_input_opts {
    int _;
    const char *label;         /* label above the field */
    const char *placeholder;
    const char *icon;          /* leading icon */
    const char *hint;          /* helper text below */
    const char *error;         /* error text below (red) */
    bool password;
    bool disabled;
    bool clearable;            /* x button when not empty */
    bool autofocus;
    bool read_only;
    cui_control_size size;
    cui_size width;
    float height;              /* textarea: height in points; negative fills the space */
    const char *prefix;        /* muted text before the value, e.g. "$" or "https://" */
    const char *suffix;        /* muted text after the value, e.g. "USD" or "kg" */
} cui_input_opts;

/* Single line text field. Returns CUI_CHANGED / CUI_SUBMITTED (Enter) flags. */
CUI_API int cui_input_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *opts);
/* Multi-line text area with word wrap. */
CUI_API int cui_textarea_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *opts);
/* Search field with a search icon and a clear button. */
CUI_API int cui_search_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_input_opts *opts);

typedef struct cui_number_opts { int _; const char *label; double step; int decimals; bool disabled; cui_size width; const char *suffix; } cui_number_opts;
CUI_API bool cui_number_ex(cui_ctx *ui, const char *id, double *value, double min, double max, const cui_number_opts *opts);

/* ------------------------------------------------------------------------ */
/* Select, combobox, date picker                                            */
/* ------------------------------------------------------------------------ */

typedef struct cui_select_opts {
    int _;
    const char *label;
    const char *placeholder;
    const char *const *icons;
    bool searchable;           /* combobox: filter options by typing */
    bool disabled;
    cui_size width;
} cui_select_opts;
CUI_API bool cui_select_ex(cui_ctx *ui, const char *id, const char *const *items, int count, int *index, const cui_select_opts *opts);

typedef struct cui_date { int year, month, day; } cui_date;   /* month 1..12 */
CUI_API cui_date cui_date_today(void);
CUI_API bool cui_calendar(cui_ctx *ui, const char *id, cui_date *value);
typedef struct cui_date_opts { int _; const char *label; const char *placeholder; cui_size width; } cui_date_opts;
CUI_API bool cui_date_picker_ex(cui_ctx *ui, const char *id, cui_date *value, const cui_date_opts *opts);

CUI_API bool cui_color_picker(cui_ctx *ui, const char *id, cui_color *value);

/* ------------------------------------------------------------------------ */
/* Navigation                                                               */
/* ------------------------------------------------------------------------ */

typedef enum cui_tabs_variant { CUI_TABS_UNDERLINE = 0, CUI_TABS_PILL } cui_tabs_variant;
typedef struct cui_tabs_opts { int _; cui_tabs_variant variant; const char *const *icons; bool full_width; } cui_tabs_opts;
CUI_API bool cui_tabs_ex(cui_ctx *ui, const char *id, const char *const *items, int count, int *index, const cui_tabs_opts *opts);

/* Sidebar navigation item with a gliding selection highlight shared by all
 * items in the same group. */
typedef struct cui_nav_opts { int _; const char *icon; const char *badge; bool selected; const char *group; } cui_nav_opts;
CUI_API bool cui_nav_item_ex(cui_ctx *ui, const char *label, const cui_nav_opts *opts);
CUI_API void cui_nav_section(cui_ctx *ui, const char *title);

CUI_API int cui_breadcrumb(cui_ctx *ui, const char *const *items, int count);   /* returns clicked index or -1 */
CUI_API bool cui_pagination(cui_ctx *ui, const char *id, int *page, int page_count);  /* page is 0 based */
CUI_API void cui_stepper(cui_ctx *ui, const char *const *steps, int count, int current);

/* ------------------------------------------------------------------------ */
/* Menus and popovers                                                       */
/* ------------------------------------------------------------------------ */

typedef enum cui_placement {
    CUI_PLACE_BOTTOM_START = 0, CUI_PLACE_BOTTOM, CUI_PLACE_BOTTOM_END,
    CUI_PLACE_TOP_START, CUI_PLACE_TOP, CUI_PLACE_TOP_END,
    CUI_PLACE_RIGHT_START, CUI_PLACE_LEFT_START, CUI_PLACE_RIGHT, CUI_PLACE_LEFT
} cui_placement;

/* Popover anchored to the previous component. Toggle it with
 * cui_popover_toggle / cui_popover_open. */
typedef struct cui_popover_opts { int _; cui_placement placement; float width; float pad; float gap; } cui_popover_opts;
CUI_API bool cui_popover_begin(cui_ctx *ui, const char *id, const cui_popover_opts *opts);
CUI_API void cui_popover_end(cui_ctx *ui);
CUI_API void cui_popover_open(cui_ctx *ui, const char *id);
CUI_API void cui_popover_close(cui_ctx *ui, const char *id);
CUI_API void cui_popover_toggle(cui_ctx *ui, const char *id);
CUI_API bool cui_popover_is_open(cui_ctx *ui, const char *id);

/* Dropdown menu: a button that opens a menu. Items return true when chosen. */
typedef struct cui_menu_opts { int _; const char *icon; cui_button_variant variant; cui_placement placement; float width; bool icon_only; } cui_menu_opts;
CUI_API bool cui_menu_begin(cui_ctx *ui, const char *label, const cui_menu_opts *opts);
CUI_API void cui_menu_end(cui_ctx *ui);
/* Context menu: opens on right click over the previous component. */
CUI_API bool cui_context_menu_begin(cui_ctx *ui, const char *id);
CUI_API void cui_context_menu_end(cui_ctx *ui);

typedef struct cui_menu_item_opts { int _; const char *icon; const char *shortcut; bool danger; bool disabled; bool checked; } cui_menu_item_opts;
CUI_API bool cui_menu_item_ex(cui_ctx *ui, const char *label, const cui_menu_item_opts *opts);
CUI_API void cui_menu_separator(cui_ctx *ui);
CUI_API void cui_menu_label(cui_ctx *ui, const char *text);

/* Tooltip for the previous component, shown after a short hover delay. */
CUI_API void cui_tooltip(cui_ctx *ui, const char *text);

/* ------------------------------------------------------------------------ */
/* Dialogs, sheets, toasts, alerts                                          */
/* ------------------------------------------------------------------------ */

typedef struct cui_dialog_opts {
    int _;
    const char *description;
    float width;               /* default 440 */
    bool no_close_button;
    bool persistent;           /* clicking outside or Esc does not close */
} cui_dialog_opts;
/* Modal dialog. Shown while *open is true; closing sets it to false. */
CUI_API bool cui_dialog_begin(cui_ctx *ui, const char *title, bool *open, const cui_dialog_opts *opts);
CUI_API void cui_dialog_end(cui_ctx *ui);
/* A right-aligned row for dialog actions. */
CUI_API void cui_dialog_footer_begin(cui_ctx *ui);
CUI_API void cui_dialog_footer_end(cui_ctx *ui);

typedef enum cui_side { CUI_SIDE_RIGHT = 0, CUI_SIDE_LEFT, CUI_SIDE_BOTTOM, CUI_SIDE_TOP } cui_side;
typedef struct cui_sheet_opts { int _; cui_side side; float size; const char *description; } cui_sheet_opts;
CUI_API bool cui_sheet_begin(cui_ctx *ui, const char *title, bool *open, const cui_sheet_opts *opts);
CUI_API void cui_sheet_end(cui_ctx *ui);

typedef enum cui_tone {
    CUI_TONE_NEUTRAL = 0, CUI_TONE_INFO, CUI_TONE_SUCCESS, CUI_TONE_WARNING, CUI_TONE_DANGER,
    CUI_TONE_VIOLET, CUI_TONE_TEAL, CUI_TONE_ROSE, CUI_TONE_ACCENT
} cui_tone;

typedef struct cui_toast_opts {
    int _;
    const char *description;
    cui_tone tone;
    float duration;            /* seconds, default 4; negative = until dismissed */
    const char *action;        /* optional action button label */
    bool show_timer;           /* a draining line shows how long the action stays (undo) */
} cui_toast_opts;
/* Queue a toast. Returns a handle; cui_toast_action_clicked(handle) tells
 * whether its action button was pressed. */
CUI_API int cui_toast_ex(cui_ctx *ui, const char *title, const cui_toast_opts *opts);
CUI_API bool cui_toast_action_clicked(cui_ctx *ui, int handle);
CUI_API void cui_toast_dismiss(cui_ctx *ui, int handle);

typedef struct cui_alert_opts { int _; const char *description; cui_tone tone; const char *icon; bool dismissible; } cui_alert_opts;
/* Inline alert. Returns false once dismissed (when dismissible). */
CUI_API bool cui_alert_ex(cui_ctx *ui, const char *title, const cui_alert_opts *opts);

/* ------------------------------------------------------------------------ */
/* Feedback and display                                                     */
/* ------------------------------------------------------------------------ */

typedef struct cui_progress_opts { int _; bool indeterminate; cui_tone tone; float height; const char *label; bool show_value; cui_size width; } cui_progress_opts;
CUI_API void cui_progress_ex(cui_ctx *ui, float value, const cui_progress_opts *opts);
CUI_API void cui_spinner(cui_ctx *ui, float size);
CUI_API void cui_skeleton(cui_ctx *ui, cui_size width, float height);

typedef struct cui_badge_opts { int _; cui_tone tone; bool dot; bool solid; const char *icon; cui_control_size size; } cui_badge_opts;
CUI_API void cui_badge_ex(cui_ctx *ui, const char *text, const cui_badge_opts *opts);

typedef struct cui_avatar_opts { int _; float size; cui_texture image; const char *status; bool square; } cui_avatar_opts;
CUI_API void cui_avatar_ex(cui_ctx *ui, const char *name, const cui_avatar_opts *opts);
CUI_API void cui_avatar_group(cui_ctx *ui, const char *const *names, int count, int max_visible, float size);

typedef struct cui_empty_opts { int _; const char *icon; const char *description; const char *action; } cui_empty_opts;
/* Empty state. Returns true when its action button is clicked. */
CUI_API bool cui_empty_state_ex(cui_ctx *ui, const char *title, const cui_empty_opts *opts);

typedef struct cui_metric_opts {
    int _; float delta; const char *delta_label; const float *spark; int spark_count; const char *icon; cui_size width;
    bool lower_is_better;       /* build times, errors, latency: a fall is good news, shown in green */
} cui_metric_opts;
CUI_API void cui_metric_ex(cui_ctx *ui, const char *label, const char *value, const cui_metric_opts *opts);

CUI_API void cui_meter(cui_ctx *ui, const char *label, float used, float total, const char *unit);

/* ------------------------------------------------------------------------ */
/* Data display                                                             */
/* ------------------------------------------------------------------------ */

/* Accordion / disclosure. Content animates its height. */
CUI_API bool cui_accordion_begin(cui_ctx *ui, const char *title, bool *open);
CUI_API void cui_accordion_end(cui_ctx *ui);

/* Tree view node. */
typedef struct cui_tree_opts { int _; const char *icon; bool leaf; bool selected; } cui_tree_opts;
CUI_API bool cui_tree_begin(cui_ctx *ui, const char *label, bool *open, const cui_tree_opts *opts);
CUI_API void cui_tree_end(cui_ctx *ui);
CUI_API bool cui_tree_clicked(cui_ctx *ui);   /* the node created by the last cui_tree_begin was clicked */

/* List item: a selectable row with optional icon, subtitle and trailing text. */
typedef struct cui_list_item_opts { int _; const char *icon; const char *subtitle; const char *trailing; bool selected; const char *avatar; } cui_list_item_opts;
CUI_API bool cui_list_item_ex(cui_ctx *ui, const char *title, const cui_list_item_opts *opts);

/* Virtual list: only the visible rows are built, so lists with a million
 * rows stay fast. Fills first/last with the visible range. */
CUI_API void cui_virtual_list_begin(cui_ctx *ui, const char *id, int count, float row_height, cui_size height, int *first, int *last);
CUI_API void cui_virtual_list_end(cui_ctx *ui);

/* Table */
typedef struct cui_table_column {
    const char *title;
    cui_size width;            /* default fill */
    cui_text_align align;
    bool sortable;
} cui_table_column;

typedef struct cui_table_sort { int column; bool descending; } cui_table_sort;

typedef struct cui_table_opts {
    int _;
    cui_table_sort *sort;      /* optional, updated when a sortable header is clicked */
    cui_size height;           /* scrolls when set */
    bool striped;
    bool compact;
    bool selectable;
} cui_table_opts;
CUI_API bool cui_table_begin_ex(cui_ctx *ui, const char *id, const cui_table_column *columns, int column_count, const cui_table_opts *opts);
/* Begin a row. Returns true when the row is clicked. */
CUI_API bool cui_table_row(cui_ctx *ui, bool selected);
/* Next cell; put any components inside, or use cui_table_cell_text. */
CUI_API void cui_table_cell(cui_ctx *ui);
CUI_API void cui_table_cell_text(cui_ctx *ui, const char *text);
CUI_API void cui_table_cell_textf(cui_ctx *ui, const char *fmt, ...) CUI_PRINTF(2, 3);
CUI_API void cui_table_end(cui_ctx *ui);

/* Charts */
typedef struct cui_series {
    const char *name;
    const float *values;
    int count;
    cui_color color;           /* default: series palette */
} cui_series;

typedef struct cui_chart_opts {
    int _;
    cui_size width;
    float height;              /* default 220 */
    const char *const *labels; /* x axis labels */
    int label_count;
    bool area;                 /* fill under lines */
    bool no_grid;
    bool no_legend;
    bool stacked;              /* bar chart */
    bool smooth;               /* curved lines */
    const char *format;        /* value format for axis and tooltip, default "%.0f" */
    float min, max;            /* fixed y range when min != max */
} cui_chart_opts;
CUI_API void cui_line_chart_ex(cui_ctx *ui, const char *id, const cui_series *series, int count, const cui_chart_opts *opts);
CUI_API void cui_bar_chart_ex(cui_ctx *ui, const char *id, const cui_series *series, int count, const cui_chart_opts *opts);
CUI_API void cui_donut_chart_ex(cui_ctx *ui, const char *id, const float *values, const char *const *labels, int count, const cui_chart_opts *opts);
CUI_API void cui_sparkline(cui_ctx *ui, const float *values, int count, cui_size width, float height, cui_color color);
CUI_API void cui_gauge(cui_ctx *ui, const char *label, float value, float size);

/* Images */
CUI_API cui_texture cui_image_create(cui_ctx *ui, int w, int h, const void *rgba);
CUI_API cui_texture cui_image_load(cui_ctx *ui, const char *path, int *w, int *h);   /* PNG/JPEG via stb_image */
CUI_API void cui_image_destroy(cui_ctx *ui, cui_texture tex);
typedef struct cui_image_opts { int _; cui_size width, height; float radius; cui_color tint; } cui_image_opts;
CUI_API void cui_image_ex(cui_ctx *ui, cui_texture tex, const cui_image_opts *opts);

/* ------------------------------------------------------------------------ */
/* Command palette (block)                                                  */
/* ------------------------------------------------------------------------ */

typedef struct cui_command {
    const char *title;
    const char *icon;
    const char *group;
    const char *shortcut;
} cui_command;
/* Spotlight style command palette. Returns the index of the chosen command,
 * or -1. Open it with a shortcut: if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K)) open = true; */
CUI_API int cui_command_palette(cui_ctx *ui, bool *open, const cui_command *commands, int count);

/* ------------------------------------------------------------------------ */
/* More components                                                          */
/* ------------------------------------------------------------------------ */

/* Radio cards: larger selectable cards for choices that need a description
 * (plans, templates). */
typedef struct cui_choice { const char *title; const char *description; const char *icon; const char *badge; } cui_choice;
typedef struct cui_radio_cards_opts { int _; int columns; } cui_radio_cards_opts;
CUI_API bool cui_radio_cards_ex(cui_ctx *ui, const char *id, const cui_choice *choices, int count, int *index, const cui_radio_cards_opts *opts);

/* Tag input: tags are stored comma separated in buf. Enter or comma adds the
 * typed tag, Backspace on an empty field removes the last one. */
typedef struct cui_tag_opts { int _; const char *label; const char *placeholder; int max_tags; cui_size width; } cui_tag_opts;
CUI_API bool cui_tag_input_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_tag_opts *opts);

/* One time code input: `length` boxes; code must hold length + 1 bytes.
 * Returns true when all boxes are filled (once per change). */
CUI_API bool cui_otp_input(cui_ctx *ui, const char *id, char *code, int length);

/* Connected buttons. Put buttons between begin and end. */
CUI_API void cui_button_group_begin(cui_ctx *ui);
CUI_API void cui_button_group_end(cui_ctx *ui);

/* Icon button that copies text to the clipboard and confirms with a check. */
CUI_API bool cui_copy_button(cui_ctx *ui, const char *id, const char *text);

/* Hold to confirm: a destructive action that fills while held and fires when
 * full. Returns true once. */
typedef struct cui_hold_opts { int _; float duration; const char *icon; cui_button_variant variant; } cui_hold_opts;
CUI_API bool cui_hold_button_ex(cui_ctx *ui, const char *label, const cui_hold_opts *opts);

/* Password strength meter (0 weak .. 4 strong). Returns the score. */
CUI_API int cui_password_strength(cui_ctx *ui, const char *password);

/* Activity timeline item. */
typedef struct cui_timeline_opts { int _; const char *time; const char *description; const char *icon; cui_tone tone; bool last; } cui_timeline_opts;
CUI_API void cui_timeline_item_ex(cui_ctx *ui, const char *title, const cui_timeline_opts *opts);

/* Reorderable list: drag rows to reorder. `order` holds your item indices in
 * display order and is rewritten when a row is dropped. Build the rows in
 * display order: for (i...) cui_reorder_item(ui, i) { draw item order[i] }.
 * Returns true on the frame the order changes. */
CUI_API void cui_reorder_begin(cui_ctx *ui, const char *id, int *order, int count);
CUI_API bool cui_reorder_end(cui_ctx *ui);
CUI_API void cui_reorder_item_begin(cui_ctx *ui, int position);
CUI_API void cui_reorder_item_end(cui_ctx *ui);

/* Circular progress with the value in the middle. */
CUI_API void cui_progress_ring(cui_ctx *ui, const char *label, float value, float size);

/* ------------------------------------------------------------------------ */
/* More actions                                                             */
/* ------------------------------------------------------------------------ */

/* Split button: a main action with a menu of related actions on the chevron.
 * Returns 0 when the main part is clicked, 1 + i when menu item i is chosen
 * and -1 otherwise. */
typedef struct cui_split_button_opts {
    int _;
    cui_button_variant variant;   /* default primary */
    cui_control_size size;
    const char *icon;
    const char *const *item_icons;
    bool disabled;
} cui_split_button_opts;
CUI_API int cui_split_button_ex(cui_ctx *ui, const char *label, const char *const *items, int count, const cui_split_button_opts *opts);

/* Toggle group: a row of toggles (bold, italic...). values[i] is toggled
 * when item i is clicked; with exclusive set exactly one stays on. Labels
 * become tooltips when icon_only is set. Returns true when a value changed. */
typedef struct cui_toggle_group_opts { int _; const char *const *icons; bool exclusive; bool icon_only; cui_control_size size; } cui_toggle_group_opts;
CUI_API bool cui_toggle_group_ex(cui_ctx *ui, const char *id, const char *const *labels, int count, bool *values, const cui_toggle_group_opts *opts);

/* Two step destructive button: the first click morphs it into a confirm
 * state with a countdown, the second click confirms. Returns true once. */
typedef struct cui_confirm_opts {
    int _;
    const char *confirm_label;    /* default "Click to confirm" */
    const char *icon;
    cui_button_variant variant;   /* resting look, default secondary */
    cui_control_size size;
    float timeout;                /* seconds before it disarms, default 3 */
} cui_confirm_opts;
CUI_API bool cui_confirm_button_ex(cui_ctx *ui, const char *label, const cui_confirm_opts *opts);

/* Button that shows the result of the work it started: idle, a spinner while
 * busy, then a check (or an error) that settles back. Width morphs smoothly
 * between the states. Returns true when clicked while idle. */
typedef enum cui_status { CUI_STATUS_IDLE = 0, CUI_STATUS_BUSY, CUI_STATUS_SUCCESS, CUI_STATUS_ERROR } cui_status;
typedef struct cui_status_button_opts {
    int _;
    const char *busy_label;       /* default label + "..." */
    const char *success_label;    /* default "Done" */
    const char *error_label;      /* default "Failed" */
    const char *icon;
    cui_button_variant variant;   /* default primary */
    cui_control_size size;
} cui_status_button_opts;
CUI_API bool cui_status_button_ex(cui_ctx *ui, const char *label, cui_status status, const cui_status_button_opts *opts);

/* Reactions: icon pills with counts. Clicking toggles `active` and adjusts
 * the count. Returns the index that changed, or -1. */
typedef struct cui_reaction { const char *icon; int count; bool active; } cui_reaction;
CUI_API int cui_reactions(cui_ctx *ui, const char *id, cui_reaction *items, int count);

/* Speed dial: a round floating action button that fans out related actions.
 * Returns the chosen action index, or -1. */
typedef struct cui_speed_action { const char *icon; const char *label; } cui_speed_action;
typedef struct cui_speed_dial_opts { int _; const char *icon; cui_placement placement; const char *tooltip; } cui_speed_dial_opts;
CUI_API int cui_speed_dial_ex(cui_ctx *ui, const char *id, const cui_speed_action *actions, int count, const cui_speed_dial_opts *opts);

/* Theme switcher: light, dark and (optionally) system, as a compact icon
 * segmented control. mode is 0 light, 1 dark, 2 system. Light and dark are
 * applied for you; for system call cui_set_theme with the OS setting.
 * Returns true when mode changed. */
CUI_API bool cui_theme_switcher(cui_ctx *ui, int *mode, bool with_system);
/* Sun and moon icon button that toggles the theme with a morph. */
CUI_API bool cui_theme_toggle(cui_ctx *ui);

/* User menu: an avatar, a name and a line of detail that opens a menu of
 * account actions. Put menu items between begin and end. */
typedef struct cui_user_menu_opts { int _; const char *detail; const char *status; cui_texture avatar; bool compact; cui_placement placement; float width; } cui_user_menu_opts;
CUI_API bool cui_user_menu_begin(cui_ctx *ui, const char *name, const cui_user_menu_opts *opts);
CUI_API void cui_user_menu_end(cui_ctx *ui);

/* Swipe actions: drag a row sideways (or swipe on a trackpad) to reveal
 * actions behind it. A long swipe triggers the first action. The row content
 * goes between begin and end; end returns the chosen action: 0..left_count-1
 * for left actions, 100 + i for right actions, or -1. */
typedef struct cui_swipe_action { const char *icon; const char *label; cui_tone tone; } cui_swipe_action;
CUI_API void cui_swipe_item_begin(cui_ctx *ui, const char *id, const cui_swipe_action *left, int left_count,
                                  const cui_swipe_action *right, int right_count);
CUI_API int cui_swipe_item_end(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Data grid                                                                */
/* ------------------------------------------------------------------------ */

/* A virtualized table for large data: sticky header, resizable and sortable
 * columns, checkbox selection (click, Ctrl+click, Shift+click, Ctrl+A),
 * keyboard navigation and editable cells. Build only the rows in
 * [first, last):
 *
 *     int first, last;
 *     if (cui_data_grid_begin(ui, "people", cols, 4, count, &first, &last, .selected = sel)) {
 *         for (int r = first; r < last; r++) {
 *             cui_data_grid_row(ui, r);
 *             cui_data_grid_cell_text(ui, people[r].name);
 *             ...
 *         }
 *         cui_data_grid_end(ui);
 *     }
 */
typedef struct cui_grid_column {
    const char *title;
    float width;               /* initial width in points, default 150 */
    cui_text_align align;
    bool sortable;
    bool fixed;                /* not resizable */
} cui_grid_column;
typedef struct cui_grid_opts {
    int _;
    cui_size width, height;    /* default fill */
    float row_height;          /* default 40 */
    bool *selected;            /* one bool per row; adds a checkbox column */
    cui_table_sort *sort;
    bool striped;
    int *cursor;               /* the keyboard row, optional */
} cui_grid_opts;
CUI_API bool cui_data_grid_begin_ex(cui_ctx *ui, const char *id, const cui_grid_column *columns, int column_count, int row_count,
                                    int *first, int *last, const cui_grid_opts *opts);
CUI_API bool cui_data_grid_row(cui_ctx *ui, int row);   /* true when the row was clicked */
CUI_API void cui_data_grid_cell(cui_ctx *ui);           /* next cell; put components inside */
CUI_API void cui_data_grid_cell_text(cui_ctx *ui, const char *text);
/* Editable text cell: double click (or Enter on the cursor row) to edit,
 * Enter or clicking away commits, Esc reverts. True when committed. */
CUI_API bool cui_data_grid_cell_edit(cui_ctx *ui, char *buf, size_t cap);
/* The same editable text, inside the current cell (after an avatar, say). */
CUI_API bool cui_data_grid_edit(cui_ctx *ui, char *buf, size_t cap);
CUI_API void cui_data_grid_end(cui_ctx *ui);

/* Tree cell: indentation, a disclosure chevron and a label, for tree tables
 * (use it in a table or grid cell). Returns true when *open toggled. */
CUI_API bool cui_tree_cell(cui_ctx *ui, const char *label, int depth, bool has_children, bool *open, const char *icon);

/* Filter bar: a search field, a chip per active filter (click it to pick
 * values) and "+ Filter" to add one. selected[i] is a bit mask of the
 * chosen options of fields[i]. Returns true when anything changed. */
typedef struct cui_filter_field { const char *name; const char *icon; const char *const *options; int option_count; } cui_filter_field;
CUI_API bool cui_filter_bar(cui_ctx *ui, const char *id, const cui_filter_field *fields, int count, uint32_t *selected,
                            char *search, size_t search_cap);

/* ------------------------------------------------------------------------ */
/* Code                                                                     */
/* ------------------------------------------------------------------------ */

/* Code block with syntax highlighting (lang: "c", "cpp", "js", "ts",
 * "python", "rust", "go", "json", "shell"), line numbers, a title bar and a
 * copy button. Long lines scroll sideways. */
typedef struct cui_code_opts {
    int _;
    const char *lang;
    const char *title;         /* e.g. a file name */
    bool line_numbers;
    int highlight_from, highlight_to;   /* 1 based line range to emphasize */
    float max_height;          /* scrolls beyond this, default none */
    float size;                /* font size, default 13 */
    bool no_header;            /* no title bar, even with a lang */
} cui_code_opts;
CUI_API void cui_code_block_ex(cui_ctx *ui, const char *id, const char *code, const cui_code_opts *opts);

/* JSON viewer: a collapsible tree with typed colors and item counts.
 * Returns false when the text is not valid JSON (and shows the error). */
typedef struct cui_json_opts { int _; int expand_depth; float max_height; } cui_json_opts;
CUI_API bool cui_json_view_ex(cui_ctx *ui, const char *id, const char *json, const cui_json_opts *opts);

/* ------------------------------------------------------------------------ */
/* Conversation                                                             */
/* ------------------------------------------------------------------------ */

/* Chat message bubble. Consecutive messages from one person set grouped so
 * they stack tightly and show the avatar once. */
typedef struct cui_chat_opts {
    int _;
    bool mine;                 /* right aligned, accent bubble */
    const char *author;        /* name above the first bubble of a group */
    const char *time;
    const char *status;        /* under your own last message, e.g. "Read" */
    bool grouped;              /* continues the previous message */
    bool last_in_group;        /* shows the avatar (others) and status (mine) */
    cui_texture avatar;
} cui_chat_opts;
CUI_API void cui_chat_message_ex(cui_ctx *ui, const char *text, const cui_chat_opts *opts);
CUI_API void cui_chat_divider(cui_ctx *ui, const char *label);       /* "Today" */
CUI_API void cui_typing_indicator(cui_ctx *ui, const char *name);    /* bouncing dots */

/* Comment with replies. depth indents replies under a connector line.
 * Returns CUI_COMMENT_LIKE or CUI_COMMENT_REPLY flags for what was clicked;
 * *liked and *likes are updated for you. */
enum { CUI_COMMENT_LIKE = 1, CUI_COMMENT_REPLY = 2 };
typedef struct cui_comment_opts { int _; const char *time; int depth; int *likes; bool *liked; bool edited; const char *badge; } cui_comment_opts;
CUI_API int cui_comment_ex(cui_ctx *ui, const char *id, const char *author, const char *text, const cui_comment_opts *opts);

/* Presence: who is here now. Overlapping avatars with a colored ring each,
 * that spread out on hover and name themselves. */
typedef struct cui_presence_user { const char *name; cui_color color; cui_texture avatar; bool idle; } cui_presence_user;
CUI_API void cui_presence(cui_ctx *ui, const char *id, const cui_presence_user *users, int count, int max_visible, float size);

/* ------------------------------------------------------------------------ */
/* Media                                                                    */
/* ------------------------------------------------------------------------ */

/* Carousel: slides that you swipe, drag, scroll or page with the arrows
 * and dots. Build every slide between begin and end:
 *     cui_carousel(ui, "tour", 4, &index, .height = 220) {
 *         for (int k = 0; k < 4; k++) cui_carousel_slide(ui) { ... }
 *     }
 */
typedef struct cui_carousel_opts { int _; float height; float auto_advance; float gap; bool no_arrows, no_dots, loop; } cui_carousel_opts;
CUI_API bool cui_carousel_begin(cui_ctx *ui, const char *id, int count, int *index, const cui_carousel_opts *opts);
CUI_API void cui_carousel_slide_begin(cui_ctx *ui);
CUI_API void cui_carousel_slide_end(cui_ctx *ui);
CUI_API void cui_carousel_end(cui_ctx *ui);

/* Before and after: drag the divider to compare two images. *pos is 0..1. */
typedef struct cui_compare_opts {
    int _; cui_size width; float height; const char *before_label, *after_label;
    float aspect;               /* the images' width / height: they are cropped to fill, never stretched */
} cui_compare_opts;
CUI_API bool cui_image_compare_ex(cui_ctx *ui, const char *id, cui_texture before, cui_texture after, float *pos, const cui_compare_opts *opts);

/* Card stack: a deck where the top card can be flicked away left or right.
 * Build `count` cards; card 0 is the top. end returns -1 or 1 on the frame
 * the top card leaves (to the left or right), else 0. */
typedef struct cui_card_stack_opts { int _; cui_size width; float height; int visible; } cui_card_stack_opts;
CUI_API void cui_card_stack_begin(cui_ctx *ui, const char *id, int count, const cui_card_stack_opts *opts);
CUI_API void cui_card_stack_item_begin(cui_ctx *ui, int depth);
CUI_API void cui_card_stack_item_end(cui_ctx *ui);
CUI_API int cui_card_stack_end(cui_ctx *ui);

/* Odometer: a number whose digits roll to their new values. */
typedef struct cui_counter_opts { int _; const char *format; float size; cui_font font; cui_color color; bool group_thousands; } cui_counter_opts;
CUI_API void cui_counter_ex(cui_ctx *ui, const char *id, double value, const cui_counter_opts *opts);

/* Ticker: items that scroll by continuously, pausing on hover. */
typedef struct cui_ticker_item { const char *label; const char *value; float delta; bool lower_is_better; } cui_ticker_item;   /* a fall is good: shown green */
typedef struct cui_ticker_opts { int _; float speed; float height; } cui_ticker_opts;
CUI_API void cui_ticker_ex(cui_ctx *ui, const char *id, const cui_ticker_item *items, int count, const cui_ticker_opts *opts);

/* ------------------------------------------------------------------------ */
/* More charts                                                              */
/* ------------------------------------------------------------------------ */
/* Every chart below has a hover tooltip, animates in once, follows the theme
 * and returns the index of the mark that was clicked, or -1. Colors default
 * to the series palette; numbers use `format` (printf, default compact). */

/* Heatmap: a rows x cols matrix (values[row * cols + col]) on one hue,
 * light to dark, with a scale below. */
typedef struct cui_heatmap_opts {
    int _;
    const char *const *row_labels, *const *col_labels;
    float cell;                /* cell size, default fits the width */
    float min, max;            /* value range, default the data's */
    cui_color color;           /* hue of the scale, default the accent */
    const char *format;
    cui_size width;
} cui_heatmap_opts;
CUI_API int cui_heatmap_ex(cui_ctx *ui, const char *id, const float *values, int rows, int cols, const cui_heatmap_opts *opts);

/* Calendar heatmap: one cell per day from start, weeks as columns, in five
 * levels, with month and weekday labels and a Less / More legend. */
typedef struct cui_calendar_heatmap_opts { int _; float cell; cui_color color; const char *format; const char *unit; } cui_calendar_heatmap_opts;
CUI_API int cui_calendar_heatmap_ex(cui_ctx *ui, const char *id, cui_date start, const float *values, int days, const cui_calendar_heatmap_opts *opts);

/* Treemap: rectangles with areas proportional to their values. */
typedef struct cui_viz_item { const char *label; float value; cui_color color; } cui_viz_item;
typedef struct cui_treemap_opts { int _; cui_size width; float height; const char *format; } cui_treemap_opts;
CUI_API int cui_treemap_ex(cui_ctx *ui, const char *id, const cui_viz_item *items, int count, const cui_treemap_opts *opts);

/* Radar chart: series over shared axes (series[i].values[axis]). */
typedef struct cui_radar_opts { int _; float size; float max; const char *format; bool no_legend; } cui_radar_opts;
CUI_API int cui_radar_chart_ex(cui_ctx *ui, const char *id, const cui_series *series, int count, const char *const *axes, int axis_count,
                               const cui_radar_opts *opts);

/* Funnel: stages narrowing by value, with the conversion between them. */
typedef struct cui_funnel_opts { int _; cui_size width; float height; const char *format; cui_color color; } cui_funnel_opts;
CUI_API int cui_funnel_ex(cui_ctx *ui, const char *id, const cui_viz_item *stages, int count, const cui_funnel_opts *opts);

/* Sankey: flows between nodes; columns follow the links from the sources. */
typedef struct cui_sankey_link { int from, to; float value; } cui_sankey_link;
typedef struct cui_sankey_opts { int _; cui_size width; float height; const char *format; float node_width; } cui_sankey_opts;
CUI_API int cui_sankey_ex(cui_ctx *ui, const char *id, const char *const *nodes, int node_count, const cui_sankey_link *links, int link_count,
                          const cui_sankey_opts *opts);

/* Waffle: a 10 x 10 grid of squares split by share, with a legend. */
typedef struct cui_waffle_opts { int _; float cell; int columns, rows; const char *format; } cui_waffle_opts;
CUI_API int cui_waffle_ex(cui_ctx *ui, const char *id, const cui_viz_item *items, int count, const cui_waffle_opts *opts);

/* Split bar: one bar split into parts, with a legend of values and shares. */
typedef struct cui_split_bar_opts { int _; cui_size width; float height; const char *format; bool no_legend; } cui_split_bar_opts;
CUI_API int cui_split_bar_ex(cui_ctx *ui, const char *id, const cui_viz_item *items, int count, const cui_split_bar_opts *opts);

/* Bar list: ranked horizontal bars. Rows are keyed by label, so when the
 * values change the bars grow and the rows slide to their new rank. */
typedef struct cui_bar_list_opts { int _; cui_size width; const char *format; int max_rows; bool keep_order; cui_color color; } cui_bar_list_opts;
CUI_API int cui_bar_list_ex(cui_ctx *ui, const char *id, const cui_viz_item *items, int count, const cui_bar_list_opts *opts);

/* Activity rings: concentric progress rings; values past 1 keep winding. */
typedef struct cui_rings_opts { int _; float size; float thickness; const char *center; const char *caption; bool legend; /* names and shares beside the rings */ } cui_rings_opts;
CUI_API int cui_activity_rings_ex(cui_ctx *ui, const char *id, const cui_viz_item *rings, int count, const cui_rings_opts *opts);

/* Scatter (or bubble) chart: points per series, optional sizes. */
typedef struct cui_scatter_series { const char *name; const cui_vec2 *points; const float *sizes; int count; cui_color color; } cui_scatter_series;
typedef struct cui_scatter_opts {
    int _;
    cui_size width;
    float height;
    const char *x_label, *y_label;
    const char *x_format, *y_format;
    bool no_legend;
} cui_scatter_opts;
CUI_API int cui_scatter_chart_ex(cui_ctx *ui, const char *id, const cui_scatter_series *series, int count, const cui_scatter_opts *opts);

/* ------------------------------------------------------------------------ */
/* Motion and text effects                                                  */
/* ------------------------------------------------------------------------ */
/* All of these respect reduced motion: they show their final state. */

typedef struct cui_text_fx_opts {
    int _;
    cui_font font;
    float size;
    cui_color color;
    cui_size width;
    bool nowrap;
    float speed;               /* shimmer: sweeps per second; typewriter: characters per second */
    float delay;               /* seconds before it starts */
    bool caret;                /* typewriter: a blinking caret while typing */
} cui_text_fx_opts;
/* Text with a highlight sweeping across it, for "Thinking..." states. */
CUI_API void cui_shimmer_text_ex(cui_ctx *ui, const char *text, const cui_text_fx_opts *opts);
/* Types the text out. When the text grows (streamed tokens) it carries on
 * from where it was; other changes start over. Returns true once complete. */
CUI_API bool cui_typewriter_ex(cui_ctx *ui, const char *id, const char *text, const cui_text_fx_opts *opts);
/* Words fade and rise into place one after another, when first shown and
 * again whenever the text changes. */
CUI_API void cui_text_reveal_ex(cui_ctx *ui, const char *id, const char *text, const cui_text_fx_opts *opts);
/* One word at a time; the next one rolls up into place and the width follows. */
CUI_API void cui_word_rotate_ex(cui_ctx *ui, const char *id, const char *const *words, int count, float interval, const cui_text_fx_opts *opts);
/* A figure whose changed digits roll into place: up when the value rises, down
 * when it falls, a beat apart from the left; unchanged characters stay still.
 * Pass the formatted text ("$64,210", "1.2k"); digits are tabular. */
CUI_API void cui_number_roll_ex(cui_ctx *ui, const char *id, const char *text, const cui_text_fx_opts *opts);

/* Text painted with a gradient from one color to another; animate makes it drift. */
typedef struct cui_gradient_text_opts { int _; cui_font font; float size; cui_color from, to; bool animate; cui_size width; } cui_gradient_text_opts;
CUI_API void cui_gradient_text_ex(cui_ctx *ui, const char *text, const cui_gradient_text_opts *opts);

/* Spotlight card: a soft light follows the pointer across the card and lights
 * the nearest stretch of its border. beam sends a light around the edge. */
typedef struct cui_spotlight_opts { int _; cui_size width, height; float pad, gap; cui_color color; bool beam; } cui_spotlight_opts;
CUI_API void cui_spotlight_begin(cui_ctx *ui, const char *id, const cui_spotlight_opts *opts);
CUI_API void cui_spotlight_end(cui_ctx *ui);

typedef enum cui_loader_style { CUI_LOADER_DOTS = 0, CUI_LOADER_BARS, CUI_LOADER_PULSE, CUI_LOADER_ORBIT, CUI_LOADER_GRID } cui_loader_style;
typedef struct cui_loader_opts { int _; cui_loader_style style; float size; cui_color color; } cui_loader_opts;
CUI_API void cui_loader_ex(cui_ctx *ui, const cui_loader_opts *opts);

/* Dock: a row of icons that grow as the pointer passes, with labels and a
 * dot under the running ones. Each item returns true when clicked. */
typedef struct cui_dock_opts { int _; float size; float magnify; } cui_dock_opts;
typedef struct cui_dock_item_opts { int _; cui_color color; bool running; int badge; } cui_dock_item_opts;
CUI_API void cui_dock_begin(cui_ctx *ui, const char *id, const cui_dock_opts *opts);
CUI_API bool cui_dock_item_ex(cui_ctx *ui, const char *icon, const char *label, const cui_dock_item_opts *opts);
CUI_API void cui_dock_separator(cui_ctx *ui);
CUI_API void cui_dock_end(cui_ctx *ui);

/* Bottom tab bar for compact layouts, with a pill that glides to the selection. */
typedef struct cui_tab_bar_item { const char *icon; const char *label; int badge; } cui_tab_bar_item;
typedef struct cui_tab_bar_opts { int _; cui_size width; bool labels_on_selected_only; } cui_tab_bar_opts;
CUI_API bool cui_tab_bar_ex(cui_ctx *ui, const char *id, const cui_tab_bar_item *items, int count, int *selected, const cui_tab_bar_opts *opts);

/* Control tile: a large toggle with an icon, a label and a state line. */
typedef struct cui_tile_opts { int _; const char *icon; const char *detail; cui_color color; cui_size width; bool wide; } cui_tile_opts;
CUI_API bool cui_control_tile_ex(cui_ctx *ui, const char *label, bool *on, const cui_tile_opts *opts);

/* Waveform: bars from peaks (0..1). With progress, the played part is
 * colored and clicking or dragging seeks; returns true when it did. */
typedef struct cui_waveform_opts { int _; cui_size width; float height; float bar; float gap; cui_color color; } cui_waveform_opts;
CUI_API bool cui_waveform_ex(cui_ctx *ui, const char *id, const float *peaks, int count, float *progress, const cui_waveform_opts *opts);

/* Now playing card: artwork, title, artist, a scrubber and transport buttons. */
typedef struct cui_now_playing { const char *title, *artist; cui_texture art; cui_color art_color; float position, duration; bool playing; } cui_now_playing;
typedef struct cui_now_playing_opts { int _; cui_size width; bool compact; } cui_now_playing_opts;
enum { CUI_PLAYER_TOGGLE = 1, CUI_PLAYER_PREV = 2, CUI_PLAYER_NEXT = 4, CUI_PLAYER_SEEK = 8 };
CUI_API int cui_now_playing_ex(cui_ctx *ui, const char *id, cui_now_playing *np, const cui_now_playing_opts *opts);

/* ------------------------------------------------------------------------ */
/* AI interfaces                                                            */
/* ------------------------------------------------------------------------ */

/* Reasoning: a model's thinking. While thinking, the header shimmers and
 * counts the seconds; afterwards it reads "Thought for 12s" and the body
 * folds away. Click the header to open or close it. Put text inside. */
typedef struct cui_reasoning_opts { int _; bool thinking; float seconds; const char *title; } cui_reasoning_opts;
CUI_API bool cui_reasoning_begin(cui_ctx *ui, const char *id, const cui_reasoning_opts *opts);
CUI_API void cui_reasoning_end(cui_ctx *ui);

/* Tool call: a compact row with the tool, a detail and its status; opens to
 * show the input and the output (shown as JSON when they parse). */
typedef enum cui_task_status { CUI_TASK_PENDING = 0, CUI_TASK_RUNNING, CUI_TASK_DONE, CUI_TASK_FAILED, CUI_TASK_SKIPPED } cui_task_status;
typedef struct cui_tool_call_opts { int _; cui_task_status status; const char *icon; const char *detail; const char *input; const char *output; float seconds; } cui_tool_call_opts;
CUI_API void cui_tool_call_ex(cui_ctx *ui, const char *id, const char *name, const cui_tool_call_opts *opts);

/* Agent plan: steps with their status on a connected rail, and progress. */
typedef struct cui_plan_step { const char *title; const char *detail; cui_task_status status; } cui_plan_step;
typedef struct cui_agent_plan_opts { int _; const char *title; } cui_agent_plan_opts;
CUI_API int cui_agent_plan_ex(cui_ctx *ui, const char *id, const cui_plan_step *steps, int count, const cui_agent_plan_opts *opts);

/* Agent status pill: what the agent is doing right now and for how long. */
typedef struct cui_agent_status_opts { int _; cui_task_status status; float seconds; const char *icon; } cui_agent_status_opts;
CUI_API void cui_agent_status_ex(cui_ctx *ui, const char *text, const cui_agent_status_opts *opts);

/* Sources: cards for the documents an answer used, and inline citations
 * that show a card on hover. Both return true / the index when clicked. */
typedef struct cui_source { const char *title; const char *url; const char *snippet; } cui_source;
CUI_API int cui_source_list(cui_ctx *ui, const char *id, const cui_source *sources, int count);
CUI_API bool cui_citation(cui_ctx *ui, int number, const cui_source *source);

/* Context meter: a small ring of how full the context is; hover for a
 * breakdown. It turns amber past 80% and red past 95%. */
typedef struct cui_context_opts { int _; const cui_viz_item *parts; int part_count; const char *unit; } cui_context_opts;
CUI_API void cui_context_meter_ex(cui_ctx *ui, const char *id, float used, float limit, const cui_context_opts *opts);

/* Response actions: copy, regenerate, vote and share under an answer. */
enum { CUI_RESPONSE_COPY = 1, CUI_RESPONSE_REGENERATE = 2, CUI_RESPONSE_UP = 4, CUI_RESPONSE_DOWN = 8, CUI_RESPONSE_SHARE = 16 };
typedef struct cui_response_actions_opts { int _; const char *copy_text; int *vote; bool no_regenerate, no_share; } cui_response_actions_opts;
CUI_API int cui_response_actions_ex(cui_ctx *ui, const char *id, const cui_response_actions_opts *opts);

/* Prompt suggestions: cards to start a conversation with. */
typedef struct cui_prompt_suggestion { const char *icon; const char *title; const char *detail; } cui_prompt_suggestion;
typedef struct cui_prompt_suggestions_opts { int _; int columns; } cui_prompt_suggestions_opts;
CUI_API int cui_prompt_suggestions_ex(cui_ctx *ui, const char *id, const cui_prompt_suggestion *items, int count, const cui_prompt_suggestions_opts *opts);

/* Model picker: the current model on a button; a list with details opens. */
typedef struct cui_model { const char *name; const char *detail; const char *badge; const char *icon; } cui_model;
typedef struct cui_model_picker_opts { int _; bool ghost; cui_placement placement; } cui_model_picker_opts;
CUI_API bool cui_model_picker_ex(cui_ctx *ui, const char *id, const cui_model *models, int count, int *selected, const cui_model_picker_opts *opts);

/* Chat composer: a growing text area with attachments, a model picker and a
 * send button that turns into stop while busy. Enter sends, Shift+Enter adds
 * a line. Returns CUI_COMPOSER_* flags; *removed is the attachment whose x
 * was clicked. */
enum { CUI_COMPOSER_SEND = 1, CUI_COMPOSER_STOP = 2, CUI_COMPOSER_ATTACH = 4, CUI_COMPOSER_REMOVE = 8, CUI_COMPOSER_MODEL = 16 };
typedef struct cui_composer_opts {
    int _;
    const char *placeholder;
    bool busy;
    const char *const *attachments;
    int attachment_count;
    int *removed;
    const cui_model *models;
    int model_count;
    int *model;
    int max_lines;             /* default 8 */
    cui_size width;
} cui_composer_opts;
CUI_API int cui_chat_composer_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_composer_opts *opts);

/* Voice orb: a living shape for voice modes. level is the input or output
 * loudness (0..1). Returns true when clicked. */
typedef enum cui_voice_state { CUI_VOICE_IDLE = 0, CUI_VOICE_LISTENING, CUI_VOICE_THINKING, CUI_VOICE_SPEAKING } cui_voice_state;
typedef struct cui_voice_orb_opts { int _; float size; cui_color color; } cui_voice_orb_opts;
CUI_API bool cui_voice_orb_ex(cui_ctx *ui, const char *id, cui_voice_state state, float level, const cui_voice_orb_opts *opts);

/* ------------------------------------------------------------------------ */
/* Planning                                                                 */
/* ------------------------------------------------------------------------ */

/* Kanban board: columns of cards you drag between columns and within them.
 * The board reports drops; you move the card in your data:
 *
 *     cui_kanban_begin(ui, "board");
 *     for (int c = 0; c < ncols; c++) {
 *         cui_kanban_column_begin(ui, cols[c].title, cols[c].count, .dot = CUI_COLOR_SERIES(c));
 *         for (each card k in column c) {
 *             cui_kanban_card_begin(ui, card_id);   ... content ...   cui_kanban_card_end(ui);
 *         }
 *         cui_kanban_column_end(ui);
 *     }
 *     cui_kanban_move m;
 *     if (cui_kanban_end(ui, &m)) move_card(m.card, m.to_column, m.to_index);
 */
typedef struct cui_kanban_move { int card; int from_column, to_column, to_index; } cui_kanban_move;
typedef struct cui_kanban_column_opts { int _; cui_color dot; int limit; float width; } cui_kanban_column_opts;
CUI_API void cui_kanban_begin(cui_ctx *ui, const char *id);
CUI_API bool cui_kanban_column_begin_ex(cui_ctx *ui, const char *title, int card_count, const cui_kanban_column_opts *opts);   /* true: "+" clicked */
CUI_API bool cui_kanban_card_begin(cui_ctx *ui, int card_id);   /* true when clicked */
CUI_API void cui_kanban_card_end(cui_ctx *ui);
CUI_API void cui_kanban_column_end(cui_ctx *ui);
CUI_API bool cui_kanban_end(cui_ctx *ui, cui_kanban_move *move);

/* Week calendar: seven day columns on an hour grid. Drag an event to move
 * it (15 minute steps), drag its bottom edge to resize it. Returns the
 * index of a clicked event, or -1. */
typedef struct cui_cal_event { const char *title; const char *detail; int day; float start, end; cui_tone tone; } cui_cal_event;
typedef struct cui_week_opts { int _; float start_hour, end_hour, hour_height; cui_size height; bool read_only; } cui_week_opts;
CUI_API int cui_week_calendar_ex(cui_ctx *ui, const char *id, cui_date week_start, cui_cal_event *events, int count, const cui_week_opts *opts);

/* Gantt chart: a task list beside a timeline of bars. Drag a bar to move
 * it, its ends to resize it. Dependencies are drawn as elbow arrows.
 * Returns the index of a clicked task, or -1. */
typedef struct cui_gantt_task { const char *name; const char *owner; int start, end; float progress; cui_tone tone; int depends_on; bool milestone; } cui_gantt_task;
typedef struct cui_gantt_opts { int _; float day_width, row_height; cui_size height; int today; bool read_only; } cui_gantt_opts;
CUI_API int cui_gantt_ex(cui_ctx *ui, const char *id, cui_gantt_task *tasks, int count, cui_date origin, int days, const cui_gantt_opts *opts);

/* ------------------------------------------------------------------------ */
/* Disclosure                                                               */
/* ------------------------------------------------------------------------ */

/* Hover card: rich content that appears when the pointer rests on the
 * previous component, and stays while the pointer is over either. */
typedef struct cui_hover_card_opts { int _; cui_placement placement; float width; float delay; } cui_hover_card_opts;
CUI_API bool cui_hover_card_begin(cui_ctx *ui, const char *id, const cui_hover_card_opts *opts);
CUI_API void cui_hover_card_end(cui_ctx *ui);

/* Confirm popover: a small question anchored to the previous component.
 * Open it with cui_popover_open(ui, id). Returns true when confirmed. */
typedef struct cui_confirm_popover_opts {
    int _;
    const char *title;
    const char *description;
    const char *confirm_label;    /* default "Confirm" */
    const char *cancel_label;     /* default "Cancel" */
    bool danger;
    cui_placement placement;
} cui_confirm_popover_opts;
CUI_API bool cui_confirm_popover_ex(cui_ctx *ui, const char *id, const cui_confirm_popover_opts *opts);

/* Text clamped to a few lines with a fade and Show more / Show less. */
typedef struct cui_expandable_text_opts { int _; int lines; float size; cui_color color; cui_color fade_to; } cui_expandable_text_opts;
CUI_API bool cui_expandable_text_ex(cui_ctx *ui, const char *id, const char *text, const cui_expandable_text_opts *opts);

/* Expandable card: a card whose header opens a body with a height spring. */
typedef struct cui_expandable_card_opts { int _; const char *description; const char *icon; const char *badge; cui_tone badge_tone; } cui_expandable_card_opts;
CUI_API bool cui_expandable_card_begin(cui_ctx *ui, const char *title, bool *open, const cui_expandable_card_opts *opts);
CUI_API void cui_expandable_card_end(cui_ctx *ui);

/* Table of contents for a scroll area: highlights the section in view on a
 * progress rail, and scrolls to a section when clicked. Put
 * cui_toc_anchor(ui, "anchor") at each heading inside the scroll area, and
 * call cui_toc in the same scope as the scroll area. Returns the active index. */
typedef struct cui_toc_item { const char *title; const char *anchor; int level; } cui_toc_item;
CUI_API int cui_toc(cui_ctx *ui, const char *scroll_id, const cui_toc_item *items, int count);
CUI_API void cui_toc_anchor(cui_ctx *ui, const char *anchor);

/* ------------------------------------------------------------------------ */
/* More feedback                                                            */
/* ------------------------------------------------------------------------ */

/* Announcement banner across the top of a view. Collapses smoothly when
 * dismissed (*open becomes false). Returns true when the action is clicked. */
typedef struct cui_banner_opts { int _; cui_tone tone; const char *icon; const char *action; bool dismissible; } cui_banner_opts;
CUI_API bool cui_banner_ex(cui_ctx *ui, const char *text, bool *open, const cui_banner_opts *opts);

/* Countdown: days, hours, minutes and seconds as tiles whose digits roll. */
typedef struct cui_countdown_opts { int _; bool hide_days; bool compact; } cui_countdown_opts;
CUI_API void cui_countdown_ex(cui_ctx *ui, const char *id, double seconds_left, const cui_countdown_opts *opts);

/* Onboarding checklist: a card with progress and steps. The first open step
 * shows its description and action. Clicking a step's circle toggles done.
 * Returns the index whose action was clicked, or -1. */
typedef struct cui_check_step { const char *title; const char *description; const char *action; bool done; } cui_check_step;
typedef struct cui_checklist_opts { int _; const char *title; const char *description; } cui_checklist_opts;
CUI_API int cui_checklist_ex(cui_ctx *ui, const char *id, cui_check_step *steps, int count, const cui_checklist_opts *opts);

/* ------------------------------------------------------------------------ */
/* Pickers                                                                  */
/* ------------------------------------------------------------------------ */

/* Multi-select: a field with chips for the chosen items and a list with
 * checkboxes (and search). selected[i] is true for chosen items. */
typedef struct cui_multi_select_opts {
    int _;
    const char *label;
    const char *placeholder;
    const char *const *icons;
    bool searchable;
    int max_chips;             /* chips before "+N", default 3 */
    cui_size width;
} cui_multi_select_opts;
CUI_API bool cui_multi_select_ex(cui_ctx *ui, const char *id, const char *const *items, int count, bool *selected, const cui_multi_select_opts *opts);

/* Date ranges. A zero start means nothing is chosen. */
typedef struct cui_date_range { cui_date start, end; } cui_date_range;
/* One or two months side by side; click a start and an end. */
CUI_API bool cui_range_calendar(cui_ctx *ui, const char *id, cui_date_range *range, int months);
typedef struct cui_date_range_opts { int _; const char *label; const char *placeholder; cui_size width; bool no_presets; } cui_date_range_opts;
CUI_API bool cui_date_range_picker_ex(cui_ctx *ui, const char *id, cui_date_range *range, const cui_date_range_opts *opts);
CUI_API int cui_date_to_days(cui_date d);      /* days since 1970-01-01 */
CUI_API cui_date cui_date_from_days(int days);

/* Time picker: a field that opens hour and minute columns. hour is 0..23. */
typedef struct cui_time_opts { int _; const char *label; bool twelve_hour; int minute_step; cui_size width; } cui_time_opts;
CUI_API bool cui_time_picker_ex(cui_ctx *ui, const char *id, int *hour, int *minute, const cui_time_opts *opts);

/* Wheel picker: a drum of options that you drag, scroll or arrow through.
 * Several side by side make a date or time reel. */
/* join_left / join_right square off the selection band so wheels placed
 * side by side share one continuous band. */
typedef struct cui_wheel_opts { int _; int rows; cui_size width; bool loop; bool join_left, join_right; } cui_wheel_opts;
CUI_API bool cui_wheel_picker_ex(cui_ctx *ui, const char *id, const char *const *items, int count, int *index, const cui_wheel_opts *opts);

/* Time dial: a clock face. Drag the hand to set hours, then minutes. */
CUI_API bool cui_time_dial(cui_ctx *ui, const char *id, int *hour, int *minute, float size);

/* Ruler slider: a tape measure that slides under a fixed marker, with
 * momentum and snapping to step. */
typedef struct cui_ruler_opts { int _; float step; int major_every; float spacing; const char *format; const char *unit; cui_size width; } cui_ruler_opts;
CUI_API bool cui_ruler_slider_ex(cui_ctx *ui, const char *label, float *value, float min, float max, const cui_ruler_opts *opts);

/* Range slider over a histogram of the data it filters. */
typedef struct cui_histogram_opts { int _; float height; const char *format; } cui_histogram_opts;
CUI_API bool cui_histogram_slider_ex(cui_ctx *ui, const char *label, const float *bins, int bin_count, float *lo, float *hi,
                                     float min, float max, const cui_histogram_opts *opts);

/* ------------------------------------------------------------------------ */
/* Capture                                                                  */
/* ------------------------------------------------------------------------ */

/* Signature pad: draw with the mouse or a pen. The strokes are kept for you;
 * read them with cui_signature_points (stroke_ends[i] is one past the last
 * point of stroke i). */
typedef struct cui_signature_opts { int _; cui_size width; float height; const char *placeholder; cui_color ink; } cui_signature_opts;
CUI_API bool cui_signature_pad_ex(cui_ctx *ui, const char *id, const cui_signature_opts *opts);
CUI_API int cui_signature_points(cui_ctx *ui, const char *id, const cui_vec2 **points, const int **stroke_ends, int *stroke_count);
CUI_API void cui_signature_clear(cui_ctx *ui, const char *id);

/* Dropzone: drop files from the OS, or click to browse. Returns the number
 * of files dropped on it this frame (read them with cui_dropped_file), -1
 * when clicked (open a file dialog), 0 otherwise. accept filters by
 * extension, e.g. "png;jpg;pdf". */
typedef struct cui_dropzone_opts {
    int _;
    const char *title;         /* default "Drop files here" */
    const char *description;   /* default "or click to browse" */
    const char *icon;
    const char *accept;
    float height;              /* default 160 */
    cui_size width;
    bool compact;              /* one row instead of a tall area */
} cui_dropzone_opts;
CUI_API int cui_dropzone_ex(cui_ctx *ui, const char *id, const cui_dropzone_opts *opts);
CUI_API bool cui_path_has_extension(const char *path, const char *accept);

/* Avatar upload: a round avatar that takes a dropped image, with a camera
 * badge on hover. Returns 1 when *image changed, -1 when clicked (browse),
 * 0 otherwise. A dropped image is loaded with cui_image_load. */
CUI_API int cui_avatar_upload(cui_ctx *ui, const char *id, const char *name, cui_texture *image, float size);

/* ------------------------------------------------------------------------ */
/* More inputs                                                              */
/* ------------------------------------------------------------------------ */

/* Inline edit: text that turns into a field in place. Click (or Enter when
 * focused) to edit, Enter or clicking away saves, Esc cancels. Returns
 * CUI_CHANGED when an edit is committed. */
typedef struct cui_inline_edit_opts { int _; cui_font font; float size; const char *placeholder; cui_size width; } cui_inline_edit_opts;
CUI_API int cui_inline_edit_ex(cui_ctx *ui, const char *id, char *buf, size_t cap, const cui_inline_edit_opts *opts);

/* Scrub field: drag sideways anywhere on it to change the value (Shift for
 * fine steps), or click to type a number. Arrow keys step when focused. */
typedef struct cui_scrub_opts {
    int _;
    float step;                /* default (max - min) / 100 */
    const char *format;        /* default "%.0f" */
    const char *suffix;        /* e.g. "px" or "%" */
    const char *icon;          /* shown instead of the label */
    cui_size width;
} cui_scrub_opts;
CUI_API bool cui_scrub_ex(cui_ctx *ui, const char *label, float *value, float min, float max, const cui_scrub_opts *opts);

/* Money field: shows the amount with thousands separators, edits as plain
 * digits, keeps a fixed number of decimals. */
typedef struct cui_money_opts { int _; const char *label; const char *symbol; const char *code; int decimals; cui_size width; const char *hint; } cui_money_opts;
CUI_API bool cui_money_input_ex(cui_ctx *ui, const char *id, double *value, const cui_money_opts *opts);

/* Shortcut recorder: click and press a key combination to record it.
 * Backspace clears, Esc cancels. Returns true when the combination changed.
 * cui_shortcut_label formats a combination ("Ctrl Shift K"). */
typedef struct cui_key_combo { int mods; cui_key key; } cui_key_combo;
typedef struct cui_shortcut_recorder_opts { int _; const char *placeholder; cui_size width; } cui_shortcut_recorder_opts;
CUI_API bool cui_shortcut_recorder_ex(cui_ctx *ui, const char *id, cui_key_combo *combo, const cui_shortcut_recorder_opts *opts);
CUI_API const char *cui_shortcut_label(cui_ctx *ui, cui_key_combo combo);
CUI_API const char *cui_key_name(cui_key key);

/* ---- Node editor ---- */

/* Node editor: nodes with input and output pins on a canvas that pans and
 * zooms, joined by curved links. You own the nodes, pins and links; the
 * editor moves the node positions you pass and reports what the user did:
 *
 *     if (cui_node_editor_begin(ui, "graph", &view, .height = CUI_PX(480))) {
 *         for (each node n) {
 *             cui_node_begin(ui, n->id, &n->x, &n->y, n->title);
 *             cui_node_input(ui, n->in_pin, "Color", .type = 1);
 *             cui_node_content(ui) { cui_slider(ui, "##fac", &n->factor, 0, 1); }
 *             cui_node_output(ui, n->out_pin, "Result", .type = 1);
 *             cui_node_end(ui);
 *         }
 *         for (each link l) cui_node_link(ui, l->id, l->from_pin, l->to_pin);
 *         int a, b, id;
 *         while (cui_node_link_created(ui, &a, &b)) add_link(a, b);   (output pin, input pin)
 *         while (cui_node_link_removed(ui, &id)) remove_link(id);
 *         while (cui_node_deleted(ui, &id)) remove_node(id);
 *         cui_node_editor_end(ui);
 *     }
 *
 * Node, pin and link ids are your own integers (pin ids unique in the
 * editor). Scroll to zoom around the pointer; pan with the middle button or
 * Space and drag. Drag a card to move it (with the rest of the selection),
 * click to select, Shift or Ctrl to add, drag on the empty canvas for a
 * marquee. Drag from a pin to make a link; drag a link's input end off its
 * pin to detach it. Delete removes the selection, F frames every node. */
typedef struct cui_node_view { float x, y, zoom; } cui_node_view;   /* pan in points; zoom 0 means 1 */
typedef struct cui_node_editor_opts {
    int _;
    cui_size width, height;    /* default: fill the width, 440 tall */
    float min_zoom, max_zoom;  /* default 0.25 .. 2 */
    bool no_grid;
    bool minimap;              /* an overview of the graph in the bottom right corner; click it to move there */
} cui_node_editor_opts;
CUI_API bool cui_node_editor_begin_ex(cui_ctx *ui, const char *id, cui_node_view *view, const cui_node_editor_opts *opts);
CUI_API void cui_node_editor_end(cui_ctx *ui);
/* A node at canvas position (*x, *y), updated while it is dragged. Returns
 * true while the node is selected. */
typedef struct cui_node_opts { int _; float width; const char *icon; } cui_node_opts;   /* width default 184 */
CUI_API bool cui_node_begin_ex(cui_ctx *ui, int node_id, float *x, float *y, const char *title, const cui_node_opts *opts);
CUI_API void cui_node_end(cui_ctx *ui);
/* A row with a pin on the left (input) or right (output). type picks the pin
 * color from the series palette and limits links to pins of the same type
 * (0 connects to anything). Returns true when the pin has a link. */
typedef struct cui_pin_opts { int _; int type; } cui_pin_opts;
CUI_API bool cui_node_input_ex(cui_ctx *ui, int pin_id, const char *label, const cui_pin_opts *opts);
CUI_API bool cui_node_output_ex(cui_ctx *ui, int pin_id, const char *label, const cui_pin_opts *opts);
/* A row for ordinary components. They are laid out at 100% and scaled with
 * the view, so their text is sharpest at 100% zoom and below. */
CUI_API void cui_node_content_begin(cui_ctx *ui);
CUI_API void cui_node_content_end(cui_ctx *ui);
/* A link from an output pin to an input pin. Returns true while it is selected. */
CUI_API bool cui_node_link(cui_ctx *ui, int link_id, int from_pin, int to_pin);
/* What the user did this frame, one event per call; call them after
 * cui_node_editor_begin (and up to the end of the frame). */
CUI_API bool cui_node_link_created(cui_ctx *ui, int *from_pin, int *to_pin);
CUI_API bool cui_node_link_removed(cui_ctx *ui, int *link_id);
CUI_API bool cui_node_deleted(cui_ctx *ui, int *node_id);
#ifndef __cplusplus
#define cui_node_editor_begin(ui, id, view, ...) cui_node_editor_begin_ex((ui), (id), (view), CUI__OPTS(cui_node_editor_opts, __VA_ARGS__))
#define cui_node_begin(ui, node_id, x, y, title, ...) cui_node_begin_ex((ui), (node_id), (x), (y), (title), CUI__OPTS(cui_node_opts, __VA_ARGS__))
#define cui_node_input(ui, pin, label, ...)  cui_node_input_ex((ui), (pin), (label), CUI__OPTS(cui_pin_opts, __VA_ARGS__))
#define cui_node_output(ui, pin, label, ...) cui_node_output_ex((ui), (pin), (label), CUI__OPTS(cui_pin_opts, __VA_ARGS__))
#define cui_node_content(ui)                 CUI__SCOPE(cui_node_content_begin(ui), cui_node_content_end(ui))
#endif

/* ------------------------------------------------------------------------ */
/* App chrome: menu bar, toolbar, status bar, document tabs                 */
/* ------------------------------------------------------------------------ */

/* Menu bar: File / Edit / View along the top of a window. Each menu holds
 * the usual menu items (cui_menu_item, cui_menu_separator, cui_menu_label).
 * Once a menu is open, hovering another one switches to it. Alt (on its own)
 * or F10 focuses the bar; arrows move between menus and items, Enter
 * chooses, Esc steps back. Alt + a menu's mnemonic letter opens it (the
 * first letter of its label unless `mnemonic` says otherwise). */
typedef struct cui_menu_bar_opts { int _; bool no_border; } cui_menu_bar_opts;
CUI_API void cui_menu_bar_begin(cui_ctx *ui, const char *id, const cui_menu_bar_opts *opts);
CUI_API void cui_menu_bar_end(cui_ctx *ui);
/* One menu of the bar. Returns true while its items should be built; call
 * cui_menu_bar_menu_end only then. */
typedef struct cui_menu_bar_menu_opts { int _; char mnemonic; float width; bool disabled; } cui_menu_bar_menu_opts;
CUI_API bool cui_menu_bar_menu_begin(cui_ctx *ui, const char *label, const cui_menu_bar_menu_opts *opts);
CUI_API void cui_menu_bar_menu_end(cui_ctx *ui);

/* Toolbar: a row of compact icon buttons, toggles, separators and small text
 * buttons. Icon only buttons show their tooltip as their name. Items that do
 * not fit collapse, from the end, into a "more" menu; an item chosen there
 * returns true on the next frame. */
typedef struct cui_toolbar_opts { int _; bool no_border; } cui_toolbar_opts;
CUI_API void cui_toolbar_begin(cui_ctx *ui, const char *id, const cui_toolbar_opts *opts);
CUI_API void cui_toolbar_end(cui_ctx *ui);
typedef struct cui_tool_opts {
    int _;
    const char *label;         /* text on the button (icon may be NULL); also its name in the overflow menu */
    const char *tooltip;       /* the name of an icon only button */
    const char *shortcut;      /* shown in the tooltip, e.g. "Ctrl B" */
    bool *toggle;              /* toggle button: flipped on click, shown pressed while true */
    bool disabled;
} cui_tool_opts;
CUI_API bool cui_toolbar_button_ex(cui_ctx *ui, const char *icon, const cui_tool_opts *opts);
CUI_API void cui_toolbar_separator(cui_ctx *ui);
CUI_API void cui_toolbar_spacer(cui_ctx *ui);   /* pushes the following items to the right */

/* Status bar: a thin bar along the bottom of a window, with a hairline on
 * top and no fill of its own (put it in a box with the background you want).
 * Items go left to right; cui_status_bar_spacer sends the rest to the right
 * edge. */
typedef struct cui_status_bar_opts { int _; bool no_border; } cui_status_bar_opts;
CUI_API void cui_status_bar_begin(cui_ctx *ui, const char *id, const cui_status_bar_opts *opts);
CUI_API void cui_status_bar_end(cui_ctx *ui);
CUI_API void cui_status_bar_spacer(cui_ctx *ui);
typedef struct cui_status_item_opts { int _; const char *icon; cui_tone tone; const char *tooltip; bool disabled; } cui_status_item_opts;
/* A message; `tone` colors its icon. */
CUI_API void cui_status_bar_text_ex(cui_ctx *ui, const char *text, const cui_status_item_opts *opts);
/* A small clickable item ("Ln 12, Col 4", "UTF-8"). Returns true when clicked. */
CUI_API bool cui_status_bar_item_ex(cui_ctx *ui, const char *label, const cui_status_item_opts *opts);
/* A small progress bar with an optional label. value is 0..1. */
typedef struct cui_status_progress_opts { int _; const char *label; bool indeterminate; float width; } cui_status_progress_opts;
CUI_API void cui_status_bar_progress_ex(cui_ctx *ui, float value, const cui_status_progress_opts *opts);

/* Document tabs: editor style tabs. Clicking selects (*selected is
 * updated), the x or a middle click asks to close, dragging reorders, and
 * the strip scrolls sideways when the tabs do not fit. A dirty tab shows a
 * dot that turns into the close button under the pointer. The selected tab
 * is filled with the surface color so it joins the content below; the strip
 * itself has no fill, like the other bars. Tabs keep their
 * state by title, so give two tabs with the same name a key: "main.c##src".
 * The component never removes or moves your tabs itself: it reports a close
 * or a move in the result and you apply it (selection already follows a
 * move). With keyboard focus on the strip, Left and Right select, Home and
 * End jump, Shift+Left/Right move the tab and Delete closes it. With
 * `shortcuts` set it also answers Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+PageUp/Down
 * and Ctrl+W (Cmd+W on macOS) from anywhere in the window. */
typedef struct cui_doc_tab { const char *title; const char *icon; const char *tooltip; bool dirty; } cui_doc_tab;
typedef struct cui_doc_tabs_opts { int _; bool shortcuts; bool no_close; bool no_reorder; } cui_doc_tabs_opts;
typedef struct cui_doc_tabs_result {
    bool changed;              /* *selected changed (click, keys or a move) */
    int close;                 /* tab to close, or -1 */
    int move_from, move_to;    /* move the tab at move_from to move_to, or -1 */
} cui_doc_tabs_result;
CUI_API cui_doc_tabs_result cui_doc_tabs_ex(cui_ctx *ui, const char *id, const cui_doc_tab *tabs, int count, int *selected, const cui_doc_tabs_opts *opts);
/* Tool panels: property inspector, log console                             */
/* ------------------------------------------------------------------------ */

/* Property inspector: labelled rows in folding groups, a label column on the
 * left (drag the line between the columns to resize it) and an editor on the
 * right. Every row returns true when its value changed. Groups end at the
 * next group or at cui_inspector_end.
 *
 *     static bool transform_open = true;
 *     if (cui_inspector_begin(ui, "props")) {
 *         if (cui_inspector_group(ui, "Transform", &transform_open)) {
 *             cui_prop_vec3(ui, "Position", pos);
 *             cui_prop_float(ui, "Scale", &scale, .min = 0.01f, .max = 100, .step = 0.01f);
 *         }
 *         cui_inspector_end(ui);
 *     }
 *
 * Number rows are scrub fields: drag the field or its label sideways (Shift
 * for fine steps), click to type, arrows step. Rows can also be used outside
 * an inspector. */
typedef struct cui_inspector_opts {
    int _;
    cui_size width;            /* default fill */
    float label_width;         /* initial label column in points, default 38% of the width */
} cui_inspector_opts;
CUI_API bool cui_inspector_begin_ex(cui_ctx *ui, const char *id, const cui_inspector_opts *opts);
CUI_API void cui_inspector_end(cui_ctx *ui);
/* A folding group header. Build its rows only when this returns true (it
 * stays true while the group animates closed). open may be NULL for a plain
 * section that does not fold. */
CUI_API bool cui_inspector_group(cui_ctx *ui, const char *title, bool *open);

typedef struct cui_prop_opts {
    int _;
    float min, max;            /* number and vector rows; both 0 = unbounded */
    float step;                /* drag and arrow step, default 1/100 of the range (0.1 unbounded) */
    const char *format;        /* printf format, default from the step ("%.2f" for 0.01) */
    const char *suffix;        /* unit after the value, e.g. "px" or "deg" */
    const char *placeholder;   /* text rows */
    const char *tooltip;       /* shown on the label */
    bool disabled;
    bool checkbox;             /* bool rows: a checkbox instead of a switch */
} cui_prop_opts;
CUI_API bool cui_prop_float_ex(cui_ctx *ui, const char *label, float *value, const cui_prop_opts *opts);
CUI_API bool cui_prop_int_ex(cui_ctx *ui, const char *label, int *value, const cui_prop_opts *opts);
CUI_API bool cui_prop_text_ex(cui_ctx *ui, const char *label, char *buf, size_t cap, const cui_prop_opts *opts);
CUI_API bool cui_prop_bool_ex(cui_ctx *ui, const char *label, bool *value, const cui_prop_opts *opts);
CUI_API bool cui_prop_enum_ex(cui_ctx *ui, const char *label, const char *const *items, int count, int *index, const cui_prop_opts *opts);
/* A swatch and hex value that opens the color picker. */
CUI_API bool cui_prop_color_ex(cui_ctx *ui, const char *label, cui_color *value, const cui_prop_opts *opts);
/* Compact X, Y (Z, W) scrub fields side by side; value holds 2, 3 or 4 floats. */
CUI_API bool cui_prop_vec_ex(cui_ctx *ui, const char *label, float *value, int components, const cui_prop_opts *opts);

/* Log console. The app owns the storage: give cui_log_init an array of line
 * records and a byte buffer for the text. Appending never allocates; when
 * either is full the oldest lines are dropped (a ring buffer). Lines longer
 * than the text buffer are cut to fit.
 *
 *     static cui_log_line lines[4096];
 *     static char text[256 * 1024];
 *     static cui_log_buffer log;
 *     cui_log_init(&log, lines, 4096, text, sizeof text);
 *     log.now = seconds_since_start();             // optional timestamp source
 *     cui_log_append(&log, CUI_LOG_WARN, "Shader %s took %d ms", name, ms);
 *     ...
 *     cui_log_view(ui, "log", &log, .height = 320, .timestamps = true);
 *
 * Each append is one line: a trailing newline is dropped and other control
 * characters show as spaces. The view keeps its own filter, search, follow
 * and selection state (a small index of the matching lines, at most one entry
 * per line slot, freed with the context). */
typedef enum cui_log_level { CUI_LOG_TRACE = 0, CUI_LOG_DEBUG, CUI_LOG_INFO, CUI_LOG_WARN, CUI_LOG_ERROR, CUI_LOG_LEVEL_COUNT } cui_log_level;
typedef struct cui_log_line { uint32_t offset, len; double time; uint8_t level; } cui_log_line;
typedef struct cui_log_buffer {
    double now;                /* timestamp given to the next appended line (seconds) */
    /* everything below is managed by cui_log_*; read it, do not write it */
    cui_log_line *lines; int line_cap;
    char *text; uint32_t text_cap;
    int first, count;          /* ring of lines: oldest record and how many */
    uint32_t text_end;         /* where the next line's text goes */
    uint64_t total;            /* lines ever appended; line i of the buffer is number total - count + i */
    int level_count[CUI_LOG_LEVEL_COUNT];
} cui_log_buffer;
CUI_API void cui_log_init(cui_log_buffer *buf, cui_log_line *lines, int line_cap, char *text, size_t text_cap);
CUI_API void cui_log_append(cui_log_buffer *buf, cui_log_level level, const char *fmt, ...) CUI_PRINTF(3, 4);
CUI_API void cui_log_appendv(cui_log_buffer *buf, cui_log_level level, const char *fmt, va_list ap);
/* Appends len bytes (-1: up to the terminator) stamped with time. */
CUI_API void cui_log_append_text(cui_log_buffer *buf, cui_log_level level, double time, const char *text, int len);
CUI_API void cui_log_clear(cui_log_buffer *buf);
/* Line i (0 = oldest), or NULL when out of range. *text is terminated (and len bytes long). */
CUI_API const cui_log_line *cui_log_get(const cui_log_buffer *buf, int i, const char **text);

typedef struct cui_log_view_opts {
    int _;
    cui_size width;            /* default fill */
    float height;              /* default 320; negative fills the space */
    bool timestamps;           /* show each line's time as HH:MM:SS.mmm */
    bool no_toolbar;           /* just the lines */
    float size;                /* font size, default 12 */
} cui_log_view_opts;
/* Toolbar: level filters, a search field that filters and highlights,
 * Follow (keeps the newest line in view; scrolling up turns it off, reaching
 * the bottom turns it back on), Copy (selected lines, or all shown) and Clear.
 * Click a line to select it, Shift+click for a range; with the list focused,
 * arrows move, Shift+arrows extend, Ctrl+A selects all, Ctrl+C copies, Esc
 * clears the selection, End follows again. */
CUI_API void cui_log_view_ex(cui_ctx *ui, const char *id, cui_log_buffer *buf, const cui_log_view_opts *opts);

/* ------------------------------------------------------------------------ */
/* Custom drawing                                                           */
/* ------------------------------------------------------------------------ */

typedef struct cui_corners { float tl, tr, br, bl; } cui_corners;
#define CUI_CORNERS(r) (CUI__LIT(cui_corners){ (r), (r), (r), (r) })

CUI_API void cui_draw_rect(cui_ctx *ui, cui_rect r, cui_corners radius, cui_color fill);
CUI_API void cui_draw_rect_border(cui_ctx *ui, cui_rect r, cui_corners radius, cui_color fill, cui_color border, float border_width);
CUI_API void cui_draw_rect_gradient(cui_ctx *ui, cui_rect r, cui_corners radius, cui_color top, cui_color bottom);
CUI_API void cui_draw_shadow(cui_ctx *ui, cui_rect r, cui_corners radius, float blur, float spread, cui_vec2 offset, cui_color color);
CUI_API void cui_draw_line(cui_ctx *ui, cui_vec2 a, cui_vec2 b, float thickness, cui_color color);
CUI_API void cui_draw_polyline(cui_ctx *ui, const cui_vec2 *points, int count, float thickness, cui_color color, bool closed);
CUI_API void cui_draw_polygon(cui_ctx *ui, const cui_vec2 *points, int count, cui_color color);   /* convex */
CUI_API void cui_draw_circle(cui_ctx *ui, cui_vec2 center, float radius, cui_color color);
CUI_API void cui_draw_arc(cui_ctx *ui, cui_vec2 center, float radius, float a0, float a1, float thickness, cui_color color);
CUI_API void cui_draw_text(cui_ctx *ui, cui_font font, float size, cui_vec2 pos, cui_color color, const char *text, int len);
CUI_API void cui_draw_icon(cui_ctx *ui, const char *icon, cui_rect r, float size, cui_color color);
CUI_API void cui_draw_image(cui_ctx *ui, cui_texture tex, cui_rect r, cui_rect uv, cui_corners radius, cui_color tint);
CUI_API void cui_push_clip(cui_ctx *ui, cui_rect r);
CUI_API void cui_pop_clip(cui_ctx *ui);

/* Canvas: a box of the given size that you paint yourself. The callback
 * runs after layout, with the final rectangle. Returns interaction info. */
typedef void (*cui_paint_fn)(cui_ctx *ui, cui_rect rect, void *user);
typedef struct cui_canvas_result { cui_rect rect; bool hovered, pressed, clicked; cui_vec2 mouse; } cui_canvas_result;
CUI_API cui_canvas_result cui_canvas(cui_ctx *ui, const char *id, cui_size width, cui_size height, cui_paint_fn paint, const void *user, size_t user_size);

/* Persistent per-id storage for your own components (zeroed on first use). */
CUI_API void *cui_state(cui_ctx *ui, cui_id id, size_t size);

/* Animation helpers. Values move toward target with a spring and request
 * frames while moving. Reduced motion makes them snap. */
CUI_API float cui_animate(cui_ctx *ui, cui_id id, float target);                 /* smooth, no overshoot */
CUI_API float cui_animate_spring(cui_ctx *ui, cui_id id, float target, float duration, float bounce);

/* ------------------------------------------------------------------------ */
/* Draw data (consumed by renderers)                                        */
/* ------------------------------------------------------------------------ */

/* Vertex shading modes. See docs/rendering.md for the reference shader. */
enum { CUI_SHADE_TEXT = 0, CUI_SHADE_RRECT = 1, CUI_SHADE_SHADOW = 2, CUI_SHADE_IMAGE = 3, CUI_SHADE_SOLID = 4 };

typedef struct cui_vertex {
    float x, y;            /* position in pixels */
    float u, v;            /* texture coordinate */
    float lx, ly;          /* position relative to the shape center, pixels */
    float hw, hh;          /* half size of the shape, pixels */
    float r_tl, r_tr, r_br, r_bl;  /* corner radii, pixels */
    float border;          /* border width (RRECT) or blur sigma (SHADOW) */
    float mode;            /* CUI_SHADE_* */
    cui_color color;       /* fill */
    cui_color border_color;
} cui_vertex;

typedef struct cui_draw_cmd {
    cui_rect clip;         /* pixels */
    cui_texture texture;   /* 0 = font atlas */
    uint32_t index_offset;
    uint32_t index_count;
} cui_draw_cmd;

typedef struct cui_draw_data {
    const cui_vertex *vertices;
    int vertex_count;
    const uint32_t *indices;
    int index_count;
    const cui_draw_cmd *commands;
    int command_count;
    int width_px, height_px;   /* framebuffer size */
    cui_color clear_color;
    cui_texture font_atlas;
} cui_draw_data;

/* Valid after cui_end_frame until the next cui_begin_frame. */
CUI_API const cui_draw_data *cui_get_draw_data(cui_ctx *ui);

/* ------------------------------------------------------------------------ */
/* Named-argument macros                                                    */
/* ------------------------------------------------------------------------ */

#ifndef __cplusplus
#define CUI__OPTS(type, ...) (&(type){ ._ = 0, __VA_ARGS__ })
#define CUI__CAT2(a, b) a##b
#define CUI__CAT(a, b) CUI__CAT2(a, b)
#define CUI__V CUI__CAT(cui__scope_, __LINE__)
/* Runs the body once between begin and end. */
#define CUI__SCOPE(begin, end) for (int CUI__V = ((void)(begin), 0); !CUI__V; CUI__V = 1, (end))
/* Runs the body (and end) only when begin returns true. */
#define CUI__SCOPE_IF(begin, end) for (int CUI__V = !(begin); !CUI__V; CUI__V = 1, (end))

#define cui_box(ui, ...)        CUI__SCOPE(cui_box_begin((ui), CUI__OPTS(cui_box, __VA_ARGS__)), cui_box_end(ui))
#define cui_row(ui, ...)        CUI__SCOPE(cui_row_begin((ui), CUI__OPTS(cui_box, __VA_ARGS__)), cui_box_end(ui))
#define cui_column(ui, ...)     CUI__SCOPE(cui_column_begin((ui), CUI__OPTS(cui_box, __VA_ARGS__)), cui_box_end(ui))
#define cui_card(ui, ...)       CUI__SCOPE(cui_card_begin((ui), CUI__OPTS(cui_card_opts, __VA_ARGS__)), cui_card_end(ui))
#define cui_scroll(ui, ...)     CUI__SCOPE(cui_scroll_begin((ui), CUI__OPTS(cui_scroll_opts, __VA_ARGS__)), cui_scroll_end(ui))
#define cui_split(ui, id, ratio, ...) CUI__SCOPE(cui_split_begin((ui), (id), (ratio), CUI__OPTS(cui_split_opts, __VA_ARGS__)), cui_split_end(ui))
#define cui_id_scope(ui, str)   CUI__SCOPE(cui_push_id((ui), (str)), cui_pop_id(ui))
#define cui_disabled_scope(ui, d) CUI__SCOPE(cui_push_disabled((ui), (d)), cui_pop_disabled(ui))

#define cui_text(ui, text, ...)             cui_text_ex((ui), (text), CUI__OPTS(cui_text_opts, __VA_ARGS__))
#define cui_button(ui, label, ...)          cui_button_ex((ui), (label), CUI__OPTS(cui_button_opts, __VA_ARGS__))
#define cui_icon_button(ui, icon, ...)      cui_icon_button_ex((ui), (icon), CUI__OPTS(cui_button_opts, __VA_ARGS__))
#define cui_segmented(ui, id, items, n, idx, ...) cui_segmented_ex((ui), (id), (items), (n), (idx), CUI__OPTS(cui_segmented_opts, __VA_ARGS__))
#define cui_checkbox(ui, label, v, ...)     cui_checkbox_ex((ui), (label), (v), CUI__OPTS(cui_check_opts, __VA_ARGS__))
#define cui_switch(ui, label, v, ...)       cui_switch_ex((ui), (label), (v), CUI__OPTS(cui_check_opts, __VA_ARGS__))
#define cui_radio(ui, label, v, opt, ...)   cui_radio_ex((ui), (label), (v), (opt), CUI__OPTS(cui_check_opts, __VA_ARGS__))
#define cui_slider(ui, label, v, lo, hi, ...) cui_slider_ex((ui), (label), (v), (lo), (hi), CUI__OPTS(cui_slider_opts, __VA_ARGS__))
#define cui_chip(ui, label, sel, ...)       cui_chip_ex((ui), (label), (sel), CUI__OPTS(cui_chip_opts, __VA_ARGS__))
#define cui_input(ui, id, buf, cap, ...)    cui_input_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_input_opts, __VA_ARGS__))
#define cui_textarea(ui, id, buf, cap, ...) cui_textarea_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_input_opts, __VA_ARGS__))
#define cui_search(ui, id, buf, cap, ...)   cui_search_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_input_opts, __VA_ARGS__))
#define cui_number(ui, id, v, lo, hi, ...)  cui_number_ex((ui), (id), (v), (lo), (hi), CUI__OPTS(cui_number_opts, __VA_ARGS__))
#define cui_select(ui, id, items, n, idx, ...) cui_select_ex((ui), (id), (items), (n), (idx), CUI__OPTS(cui_select_opts, __VA_ARGS__))
#define cui_date_picker(ui, id, v, ...)     cui_date_picker_ex((ui), (id), (v), CUI__OPTS(cui_date_opts, __VA_ARGS__))
#define cui_tabs(ui, id, items, n, idx, ...) cui_tabs_ex((ui), (id), (items), (n), (idx), CUI__OPTS(cui_tabs_opts, __VA_ARGS__))
#define cui_nav_item(ui, label, ...)        cui_nav_item_ex((ui), (label), CUI__OPTS(cui_nav_opts, __VA_ARGS__))
#define cui_popover(ui, id, ...)            CUI__SCOPE_IF(cui_popover_begin((ui), (id), CUI__OPTS(cui_popover_opts, __VA_ARGS__)), cui_popover_end(ui))
#define cui_menu(ui, label, ...)            CUI__SCOPE_IF(cui_menu_begin((ui), (label), CUI__OPTS(cui_menu_opts, __VA_ARGS__)), cui_menu_end(ui))
#define cui_context_menu(ui, id)            CUI__SCOPE_IF(cui_context_menu_begin((ui), (id)), cui_context_menu_end(ui))
#define cui_menu_item(ui, label, ...)       cui_menu_item_ex((ui), (label), CUI__OPTS(cui_menu_item_opts, __VA_ARGS__))
#define cui_dialog(ui, title, open, ...)    CUI__SCOPE_IF(cui_dialog_begin((ui), (title), (open), CUI__OPTS(cui_dialog_opts, __VA_ARGS__)), cui_dialog_end(ui))
#define cui_dialog_footer(ui)               CUI__SCOPE(cui_dialog_footer_begin(ui), cui_dialog_footer_end(ui))
#define cui_sheet(ui, title, open, ...)     CUI__SCOPE_IF(cui_sheet_begin((ui), (title), (open), CUI__OPTS(cui_sheet_opts, __VA_ARGS__)), cui_sheet_end(ui))
#define cui_toast(ui, title, ...)           cui_toast_ex((ui), (title), CUI__OPTS(cui_toast_opts, __VA_ARGS__))
#define cui_alert(ui, title, ...)           cui_alert_ex((ui), (title), CUI__OPTS(cui_alert_opts, __VA_ARGS__))
#define cui_progress(ui, value, ...)        cui_progress_ex((ui), (value), CUI__OPTS(cui_progress_opts, __VA_ARGS__))
#define cui_badge(ui, text, ...)            cui_badge_ex((ui), (text), CUI__OPTS(cui_badge_opts, __VA_ARGS__))
#define cui_avatar(ui, name, ...)           cui_avatar_ex((ui), (name), CUI__OPTS(cui_avatar_opts, __VA_ARGS__))
#define cui_empty_state(ui, title, ...)     cui_empty_state_ex((ui), (title), CUI__OPTS(cui_empty_opts, __VA_ARGS__))
#define cui_metric(ui, label, value, ...)   cui_metric_ex((ui), (label), (value), CUI__OPTS(cui_metric_opts, __VA_ARGS__))
#define cui_accordion(ui, title, open)      CUI__SCOPE_IF(cui_accordion_begin((ui), (title), (open)), cui_accordion_end(ui))
#define cui_tree(ui, label, open, ...)      CUI__SCOPE_IF(cui_tree_begin((ui), (label), (open), CUI__OPTS(cui_tree_opts, __VA_ARGS__)), cui_tree_end(ui))
#define cui_list_item(ui, title, ...)       cui_list_item_ex((ui), (title), CUI__OPTS(cui_list_item_opts, __VA_ARGS__))
#define cui_virtual_list(ui, id, n, rh, h, first, last) CUI__SCOPE(cui_virtual_list_begin((ui), (id), (n), (rh), (h), (first), (last)), cui_virtual_list_end(ui))
#define cui_table(ui, id, cols, ncols, ...) CUI__SCOPE_IF(cui_table_begin_ex((ui), (id), (cols), (ncols), CUI__OPTS(cui_table_opts, __VA_ARGS__)), cui_table_end(ui))
#define cui_line_chart(ui, id, s, n, ...)   cui_line_chart_ex((ui), (id), (s), (n), CUI__OPTS(cui_chart_opts, __VA_ARGS__))
#define cui_bar_chart(ui, id, s, n, ...)    cui_bar_chart_ex((ui), (id), (s), (n), CUI__OPTS(cui_chart_opts, __VA_ARGS__))
#define cui_donut_chart(ui, id, v, l, n, ...) cui_donut_chart_ex((ui), (id), (v), (l), (n), CUI__OPTS(cui_chart_opts, __VA_ARGS__))
#define cui_image(ui, tex, ...)             cui_image_ex((ui), (tex), CUI__OPTS(cui_image_opts, __VA_ARGS__))
#define cui_radio_cards(ui, id, ch, n, idx, ...) cui_radio_cards_ex((ui), (id), (ch), (n), (idx), CUI__OPTS(cui_radio_cards_opts, __VA_ARGS__))
#define cui_tag_input(ui, id, buf, cap, ...) cui_tag_input_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_tag_opts, __VA_ARGS__))
#define cui_button_group(ui)                CUI__SCOPE(cui_button_group_begin(ui), cui_button_group_end(ui))
#define cui_hold_button(ui, label, ...)     cui_hold_button_ex((ui), (label), CUI__OPTS(cui_hold_opts, __VA_ARGS__))
#define cui_reorder_item(ui, pos)           CUI__SCOPE(cui_reorder_item_begin((ui), (pos)), cui_reorder_item_end(ui))
#define cui_timeline_item(ui, title, ...)   cui_timeline_item_ex((ui), (title), CUI__OPTS(cui_timeline_opts, __VA_ARGS__))
#define cui_kanban_column_begin(ui, title, count, ...) cui_kanban_column_begin_ex((ui), (title), (count), CUI__OPTS(cui_kanban_column_opts, __VA_ARGS__))
#define cui_week_calendar(ui, id, week, events, count, ...) cui_week_calendar_ex((ui), (id), (week), (events), (count), CUI__OPTS(cui_week_opts, __VA_ARGS__))
#define cui_gantt(ui, id, tasks, count, origin, days, ...) cui_gantt_ex((ui), (id), (tasks), (count), (origin), (days), CUI__OPTS(cui_gantt_opts, __VA_ARGS__))
#define cui_reasoning(ui, id, ...)          CUI__SCOPE_IF(cui_reasoning_begin((ui), (id), CUI__OPTS(cui_reasoning_opts, __VA_ARGS__)), cui_reasoning_end(ui))
#define cui_tool_call(ui, id, name, ...)    cui_tool_call_ex((ui), (id), (name), CUI__OPTS(cui_tool_call_opts, __VA_ARGS__))
#define cui_agent_plan(ui, id, steps, n, ...) cui_agent_plan_ex((ui), (id), (steps), (n), CUI__OPTS(cui_agent_plan_opts, __VA_ARGS__))
#define cui_agent_status(ui, text, ...)     cui_agent_status_ex((ui), (text), CUI__OPTS(cui_agent_status_opts, __VA_ARGS__))
#define cui_context_meter(ui, id, used, limit, ...) cui_context_meter_ex((ui), (id), (used), (limit), CUI__OPTS(cui_context_opts, __VA_ARGS__))
#define cui_response_actions(ui, id, ...)   cui_response_actions_ex((ui), (id), CUI__OPTS(cui_response_actions_opts, __VA_ARGS__))
#define cui_prompt_suggestions(ui, id, items, n, ...) cui_prompt_suggestions_ex((ui), (id), (items), (n), CUI__OPTS(cui_prompt_suggestions_opts, __VA_ARGS__))
#define cui_model_picker(ui, id, models, n, sel, ...) cui_model_picker_ex((ui), (id), (models), (n), (sel), CUI__OPTS(cui_model_picker_opts, __VA_ARGS__))
#define cui_chat_composer(ui, id, buf, cap, ...) cui_chat_composer_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_composer_opts, __VA_ARGS__))
#define cui_voice_orb(ui, id, state, level, ...) cui_voice_orb_ex((ui), (id), (state), (level), CUI__OPTS(cui_voice_orb_opts, __VA_ARGS__))
#define cui_shimmer_text(ui, text, ...)    cui_shimmer_text_ex((ui), (text), CUI__OPTS(cui_text_fx_opts, __VA_ARGS__))
#define cui_typewriter(ui, id, text, ...)   cui_typewriter_ex((ui), (id), (text), CUI__OPTS(cui_text_fx_opts, __VA_ARGS__))
#define cui_text_reveal(ui, id, text, ...)  cui_text_reveal_ex((ui), (id), (text), CUI__OPTS(cui_text_fx_opts, __VA_ARGS__))
#define cui_word_rotate(ui, id, words, n, interval, ...) cui_word_rotate_ex((ui), (id), (words), (n), (interval), CUI__OPTS(cui_text_fx_opts, __VA_ARGS__))
#define cui_number_roll(ui, id, text, ...)  cui_number_roll_ex((ui), (id), (text), CUI__OPTS(cui_text_fx_opts, __VA_ARGS__))
#define cui_gradient_text(ui, text, ...)  cui_gradient_text_ex((ui), (text), CUI__OPTS(cui_gradient_text_opts, __VA_ARGS__))
#define cui_spotlight(ui, id, ...)          CUI__SCOPE(cui_spotlight_begin((ui), (id), CUI__OPTS(cui_spotlight_opts, __VA_ARGS__)), cui_spotlight_end(ui))
#define cui_loader(ui, ...)                 cui_loader_ex((ui), CUI__OPTS(cui_loader_opts, __VA_ARGS__))
#define cui_dock(ui, id, ...)               CUI__SCOPE(cui_dock_begin((ui), (id), CUI__OPTS(cui_dock_opts, __VA_ARGS__)), cui_dock_end(ui))
#define cui_dock_item(ui, icon, label, ...) cui_dock_item_ex((ui), (icon), (label), CUI__OPTS(cui_dock_item_opts, __VA_ARGS__))
#define cui_tab_bar(ui, id, items, n, sel, ...) cui_tab_bar_ex((ui), (id), (items), (n), (sel), CUI__OPTS(cui_tab_bar_opts, __VA_ARGS__))
#define cui_control_tile(ui, label, on, ...) cui_control_tile_ex((ui), (label), (on), CUI__OPTS(cui_tile_opts, __VA_ARGS__))
#define cui_waveform(ui, id, peaks, n, progress, ...) cui_waveform_ex((ui), (id), (peaks), (n), (progress), CUI__OPTS(cui_waveform_opts, __VA_ARGS__))
#define cui_now_playing(ui, id, np, ...)    cui_now_playing_ex((ui), (id), (np), CUI__OPTS(cui_now_playing_opts, __VA_ARGS__))
#define cui_heatmap(ui, id, v, rows, cols, ...) cui_heatmap_ex((ui), (id), (v), (rows), (cols), CUI__OPTS(cui_heatmap_opts, __VA_ARGS__))
#define cui_calendar_heatmap(ui, id, start, v, days, ...) cui_calendar_heatmap_ex((ui), (id), (start), (v), (days), CUI__OPTS(cui_calendar_heatmap_opts, __VA_ARGS__))
#define cui_treemap(ui, id, items, n, ...)  cui_treemap_ex((ui), (id), (items), (n), CUI__OPTS(cui_treemap_opts, __VA_ARGS__))
#define cui_radar_chart(ui, id, s, n, axes, na, ...) cui_radar_chart_ex((ui), (id), (s), (n), (axes), (na), CUI__OPTS(cui_radar_opts, __VA_ARGS__))
#define cui_funnel(ui, id, stages, n, ...)  cui_funnel_ex((ui), (id), (stages), (n), CUI__OPTS(cui_funnel_opts, __VA_ARGS__))
#define cui_sankey(ui, id, nodes, nn, links, nl, ...) cui_sankey_ex((ui), (id), (nodes), (nn), (links), (nl), CUI__OPTS(cui_sankey_opts, __VA_ARGS__))
#define cui_waffle(ui, id, items, n, ...)   cui_waffle_ex((ui), (id), (items), (n), CUI__OPTS(cui_waffle_opts, __VA_ARGS__))
#define cui_split_bar(ui, id, items, n, ...) cui_split_bar_ex((ui), (id), (items), (n), CUI__OPTS(cui_split_bar_opts, __VA_ARGS__))
#define cui_bar_list(ui, id, items, n, ...) cui_bar_list_ex((ui), (id), (items), (n), CUI__OPTS(cui_bar_list_opts, __VA_ARGS__))
#define cui_activity_rings(ui, id, rings, n, ...) cui_activity_rings_ex((ui), (id), (rings), (n), CUI__OPTS(cui_rings_opts, __VA_ARGS__))
#define cui_scatter_chart(ui, id, s, n, ...) cui_scatter_chart_ex((ui), (id), (s), (n), CUI__OPTS(cui_scatter_opts, __VA_ARGS__))
#define cui_chat_message(ui, text, ...)     cui_chat_message_ex((ui), (text), CUI__OPTS(cui_chat_opts, __VA_ARGS__))
#define cui_comment(ui, id, author, text, ...) cui_comment_ex((ui), (id), (author), (text), CUI__OPTS(cui_comment_opts, __VA_ARGS__))
#define cui_carousel(ui, id, count, index, ...) CUI__SCOPE(cui_carousel_begin((ui), (id), (count), (index), CUI__OPTS(cui_carousel_opts, __VA_ARGS__)), cui_carousel_end(ui))
#define cui_carousel_slide(ui)              CUI__SCOPE(cui_carousel_slide_begin(ui), cui_carousel_slide_end(ui))
#define cui_image_compare(ui, id, before, after, pos, ...) cui_image_compare_ex((ui), (id), (before), (after), (pos), CUI__OPTS(cui_compare_opts, __VA_ARGS__))
#define cui_card_stack_item(ui, depth)      CUI__SCOPE(cui_card_stack_item_begin((ui), (depth)), cui_card_stack_item_end(ui))
#define cui_counter(ui, id, value, ...)     cui_counter_ex((ui), (id), (value), CUI__OPTS(cui_counter_opts, __VA_ARGS__))
#define cui_ticker(ui, id, items, count, ...) cui_ticker_ex((ui), (id), (items), (count), CUI__OPTS(cui_ticker_opts, __VA_ARGS__))
#define cui_data_grid_begin(ui, id, cols, ncols, rows, first, last, ...) cui_data_grid_begin_ex((ui), (id), (cols), (ncols), (rows), (first), (last), CUI__OPTS(cui_grid_opts, __VA_ARGS__))
#define cui_code_block(ui, id, code, ...)   cui_code_block_ex((ui), (id), (code), CUI__OPTS(cui_code_opts, __VA_ARGS__))
#define cui_json_view(ui, id, json, ...)    cui_json_view_ex((ui), (id), (json), CUI__OPTS(cui_json_opts, __VA_ARGS__))
#define cui_hover_card(ui, id, ...)         CUI__SCOPE_IF(cui_hover_card_begin((ui), (id), CUI__OPTS(cui_hover_card_opts, __VA_ARGS__)), cui_hover_card_end(ui))
#define cui_confirm_popover(ui, id, ...)    cui_confirm_popover_ex((ui), (id), CUI__OPTS(cui_confirm_popover_opts, __VA_ARGS__))
#define cui_expandable_text(ui, id, text, ...) cui_expandable_text_ex((ui), (id), (text), CUI__OPTS(cui_expandable_text_opts, __VA_ARGS__))
#define cui_expandable_card(ui, title, open, ...) CUI__SCOPE_IF(cui_expandable_card_begin((ui), (title), (open), CUI__OPTS(cui_expandable_card_opts, __VA_ARGS__)), cui_expandable_card_end(ui))
#define cui_banner(ui, text, open, ...)     cui_banner_ex((ui), (text), (open), CUI__OPTS(cui_banner_opts, __VA_ARGS__))
#define cui_countdown(ui, id, seconds, ...) cui_countdown_ex((ui), (id), (seconds), CUI__OPTS(cui_countdown_opts, __VA_ARGS__))
#define cui_checklist(ui, id, steps, count, ...) cui_checklist_ex((ui), (id), (steps), (count), CUI__OPTS(cui_checklist_opts, __VA_ARGS__))
#define cui_multi_select(ui, id, items, count, selected, ...) cui_multi_select_ex((ui), (id), (items), (count), (selected), CUI__OPTS(cui_multi_select_opts, __VA_ARGS__))
#define cui_date_range_picker(ui, id, range, ...) cui_date_range_picker_ex((ui), (id), (range), CUI__OPTS(cui_date_range_opts, __VA_ARGS__))
#define cui_time_picker(ui, id, hour, minute, ...) cui_time_picker_ex((ui), (id), (hour), (minute), CUI__OPTS(cui_time_opts, __VA_ARGS__))
#define cui_wheel_picker(ui, id, items, count, index, ...) cui_wheel_picker_ex((ui), (id), (items), (count), (index), CUI__OPTS(cui_wheel_opts, __VA_ARGS__))
#define cui_ruler_slider(ui, label, value, min, max, ...) cui_ruler_slider_ex((ui), (label), (value), (min), (max), CUI__OPTS(cui_ruler_opts, __VA_ARGS__))
#define cui_histogram_slider(ui, label, bins, n, lo, hi, min, max, ...) cui_histogram_slider_ex((ui), (label), (bins), (n), (lo), (hi), (min), (max), CUI__OPTS(cui_histogram_opts, __VA_ARGS__))
#define cui_signature_pad(ui, id, ...)      cui_signature_pad_ex((ui), (id), CUI__OPTS(cui_signature_opts, __VA_ARGS__))
#define cui_dropzone(ui, id, ...)           cui_dropzone_ex((ui), (id), CUI__OPTS(cui_dropzone_opts, __VA_ARGS__))
#define cui_split_button(ui, label, items, count, ...) cui_split_button_ex((ui), (label), (items), (count), CUI__OPTS(cui_split_button_opts, __VA_ARGS__))
#define cui_toggle_group(ui, id, labels, count, values, ...) cui_toggle_group_ex((ui), (id), (labels), (count), (values), CUI__OPTS(cui_toggle_group_opts, __VA_ARGS__))
#define cui_confirm_button(ui, label, ...)  cui_confirm_button_ex((ui), (label), CUI__OPTS(cui_confirm_opts, __VA_ARGS__))
#define cui_status_button(ui, label, status, ...) cui_status_button_ex((ui), (label), (status), CUI__OPTS(cui_status_button_opts, __VA_ARGS__))
#define cui_speed_dial(ui, id, actions, count, ...) cui_speed_dial_ex((ui), (id), (actions), (count), CUI__OPTS(cui_speed_dial_opts, __VA_ARGS__))
#define cui_user_menu(ui, name, ...)        CUI__SCOPE_IF(cui_user_menu_begin((ui), (name), CUI__OPTS(cui_user_menu_opts, __VA_ARGS__)), cui_user_menu_end(ui))
#define cui_inline_edit(ui, id, buf, cap, ...) cui_inline_edit_ex((ui), (id), (buf), (cap), CUI__OPTS(cui_inline_edit_opts, __VA_ARGS__))
#define cui_scrub(ui, label, value, min, max, ...) cui_scrub_ex((ui), (label), (value), (min), (max), CUI__OPTS(cui_scrub_opts, __VA_ARGS__))
#define cui_money_input(ui, id, value, ...) cui_money_input_ex((ui), (id), (value), CUI__OPTS(cui_money_opts, __VA_ARGS__))
#define cui_shortcut_recorder(ui, id, combo, ...) cui_shortcut_recorder_ex((ui), (id), (combo), CUI__OPTS(cui_shortcut_recorder_opts, __VA_ARGS__))
/* app chrome */
#define cui_menu_bar(ui, id, ...)           CUI__SCOPE(cui_menu_bar_begin((ui), (id), CUI__OPTS(cui_menu_bar_opts, __VA_ARGS__)), cui_menu_bar_end(ui))
#define cui_menu_bar_menu(ui, label, ...)   CUI__SCOPE_IF(cui_menu_bar_menu_begin((ui), (label), CUI__OPTS(cui_menu_bar_menu_opts, __VA_ARGS__)), cui_menu_bar_menu_end(ui))
#define cui_toolbar(ui, id, ...)            CUI__SCOPE(cui_toolbar_begin((ui), (id), CUI__OPTS(cui_toolbar_opts, __VA_ARGS__)), cui_toolbar_end(ui))
#define cui_toolbar_button(ui, icon, ...)   cui_toolbar_button_ex((ui), (icon), CUI__OPTS(cui_tool_opts, __VA_ARGS__))
#define cui_status_bar(ui, id, ...)         CUI__SCOPE(cui_status_bar_begin((ui), (id), CUI__OPTS(cui_status_bar_opts, __VA_ARGS__)), cui_status_bar_end(ui))
#define cui_status_bar_text(ui, text, ...)  cui_status_bar_text_ex((ui), (text), CUI__OPTS(cui_status_item_opts, __VA_ARGS__))
#define cui_status_bar_item(ui, label, ...) cui_status_bar_item_ex((ui), (label), CUI__OPTS(cui_status_item_opts, __VA_ARGS__))
#define cui_status_bar_progress(ui, value, ...) cui_status_bar_progress_ex((ui), (value), CUI__OPTS(cui_status_progress_opts, __VA_ARGS__))
#define cui_doc_tabs(ui, id, tabs, count, selected, ...) cui_doc_tabs_ex((ui), (id), (tabs), (count), (selected), CUI__OPTS(cui_doc_tabs_opts, __VA_ARGS__))
/* tool panels */
#define cui_inspector_begin(ui, id, ...)    cui_inspector_begin_ex((ui), (id), CUI__OPTS(cui_inspector_opts, __VA_ARGS__))
#define cui_inspector(ui, id, ...)          CUI__SCOPE_IF(cui_inspector_begin_ex((ui), (id), CUI__OPTS(cui_inspector_opts, __VA_ARGS__)), cui_inspector_end(ui))
#define cui_prop_float(ui, label, v, ...)   cui_prop_float_ex((ui), (label), (v), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_int(ui, label, v, ...)     cui_prop_int_ex((ui), (label), (v), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_text(ui, label, buf, cap, ...) cui_prop_text_ex((ui), (label), (buf), (cap), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_bool(ui, label, v, ...)    cui_prop_bool_ex((ui), (label), (v), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_enum(ui, label, items, n, idx, ...) cui_prop_enum_ex((ui), (label), (items), (n), (idx), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_color(ui, label, v, ...)   cui_prop_color_ex((ui), (label), (v), CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_vec2(ui, label, v, ...)    cui_prop_vec_ex((ui), (label), (v), 2, CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_vec3(ui, label, v, ...)    cui_prop_vec_ex((ui), (label), (v), 3, CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_prop_vec4(ui, label, v, ...)    cui_prop_vec_ex((ui), (label), (v), 4, CUI__OPTS(cui_prop_opts, __VA_ARGS__))
#define cui_log_view(ui, id, buf, ...)      cui_log_view_ex((ui), (id), (buf), CUI__OPTS(cui_log_view_opts, __VA_ARGS__))
#endif /* !__cplusplus */

#ifdef __cplusplus
}
#endif

#endif /* CUI_H */
