/* Tree with several selected rows, drag and drop and OS file drops (opt in).
 *
 * cui_tree_multi_begin / _end wrap a tree built with cui_tree_begin. Rows that carry an item report to the
 * wrapper (cui__tsel_row, called from cui_tree_begin); everything that needs the whole list of rows (a Shift
 * range, Up and Down, where a drag would land) uses the rows of the frame before, with their rectangles, or is
 * settled in _end when every row of this frame is known. Selection and drag state live in the app's cui_tree_sel.
 * Outline: selection set, input (begin), rows, resolve (end), preview. */
#include "cui_internal.h"

typedef struct { uint64_t item, parent; cui_id id; bool folder; } cui__tsv;

struct cui_tree_sel_priv {
    /* the rows shown: [cur] is being built, the other is the frame before */
    cui__tsv *vis[2]; int vis_n[2], vis_cap[2]; int cur;
    /* a hash of the selected items, rebuilt when dirty */
    uint64_t *hash; int hash_n; bool dirty;
    /* the dragged items (and the dropped ones, until the next drag) */
    uint64_t *drag; int drag_n, drag_cap;
    char drag_label[96];
    /* pointer */
    bool pend; uint64_t pend_item; float pend_x, pend_y; char pend_label[96]; bool pend_mod;
    bool dragging, cancelled;
    /* drop target */
    bool tvalid, tdirect, denied;
    uint64_t target;
    double since;
    uint64_t expanded;
    /* things the rows saw this frame */
    uint64_t click_item; int click_mods;
    uint64_t ctx_item; bool ctx;
    uint64_t focus_item;
};

typedef struct cui__tsel_build {
    const cui_tree_multi_opts *o;
    cui_tree_sel *sel;
    struct cui_tree_sel_priv *p;
    cui_id id;
    int col;
    cui_rect view; bool has_view;
    cui_tree_multi_result res;
    struct cui__tsel_build *prev;
} cui__tsel_build;

static struct cui_tree_sel_priv *sel_priv(cui_tree_sel *s) {
    if (!s->p) s->p = calloc(1, sizeof *s->p);
    return s->p;
}

/* ---- the selection ---- */

static void sel_rehash(cui_tree_sel *s, struct cui_tree_sel_priv *p) {
    int n = 16;
    while (n < s->count * 2 + 2) n *= 2;
    free(p->hash);
    p->hash = calloc((size_t)n, sizeof *p->hash);
    p->hash_n = p->hash ? n : 0;
    p->dirty = false;
    for (int i = 0; p->hash && i < s->count; i++) {
        uint64_t k = s->items[i];
        int h = (int)((k * 0x9E3779B97F4A7C15ull) >> 40) & (n - 1);
        while (p->hash[h]) h = (h + 1) & (n - 1);
        p->hash[h] = k;
    }
}

bool cui_tree_sel_has(const cui_tree_sel *s, uint64_t item) {
    if (!s || !s->p || !item || s->count == 0) return false;
    struct cui_tree_sel_priv *p = s->p;
    if (p->dirty || !p->hash) sel_rehash((cui_tree_sel *)s, p);
    if (!p->hash) return false;
    int n = p->hash_n, h = (int)((item * 0x9E3779B97F4A7C15ull) >> 40) & (n - 1);
    while (p->hash[h]) {
        if (p->hash[h] == item) return true;
        h = (h + 1) & (n - 1);
    }
    return false;
}

void cui_tree_sel_add(cui_tree_sel *s, uint64_t item) {
    if (!s || !item || cui_tree_sel_has(s, item)) return;
    struct cui_tree_sel_priv *p = sel_priv(s);
    if (!p) return;
    if (s->count == s->cap) {
        int nc = s->cap ? s->cap * 2 : 16;
        uint64_t *ni = realloc(s->items, (size_t)nc * sizeof *ni);
        if (!ni) return;
        s->items = ni; s->cap = nc;
    }
    s->items[s->count++] = item;
    p->dirty = true;   /* the next lookup rebuilds the table */
}

void cui_tree_sel_clear(cui_tree_sel *s) {
    if (!s) return;
    s->count = 0;
    if (s->p) s->p->dirty = true;
}

void cui_tree_sel_set(cui_tree_sel *s, uint64_t item) {
    if (!s) return;
    cui_tree_sel_clear(s);
    cui_tree_sel_add(s, item);
    s->anchor = s->cursor = item;
}

void cui_tree_sel_free(cui_tree_sel *s) {
    if (!s) return;
    if (s->p) {
        for (int i = 0; i < 2; i++) free(s->p->vis[i]);
        free(s->p->hash);
        free(s->p->drag);
        free(s->p);
    }
    free(s->items);
    memset(s, 0, sizeof *s);
}

static void sel_remove(cui_tree_sel *s, uint64_t item) {
    for (int i = 0; i < s->count; i++)
        if (s->items[i] == item) {
            memmove(&s->items[i], &s->items[i + 1], (size_t)(s->count - i - 1) * sizeof *s->items);
            s->count--;
            if (s->p) s->p->dirty = true;
            return;
        }
}

static int vis_index(const cui__tsv *v, int n, uint64_t item) {
    for (int i = 0; i < n; i++) if (v[i].item == item) return i;
    return -1;
}

/* the rows a..b (either order) of the rows shown; replaces the selection unless add */
static void sel_range(cui_tree_sel *s, const cui__tsv *v, int n, int a, int b, bool add) {
    if (!add) cui_tree_sel_clear(s);
    if (a > b) { int t = a; a = b; b = t; }
    for (int i = a; i <= b && i < n; i++) cui_tree_sel_add(s, v[i].item);
}

static bool sel_equal(const cui_tree_sel *s, const uint64_t *before, int n) {
    if (s->count != n) return false;
    for (int i = 0; i < n; i++) if (s->items[i] != before[i]) return false;
    return true;
}

/* ---- where the pointer is ---- */

static bool tsel_over_column(cui_ctx *ui, const cui__tsel_build *b, float x, float y) {
    if (!cui__has_prev_rect(ui, b->id)) return false;
    cui_rect c = cui__prev_rect(ui, b->id);
    if (b->has_view) {   /* the column is as high as its rows; the visible part is the scroll area */
        if (!cui__rect_contains(b->view, x, y)) return false;
        c.y = b->view.y; c.h = b->view.h;
    }
    return cui__rect_contains(c, x, y);
}

/* the folder a drop at (x, y) goes to: the folder row there, a file row's parent, or the root */
static void tsel_find_target(cui_ctx *ui, cui__tsel_build *b, float x, float y) {
    struct cui_tree_sel_priv *p = b->p;
    int prev = p->cur ^ 1;
    uint64_t t = 0;
    bool direct = false, found = false;
    if (!b->has_view || cui__rect_contains(b->view, x, y)) {
        for (int i = 0; i < p->vis_n[prev]; i++) {
            const cui__tsv *v = &p->vis[prev][i];
            if (!cui__has_prev_rect(ui, v->id)) continue;
            if (!cui__rect_contains(cui__prev_rect(ui, v->id), x, y)) continue;
            found = true;
            if (v->folder) { t = v->item; direct = true; }
            else t = v->parent ? v->parent : b->o->root_item;
            break;
        }
        if (!found && tsel_over_column(ui, b, x, y)) t = b->o->root_item;
    }
    bool valid = t != 0;
    if (!valid || !p->tvalid || t != p->target || direct != p->tdirect) p->since = ui->time;
    p->tvalid = valid; p->target = t; p->tdirect = direct;
}

static void tsel_clear_target(struct cui_tree_sel_priv *p) { p->tvalid = false; p->denied = false; p->target = 0; p->tdirect = false; }

/* ---- begin: pointer input before the rows are built ---- */

static void tsel_scroll_for_drag(cui_ctx *ui, cui__tsel_build *b) {
    if (!b->o->scroll_id || !b->has_view) return;
    float edge = 28, speed = 520;
    float dir = 0;
    if (ui->my < b->view.y + edge) dir = -(1.0f - cui__clampf((ui->my - b->view.y) / edge, 0, 1));
    else if (ui->my > b->view.y + b->view.h - edge) dir = 1.0f - cui__clampf((b->view.y + b->view.h - ui->my) / edge, 0, 1);
    if (dir == 0) return;
    cui_scroll_info si = cui_scroll_get(ui, b->o->scroll_id);
    if (!si.found) return;
    float dt = ui->dt > 0.05f ? 0.05f : ui->dt;
    cui_scroll_set(ui, b->o->scroll_id, si.y + dir * speed * dt, true);
    ui->animating = true;
}

static void tsel_start_drag(cui_ctx *ui, cui__tsel_build *b) {
    cui_tree_sel *s = b->sel;
    struct cui_tree_sel_priv *p = b->p;
    int mods = cui_key_mods(ui);
    if (!cui_tree_sel_has(s, p->pend_item)) {   /* dragging a row outside the selection drags that row */
        if (mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) cui_tree_sel_add(s, p->pend_item);
        else cui_tree_sel_clear(s), cui_tree_sel_add(s, p->pend_item);
        s->anchor = s->cursor = p->pend_item;
        b->res.selection_changed = true;
    }
    if (s->count > p->drag_cap) {
        uint64_t *nd = realloc(p->drag, (size_t)s->count * sizeof *nd);
        if (!nd) return;
        p->drag = nd; p->drag_cap = s->count;
    }
    memcpy(p->drag, s->items, (size_t)s->count * sizeof *p->drag);
    p->drag_n = s->count;
    if (s->count == 1) snprintf(p->drag_label, sizeof p->drag_label, "%s", p->pend_label);
    else snprintf(p->drag_label, sizeof p->drag_label, "%d items", s->count);
    p->dragging = true;
    p->pend = false;
    tsel_clear_target(p);
}

static void tsel_pointer(cui_ctx *ui, cui__tsel_build *b) {
    struct cui_tree_sel_priv *p = b->p;
    const cui_tree_multi_opts *o = b->o;
    bool down = ui->mdown[CUI_MOUSE_LEFT];
    int prev = p->cur ^ 1;
    if (o->drag && p->pend && down && !p->dragging && !p->cancelled && p->vis_n[prev] > 0) {
        float dx = ui->mx - p->pend_x, dy = ui->my - p->pend_y;
        if (dx * dx + dy * dy > 25.0f) tsel_start_drag(ui, b);
    }
    if (!down) p->pend = false;
    if (p->dragging) {
        if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
            p->dragging = false; p->cancelled = true;
            tsel_clear_target(p);
            if (ui->active == b->id) ui->active = 0;
        } else {
            tsel_find_target(ui, b, ui->mx, ui->my);
            p->denied = p->tvalid && o->can_drop && !o->can_drop(o->user, p->drag, p->drag_n, p->target);
            cui__set_cursor(ui, p->denied ? CUI_CURSOR_NOT_ALLOWED : CUI_CURSOR_MOVE);
            tsel_scroll_for_drag(ui, b);
            ui->animating = true;
            if (!down) {
                if (p->tvalid && !p->denied) {
                    b->res.dropped = true;
                    b->res.drop_items = p->drag;
                    b->res.drop_count = p->drag_n;
                    b->res.drop_target = p->target;
                    b->res.drop_copy = (cui_key_mods(ui) & CUI_MOD_CTRL) != 0;
                }
                p->dragging = false;
                tsel_clear_target(p);
            }
        }
    } else if (o->os_drop && ui->drag_hover && !cui_is_disabled(ui)) {   /* files over the window */
        tsel_find_target(ui, b, ui->drag_x, ui->drag_y);
        p->denied = false;
        tsel_scroll_for_drag(ui, b);
        ui->animating = true;
    } else {
        tsel_clear_target(p);
    }
    if (!down) p->cancelled = false;
    if (o->os_drop && ui->drop_count > 0 && !ui->drop_claimed && !cui_is_disabled(ui)) {
        tsel_find_target(ui, b, ui->drop_x, ui->drop_y);
        if (p->tvalid) {
            b->res.os_dropped = true;
            b->res.os_target = p->target;
            b->res.os_count = ui->drop_count;
            ui->drop_claimed = true;
        }
        tsel_clear_target(p);
    }
    b->res.dragging = p->dragging;
}

void cui_tree_multi_begin_ex(cui_ctx *ui, const cui_tree_multi_opts *o) {
    cui__tsel_build *b = cui__frame_alloc(ui, sizeof *b);
    memset(b, 0, sizeof *b);
    cui_tree_multi_opts none;
    memset(&none, 0, sizeof none);
    b->o = o ? o : &none;
    b->sel = b->o->sel;
    b->id = cui_make_id(ui, "cui_tree_multi") ^ 0x7EE5E1u;
    b->prev = ui->building.tree_multi;
    ui->building.tree_multi = b;
    if (b->sel && (b->p = sel_priv(b->sel))) {
        struct cui_tree_sel_priv *p = b->p;
        p->cur ^= 1;
        p->vis_n[p->cur] = 0;
        p->focus_item = 0; p->click_item = 0; p->ctx = false; p->ctx_item = 0; p->expanded = 0;
        if (b->o->scroll_id) {
            cui_id sid = cui_make_id(ui, b->o->scroll_id);
            if (cui__has_prev_rect(ui, sid)) { b->view = cui__prev_rect(ui, sid); b->has_view = true; }
        }
        tsel_pointer(ui, b);
    }
    int col = cui__node_new(ui, b->id);
    ui->nodes[col].w = CUI_FILL;
    ui->nodes[col].gap = 1;
    cui__open(ui, col);
    b->col = col;
    if (b->p && b->p->dragging) ui->active = b->id;   /* keep the pointer while dragging */
}

/* ---- rows ---- */

bool cui__tsel_row(cui_ctx *ui, cui_id id, const cui_tree_opts *o, const char *label, bool folder, bool is_open,
                   const cui_interact *r, cui__tsel_flags *out) {
    cui__tsel_build *b = ui->building.tree_multi;
    if (!b || !b->p || !o->item) return false;
    struct cui_tree_sel_priv *p = b->p;
    memset(out, 0, sizeof *out);
    int cur = p->cur;
    if (p->vis_n[cur] == p->vis_cap[cur]) {
        int nc = p->vis_cap[cur] ? p->vis_cap[cur] * 2 : 64;
        cui__tsv *nv = realloc(p->vis[cur], (size_t)nc * sizeof *nv);
        if (!nv) return false;
        p->vis[cur] = nv; p->vis_cap[cur] = nc;
    }
    cui__tsv *v = &p->vis[cur][p->vis_n[cur]++];
    v->item = o->item; v->parent = o->parent; v->id = id; v->folder = folder;
    out->selected = cui_tree_sel_has(b->sel, o->item);
    int mods = cui_key_mods(ui);
    if (r->keyboard_focused) p->focus_item = o->item;
    if (r->hovered && ui->mpressed[CUI_MOUSE_LEFT]) {
        p->pend = true; p->pend_item = o->item; p->pend_x = ui->mx; p->pend_y = ui->my;
        snprintf(p->pend_label, sizeof p->pend_label, "%s", label);
    }
    if (r->right_clicked) { p->ctx = true; p->ctx_item = o->item; }
    if (r->clicked) {
        if (r->keyboard_activate) { b->sel->cursor = o->item; }
        else {
            p->click_item = o->item; p->click_mods = mods;
            out->mod_click = (mods & (CUI_MOD_CTRL | CUI_MOD_SUPER | CUI_MOD_SHIFT)) != 0;
        }
    }
    if (p->tvalid && !p->denied && p->target == o->item && p->tdirect) {
        out->target = true;
        if (folder && !is_open) {
            float delay = b->o->expand_delay > 0 ? b->o->expand_delay : 0.6f;
            float left = delay - (float)(ui->time - p->since);
            if (left <= 0) {
                out->expand = true;
                p->expanded = o->item;
                p->since = ui->time;
            } else {
                cui__wakeup_after(ui, left + 0.01f);
            }
        }
    }
    return true;
}

/* ---- end: settle what needed every row ---- */

static void tsel_keep_in_view(cui_ctx *ui, const cui__tsel_build *b, cui_id row) {
    if (!b->o->scroll_id || !b->has_view || !cui__has_prev_rect(ui, row)) return;
    cui_rect r = cui__prev_rect(ui, row);
    cui_scroll_info si = cui_scroll_get(ui, b->o->scroll_id);
    if (!si.found) return;
    if (r.y < b->view.y) cui_scroll_set(ui, b->o->scroll_id, si.target_y - (b->view.y - r.y) - 6, false);
    else if (r.y + r.h > b->view.y + b->view.h) cui_scroll_set(ui, b->o->scroll_id, si.target_y + (r.y + r.h - b->view.y - b->view.h) + 6, false);
}

/* Up, Down, Home, End, Ctrl+A, Delete and F2 for the row that has the keyboard */
static void tsel_keys(cui_ctx *ui, cui__tsel_build *b) {
    cui_tree_sel *s = b->sel;
    struct cui_tree_sel_priv *p = b->p;
    const cui__tsv *v = p->vis[p->cur];
    int n = p->vis_n[p->cur];
    if (!p->focus_item || n == 0) return;
    int mods = cui_key_mods(ui);
    bool shift = (mods & CUI_MOD_SHIFT) != 0, ctrl = (mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) != 0;
    int at = vis_index(v, n, p->focus_item);
    if (at < 0) return;
    s->cursor = p->focus_item;
    int to = at;
    if (cui_key_pressed(ui, CUI_KEY_DOWN)) to = at + 1 < n ? at + 1 : at;
    else if (cui_key_pressed(ui, CUI_KEY_UP)) to = at > 0 ? at - 1 : 0;
    else if (cui_key_pressed(ui, CUI_KEY_HOME)) to = 0;
    else if (cui_key_pressed(ui, CUI_KEY_END)) to = n - 1;
    if (to != at) {
        if (shift) {
            int a = vis_index(v, n, s->anchor);
            if (a < 0) { a = at; s->anchor = v[at].item; }
            sel_range(s, v, n, a, to, false);
        } else if (!ctrl) {
            cui_tree_sel_set(s, v[to].item);
        }
        s->cursor = v[to].item;
        b->res.selection_changed = true;
        ui->focus = v[to].id;
        ui->focus_visible = true;
        tsel_keep_in_view(ui, b, v[to].id);
    }
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_A)) {
        sel_range(s, v, n, 0, n - 1, false);
        b->res.selection_changed = true;
    }
    if (cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_F2)) {
        if (s->count == 0) { cui_tree_sel_set(s, p->focus_item); b->res.selection_changed = true; }
        if (cui_key_pressed(ui, CUI_KEY_DELETE)) b->res.key_delete = true;
        else b->res.key_rename = true;
    }
}

/* a click settles here: a Shift range needs every row shown, in order */
static void tsel_click(cui_ctx *ui, cui__tsel_build *b) {
    (void)ui;
    cui_tree_sel *s = b->sel;
    struct cui_tree_sel_priv *p = b->p;
    const cui__tsv *v = p->vis[p->cur];
    int n = p->vis_n[p->cur];
    uint64_t it = p->click_item;
    bool shift = (p->click_mods & CUI_MOD_SHIFT) != 0, ctrl = (p->click_mods & (CUI_MOD_CTRL | CUI_MOD_SUPER)) != 0;
    if (shift) {
        int a = vis_index(v, n, s->anchor), c = vis_index(v, n, it);
        if (a < 0) { a = c; s->anchor = it; }
        if (c >= 0) sel_range(s, v, n, a, c, ctrl);
    } else if (ctrl) {
        if (cui_tree_sel_has(s, it)) sel_remove(s, it); else cui_tree_sel_add(s, it);
        s->anchor = it;
    } else {
        cui_tree_sel_set(s, it);
    }
    s->cursor = it;
}

static void tsel_preview(cui_ctx *ui, cui__tsel_build *b) {
    struct cui_tree_sel_priv *p = b->p;
    int g = cui__float_begin(ui, b->id ^ 0x6057u, 1900, 0, CUI__PLACE_POINT, 0);
    cui_node *n = &ui->nodes[g];
    n->fx = ui->mx + 14;
    n->fy = ui->my + 12;
    n->dir = CUI_ROW; n->align = CUI_ALIGN_CENTER; n->gap = 6;
    n->h = CUI_PX(28);
    n->pad[1] = n->pad[3] = 10;
    n->radius = CUI_CORNERS(CUI__R_ITEM(ui));
    n->bg = CUI_C(ui, SURFACE_RAISED);
    n->border = 1;
    n->border_color = p->denied ? CUI_C(ui, DANGER) : CUI_C(ui, BORDER);
    n->elevation = CUI_ELEVATION_FLOATING;
    n->opacity = 0.96f;
    cui__icon_node(ui, p->drag_n > 1 ? CUI_ICON_COPY : CUI_ICON_FILE, 14, CUI_C(ui, FOREGROUND));
    int t = cui__text_node(ui, p->drag_label, cui__display_len(p->drag_label), CUI_FONT_BODY, cui__fs(ui, ui->theme.text_body),
                           CUI_C(ui, FOREGROUND), false);
    ui->nodes[t].line_height = 1.3f;
    ui->nodes[t].max_w = 220;
    cui__float_end(ui);
}

cui_tree_multi_result cui_tree_multi_end(cui_ctx *ui) {
    cui_tree_multi_result none;
    memset(&none, 0, sizeof none);
    cui__tsel_build *b = ui->building.tree_multi;
    if (!b) return none;
    cui__close(ui);   /* the column */
    ui->building.tree_multi = b->prev;
    if (!b->p) return b->res;
    cui_tree_sel *s = b->sel;
    struct cui_tree_sel_priv *p = b->p;
    uint64_t *before = NULL;
    int nbefore = s->count;
    if (nbefore > 0) {
        before = malloc((size_t)nbefore * sizeof *before);
        if (before) memcpy(before, s->items, (size_t)nbefore * sizeof *before); else nbefore = 0;
    }
    if (p->click_item) tsel_click(ui, b);
    if (p->ctx) {
        if (!cui_tree_sel_has(s, p->ctx_item)) cui_tree_sel_set(s, p->ctx_item);
        s->cursor = p->ctx_item;
        b->res.context = true;
        b->res.context_item = p->ctx_item;
        b->res.context_x = ui->mx; b->res.context_y = ui->my;
    }
    if (p->focus_item || p->click_item) tsel_keys(ui, b);
    if (before ? !sel_equal(s, before, nbefore) : s->count != 0) b->res.selection_changed = true;
    free(before);
    b->res.expanded = p->expanded;
    if (p->dragging) tsel_preview(ui, b);
    b->res.dragging = p->dragging;
    return b->res;
}
