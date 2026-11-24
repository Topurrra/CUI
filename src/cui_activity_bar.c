/* Activity bar: the narrow column of icons on a window's edge that switches what the side bar shows (an IDE's
 * Files, Search, Source control, Run...). One item is selected and marked with a bar on its outer edge; items can sit
 * at the bottom (account, settings) and can be plain buttons that never take the selection. Items can start groups
 * (a hairline above them), and with .fold the top items that do not fit a short bar go into a More menu. */
#include "cui_internal.h"

typedef struct { float on, hover; } cui__ab_state;
typedef struct { float on; bool right; cui_color c; } cui__ab_paint;

#define CUI__AB_GAP 2.0f
#define CUI__AB_PAD 4.0f
#define CUI__AB_SEP 9.0f        /* a group's hairline with its room above and below */
#define CUI__AB_MORE (-1)       /* the More button in the display order */

static void cui__paint_ab_item(cui_ctx *ui, cui_node *n, void *data) {
    const cui__ab_paint *p = data;
    if (p->on <= 0.01f) return;
    cui_rect r = n->rect;
    float h = r.h * 0.5f * (0.4f + 0.6f * p->on);
    float x = p->right ? r.x + r.w - 2 : r.x;
    cui_draw_rect(ui, (cui_rect){ x, r.y + (r.h - h) * 0.5f, 2, h }, CUI_CORNERS(1), cui_alpha(p->c, p->on));
}

static void cui__paint_ab_sep(cui_ctx *ui, cui_node *n, void *data) {
    cui_color c = *(const cui_color *)data;
    cui_rect r = n->rect;
    float w = r.w * 0.42f;
    cui_draw_rect(ui, (cui_rect){ r.x + (r.w - w) * 0.5f, r.y + (r.h - 1) * 0.5f, w, 1 }, CUI_CORNERS(0), c);
}

/* What one frame of the bar works with, shared by its phases. */
typedef struct {
    cui_ctx *ui;
    const cui_activity_item *items;
    const cui_activity_bar_opts *o;
    cui_id id;
    float w;
    int sel, clicked;
    cui_color bar_bg;
    int top[256], nt, bot[256], nb;   /* the top items, then the bottom ones, in the caller's order */
    int shown_top;                     /* how many top items show; the rest fold into More */
    bool more;                         /* the More button shows */
    float spacer;                      /* with .fold, the room between the top and bottom items; -1: it grows */
    int order[514], m;                 /* what shows, top to bottom (CUI__AB_MORE for More) */
    bool moved;                        /* one focus move a frame: the item it lands on sees the key too */
} cui__ab;

static float cui__ab_height(const cui__ab *b, int shown_top, bool more) {
    int nodes = 0;
    float h = 2 * CUI__AB_PAD;
    for (int j = 0; j < shown_top; j++) {
        if (j > 0 && b->items[b->top[j]].group) { h += CUI__AB_SEP; nodes++; }
        h += b->w; nodes++;
    }
    if (more) { h += b->w; nodes++; }
    if (b->nb > 0) nodes++;              /* the spacer, which can be empty */
    for (int j = 0; j < b->nb; j++) {
        if (j > 0 && b->items[b->bot[j]].group) { h += CUI__AB_SEP; nodes++; }
        h += b->w; nodes++;
    }
    return h + (nodes > 1 ? CUI__AB_GAP * (float)(nodes - 1) : 0);
}

/* how many top items show: all, or (with .fold, in a bar shorter than they need) the first ones and More */
static void cui__ab_fit(cui__ab *b, int count) {
    for (int k = 0; k < count; k++) {
        if (b->items[k].bottom) { if (b->nb < 256) b->bot[b->nb++] = k; }
        else if (b->nt < 256) b->top[b->nt++] = k;
    }
    b->shown_top = b->nt;
    b->more = false;
    b->spacer = -1;
    if (!b->o->fold) return;
    float avail = cui__prev_rect(b->ui, b->id).h;   /* the space the bar had last frame; 0 on its first */
    if (avail <= 0) return;
    if (cui__ab_height(b, b->nt, false) > avail + 0.5f) {
        b->more = true;
        b->shown_top = b->nt - 1;
        while (b->shown_top > 0 && cui__ab_height(b, b->shown_top, true) > avail + 0.5f) b->shown_top--;
    }
    /* a folding bar lays out as a scroll area (its items never push it taller), where a growing child does not grow:
     * the room is measured instead, so the bottom items sit at the bottom */
    float rest = avail - cui__ab_height(b, b->shown_top, b->more);
    b->spacer = rest > 0 ? rest : 0;
}

static bool cui__ab_folded(const cui__ab *b, int k) {
    for (int j = b->shown_top; j < b->nt; j++) if (b->top[j] == k) return true;
    return false;
}

static cui_id cui__ab_item_id(const cui__ab *b, int k) {
    int key = k == CUI__AB_MORE ? 0x3F3F : k;
    return cui__hash(&key, sizeof key, b->id ^ 0xAC7u);
}

static void cui__ab_separator(cui__ab *b) {
    cui_ctx *ui = b->ui;
    int s = cui__leaf(ui, 0, CUI_PX(b->w), CUI_PX(CUI__AB_SEP));
    cui_color *c = CUI_PAINT_DATA(ui, cui_color);
    *c = CUI_C(ui, BORDER);
    ui->nodes[s].paint = cui__paint_ab_sep;
    ui->nodes[s].paint_data = c;
}

/* one icon: input, the selection mark, the badge and the tooltip. j is its place in b->order. */
static void cui__ab_item(cui__ab *b, int j) {
    cui_ctx *ui = b->ui;
    const cui_activity_bar_opts *o = b->o;
    int k = b->order[j];
    bool is_more = k == CUI__AB_MORE;
    const cui_activity_item *it = is_more ? NULL : &b->items[k];
    cui_id iid = cui__ab_item_id(b, k);
    cui_interact r = cui__interact(ui, iid, CUI_IF_FOCUSABLE);
    if (r.clicked || r.keyboard_activate) {
        if (is_more) {
            cui_rect pr = cui__prev_rect(ui, iid);
            cui_context_menu_open_at(ui, "fold", o->right ? pr.x - 4 : pr.x + pr.w + 4, pr.y);
        } else {
            b->clicked = k;
            if (!it->button) b->sel = (o->toggle && b->sel == k) ? -1 : k;
        }
    }
    if (r.focused && !b->moved) {   /* arrows move the focus along the bar */
        int step = cui_key_pressed(ui, CUI_KEY_DOWN) ? 1 : cui_key_pressed(ui, CUI_KEY_UP) ? -1 : 0;
        if (step && j + step >= 0 && j + step < b->m) { ui->focus = cui__ab_item_id(b, b->order[j + step]); b->moved = true; }
    }
    bool on = is_more ? (b->sel >= 0 && cui__ab_folded(b, b->sel)) : (!it->button && k == b->sel);
    cui__ab_state *st = cui_state(ui, iid, sizeof *st);
    cui__tween(ui, &st->on, on ? 1.0f : 0.0f, CUI__T_HOVER);
    cui__tween(ui, &st->hover, r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
    float w = b->w;
    int in = cui__node_new(ui, iid);
    cui_node *itn = &ui->nodes[in];
    itn->w = CUI_PX(w); itn->h = CUI_PX(w);
    itn->dir = CUI_STACK; itn->align = CUI_ALIGN_CENTER; itn->justify = CUI_JUSTIFY_CENTER;
    cui__ab_paint *p = cui__frame_alloc(ui, sizeof *p);
    p->on = st->on; p->right = o->right; p->c = CUI_C(ui, FOREGROUND);
    itn->paint = cui__paint_ab_item;
    itn->paint_data = p;
    cui__mark_interactive(ui, in, CUI_IF_FOCUSABLE);
    cui__focus_ring(ui, in, r.focus_visible ? 1.0f : 0.0f);
    if (r.hovered) cui__set_cursor(ui, CUI_CURSOR_HAND);
    cui__open(ui, in);
    float lit = st->on > st->hover ? st->on : st->hover;
    cui_color fg = cui_mix(CUI_C(ui, TEXT_MUTED), CUI_C(ui, FOREGROUND), lit);
    const char *icon = is_more ? CUI_ICON_ELLIPSIS : it->icon ? it->icon : CUI_ICON_CIRCLE;
    cui__icon_node(ui, icon, w * 0.5f, fg);
    if (it && it->badge > 0) {
        const char *bs = it->badge > 99 ? "99+" : cui__frame_printf(ui, "%d", it->badge);
        float bw = it->badge > 9 ? 22.0f : 16.0f;
        int bd = cui__absolute(ui, 0, w * 0.5f + 1, w * 0.5f - 1, bw, 16);
        cui_node *bn = &ui->nodes[bd];
        bn->radius = CUI_CORNERS(8);
        bn->bg = cui__color_set(o->badge_color) ? cui_color_of(ui, o->badge_color) : CUI_C(ui, ACCENT);
        bn->border = 1.5f; bn->border_color = b->bar_bg;
        bn->dir = CUI_STACK; bn->align = CUI_ALIGN_CENTER; bn->justify = CUI_JUSTIFY_CENTER;
        cui__open(ui, bd);
        int bt = cui__text_node(ui, bs, -1, CUI_FONT_MEDIUM, cui__fs(ui, 10), cui__rgba(255, 255, 255, 255), false);
        ui->nodes[bt].line_height = 1.0f;
        cui__close(ui);
    }
    cui__close(ui);
    ui->last_id = iid;
    const char *tip = is_more ? "More" : it->tooltip;
    /* beside the bar, not above the icon: above, it would cover the icon over this one */
    if (tip) cui_tooltip_ex(ui, tip, &(cui_tooltip_opts){ ._ = 0, .placement = o->right ? CUI_PLACE_LEFT : CUI_PLACE_RIGHT });
}

/* the More button's menu: the folded items by name; choosing one is a click on it */
static void cui__ab_fold_menu(cui__ab *b) {
    cui_ctx *ui = b->ui;
    if (!cui_context_menu_begin(ui, "fold")) return;
    for (int j = b->shown_top; j < b->nt; j++) {
        int k = b->top[j];
        const cui_activity_item *it = &b->items[k];
        cui_push_id(ui, cui__frame_printf(ui, "%d", k));
        bool chosen = cui_menu_item_ex(ui, it->tooltip ? it->tooltip : "", &(cui_menu_item_opts){
            ._ = 0, .icon = it->icon, .checked = !it->button && k == b->sel });
        cui_pop_id(ui);
        if (chosen) {
            b->clicked = k;
            if (!it->button) b->sel = (b->o->toggle && b->sel == k) ? -1 : k;
        }
    }
    cui_context_menu_end(ui);
}

int cui_activity_bar_ex(cui_ctx *ui, const char *id_str, const cui_activity_item *items, int count, int *selected, const cui_activity_bar_opts *o) {
    cui_activity_bar_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui__ab *b = cui__frame_alloc(ui, sizeof *b);
    memset(b, 0, sizeof *b);
    b->ui = ui; b->items = items; b->o = o;
    b->id = cui_make_id(ui, id_str);
    b->w = o->width > 0 ? o->width : 48;
    b->sel = selected ? *selected : -1;
    b->clicked = -1;
    if (!items || count < 0) count = 0;
    cui__ab_fit(b, count);

    int n = cui__node_new(ui, b->id);
    cui_node *nd = &ui->nodes[n];
    nd->dir = CUI_COLUMN; nd->align = CUI_ALIGN_CENTER; nd->gap = CUI__AB_GAP;
    nd->pad[0] = CUI__AB_PAD; nd->pad[2] = CUI__AB_PAD;
    nd->w = CUI_PX(b->w);
    nd->h = o->height.kind ? o->height : CUI_FILL;
    /* folding: the bar takes the height it is given, never its items' (they are fitted to it next frame) */
    if (o->fold) nd->flags |= CUI_NF_SCROLL_Y | CUI_NF_CLIP;
    nd->bg = cui__color_set(o->bg) ? cui_color_of(ui, o->bg) : CUI_C(ui, SURFACE_MUTED);
    b->bar_bg = nd->bg;
    cui__open(ui, n);
    cui_push_id(ui, id_str);
    for (int j = 0; j < b->shown_top; j++) b->order[b->m++] = b->top[j];
    if (b->more) b->order[b->m++] = CUI__AB_MORE;
    int first_bottom = b->m;
    for (int j = 0; j < b->nb; j++) b->order[b->m++] = b->bot[j];
    for (int j = 0; j < b->m; j++) {
        int k = b->order[j];
        if (j == first_bottom)   /* everything after this sits at the bottom */
            cui__leaf(ui, 0, CUI_PX(1), b->spacer >= 0 ? CUI_PX(b->spacer) : CUI_GROW(1));
        bool first_of_section = j == 0 || j == first_bottom;
        if (k != CUI__AB_MORE && items[k].group && !first_of_section) cui__ab_separator(b);
        cui__ab_item(b, j);
        if (k == CUI__AB_MORE) cui__ab_fold_menu(b);   /* right after More: a right click on it opens it too */
    }
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = b->id;
    if (selected) *selected = b->sel;
    return b->clicked;
}
