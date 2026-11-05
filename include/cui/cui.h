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

    float radius_control;   /* buttons, inputs                (default 18) */
    float radius_panel;     /* menus, popovers, nested panels (default 22) */
    float radius_surface;   /* cards, dialogs                 (default 28) */

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
    float gap;                 /* space between children */
    int columns;               /* CUI_GRID only */
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
    float pad, pad_x, pad_y, gap;
    cui_dir dir;
    cui_align align;
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

typedef struct cui_metric_opts { int _; float delta; const char *delta_label; const float *spark; int spark_count; const char *icon; cui_size width; } cui_metric_opts;
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
#endif /* !__cplusplus */

#ifdef __cplusplus
}
#endif

#endif /* CUI_H */
