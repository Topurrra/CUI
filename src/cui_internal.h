/* Internal declarations shared by the CUI implementation files.
 *
 * Every file in src/ includes this header. It holds what more than one file needs; anything
 * one file uses stays static in that file. Sections, in order (the file that implements each
 * is in its heading):
 *
 *   small math and color helpers     inline, here
 *   memory                           cui_core.c (allocator, frame arena, CUI__GROW)
 *   PNG and font outlines            cui_image.c, cui_text.c (used by color emoji)
 *   utf8 and strings                 cui_core.c
 *   hashing                          cui_core.c
 *   animation                        cui_core.c (springs, tweens) and the motion tokens
 *   fonts and text                   cui_text.c, cui_draw.c
 *   draw list                        cui_draw.c (the ordered dither primitive, layer groups); moving textures
 *                                    between renderers, render/cui_render_sw.c
 *   software screenshots             render/cui_render_sw.c
 *   layout tree                      cui_layout.c (cui_node, building and laying out the tree)
 *   per id state                     cui_core.c (cui_state, garbage collection)
 *   interaction                      cui_core.c (hit testing, focus, cui__interact)
 *   popups and toasts                cui_overlay.c
 *   the context                      struct cui_ctx: everything one context owns
 *   theme                            cui_theme.c, plus cui__fs and cui__emphasis
 *   user-facing text                 cui_strings.c (month and weekday names, for translation)
 *   shared widget pieces             cui_widgets.c, cui_controls.c, cui_overlay.c...
 *   text editing primitives          cui_textedit.c (shared with the code editor)
 *   syntax highlighting              cui_code.c (code block, code editor, diff viewer), cui_code_langs.c
 *   code editor folding and minimap  cui_fold.c, cui_minimap.c
 *   layered graph layout             cui_graph.c (dependency graph, mermaid flowcharts)
 *   cell grid                        cui_cellgrid.c (the pure parts: column search, selection keys; see below)
 *   mermaid diagrams                 cui_mermaid.c (fenced mermaid blocks in markdown)
 *
 * Naming: cui__ for internal functions, CUI__ for internal constants and macros, cui_ for
 * the public API in include/cui/. */
#ifndef CUI_INTERNAL_H
#define CUI_INTERNAL_H

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#ifndef CUI_BUILDING
#define CUI_BUILDING
#endif

/* The OpenType shaper (cui_shape.c) is in: all of it with CUI_TEXT_SHAPING, only the terminal's ligatures with
 * CUI_VT_LIGATURES (no bidi, no shaped labels). */
#if defined(CUI_TEXT_SHAPING) || defined(CUI_VT_LIGATURES)
#define CUI__SHAPER 1
#endif

#include "cui/cui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CUI_PI 3.14159265358979323846f

/* the frame probe (cui_set_probe): one pointer test when nobody listens */
extern cui_probe_fn cui__probe_fn;
extern void *cui__probe_user;
#define CUI__PROBE(mark) do { if (cui__probe_fn) cui__probe_fn(cui__probe_user, (mark)); } while (0)

#define CUI_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CUI_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CUI_CLAMP(x, lo, hi) CUI_MIN(CUI_MAX((x), (lo)), (hi))
#define CUI_COUNTOF(a) ((int)(sizeof(a) / sizeof((a)[0])))

static inline float cui__clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }
static inline float cui__lerp(float a, float b, float t) { return a + (b - a) * t; }
static inline bool cui__rect_contains(cui_rect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline cui_rect cui__rect_intersect(cui_rect a, cui_rect b) {
    float x0 = CUI_MAX(a.x, b.x), y0 = CUI_MAX(a.y, b.y);
    float x1 = CUI_MIN(a.x + a.w, b.x + b.w), y1 = CUI_MIN(a.y + a.h, b.y + b.h);
    cui_rect r = { x0, y0, CUI_MAX(0, x1 - x0), CUI_MAX(0, y1 - y0) };
    return r;
}
static inline cui_rect cui__rect_inset(cui_rect r, float d) {
    cui_rect o = { r.x + d, r.y + d, CUI_MAX(0, r.w - 2 * d), CUI_MAX(0, r.h - 2 * d) };
    return o;
}
static inline cui_color cui__rgba(int r, int g, int b, int a) {
    cui_color c = { (uint8_t)r, (uint8_t)g, (uint8_t)b, (uint8_t)a };
    return c;
}
static inline bool cui__is_token(cui_color c) { return c.a == 0 && c.g == 0xC5 && c.b == 0x1D && c.r != 0; }
static inline bool cui__color_set(cui_color c) { return c.a != 0 || cui__is_token(c); }

/* ---- memory ---- */
void *cui__alloc(cui_ctx *ui, size_t size);
size_t cui__inflate(const void *src, size_t src_len, void *dst, size_t dst_cap);  /* raw DEFLATE */
void *cui__realloc(cui_ctx *ui, void *p, size_t size);
void cui__free(cui_ctx *ui, void *p);
void *cui__frame_alloc(cui_ctx *ui, size_t size);       /* freed at frame start */
char *cui__frame_strdup(cui_ctx *ui, const char *s, int len);
char *cui__frame_printf(cui_ctx *ui, const char *fmt, ...) CUI_PRINTF(2, 3);
char *cui__frame_vprintf(cui_ctx *ui, const char *fmt, va_list ap);

#define CUI__GROW(ui, arr, count, cap, need) do {                                 \
    if ((count) + (need) > (cap)) {                                               \
        int ncap__ = (cap) ? (cap) * 2 : 64;                                      \
        while (ncap__ < (count) + (need)) ncap__ *= 2;                            \
        (arr) = cui__realloc((ui), (arr), sizeof(*(arr)) * (size_t)ncap__);        \
        (cap) = ncap__;                                                           \
    }                                                                             \
} while (0)

/* ---- PNG and font outlines, for color emoji (cui_emoji.c) ---- */
unsigned char *cui__png_decode(const unsigned char *data, int len, int *w, int *h);   /* cui_image.c */
void cui__png_free(unsigned char *px);
typedef struct cui__tt cui__tt;                                                        /* cui_text.c */
cui__tt *cui__tt_open(cui_ctx *ui, const unsigned char *data, int offset);
void cui__tt_vmetrics(cui__tt *t, int *ascent, int *descent);
bool cui__tt_box(cui__tt *t, int gid, float scale, int *x0, int *y0, int *x1, int *y1);
void cui__tt_render(cui__tt *t, int gid, float scale, unsigned char *out, int w, int h, int stride);

/* ---- utf8 ---- */
int cui__utf8_decode(const char *s, int len, uint32_t *cp);   /* returns bytes consumed (>=1) */
int cui__utf8_encode(uint32_t cp, char *out);
int cui__utf8_prev(const char *s, int pos);
int cui__utf8_next(const char *s, int len, int pos);
int cui__display_len(const char *label);                      /* length before "##" */
static inline int cui__ascii_lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
bool cui__contains_ci(const char *hay, const char *needle);   /* ASCII case-insensitive substring; an empty needle matches */

/* ---- hashing ---- */
uint32_t cui__hash(const void *data, size_t len, uint32_t seed);
uint32_t cui__hash_str(const char *s, uint32_t seed);

/* ---- animation ---- */
typedef struct cui_spring { float x, v; } cui_spring;
float cui__spring(cui_ctx *ui, cui_spring *s, float target, float duration, float bounce);
float cui__tween(cui_ctx *ui, float *x, float target, float duration);   /* exponential approach */
#define CUI_SPRING_SMOOTH 0.40f, 0.0f
#define CUI_SPRING_SNAPPY 0.26f, 0.12f
#define CUI_SPRING_MORPH  0.42f, 0.16f

/* Motion and state tokens. One value per job, so every component answers in
 * the same time and the same way. Tween durations are the time to settle. */
#define CUI__T_HIGHLIGHT 0.10f   /* list and menu rows that follow the pointer or the keys */
#define CUI__T_HOVER     0.14f   /* hover color, edges, icon color */
#define CUI__T_FOCUS     0.14f   /* focus rings and focused edges */
#define CUI__T_STATE     0.18f   /* checked, selected, on: fills that report a state */
#define CUI__SPRING_EXPAND 0.32f, 0.0f   /* disclosures, trees, accordions open and close */
#define CUI__SPRING_ENTER  0.28f, 0.0f   /* popovers, menus, tooltips, hover cards */
#define CUI__SPRING_DIALOG 0.34f, 0.0f   /* dialogs */
#define CUI__SPRING_EXIT   0.16f, 0.0f   /* every floating layer leaves faster than it came */
#define CUI__ENTER_SCALE 0.96f   /* floating layers grow from here */
#define CUI__PRESS       0.97f   /* buttons, chips, tiles */
#define CUI__PRESS_WIDE  0.98f   /* rows and cards: wide targets travel further at the same scale */
#define CUI__DISABLED    0.45f   /* opacity of anything disabled */
/* Radius steps derived from the theme, so a custom scale carries everywhere */
#define CUI__R_ITEM(ui)    CUI_MAX(6.0f, (ui)->theme.radius_panel - 6.0f)   /* rows inside a panel */
#define CUI__R_CONTROL(ui, h) CUI_MIN((ui)->theme.radius_control, (h) * 0.5f)
#define CUI__FIELD_PAD 12.0f      /* fields: text side */
#define CUI__FIELD_PAD_ICON 10.0f /* fields: the side with an icon */
#define CUI__R_SMALL 6.0f   /* marks inside rows: attachment chips, bars, events */

/* ---- fonts ---- */
typedef struct cui_glyph {
    uint32_t key_cp;
    uint16_t font;
    uint16_t size_q;     /* pixel size * 4 */
    uint8_t subpx;
    uint8_t pad_;
    uint32_t stamp;      /* the frame it was last drawn in, so a full atlas can drop old sizes */
    int16_t x0, y0;      /* offset of bitmap from pen position (pixels) */
    uint16_t w, h;
    uint16_t ax, ay;     /* atlas position */
    int next;            /* hash chain */
} cui_glyph;

typedef struct cui_font_data cui_font_data;

typedef struct cui_text_layout_line { int start, end; float width; } cui_text_layout_line;

#ifdef CUI_TEXT_SHAPING
bool cui__text_needs_shaping(cui_ctx *ui, const char *text, int len);
/* one line of a text field in reading order (cui_input_use_bidi, cui_text.c): wanted when the option is on
 * and the text has right-to-left characters; the caret x of a byte offset, the offset nearest x, the x pieces
 * (pairs in xs, at most max) a selection [s0, s1) covers, the shaped width, and the line drawn */
bool cui__bidi_wanted(cui_ctx *ui, const char *text, int len);
float cui__bidi_caret_x(cui_ctx *ui, cui_font font, float size, const char *text, int len, int off);
int cui__bidi_hit(cui_ctx *ui, cui_font font, float size, const char *text, int len, float x);
int cui__bidi_spans(cui_ctx *ui, cui_font font, float size, const char *text, int len, int s0, int s1, float *xs, int max);
float cui__bidi_width(cui_ctx *ui, cui_font font, float size, const char *text, int len);
void cui__bidi_draw(cui_ctx *ui, cui_font font, float size, float x, float y, cui_color color, const char *text, int len);
/* text area lines with cui_input_use_paragraphs: what one wrapped line [ls, le) of buf needs. The line belongs to the paragraph
 * between newlines; the paragraph's base direction (the field's explicit one, else the first strong character) orders it, and
 * a right-to-left paragraph is right-aligned (ox: the line's left edge from the area's left). cui__ta_ctx makes the cui__bidi_*
 * calls that follow it use the paragraph (call cui__ta_ctx_end after them); without the option it is the old per-line state. */
typedef struct cui__ta_line { bool bidi, rtl; float ox; const char *para; int plen, dir; } cui__ta_line;
void cui__ta_line_info(cui_ctx *ui, cui_font font, float size, const char *buf, int len, int ls, int le, int field_dir, float area_w, cui__ta_line *out);
void cui__ta_ctx(cui_ctx *ui, const cui__ta_line *t);
void cui__ta_ctx_end(cui_ctx *ui);
/* the offset one step to the left (dir < 0) or right (dir > 0) of off on the screen, over the offsets 0..max_off of the line;
 * -1 when there is none (lead: the caret is at the leading edge of the character at off, see cui__bidi_caret_x_at) */
int cui__bidi_step(cui_ctx *ui, cui_font font, float size, const char *text, int len, int max_off, int off, bool lead, int dir, bool *lead_out);
float cui__bidi_caret_x_at(cui_ctx *ui, cui_font font, float size, const char *text, int len, int off, bool lead);
#endif
/* Glyph metrics for layout, in points. */
float cui__font_line_height(cui_ctx *ui, cui_font font, float size);
float cui__font_ascent(cui_ctx *ui, cui_font font, float size);
float cui__text_width(cui_ctx *ui, cui_font font, float size, const char *text, int len, bool tabular);
/* Index of the byte boundary closest to x within a single line. */
int cui__text_hit(cui_ctx *ui, cui_font font, float size, const char *text, int len, float x);
/* Word wrap into lines. Returns line count; writes up to max lines. */
int cui__text_wrap(cui_ctx *ui, cui_font font, float size, const char *text, int len, float width,
                   cui_text_layout_line *lines, int max_lines);
float cui__text_min_width(cui_ctx *ui, cui_font font, float size, const char *text, int len);
/* Draw single line text with top-left at pos (points). */
void cui__draw_text_line(cui_ctx *ui, cui_font font, float size, float x, float y, cui_color color,
                         const char *text, int len, bool tabular);
/* Terminal cells with ligatures (cui_vt_look.ligatures / .font_features; needs CUI__SHAPER, else false). Set up for
 * the font and the features of this view: true when its ASCII runs must be drawn with cui__draw_cells (the font has
 * ligatures, or a feature it has was asked for); false: draw them as before. */
bool cui__cells_begin(cui_ctx *ui, cui_font font, bool ligatures, const char *features);
/* ASCII text s[0..n), one byte a cell (spaces included), cell i's pen at x + i * cw as cui__draw_text_line puts it for
 * a monospace font; y the top of the font box. A ligature is drawn in the cells of its characters and never moves one.
 * Words are shaped once and kept (1024 of them), so text seen before costs a table lookup. */
void cui__draw_cells(cui_ctx *ui, cui_font font, float size, float x, float y, float cw, cui_color color, const char *s, int n);
typedef enum { CUI__AT_LEFT, CUI__AT_CENTER, CUI__AT_RIGHT } cui__text_anchor;
/* one line of text placed by a point: x is its left edge, center or right edge, y its vertical middle */
void cui__draw_text_at(cui_ctx *ui, cui_font font, float size, float x, float y, cui__text_anchor anchor, cui_color color, const char *s, bool tab);
/* Draw text truncated with an ellipsis to max_w. */
void cui__draw_text_ellipsis(cui_ctx *ui, cui_font font, float size, float x, float y, float max_w,
                             cui_color color, const char *text, int len, bool tabular);
void cui__fonts_init(cui_ctx *ui, bool load_defaults);
void cui__fonts_shutdown(cui_ctx *ui);
void cui__fonts_begin_frame(cui_ctx *ui);
void cui__fonts_upload(cui_ctx *ui);
cui_font cui__resolve_font(cui_ctx *ui, cui_font f);
/* system fonts by script (cui_sysfont.c; cui_text.c adds the fonts and calls the hook) */
int cui__font_add(cui_ctx *ui, const void *data, size_t size, bool copy, int face, bool sys);   /* internal index or -1 */
cui_font cui__font_load_face(cui_ctx *ui, const void *data, size_t size, bool copy, int face);   /* a public id or -1 (cui_font_load_system) */
bool cui__fonts_reserve(cui_ctx *ui, int n);                 /* room for n fonts without the array moving */
void cui__fonts_changed(cui_ctx *ui);                        /* drops cached layouts, asks for a redraw */
int cui__font_glyph(cui_ctx *ui, cui_font font, uint32_t cp, cui_font *out_font);   /* glyph drawn for cp, fallbacks included; 0 none */
bool cui__glyph_ink(cui_ctx *ui, cui_font font, float size, uint32_t cp, float *x0, float *x1);  /* its ink from the pen in points, left and right */
int cui__font_own_glyph(cui_ctx *ui, cui_font f, uint32_t cp);                      /* in internal font f alone; 0 none */

/* ---- draw list ---- */
/* The software renderer (render/cui_render_sw.c) makes each of its textures again in another,
 * empty renderer, in order, so each keeps its handle there: the app layer moves to OpenGL
 * once that is ready, and the atlas, emoji and images made before stay valid. */
struct cui_sw_renderer;
void cui__sw_move_textures(struct cui_sw_renderer *r, const cui_renderer_iface *to);
/* the last frame: pixels moved by (0: none), and the share of the frame drawn and moved (0 to 1) */
void cui__sw_last_stats(const struct cui_sw_renderer *r, int *shift, float *drawn, float *moved);
/* the layer cache so far: tiles under a bitmap saved, and copied back instead of drawn; groups copied on whole
 * pixels by CUI_LAYER_SNAP instead of resampled (tests) */
void cui__sw_under_stats(const struct cui_sw_renderer *r, long *saved, long *restored, long *snapped);
/* tests: the blend of scaled bitmaps, -1 the fastest (default), 0 the first one, 1 SSE2, 2 AVX2 where there is AVX2 */
void cui__sw_test_blend(int level);
typedef struct cui_xform { float scale, ox, oy, alpha, tx, ty; } cui_xform;

/* A group of the draw list (cui_set_layer_cache, cui_draw.c): where its rest copy is in the group
 * buffers, its commands in the frame's, and how it is placed. */
typedef struct cui__group {
    cui_id id;
    int c0, nc;                 /* its commands in dl.cmd */
    int gv0, nv, gi0, ni, gc0, ngc;   /* its rest copy in dl.gvtx, dl.gidx, dl.gcmd */
    cui_xform xf;               /* scale, ox, oy (pixels), alpha */
    cui_rect clip;              /* screen pixels */
} cui__group;

typedef struct cui_draw_list {
    cui_vertex *vtx; int vtx_count, vtx_cap;
    uint32_t *idx; int idx_count, idx_cap;
    cui_draw_cmd *cmd; int cmd_count, cmd_cap;
    cui_rect clip_stack[64]; int clip_depth;
    cui_xform xform_stack[32]; int xform_depth;
    cui_texture cur_tex;
    /* What the vertices are drawn with: xform_stack, except inside a group, where it starts again
     * from the group's own origin (identity) while xform_stack keeps the screen transform for hits. */
    cui_xform dxform_stack[32];
    /* layer groups (cui_set_layer_cache): on, the group being drawn, its clips in its own space,
     * the first command a new one may merge into, and the groups and their rest copies */
    bool layer_cache, group_on;
    int cmd_floor;
    cui_rect gclip_stack[64]; int gclip_depth;
    cui__group cur;
    int g_v0, g_i0;
    cui__group *groups; int group_count, group_cap;
    cui_vertex *gvtx; int gvtx_count, gvtx_cap;
    uint32_t *gidx; int gidx_count, gidx_cap;
    cui_draw_cmd *gcmd; int gcmd_count, gcmd_cap;
    cui_draw_group *gpub; int gpub_cap;   /* what cui_draw_data.groups points at */
    unsigned layer_flags;                 /* cui_set_layer_cache_flags */
} cui_draw_list;

void cui__draw_reset(cui_ctx *ui);
void cui__push_xform(cui_ctx *ui, float scale, float ox, float oy, float alpha, float tx, float ty);
void cui__pop_xform(cui_ctx *ui);
/* The transform vertices are drawn with now (in a group: from the group's origin). */
cui_xform cui__draw_xform(cui_ctx *ui);
/* A group: what is drawn between begin and end (right after the cui__push_xform of the node that
 * animates, before its pop) is recorded at the group's origin and kept beside the frame's commands,
 * which get the transform as before. Groups do not nest: a begin inside one does nothing (false). */
bool cui__group_begin(cui_ctx *ui, cui_id id);
void cui__group_end(cui_ctx *ui);
/* cui_draw_data.groups for the frame (cui_end_frame) and the buffers freed (cui_destroy) */
void cui__groups_publish(cui_ctx *ui, cui_draw_data *dd);
void cui__groups_free(cui_ctx *ui);
void cui__draw_glyph_quad(cui_ctx *ui, float x0, float y0, float x1, float y1,
                          float u0, float v0, float u1, float v1, cui_color c);
/* Rectangles on whole device pixels as one batch, without anti-aliasing (r: x0, y0, x1, y1 each, in device pixels): see cui_draw.c */
void cui__draw_rects_px(cui_ctx *ui, const float *r, int n, cui_color col);
void cui__draw_check(cui_ctx *ui, cui_rect r, float t, float thickness, cui_color c);
void cui__draw_top_light(cui_ctx *ui, cui_rect r, cui_corners rad, float alpha);
void cui__draw_sheen(cui_ctx *ui, cui_rect r, cui_corners rad, float k);
void cui__draw_spinner(cui_ctx *ui, cui_vec2 center, float radius, float thickness, cui_color c);
void cui__draw_focus_ring(cui_ctx *ui, cui_rect r, cui_corners radius, float alpha);
void cui__draw_elevation(cui_ctx *ui, cui_rect r, cui_corners radius, cui_elevation e, float alpha);
/* Raw triangles with per vertex colors (points). */
typedef struct cui_cvert { float x, y; cui_color c; } cui_cvert;
void cui__draw_triangles(cui_ctx *ui, const cui_cvert *v, int nv, const uint32_t *idx, int ni);
/* Ordered dither (CUI_SHADE_DITHER): dots of one color on a grid of square cells, a cell on
 * where floor <= its 8x8 Bayer threshold < the density there. Density is given per vertex and
 * runs linearly between them, so a falling edge, a band or a fade is a few quads, not a dot
 * per rectangle. The renderers do the per pixel test, so a dot is never blended: it is the
 * color or nothing, and lower densities are always a subset of higher ones. */
typedef struct cui__dither {
    cui_color color;           /* the dots; theme tokens work */
    float cell;                /* side of a dot in points, 0 = 2 (rounded to whole pixels) */
    cui_vec2 origin;           /* points: the grid is anchored here, so dots move with their shape */
    cui_vec2 phase;            /* whole cells: shifts the pattern (a drift or a moving noise) */
    float floor;               /* cells with a threshold below this stay off: a dissolve from 0 to 1 */
    cui_rect shape;            /* the dots are cut to this rounded rectangle; zero size: no cut */
    cui_corners radius;        /* corners of `shape` */
} cui__dither;
typedef struct cui__dvert { float x, y, density; } cui__dvert;   /* points; density 0..1 */
void cui__draw_dither_mesh(cui_ctx *ui, const cui__dither *d, const cui__dvert *v, int nv, const uint32_t *idx, int ni);
/* a rectangle whose density runs from d0 to d1, left to right, or top to bottom when vertical */
void cui__draw_dither_rect(cui_ctx *ui, const cui__dither *d, cui_rect r, float d0, float d1, bool vertical);
/* Cells with a density each, for patterns computed per cell (a sweep, a noise, a trail): the
 * cols x rows densities, row major, of the cells from (i0, j0) on the grid anchored at
 * d->origin. Each cell is an exact square of the grid at any scale; a run of cells with the
 * same density in a row is one quad, and cells at 0 draw nothing. */
void cui__draw_dither_cells(cui_ctx *ui, const cui__dither *d, int i0, int j0, int cols, int rows, const float *density);

/* ---- software screenshots ---- */
struct cui_sw_renderer;
void cui__sw_render_screenshot(struct cui_sw_renderer *r, const cui_draw_data *dd,
                               void *rgba, int stride);

/* ---- layout tree ---- */
enum {
    CUI_NF_TEXT        = 1 << 0,
    CUI_NF_INTERACTIVE = 1 << 1,   /* records a hit rect */
    CUI_NF_SCROLL_X    = 1 << 2,
    CUI_NF_SCROLL_Y    = 1 << 3,
    CUI_NF_CLIP        = 1 << 4,
    CUI_NF_FLOATING    = 1 << 5,
    CUI_NF_BLOCKER     = 1 << 6,   /* blocks input to everything below (modal scrim) */
    CUI_NF_WRAP        = 1 << 7,
    CUI_NF_TABULAR     = 1 << 8,
    CUI_NF_FOCUSABLE   = 1 << 9,
    CUI_NF_SCROLLBAR   = 1 << 10,
    CUI_NF_HIT_OPAQUE  = 1 << 11,  /* catches clicks (e.g. popover background) */
    CUI_NF_PAINT_AFTER = 1 << 12,  /* paint callback runs after children */
    CUI_NF_ABSOLUTE    = 1 << 13,  /* positioned at (fx, fy) inside the parent, out of flow */
    CUI_NF_ON_TOP      = 1 << 14,  /* painted after its siblings */
    CUI_NF_ROW_WRAP    = 1 << 15,  /* row that breaks children onto new lines */
    CUI_NF_LINE_START  = 1 << 16,  /* computed: first child of a wrapped line */
    CUI_NF_KEEP_W      = 1 << 17,  /* never shrinks below its preferred width (its siblings give way) */
    CUI_NF_ADAPTIVE    = 1 << 18,  /* rearranges itself when narrow: min_w is its minimum, not its content */
    CUI_NF_OVERLAY     = 1 << 19,  /* absolute: painted and hit tested after the flow children, over them */
    CUI_NF_LAYER       = 1 << 20   /* a floating panel: with the layer cache on, drawn as a group while it animates */
    /* bits 20 and up are the hit-only CUI_HIT_* flags: a hit records its node's flags in the same word */
};

struct cui_node;
typedef void (*cui__paint_fn)(cui_ctx *ui, struct cui_node *n, void *data);
/* Per glyph text effect: gets the byte offset in the node's text, the glyph's
 * pen x and the line's top (points); returns the glyph color and may move it. */
typedef cui_color (*cui__text_fx_fn)(cui_ctx *ui, void *data, int byte, float x, float line_y, float *dy, cui_color c);

typedef struct cui_node {
    cui_id id;
    int parent, first, last, next;
    int abs_first, abs_last;   /* absolutely positioned children */
    int abs_anchor;            /* absolute: position relative to this node (-1 = parent) */
    int child_count;
    uint32_t flags;

    /* layout input */
    uint8_t direction;
    bool rtl;
    uint8_t dir, align, justify, self_align;   /* self_align: 0 = inherit */
    cui_size w, h;
    float min_w, max_w, min_h, max_h;
    float pad[4];             /* top, right, bottom, left */
    float gap;
    int columns;
    float col_min;            /* grid: minimum column width, 0 = fixed column count */
    float pref_hint;          /* preferred width of a fit node that may shrink (0 = from content) */

    /* text */
    const char *text;
    int text_len;
    cui_font font;
    float font_size;
    float line_height;        /* multiplier */
    uint8_t text_align;      /* text or row-cell content alignment */
    cui_color text_color;

    /* style */
    cui_color bg, border_color;
    float border;
    cui_corners radius;
    cui_elevation elevation;
    float elevation_alpha;
    float opacity;
    float scale;              /* transform around center, 1 = none */
    float tx, ty;             /* translation */

    /* custom paint */
    cui__paint_fn paint;
    cui__text_fx_fn text_fx;
    void *text_fx_data;
    void *paint_data;

    /* floating */
    int z;
    cui_id anchor;            /* 0 = viewport */
    uint8_t placement;
    float offset;             /* distance from anchor */
    float fx, fy;             /* explicit position when anchor == 0 and fpos set */
    bool fpos;
    int root_index;           /* order among floating roots */

    /* scroll */
    float scroll_x, scroll_y;

    /* computed */
    float pref_w, pref_h, minc_w, minc_h;   /* content based sizes */
    float cmin_w;                           /* minimum width of the content alone, before min_w */
    cui_rect rect;
    float content_w, content_h;
    int text_lines;
    cui_text_layout_line *lines;
    int grid_cols;            /* grid: columns used by the latest layout */
} cui_node;

/* self_align value for intrinsic size components: never stretched, otherwise
 * aligned like their siblings */
#define CUI__ALIGN_NOSTRETCH 7

/* placement values beyond cui_placement for floating roots */
#define CUI__LAYERS_SEEN 16   /* floating layers cui_floating_layers keeps */
enum { CUI__PLACE_FULL = 100, CUI__PLACE_CENTER = 101, CUI__PLACE_POINT = 102, CUI__PLACE_EXACT = 103 };   /* EXACT: at (fx, fy) in the size asked, not kept inside the window */

typedef struct cui_scroll_state {
    float x, y;                 /* current offset */
    float tx, ty;               /* target offset */
    float content_w, content_h; /* from last layout */
    float view_w, view_h;
    float bar_t;                /* scrollbar visibility 0..1 */
    double last_activity;
    bool drag_y, drag_x;
    float drag_grab;
    bool hover_bar;
    cui_color fade;             /* edge fade color, alpha 0 for none */
    float pend_y;               /* cui_scroll_set waiting for the layout: resolved against this frame's max */
    bool pend, pend_instant;
    bool stick, unstuck;        /* .stick_to_bottom, and whether the user has left the bottom */
} cui_scroll_state;

int cui__node_new(cui_ctx *ui, cui_id id);
/* Code, numbers, times and key combinations read left to right even in a right-to-left
 * interface, and so do components whose grid is drawn by hand under a header made of
 * layout boxes (a week calendar, a gantt): mirroring one without the other would put
 * events under the wrong day. The component's first node (its root) keeps LTR unless
 * the app gave it a direction itself. */
#define cui__keep_ltr(ui, first) do { int f_ = (first); \
    if (f_ < (ui)->node_count && !(ui)->nodes[f_].direction) (ui)->nodes[f_].direction = CUI_DIRECTION_LTR; } while (0)
cui_node *cui__node(cui_ctx *ui, int index);
int cui__open(cui_ctx *ui, int node);            /* push node as current parent */
void cui__close(cui_ctx *ui);
int cui__current(cui_ctx *ui);
void cui__apply_box(cui_ctx *ui, cui_node *n, const cui_box *b);
int cui__leaf(cui_ctx *ui, cui_id id, cui_size w, cui_size h);
/* Width for a component with a default size: `requested` when the caller set
 * one, otherwise `def` points while there is room, shrinking toward the
 * content minimum in a narrower container (never stretched). */
void cui__soft_width(cui_node *n, cui_size requested, float def);
int cui__text_node(cui_ctx *ui, const char *text, int len, cui_font font, float size, cui_color color, bool wrap);
/* The first text node from index from on whose text is text (len bytes, -1: up to the NUL; text
 * NULL: any text), or -1: for a view that marks up text another component built */
int cui__find_text_node(cui_ctx *ui, int from, const char *text, int len);
void cui__layout(cui_ctx *ui);
void cui__paint(cui_ctx *ui);
int cui__float_begin(cui_ctx *ui, cui_id id, int z, cui_id anchor, int placement, float offset);
void cui__float_end(cui_ctx *ui);
int cui__absolute(cui_ctx *ui, cui_id id, float x, float y, float w, float h);
cui_rect cui__xform_rect(cui_ctx *ui, cui_rect r);
void cui__paint_add_hit(cui_ctx *ui, cui_id id, cui_rect r, uint32_t flags);
/* From a paint callback: the wheel over r (inside the current clip) goes to id, ahead of any scroll area under it. */
void cui__paint_wheel_hit(cui_ctx *ui, cui_id id, cui_rect r);
/* Terminal colors and block elements, shared by the terminal (cui_terminal.c) and the VT view (cui_vt.c):
 * the 16 ANSI colors tuned for the theme, the 256 color palette, and U+2580..U+2593 drawn as rectangles (k = code point - 0x2580). */
cui_color cui__tm_palette(cui_ctx *ui, int k);
cui_color cui__tm_256(cui_ctx *ui, int n);
void cui__tm_block(cui_ctx *ui, int k, cui_rect c, cui_color col);
/* The VT screen's hand-drawn characters (cui_vt_glyphs.c): box drawing, block elements, braille, sextants and wedges, status line
 * shapes. Draws the character cp in the cell (logical pixels) on whole device pixels and returns true, or false when cp is not one of them. */
bool cui__vt_glyph(cui_ctx *ui, uint32_t cp, cui_rect cell, cui_color col);
uint8_t cui__vt_box_arms(uint32_t cp);          /* U+2500..U+257F: left, right, up, down arms, 2 bits each: 0 none, 1 light, 2 heavy, 3 double */
int cui__vt_sextant_bits(uint32_t cp);          /* U+1FB00..U+1FB3B: the six cells as bits (upper left first), 0 outside */
/* The VT screen's opt-in parts (cui_vt_sixel.c, switched on with cui_vt_set_features): answers to the queries a console
 * host forwards, and sixel pictures kept with the rows of the screen. cui_vt.c owns the grid and calls the hooks below only
 * while the parts exist (the screen's extension is set) or a picture is kept (the bits of cui__vt_pic_bits). Rows are named
 * by their slot, the index of their cells that the row map points at, so pictures move with the rows for free. */
typedef struct cui__vt_x cui__vt_x;
typedef struct { int cols, rows, cur, cx, cy, stop, sbot, sb_count, sb_max; uint64_t sb_total; int cell_w, cell_h, cell_w16; } cui__vt_info;
enum { CUI__VT_PIC_MAIN = 1, CUI__VT_PIC_ALT = 2, CUI__VT_PIC_SB = 4 };   /* where pictures are kept: the two screens, the scrollback */
typedef struct { cui_texture tex; int c0, c1; } cui__vt_pic_draw;       /* one row's picture for the view: columns c0..c1-1 */
/* cui_vt.c, for cui_vt_sixel.c */
void cui__vt_get_info(const cui_vt *v, cui__vt_info *o);
cui__vt_x *cui__vt_ext(const cui_vt *v);          /* the extension (NULL until features or colors are set) */
void cui__vt_set_ext(cui_vt *v, cui__vt_x *x);
void cui__vt_set_pic_bits(cui_vt *v, uint8_t bits);   /* CUI__VT_PIC_*, kept up to date by cui_vt_sixel.c */
int cui__vt_slot(const cui_vt *v, int scr, int y);
void cui__vt_reply(cui_vt *v, const char *s);
void cui__vt_scroll_lines(cui_vt *v, int n);      /* n line feeds at the bottom of the scroll region (rows leave into the scrollback) */
void cui__vt_set_cursor(cui_vt *v, int x, int y);
int cui__vt_mode_report(const cui_vt *v, int mode, bool dec);     /* DECRQM: 0 not recognized, 1 set, 2 reset */
void cui__vt_reflow_set(cui_vt *v, bool on);      /* CUI_VT_FEATURE_REFLOW (cui_vt_set_features passes it here; it is not an extension feature) */
void cui__vt_sb_mark_pic(cui_vt *v);              /* the newest scrollback line holds a picture: a reflow keeps it as it is */
/* cui_vt_sixel.c, for cui_vt.c */
void cui__vt_pic_sb_remap(cui_vt *v, uint64_t first, const int32_t *map, int n);   /* reflow: scrollback line first + i is now first + map[i] (-1: gone) */
void cui__vt_x_free(cui_vt *v);
size_t cui__vt_x_memory(const cui_vt *v);
void cui__vt_x_reset(cui_vt *v);                  /* RIS: sixel display mode off, a picture being read is dropped */
bool cui__vt_x_reply(cui_vt *v, char *inl, int *inl_n, size_t inl_cap, const char *s, size_t n);   /* true: queued past the inline buffer */
size_t cui__vt_x_take(cui_vt *v, char *out, size_t cap);
bool cui__vt_query_csi(cui_vt *v, char f, char priv, char inter, const int *p, int np);   /* true: answered here */
void cui__vt_query_osc(cui_vt *v, int code, const char *arg, int n, bool bel);
bool cui__vt_sixel_begin(cui_vt *v, const int *p, int np);        /* DCS P1;P2;P3 q: true when the data that follows is read */
size_t cui__vt_sixel_feed(cui_vt *v, const unsigned char *d, size_t n);   /* bytes used, stopping before ESC, CAN or SUB */
void cui__vt_sixel_end(cui_vt *v);
void cui__vt_sixel_mode(cui_vt *v, bool set);     /* DECSDM (?80) */
void cui__vt_pic_erase(cui_vt *v, int scr, int slot, int x0, int x1);
void cui__vt_pic_shift(cui_vt *v, int slot, int x, int n);        /* the current screen's row: n > 0 inserts n cells at x, n < 0 deletes */
void cui__vt_pic_pushed(cui_vt *v, int slot);     /* the main screen's row in slot went into the scrollback (or away when it keeps none) */
void cui__vt_pic_trim_sb(cui_vt *v);              /* scrollback lines were dropped or cleared */
void cui__vt_pic_drop_screen(cui_vt *v, int scr);
void cui__vt_x_resize(cui_vt *v, int scr, const uint16_t *old_map, int old_rows, int cols, int rows);
const cui__vt_pic_draw *cui__vt_pic_view(cui_ctx *ui, cui_id id, const cui_vt *v, int off, bool frozen);   /* per visible row, valid this frame; NULL: none */
uint32_t cui__vt_pic_px(const cui_vt *v, int row, int px, int py);   /* tests: a picture pixel (r | g << 8 | b << 16 | a << 24) */
/* Find in the scrollback (cui_vt_find.c, cui_vt_find and friends). Rows are counted as the view counts them: sb_total + y for
 * the live screen, older rows smaller. */
typedef struct cui__vt_find cui__vt_find;
typedef struct { int64_t r0, r1; int c0, c1; } cui__vt_span;   /* a match: from cell (r0, c0) to (r1, c1), the end not included */
typedef struct { cui_color fill, edge, cur_fill, cur_edge; } cui__vt_find_look;
#define CUI__VT_FIND_SPACER 0xFFFFFFFFu                       /* cui__vt_row_chars: the second cell of a wide character */
/* cui_regex.c: a pattern compiled once and searched many times; find: 1 (start, end in bytes), 0 none, -2 too complex */
typedef struct cui__regex cui__regex;
cui__regex *cui__regex_new(const char *pattern, int flags, const char **error);   /* NULL with *error for an invalid pattern */
void cui__regex_free(cui__regex *r);
int cui__regex_find(const cui__regex *r, const char *text, int len, int from, int *start, int *end, const char **error);
/* cui_vt.c, for cui_vt_find.c */
cui__vt_find **cui__vt_find_slot(cui_vt *v);
const cui__vt_find *cui__vt_find_get(const cui_vt *v);
uint32_t cui__vt_wseq(const cui_vt *v);                         /* bumps at every cui_vt_write and cui_vt_resize */
int cui__vt_row_chars(const cui_vt *v, int64_t abs, uint32_t *out, int cap, bool *wrapped);   /* a row's code points; -1: no such row */
/* cui_vt_find.c, for cui_vt.c */
void cui__vt_find_free(cui_vt *v);
size_t cui__vt_find_memory(const cui_vt *v);
/* a view with .find, once a frame: finds stale matches again (at most every 100 ms), and returns true with the current
 * match in *cur when it moved since *seen (which it updates) */
bool cui__vt_find_frame(cui_ctx *ui, cui_vt *v, uint32_t *seen, cui__vt_span *cur);
void cui__vt_find_look_of(cui_ctx *ui, cui__vt_find_look *o);
void cui__vt_find_paint_row(cui_ctx *ui, const cui_vt *v, int64_t arow, float x0, float ry, float cw, float lh, const cui__vt_find_look *k);
int cui__vt_pic_count(const cui_vt *v);                             /* tests: slices kept */
/* The VT screen's links (cui_vt_links.c): OSC 8 hyperlinks kept per cell (CUI_VT_FEATURE_LINKS), links found in the text
 * under the mouse, the smart double and triple click. cui_vt.c calls the cui__vt_lk_* hooks only while the screen has a
 * link table (cui__vt_lk is set, after the first OSC 8 with the feature on); a screen without one tests one pointer. */
typedef struct cui__vt_lx cui__vt_lx;
typedef struct { bool on; int64_t r0, r1; int c0, c1; } cui__vt_lpaint;   /* the link under the mouse with Ctrl: cells r0:c0 to r1:c1, rows as sb_total + y */
/* cui_vt.c, for cui_vt_links.c (which also reads rows with cui__vt_row_chars and the write count with cui__vt_wseq, above) */
cui__vt_lx *cui__vt_lk(const cui_vt *v);
void cui__vt_set_lk(cui_vt *v, cui__vt_lx *lx);
void cui__vt_set_lk_on(cui_vt *v, bool on);
/* cui_vt_links.c, for cui_vt.c and cui_vt_sixel.c */
void cui__vt_lk_feature(cui_vt *v, bool on);                          /* CUI_VT_FEATURE_LINKS on or off (off frees the table) */
void cui__vt_lk_free(cui_vt *v);
size_t cui__vt_lk_memory(const cui_vt *v);
void cui__vt_lk_osc(cui_vt *v, const char *arg, int n, bool cut);     /* OSC 8: params ; URI (cut: the URI did not fit the buffer) */
void cui__vt_lk_put(cui_vt *v, int scr, int slot, int x, int w);      /* a character of w cells was written at x of the row in slot */
void cui__vt_lk_erase(cui_vt *v, int scr, int slot, int x0, int x1);
void cui__vt_lk_shift(cui_vt *v, int scr, int slot, int x, int n);    /* n > 0 inserts n blank cells at x, n < 0 deletes */
void cui__vt_lk_pushed(cui_vt *v, int slot);                          /* the main screen's row in slot went into the scrollback */
void cui__vt_lk_reset(cui_vt *v);                                     /* RIS: no link open */
/* The VT screen's command marks, progress and copy requests (cui_vt_marks.c, CUI_VT_FEATURE_MARKS). Rows as the view counts
 * them (sb_total + y of the main screen). cui_vt.c calls the cui__vt_mk_* hooks only while the feature is on (cui__vt_mk_get is
 * set), and the scroll hook only when rows move other than by a line feed at the bottom of the whole screen. */
typedef struct cui__vt_mk cui__vt_mk;
typedef struct { int64_t r; int c; int64_t orow; int ocol; int mi, k; bool hit; } cui__vt_rfpt;   /* a cell a reflow follows: source row and column, where it went */
typedef struct { cui__vt_rfpt *sb, *base, *cut; int nsb, nbase, ncut; int64_t sb_first; } cui__vt_mk_plan;
/* cui_vt.c, for cui_vt_marks.c */
cui__vt_mk *cui__vt_mk_get(const cui_vt *v);
void cui__vt_set_mk(cui_vt *v, cui__vt_mk *mk);
int cui__vt_main_chars(const cui_vt *v, int64_t abs, uint32_t *out, int cap, bool *wrapped);   /* as cui__vt_row_chars, of the main screen always */
/* Saving a screen (cui_vt_save.c: cui_vt_save and cui_vt_load). Rows as above (sb_total + y of the main screen). */
typedef struct { uint32_t cp; int attrs; int32_t fg, bg; bool spacer; } cui__vt_scell;   /* attrs, fg, bg as cui_vt_cell has them; spacer: a wide character's second cell */
typedef struct { int64_t row[4]; int col[4]; int exit_code; bool finished; double dur_ms; const char *cmd; } cui__vt_mkx;   /* a mark's A B C D (row INT64_MIN: not come); dur_ms < 0 unknown */
/* cui_vt.c, for cui_vt_save.c */
int cui__vt_save_row(const cui_vt *v, int64_t abs, cui__vt_scell *out, int cap, int *wrapped);   /* cells (at most cap), wrapped 0, 1 or 2 (after a padding cell); -1 no such row */
void cui__vt_save_cursor(const cui_vt *v, int *cx, int *cy);         /* the main screen's cursor */
/* cui_vt_links.c, for cui_vt_save.c */
const uint16_t *cui__vt_lk_main_ids(const cui_vt *v, int64_t abs, int *n);   /* the link ids of a main screen or scrollback row; NULL none */
const char *cui__vt_lk_uri_of(const cui_vt *v, uint16_t id);
/* cui_vt_marks.c, for cui_vt_save.c */
int cui__vt_mk_export(const cui_vt *v, cui__vt_mkx *out, int cap);  /* oldest first; returns how many there are (all of them, even past cap) */
void cui__vt_mk_set_durations(cui_vt *v, int first, const double *dur, int n);   /* marks first.. end now and began dur ms before (dur < 0: left as they are) */
/* cui_vt_marks.c, for cui_vt.c and cui_vt_sixel.c */
void cui__vt_mk_feature(cui_vt *v, bool on);
void cui__vt_mk_free(cui_vt *v);
size_t cui__vt_mk_memory(const cui_vt *v);
void cui__vt_mk_osc(cui_vt *v, int code, const char *arg, int n);    /* OSC 133, 633 and 9 */
void cui__vt_mk_c52_begin(cui_vt *v);                                 /* after "52;" */
size_t cui__vt_mk_c52_feed(cui_vt *v, const unsigned char *d, size_t n);   /* bytes used, stopping before BEL or ESC */
void cui__vt_mk_c52_end(cui_vt *v);
void cui__vt_mk_scroll(cui_vt *v, int top, int bot, int n, int how); /* rows top..bot move: 1 up into the scrollback, 0 up, -1 down */
void cui__vt_mk_trim(cui_vt *v);                                      /* scrollback lines went */
void cui__vt_mk_reset(cui_vt *v);                                     /* RIS */
void cui__vt_mk_cut(cui_vt *v, int64_t t0, int drop);                 /* a resize cut the screen; drop rows went up */
void cui__vt_mk_rf_begin(cui_vt *v);
void cui__vt_mk_rf_capture(cui_vt *v);                                /* a reflow's base was taken from the main screen */
bool cui__vt_mk_rf_plan(cui_vt *v, bool use_base, int64_t t0, cui__vt_mk_plan *pl);
void cui__vt_mk_rf_end(cui_vt *v, cui__vt_mk_plan *pl, bool sb_ok, bool main_done, int p, int drop);
bool cui__vt_mk_view(const cui_vt *v, unsigned *seen, int64_t *prompt, bool *sel, cui__vt_span *out);
void cui__vt_mk_paint(cui_ctx *ui, const cui_vt *v, cui_rect track);
int cui__vt_mk_hit(const cui_vt *v, cui_rect track, float mx, float my);
/* the view (cui_vt.c's view body and paint) */
const cui__vt_lpaint *cui__vt_links_hover(cui_ctx *ui, cui_id id, const cui_vt *v, bool hovered, int64_t row, int col, bool ctrl);
const cui_vt_link *cui__vt_links_take(cui_ctx *ui, cui_id id);        /* the link under the mouse, copied to frame memory */
void cui__vt_smart_word(const cui_vt *v, int64_t row, int col, int64_t *r0, int *c0, int64_t *r1, int *c1);
void cui__vt_smart_line(const cui_vt *v, int64_t row, int64_t *r0, int64_t *r1);
void cui__vt_trim_copy(char *text);                                   /* spaces before each line break and at the end go */
void cui__vt_links_paint(cui_ctx *ui, const cui_vt *v, const cui__vt_lpaint *lp, int64_t top, int rows, float x0, float y0, float cw, float lh, cui_color col);
int cui__vt_links_detections(void);                                   /* tests: how many times the view looked for a link (all screens) */
/* The VT screen's characters by Unicode 17 and its grapheme clusters (cui_vt_width.c, CUI_VT_FEATURE_CLUSTERS). A cell's code
 * point from CUI__VT_CLUSTER0 on names a cluster in the table. Properties of a character (cui__vtw_props): */
#define CUI__VT_CLUSTER0 0x110000u
enum { CUI__VTW_OTHER, CUI__VTW_CR, CUI__VTW_LF, CUI__VTW_CONTROL, CUI__VTW_EXTEND, CUI__VTW_ZWJ, CUI__VTW_RI, CUI__VTW_PREPEND,
       CUI__VTW_SPACING, CUI__VTW_L, CUI__VTW_V, CUI__VTW_T, CUI__VTW_LV, CUI__VTW_LVT };   /* Grapheme_Cluster_Break */
#define CUI__VTW_GCB(p) (((p) >> 2) & 15u)
#define CUI__VTW_PICT 0x40u            /* Extended_Pictographic */
#define CUI__VTW_VS16 0x80u            /* has an emoji style (FE0F makes it 2 cells) */
#define CUI__VTW_PRES 0x100u           /* Emoji_Presentation (FE0E makes it 1 cell) */
#define CUI__VTW_INCB(p) (((p) >> 9) & 3u)   /* Indic_Conjunct_Break: 0 none, 1 linker, 2 consonant, 3 extend */
typedef struct { uint16_t last; uint8_t ri, pict, incb; } cui__vtw_gs;   /* a cluster being read: its last character and the rules' state */
unsigned cui__vtw_props(uint32_t c);
typedef struct { uint32_t lo, n; uint16_t p; } cui__vtw_run;          /* the run the last lookup fell in (n 0: none yet) */
unsigned cui__vtw_props_in(cui__vtw_run *r, uint32_t c);              /* cui__vtw_props with that remembered */
int cui__vtw_cells(unsigned props);                                   /* alone: 1 or 2, 0 for a character with nothing to show alone */
void cui__vtw_begin(cui__vtw_gs *s, unsigned props);                  /* a cluster starts with this character */
bool cui__vtw_join(cui__vtw_gs *s, unsigned props);                   /* the next character joins the cluster (else one starts with it) */
int cui__vtw_cluster_cells(const uint32_t *cps, int n);
bool cui__vtw_cluster_emoji(const uint32_t *cps, int n, int cells);
uint32_t cui__vtw_compose(uint32_t a, uint32_t b);                    /* canonical composite of a BMP pair, 0 none */
typedef struct cui__vt_gcl cui__vt_gcl;
uint32_t cui__vt_gcl_add(cui__vt_gcl **g, const uint32_t *cps, int n, unsigned flags, bool *full);   /* 0: not kept (*full: collect, try again) */
void cui__vt_gcl_undo(cui__vt_gcl *g, uint32_t cp);
const uint32_t *cui__vt_gcl_get(const cui__vt_gcl *g, uint32_t cp, int *n, unsigned *flags);         /* NULL (n 0) for a cluster not there */
void cui__vt_gcl_mark(cui__vt_gcl *g, uint32_t cp);                   /* a collection: a cell holds cp ... */
void cui__vt_gcl_sweep(cui__vt_gcl *g);                               /* ... and the rest goes */
void cui__vt_gcl_free(cui__vt_gcl *g);
size_t cui__vt_gcl_memory(const cui__vt_gcl *g);
uint32_t cui__vt_gcl_count(const cui__vt_gcl *g);
void cui__vt_clusters_set(cui_vt *v, bool on);                        /* cui_vt.c, for cui_vt_set_features */
int cui__vt_cell_cps(const cui_vt *v, uint32_t cp, uint32_t *out, int cap);   /* cui_vt.c, for cui_vt_save.c: a cell's code points (a cluster's all) */
/* The screen pointer in the layout space of wheel area `id`, using its last paint transform. */
cui_vec2 cui__wheel_point(cui_ctx *ui, cui_id id);

/* ---- per id state ---- */
typedef struct cui_state_entry {
    cui_id id;
    uint64_t last_frame;
    cui_rect rect;             /* rect from the previous frame */
    bool has_rect;
    size_t size;
    void *data;
    void (*dtor)(cui_ctx *ui, void *data);   /* frees memory owned by data */
    int next;
} cui_state_entry;

cui_state_entry *cui__state_entry(cui_ctx *ui, cui_id id);
float cui__hover_fade(cui_ctx *ui, cui_id id, bool on, float duration);   /* a 0..1 fade that keeps state only while above zero */
void *cui__state_lazy(cui_ctx *ui, cui_id id, size_t size, bool want);   /* state only once wanted: NULL while untouched */
void cui__state_set_dtor(cui_ctx *ui, cui_id id, void (*dtor)(cui_ctx *ui, void *data));
cui_rect cui__prev_rect(cui_ctx *ui, cui_id id);
bool cui__has_prev_rect(cui_ctx *ui, cui_id id);

/* ---- interaction ---- */
typedef struct cui_hit {
    cui_id id;
    cui_rect rect;
    uint32_t flags;
    cui_id layer;
    int order;
} cui_hit;

/* An inactive scope stays inactive through nested pushes. Outside any scope, both fields are zero. */
typedef struct { cui_id id; bool inactive; } cui__focus_scope;
typedef struct { cui_id id, layer; cui__focus_scope scope; bool component_input; } cui__focus_entry;   /* component_input: a canvas that routes its own keys */

/* Hit-only flags for controls whose wheel range is separate from layout scroll.
 * They share cui_hit.flags with the CUI_NF_* node flags, so they sit above all of them. */
enum { CUI_HIT_WHEEL_UP = 1 << 20, CUI_HIT_WHEEL_DOWN = 1 << 21 };
typedef char cui__hit_flags_above_node_flags[(unsigned)CUI_HIT_WHEEL_UP > (unsigned)CUI_NF_OVERLAY ? 1 : -1];

/* A vertical delta reserved for one hit while the normal route handles X. */
enum { CUI__VERTICAL_WHEEL_STATE = 0x57484559u };
typedef struct { uint64_t frame; float delta; } cui__vertical_wheel;
float cui__take_vertical_wheel(cui_ctx *ui, cui_id id);

typedef struct cui_interact {
    bool hovered, pressed, clicked, right_clicked, double_clicked, focused, active, released, keyboard_activate;
    bool focus_visible;
    bool keyboard_focused;    /* focused with no popup above this layer */
} cui_interact;

enum { CUI_IF_FOCUSABLE = 1, CUI_IF_DRAG = 2, CUI_IF_KEEP_FOCUS = 4, CUI_IF_TEXT = 8, CUI_IF_COMPONENT_INPUT = 16 };
typedef struct cui_modal_layer {
    cui_id id;
    int z;
} cui_modal_layer;
cui_interact cui__interact(cui_ctx *ui, cui_id id, int flags);
void cui__mark_interactive(cui_ctx *ui, int node, int flags);
void cui__set_cursor(cui_ctx *ui, cui_cursor c);
void cui__wakeup_after(cui_ctx *ui, float seconds);
/* The text caret, phase seconds after it last moved: whether it still blinks (cui_set_caret_blink), and the wakeup for its next
 * step while it does (none once it stays on). */
bool cui__caret_blinks(cui_ctx *ui, double phase);
void cui__caret_wakeup(cui_ctx *ui);
bool cui__is_layer_open(cui_ctx *ui, cui_id layer);

/* ---- popups ---- */
typedef struct cui_popup {
    cui_id id;
    cui_id parent_layer;       /* layer that submitted the popup */
    cui_id anchor;
    cui_rect anchor_rect;
    bool open;
    bool at_mouse;
    bool no_anchor_w;          /* opened at a point from code: does not copy the last component's width */
    bool point_anchor;         /* anchored to the point in `mouse` with the placement (cui_popover_at) */
    cui_vec2 mouse;
    uint64_t opened_frame;
    int z;                     /* stacking order: later opens are on top */
    uint64_t last_frame;
    float t;                   /* open animation 0..1 */
    cui_spring s;
} cui_popup;

typedef struct cui_toast_item {
    double last_tick;
    int handle;
    char title[128];
    char desc[256];
    char action[48];
    cui_tone tone;
    float duration, age;
    bool dismissed, action_clicked, hovered_pause, show_timer;
    cui_spring enter;          /* 0..1 appear */
    cui_spring y;              /* stacked position */
    cui_spring sx;             /* swipe offset; right dismisses */
    float grab_x, vel;         /* drag anchor and smoothed speed (points per second) */
    bool swiping;
    float height;
} cui_toast_item;

/* Scope stacks used while building one frame (kept per context). */
typedef struct cui__popup_build { cui_id id; bool disabled_pushed; bool menu; int item_index; } cui__popup_build;
typedef struct cui__modal_build { cui_id id; bool disabled_pushed; } cui__modal_build;
typedef struct cui__vlist_build { int count; float row_h; int first; } cui__vlist_build;

/* The standard edit menu (cui_edit_items, cui_edit_command, cui_input_use_edit_menu; cui_textedit.c). Each text widget
 * (text field, code editor, markdown view) says every frame what it can do (cui__edit_seen); the menu's items and
 * commands act on the target: the one right-clicked, else the one that had the keyboard when the menu opened. */
enum { CUI__EM_RO = 1, CUI__EM_SEL = 2, CUI__EM_UNDO = 4, CUI__EM_REDO = 8, CUI__EM_SECRET = 16,
       CUI__EM_VIEW = 32 /* not a keyboard focus (the markdown view): a command does not move the focus to it */ };
typedef struct cui__edit_target { cui_id id; int flags; uint64_t seen; } cui__edit_target;
typedef struct cui__edit_menu {
    cui__edit_target cur, last;   /* the text widget with the keyboard this frame, and last frame (id 0: none) */
    cui__edit_target menu;        /* the one the open menu belongs to */
    uint64_t menu_frame;          /* the frame a right press set `menu` (a menu opening that frame keeps it) */
    int cmd;                      /* a CUI_EDIT_* command waiting for widget cmd_id's next frame, 0 none */
    cui_id cmd_id;
    uint64_t cmd_frame;
    int edk;                      /* during the target code editor's keys: its command as CUI_EDK_* + 1, else 0 */
    bool fields;                  /* cui_input_use_edit_menu: text fields open the menu on a right click */
    bool keep_focus;              /* the items being built leave the keyboard where it is (cui_edit_items) */
} cui__edit_menu;

#define CUI_MAX_TOASTS 16
#define CUI_ID_STACK 64
#define CUI_PARENT_STACK 256

struct cui__emoji;
struct cui__sysfont;
#ifdef CUI_TEXT_SHAPING
typedef struct cui__ell_memo { uint64_t key; int cut; float cut_w; } cui__ell_memo;
#endif
struct cui_ctx {
    struct cui__emoji *emoji;   /* color emoji: font, cache and data (cui_emoji.c) */
    /* set by cui_emoji.c when emoji are first used, so the core does not link it in:
     * 0 a frame begins, 1 the renderer changes, 2 the context goes */
    void (*emoji_hook)(cui_ctx *ui, int what);
    /* set with it, so text draws emoji in place: at s, the bytes of an emoji the emoji font
     * draws (0 for none); with r, draws that emoji in r (cui_text.c measures and calls it) */
    int (*emoji_text)(cui_ctx *ui, const char *s, int len, const cui_rect *r, float alpha);
    /* set by cui_font_use_system_fallback (cui_sysfont.c): for a character no font of the app has, what 0
     * looks it up in the system fonts of its script (the internal font index, glyph in *gi; -1 for none)
     * and what 2 lets the fonts go when the context does */
    struct cui__sysfont *sysfont;
    int (*sysfont_hook)(cui_ctx *ui, int what, cui_font font, uint32_t cp, int *gi);
    cui_allocator alloc;
    cui_renderer_iface renderer;
    cui_platform_iface platform;

    /* viewport */
    float vw, vh, scale;
    cui_direction direction;

    /* time */
    double time;
    float dt;
    float step;                /* the step of transitions: dt, or at most one 60 Hz frame after a rest */
    bool wanted_frame;         /* the last frame asked for the next one: the time since is no rest */
    uint64_t frame;
    bool in_frame;
    bool animating;
    int settle_frames;
    float wakeup;              /* seconds, <0 none */

    /* frame arena */
    char *arena; size_t arena_used, arena_cap;
    /* the most each per-frame buffer needed lately: much bigger ones shrink (cui__trim_buffers) */
    int peak_vtx, peak_idx, peak_cmd, peak_nodes; size_t peak_arena; uint64_t peak_since;
    struct cui_arena_block { struct cui_arena_block *next; } *arena_overflow;

    /* nodes */
    cui_node *nodes; int node_count, node_cap;
    int parents[CUI_PARENT_STACK]; int parent_depth;
    int *float_roots; int float_count, float_cap;
    struct { cui_layer_rect r[CUI__LAYERS_SEEN]; int n; } layers_seen;   /* cui_floating_layers: the latest layout's floating roots */
    int last_node;
    cui_id last_id;

    /* ids */
    cui_id id_stack[CUI_ID_STACK]; int id_depth;
    int auto_counter[CUI_ID_STACK];
    int disabled_depth;
    bool disabled_stack[32]; int disabled_n;

    /* state store (open addressing over chained entries) */
    cui_state_entry *states; int state_count, state_cap;
    int *state_buckets; int bucket_count;
    int state_free;

    /* hit testing */
    cui_hit *hits; int hit_count, hit_cap;           /* current frame */
    cui_hit *prev_hits; int prev_hit_count, prev_hit_cap;
    cui_id hot, hot_layer;          /* topmost interactive item under the mouse */
    cui_id scroll_target;           /* topmost scroll area under the mouse */
    cui_id active;                  /* item being pressed/dragged */
    cui_id focus;                   /* keyboard focus */
    bool focus_visible;             /* focus came from the keyboard */
    cui_id focus_next_request;
    int focus_move;                 /* +1/-1 tab navigation requested */
    cui__focus_entry *focus_order; int focus_count, focus_cap;
    cui__focus_entry *prev_focus_order; int prev_focus_count, prev_focus_cap;
    cui__focus_scope *focus_scope_stack; int focus_scope_depth, focus_scope_cap;
    cui_id active_focus_scope, prev_active_focus_scope;
    cui_modal_layer modal, prev_modal;                  /* topmost open modal */
    bool edit_prev;                                     /* a text field had focus last frame */
    cui_id hover_id_for_tooltip;
    double hover_start;
    cui_id layer_stack[32]; int layer_depth;   /* current floating layer while building */
    cui_id current_layer;
    cui_cursor cursor;

    /* input */
    struct {
        cui_input_hook_fn capture;
        void *user;
        cui_id focus_request, focus_owner;
        cui_id previous_tab_owner;     /* built-in editor's Tab owner during replay */
        bool repeated;
        bool blocked_keys[CUI_KEY_COUNT];
    } component_input;
    float mx, my, pmx, pmy;
    bool mouse_inside;
    bool mdown[3], mpressed[3], mreleased[3];
    float press_x, press_y, release_x, release_y;   /* where the left button was last pressed and released */
    int click_count;
    double last_click_time;
    float last_click_x, last_click_y;
    struct {
        int count;
        bool restarted;
        float first_x, first_y, last_x, last_y;
    } pending_clicks;               /* input collected before the frame clock advances */
    float scroll_dx, scroll_dy;
    bool kdown[CUI_KEY_COUNT];
    uint8_t kpressed[CUI_KEY_COUNT];
    int mods;
    char text_in[256]; int text_in_len;
    bool window_focused;
    bool input_this_frame;
    unsigned typed_seq;             /* bumps on every key press and text input (cui_app_unpace_answers) */

    /* theme */
    cui_theme theme;
    cui_theme target_theme;
    cui_theme_mode mode;
    cui_accent accent;
    cui_dark_style dark_style;
    bool reduced_motion;
    float theme_t;              /* theme transition progress */
    cui_theme from_theme;
    const cui_strings *strings; /* the app's text for another language, or NULL (cui_strings.c) */

    /* fonts */
#ifdef CUI_TEXT_SHAPING
    struct cui_shaped_paragraph *shape_cache;
    int shape_cache_count;
    cui__ell_memo ell_memo[16];     /* the last cuts of cui__draw_text_ellipsis for shaped text */
    int shape_cache_cap;            /* paragraphs kept: 64, more while one frame uses more (cui__paragraph) */
    const char *text_paragraph;
    int text_paragraph_len;
    cui_direction text_direction;
    bool input_bidi;                /* cui_input_use_bidi: text fields draw right-to-left text in reading order */
    bool input_paras;               /* cui_input_use_paragraphs: text areas order and align each paragraph by its own direction */
    const char *bidi_para;          /* set around the cui__bidi_* calls for one text area line (cui__ta_ctx) */
    int bidi_para_len, bidi_para_dir;
#endif
#ifdef CUI__SHAPER
    struct cui__cells_lig *cells_lig;   /* cui_vt's ligatures: the font's plan and the shaped words (cui__cells_begin) */
#endif
    bool text_unshaped;             /* editing keeps logical glyph positions */
    cui_font_data *fonts; int font_count, font_cap;
    cui_font roles[5];
    cui_glyph *glyphs; int glyph_count, glyph_cap;
    int glyph_buckets[4096];
    uint64_t atlas_check_frame;   /* when the atlas last looked at shrinking */
    uint8_t *atlas; int atlas_w, atlas_h;
    int pack_x, pack_y, pack_row_h;
    bool atlas_dirty; int dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    bool atlas_reset_pending;
    cui_texture atlas_tex;
    int atlas_tex_w, atlas_tex_h;

    /* drawing */
    cui_draw_list dl;
    cui_draw_data draw_data;

    /* widgets */
    cui_popup popups[32]; int popup_count;
    cui_id open_menu_stack[8]; int open_menu_depth;
    cui_toast_item toasts[CUI_MAX_TOASTS]; int toast_count;
    int toast_next_handle;
    cui_id dialog_stack[8]; int dialog_depth;
    cui_id nav_group_last[8];
    int tooltip_node;
    const char *tooltip_text;
    cui_id tooltip_id;
    cui_rect tooltip_rect;
    cui_id tree_last_clicked;
    bool tree_clicked;
    /* text editing */
    struct cui_edit_state *edit;   /* edit state of the focused text field */
    cui_id tab_owner;              /* the focused control types Tab instead of moving focus (code editor); set every frame */
    double caret_time;
    float caret_blink_s;           /* cui_set_caret_blink: 0 blinks while focused, > 0 that many seconds, < 0 never */
    /* scope stacks */
    cui__popup_build pb_stack[16]; int pb_depth;
    cui__modal_build mb_stack[8]; int mb_depth;
    cui__vlist_build vl_stack[8]; int vl_depth;
    bool acc_dis[32]; int acc_depth;
    int tree_depth;
    int button_group_depth;
    int layer_seq;             /* increments whenever a popup or modal opens */
    /* one shot font override for the next text field (inline edit) */
    bool tf_override;
    cui_font tf_font;
    float tf_size;
    cui_id tf_select_all;      /* this field selects all of its text when it next has focus */
    cui__edit_menu em;         /* the standard edit menu's target and pending command (cui_textedit.c) */
    /* files dropped from the OS, valid for one frame */
    char *drop_buf; size_t drop_used, drop_cap;
    int drop_off[64]; int drop_count;
    float drop_x, drop_y;
    bool drop_claimed;
    bool drag_hover; float drag_x, drag_y;
    bool slider_no_header;     /* next range slider skips its label row */
    /* Containers being built. A component's _begin sets its pointer (frame memory), the
     * items between begin and end read it, and _end clears it. They do not nest unless
     * noted, so a new begin/end component adds its pointer here and nowhere else. */
    struct {
        struct cui_table_build *table; int table_depth;   /* tables nest: depth counts them */
        const cui_vec2 *screen_pointer; /* outermost saved pointer while node content remaps mouse input */
        struct cui_split_build *split;
        struct cui_reorder_build *reorder;
        struct cui_swipe_build *swipe;
        struct cui_grid_build *grid;
        void *carousel;
        void *status_bar;          /* its width budget */
        void *card_stack;
        void *kanban;
        void *tree_multi;          /* the tree with several selected rows being built (cui_treesel.c) */
        void *dock;
        void *spot;                /* spotlight card */
        void *reasoning;
        void *veil;                /* veils nest: each keeps the one around it */
        struct cui__skel_group *skeleton;   /* skeleton groups nest: each keeps the one around it */
    } building;
    cui__text_fx_fn text_fx;       /* set while a node with a text effect paints */
    void *text_fx_data;
    int text_fx_base;              /* byte offset of the line being drawn */
    float text_fx_line_y;
};

/* ---- theme ---- */
void cui__theme_init(cui_ctx *ui, cui_theme_mode mode, cui_accent accent, cui_dark_style style);
void cui__theme_update(cui_ctx *ui);
void cui__theme_build(cui_theme *t, cui_theme_mode mode, cui_accent accent, cui_dark_style style);
/* a press scale (CUI__PRESS or CUI__PRESS_WIDE while pressed, 1 at rest): the springy default, or theme.press_time */
float cui__press(cui_ctx *ui, cui_spring *s, bool pressed, float scale);
cui_color cui__tone_color(cui_ctx *ui, cui_tone tone);
#define CUI_C(ui, tok) ((ui)->theme.colors[CUI_TOKEN_##tok])
/* a font size in points, scaled by the theme's text size setting */
static inline float cui__fs(cui_ctx *ui, float size) { return size * ui->theme.font_scale; }
/* the color that marks a selected or active thing: the accent, or the text color under the
 * neutral accent (a neutral theme has no accent hue to spend) */
static inline cui_color cui__emphasis(cui_ctx *ui) {
    return ui->accent == CUI_ACCENT_NEUTRAL ? CUI_C(ui, FOREGROUND) : CUI_C(ui, ACCENT);
}
/* selected text behind the glyphs: the accent, or a blue under the neutral accent (a gray
 * would read as disabled) */
static inline cui_color cui__selection_color(cui_ctx *ui) {
    return ui->accent == CUI_ACCENT_NEUTRAL ? cui_alpha(cui_oklch(0.6f, 0.13f, 250, 1), 0.28f) : cui_alpha(CUI_C(ui, ACCENT), 0.26f);
}

/* ---- user-facing text (cui_strings.c): the app's table (cui_set_strings) or English ---- */
const char *cui__month_name(cui_ctx *ui, int m);      /* m 0..11, January first */
const char *cui__month_short(cui_ctx *ui, int m);     /* "Jan" */
const char *cui__weekday_name(cui_ctx *ui, int d);    /* d 0..6, Monday first */
const char *cui__weekday_short(cui_ctx *ui, int d);   /* "Mon" */
const char *cui__weekday_min(cui_ctx *ui, int d);     /* "Mo" */
const char *cui__today_text(cui_ctx *ui);
const char *cui__size_text(cui_ctx *ui, double bytes);   /* "4.2 KB" */
const char *cui__mmss(cui_ctx *ui, float seconds);        /* a play time, "3:07"; negative reads as 0 */
int cui__utc_offset(int64_t t);                           /* minutes east of UTC of the local zone at t */

/* ---- shared widget pieces ---- */
/* turns on color emoji in all text (cui_emoji.c); components with free text, like markdown, call it */
void cui__emoji_text_on(cui_ctx *ui);
void cui__widgets_begin_frame(cui_ctx *ui);
void cui__widgets_end_frame(cui_ctx *ui);   /* toasts, tooltips */
void cui__tooltip_passive(cui_ctx *ui, cui_id anchor, const char *text);   /* hover by rect, for things that are not interactive */
void cui__spinner_node(cui_ctx *ui, float size, cui_color c);   /* the arc spinner (cui_widgets.c), also in buttons */
void cui__overlay_end_frame(cui_ctx *ui);
float cui__control_height(cui_ctx *ui, cui_control_size s);
cui_popup *cui__popup_get(cui_ctx *ui, cui_id id, bool create);
bool cui__popup_begin(cui_ctx *ui, cui_id id, cui_id anchor, int placement, float width, float pad, float gap, bool menu);
void cui__popup_end(cui_ctx *ui);
void cui__popup_set(cui_ctx *ui, cui_id id, bool open);
void cui__close_all_popups(cui_ctx *ui);
bool cui__keys_covered(cui_ctx *ui);   /* an open popup sits above the layer being built, so the keys are its (cui_chrome.c) */
void cui__color_picker_open(cui_ctx *ui, cui_id id, cui_color value);   /* opens cui_color_picker's popover from code */
/* a dashed rounded rectangle, the dashes continuing round the corners (cui_pickers.c: the dropzone, gantt plans) */
void cui__draw_dashed_rrect(cui_ctx *ui, cui_rect r, float rad, cui_color c, float th, float dash, float gap);
typedef struct { int highlight; int item_count; int prev_item_count; bool keyboard; cui_id open_sub; } cui__menu_nav;
typedef struct { cui_color bg, bg_hover, bg_press, fg, border, border_hover; } cui__btn_colors;
cui__btn_colors cui__button_colors(cui_ctx *ui, cui_button_variant v, bool selected);
bool cui__icon_button_id(cui_ctx *ui, cui_id id, const char *icon, const cui_button_opts *o);
/* Edge for controls their label already identifies (outline buttons, chips,
 * groups). Fields, checkboxes and radios keep BORDER_STRONG (3:1): their edge
 * is the only thing that marks them. */
cui_color cui__quiet_edge(cui_ctx *ui, float hover);
cui_color cui__control_edge(cui_ctx *ui, float hover);   /* the 3:1 edge of fields, checks, radios, dropzones */
cui_color cui__edge_on(cui_ctx *ui);                     /* the edge of a focused or open field */
void cui__focus_ring(cui_ctx *ui, int node, float alpha);   /* keyboard focus ring painted after the node */
void cui__icon_swap(cui_ctx *ui, cui_id state_id, const char *icon, float size, cui_color c);
cui_id cui__position_id(cui_ctx *ui, uint32_t salt);       /* stable id from the parent and child index */
void cui__chevron_node(cui_ctx *ui, float angle, float size, cui_color color);   /* 0 points down */
int cui__initials(const char *name, char *out);   /* up to two initials of a name, for avatars; out holds 16 bytes */
/* Room for a custom item of width w in the toolbar being built (cui_chrome.c); false when it does not
 * fit and the caller should leave it out. Without a toolbar there is always room. */
bool cui__toolbar_reserve(cui_ctx *ui, float w);
int cui__text_field(cui_ctx *ui, cui_id id, char *buf, size_t cap, const cui_input_opts *o, bool multiline, int style, const char *trailing_icon_default);
int cui__edit_lines(cui_ctx *ui, cui_font f, float size, const char *buf, int len, float width, cui_text_layout_line **out);   /* wrapped lines of an edit buffer */
void cui__label_text(cui_ctx *ui, const char *label, cui_font font, float size, cui_color color);
void cui__icon_node(cui_ctx *ui, const char *icon, float size, cui_color color);
void cui__field_label(cui_ctx *ui, const char *label);
void cui__field_message(cui_ctx *ui, const char *hint, const char *error);
int cui__scroll_begin_ex(cui_ctx *ui, cui_id id, const cui_box *box, bool horizontal);
void cui__nice_range(float lo, float hi, int target_ticks, float *out_lo, float *out_hi, float *out_step);
const char *cui__fmt_value(cui_ctx *ui, const char *fmt, float v);
/* slider keys on a 0..1 position (cui_controls.c): arrows move by step (Shift: ten steps),
 * Home and End go to the ends; right to left, Left moves toward the end */
float cui__slider_keys(cui_ctx *ui, float t, float step, bool rtl);
/* gradient pieces (cui_curves.c): a checkerboard cut to a rounded rectangle, and a ramp of
 * stops drawn as cui_gradient_eval colors it, its ends rounded by rad */
void cui__draw_checker(cui_ctx *ui, cui_rect r, float rad, float cell, cui_color light, cui_color dark);
void cui__draw_gradient_ramp(cui_ctx *ui, cui_rect r, float rad, const cui_gradient_stop *stops, int n);
/* a dashed rounded rectangle, the dashes running on round the corners (cui_pickers.c) */
void cui__draw_dashed_rrect(cui_ctx *ui, cui_rect r, float rad, cui_color c, float th, float dash, float gap);
/* chart annotations (cui_instrument.c): drawn by every chart that takes them */
void cui__paint_annotations(cui_ctx *ui, const cui_chart_annotation *a, int n, cui_rect plot, double x0, double x1,
                            double y0, double y1, float label_top, bool gutter);
cui_chart_annotation *cui__copy_annotations(cui_ctx *ui, const cui_chart_annotation *a, int n);
/* a dashed hairline from x0 to x1 at y: `dash` points drawn every `period` (cui_instrument.c) */
void cui__draw_hdash(cui_ctx *ui, float x0, float x1, float y, float dash, float period, cui_color c);
/* Time rulers (flame graph, trace view, waterfall), shared by the three views
 * in cui_flame.c and checked by the tests: cui__fl_step gives the tick step for
 * at least `raw` seconds, cui__fl_tick the label of a tick `step` apart
 * ("4 min 50 s"), cui__fl_dur a duration in its unit ("12.4 ms", "1.50 min"),
 * cui__fl_ruler_step the step a ruler `pw` points wide uses for t0..t1, wide
 * enough for every label in view, and cui__fl_tick_x where tick q is drawn. */
double cui__fl_step(double raw);
double cui__fl_ruler_step(cui_ctx *ui, float pw, double t0, double t1, double origin);
float cui__fl_tick_x(float px0, double k, double t0, double origin, double q, double step);
const char *cui__fl_tick(cui_ctx *ui, double t, double step);
const char *cui__fl_dur(cui_ctx *ui, double s);
void cui__scroll_end_ex(cui_ctx *ui);

/* ---- cell grid (cui_cellgrid.c): the parts that are pure functions, so the tests can call them ---- */
enum { CUI__CG_NAV_NONE = 0, CUI__CG_NAV_SAME, CUI__CG_NAV_MOVED, CUI__CG_NAV_ALL };   /* not a nav key; unchanged; moved (follow it); select all (do not scroll) */
int cui__cg_col_at(const double *edges, int cols, double x);   /* the column holding x among cols + 1 ascending edges, or -1 */
int cui__cg_nav(cui_cellgrid_sel *s, int rows, int cols, cui_key key, int mods, int page);
double cui__cg_into_view(double scroll, double view, double lo, double hi);
static inline bool cui__cg_sel_same(const cui_cellgrid_sel *a, const cui_cellgrid_sel *b) {
    return a->row == b->row && a->col == b->col && a->anchor_row == b->anchor_row && a->anchor_col == b->anchor_col;
}
/* the data grid's rows, for the tree table that drags over them (cui_grid.c): where grid `id`
 * put its rows last frame (the area under the sticky header, and the top of row 0, which moves
 * with the scroll), false before its first layout; a nudge of its scroll target; and whether
 * the row cui_data_grid_row just began was pressed this frame */
bool cui__grid_rows_at(cui_ctx *ui, cui_id id, cui_rect *view, float *row0_top);
void cui__grid_scroll_by(cui_ctx *ui, cui_id id, float dy);
bool cui__grid_row_pressed(cui_ctx *ui);
cui_color cui__backdrop(cui_ctx *ui);

void cui__paint_scrollbars(cui_ctx *ui, cui_node *n, cui_rect clip);
void cui__paint_nav_highlight(cui_ctx *ui, cui_node *n, void *data);
void cui__paint_focus_ring_node(cui_ctx *ui, cui_node *n, void *data);
void cui__paint_sheen(cui_ctx *ui, cui_node *n, void *data);   /* data: float strength or NULL */

/* paint data helpers */
#define CUI_PAINT_DATA(ui, type) ((type *)cui__frame_alloc((ui), sizeof(type)))

cui_color cui__legible(cui_color fg, cui_color bg, float ratio);
cui_color cui__shade(cui_color c, float L, float chroma_mul);

/* ---- text editing primitives (cui_textedit.c), shared with the code editor ---- */
#define CUI__UNDO_MAX 48
typedef struct cui_edit_state {
    int cursor, anchor;        /* byte offsets */
    float scroll_x, scroll_y;  /* single line horizontal scroll / textarea vertical */
    float preferred_x;         /* for up/down in text areas */
    bool selecting;
    float hover, focus;
    char *undo[CUI__UNDO_MAX];
    int undo_cursor[CUI__UNDO_MAX];
    int undo_count, undo_pos;
    double last_edit;
    int last_kind;
    int last_cursor;
    bool had_focus;
    bool init;
    bool show_password;
    int lead_at;               /* the caret at this offset sits at the leading edge of the character after it (cui__bidi_caret_x_at); -1: nowhere */
    int para_dir;              /* text area with cui_input_use_paragraphs: the field's cui_input_opts.direction (set every frame) */
} cui_edit_state;
void cui__edit_attach(cui_ctx *ui, cui_id id, cui_edit_state *e);   /* once per state: the history is freed with it */
/* The edit menu (cui__edit_menu): a text widget's report for this frame (CUI__EM_* flags; focused: it has the keyboard;
 * menu: it was right-clicked, the menu is for it), and the command waiting for it (CUI_EDIT_*, taken: 0 after). */
void cui__edit_seen(cui_ctx *ui, cui_id id, int flags, bool focused, bool menu);
int cui__edit_cmd_take(cui_ctx *ui, cui_id id);
/* The menu CUI opens itself on a right click (a text field with cui_input_use_edit_menu, a code editor with .edit_menu):
 * opens popup pid at the pointer (open), then builds it if it is showing. keys: the shortcuts shown (NULL the usual). */
void cui__edit_menu_popup(cui_ctx *ui, cui_id pid, bool open, const struct cui_editor_keys *keys);
/* While that menu shows, the keys are its own (arrows, Enter, Esc): hold takes this frame's keys and typed text away
 * from the widget (true when it did), release gives them back for the menu, which is built after the widget. */
typedef struct cui__keys_held { uint8_t k[CUI_KEY_COUNT]; int text; } cui__keys_held;
bool cui__edit_menu_hold(cui_ctx *ui, cui_id pid, cui__keys_held *h);
void cui__edit_menu_release(cui_ctx *ui, const cui__keys_held *h);
/* Snapshot before an edit. kind: 1 insert, 2 delete, 3 other; edits of the
 * same kind at the caret within a second merge into one step. */
void cui__edit_undo_push(cui_ctx *ui, cui_edit_state *e, const char *buf, int kind);
bool cui__edit_undo_step(cui_ctx *ui, cui_edit_state *e, char *buf, size_t cap, bool redo);
int cui__edit_insert(char *buf, int *len, size_t cap, int pos, const char *s, int n);   /* drops '\r'; what fits, never half a code point */
void cui__edit_delete(char *buf, int *len, int a, int b);
bool cui__edit_is_word(unsigned char c);
int cui__edit_word_left(const char *s, int pos);
int cui__edit_word_right(const char *s, int len, int pos);

/* ---- text documents (cui_textdoc.c): the piece table behind cui_text_doc, and byte scans ---- */
const char *cui__find_byte(const char *p, size_t n, char c);        /* memchr at memory speed */
const char *cui__find_byte_back(const char *p, size_t n, char c);   /* the last c in p[0, n) */
size_t cui__count_byte(const char *p, size_t n, char c);
int cui__fill_nl(const char *s, int a, int b, int *out);            /* out[k]: the byte after the k-th '\n' of s[a, b); how many */
/* One edit as the document logged it: bytes [a, b0) became [a, b1); before it, a was on line la
 * and b0 on line lb; the new bytes hold k line breaks. Editors patch their caches from these. */
typedef struct { unsigned rev; int a, b0, b1, la, lb, k; } cui__tsplice;
#define CUI__DOC_LOG 256
int cui__doc_line_start(const cui_text_doc *d, int line);   /* line past the last: length + 1 (as the editor's index) */
int cui__doc_line_of(const cui_text_doc *d, int pos);
/* the piece holding byte pos: returns p with p[0] = byte *cs, valid to *ce; NULL at the end */
const char *cui__doc_chunk(const cui_text_doc *d, int pos, int *cs, int *ce);
/* Replace [a, b) with t[0, n): own (when not NULL) is t, given away (release(own, user), NULL: free);
 * caret is kept for undo; merge joins the step to the last one when it continues it (typing on,
 * deleting on) or else makes it part of the same undo group. nl (malloc'd, taken, or NULL) holds the
 * offsets of t's nn line breaks when they are known already: an own text is then not read again. */
bool cui__doc_edit(cui_text_doc *d, int a, int b, const char *t, int n, char *own, void (*release)(char *, void *), void *user,
                   int caret, bool merge, int *nl, int nn);
bool cui__doc_undo(cui_text_doc *d, bool redo, int *caret);
const cui__tsplice *cui__doc_splice_at(const cui_text_doc *d, unsigned rev);   /* valid for the last CUI__DOC_LOG revisions after base_rev */
unsigned cui__doc_base_rev(const cui_text_doc *d);
unsigned cui__doc_serial(const cui_text_doc *d);   /* unique per document in the process */
int cui__doc_maxlen(const cui_text_doc *d);
/* every line's length in bytes, in order (line_count of them), from the index alone: emit gets them in batches */
void cui__doc_line_lengths(const cui_text_doc *d, void (*emit)(void *u, const int *lens, int n), void *u);
/* Many stretches replaced as one edit (Replace all, found on another thread): [a, b) becomes the parts
 * in order, bytes [at, at + len) of the text (at >= 0) or of r (at < 0: from -at - 1). r and rnl (its
 * rnn line break offsets, or NULL) are taken. O(parts + pieces in [a, b)), one undo step. */
typedef struct { int at, len; } cui__tpart;
bool cui__doc_edit_parts(cui_text_doc *d, int a, int b, const cui__tpart *parts, int np, char *r, int rn, int *rnl, int rnn, int caret);
/* a snapshot of bytes [a, b) only (O(log n + its pieces)); its pieces with their offsets in the document */
cui_text_snap *cui__doc_snapshot_range(const cui_text_doc *d, int a, int b);
/* a prepared text (cui_text_prepare) as one edit and undo step: O(pieces), its bytes and line breaks taken as they are */
bool cui__doc_edit_prepared(cui_text_doc *d, int a, int b, cui_text_prepared *t, int caret);
/* takes a prepared text apart: its bytes (returned), length, line breaks and longest line; t is freed */
char *cui__prepared_take(cui_text_prepared *t, int *len, int **nl, int *nn, int *maxlen, int *maxcnt);
/* true for cui_text_prepare_adopt's: its bytes are given back with *release(p, *user), never freed */
bool cui__prepared_adopted(const cui_text_prepared *t, void (**release)(char *, void *), void **user);
const char *cui__snap_piece(const cui_text_snap *s, int i, int *a, int *b);
int cui__snap_pieces(const cui_text_snap *s);
/* the code editor's limits (tests lower them): line states found on the UI thread in a frame (bytes of text), the
 * document length from which Replace all runs on a thread, the most bytes of a buffer one job for line states copies,
 * the bytes left to count from which the find bar counts on a thread, and the length from which a line is "very long"
 * (only what is in view of it is read) */
extern int cui__ed_sync_bytes, cui__ed_rall_bytes, cui__ed_copy_bytes, cui__ed_count_bytes, cui__ed_long_bytes;
/* the document length from which the find bar searches only that many bytes from the caret in a frame (the rest on a thread) */
extern int cui__ed_find_bytes;
/* the markdown view's limits (tests lower them): the length from which a text takes the big path (only the
 * blocks near the window are built), the length from which its blocks are cut on another thread, and the
 * first part of such a text cut at once meanwhile */
extern int cui__md_big_bytes, cui__md_sync_bytes, cui__md_first_bytes;

/* ---- syntax highlighting (cui_code.c): the code block, the code editor and the diff viewer ---- */
typedef struct {
    const char *const *kw; const char *const *types; const char *line_comment, *line_comment2;
    bool block_comments, hash_preproc, backtick, no_caps_types, plain, markdown;
    bool fold_case;                    /* keywords, types and constants match in any case (SQL) */
    const char *const *consts;         /* more words colored as constants, or NULL */
    int mode;                          /* CUI__LM_*: a language with its own line tokenizer (markdown, config, markup and C-like ones) */
    bool multi;                        /* its lines carry an int state (cui__tokenize_st); the code editor caches it per line */
} cui__lang;
enum { CUI__LM_NONE, CUI__LM_MARKDOWN, CUI__LM_YAML, CUI__LM_TOML, CUI__LM_INI, CUI__LM_XML, CUI__LM_HTML, CUI__LM_CSS,
       CUI__LM_POWERSHELL, CUI__LM_CSHARP, CUI__LM_JAVA, CUI__LM_BATCH, CUI__LM_HTTP };
typedef struct { int start, len; uint8_t kind; } cui__tok;
#define CUI__TK_COUNT 10   /* token kinds; colors from cui__syntax_colors */
cui__lang cui__lang_for(const char *lang);
/* Tokenize one line. *in_block carries an open block comment between lines. */
/* markdown, one line: the multi-line state is an int (0 none); start it at CUI__MD_START on line 0 to find front matter */
#define CUI__MD_START 5
int cui__tokenize_md(const char *s, int n, int *state, cui__tok *out, int cap);
/* One line of any language with a mode (markdown, yaml, toml, ini, xml, html, css, powershell, csharp, java, batch).
 * *state is the int state between lines: 0 at the start (CUI__MD_START for markdown), kept per line by the code
 * editor. The tokens cover the line without gaps. cap 0 finds the state only. A caller that has only a bool passes 0 or 1. */
int cui__tokenize_st(const cui__lang *L, const char *s, int n, int *state, cui__tok *out, int cap);
int cui__tokenize_x(const cui__lang *L, const char *s, int n, int *state, cui__tok *out, int cap);   /* cui_code_langs.c */
bool cui__in_list(const char *const *list, const char *s, int n, bool fold);
bool cui__ident_start(char c);
bool cui__ident_char(char c);
int cui__tokenize(const cui__lang *L, bool json, const char *s, int n, bool *in_block, cui__tok *out, int cap);
/* The same, by language name as the code block takes it ("c", "py", "json"...). */
int cui__tokenize_lang(const char *lang, const char *s, int n, bool *in_block, cui__tok *out, int cap);
void cui__syntax_colors(cui_ctx *ui, cui_color *c);   /* CUI__TK_COUNT colors, by token kind */
/* One line of code in the mono font at (x, y), highlighted when lang is set, stopping past
 * max_x; *in_block carries a block comment to the next line. Returns the x after it. */
float cui__draw_code_line(cui_ctx *ui, const char *lang, const char *s, int n, bool *in_block, float x, float y, float size, const cui_color *colors, float max_x);
/* A code block whose text a surrounding view selects (the markdown view). The view sets
 * from, to and color, which are read when the block paints, so they may change after the
 * block is built; the block fills in where its text sits, for cui__code_sel_hit. */
typedef struct cui__code_sel {
    int from, to;              /* selected bytes of the code */
    cui_color color;
    cui_id canvas;             /* set by the block: the node the text is painted in */
    float size, lh, x0, y0;    /* set by the block: font size, line height, text origin in the canvas */
    float wrap_w;              /* set by the block: the width its lines wrap at (cui_code_opts.wrap), 0 when they do not */
} cui__code_sel;
void cui__code_block_sel(cui_ctx *ui, const char *id, const char *code, const cui_code_opts *o, cui__code_sel *sel);
/* The byte of code nearest the point (x, y), in the block's latest layout. */
int cui__code_sel_hit(cui_ctx *ui, const cui__code_sel *sel, const char *code, int len, float x, float y);

/* ---- code editor folding and minimap (cui_fold.c, cui_minimap.c) ----
 * Both read the editor's text through a cui__lines and follow its edits (lines [la, lb] became [la, la + k]). Their
 * glue in cui_editor.c (compiled for speed in apps' size builds) is CUI__COLD: kept small, out of the hot paths.
 * CUI__COLD also marks other code that runs once (create, destroy, a job's start) or per click or command in the files
 * apps compile for speed: GCC compiles it for size and keeps it apart. */
#if defined(__GNUC__) || defined(__clang__)
#define CUI__COLD __attribute__((cold, noinline))
#else
#define CUI__COLD
#endif
#define CUI__LINES_CAP 256   /* a cui__lines gives at most this many bytes of a line: enough for its indentation and look */
typedef struct cui__lines {
    const char *(*line)(void *u, int i, int *ls, int *le);   /* line i's first bytes as q[*ls, *le) of the q returned (NULL past the end) */
    int (*state)(void *u, int i);                            /* the highlighting state before line i, -1 not known (NULL: none) */
    void *u;
    int nl, tab;
} cui__lines;
/* Folds: lines h[i] + 1 to e[i] are hidden, h sorted. The hidden runs are their union, [va[j], vb[j]] with vp[j] hidden
 * lines above run j (vp[vn]: all). chg0..chg1: lines that were shown and are hidden now, or the other way, since the
 * editor last looked (it keeps their wrapped rows). */
typedef struct cui__fold {
    int *h, *e; int n, cap;
    int *va, *vb, *vp; int vn, vcap;
    int hidden;
    int chg0, chg1;
} cui__fold;
void cui__fold_free(cui_ctx *ui, cui__fold *F);
void cui__fold_clear(cui__fold *F);
int cui__fold_at(const cui__fold *F, int h);                         /* the fold with header h, or -1 */
bool cui__fold_set(cui_ctx *ui, cui__fold *F, int h, int e);         /* fold lines h + 1 to e (false: no change) */
bool cui__fold_unset(cui_ctx *ui, cui__fold *F, int h);
bool cui__fold_reveal(cui_ctx *ui, cui__fold *F, int line);          /* open every fold that hides line */
void cui__fold_splice(cui_ctx *ui, cui__fold *F, int la, int lb, int k);
int cui__fold_all(cui_ctx *ui, cui__fold *F, const cui__lines *L, bool md);   /* fold every block (reads the whole text) */
/* blocks: whether line h starts one (looks at the next lines that are not blank), its last line (-1: none), and the
 * innermost one holding a line that is not folded (its header, -1 none). md: by markdown headings, else indentation. */
bool cui__fold_starts(const cui__lines *L, bool md, int h);
int cui__fold_end(const cui__lines *L, bool md, int h);
int cui__fold_around(const cui__lines *L, bool md, const cui__fold *F, int line, int *end);
/* rows with the hidden lines taken out (no wrap): a line's row (a hidden line: the row of the next line shown), the line
 * shown at a row (rows past the end go on past the last line), the first line shown at or after a line, at or before,
 * and the first hidden line at or after a line (INT_MAX: none). F may be NULL: no folds. */
bool cui__fold_hidden(const cui__fold *F, int line);
int cui__fold_row(const cui__fold *F, int line);
int cui__fold_line(const cui__fold *F, int row);
int cui__fold_next(const cui__fold *F, int line);
int cui__fold_prev(const cui__fold *F, int line);
int cui__fold_gap(const cui__fold *F, int line);
#define CUI__FOLD_W 14.0f        /* the fold markers' lane, at the right of the gutter */
#define CUI__FOLD_BOX_COLS 2.6f  /* the box after a folded line, in columns */
/* What the editor's paint draws of the folds: lines in view with a marker, dy their tops from the editor's top, kind 1
 * a block (its marker shows while the pointer is over the gutter) or 2 folded (its box goes after column col of the row
 * by from the top: wrapped, the line's last row). */
typedef struct cui__fold_paint {
    int n; const float *dy, *by; const uint8_t *kind; const int *col;
    float lh, cw, fw; int cb;
    bool hover;
    cui_color mark, mark_on, box, box_edge, dots;
} cui__fold_paint;
void cui__fold_draw(cui_ctx *ui, const cui__fold_paint *fp, cui_rect r, float gx, float x0, int part);   /* part 0 the gutter, 1 the boxes */
/* Minimap bars, one a line: indentation and length in columns, token kind, and the line state it is for + 1 (0: not made). */
typedef struct { uint8_t ind, len, kind, st; } cui__mini_bar;
typedef struct cui__minimap {
    cui__mini_bar *b; int n, cap;
    bool drag; float grab;       /* a press on the strip is held, at grab points from the top of the box of the rows in view */
} cui__minimap;
#define CUI__MINI_W ((float)CUI_MM_WIDTH)   /* the strip's width */
#define CUI__MINI_ROW 2.0f         /* a row's height on it */
#define CUI__MINI_COLS 80          /* columns that fit across it */
void cui__mini_free(cui_ctx *ui, cui__minimap *M);
void cui__mini_reset(cui__minimap *M);
void cui__mini_lines(cui_ctx *ui, cui__minimap *M, int nl);
void cui__mini_splice(cui_ctx *ui, cui__minimap *M, int la, int lb, int k);
const cui__mini_bar *cui__mini_bar_of(cui_ctx *ui, cui__minimap *M, const cui__lines *L, const cui__lang *lang, bool json, int i);
typedef struct { double top, k; float rh, vy, vh; } cui__mini_geo;   /* the text's row at the strip's top; the box of the view */
void cui__mini_geo_of(cui__mini_geo *g, float h, int rows, double lh, double sy, double max_sy, double view_h);
typedef struct cui__mini_paint {
    int n; const float *y; const cui__mini_bar *b;   /* the rows on the strip: tops from its top, and bars */
    float rh, cw, vy, vh;
    bool hot;
    cui_color colors[CUI__TK_COUNT], fg, view, view_hot, edge;
    cui_id sid;                  /* the editor's scroll: the wheel over the strip scrolls the text */
} cui__mini_paint;
void cui__mini_paint_fn(cui_ctx *ui, cui_node *n, void *data);

/* ---- layered graph layout (cui_graph.c): the dependency graph's layout, for other diagrams too ---- */
typedef struct { float x, y, w, h; int layer; } cui__layer_box;
typedef struct {
    int n;                       /* nodes; box[n..nv) are waypoints of edges that skip layers */
    int nv;
    cui__layer_box *box;
    int ne;                      /* edges as laid out: each runs from[k] -> its waypoints -> to[k] */
    int *from, *to, *rstart, *rlen, *route;
    float gw, gh;                /* the graph's size */
    bool top_down;
} cui__layered;
void cui__layered_free(cui_ctx *ui, cui__layered *g);
/* Lays out n boxes of the sizes w[i] x h[i] in layers (edges point to later layers, cycles
 * broken), gap_layer between layers (0.6 of it top down) and gap_node between the boxes of a
 * layer. Edges out of range or from a node to itself are left out, so the laid out edges match
 * the given ones only when there are none. Heap memory in g; scratch from the frame arena.
 * max_boxes > 0 caps nodes plus waypoints: past it nothing is laid out and it returns false. */
bool cui__layered_layout(cui_ctx *ui, cui__layered *g, const float *w, const float *h, int n,
                         const cui_graph_edge *edges, int ne, bool top_down, float gap_layer, float gap_node, int max_boxes);

/* ---- mermaid diagrams (cui_mermaid.c): fenced mermaid blocks in the markdown view ---- */
/* A diagram's labels as text, for the markdown view's selection: flowchart nodes in reading
 * order (layer by layer, then across), then edge labels; a pie's legend lines ("Dogs 82%");
 * a gantt's task names. The view sets from, to and color, read when the diagram paints, so
 * they may change after it is built; the diagram fills in the rest, in frame memory. */
typedef struct cui__mermaid_sel {
    int from, to;              /* selected bytes of text: labels they touch highlight whole */
    cui_color color;
    char *text; int len;       /* the labels, a line each */
    int count;                 /* labels */
    int *at;                   /* count + 1 offsets: label k is text[at[k]] up to at[k + 1] - 1 */
    cui_rect *rect;            /* where label k was in the latest layout; zero width when unknown */
} cui__mermaid_sel;
/* Builds the diagram in src (len bytes) under id, body the markdown's text size; false, with
 * nothing built, when src is not a diagram it draws. *natural gets the width it would like.
 * sel, when not NULL, gets the labels. */
bool cui__mermaid_block(cui_ctx *ui, const char *id, const char *src, int len, float body, float *natural, cui__mermaid_sel *sel);
/* For tests: lays out the flowchart in src and counts where routed edges cross each other and
 * where an edge runs through a box that is not one of its ends. False for anything else. */
bool cui__mermaid_probe(cui_ctx *ui, const char *src, int len, float body, int *crossings, int *box_hits);
/* For tests: the scale, expand button and expanded view of the flowchart built under id. */
bool cui__mermaid_view_info(cui_ctx *ui, const char *id, float *zoom, bool *expand, bool *big_open, float *natural);

/* ---- tree selection and drag and drop (cui_treesel.c) ---- */
/* What a row built inside cui_tree_multi_begin learns: drawn selected, the drop target, a click with Ctrl or Shift
 * (it does not toggle the folder), and a closed folder that has been hovered long enough by a drag (open it).
 * False, with out untouched, for a row without an item or outside the wrapper. */
typedef struct cui__tsel_flags { bool selected, target, mod_click, expand; } cui__tsel_flags;
bool cui__tsel_row(cui_ctx *ui, cui_id id, const cui_tree_opts *o, const char *label, bool folder, bool is_open,
                   const cui_interact *r, cui__tsel_flags *out);

#endif /* CUI_INTERNAL_H */
