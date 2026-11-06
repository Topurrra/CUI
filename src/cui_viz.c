/* More charts: heatmaps, treemap, radar, funnel, sankey, waffle, split bar,
 * bar list, activity rings and scatter. Each one builds a single node whose
 * paint callback draws the marks; hover is resolved at build time from the
 * previous frame's rect using the same geometry the paint uses. */
#include "cui_internal.h"

#define cui__fs cui__fsz

typedef struct { cui_spring intro; float hover_t; int hover, shown; } cui__viz_state;

typedef struct {
    float alpha;
    float x, y;              /* anchor (pointer) */
    cui_rect bounds;         /* keep inside */
    const char *title;
    int n;
    const char *label[8], *value[8];
    cui_color sw[8];
} cui__tip;

static cui_color cui__viz_color(cui_ctx *ui, cui_color c, int k) {
    if (cui__color_set(c)) return cui_color_of(ui, c);
    return ui->theme.colors[CUI_TOKEN_SERIES_1 + (k % 6)];
}

static cui_color cui__viz_base(cui_ctx *ui, cui_color c) {
    if (cui__color_set(c)) return cui_color_of(ui, c);
    return ui->accent == CUI_ACCENT_NEUTRAL ? CUI_C(ui, SERIES_1) : CUI_C(ui, ACCENT);
}

static cui_color cui__viz_empty(cui_ctx *ui) {
    return ui->mode == CUI_THEME_DARK ? cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, FOREGROUND), 0.07f) : cui_mix(CUI_C(ui, SURFACE), CUI_C(ui, FOREGROUND), 0.055f);
}

/* one hue, light to dark (light mode) or dim to bright (dark mode) */
static cui_color cui__viz_seq(cui_ctx *ui, cui_color base, float t) {
    t = cui__clampf(t, 0, 1);
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color e = cui__viz_empty(ui);
    cui_color lo = cui_mix(e, base, dark ? 0.28f : 0.2f);
    cui_color hi = dark ? cui_mix(base, cui__rgba(255, 255, 255, 255), 0.12f) : cui_mix(base, cui__rgba(0, 0, 0, 255), 0.22f);
    cui_color mid = base;
    return t < 0.6f ? cui_mix(lo, mid, t / 0.6f) : cui_mix(mid, hi, (t - 0.6f) / 0.4f);
}

static bool cui__viz_light_text(cui_color c) {
    float l = (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
    return l < 0.52f;
}

static void cui__tip_draw(cui_ctx *ui, const cui__tip *t) {
    if (!t || t->alpha <= 0.01f || !t->title) return;
    float fs = ui->theme.text_sm, pad = 10, lh = 18;
    float fh = cui__font_line_height(ui, CUI_FONT_MEDIUM, fs);
    float w = cui__text_width(ui, CUI_FONT_MEDIUM, fs, t->title, -1, false);
    for (int k = 0; k < t->n; k++) {
        float lw = t->label[k] ? cui__text_width(ui, CUI_FONT_BODY, fs, t->label[k], -1, false) : 0;
        float vw = t->value[k] ? cui__text_width(ui, CUI_FONT_MEDIUM, fs, t->value[k], -1, true) : 0;
        w = CUI_MAX(w, (t->sw[k].a ? 14 : 0) + lw + 18 + vw);
    }
    float W = w + pad * 2, H = pad * 2 + lh * (float)(1 + t->n) + (t->n ? 2 : 0);
    float x = t->x + 14, y = t->y - H - 10;
    if (x + W > t->bounds.x + t->bounds.w + 20) x = t->x - 14 - W;
    if (y < t->bounds.y - 30) y = t->y + 18;
    cui_rect r = { x, y, W, H };
    cui__push_xform(ui, 1, 0, 0, t->alpha, 0, 0);
    cui__draw_elevation(ui, r, CUI_CORNERS(12), CUI_ELEVATION_RAISED, 1);
    cui_draw_rect_border(ui, r, CUI_CORNERS(12), CUI_C(ui, SURFACE_RAISED), ui->mode == CUI_THEME_DARK ? CUI_C(ui, BORDER) : cui__rgba(0, 0, 0, 18), 1);
    float ty = y + pad;
    cui__draw_text_line(ui, CUI_FONT_MEDIUM, fs, x + pad, ty + (lh - fh) * 0.5f, CUI_C(ui, FOREGROUND), t->title, -1, false);
    ty += lh + 2;
    for (int k = 0; k < t->n; k++) {
        float lx = x + pad;
        if (t->sw[k].a) { cui_draw_rect(ui, (cui_rect){ lx, ty + lh * 0.5f - 4, 8, 8 }, CUI_CORNERS(2.5f), t->sw[k]); lx += 14; }
        if (t->label[k]) cui__draw_text_line(ui, CUI_FONT_BODY, fs, lx, ty + (lh - fh) * 0.5f, CUI_C(ui, TEXT_SECONDARY), t->label[k], -1, false);
        if (t->value[k]) {
            float vw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, t->value[k], -1, true);
            cui__draw_text_line(ui, CUI_FONT_MEDIUM, fs, x + W - pad - vw, ty + (lh - fh) * 0.5f, CUI_C(ui, FOREGROUND), t->value[k], -1, true);
        }
        ty += lh;
    }
    cui__pop_xform(ui);
}

static void cui__tip_row(cui__tip *t, const char *label, const char *value, cui_color sw) {
    if (t->n >= 8) return;
    t->label[t->n] = label; t->value[t->n] = value; t->sw[t->n] = sw; t->n++;
}

/* Starts a chart: state, intro, hover fade. hover is the mark under the pointer this frame. */
static cui__viz_state *cui__viz_state_get(cui_ctx *ui, cui_id id) {
    cui__viz_state *st = cui_state(ui, id, sizeof *st);
    cui__spring(ui, &st->intro, 1.0f, 0.7f, 0.0f);
    return st;
}

static void cui__viz_hover(cui_ctx *ui, cui__viz_state *st, int hover) {
    st->hover = hover;
    if (hover >= 0) st->shown = hover;
    cui__tween(ui, &st->hover_t, hover >= 0 ? 1.0f : 0.0f, CUI__T_HOVER);
}

static int cui__viz_node(cui_ctx *ui, cui_id id, cui_size w, cui_size h, cui__paint_fn paint, void *data, bool hovered) {
    int n = cui__node_new(ui, id);
    cui_node *nd = &ui->nodes[n];
    nd->w = w; nd->h = h;
    nd->paint = paint; nd->paint_data = data;
    nd->flags |= CUI_NF_PAINT_AFTER;
    if (hovered) nd->flags |= CUI_NF_ON_TOP;   /* the tooltip over the neighbours */
    cui__mark_interactive(ui, n, 0);
    ui->last_id = id;
    return n;
}

/* Legend: swatch and name per entry, wrapped to width. Returns its height. */
static float cui__viz_legend(cui_ctx *ui, float x, float y, float w, const char *const *names, const cui_color *colors, int n, int hl, bool draw) {
    float fs = cui__fs(ui, ui->theme.text_xs), lh = 18, cx = x, cy = y;
    float fh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
    for (int k = 0; k < n; k++) {
        const char *nm = names[k] ? names[k] : "";
        float iw = 14 + cui__text_width(ui, CUI_FONT_BODY, fs, nm, -1, false);
        if (cx > x && cx + iw > x + w) { cx = x; cy += lh + 4; }
        if (draw) {
            float a = (hl >= 0 && hl != k) ? 0.45f : 1.0f;
            cui_draw_rect(ui, (cui_rect){ cx, cy + lh * 0.5f - 4, 8, 8 }, CUI_CORNERS(2.5f), cui_alpha(colors[k], a));
            cui__draw_text_line(ui, CUI_FONT_BODY, fs, cx + 13, cy + (lh - fh) * 0.5f, cui_alpha(CUI_C(ui, TEXT_SECONDARY), a), nm, -1, false);
        }
        cx += iw + 16;
    }
    return n ? cy - y + lh : 0;
}

static void cui__text_at(cui_ctx *ui, cui_font f, float size, float x, float y, int align, cui_color c, const char *s, bool tab) {
    /* align: 0 left, 1 center, 2 right; y is the vertical middle */
    float w = cui__text_width(ui, f, size, s, -1, tab);
    float fh = cui__font_line_height(ui, f, size);
    float tx = align == 1 ? x - w * 0.5f : align == 2 ? x - w : x;
    cui__draw_text_line(ui, f, size, tx, y - fh * 0.5f, c, s, -1, tab);
}

static float *cui__copy_floats(cui_ctx *ui, const float *v, int n) {
    float *o = cui__frame_alloc(ui, sizeof(float) * (size_t)CUI_MAX(1, n));
    if (v && n > 0) memcpy(o, v, sizeof(float) * (size_t)n);
    else if (n > 0) memset(o, 0, sizeof(float) * (size_t)n);
    return o;
}

static cui_viz_item *cui__copy_items(cui_ctx *ui, const cui_viz_item *v, int n) {
    cui_viz_item *o = cui__frame_alloc(ui, sizeof(cui_viz_item) * (size_t)CUI_MAX(1, n));
    if (v && n > 0) memcpy(o, v, sizeof(cui_viz_item) * (size_t)n);
    for (int k = 0; k < n; k++) if (!(o[k].value >= 0)) o[k].value = 0;   /* negatives and NaN count as nothing */
    return o;
}

static const char *cui__pct(cui_ctx *ui, float part, float total) {
    if (total <= 0) return "0%";
    float p = part / total * 100;
    return p < 10 && p != floorf(p) ? cui__frame_printf(ui, "%.1f%%", (double)p) : cui__frame_printf(ui, "%.0f%%", (double)p);
}

/* ------------------------------------------------------------------------ */
/* Heatmap                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct { float lw, cell, gap, gx, gy, grid_h; } cui__hm_geo;

static cui__hm_geo cui__hm_layout(cui_ctx *ui, cui_rect r, int rows, int cols, const cui_heatmap_opts *o) {
    cui__hm_geo g;
    float fs = cui__fs(ui, ui->theme.text_xs);
    g.lw = 0;
    if (o->row_labels)
        for (int i = 0; i < rows; i++) g.lw = CUI_MAX(g.lw, cui__text_width(ui, CUI_FONT_BODY, fs, o->row_labels[i] ? o->row_labels[i] : "", -1, false) + 12);
    g.gap = 3;
    float avail = (r.w > 0 ? r.w : 480) - g.lw;
    g.cell = o->cell > 0 ? o->cell : cui__clampf((avail - g.gap * (float)(cols - 1)) / (float)CUI_MAX(1, cols), 12, 56);
    if (o->cell <= 0 && r.w > 0 && g.lw + cols * (g.cell + g.gap) - g.gap > r.w + 0.5f) {
        /* a narrow container: tighter gaps and smaller cells rather than spilling out */
        g.gap = 2;
        g.cell = cui__clampf((avail - g.gap * (float)(cols - 1)) / (float)CUI_MAX(1, cols), 6, 12);
    }
    g.gx = r.x + g.lw; g.gy = r.y;
    g.grid_h = rows * (g.cell + g.gap) - g.gap;
    return g;
}

typedef struct {
    float *v; int rows, cols; float lo, hi;
    cui_heatmap_opts o; cui_color base;
    float intro; int hover; cui__tip tip;
} cui__hm_paint;

static void cui__paint_heatmap(cui_ctx *ui, cui_node *n, void *data) {
    cui__hm_paint *p = data;
    cui__hm_geo g = cui__hm_layout(ui, n->rect, p->rows, p->cols, &p->o);
    float fs = cui__fs(ui, ui->theme.text_xs);
    float rad = g.cell >= 18 ? 4 : 2.5f;
    for (int i = 0; i < p->rows; i++) {
        if (p->o.row_labels && p->o.row_labels[i])
            cui__text_at(ui, CUI_FONT_BODY, fs, g.gx - 10, g.gy + i * (g.cell + g.gap) + g.cell * 0.5f, 2, CUI_C(ui, TEXT_SECONDARY), p->o.row_labels[i], false);
        for (int j = 0; j < p->cols; j++) {
            float t = (p->v[i * p->cols + j] - p->lo) / (p->hi - p->lo);
            float a = cui__clampf(p->intro * 1.8f - (float)j / (float)p->cols * 0.8f, 0, 1);
            cui_rect c = { g.gx + j * (g.cell + g.gap), g.gy + i * (g.cell + g.gap), g.cell, g.cell };
            cui_draw_rect(ui, c, CUI_CORNERS(rad), cui_mix(cui__viz_empty(ui), cui__viz_seq(ui, p->base, t), a));
        }
    }
    if (p->hover >= 0 && p->tip.alpha > 0.01f) {
        int i = p->hover / p->cols, j = p->hover % p->cols;
        cui_rect c = { g.gx + j * (g.cell + g.gap) - 1.5f, g.gy + i * (g.cell + g.gap) - 1.5f, g.cell + 3, g.cell + 3 };
        cui_draw_rect_border(ui, c, CUI_CORNERS(rad + 1.5f), cui__rgba(0, 0, 0, 0), cui_alpha(CUI_C(ui, FOREGROUND), p->tip.alpha), 1.5f);
    }
    float by = g.gy + g.grid_h;
    if (p->o.col_labels) {
        float last = -1e9f;
        for (int j = 0; j < p->cols; j++) {
            const char *s = p->o.col_labels[j];
            if (!s) continue;
            float cx = g.gx + j * (g.cell + g.gap) + g.cell * 0.5f, w = cui__text_width(ui, CUI_FONT_BODY, fs, s, -1, false);
            if (cx - w * 0.5f < last + 6) continue;
            cui__text_at(ui, CUI_FONT_BODY, fs, cx, by + 13, 1, CUI_C(ui, TEXT_MUTED), s, false);
            last = cx + w * 0.5f;
        }
        by += 22;
    }
    /* scale */
    const char *lo = cui__fmt_value(ui, p->o.format, p->lo), *hi = cui__fmt_value(ui, p->o.format, p->hi);
    float hiw = cui__text_width(ui, CUI_FONT_BODY, fs, hi, -1, true);
    float sw = 120, sx = g.gx + p->cols * (g.cell + g.gap) - g.gap - hiw - 8 - sw, sy = by + 12;
    for (int k = 0; k < 24; k++) {
        cui_rect c = { sx + sw * k / 24.0f, sy, sw / 24.0f + 0.5f, 8 };
        cui_corners cr = CUI_CORNERS(0);
        if (k == 0) cr = (cui_corners){ 4, 0, 0, 4 };
        if (k == 23) cr = (cui_corners){ 0, 4, 4, 0 };
        cui_draw_rect(ui, c, cr, cui__viz_seq(ui, p->base, (k + 0.5f) / 24.0f));
    }
    cui__text_at(ui, CUI_FONT_BODY, fs, sx - 8, sy + 4, 2, CUI_C(ui, TEXT_MUTED), lo, true);
    cui__text_at(ui, CUI_FONT_BODY, fs, sx + sw + 8, sy + 4, 0, CUI_C(ui, TEXT_MUTED), hi, true);
    cui__tip_draw(ui, &p->tip);
}

int cui_heatmap_ex(cui_ctx *ui, const char *id_str, const float *values, int rows, int cols, const cui_heatmap_opts *o) {
    cui_heatmap_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    rows = CUI_MAX(0, rows); cols = CUI_MAX(0, cols);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui__hm_geo g = cui__hm_layout(ui, pr, rows, cols, o);
    float lo = o->min, hi = o->max;
    if (!(o->max > o->min)) {
        lo = 1e30f; hi = -1e30f;
        for (int k = 0; k < rows * cols; k++) { lo = CUI_MIN(lo, values[k]); hi = CUI_MAX(hi, values[k]); }
        if (rows * cols == 0) { lo = 0; hi = 1; }
        if (hi <= lo) hi = lo + 1;
    }
    int hover = -1;
    if (r.hovered && rows && cols) {
        int j = (int)floorf((ui->mx - g.gx) / (g.cell + g.gap)), i = (int)floorf((ui->my - g.gy) / (g.cell + g.gap));
        if (i >= 0 && i < rows && j >= 0 && j < cols) hover = i * cols + j;
    }
    cui__viz_hover(ui, st, hover);
    cui__hm_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->v = cui__copy_floats(ui, values, rows * cols);
    p->rows = rows; p->cols = cols; p->lo = lo; p->hi = hi; p->o = *o;
    p->base = cui__viz_base(ui, o->color);
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f ? st->shown : -1;
    if (p->hover >= rows * cols) p->hover = -1;
    if (p->hover >= 0) {
        int i = p->hover / cols, j = p->hover % cols;
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        const char *rl = o->row_labels && o->row_labels[i] ? o->row_labels[i] : cui__frame_printf(ui, "Row %d", i + 1);
        const char *cl = o->col_labels && o->col_labels[j] ? o->col_labels[j] : cui__frame_printf(ui, "%d", j + 1);
        p->tip.title = cui__frame_printf(ui, "%s \xc2\xb7 %s", rl, cl);
        cui__tip_row(&p->tip, "Value", cui__fmt_value(ui, o->format, values[p->hover]), cui__viz_seq(ui, p->base, (values[p->hover] - lo) / (hi - lo)));
    }
    float h = g.grid_h + (o->col_labels ? 22 : 0) + 30;
    cui_size w = o->width.kind ? o->width : (o->cell > 0 ? CUI_PX(g.lw + cols * (g.cell + g.gap) - g.gap) : CUI_FILL);
    cui__viz_node(ui, id, w, CUI_PX(h), cui__paint_heatmap, p, hover >= 0);
    if (hover >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Calendar heatmap                                                         */
/* ------------------------------------------------------------------------ */

typedef struct {
    float *v; int days, wd0, weeks; int base_day; float maxv, total;
    float cell, gap; cui_color base; cui_calendar_heatmap_opts o;
    float intro; int hover; cui__tip tip;
} cui__cal_paint;

static const char *cui__mon3[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *cui__wd3[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };

static int cui__cal_level(float v, float maxv) {
    if (v <= 0 || maxv <= 0) return 0;
    int l = 1 + (int)floorf(v / maxv * 4 - 0.0001f);
    return CUI_CLAMP(l, 1, 4);
}

static cui_color cui__cal_color(cui_ctx *ui, cui_color base, int level) {
    if (level == 0) return cui__viz_empty(ui);
    static const float t[5] = { 0, 0.08f, 0.38f, 0.68f, 1.0f };
    return cui__viz_seq(ui, base, t[level]);
}

#define CUI__CAL_LW 32.0f
#define CUI__CAL_TOP 20.0f

static void cui__paint_calendar(cui_ctx *ui, cui_node *n, void *data) {
    cui__cal_paint *p = data;
    cui_rect r = n->rect;
    float fs = cui__fs(ui, ui->theme.text_xs), step = p->cell + p->gap;
    float gx = r.x + CUI__CAL_LW, gy = r.y + CUI__CAL_TOP;
    float rad = p->cell >= 12 ? 3 : 2;
    for (int k = 0; k < 5; k += 2) cui__text_at(ui, CUI_FONT_BODY, fs, r.x, gy + k * step + p->cell * 0.5f, 0, CUI_C(ui, TEXT_MUTED), cui__wd3[k], false);
    float last_end = -1e9f;
    for (int w = 0; w < p->weeks; w++) {
        int first = w * 7 - p->wd0;   /* day index of this column's Monday */
        cui_date dd = cui_date_from_days(p->base_day + CUI_MAX(first, 0));
        bool starts = first <= 0 ? (w == 0) : dd.day <= 7;
        float x = gx + w * step;
        if (starts && x > last_end + 4 && w < p->weeks - 1) {
            cui__text_at(ui, CUI_FONT_BODY, fs, x, r.y + 8, 0, CUI_C(ui, TEXT_MUTED), cui__mon3[dd.month - 1], false);
            last_end = x + cui__text_width(ui, CUI_FONT_BODY, fs, cui__mon3[dd.month - 1], -1, false);
        }
        for (int k = 0; k < 7; k++) {
            int di = first + k;
            if (di < 0 || di >= p->days) continue;
            int lv = cui__cal_level(p->v[di], p->maxv);
            float a = cui__clampf(p->intro * 1.8f - (float)w / (float)p->weeks * 0.8f, 0, 1);
            cui_rect c = { x, gy + k * step, p->cell, p->cell };
            cui_draw_rect(ui, c, CUI_CORNERS(rad), cui_mix(cui__viz_empty(ui), cui__cal_color(ui, p->base, lv), a));
            if (di == p->hover && p->tip.alpha > 0.01f)
                cui_draw_rect_border(ui, (cui_rect){ c.x - 1.5f, c.y - 1.5f, c.w + 3, c.h + 3 }, CUI_CORNERS(rad + 1.5f), cui__rgba(0, 0, 0, 0),
                                     cui_alpha(CUI_C(ui, FOREGROUND), p->tip.alpha), 1.5f);
        }
    }
    /* legend */
    float ly = gy + 7 * step + 12;
    float lx = gx + p->weeks * step - p->gap;
    float sz = CUI_MIN(p->cell, 12);
    float more_w = cui__text_width(ui, CUI_FONT_BODY, fs, "More", -1, false);
    float x = lx - more_w;
    cui__text_at(ui, CUI_FONT_BODY, fs, x, ly + sz * 0.5f, 0, CUI_C(ui, TEXT_MUTED), "More", false);
    x -= 6;
    for (int lv = 4; lv >= 0; lv--) { x -= sz; cui_draw_rect(ui, (cui_rect){ x, ly, sz, sz }, CUI_CORNERS(rad), cui__cal_color(ui, p->base, lv)); x -= 3; }
    cui__text_at(ui, CUI_FONT_BODY, fs, x - 3, ly + sz * 0.5f, 2, CUI_C(ui, TEXT_MUTED), "Less", false);
    if (p->o.unit) {
        const char *s = cui__frame_printf(ui, "%s %s", cui__fmt_value(ui, p->o.format, p->total), p->o.unit);
        cui__text_at(ui, CUI_FONT_BODY, fs, gx, ly + sz * 0.5f, 0, CUI_C(ui, TEXT_SECONDARY), s, true);
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_calendar_heatmap_ex(cui_ctx *ui, const char *id_str, cui_date start, const float *values, int days, const cui_calendar_heatmap_opts *o) {
    cui_calendar_heatmap_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    days = CUI_MAX(0, days);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    int base = cui_date_to_days(start);
    int wd0 = ((base + 3) % 7 + 7) % 7;   /* 0 = Monday */
    int weeks = (wd0 + days + 6) / 7;
    float cell = o->cell > 0 ? o->cell : 11, gap = cell >= 10 ? 3 : 2, step = cell + gap;
    float full_w = CUI__CAL_LW + weeks * step - gap;
    if (pr.w > 0 && pr.w + 0.5f < full_w) {
        /* narrower than the whole range: show the most recent weeks that fit */
        int fit = CUI_MAX(1, (int)floorf((pr.w - CUI__CAL_LW + gap) / step));
        if (fit < weeks) {
            int skip = (weeks - fit) * 7 - wd0;
            values += skip; days -= skip; base += skip;
            wd0 = 0; weeks = fit;
        }
    }
    float maxv = 0, total = 0;
    for (int k = 0; k < days; k++) { maxv = CUI_MAX(maxv, values[k]); total += values[k] > 0 ? values[k] : 0; }
    int hover = -1;
    if (r.hovered) {
        int w = (int)floorf((ui->mx - pr.x - CUI__CAL_LW) / step), k = (int)floorf((ui->my - pr.y - CUI__CAL_TOP) / step);
        if (w >= 0 && w < weeks && k >= 0 && k < 7) {
            int di = w * 7 - wd0 + k;
            if (di >= 0 && di < days) hover = di;
        }
    }
    cui__viz_hover(ui, st, hover);
    cui__cal_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->v = cui__copy_floats(ui, values, days);
    p->days = days; p->wd0 = wd0; p->weeks = weeks; p->base_day = base; p->maxv = maxv; p->total = total;
    p->cell = cell; p->gap = gap; p->base = cui__viz_base(ui, o->color); p->o = *o;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < days ? st->shown : -1;
    if (p->hover >= 0) {
        cui_date dd = cui_date_from_days(base + p->hover);
        int wd = ((base + p->hover + 3) % 7 + 7) % 7;
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = cui__frame_printf(ui, "%s, %s %d, %d", cui__wd3[wd], cui__mon3[dd.month - 1], dd.day, dd.year);
        cui__tip_row(&p->tip, o->unit ? o->unit : "Value", cui__fmt_value(ui, o->format, values[p->hover]),
                     cui__cal_color(ui, p->base, cui__cal_level(values[p->hover], maxv)));
    }
    float h = CUI__CAL_TOP + 7 * step - gap + 12 + CUI_MIN(cell, 12) + 2;
    int node = cui__viz_node(ui, id, CUI_FIT, CUI_PX(h), cui__paint_calendar, p, hover >= 0);
    cui__soft_width(&ui->nodes[node], CUI_FIT, full_w);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Treemap                                                                  */
/* ------------------------------------------------------------------------ */

static float cui__sq_worst(const float *v, int n, float sum, float side) {
    float mx = 0, mn = 1e30f;
    for (int k = 0; k < n; k++) { mx = CUI_MAX(mx, v[k]); mn = CUI_MIN(mn, v[k]); }
    float s2 = side * side, sum2 = sum * sum;
    if (mn <= 0 || sum2 <= 0) return 1e30f;
    return CUI_MAX(s2 * mx / sum2, sum2 / (s2 * mn));
}

/* squarified layout of areas (sorted, largest first) into r */
static void cui__squarify(const float *v, int n, cui_rect r, cui_rect *out) {
    int i = 0;
    while (i < n) {
        float side = CUI_MIN(r.w, r.h);
        if (side <= 0) { for (; i < n; i++) out[i] = (cui_rect){ r.x, r.y, 0, 0 }; return; }
        int j = i;
        float sum = 0, worst = 1e30f;
        while (j < n) {
            float s2 = sum + v[j];
            float w = cui__sq_worst(v + i, j - i + 1, s2, side);
            if (j > i && w > worst) break;
            worst = w; sum = s2; j++;
        }
        float thick = sum / side;
        if (r.w >= r.h) {
            float y = r.y;
            for (int k = i; k < j; k++) { float h = thick > 0 ? v[k] / thick : 0; out[k] = (cui_rect){ r.x, y, thick, h }; y += h; }
            r.x += thick; r.w -= thick;
        } else {
            float x = r.x;
            for (int k = i; k < j; k++) { float w = thick > 0 ? v[k] / thick : 0; out[k] = (cui_rect){ x, r.y, w, thick }; x += w; }
            r.y += thick; r.h -= thick;
        }
        i = j;
    }
}

typedef struct { cui_viz_item *items; int count; int *order; float total; const char *format; float intro; int hover; cui__tip tip; } cui__tm_paint;

/* explicit colors stay; the rest are shades of one hue, darker for more */
static cui_color cui__tm_color(cui_ctx *ui, const cui_viz_item *items, const int *order, int idx) {
    if (cui__color_set(items[idx].color)) return cui_color_of(ui, items[idx].color);
    float top = items[order[0]].value;
    float t = top > 0 ? items[idx].value / top : 0;
    return cui__viz_seq(ui, cui__viz_base(ui, (cui_color){ 0 }), 0.12f + 0.88f * t);
}

static cui_rect *cui__tm_layout(cui_ctx *ui, cui_rect r, const cui_viz_item *items, const int *order, int n, float total) {
    float *a = cui__frame_alloc(ui, sizeof(float) * (size_t)CUI_MAX(1, n));
    cui_rect *out = cui__frame_alloc(ui, sizeof(cui_rect) * (size_t)CUI_MAX(1, n));
    float area = r.w * r.h;
    int m = 0;
    for (int k = 0; k < n; k++) { a[k] = total > 0 ? items[order[k]].value / total * area : 0; if (a[k] > 0) m = k + 1; }
    cui__squarify(a, m, r, out);
    for (int k = m; k < n; k++) out[k] = (cui_rect){ r.x, r.y, 0, 0 };
    return out;   /* indexed by rank */
}

static void cui__paint_treemap(cui_ctx *ui, cui_node *n, void *data) {
    cui__tm_paint *p = data;
    cui_rect *rc = cui__tm_layout(ui, n->rect, p->items, p->order, p->count, p->total);
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    for (int k = 0; k < p->count; k++) {
        int idx = p->order[k];
        cui_rect c = { rc[k].x + 1, rc[k].y + 1, rc[k].w - 2, rc[k].h - 2 };
        if (c.w <= 0.5f || c.h <= 0.5f) continue;
        float a = cui__clampf(p->intro * 1.6f - (float)k / (float)p->count * 0.6f, 0, 1);
        float s = 0.92f + 0.08f * a;
        cui_rect sc = { c.x + c.w * (1 - s) * 0.5f, c.y + c.h * (1 - s) * 0.5f, c.w * s, c.h * s };
        cui_color col = cui__tm_color(ui, p->items, p->order, idx);
        bool hov = idx == p->hover;
        if (p->hover >= 0 && p->tip.alpha > 0.01f && !hov) col = cui_mix(col, CUI_C(ui, SURFACE), 0.35f * p->tip.alpha);
        float rad = CUI_MIN(8.0f, CUI_MIN(c.w, c.h) * 0.25f);
        cui_draw_rect(ui, sc, CUI_CORNERS(rad), cui_alpha(col, a));
        if (c.w > 56 && c.h > 30 && p->items[idx].label) {
            cui_color tc = cui__viz_light_text(col) ? cui__rgba(255, 255, 255, 255) : cui__rgba(20, 20, 20, 255);
            cui_push_clip(ui, c);
            float lh = cui__font_line_height(ui, CUI_FONT_MEDIUM, fs);
            cui__draw_text_line(ui, CUI_FONT_MEDIUM, fs, c.x + 10, c.y + 8, cui_alpha(tc, a), p->items[idx].label, -1, false);
            if (c.h > 48)
                cui__draw_text_line(ui, CUI_FONT_BODY, fx, c.x + 10, c.y + 8 + lh, cui_alpha(tc, a * 0.75f), cui__fmt_value(ui, p->format, p->items[idx].value), -1, true);
            cui_pop_clip(ui);
        }
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_treemap_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *items_in, int count, const cui_treemap_opts *o) {
    cui_treemap_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui_viz_item *items = cui__copy_items(ui, items_in, count);
    int *order = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, count));
    float total = 0;
    for (int k = 0; k < count; k++) { order[k] = k; total += items[k].value; }
    for (int a = 1; a < count; a++) {   /* largest first, stable */
        int v = order[a], b = a - 1;
        while (b >= 0 && items[order[b]].value < items[v].value) { order[b + 1] = order[b]; b--; }
        order[b + 1] = v;
    }
    int hover = -1;
    if (r.hovered && pr.w > 0) {
        cui_rect *rc = cui__tm_layout(ui, pr, items, order, count, total);
        for (int k = 0; k < count; k++)
            if (ui->mx >= rc[k].x && ui->mx < rc[k].x + rc[k].w && ui->my >= rc[k].y && ui->my < rc[k].y + rc[k].h) hover = order[k];
    }
    cui__viz_hover(ui, st, hover);
    cui__tm_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->items = items; p->count = count; p->order = order; p->total = total; p->format = o->format;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < count ? st->shown : -1;
    if (p->hover >= 0) {
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = items[p->hover].label ? items[p->hover].label : "";
        cui__tip_row(&p->tip, "Value", cui__fmt_value(ui, o->format, items[p->hover].value), cui__tm_color(ui, items, order, p->hover));
        cui__tip_row(&p->tip, "Share", cui__pct(ui, items[p->hover].value, total), cui__rgba(0, 0, 0, 0));
    }
    cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(o->height > 0 ? o->height : 300), cui__paint_treemap, p, hover >= 0);
    if (hover >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Radar                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct {
    cui_series *series; int count; const char **axes; int na;
    float maxv, step; cui_radar_opts o; float size;
    float intro; int hover; cui__tip tip; float legend_h;
} cui__radar_paint;

static void cui__paint_radar(cui_ctx *ui, cui_node *n, void *data) {
    cui__radar_paint *p = data;
    cui_rect r = n->rect;
    float R = p->size * 0.5f;
    cui_vec2 c = { r.x + r.w * 0.5f, r.y + 26 + R };
    float fs = cui__fs(ui, ui->theme.text_xs);
    int na = p->na;
    if (na < 3) return;
    cui_vec2 *dir = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)na);
    for (int i = 0; i < na; i++) { float a = -CUI_PI * 0.5f + 2 * CUI_PI * i / na; dir[i].x = cosf(a); dir[i].y = sinf(a); }
    /* grid */
    int rings = (int)lroundf(p->maxv / p->step);
    rings = CUI_CLAMP(rings, 1, 8);
    cui_vec2 *pts = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)na);
    cui_rect grid_labels[8];
    /* ring values go on the spoke where no series point sits near a ring, so numbers and data never collide */
    int lab_spoke = 0;
    float best = 1e9f;
    for (int i = 0; i < na; i++) {
        float cost = 0;
        for (int k = 0; k < p->count; k++) {
            float v = i < p->series[k].count ? p->series[k].values[i] / p->maxv : 0;
            for (int g = 1; g < rings; g++) cost += CUI_MAX(0.0f, 1.0f - fabsf(v - (float)g / rings) / 0.2f);
        }
        cost += fabsf(dir[i].x) * 0.6f;   /* numbers read best stacked on a vertical spoke */
        if (cost < best - 1e-4f) { best = cost; lab_spoke = i; }
    }
    for (int g = 1; g <= rings; g++) {
        float rr = R * g / rings;
        for (int i = 0; i < na; i++) { pts[i].x = c.x + dir[i].x * rr; pts[i].y = c.y + dir[i].y * rr; }
        cui_draw_polyline(ui, pts, na, 1, g == rings ? CUI_C(ui, BORDER) : CUI_C(ui, BORDER_SUBTLE), true);
        const char *gl = cui__fmt_value(ui, p->o.format, p->step * g);
        float gw = cui__text_width(ui, CUI_FONT_BODY, fs * 0.92f, gl, -1, true);
        cui_vec2 at = { c.x + dir[lab_spoke].x * rr - gw * 0.5f, c.y + dir[lab_spoke].y * rr };
        grid_labels[g - 1] = (cui_rect){ at.x, at.y, gw, 0 };
    }
    for (int i = 0; i < na; i++) {
        bool h = i == p->hover && p->tip.alpha > 0.01f;
        cui_draw_line(ui, c, (cui_vec2){ c.x + dir[i].x * R, c.y + dir[i].y * R }, h ? 1.5f : 1, h ? cui_mix(CUI_C(ui, BORDER_SUBTLE), CUI_C(ui, BORDER_STRONG), p->tip.alpha) : CUI_C(ui, BORDER_SUBTLE));
        float lx = c.x + dir[i].x * (R + 14), ly = c.y + dir[i].y * (R + 12);
        int al = dir[i].x > 0.3f ? 0 : dir[i].x < -0.3f ? 2 : 1;
        if (al == 1) ly += dir[i].y * 4;
        cui__text_at(ui, h ? CUI_FONT_MEDIUM : CUI_FONT_BODY, fs, lx, ly, al, h ? CUI_C(ui, FOREGROUND) : CUI_C(ui, TEXT_SECONDARY), p->axes[i] ? p->axes[i] : "", false);
    }
    /* ring values sit on the grid, under the data: the inner rings only, the outer one meets the axis names */
    for (int g = 1; g < rings || (rings == 1 && g == 1); g++) {
        cui_rect q = grid_labels[g - 1];
        cui_draw_rect(ui, (cui_rect){ q.x - 3, q.y - 7, q.w + 6, 14 }, CUI_CORNERS(7), CUI_C(ui, SURFACE));
        cui__text_at(ui, CUI_FONT_BODY, fs * 0.92f, q.x, q.y, 0, CUI_C(ui, TEXT_MUTED), cui__fmt_value(ui, p->o.format, p->step * g), true);
    }
    /* series */
    bool dark = ui->mode == CUI_THEME_DARK;
    for (int k = 0; k < p->count; k++) {
        cui_series *s = &p->series[k];
        cui_color col = cui__viz_color(ui, s->color, k);
        for (int i = 0; i < na; i++) {
            float v = i < s->count ? cui__clampf(s->values[i] / p->maxv, 0, 1.15f) : 0;
            float e = cui__clampf(p->intro, 0, 1);
            pts[i].x = c.x + dir[i].x * R * v * e; pts[i].y = c.y + dir[i].y * R * v * e;
        }
        cui_cvert *vv = cui__frame_alloc(ui, sizeof(cui_cvert) * (size_t)(na + 1));
        uint32_t *idx = cui__frame_alloc(ui, sizeof(uint32_t) * (size_t)na * 3);
        /* only the last series is filled: stacked translucent fills mix into a muddy third color */
        cui_color fill = cui_alpha(col, (k == p->count - 1 ? (p->count > 1 ? 0.14f : 0.18f) : 0.0f) * (dark ? 1.3f : 1.0f));
        vv[0].x = c.x; vv[0].y = c.y; vv[0].c = fill;
        for (int i = 0; i < na; i++) { vv[i + 1].x = pts[i].x; vv[i + 1].y = pts[i].y; vv[i + 1].c = fill; }
        for (int i = 0; i < na; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = (uint32_t)(i + 1); idx[i * 3 + 2] = (uint32_t)((i + 1) % na + 1); }
        cui__draw_triangles(ui, vv, na + 1, idx, na * 3);
        cui_draw_polyline(ui, pts, na, 2, col, true);
        for (int i = 0; i < na; i++) {
            bool h = i == p->hover && p->tip.alpha > 0.01f;
            float rr = h ? 4.5f : 3;
            cui_draw_circle(ui, pts[i], rr + 1.5f, CUI_C(ui, SURFACE));
            cui_draw_circle(ui, pts[i], rr, col);
        }
    }
    if (p->count >= 2 && !p->o.no_legend) {
        const char **names = cui__frame_alloc(ui, sizeof(char *) * (size_t)p->count);
        cui_color *cols = cui__frame_alloc(ui, sizeof(cui_color) * (size_t)p->count);
        float lw = 0;
        for (int k = 0; k < p->count; k++) { names[k] = p->series[k].name; cols[k] = cui__viz_color(ui, p->series[k].color, k); }
        for (int k = 0; k < p->count; k++) lw += 14 + cui__text_width(ui, CUI_FONT_BODY, fs, names[k] ? names[k] : "", -1, false) + 16;
        lw = CUI_MIN(lw - 16, r.w);
        cui__viz_legend(ui, r.x + (r.w - lw) * 0.5f, c.y + R + 34, lw + 1, names, cols, p->count, -1, true);
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_radar_chart_ex(cui_ctx *ui, const char *id_str, const cui_series *series, int count, const char *const *axes, int na, const cui_radar_opts *o) {
    cui_radar_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count); na = CUI_MAX(0, na);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    float size = o->size > 0 ? o->size : 260;
    float maxv = 0;
    for (int k = 0; k < count; k++)
        for (int i = 0; i < series[k].count && i < na; i++) maxv = CUI_MAX(maxv, series[k].values[i]);
    float lo, hi, step;
    if (o->max > 0) { hi = o->max; step = o->max / 4; }
    else { cui__nice_range(0, maxv > 0 ? maxv : 1, 4, &lo, &hi, &step); }
    int hover = -1;
    if (r.hovered && na >= 3) {
        float R = size * 0.5f;
        float cx = pr.x + pr.w * 0.5f, cy = pr.y + 26 + R;
        float dx = ui->mx - cx, dy = ui->my - cy;
        if (dx * dx + dy * dy < (R + 36) * (R + 36)) {
            float a = atan2f(dy, dx) + CUI_PI * 0.5f;
            if (a < 0) a += 2 * CUI_PI;
            hover = (int)lroundf(a / (2 * CUI_PI) * na) % na;
        }
    }
    cui__viz_hover(ui, st, hover);
    cui__radar_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->series = cui__frame_alloc(ui, sizeof(cui_series) * (size_t)CUI_MAX(1, count));
    for (int k = 0; k < count; k++) { p->series[k] = series[k]; p->series[k].values = cui__copy_floats(ui, series[k].values, series[k].count); }
    p->axes = cui__frame_alloc(ui, sizeof(char *) * (size_t)CUI_MAX(1, na));
    for (int i = 0; i < na; i++) p->axes[i] = axes ? axes[i] : NULL;
    p->count = count; p->na = na; p->maxv = hi; p->step = step; p->o = *o; p->size = size;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < na ? st->shown : -1;
    if (p->hover >= 0) {
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = axes && axes[p->hover] ? axes[p->hover] : "";
        for (int k = 0; k < count; k++)
            cui__tip_row(&p->tip, series[k].name ? series[k].name : "", p->hover < series[k].count ? cui__fmt_value(ui, o->format, series[k].values[p->hover]) : "-",
                         cui__viz_color(ui, series[k].color, k));
    }
    float h = size + 52 + (count >= 2 && !o->no_legend ? 30 : 0);
    cui__viz_node(ui, id, CUI_PX(size + 180), CUI_PX(h), cui__paint_radar, p, hover >= 0);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Funnel                                                                   */
/* ------------------------------------------------------------------------ */

#define CUI__FN_ROW 40.0f
#define CUI__FN_LINK 14.0f

typedef struct { cui_viz_item *items; int count; float maxv; cui_funnel_opts o; cui_color base; float intro; int hover; cui__tip tip; float lw; } cui__fn_paint;

static void cui__paint_funnel(cui_ctx *ui, cui_node *n, void *data) {
    cui__fn_paint *p = data;
    cui_rect r = n->rect;
    float fs = cui__fs(ui, ui->theme.text_sm), fx = cui__fs(ui, ui->theme.text_xs);
    float rw = 64, bx = r.x + p->lw, bw = r.w - p->lw - rw;
    float cx = bx + bw * 0.5f;
    float prev_x0 = 0, prev_x1 = 0, prev_y = 0;
    for (int k = 0; k < p->count; k++) {
        float y = r.y + k * (CUI__FN_ROW + CUI__FN_LINK);
        float e = cui__clampf(p->intro * 1.6f - (float)k / (float)p->count * 0.6f, 0, 1);
        float frac = p->maxv > 0 ? p->items[k].value / p->maxv : 0;
        float w = CUI_MAX(6.0f, bw * frac * e);
        float x0 = cx - w * 0.5f, x1 = cx + w * 0.5f;
        cui_color col = cui__color_set(p->items[k].color) ? cui_color_of(ui, p->items[k].color) : cui_mix(p->base, CUI_C(ui, SURFACE), 0.13f * (float)k);
        bool hov = k == p->hover && p->tip.alpha > 0.01f;
        if (p->hover >= 0 && !hov) col = cui_mix(col, CUI_C(ui, SURFACE), 0.3f * p->tip.alpha);
        if (k > 0) {
            cui_vec2 q[4] = { { prev_x0, prev_y }, { prev_x1, prev_y }, { x1, y }, { x0, y } };
            cui_draw_polygon(ui, q, 4, cui_alpha(col, 0.16f));
        }
        cui_draw_rect(ui, (cui_rect){ x0, y, w, CUI__FN_ROW }, CUI_CORNERS(8), col);
        const char *vs = cui__fmt_value(ui, p->o.format, p->items[k].value);
        float vw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, vs, -1, true);
        if (vw + 20 < w) cui__text_at(ui, CUI_FONT_MEDIUM, fs, cx, y + CUI__FN_ROW * 0.5f, 1, cui__viz_light_text(col) ? cui__rgba(255, 255, 255, 255) : cui__rgba(20, 20, 20, 255), vs, true);
        else cui__text_at(ui, CUI_FONT_MEDIUM, fs, x1 + 8, y + CUI__FN_ROW * 0.5f, 0, CUI_C(ui, FOREGROUND), vs, true);
        cui__text_at(ui, hov ? CUI_FONT_MEDIUM : CUI_FONT_BODY, fs, r.x, y + CUI__FN_ROW * 0.5f, 0, CUI_C(ui, FOREGROUND), p->items[k].label ? p->items[k].label : "", false);
        const char *pc = cui__pct(ui, p->items[k].value, p->items[0].value);
        /* the second stage's step and overall rates are the same number: say it once */
        cui__text_at(ui, CUI_FONT_MEDIUM, fs, r.x + r.w, y + CUI__FN_ROW * 0.5f - (k > 1 ? 7 : 0), 2, CUI_C(ui, FOREGROUND), pc, true);
        if (k > 1) cui__text_at(ui, CUI_FONT_BODY, fx, r.x + r.w, y + CUI__FN_ROW * 0.5f + 9, 2, CUI_C(ui, TEXT_MUTED),
                                cui__frame_printf(ui, "%s of previous", cui__pct(ui, p->items[k].value, p->items[k - 1].value)), true);   /* step conversion, said plainly */
        prev_x0 = x0; prev_x1 = x1; prev_y = y + CUI__FN_ROW;
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_funnel_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *stages, int count, const cui_funnel_opts *o) {
    cui_funnel_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui_viz_item *items = cui__copy_items(ui, stages, count);
    float maxv = 0, lw = 0, fs = cui__fs(ui, ui->theme.text_sm);
    for (int k = 0; k < count; k++) {
        maxv = CUI_MAX(maxv, items[k].value);
        lw = CUI_MAX(lw, cui__text_width(ui, CUI_FONT_MEDIUM, fs, items[k].label ? items[k].label : "", -1, false));
    }
    lw += 20;
    int hover = -1;
    if (r.hovered) {
        int k = (int)floorf((ui->my - pr.y + CUI__FN_LINK * 0.5f) / (CUI__FN_ROW + CUI__FN_LINK));
        if (k >= 0 && k < count) hover = k;
    }
    cui__viz_hover(ui, st, hover);
    cui__fn_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->items = items; p->count = count; p->maxv = maxv; p->o = *o; p->base = cui__viz_base(ui, o->color); p->lw = lw;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < count ? st->shown : -1;
    if (p->hover >= 0) {
        int k = p->hover;
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = items[k].label ? items[k].label : "";
        cui__tip_row(&p->tip, "Count", cui__fmt_value(ui, o->format, items[k].value), cui__rgba(0, 0, 0, 0));
        cui__tip_row(&p->tip, "Of the first stage", cui__pct(ui, items[k].value, items[0].value), cui__rgba(0, 0, 0, 0));
        if (k > 0) cui__tip_row(&p->tip, "From the stage before", cui__pct(ui, items[k].value, items[k - 1].value), cui__rgba(0, 0, 0, 0));
    }
    float h = count * CUI__FN_ROW + CUI_MAX(0, count - 1) * CUI__FN_LINK;
    cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(h), cui__paint_funnel, p, hover >= 0);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Sankey                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct {
    int nn, nl;
    const char **names;
    cui_sankey_link *links;
    cui_rect *node;          /* node rects */
    float *sy, *ty;          /* each link's band top at the source and the target */
    float k;                 /* points per unit */
    float *in, *out;
    int *col; int maxcol;
    int *color;              /* palette slot: sources in order, the rest inherit their main inflow */
} cui__sk_geo;

static cui__sk_geo cui__sk_layout(cui_ctx *ui, cui_rect r, const char *const *names, int nn, const cui_sankey_link *links_in, int nl, float nw, const char *fmt) {
    cui__sk_geo g;
    memset(&g, 0, sizeof g);
    g.nn = nn; g.nl = nl;
    g.names = (const char **)names;
    g.links = cui__frame_alloc(ui, sizeof(cui_sankey_link) * (size_t)CUI_MAX(1, nl));
    int m = 0;
    for (int l = 0; l < nl; l++)
        if (links_in[l].from >= 0 && links_in[l].from < nn && links_in[l].to >= 0 && links_in[l].to < nn && links_in[l].from != links_in[l].to && links_in[l].value > 0)
            g.links[m++] = links_in[l];
    g.nl = nl = m;
    g.node = cui__frame_alloc(ui, sizeof(cui_rect) * (size_t)CUI_MAX(1, nn));
    g.sy = cui__frame_alloc(ui, sizeof(float) * (size_t)CUI_MAX(1, nl));
    g.ty = cui__frame_alloc(ui, sizeof(float) * (size_t)CUI_MAX(1, nl));
    g.in = cui__copy_floats(ui, NULL, nn);
    g.out = cui__copy_floats(ui, NULL, nn);
    g.col = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, nn));
    for (int i = 0; i < nn; i++) g.col[i] = 0;
    for (int l = 0; l < nl; l++) { g.out[g.links[l].from] += g.links[l].value; g.in[g.links[l].to] += g.links[l].value; }
    /* depth: longest path from a source (bounded, so cycles cannot hang) */
    for (int it = 0; it < nn; it++) {
        bool ch = false;
        for (int l = 0; l < nl; l++) {
            int a = g.links[l].from, b = g.links[l].to;
            if (g.col[b] < g.col[a] + 1 && g.col[a] + 1 < nn) { g.col[b] = g.col[a] + 1; ch = true; }
        }
        if (!ch) break;
    }
    g.maxcol = 0;
    for (int i = 0; i < nn; i++) g.maxcol = CUI_MAX(g.maxcol, g.col[i]);
    /* an outcome that ends early (a drop off) stays at its own step; only one fed from the last step sits at the far right,
       so no ribbon has to cross a column of nodes to reach it */
    /* room on the right for the last column's labels */
    float fs = cui__fs(ui, ui->theme.text_xs), right_w = 0;
    if (g.maxcol > 0)
        for (int i = 0; i < nn; i++)
            if (g.col[i] == g.maxcol && (g.in[i] > 0 || g.out[i] > 0))
                right_w = CUI_MAX(right_w, cui__text_width(ui, CUI_FONT_MEDIUM, fs, names && names[i] ? names[i] : "", -1, false) + 6 +
                                               cui__text_width(ui, CUI_FONT_BODY, fs, cui__fmt_value(ui, fmt, CUI_MAX(g.in[i], g.out[i])), -1, true) + 10);
    float span = r.w - right_w - nw;
    /* color goes to the outcomes, the largest first; sources and steps stay neutral, so the eye follows where things end up */
    g.color = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, nn));
    for (int i = 0; i < nn; i++) g.color[i] = -1;
    for (int slot = 0;; slot++) {
        int best = -1;
        for (int i = 0; i < nn; i++)
            if (g.color[i] < 0 && g.out[i] <= 0 && g.in[i] > 0 && (best < 0 || g.in[i] > g.in[best])) best = i;
        if (best < 0) break;
        g.color[best] = slot;
    }
    float pad = 14;
    g.k = 1e30f;
    for (int c = 0; c <= g.maxcol; c++) {
        float sum = 0; int cnt = 0;
        for (int i = 0; i < nn; i++) if (g.col[i] == c) { sum += CUI_MAX(g.in[i], g.out[i]); cnt++; }
        if (cnt && sum > 0) g.k = CUI_MIN(g.k, (r.h - pad * (cnt - 1)) / sum);
    }
    if (g.k > 1e29f || g.k < 0) g.k = 0;
    for (int c = 0; c <= g.maxcol; c++) {
        float sum = 0; int cnt = 0;
        for (int i = 0; i < nn; i++) if (g.col[i] == c) { sum += CUI_MAX(g.in[i], g.out[i]) * g.k; cnt++; }
        float y = r.y + (r.h - sum - pad * CUI_MAX(0, cnt - 1)) * 0.5f;
        float x = g.maxcol > 0 ? r.x + span * c / g.maxcol : r.x;
        for (int i = 0; i < nn; i++) {
            if (g.col[i] != c) continue;
            float h = CUI_MAX(g.in[i], g.out[i]) * g.k;
            g.node[i] = (cui_rect){ x, y, nw, CUI_MAX(2.0f, h) };
            y += h + pad;
        }
    }
    /* link bands, ordered by the other end's position so ribbons do not cross needlessly */
    int *ord = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, nl));
    for (int l = 0; l < nl; l++) ord[l] = l;
    for (int a = 1; a < nl; a++) {
        int v = ord[a], b = a - 1;
        while (b >= 0 && g.node[g.links[ord[b]].to].y > g.node[g.links[v].to].y) { ord[b + 1] = ord[b]; b--; }
        ord[b + 1] = v;
    }
    float *off = cui__copy_floats(ui, NULL, nn);
    for (int q = 0; q < nl; q++) { int l = ord[q]; int a = g.links[l].from; g.sy[l] = g.node[a].y + off[a]; off[a] += g.links[l].value * g.k; }
    for (int a = 1; a < nl; a++) {
        int v = ord[a], b = a - 1;
        while (b >= 0 && g.node[g.links[ord[b]].from].y > g.node[g.links[v].from].y) { ord[b + 1] = ord[b]; b--; }
        ord[b + 1] = v;
    }
    memset(off, 0, sizeof(float) * (size_t)nn);
    for (int q = 0; q < nl; q++) { int l = ord[q]; int b = g.links[l].to; g.ty[l] = g.node[b].y + off[b]; off[b] += g.links[l].value * g.k; }
    return g;
}

static float cui__bez(float a, float b, float t) {   /* ease between two ends with flat tangents */
    float s = t * t * (3 - 2 * t);
    return a + (b - a) * s;
}

typedef struct { cui__sk_geo g; const char *format; float nw; float intro; int hover_node, hover_link; float t; cui__tip tip; } cui__sk_paint;

static void cui__paint_sankey(cui_ctx *ui, cui_node *n, void *data) {
    cui__sk_paint *p = data;
    cui_rect r = n->rect;
    float fs = cui__fs(ui, ui->theme.text_xs);
    cui_rect plot = { r.x, r.y, r.w - 0, r.h };
    cui__sk_geo g = cui__sk_layout(ui, plot, p->g.names, p->g.nn, p->g.links, p->g.nl, p->nw, p->format);
    bool any = (p->hover_node >= 0 || p->hover_link >= 0) && p->t > 0.01f;
    const int seg = 28;
    cui_cvert *v = cui__frame_alloc(ui, sizeof(cui_cvert) * (size_t)(seg + 1) * 2);
    uint32_t *idx = cui__frame_alloc(ui, sizeof(uint32_t) * (size_t)seg * 6);
    for (int l = 0; l < g.nl; l++) {
        int a = g.links[l].from, b = g.links[l].to;
        float th = g.links[l].value * g.k;
        float x0 = g.node[a].x + g.node[a].w, x1 = g.node[b].x;
        float reach = cui__clampf(p->intro, 0, 1);
        bool hl = l == p->hover_link || a == p->hover_node || b == p->hover_node;
        bool neutral = g.color[b] < 0;
        cui_color col = neutral ? CUI_C(ui, TEXT_MUTED) : cui__viz_color(ui, (cui_color){ 0 }, g.color[b]);
        float alpha = neutral ? (ui->mode == CUI_THEME_DARK ? 0.26f : 0.2f) : (ui->mode == CUI_THEME_DARK ? 0.38f : 0.3f);
        if (any) alpha = hl ? cui__lerp(alpha, 0.6f, p->t) : cui__lerp(alpha, 0.1f, p->t);
        cui_color c = cui_alpha(col, alpha);
        for (int i = 0; i <= seg; i++) {
            float t = (float)i / seg * reach;
            float x = x0 + (x1 - x0) * t;
            float yt = cui__bez(g.sy[l], g.ty[l], t);
            v[i * 2] = (cui_cvert){ x, yt, c };
            v[i * 2 + 1] = (cui_cvert){ x, yt + th, c };
        }
        for (int i = 0; i < seg; i++) {
            uint32_t q = (uint32_t)i * 2;
            uint32_t s6[6] = { q, q + 2, q + 3, q, q + 3, q + 1 };
            memcpy(idx + i * 6, s6, sizeof s6);
        }
        cui__draw_triangles(ui, v, (seg + 1) * 2, idx, seg * 6);
    }
    for (int i = 0; i < g.nn; i++) {
        if (g.in[i] <= 0 && g.out[i] <= 0) continue;
        cui_color col = g.color[i] < 0 ? CUI_C(ui, BORDER_STRONG) : cui__viz_color(ui, (cui_color){ 0 }, g.color[i]);
        if (any && p->hover_node >= 0 && i != p->hover_node) col = cui_mix(col, CUI_C(ui, SURFACE), 0.3f * p->t);
        cui_draw_rect(ui, g.node[i], CUI_CORNERS(3), col);
        bool right = true;
        float lx = right ? g.node[i].x + g.node[i].w + 8 : g.node[i].x - 8;
        float ly = g.node[i].y + g.node[i].h * 0.5f;
        const char *nm = g.names && g.names[i] ? g.names[i] : "";
        const char *val = cui__fmt_value(ui, p->format, CUI_MAX(g.in[i], g.out[i]));
        float nmw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, nm, -1, false);
        if (right) {
            float vw = cui__text_width(ui, CUI_FONT_BODY, fs, val, -1, true);
            if (g.col[i] < g.maxcol) cui_draw_rect(ui, (cui_rect){ lx - 4, ly - 9, nmw + vw + 14, 18 }, CUI_CORNERS(6), cui_alpha(CUI_C(ui, SURFACE), ui->mode == CUI_THEME_DARK ? 0.5f : 0.7f));
            cui__text_at(ui, CUI_FONT_MEDIUM, fs, lx, ly, 0, CUI_C(ui, FOREGROUND), nm, false);
            cui__text_at(ui, CUI_FONT_BODY, fs, lx + nmw + 6, ly, 0, CUI_C(ui, TEXT_MUTED), val, true);
        } else {
            float vw = cui__text_width(ui, CUI_FONT_BODY, fs, val, -1, true);
            cui__text_at(ui, CUI_FONT_BODY, fs, lx, ly, 2, CUI_C(ui, TEXT_MUTED), val, true);
            cui__text_at(ui, CUI_FONT_MEDIUM, fs, lx - vw - 6, ly, 2, CUI_C(ui, FOREGROUND), nm, false);
        }
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_sankey_ex(cui_ctx *ui, const char *id_str, const char *const *nodes, int nn, const cui_sankey_link *links, int nl, const cui_sankey_opts *o) {
    cui_sankey_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    nn = CUI_MAX(0, nn); nl = links ? CUI_MAX(0, nl) : 0;
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    float nw = o->node_width > 0 ? o->node_width : 12;
    float h = o->height > 0 ? o->height : 300;
    int hn = -1, hl = -1;
    cui__sk_geo g = cui__sk_layout(ui, pr.w > 0 ? pr : (cui_rect){ 0, 0, 600, h }, nodes, nn, links ? links : (cui_sankey_link[1]){ { 0, 0, 0 } }, nl, nw, o->format);
    if (r.hovered && pr.w > 0) {
        for (int i = 0; i < g.nn; i++) {
            cui_rect q = g.node[i];
            if (ui->mx >= q.x - 4 && ui->mx < q.x + q.w + 4 && ui->my >= q.y - 2 && ui->my < q.y + q.h + 2) hn = i;
        }
        if (hn < 0)
            for (int l = 0; l < g.nl; l++) {
                int a = g.links[l].from, b = g.links[l].to;
                float x0 = g.node[a].x + g.node[a].w, x1 = g.node[b].x;
                if (x1 <= x0 || ui->mx < x0 || ui->mx > x1) continue;
                float t = (ui->mx - x0) / (x1 - x0);
                float yt = cui__bez(g.sy[l], g.ty[l], t), th = g.links[l].value * g.k;
                if (ui->my >= yt && ui->my <= yt + CUI_MAX(th, 3.0f)) hl = l;
            }
    }
    int hover = hn >= 0 ? hn : hl >= 0 ? nn + hl : -1;
    cui__viz_hover(ui, st, hover);
    cui__sk_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->g = g;   /* names and the filtered links; the paint lays out again in its own rect */
    p->format = o->format; p->nw = nw;
    p->intro = st->intro.x; p->t = st->hover_t;
    p->hover_node = p->hover_link = -1;
    if (st->hover_t > 0.01f) {
        if (st->shown < nn) p->hover_node = st->shown;
        else if (st->shown - nn < g.nl) p->hover_link = st->shown - nn;
    }
    p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
    if (p->hover_node >= 0) {
        int i = p->hover_node;
        p->tip.title = nodes && nodes[i] ? nodes[i] : "";
        if (g.in[i] > 0) cui__tip_row(&p->tip, "In", cui__fmt_value(ui, o->format, g.in[i]), cui__rgba(0, 0, 0, 0));
        if (g.out[i] > 0) cui__tip_row(&p->tip, "Out", cui__fmt_value(ui, o->format, g.out[i]), cui__rgba(0, 0, 0, 0));
    } else if (p->hover_link >= 0) {
        cui_sankey_link *L = &g.links[p->hover_link];
        p->tip.title = cui__frame_printf(ui, "%s \xe2\x86\x92 %s", nodes && nodes[L->from] ? nodes[L->from] : "", nodes && nodes[L->to] ? nodes[L->to] : "");
        cui__tip_row(&p->tip, "Flow", cui__fmt_value(ui, o->format, L->value), g.color[L->to] < 0 ? CUI_C(ui, TEXT_MUTED) : cui__viz_color(ui, (cui_color){ 0 }, g.color[L->to]));
        if (g.out[L->from] > 0) cui__tip_row(&p->tip, "Of the source", cui__pct(ui, L->value, g.out[L->from]), cui__rgba(0, 0, 0, 0));
    } else p->tip.title = NULL;
    cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(h), cui__paint_sankey, p, hover >= 0);
    if (hover >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return r.clicked ? hn : -1;
}

/* ------------------------------------------------------------------------ */
/* Waffle                                                                   */
/* ------------------------------------------------------------------------ */

typedef struct { cui_viz_item *items; int count; int *owner; int cols, rows; float cell, gap; float total; const char *format; float intro; int hover; cui__tip tip; } cui__wf_paint;

static void cui__paint_waffle(cui_ctx *ui, cui_node *n, void *data) {
    cui__wf_paint *p = data;
    cui_rect r = n->rect;
    float step = p->cell + p->gap;
    int cells = p->cols * p->rows;
    for (int c = 0; c < cells; c++) {
        int col = c / p->rows, row = c % p->rows;   /* fill column by column, bottom up */
        cui_rect q = { r.x + col * step, r.y + (p->rows - 1 - row) * step, p->cell, p->cell };
        int o = p->owner[c];
        float a = cui__clampf(p->intro * 2.0f - (float)c / (float)cells, 0, 1);
        cui_color fill = o >= 0 ? cui__viz_color(ui, p->items[o].color, o) : cui__viz_empty(ui);
        if (p->hover >= 0 && p->tip.alpha > 0.01f && o != p->hover) fill = cui_mix(fill, CUI_C(ui, SURFACE), 0.55f * p->tip.alpha);
        cui_draw_rect(ui, q, CUI_CORNERS(p->cell * 0.22f), cui_mix(cui__viz_empty(ui), fill, a));
    }
    /* legend on the right */
    float fs = cui__fs(ui, ui->theme.text_sm);
    float lx = r.x + p->cols * step + 18, ly = r.y + 4;
    for (int k = 0; k < p->count; k++) {
        float a = (p->hover >= 0 && p->tip.alpha > 0.01f && k != p->hover) ? 0.5f : 1.0f;
        float cy = ly + k * 26 + 9;
        cui_draw_rect(ui, (cui_rect){ lx, cy - 5, 10, 10 }, CUI_CORNERS(3), cui_alpha(cui__viz_color(ui, p->items[k].color, k), a));
        cui__text_at(ui, CUI_FONT_BODY, fs, lx + 18, cy, 0, cui_alpha(CUI_C(ui, TEXT_SECONDARY), a), p->items[k].label ? p->items[k].label : "", false);
        cui__text_at(ui, CUI_FONT_MEDIUM, fs, r.x + r.w, cy, 2, cui_alpha(CUI_C(ui, FOREGROUND), a), cui__pct(ui, p->items[k].value, p->total), true);
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_waffle_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *items_in, int count, const cui_waffle_opts *o) {
    cui_waffle_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    int cols = o->columns > 0 ? o->columns : 10, rows = o->rows > 0 ? o->rows : 10;
    cols = CUI_MIN(cols, 60); rows = CUI_MIN(rows, 60);
    int cells = cols * rows;
    float cell = o->cell > 0 ? o->cell : 18, gap = cell >= 12 ? 3 : 2, step = cell + gap;
    cui_viz_item *items = cui__copy_items(ui, items_in, count);
    float total = 0;
    for (int k = 0; k < count; k++) total += items[k].value;
    /* cells per item by largest remainder */
    int *owner = cui__frame_alloc(ui, sizeof(int) * (size_t)cells);
    int *nc = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, count));
    float *rem = cui__copy_floats(ui, NULL, count);
    int used = 0;
    for (int k = 0; k < count; k++) {
        float exact = total > 0 ? items[k].value / total * cells : 0;
        nc[k] = (int)floorf(exact); rem[k] = exact - nc[k]; used += nc[k];
    }
    while (total > 0 && used < cells) {
        int best = 0;
        for (int k = 1; k < count; k++) if (rem[k] > rem[best]) best = k;
        nc[best]++; rem[best] = -1; used++;
    }
    int c = 0;
    for (int k = 0; k < count; k++) for (int j = 0; j < nc[k] && c < cells; j++) owner[c++] = k;
    for (; c < cells; c++) owner[c] = -1;
    float fs = cui__fs(ui, ui->theme.text_sm), lw = 0;
    for (int k = 0; k < count; k++) lw = CUI_MAX(lw, cui__text_width(ui, CUI_FONT_BODY, fs, items[k].label ? items[k].label : "", -1, false));
    float gw = cols * step - gap;
    float legend_w = 18 + 18 + lw + 16 + 44;
    int hover = -1;
    if (r.hovered) {
        int cx = (int)floorf((ui->mx - pr.x) / step), cy = (int)floorf((ui->my - pr.y) / step);
        if (cx >= 0 && cx < cols && cy >= 0 && cy < rows) hover = owner[cx * rows + (rows - 1 - cy)];
        else if (ui->mx > pr.x + gw + 10) {
            int k = (int)floorf((ui->my - pr.y - 4 + 4) / 26);
            if (k >= 0 && k < count) hover = k;
        }
    }
    cui__viz_hover(ui, st, hover);
    cui__wf_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->items = items; p->count = count; p->owner = owner; p->cols = cols; p->rows = rows; p->cell = cell; p->gap = gap; p->total = total; p->format = o->format;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < count ? st->shown : -1;
    if (p->hover >= 0) {
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = items[p->hover].label ? items[p->hover].label : "";
        cui__tip_row(&p->tip, "Value", cui__fmt_value(ui, o->format, items[p->hover].value), cui__viz_color(ui, items[p->hover].color, p->hover));
        cui__tip_row(&p->tip, "Share", cui__pct(ui, items[p->hover].value, total), cui__rgba(0, 0, 0, 0));
    }
    float h = CUI_MAX(rows * step - gap, count * 26.0f);
    cui__viz_node(ui, id, CUI_PX(gw + legend_w), CUI_PX(h), cui__paint_waffle, p, hover >= 0);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Split bar                                                                */
/* ------------------------------------------------------------------------ */

typedef struct { cui_viz_item *items; int count; float total; cui_split_bar_opts o; float bh; float intro; int hover; cui__tip tip; } cui__sb_paint;

static void cui__sb_segments(cui_rect r, const cui_viz_item *items, int count, float total, float bh, float e, cui_rect *out) {
    int nz = 0;
    for (int k = 0; k < count; k++) if (items[k].value > 0) nz++;
    float gap = 2, avail = r.w - gap * (float)CUI_MAX(0, nz - 1);
    float x = r.x;
    for (int k = 0; k < count; k++) {
        float w = total > 0 ? items[k].value / total * avail * e : 0;
        out[k] = (cui_rect){ x, r.y, w, bh };
        if (items[k].value > 0) x += w + gap * e;
    }
}

static void cui__paint_split_bar(cui_ctx *ui, cui_node *n, void *data) {
    cui__sb_paint *p = data;
    cui_rect r = n->rect;
    cui_rect *seg = cui__frame_alloc(ui, sizeof(cui_rect) * (size_t)CUI_MAX(1, p->count));
    float e = cui__clampf(p->intro, 0, 1);
    cui_draw_rect(ui, (cui_rect){ r.x, r.y, r.w, p->bh }, CUI_CORNERS(p->bh * 0.5f), cui__viz_empty(ui));
    cui__sb_segments(r, p->items, p->count, p->total, p->bh, e, seg);
    int first = -1, last = -1;
    for (int k = 0; k < p->count; k++) if (p->items[k].value > 0) { if (first < 0) first = k; last = k; }
    for (int k = 0; k < p->count; k++) {
        if (p->items[k].value <= 0) continue;
        float rr = CUI_MIN(p->bh * 0.5f, 3);
        float re = p->bh * 0.5f;
        cui_corners c = { k == first ? re : rr, k == last && e > 0.99f ? re : rr, k == last && e > 0.99f ? re : rr, k == first ? re : rr };
        cui_color col = cui__viz_color(ui, p->items[k].color, k);
        if (p->hover >= 0 && p->tip.alpha > 0.01f && k != p->hover) col = cui_mix(col, CUI_C(ui, SURFACE), 0.5f * p->tip.alpha);
        cui_draw_rect(ui, seg[k], c, col);
    }
    if (!p->o.no_legend) {
        float fs = cui__fs(ui, ui->theme.text_sm), lh = 20;
        float fh = cui__font_line_height(ui, CUI_FONT_BODY, fs);
        float x = r.x, y = r.y + p->bh + 14;
        for (int k = 0; k < p->count; k++) {
            const char *lb = p->items[k].label ? p->items[k].label : "";
            const char *vs = cui__frame_printf(ui, "%s  %s", cui__fmt_value(ui, p->o.format, p->items[k].value), cui__pct(ui, p->items[k].value, p->total));
            float w = 14 + cui__text_width(ui, CUI_FONT_MEDIUM, fs, lb, -1, false) + 8 + cui__text_width(ui, CUI_FONT_BODY, fs, vs, -1, true);
            if (x > r.x && x + w > r.x + r.w) { x = r.x; y += lh + 6; }
            float a = (p->hover >= 0 && p->tip.alpha > 0.01f && k != p->hover) ? 0.5f : 1.0f;
            cui_draw_rect(ui, (cui_rect){ x, y + lh * 0.5f - 4, 8, 8 }, CUI_CORNERS(2.5f), cui_alpha(cui__viz_color(ui, p->items[k].color, k), a));
            float lw = cui__text_width(ui, CUI_FONT_MEDIUM, fs, lb, -1, false);
            cui__draw_text_line(ui, CUI_FONT_MEDIUM, fs, x + 14, y + (lh - fh) * 0.5f, cui_alpha(CUI_C(ui, FOREGROUND), a), lb, -1, false);
            cui__draw_text_line(ui, CUI_FONT_BODY, fs, x + 14 + lw + 8, y + (lh - fh) * 0.5f, cui_alpha(CUI_C(ui, TEXT_MUTED), a), vs, -1, true);
            x += w + 22;
        }
    }
    cui__tip_draw(ui, &p->tip);
}

static float cui__sb_legend_h(cui_ctx *ui, const cui_viz_item *items, int count, float total, const char *fmt, float width) {
    float fs = cui__fs(ui, ui->theme.text_sm), lh = 20, x = 0, y = 0;
    for (int k = 0; k < count; k++) {
        const char *lb = items[k].label ? items[k].label : "";
        const char *vs = cui__frame_printf(ui, "%s  %s", cui__fmt_value(ui, fmt, items[k].value), cui__pct(ui, items[k].value, total));
        float w = 14 + cui__text_width(ui, CUI_FONT_MEDIUM, fs, lb, -1, false) + 8 + cui__text_width(ui, CUI_FONT_BODY, fs, vs, -1, true);
        if (x > 0 && x + w > width) { x = 0; y += lh + 6; }
        x += w + 22;
    }
    return count ? y + lh : 0;
}

int cui_split_bar_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *items_in, int count, const cui_split_bar_opts *o) {
    cui_split_bar_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui_viz_item *items = cui__copy_items(ui, items_in, count);
    float total = 0;
    for (int k = 0; k < count; k++) total += items[k].value;
    float bh = o->height > 0 ? o->height : 10;
    int hover = -1;
    if (r.hovered && pr.w > 0) {
        if (ui->my < pr.y + bh + 6) {
            cui_rect *seg = cui__frame_alloc(ui, sizeof(cui_rect) * (size_t)CUI_MAX(1, count));
            cui__sb_segments(pr, items, count, total, bh, 1, seg);
            for (int k = 0; k < count; k++) if (items[k].value > 0 && ui->mx >= seg[k].x - 1 && ui->mx < seg[k].x + seg[k].w + 1) hover = k;
        } else if (!o->no_legend) {
            float fs = cui__fs(ui, ui->theme.text_sm), lh = 20, x = pr.x, y = pr.y + bh + 14;
            for (int k = 0; k < count; k++) {
                const char *lb = items[k].label ? items[k].label : "";
                const char *vs = cui__frame_printf(ui, "%s  %s", cui__fmt_value(ui, o->format, items[k].value), cui__pct(ui, items[k].value, total));
                float w = 14 + cui__text_width(ui, CUI_FONT_MEDIUM, fs, lb, -1, false) + 8 + cui__text_width(ui, CUI_FONT_BODY, fs, vs, -1, true);
                if (x > pr.x && x + w > pr.x + pr.w) { x = pr.x; y += lh + 6; }
                if (ui->mx >= x && ui->mx < x + w && ui->my >= y && ui->my < y + lh) hover = k;
                x += w + 22;
            }
        }
    }
    cui__viz_hover(ui, st, hover);
    cui__sb_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->items = items; p->count = count; p->total = total; p->o = *o; p->bh = bh;
    p->intro = st->intro.x;
    p->hover = st->hover_t > 0.01f && st->shown < count ? st->shown : -1;
    if (p->hover >= 0) {
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = items[p->hover].label ? items[p->hover].label : "";
        cui__tip_row(&p->tip, "Value", cui__fmt_value(ui, o->format, items[p->hover].value), cui__viz_color(ui, items[p->hover].color, p->hover));
        cui__tip_row(&p->tip, "Share", cui__pct(ui, items[p->hover].value, total), cui__rgba(0, 0, 0, 0));
    }
    float width = pr.w > 0 ? pr.w : 400;
    float h = bh + (o->no_legend ? 0 : 14 + cui__sb_legend_h(ui, items, count, total, o->format, width));
    cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(h), cui__paint_split_bar, p, hover >= 0);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Bar list                                                                 */
/* ------------------------------------------------------------------------ */

#define CUI__BL_ROW 34.0f
#define CUI__BL_GAP 6.0f

typedef struct { cui_spring y, w; float hover; bool init; } cui__bl_item_state;
typedef struct { const char *label; const char *value; float y, w, hover; cui_color c; } cui__bl_row;
typedef struct { cui__bl_row *rows; int n; float vw; } cui__bl_paint;

static void cui__paint_bar_list(cui_ctx *ui, cui_node *n, void *data) {
    cui__bl_paint *p = data;
    cui_rect r = n->rect;
    float fs = cui__fs(ui, ui->theme.text_sm);
    float bw = r.w - p->vw - 16;
    cui_push_clip(ui, r);
    for (int k = 0; k < p->n; k++) {
        cui__bl_row *row = &p->rows[k];
        float y = r.y + row->y;
        cui_rect bar = { r.x, y, CUI_MAX(row->w * bw, 4.0f), CUI__BL_ROW };
        cui_color fill = cui_alpha(row->c, (ui->mode == CUI_THEME_DARK ? 0.26f : 0.16f) + 0.1f * row->hover);
        if (row->hover > 0.01f) cui_draw_rect(ui, (cui_rect){ r.x - 6, y - 2, r.w + 12, CUI__BL_ROW + 4 }, CUI_CORNERS(10), cui_alpha(CUI_C(ui, HOVER), row->hover));
        cui_draw_rect(ui, bar, CUI_CORNERS(8), fill);
        cui_push_clip(ui, (cui_rect){ r.x, y, bw, CUI__BL_ROW });
        cui__text_at(ui, CUI_FONT_MEDIUM, fs, r.x + 12, y + CUI__BL_ROW * 0.5f, 0, CUI_C(ui, FOREGROUND), row->label, false);
        cui_pop_clip(ui);
        cui__text_at(ui, CUI_FONT_MEDIUM, fs, r.x + r.w, y + CUI__BL_ROW * 0.5f, 2, CUI_C(ui, TEXT_SECONDARY), row->value, true);
    }
    cui_pop_clip(ui);
}

int cui_bar_list_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *items_in, int count, const cui_bar_list_opts *o) {
    cui_bar_list_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui_viz_item *items = cui__copy_items(ui, items_in, count);
    int *order = cui__frame_alloc(ui, sizeof(int) * (size_t)CUI_MAX(1, count));
    float maxv = 0;
    for (int k = 0; k < count; k++) { order[k] = k; maxv = CUI_MAX(maxv, items[k].value); }
    if (!o->keep_order)
        for (int a = 1; a < count; a++) {
            int v = order[a], b = a - 1;
            while (b >= 0 && items[order[b]].value < items[v].value) { order[b + 1] = order[b]; b--; }
            order[b + 1] = v;
        }
    int shown = o->max_rows > 0 ? CUI_MIN(o->max_rows, count) : count;
    int hover_rank = -1;
    if (r.hovered) {
        int k = (int)floorf((ui->my - pr.y + CUI__BL_GAP * 0.5f) / (CUI__BL_ROW + CUI__BL_GAP));
        if (k >= 0 && k < shown) hover_rank = k;
    }
    cui__bl_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->rows = cui__frame_alloc(ui, sizeof(cui__bl_row) * (size_t)CUI_MAX(1, count));
    p->n = 0;
    p->vw = 0;
    float fs = cui__fs(ui, ui->theme.text_sm);
    for (int rank = 0; rank < count; rank++) {
        int k = order[rank];
        const char *lb = items[k].label ? items[k].label : "";
        cui_id iid = cui__hash_str(lb, id ^ 0xB1u);
        cui__bl_item_state *is = cui_state(ui, iid, sizeof *is);
        float ty = rank * (CUI__BL_ROW + CUI__BL_GAP), tw = maxv > 0 ? items[k].value / maxv : 0;
        if (rank >= shown) ty = shown * (CUI__BL_ROW + CUI__BL_GAP);   /* slides out below */
        if (!is->init) { is->y.x = ty; is->w.x = 0; is->init = true; }
        cui__spring(ui, &is->y, ty, CUI_SPRING_SMOOTH);
        cui__spring(ui, &is->w, tw, 0.6f, 0.0f);
        cui__tween(ui, &is->hover, rank == hover_rank ? 1.0f : 0.0f, CUI__T_HOVER);
        if (rank >= shown && fabsf(is->y.x - ty) < 0.5f) continue;   /* gone */
        cui__bl_row *row = &p->rows[p->n++];
        row->label = lb;
        row->value = cui__fmt_value(ui, o->format, items[k].value);
        row->y = is->y.x; row->w = cui__clampf(is->w.x, 0, 1.05f); row->hover = is->hover;
        row->c = cui__color_set(o->color) ? cui_color_of(ui, o->color) : cui__color_set(items[k].color) ? cui_color_of(ui, items[k].color) : cui__viz_base(ui, (cui_color){ 0 });
        p->vw = CUI_MAX(p->vw, cui__text_width(ui, CUI_FONT_MEDIUM, fs, row->value, -1, true));
    }
    float h = shown * (CUI__BL_ROW + CUI__BL_GAP) - (shown ? CUI__BL_GAP : 0);
    int n = cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(h), cui__paint_bar_list, p, false);
    ui->nodes[n].flags &= ~(uint32_t)CUI_NF_PAINT_AFTER;
    if (hover_rank >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return r.clicked && hover_rank >= 0 ? order[hover_rank] : -1;
}

/* ------------------------------------------------------------------------ */
/* Activity rings                                                           */
/* ------------------------------------------------------------------------ */

typedef struct { cui_viz_item *items; int count; cui_rings_opts o; float size, th; float *prog; int hover; cui__tip tip; float label_w; } cui__ring_paint;

#define CUI__RING_LEGEND_GAP 28
#define CUI__RING_ROW 26

static void cui__paint_rings(cui_ctx *ui, cui_node *n, void *data) {
    cui__ring_paint *p = data;
    cui_rect r = n->rect;
    cui_vec2 c = { r.x + (p->o.legend ? p->size * 0.5f : r.w * 0.5f), r.y + p->size * 0.5f };
    float a0 = -CUI_PI * 0.5f;
    for (int k = 0; k < p->count; k++) {
        float rad = p->size * 0.5f - p->th * 0.5f - k * (p->th + 3);
        if (rad <= p->th * 0.5f) break;
        cui_color col = cui__viz_color(ui, p->items[k].color, k);
        bool dim = p->hover >= 0 && p->tip.alpha > 0.01f && k != p->hover;
        if (dim) col = cui_mix(col, CUI_C(ui, SURFACE), 0.4f * p->tip.alpha);
        cui_draw_arc(ui, c, rad, 0, 2 * CUI_PI, p->th, cui_alpha(col, ui->mode == CUI_THEME_DARK ? 0.22f : 0.16f));
        float v = CUI_MAX(0.0f, p->prog[k]);
        if (v <= 0.001f) continue;
        float a1 = a0 + v * 2 * CUI_PI;
        if (v < 1) {
            cui_draw_arc(ui, c, rad, a0, a1, p->th, col);
            cui_draw_circle(ui, (cui_vec2){ c.x + cosf(a0) * rad, c.y + sinf(a0) * rad }, p->th * 0.5f, col);
        } else {
            cui_draw_arc(ui, c, rad, 0, 2 * CUI_PI, p->th, col);
            /* the tip overlaps the start: a soft shadow cast ahead of the cap tells them apart */
            float sa = a1 + p->th * 0.25f / rad;
            float sr = p->th * 0.5f;
            cui_vec2 sc = { c.x + cosf(sa) * rad, c.y + sinf(sa) * rad };
            cui_push_clip(ui, (cui_rect){ c.x - rad - p->th, c.y - rad - p->th, (rad + p->th) * 2, (rad + p->th) * 2 });
            cui_draw_shadow(ui, (cui_rect){ sc.x - sr, sc.y - sr, sr * 2, sr * 2 }, CUI_CORNERS(sr), p->th * 0.3f, 0, (cui_vec2){ 0, 0 },
                            cui__rgba(0, 0, 0, ui->mode == CUI_THEME_DARK ? 150 : 70));
            cui_pop_clip(ui);
            /* the last stretch of the arc sits on top of the shadow */
            cui_draw_arc(ui, c, rad, a1 - 0.6f, a1, p->th, col);
        }
        cui_vec2 tip = { c.x + cosf(a1) * rad, c.y + sinf(a1) * rad };
        cui_draw_circle(ui, tip, p->th * 0.5f, col);
    }
    if (p->o.center) {
        float fd = cui__fs(ui, p->size * 0.16f);
        cui__text_at(ui, CUI_FONT_DISPLAY, fd, c.x, c.y - (p->o.caption ? 7 : 0), 1, CUI_C(ui, FOREGROUND), p->o.center, true);
        if (p->o.caption) cui__text_at(ui, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_xs), c.x, c.y + fd * 0.55f, 1, CUI_C(ui, TEXT_MUTED), p->o.caption, false);
    }
    if (p->o.legend) {   /* outer ring first, as the rings read from the outside in */
        float fs = cui__fs(ui, ui->theme.text_sm);
        float lx = r.x + p->size + CUI__RING_LEGEND_GAP, ly = c.y - p->count * CUI__RING_ROW * 0.5f + CUI__RING_ROW * 0.5f;
        for (int k = 0; k < p->count; k++) {
            bool dim = p->hover >= 0 && p->tip.alpha > 0.01f && k != p->hover;
            cui_color col = cui__viz_color(ui, p->items[k].color, k);
            float y = ly + k * CUI__RING_ROW;
            cui_draw_rect(ui, (cui_rect){ lx, y - 4, 8, 8 }, CUI_CORNERS(2.5f), dim ? cui_mix(col, CUI_C(ui, SURFACE), 0.4f * p->tip.alpha) : col);
            cui__text_at(ui, CUI_FONT_BODY, fs, lx + 16, y, 0, CUI_C(ui, TEXT_SECONDARY), p->items[k].label ? p->items[k].label : "", false);
            cui__text_at(ui, CUI_FONT_MEDIUM, fs, lx + 16 + p->label_w + 14, y, 0, CUI_C(ui, FOREGROUND), cui__pct(ui, p->items[k].value, 1), true);
        }
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_activity_rings_ex(cui_ctx *ui, const char *id_str, const cui_viz_item *rings, int count, const cui_rings_opts *o) {
    cui_rings_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_CLAMP(count, 0, 8);
    cui_id id = cui_make_id(ui, id_str);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    cui__viz_state *st = cui_state(ui, id, sizeof *st + sizeof(cui_spring) * 8);
    cui_spring *sp = (cui_spring *)(st + 1);
    float size = o->size > 0 ? o->size : 150, th = o->thickness > 0 ? o->thickness : CUI_MAX(8.0f, size * 0.1f);
    cui__ring_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->prog = cui__copy_floats(ui, NULL, count);
    for (int k = 0; k < count; k++) {
        float v = rings[k].value >= 0 ? CUI_MIN(rings[k].value, 4.0f) : 0;
        cui__spring(ui, &sp[k], v, 0.9f + 0.1f * k, 0.0f);
        p->prog[k] = sp[k].x;
    }
    int hover = -1;
    if (r.hovered) {
        float dx = ui->mx - (pr.x + pr.w * 0.5f), dy = ui->my - (pr.y + size * 0.5f);
        float dist = sqrtf(dx * dx + dy * dy);
        for (int k = 0; k < count; k++) {
            float rad = size * 0.5f - th * 0.5f - k * (th + 3);
            if (fabsf(dist - rad) <= th * 0.5f + 1.5f) hover = k;
        }
    }
    if (o->legend && r.hovered && hover < 0) {   /* a legend row picks out its ring */
        float lx = pr.x + size + CUI__RING_LEGEND_GAP, ly = pr.y + size * 0.5f - count * CUI__RING_ROW * 0.5f;
        if (ui->mx >= lx && ui->my >= ly && ui->my < ly + count * CUI__RING_ROW) hover = CUI_CLAMP((int)((ui->my - ly) / CUI__RING_ROW), 0, count - 1);
    }
    cui__viz_hover(ui, st, hover);
    p->items = cui__copy_items(ui, rings, count); p->count = count; p->o = *o; p->size = size; p->th = th;
    p->hover = st->hover_t > 0.01f && st->shown < count ? st->shown : -1;
    if (p->hover >= 0) {
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = rings[p->hover].label ? rings[p->hover].label : "";
        cui__tip_row(&p->tip, "Progress", cui__pct(ui, rings[p->hover].value, 1), cui__viz_color(ui, rings[p->hover].color, p->hover));
    }
    float w = size;
    if (o->legend) {
        float fs = cui__fs(ui, ui->theme.text_sm), vw = 0;
        for (int k = 0; k < count; k++) {
            p->label_w = CUI_MAX(p->label_w, cui__text_width(ui, CUI_FONT_BODY, fs, rings[k].label ? rings[k].label : "", -1, false));
            vw = CUI_MAX(vw, cui__text_width(ui, CUI_FONT_MEDIUM, fs, cui__pct(ui, rings[k].value, 1), -1, true));
        }
        w += CUI__RING_LEGEND_GAP + 16 + p->label_w + 14 + vw;
    }
    cui__viz_node(ui, id, CUI_PX(w), CUI_PX(size), cui__paint_rings, p, hover >= 0);
    return r.clicked ? hover : -1;
}

/* ------------------------------------------------------------------------ */
/* Scatter                                                                  */
/* ------------------------------------------------------------------------ */

typedef struct {
    float x0, x1, xs, y0, y1, ys, smax;
    cui_rect plot;
} cui__sc_geo;

static cui__sc_geo cui__sc_layout(cui_ctx *ui, cui_rect r, const cui_scatter_series *s, int count, const cui_scatter_opts *o, float legend_h) {
    cui__sc_geo g;
    float xl = 1e30f, xh = -1e30f, yl = 1e30f, yh = -1e30f;
    g.smax = 0;
    for (int k = 0; k < count; k++)
        for (int i = 0; i < s[k].count; i++) {
            xl = CUI_MIN(xl, s[k].points[i].x); xh = CUI_MAX(xh, s[k].points[i].x);
            yl = CUI_MIN(yl, s[k].points[i].y); yh = CUI_MAX(yh, s[k].points[i].y);
            if (s[k].sizes) g.smax = CUI_MAX(g.smax, s[k].sizes[i]);
        }
    if (xl > xh) { xl = 0; xh = 1; }
    if (yl > yh) { yl = 0; yh = 1; }
    /* the axes end on the first nice value past the data; padding is added only when a point would sit on the frame */
    float px = (xh - xl) * 0.015f, py = (yh - yl) * 0.03f;
    cui__nice_range(xl, xh, 5, &g.x0, &g.x1, &g.xs);
    if (g.x1 - xh < px || (xl - g.x0 < px && g.x0 != 0)) cui__nice_range(xl - (xl - g.x0 < px && g.x0 != 0 ? px : 0), xh + (g.x1 - xh < px ? px : 0), 5, &g.x0, &g.x1, &g.xs);
    cui__nice_range(yl, yh, 4, &g.y0, &g.y1, &g.ys);
    if (g.y1 - yh < py || (yl - g.y0 < py && g.y0 != 0)) cui__nice_range(yl - (yl - g.y0 < py && g.y0 != 0 ? py : 0), yh + (g.y1 - yh < py ? py : 0), 4, &g.y0, &g.y1, &g.ys);
    float fs = cui__fs(ui, ui->theme.text_xs), aw = 0;
    for (float v = g.y0; v <= g.y1 + g.ys * 0.5f; v += g.ys) aw = CUI_MAX(aw, cui__text_width(ui, CUI_FONT_BODY, fs, cui__fmt_value(ui, o->y_format, v), -1, true));
    float top = legend_h + (o->y_label ? 22 : 6);
    g.plot = (cui_rect){ r.x + aw + 10, r.y + top, r.w - aw - 14, r.h - top - (o->x_label ? 46 : 26) };
    return g;
}

static cui_vec2 cui__sc_pt(const cui__sc_geo *g, cui_vec2 v) {
    return (cui_vec2){ g->plot.x + (v.x - g->x0) / (g->x1 - g->x0) * g->plot.w, g->plot.y + g->plot.h - (v.y - g->y0) / (g->y1 - g->y0) * g->plot.h };
}

static float cui__sc_radius(const cui__sc_geo *g, const cui_scatter_series *s, int i) {
    if (!s->sizes || g->smax <= 0) return 4.5f;
    return 3.5f + sqrtf(CUI_MAX(0.0f, s->sizes[i]) / g->smax) * 11;
}

typedef struct { cui_scatter_series *s; int count; cui_scatter_opts o; float legend_h; float intro; int hs, hi; cui__tip tip; } cui__sc_paint;

static void cui__paint_scatter(cui_ctx *ui, cui_node *n, void *data) {
    cui__sc_paint *p = data;
    cui_rect r = n->rect;
    cui__sc_geo g = cui__sc_layout(ui, r, p->s, p->count, &p->o, p->legend_h);
    float fs = cui__fs(ui, ui->theme.text_xs);
    cui_rect pl = g.plot;
    for (float v = g.y0; v <= g.y1 + g.ys * 0.5f; v += g.ys) {
        float y = pl.y + pl.h - (v - g.y0) / (g.y1 - g.y0) * pl.h;
        cui_draw_rect(ui, (cui_rect){ pl.x, y - 0.5f, pl.w, 1 }, CUI_CORNERS(0), fabsf(v - g.y0) < g.ys * 0.01f ? CUI_C(ui, BORDER) : CUI_C(ui, BORDER_SUBTLE));
        cui__text_at(ui, CUI_FONT_BODY, fs, pl.x - 8, y, 2, CUI_C(ui, TEXT_MUTED), cui__fmt_value(ui, p->o.y_format, v), true);
    }
    for (float v = g.x0; v <= g.x1 + g.xs * 0.5f; v += g.xs) {
        float x = pl.x + (v - g.x0) / (g.x1 - g.x0) * pl.w;
        cui__text_at(ui, CUI_FONT_BODY, fs, x, pl.y + pl.h + 14, 1, CUI_C(ui, TEXT_MUTED), cui__fmt_value(ui, p->o.x_format, v), true);
    }
    if (p->o.x_label) cui__text_at(ui, CUI_FONT_MEDIUM, fs, pl.x + pl.w * 0.5f, pl.y + pl.h + 36, 1, CUI_C(ui, TEXT_SECONDARY), p->o.x_label, false);
    if (p->o.y_label) cui__text_at(ui, CUI_FONT_MEDIUM, fs, r.x, pl.y - 14, 0, CUI_C(ui, TEXT_SECONDARY), p->o.y_label, false);
    if (p->hs >= 0 && p->tip.alpha > 0.01f) {
        cui_vec2 q = cui__sc_pt(&g, p->s[p->hs].points[p->hi]);
        cui_color gl = cui_alpha(CUI_C(ui, BORDER_STRONG), p->tip.alpha);
        cui_draw_rect(ui, (cui_rect){ q.x - 0.5f, q.y, 1, pl.y + pl.h - q.y }, CUI_CORNERS(0), gl);
        cui_draw_rect(ui, (cui_rect){ pl.x, q.y - 0.5f, q.x - pl.x, 1 }, CUI_CORNERS(0), gl);
    }
    cui_push_clip(ui, (cui_rect){ pl.x - 30, pl.y - 30, pl.w + 60, pl.h + 30 });
    for (int k = 0; k < p->count; k++) {
        cui_scatter_series *s = &p->s[k];
        cui_color col = cui__viz_color(ui, s->color, k);
        bool dim = p->hs >= 0 && p->tip.alpha > 0.01f && p->hs != k;
        for (int i = 0; i < s->count; i++) {
            cui_vec2 q = cui__sc_pt(&g, s->points[i]);
            float rr = cui__sc_radius(&g, s, i);
            float e = cui__clampf(p->intro * 1.6f - (q.x - pl.x) / CUI_MAX(1.0f, pl.w) * 0.6f, 0, 1);
            rr *= e;
            if (rr <= 0.2f) continue;
            bool h = k == p->hs && i == p->hi && p->tip.alpha > 0.01f;
            if (h) rr += 1.5f;
            cui_color fill = s->sizes ? cui_alpha(col, 0.42f) : col;
            if (dim) fill = cui_alpha(fill, 0.45f);
            cui_draw_circle(ui, q, rr + 1.5f, CUI_C(ui, SURFACE));
            cui_draw_circle(ui, q, rr, fill);
            if (s->sizes) cui_draw_arc(ui, q, rr - 0.5f, 0, 2 * CUI_PI, 1, cui_alpha(col, dim ? 0.5f : 1));
        }
    }
    cui_pop_clip(ui);
    if (p->count >= 2 && !p->o.no_legend) {
        const char **names = cui__frame_alloc(ui, sizeof(char *) * (size_t)p->count);
        cui_color *cols = cui__frame_alloc(ui, sizeof(cui_color) * (size_t)p->count);
        for (int k = 0; k < p->count; k++) { names[k] = p->s[k].name; cols[k] = cui__viz_color(ui, p->s[k].color, k); }
        cui__viz_legend(ui, r.x, r.y, r.w, names, cols, p->count, p->tip.alpha > 0.01f ? p->hs : -1, true);
    }
    cui__tip_draw(ui, &p->tip);
}

int cui_scatter_chart_ex(cui_ctx *ui, const char *id_str, const cui_scatter_series *series, int count, const cui_scatter_opts *o) {
    cui_scatter_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    count = CUI_MAX(0, count);
    cui_id id = cui_make_id(ui, id_str);
    cui__viz_state *st = cui__viz_state_get(ui, id);
    cui_interact r = cui__interact(ui, id, 0);
    cui_rect pr = cui__prev_rect(ui, id);
    float h = o->height > 0 ? o->height : 280;
    float legend_h = 0;
    if (count >= 2 && !o->no_legend) {
        const char **names = cui__frame_alloc(ui, sizeof(char *) * (size_t)count);
        cui_color *cols = cui__frame_alloc(ui, sizeof(cui_color) * (size_t)count);
        for (int k = 0; k < count; k++) { names[k] = series[k].name; cols[k] = cui__viz_color(ui, series[k].color, k); }
        legend_h = cui__viz_legend(ui, 0, 0, pr.w > 0 ? pr.w : 500, names, cols, count, -1, false) + 12;
    }
    int hs = -1, hi = -1;
    if (r.hovered && pr.w > 0) {
        cui__sc_geo g = cui__sc_layout(ui, pr, series, count, o, legend_h);
        float best = 24 * 24;
        for (int k = 0; k < count; k++)
            for (int i = 0; i < series[k].count; i++) {
                cui_vec2 q = cui__sc_pt(&g, series[k].points[i]);
                float rr = cui__sc_radius(&g, &series[k], i);
                float dx = ui->mx - q.x, dy = ui->my - q.y, d2 = dx * dx + dy * dy - rr * rr * 0.6f;
                if (d2 < best) { best = d2; hs = k; hi = i; }
            }
    }
    int flat = -1;
    if (hs >= 0) { flat = hi; for (int k = 0; k < hs; k++) flat += series[k].count; }
    cui__viz_hover(ui, st, flat);
    cui__sc_paint *p = cui__frame_alloc(ui, sizeof *p);
    memset(p, 0, sizeof *p);
    p->s = cui__frame_alloc(ui, sizeof(cui_scatter_series) * (size_t)CUI_MAX(1, count));
    for (int k = 0; k < count; k++) {
        p->s[k] = series[k];
        cui_vec2 *pts = cui__frame_alloc(ui, sizeof(cui_vec2) * (size_t)CUI_MAX(1, series[k].count));
        if (series[k].count > 0) memcpy(pts, series[k].points, sizeof(cui_vec2) * (size_t)series[k].count);
        p->s[k].points = pts;
        if (series[k].sizes) p->s[k].sizes = cui__copy_floats(ui, series[k].sizes, series[k].count);
    }
    p->count = count; p->o = *o; p->legend_h = legend_h; p->intro = st->intro.x;
    p->hs = p->hi = -1;
    if (st->hover_t > 0.01f && st->shown >= 0) {
        int f = st->shown;
        for (int k = 0; k < count; k++) { if (f < series[k].count) { p->hs = k; p->hi = f; break; } f -= series[k].count; }
    }
    if (p->hs >= 0) {
        const cui_scatter_series *s = &series[p->hs];
        p->tip.alpha = st->hover_t; p->tip.x = ui->mx; p->tip.y = ui->my; p->tip.bounds = pr;
        p->tip.title = s->name ? s->name : "";
        cui__tip_row(&p->tip, o->x_label ? o->x_label : "x", cui__fmt_value(ui, o->x_format, s->points[p->hi].x), cui__viz_color(ui, s->color, p->hs));
        cui__tip_row(&p->tip, o->y_label ? o->y_label : "y", cui__fmt_value(ui, o->y_format, s->points[p->hi].y), cui__rgba(0, 0, 0, 0));
        if (s->sizes) cui__tip_row(&p->tip, "Size", cui__fmt_value(ui, NULL, s->sizes[p->hi]), cui__rgba(0, 0, 0, 0));
    }
    cui__viz_node(ui, id, o->width.kind ? o->width : CUI_FILL, CUI_PX(h), cui__paint_scatter, p, flat >= 0);
    if (flat >= 0) cui__set_cursor(ui, CUI_CURSOR_HAND);
    return r.clicked ? flat : -1;
}
