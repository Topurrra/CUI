/* Activity bar: the narrow column of icons on a window's edge that switches what the side bar shows (an IDE's
 * Files, Search, Source control, Run...). One item is selected and marked with a bar on its outer edge; items can sit
 * at the bottom (account, settings) and can be plain buttons that never take the selection. */
#include "cui_internal.h"

typedef struct { float on, hover; } cui__ab_state;
typedef struct { float on; bool right; cui_color c; } cui__ab_paint;

static void cui__paint_ab_item(cui_ctx *ui, cui_node *n, void *data) {
    const cui__ab_paint *p = data;
    if (p->on <= 0.01f) return;
    cui_rect r = n->rect;
    float h = r.h * 0.5f * (0.4f + 0.6f * p->on);
    float x = p->right ? r.x + r.w - 2 : r.x;
    cui_draw_rect(ui, (cui_rect){ x, r.y + (r.h - h) * 0.5f, 2, h }, CUI_CORNERS(1), cui_alpha(p->c, p->on));
}

int cui_activity_bar_ex(cui_ctx *ui, const char *id_str, const cui_activity_item *items, int count, int *selected, const cui_activity_bar_opts *o) {
    cui_activity_bar_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_id id = cui_make_id(ui, id_str);
    float w = o->width > 0 ? o->width : 48;
    int sel = selected ? *selected : -1, clicked = -1;
    int n = cui__node_new(ui, id);
    cui_node *nd = &ui->nodes[n];
    nd->dir = CUI_COLUMN; nd->align = CUI_ALIGN_CENTER; nd->gap = 2;
    nd->pad[0] = 4; nd->pad[2] = 4;
    nd->w = CUI_PX(w);
    nd->h = o->height.kind ? o->height : CUI_FILL;
    nd->bg = cui__color_set(o->bg) ? cui_color_of(ui, o->bg) : CUI_C(ui, SURFACE_MUTED);
    cui_color bar_bg = nd->bg;
    cui__open(ui, n);
    cui_push_id(ui, id_str);
    int order[256], m = 0;   /* the top items, then the bottom ones */
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < count && m < 256; k++)
            if ((items[k].bottom ? 1 : 0) == pass) order[m++] = k;
    bool spacer = false, moved = false;   /* one focus move a frame: the item it lands on sees the key too */
    for (int j = 0; j < m; j++) {
        int k = order[j];
        const cui_activity_item *it = &items[k];
        if (it->bottom && !spacer) {   /* everything after this sits at the bottom */
            cui__leaf(ui, 0, CUI_PX(1), CUI_GROW(1));
            spacer = true;
        }
        cui_id iid = cui__hash(&k, sizeof k, id ^ 0xAC7u);
        cui_interact r = cui__interact(ui, iid, CUI_IF_FOCUSABLE);
        if (r.clicked || r.keyboard_activate) {
            clicked = k;
            if (!it->button) sel = (o->toggle && sel == k) ? -1 : k;
        }
        if (r.focused && !moved) {   /* arrows move the focus along the bar */
            int step = cui_key_pressed(ui, CUI_KEY_DOWN) ? 1 : cui_key_pressed(ui, CUI_KEY_UP) ? -1 : 0;
            if (step && j + step >= 0 && j + step < m) { int t = order[j + step]; ui->focus = cui__hash(&t, sizeof t, id ^ 0xAC7u); moved = true; }
        }
        bool on = !it->button && k == sel;
        cui__ab_state *st = cui_state(ui, iid, sizeof *st);
        cui__tween(ui, &st->on, on ? 1.0f : 0.0f, CUI__T_HOVER);
        cui__tween(ui, &st->hover, r.hovered ? 1.0f : 0.0f, CUI__T_HOVER);
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
        cui__icon_node(ui, it->icon ? it->icon : CUI_ICON_CIRCLE, w * 0.5f, fg);
        if (it->badge > 0) {
            const char *bs = it->badge > 99 ? "99+" : cui__frame_printf(ui, "%d", it->badge);
            float bw = it->badge > 9 ? 22.0f : 16.0f;
            int bd = cui__absolute(ui, 0, w * 0.5f + 1, w * 0.5f - 1, bw, 16);
            cui_node *bn = &ui->nodes[bd];
            bn->radius = CUI_CORNERS(8);
            bn->bg = cui__color_set(o->badge_color) ? cui_color_of(ui, o->badge_color) : CUI_C(ui, ACCENT);
            bn->border = 1.5f; bn->border_color = bar_bg;
            bn->dir = CUI_STACK; bn->align = CUI_ALIGN_CENTER; bn->justify = CUI_JUSTIFY_CENTER;
            cui__open(ui, bd);
            int bt = cui__text_node(ui, bs, -1, CUI_FONT_MEDIUM, cui__fs(ui, 10), cui__rgba(255, 255, 255, 255), false);
            ui->nodes[bt].line_height = 1.0f;
            cui__close(ui);
        }
        cui__close(ui);
        ui->last_id = iid;
        if (it->tooltip) cui_tooltip(ui, it->tooltip);
    }
    cui_pop_id(ui);
    cui__close(ui);
    ui->last_id = id;
    if (selected) *selected = sel;
    return clicked;
}
