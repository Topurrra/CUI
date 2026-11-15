/* Source control lists: the changed files list, the commit history list with its lane graph, and the branch
 * picker. All three take plain data (rows with labels, letters, tones, lane edges) and report what was clicked;
 * none of them knows about git. Rows come from an array or a callback, and only the rows in view are asked for.
 *
 * Phases of each list: input (against last frame), build (the nodes of the rows in view, each with its paint data
 * in frame memory), paint (one callback per row). */
#include "cui_internal.h"

#include <math.h>
#include <string.h>

/* ======================================================================== */
/* Shared                                                                   */
/* ======================================================================== */

/* the tone of a letter or chip: the theme's tokens for the status tones (the same colors the rest of the UI uses
 * for success, warning, danger and info), the palette tones as they are */
static cui_color cui__vcs_tone(cui_ctx *ui, cui_tone t) {
    if (t == CUI_TONE_INFO) return CUI_C(ui, INFO);
    return cui__tone_color(ui, t);
}

static float cui__vcs_text_w(cui_ctx *ui, cui_font font, float size, const char *s) {
    int n = s ? (int)strlen(s) : 0;
    return n > 0 ? cui_measure_text(ui, font, size, s, n).width : 0;
}

/* scrolls the list being built (inside the id scope that holds its "rows") so row idx is in view */
static void cui__vcs_keep_visible(cui_ctx *ui, int idx, float row_h) {
    cui_scroll_info si = cui_scroll_get(ui, "rows");
    if (!si.found || idx < 0) return;
    float top = (float)idx * row_h;
    float vh = si.view_h > 0 ? si.view_h : 400;
    if (top < si.y) cui_scroll_set(ui, "rows", top, true);
    else if (top + row_h > si.y + vh) cui_scroll_set(ui, "rows", top + row_h - vh, true);
}

static bool cui__vcs_has(const char *hay, const char *needle) {
    if (!needle[0]) return true;
    size_t fl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < fl && p[i] && (p[i] | 32) == (needle[i] | 32)) i++;
        if (i == fl) return true;
    }
    return false;
}

/* ======================================================================== */
/* Changes list                                                             */
/* ======================================================================== */

#define CUI__CHG_ROW      22.0f
#define CUI__CHG_MAX_SEC  16        /* sections whose fold state is kept inside when the app keeps none */
#define CUI__CHG_MAX_ACT  6
#define CUI__CHG_BTN_W    22.0f

typedef struct {
    bool collapsed[CUI__CHG_MAX_SEC];
    cui_change_ref cursor;
    bool init, reveal;
} cui__chg_state;

typedef struct {
    const char *title, *name, *dir, *icon;
    const char *acts[CUI__CHG_MAX_ACT];   /* icons */
    int nacts;
    char state;
    cui_tone tone;
    bool header, selected, focus, collapsed, hovered, struck;
    int count;
    float tail_w;                         /* the letter's or the count's room at the right */
} cui__chg_paint;

/* the frame's shared view of the sections, fold flags and cursor */
typedef struct {
    const cui_change_section *s;
    int ns;
    bool *col;
    int ncol;                             /* fold flags available */
} cui__chgf;

static bool cui__chg_folded(const cui__chgf *f, int s) { return s >= 0 && s < f->ncol && f->col[s]; }

static int cui__chg_flat_count(const cui__chgf *f) {
    int n = 0;
    for (int s = 0; s < f->ns; s++)
        if (f->s[s].count > 0) n += 1 + (cui__chg_folded(f, s) ? 0 : f->s[s].count);
    return n;
}

static cui_change_ref cui__chg_ref_at(const cui__chgf *f, int i) {
    cui_change_ref r = { -1, -1 };
    for (int s = 0; s < f->ns; s++) {
        if (f->s[s].count <= 0) continue;
        int span = 1 + (cui__chg_folded(f, s) ? 0 : f->s[s].count);
        if (i < span) { r.section = s; r.row = i - 1; return r; }
        i -= span;
    }
    return r;
}

/* the flat index of a reference, or -1 when it is not shown */
static int cui__chg_flat_of(const cui__chgf *f, cui_change_ref c) {
    int at = 0;
    for (int s = 0; s < f->ns; s++) {
        if (f->s[s].count <= 0) continue;
        if (s == c.section) {
            if (c.row < 0) return at;
            if (cui__chg_folded(f, s) || c.row >= f->s[s].count) return -1;
            return at + 1 + c.row;
        }
        at += 1 + (cui__chg_folded(f, s) ? 0 : f->s[s].count);
    }
    return -1;
}

static cui_rect cui__chg_btn(cui_rect r, float tail_w, int k) {
    return (cui_rect){ r.x + r.w - tail_w - CUI__CHG_BTN_W * (float)(k + 1), r.y + 1, CUI__CHG_BTN_W, r.h - 2 };
}

/* which button of p is at (x, y), or -1; the last button is at the right */
static int cui__chg_btn_at(const cui__chg_paint *p, cui_rect r, float x, float y) {
    for (int i = p->nacts - 1, k = 0; i >= 0; i--, k++) {
        cui_rect b = cui__chg_btn(r, p->tail_w, k);
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
    }
    return -1;
}

static void cui__paint_chg_row(cui_ctx *ui, cui_node *n, void *data) {
    const cui__chg_paint *p = data;
    cui_rect r = n->rect;
    float mx = ui->mx, my = ui->my;
    if (p->selected) cui_draw_rect(ui, r, CUI_CORNERS(4), p->focus ? CUI_COLOR_ACCENT_SUBTLE : CUI_COLOR_HOVER);
    else if (p->hovered) cui_draw_rect(ui, r, CUI_CORNERS(4), CUI_COLOR_HOVER);
    if (!p->header && !p->name) return;
    cui_push_clip(ui, r);
    float fs = cui__fs(ui, 12.5f), fm = cui__fs(ui, 11.5f);
    float ty = r.y + (r.h - fs * 1.25f) / 2 - 1;
    float right = r.x + r.w - 8;
    char tail[16];
    if (p->header) snprintf(tail, sizeof tail, "%d", p->count);
    else snprintf(tail, sizeof tail, "%c", p->state ? p->state : ' ');
    float tw = cui__vcs_text_w(ui, p->header ? CUI_FONT_BODY : CUI_FONT_MEDIUM, fm, tail);
    if (p->header && p->count > 0) {
        cui_rect pill = { right - tw - 10, r.y + 4, tw + 10, r.h - 8 };
        cui_draw_rect(ui, pill, CUI_CORNERS(7), CUI_COLOR_SURFACE_RAISED);
        cui_draw_text(ui, CUI_FONT_BODY, fm, (cui_vec2){ pill.x + 5, ty + 0.5f }, CUI_COLOR_TEXT_SECONDARY, tail, (int)strlen(tail));
    } else if (!p->header && p->state) {
        cui_draw_text(ui, CUI_FONT_MEDIUM, fm, (cui_vec2){ right - 8 - tw / 2, ty + 0.5f }, cui__vcs_tone(ui, p->tone), tail, (int)strlen(tail));
    }
    float limit = right - (p->header ? tw + 14 : 26);
    if (p->hovered) {   /* the buttons, over the end of the text */
        for (int i = p->nacts - 1, k = 0; i >= 0; i--, k++) {
            cui_rect b = cui__chg_btn(r, p->tail_w, k);
            bool on = mx >= b.x && mx < b.x + b.w && my >= b.y && my < b.y + b.h;
            if (on) cui_draw_rect(ui, b, CUI_CORNERS(4), CUI_COLOR_HOVER);
            cui_draw_icon(ui, p->acts[i], b, 14, on ? CUI_COLOR_FOREGROUND : CUI_COLOR_TEXT_SECONDARY);
            limit = b.x - 2;
        }
    }
    cui_push_clip(ui, (cui_rect){ r.x, r.y, limit - r.x, r.h });
    if (p->header) {
        cui_draw_icon(ui, p->collapsed ? CUI_ICON_CHEVRON_RIGHT : CUI_ICON_CHEVRON_DOWN, (cui_rect){ r.x + 2, r.y, 16, r.h }, 13, CUI_COLOR_TEXT_SECONDARY);
        cui_draw_text(ui, CUI_FONT_MEDIUM, cui__fs(ui, 12), (cui_vec2){ r.x + 20, ty }, CUI_COLOR_FOREGROUND, p->title, (int)strlen(p->title));
    } else {
        cui_draw_icon(ui, p->icon, (cui_rect){ r.x + 14, r.y, 16, r.h }, 14, CUI_COLOR_TEXT_SECONDARY);
        float x = r.x + 34;
        int nl = (int)strlen(p->name);
        cui_color nc = p->struck ? CUI_COLOR_TEXT_MUTED : CUI_COLOR_FOREGROUND;
        cui_draw_text(ui, CUI_FONT_BODY, fs, (cui_vec2){ x, ty }, nc, p->name, nl);
        float nw = cui_measure_text(ui, CUI_FONT_BODY, fs, p->name, nl).width;
        if (p->struck) cui_draw_line(ui, (cui_vec2){ x, r.y + r.h / 2 }, (cui_vec2){ x + nw, r.y + r.h / 2 }, 1, CUI_COLOR_TEXT_MUTED);
        if (p->dir && p->dir[0]) cui_draw_text(ui, CUI_FONT_BODY, fm, (cui_vec2){ x + nw + 6, ty + 0.5f }, CUI_COLOR_TEXT_MUTED, p->dir, (int)strlen(p->dir));
    }
    cui_pop_clip(ui);
    cui_pop_clip(ui);
}

/* the cursor moves to flat index i (clamped) */
static void cui__chg_move(const cui__chgf *f, cui_change_ref *cur, int i, int n) {
    if (n <= 0) return;
    *cur = cui__chg_ref_at(f, CUI_CLAMP(i, 0, n - 1));
}

static void cui__chg_fold(const cui__chgf *f, int s, bool v, cui_changes_result *res) {
    if (s < 0 || s >= f->ncol || f->col[s] == v) return;
    f->col[s] = v;
    res->folded = s;
}

/* Up, Down, Home, End, PageUp, PageDown, Enter, Space, Left, Right with the list focused */
static void cui__chg_keys(cui_ctx *ui, const cui__chgf *f, cui_change_ref *cur, int n, float row_h, cui_changes_result *res, bool *reveal) {
    int at = cui__chg_flat_of(f, *cur), to = at;
    cui_scroll_info si = cui_scroll_get(ui, "rows");
    int page = si.found && si.view_h > 0 ? (int)(si.view_h / row_h) - 1 : 10;
    if (page < 1) page = 1;
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) to = at < 0 ? 0 : at + 1;
    if (cui_key_pressed(ui, CUI_KEY_UP)) to = at <= 0 ? 0 : at - 1;
    if (cui_key_pressed(ui, CUI_KEY_PAGE_DOWN)) to = at < 0 ? 0 : at + page;
    if (cui_key_pressed(ui, CUI_KEY_PAGE_UP)) to = at <= 0 ? 0 : at - page;
    if (cui_key_pressed(ui, CUI_KEY_HOME)) to = 0;
    if (cui_key_pressed(ui, CUI_KEY_END)) to = n - 1;
    if (to != at) { cui__chg_move(f, cur, to, n); *reveal = true; }
    if (cur->section < 0 || cui__chg_flat_of(f, *cur) < 0) return;
    if (cui_key_pressed(ui, CUI_KEY_SPACE)) { cui_key_consume(ui, CUI_KEY_SPACE); res->space = *cur; }
    if (cui_key_pressed(ui, CUI_KEY_ENTER)) {
        cui_key_consume(ui, CUI_KEY_ENTER);
        if (cur->row >= 0) res->clicked = *cur;
        else { cui__chg_fold(f, cur->section, !cui__chg_folded(f, cur->section), res); *reveal = true; }
    }
    if (cur->row < 0 && cui_key_pressed(ui, CUI_KEY_LEFT)) cui__chg_fold(f, cur->section, true, res);
    if (cur->row < 0 && cui_key_pressed(ui, CUI_KEY_RIGHT)) cui__chg_fold(f, cur->section, false, res);
}

/* one row or header of the flat list: its input, its node */
static void cui__chg_row(cui_ctx *ui, const cui__chgf *f, cui_id id, cui_id lid, int i, cui_change_ref ref, cui_change_ref *cur,
                         bool focus, float row_h, cui_changes_result *res) {
    const cui_change_section *sec = &f->s[ref.section];
    cui__chg_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    cui_change_row row;
    memset(&row, 0, sizeof row);
    const cui_change_action *acts;
    int nacts;
    float fm = cui__fs(ui, 11.5f);
    p->header = ref.row < 0;
    if (p->header) {
        p->title = cui__frame_strdup(ui, sec->title ? sec->title : "", -1);
        p->count = sec->count;
        p->collapsed = cui__chg_folded(f, ref.section);
        acts = sec->actions; nacts = sec->nactions;
        char num[16];
        snprintf(num, sizeof num, "%d", sec->count);
        p->tail_w = CUI_MAX(30.0f, cui__vcs_text_w(ui, CUI_FONT_BODY, fm, num) + 22);
    } else {
        bool have = sec->rows ? (row = sec->rows[ref.row], true) : sec->row_at ? sec->row_at(sec->user, ref.row, &row) : false;
        if (have && row.name) {
            p->name = cui__frame_strdup(ui, row.name, -1);
            p->dir = row.dir ? cui__frame_strdup(ui, row.dir, -1) : NULL;
            p->icon = cui__frame_strdup(ui, row.icon ? row.icon : CUI_ICON_FILE, -1);
            p->state = row.state; p->tone = row.state_tone; p->struck = row.struck;
        }
        acts = have ? row.actions : NULL; nacts = have ? row.nactions : 0;
        p->tail_w = 30;
    }
    if (nacts > CUI__CHG_MAX_ACT) nacts = CUI__CHG_MAX_ACT;
    for (int k = 0; k < nacts; k++) p->acts[k] = cui__frame_strdup(ui, acts[k].icon ? acts[k].icon : "", -1);
    p->nacts = nacts;

    cui_id rid = cui__hash(&i, sizeof i, id ^ 0xC4A6E7u);
    cui_interact ri = cui__interact(ui, rid, CUI_IF_KEEP_FOCUS);
    cui_rect pr = cui__prev_rect(ui, rid);
    bool known = cui__has_prev_rect(ui, rid);
    int hit = ri.hovered && known ? cui__chg_btn_at(p, pr, ui->mx, ui->my) : -1;
    if (ri.clicked) {
        ui->focus = lid;
        if (hit >= 0) { res->action_at = ref; res->action = acts[hit].id; }
        else {
            *cur = ref;
            res->clicked = ref;
            if (p->header) cui__chg_fold(f, ref.section, !cui__chg_folded(f, ref.section), res);
        }
    }
    if (ri.hovered && hit >= 0 && acts[hit].tooltip) cui__tooltip_passive(ui, rid, acts[hit].tooltip);
    p->selected = cur->section == ref.section && cur->row == ref.row;
    p->focus = focus;
    p->hovered = ri.hovered;
    int node = cui__leaf(ui, rid, CUI_FILL, CUI_PX(row_h));
    cui__mark_interactive(ui, node, 0);
    ui->nodes[node].paint = cui__paint_chg_row;
    ui->nodes[node].paint_data = p;
    ui->nodes[node].direction = CUI_DIRECTION_LTR;
}

cui_changes_result cui_changes_list_ex(cui_ctx *ui, const char *id_str, const cui_change_section *sections, int nsections, const cui_changes_opts *o) {
    cui_changes_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_changes_result res;
    memset(&res, 0, sizeof res);
    res.clicked = res.action_at = res.space = res.cursor = (cui_change_ref){ -1, -1 };
    res.folded = -1;
    if (!sections || nsections < 0) nsections = 0;
    cui_id id = cui_make_id(ui, id_str);
    cui_id lid = id ^ 0xC4A6E5u;
    cui__chg_state *st = cui_state(ui, id ^ 0xC4A6E3u, sizeof *st);
    if (!st->init) { st->init = true; st->cursor = (cui_change_ref){ -1, -1 }; }
    cui_change_ref *cur = o->cursor ? o->cursor : &st->cursor;
    cui__chgf f = { sections, nsections, o->collapsed ? o->collapsed : st->collapsed,
                    o->collapsed ? nsections : CUI_MIN(nsections, CUI__CHG_MAX_SEC) };
    float row_h = o->row_height > 0 ? o->row_height : CUI__CHG_ROW;
    int n = cui__chg_flat_count(&f);

    int outer = cui__node_new(ui, lid);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height.kind ? o->height : CUI_FILL;
    on->flags |= CUI_NF_CLIP;
    cui__mark_interactive(ui, outer, CUI_IF_FOCUSABLE);
    cui_interact lr = cui__interact(ui, lid, CUI_IF_FOCUSABLE);
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    res.focused = lr.keyboard_focused;
    if (lr.keyboard_focused && n > 0) cui__chg_keys(ui, &f, cur, n, row_h, &res, &st->reveal);
    if (st->reveal) {
        int at = cui__chg_flat_of(&f, *cur);
        if (at >= 0) cui__vcs_keep_visible(ui, at, row_h);
        st->reveal = false;
    }
    int first, last;
    cui_virtual_list_begin(ui, "rows", n, row_h, CUI_FILL, &first, &last);
    for (int i = first; i < last; i++) {
        cui_change_ref ref = cui__chg_ref_at(&f, i);
        if (ref.section < 0) break;
        cui__chg_row(ui, &f, id, lid, i, ref, cur, lr.focused, row_h, &res);
    }
    cui_virtual_list_end(ui);
    cui_pop_id(ui);
    cui__close(ui);
    if (cui__chg_flat_of(&f, *cur) < 0 && cur->section >= 0 && cur->row >= 0) {   /* a folded section's row: the header takes it */
        if (cur->section < nsections && cui__chg_folded(&f, cur->section)) cur->row = -1;
    }
    res.cursor = *cur;
    ui->last_id = lid;
    return res;
}

void cui_changes_list_reveal(cui_ctx *ui, const char *id_str) {
    cui__chg_state *st = cui_state(ui, cui_make_id(ui, id_str) ^ 0xC4A6E3u, sizeof *st);
    if (!st->init) { st->init = true; st->cursor = (cui_change_ref){ -1, -1 }; }
    st->reveal = true;
}

/* ======================================================================== */
/* Commit log                                                               */
/* ======================================================================== */

#define CUI__LOG_LANE_W    13.0f
#define CUI__LOG_MAX_LANES 14
#define CUI__LOG_EDGES     16
#define CUI__LOG_CHIPS     4
#define CUI__LOG_MAX_ROWS  128      /* rows built in one frame, however tall the view */
#define CUI__LOG_WIDE      640.0f   /* below this width the author is left out */

typedef struct {
    int sel;
    int reveal;                     /* row to bring into view, -1 none */
    bool top, init;
} cui__log_state;

/* what every row of the frame shares */
typedef struct {
    float fs, row_h, graph_w;
    int lanes;
    cui_color pal[8];
    cui_color bg;
} cui__logf;

typedef struct {
    const cui__logf *f;
    cui_lane_edge e[CUI__LOG_EDGES];
    struct { const char *text, *suffix; cui_tone tone; bool strong; } chips[CUI__LOG_CHIPS];
    const char *hash, *subject, *author, *date;
    int16_t lane;
    uint16_t color;
    uint8_t nedges, nchips, more;
    bool merge, tip, head, selected, hovered, blank, graph;
} cui__log_paint;

static bool cui__log_dark(cui_ctx *ui) {
    cui_color bg = cui_token_color(ui, CUI_TOKEN_BACKGROUND);
    return (bg.r * 299 + bg.g * 587 + bg.b * 114) / 1000 < 128;
}

static cui_color cui__log_lane(bool dark, int n) {
    static const float hue[8] = { 255, 150, 28, 305, 85, 205, 350, 125 };
    return cui_oklch(dark ? 0.76f : 0.58f, dark ? 0.14f : 0.16f, hue[(unsigned)n % 8], 1);
}

cui_color cui_lane_color(cui_ctx *ui, int n) { return cui__log_lane(cui__log_dark(ui), n); }

static float cui__log_lane_x(const cui__logf *f, cui_rect r, int lane) {
    if (lane >= f->lanes) lane = f->lanes - 1;
    if (lane < 0) lane = 0;
    return r.x + 8 + (float)lane * CUI__LOG_LANE_W + CUI__LOG_LANE_W / 2;
}

static float cui__log_smooth(float t) { return t * t * (3 - 2 * t); }

/* a lane line from (x0, y0) to (x1, y1): straight, or an S curve when the lane changes */
static void cui__log_curve(cui_ctx *ui, float x0, float y0, float x1, float y1, cui_color col) {
    if (x0 == x1) {
        cui_draw_line(ui, (cui_vec2){ x0, y0 }, (cui_vec2){ x1, y1 }, 2.0f, col);
        return;
    }
    cui_vec2 pts[9];
    for (int i = 0; i <= 8; i++) {
        float t = (float)i / 8;
        pts[i] = (cui_vec2){ x0 + (x1 - x0) * cui__log_smooth(t), y0 + (y1 - y0) * t };
    }
    cui_draw_polyline(ui, pts, 9, 2.0f, col, false);
}

static void cui__log_paint_graph(cui_ctx *ui, const cui__log_paint *p, cui_rect r) {
    const cui__logf *f = p->f;
    float top = r.y, mid = r.y + r.h / 2, bot = r.y + r.h;
    cui_push_clip(ui, (cui_rect){ r.x, r.y, f->graph_w, r.h });
    for (int i = 0; i < p->nedges; i++) {
        const cui_lane_edge *e = &p->e[i];
        cui_color col = f->pal[e->color % 8];
        if (e->kind == CUI_LANE_PASS) cui__log_curve(ui, cui__log_lane_x(f, r, e->from), top, cui__log_lane_x(f, r, e->to), bot, col);
        else if (e->kind == CUI_LANE_IN) cui__log_curve(ui, cui__log_lane_x(f, r, e->from), top, cui__log_lane_x(f, r, p->lane), mid, col);
        else cui__log_curve(ui, cui__log_lane_x(f, r, p->lane), mid, cui__log_lane_x(f, r, e->to), bot, col);
    }
    float x = cui__log_lane_x(f, r, p->lane);
    cui_color col = f->pal[p->color % 8];
    /* the commit's own lane arrives from above (the layout lists only the extra lanes that join here) */
    if (!p->tip) cui_draw_line(ui, (cui_vec2){ x, top }, (cui_vec2){ x, mid }, 2.0f, col);
    cui_color bg = p->selected ? CUI_C(ui, ACCENT_SUBTLE) : f->bg;
    if (p->head) {
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 7.0f, CUI_C(ui, ACCENT));
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 5.2f, bg);
    }
    if (p->merge) {   /* a merge: a ring */
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 5.0f, col);
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 2.6f, bg);
    } else {
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 5.4f, bg);
        cui_draw_circle(ui, (cui_vec2){ x, mid }, 4.0f, col);
    }
    cui_pop_clip(ui);
}

/* a chip: the accent when strong, else a tint of its tone with the normal text color */
static float cui__log_chip(cui_ctx *ui, const cui__log_paint *p, int k, float x, float mid, float ty, float limit) {
    const cui__logf *f = p->f;
    float cs = f->fs - 2.5f;
    const char *text = p->chips[k].text, *suffix = p->chips[k].suffix;
    float tw = cui__vcs_text_w(ui, CUI_FONT_MEDIUM, cs, text);
    float cw = tw + cui__vcs_text_w(ui, CUI_FONT_MEDIUM, cs, suffix) + 12;
    if (x + cw > limit) return -1;
    cui_color bg, fg;
    if (p->chips[k].strong) { bg = CUI_C(ui, ACCENT); fg = CUI_C(ui, ACCENT_FOREGROUND); }
    else {
        cui_color t = p->chips[k].tone ? cui__vcs_tone(ui, p->chips[k].tone) : CUI_C(ui, TEXT_SECONDARY);
        bg = cui_alpha(t, 0.24f);   /* a tint of the tone: the text stays the normal color */
        fg = CUI_C(ui, FOREGROUND);
    }
    float ch = f->fs + 3;
    cui_draw_rect(ui, (cui_rect){ x, mid - ch / 2, cw, ch }, CUI_CORNERS(ch / 2), bg);
    cui_draw_text(ui, CUI_FONT_MEDIUM, cs, (cui_vec2){ x + 6, ty + 1.2f }, fg, text, (int)strlen(text));
    if (suffix && suffix[0])
        cui_draw_text(ui, CUI_FONT_MEDIUM, cs, (cui_vec2){ x + 6 + tw, ty + 1.2f }, cui_alpha(fg, 0.7f), suffix, (int)strlen(suffix));
    return cw + 5;
}

static void cui__paint_log_row(cui_ctx *ui, cui_node *n, void *data) {
    const cui__log_paint *p = data;
    const cui__logf *f = p->f;
    cui_rect r = n->rect;
    if (p->selected) cui_draw_rect(ui, r, CUI_CORNERS(0), CUI_COLOR_ACCENT_SUBTLE);
    else if (p->hovered) cui_draw_rect(ui, r, CUI_CORNERS(0), CUI_COLOR_HOVER);
    if (p->blank) return;
    float mid = r.y + r.h / 2;
    float text_x = r.x + 8;
    if (p->graph) {
        cui__log_paint_graph(ui, p, r);
        text_x = r.x + f->graph_w + 4;
    }
    cui_push_clip(ui, r);
    float ty = r.y + (r.h - f->fs * 1.25f) / 2 - 1;
    float small = f->fs - 1.5f;
    cui_draw_text(ui, CUI_FONT_MONO, small, (cui_vec2){ text_x, ty + 0.5f }, CUI_COLOR_TEXT_MUTED, p->hash, (int)strlen(p->hash));
    float x = text_x + cui__vcs_text_w(ui, CUI_FONT_MONO, small, "abcdef0") + 10;
    /* the right end: the date, then the author when there is room */
    float right = r.x + r.w - 10;
    int wl = (int)strlen(p->date);
    float ww = cui__vcs_text_w(ui, CUI_FONT_BODY, small, p->date);
    float date_x = right - (ww > 78 ? ww : 78);
    cui_draw_text(ui, CUI_FONT_BODY, small, (cui_vec2){ right - ww, ty + 0.5f }, CUI_COLOR_TEXT_MUTED, p->date, wl);
    float limit = date_x - 10;
    if (r.w > CUI__LOG_WIDE) {
        float aw = 130;
        float ax = date_x - aw - 12;
        cui_push_clip(ui, (cui_rect){ ax, r.y, aw, r.h });
        cui_draw_text(ui, CUI_FONT_BODY, small, (cui_vec2){ ax, ty + 0.5f }, CUI_COLOR_TEXT_SECONDARY, p->author, (int)strlen(p->author));
        cui_pop_clip(ui);
        limit = ax - 10;
    }
    /* chips, then the subject */
    for (int k = 0; k < p->nchips; k++) {
        float adv = cui__log_chip(ui, p, k, x, mid, ty, limit);
        if (adv < 0) break;
        x += adv;
    }
    if (p->more && x + 24 < limit) {
        char more[12];
        snprintf(more, sizeof more, "+%d", p->more);
        cui_draw_text(ui, CUI_FONT_BODY, f->fs - 2.5f, (cui_vec2){ x, ty + 1.2f }, CUI_COLOR_TEXT_MUTED, more, (int)strlen(more));
        x += 26;
    }
    if (limit > x) {
        cui_push_clip(ui, (cui_rect){ x, r.y, limit - x, r.h });
        cui_draw_text(ui, CUI_FONT_BODY, f->fs, (cui_vec2){ x, ty }, CUI_COLOR_FOREGROUND, p->subject, (int)strlen(p->subject));
        cui_pop_clip(ui);
    }
    cui_pop_clip(ui);
}

/* a row's data copied into frame memory for its paint */
static void cui__log_fill(cui_ctx *ui, const cui__logf *f, const cui_log_row *row, cui__log_paint *p) {
    p->f = f;
    p->hash = cui__frame_strdup(ui, row->hash ? row->hash : "", -1);
    p->subject = cui__frame_strdup(ui, row->subject ? row->subject : "", -1);
    p->author = cui__frame_strdup(ui, row->author ? row->author : "", -1);
    p->date = cui__frame_strdup(ui, row->date ? row->date : "", -1);
    p->lane = (int16_t)row->lane;
    p->color = row->color;
    p->merge = row->merge; p->tip = row->tip; p->head = row->head;
    p->graph = f->graph_w > 0;
    int ne = row->edges ? CUI_MIN(row->nedges, CUI__LOG_EDGES) : 0;
    if (ne > 0) memcpy(p->e, row->edges, (size_t)ne * sizeof *p->e);
    p->nedges = (uint8_t)ne;
    int nc = row->chips ? CUI_MIN(row->nchips, CUI__LOG_CHIPS) : 0;
    for (int k = 0; k < nc; k++) {
        p->chips[k].text = cui__frame_strdup(ui, row->chips[k].text ? row->chips[k].text : "", -1);
        p->chips[k].suffix = row->chips[k].suffix ? cui__frame_strdup(ui, row->chips[k].suffix, -1) : NULL;
        p->chips[k].tone = row->chips[k].tone;
        p->chips[k].strong = row->chips[k].strong;
    }
    p->nchips = (uint8_t)nc;
    int more = row->more_chips + (row->nchips > nc ? row->nchips - nc : 0);
    p->more = (uint8_t)CUI_MIN(more, 255);
}

/* Up, Down, PageUp, PageDown, Home, End, Enter */
static void cui__log_keys(cui_ctx *ui, cui__log_state *st, int *sel, int count, float view_rows, cui_commit_log_result *res) {
    int dir = 0, to = -1;
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) dir = 1;
    else if (cui_key_pressed(ui, CUI_KEY_UP)) dir = -1;
    else if (cui_key_pressed(ui, CUI_KEY_PAGE_DOWN)) dir = CUI_MAX(1, (int)view_rows);
    else if (cui_key_pressed(ui, CUI_KEY_PAGE_UP)) dir = -CUI_MAX(1, (int)view_rows);
    else if (cui_key_pressed(ui, CUI_KEY_HOME)) to = 0;
    else if (cui_key_pressed(ui, CUI_KEY_END)) to = count - 1;
    if (cui_key_pressed(ui, CUI_KEY_ENTER) && *sel >= 0 && *sel < count) res->activated = *sel;
    if ((!dir && to < 0) || count <= 0) return;
    if (to < 0) to = *sel < 0 ? (dir > 0 ? 0 : count - 1) : *sel + dir;
    to = CUI_CLAMP(to, 0, count - 1);
    if (to == *sel) return;
    *sel = to;
    res->moved = true;
    st->reveal = to;
}

cui_commit_log_result cui_commit_log_ex(cui_ctx *ui, const char *id_str, const cui_commit_log_opts *o) {
    cui_commit_log_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_commit_log_result res;
    memset(&res, 0, sizeof res);
    res.clicked = res.activated = res.context = res.selected = -1;
    cui_id id = cui_make_id(ui, id_str);
    cui__log_state *st = cui_state(ui, id ^ 0x106C0Du, sizeof *st);
    if (!st->init) { st->init = true; st->sel = -1; st->reveal = -1; }
    int *sel = o->selected ? o->selected : &st->sel;
    int count = o->count > 0 ? o->count : 0;
    if (!o->rows && !o->row_at) count = 0;
    int loaded = o->loaded > 0 && o->loaded < count ? o->loaded : count;

    cui__logf *f = cui__frame_alloc(ui, sizeof *f);
    memset(f, 0, sizeof *f);
    f->fs = o->font_size > 0 ? o->font_size : cui__fs(ui, ui->theme.text_sm);
    f->row_h = (float)(int)(f->fs * 2.0f);
    f->lanes = o->lanes > 0 ? CUI_MIN(o->lanes, CUI__LOG_MAX_LANES) : 1;
    f->graph_w = o->lanes > 0 ? 16 + (float)f->lanes * CUI__LOG_LANE_W : 0;
    bool dark = cui__log_dark(ui);
    for (int i = 0; i < 8; i++) f->pal[i] = cui__log_lane(dark, i);
    f->bg = CUI_C(ui, BACKGROUND);

    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height.kind ? o->height : CUI_FILL;
    cui__open(ui, outer);
    cui_push_id(ui, id_str);
    cui_scroll_info si = cui_scroll_get(ui, "rows");
    float view_h = si.found && si.view_h > 0 ? si.view_h : 400;
    res.rows_per_page = CUI_MAX(1, (int)(view_h / f->row_h) - 1);
    if (o->keys && !cui_wants_keyboard(ui) && !cui__keys_covered(ui)) cui__log_keys(ui, st, sel, count, (float)res.rows_per_page, &res);
    if (st->top) { cui_scroll_set(ui, "rows", 0, true); st->top = false; }
    if (st->reveal >= 0 && si.found) { cui__vcs_keep_visible(ui, st->reveal, f->row_h); st->reveal = -1; }
    int first, last;
    cui_virtual_list_begin(ui, "rows", count, f->row_h, CUI_FILL, &first, &last);
    if (last - first > CUI__LOG_MAX_ROWS) last = first + CUI__LOG_MAX_ROWS;
    res.first = first; res.last = last;
    int prefetch = o->prefetch > 0 ? o->prefetch : 300;
    if (loaded < count && last + prefetch > loaded) res.want = CUI_MIN(count, last + prefetch + 100);
    for (int i = first; i < last; i++) {
        cui__log_paint *p = cui__frame_alloc(ui, sizeof *p);
        memset(p, 0, sizeof *p);
        p->f = f;
        cui_log_row row;
        memset(&row, 0, sizeof row);
        bool have = i < loaded && (o->rows ? (row = o->rows[i], true) : o->row_at(o->user, i, &row));
        if (have && !row.blank) cui__log_fill(ui, f, &row, p);
        else p->blank = true;
        p->selected = i == *sel;
        cui_id rid = cui__hash(&i, sizeof i, id ^ 0x106C0Eu);
        cui_interact ri = cui__interact(ui, rid, 0);
        p->hovered = ri.hovered;
        if (!p->blank) {
            if (ri.clicked) {
                *sel = i; p->selected = true;
                res.clicked = i;
                if (ui->click_count >= 2) res.activated = i;
            }
            if (ri.right_clicked) {
                *sel = i; p->selected = true;
                res.context = i; res.mouse_x = ui->mx; res.mouse_y = ui->my;
            }
        }
        int node = cui__leaf(ui, rid, CUI_FILL, CUI_PX(f->row_h));
        cui__mark_interactive(ui, node, 0);
        ui->nodes[node].paint = cui__paint_log_row;
        ui->nodes[node].paint_data = p;
        ui->nodes[node].direction = CUI_DIRECTION_LTR;
    }
    cui_virtual_list_end(ui);
    cui_pop_id(ui);
    cui__close(ui);
    res.selected = *sel;
    ui->last_id = id;
    return res;
}

void cui_commit_log_reveal(cui_ctx *ui, const char *id_str, int row, bool top) {
    cui__log_state *st = cui_state(ui, cui_make_id(ui, id_str) ^ 0x106C0Du, sizeof *st);
    if (!st->init) { st->init = true; st->sel = -1; st->reveal = -1; }
    if (top) st->top = true;
    else st->reveal = row;
}

/* ======================================================================== */
/* Branch picker                                                            */
/* ======================================================================== */

typedef struct {
    char filter[128];
    int gen;
    bool was_open;
} cui__pick_state;

cui_branch_picker_result cui_branch_picker_ex(cui_ctx *ui, const char *id_str, const cui_pick_section *sections, int nsections, const cui_branch_picker_opts *o) {
    cui_branch_picker_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_branch_picker_result res = { -1, -1, NULL, false, NULL };
    if (!sections || nsections < 0) nsections = 0;
    cui_id sid = cui_make_id(ui, id_str) ^ 0xB2A9C4u;
    if (!cui_popover_is_open(ui, id_str)) {
        cui__pick_state *old = cui__state_lazy(ui, sid, sizeof *old, false);
        if (old) old->was_open = false;
        return res;
    }
    cui__pick_state *st = cui_state(ui, sid, sizeof *st);
    if (!st->was_open) { st->was_open = true; st->filter[0] = 0; st->gen++; }   /* a new field each time it opens, so it takes the keyboard */
    cui_popover_opts po;
    memset(&po, 0, sizeof po);
    po.placement = o->placement ? o->placement : CUI_PLACE_TOP_START;
    po.width = o->width > 0 ? o->width : 300;
    po.pad = 6; po.gap = 4;
    if (!cui_popover_begin(ui, id_str, &po)) return res;
    int max_shown = o->max_shown > 0 ? o->max_shown : 300;
    cui_input_opts io;
    memset(&io, 0, sizeof io);
    io.placeholder = o->placeholder ? o->placeholder : "Find a branch";
    io.autofocus = true;
    io.size = CUI_SIZE_SM;
    cui_push_id_int(ui, st->gen);
    cui_search_ex(ui, "filter", st->filter, sizeof st->filter, &io);
    cui_pop_id(ui);
    bool exact = false;
    cui_scroll_opts so;
    memset(&so, 0, sizeof so);
    so.id = "list"; so.width = CUI_FILL; so.height = CUI_PX(o->list_height > 0 ? o->list_height : 280);
    cui_scroll_begin(ui, &so);
    for (int s = 0; s < nsections; s++) {
        const cui_pick_section *sec = &sections[s];
        int shown = 0;
        for (int i = 0; i < sec->count && shown < max_shown; i++) {
            cui_pick_item it;
            memset(&it, 0, sizeof it);
            if (sec->items) it = sec->items[i];
            else if (!sec->item_at || !sec->item_at(sec->user, i, &it)) continue;
            if (!it.name) continue;
            if (!strcmp(it.name, st->filter)) exact = true;
            if (!cui__vcs_has(it.name, st->filter)) continue;
            if (shown++ == 0 && sec->title) cui_menu_label(ui, sec->title);
            const char *nm = cui__frame_strdup(ui, it.name, -1);
            cui_push_id_int(ui, s * 100000 + i);
            cui_menu_item_opts mo;
            memset(&mo, 0, sizeof mo);
            mo.checked = it.current;
            mo.icon = sec->icon;
            if (cui_menu_item_ex(ui, nm, &mo) && !it.current) {
                res.section = s; res.item = i; res.picked = nm;
                cui_popover_close_self(ui);
            }
            cui_pop_id(ui);
        }
    }
    cui_scroll_end(ui);
    cui_separator(ui);
    const char *noun = o->create_noun ? o->create_noun : "branch";
    bool named = st->filter[0] && !exact;
    const char *label = named ? cui__frame_printf(ui, "Create %s \"%.150s\"", noun, st->filter) : cui__frame_printf(ui, "Create %s...", noun);
    cui_menu_item_opts mo;
    memset(&mo, 0, sizeof mo);
    mo.icon = CUI_ICON_PLUS;
    if (cui_menu_item_ex(ui, label, &mo)) {
        res.create = true;
        res.create_name = named ? cui__frame_strdup(ui, st->filter, -1) : NULL;
        cui_popover_close_self(ui);
    }
    cui_popover_end(ui);
    return res;
}
