/* CUI test suite: layout, interaction, text editing, overlays, theming and
 * the software renderer. Runs headless. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, eps) do { g_checks++; double a_ = (a), b_ = (b); if (fabs(a_ - b_) > (eps)) { g_failed++; \
    printf("  FAIL %s:%d: %s = %g, expected %g\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

static cui_sw_renderer *g_sw;
static char g_clip[1024];
static void clip_set(void *u, const char *s) { (void)u; snprintf(g_clip, sizeof g_clip, "%s", s); }
static const char *clip_get(void *u) { (void)u; return g_clip; }

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_platform(ui, &(cui_platform_iface){ .set_clipboard = clip_set, .get_clipboard = clip_get });
    cui_set_reduced_motion(ui, true);   /* deterministic: animations snap */
    return ui;
}

typedef void (*build_fn)(cui_ctx *ui, void *user);
static void run(cui_ctx *ui, build_fn fn, void *user, int frames) {
    for (int i = 0; i < frames; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        fn(ui, user);
        cui_end_frame(ui);
    }
}

static void click_at(cui_ctx *ui, build_fn fn, void *user, float x, float y) {
    cui_input_mouse_move(ui, x, y);
    run(ui, fn, user, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, fn, user, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, fn, user, 1);
}

static cui_vec2 center(cui_rect r) { cui_vec2 c = { r.x + r.w / 2, r.y + r.h / 2 }; return c; }

/* ------------------------------------------------------------------------ */

static void test_color(void) {
    printf("color\n");
    cui_color w = cui_oklch(1, 0, 0, 1), b = cui_oklch(0, 0, 0, 1);
    CHECK(w.r == 255 && w.g == 255 && w.b == 255);
    CHECK(b.r == 0 && b.g == 0 && b.b == 0);
    cui_color m = cui_mix(w, b, 0.5f);
    CHECK(m.r > 90 && m.r < 140 && m.r == m.g && m.g == m.b);
    cui_color t = cui_mix(CUI_RGB(0xff0000), CUI_TRANSPARENT, 1.0f);
    CHECK(t.a == 0);
    cui_color a = cui_alpha(CUI_RGB(0x123456), 0.5f);
    CHECK(a.a == 128 && a.r == 0x12);
}

/* layout --------------------------------------------------------------- */

typedef struct { cui_id a, b, c; } ids3;

static void build_row(cui_ctx *ui, void *u) {
    ids3 *ids = u;
    cui_row(ui, .width = CUI_PX(500), .height = CUI_PX(100), .pad = 10, .gap = 20) {
        ids->a = cui_box_begin(ui, &(cui_box){ .id = "fixed", .width = CUI_PX(100), .height = CUI_FILL }); cui_box_end(ui);
        ids->b = cui_box_begin(ui, &(cui_box){ .id = "grow1", .width = CUI_GROW(1) }); cui_box_end(ui);
        ids->c = cui_box_begin(ui, &(cui_box){ .id = "grow2", .width = CUI_GROW(2) }); cui_box_end(ui);
    }
}

static cui_rect rect_of(cui_ctx *ui, cui_id id) { return cui_get_rect(ui, id); }

static void test_layout_row(void) {
    printf("layout: row grow and fixed\n");
    cui_ctx *ui = new_ctx();
    ids3 ids;
    run(ui, build_row, &ids, 2);
    cui_rect a = rect_of(ui, ids.a), b = rect_of(ui, ids.b), c = rect_of(ui, ids.c);
    CHECK_NEAR(a.x, 10, 0.01); CHECK_NEAR(a.w, 100, 0.01);
    CHECK_NEAR(a.h, 80, 0.01);                       /* fill height inside padding */
    float free_w = 500 - 20 - 100 - 40;              /* 340 split 1:2 */
    CHECK_NEAR(b.w, free_w / 3, 0.05);
    CHECK_NEAR(c.w, free_w * 2 / 3, 0.05);
    CHECK_NEAR(b.x, 10 + 100 + 20, 0.01);
    CHECK_NEAR(c.x + c.w, 490, 0.05);
    cui_destroy(ui);
}

typedef struct { cui_id col, txt; float width; } wrap_ctx;
static void build_wrap(cui_ctx *ui, void *u) {
    wrap_ctx *w = u;
    w->col = cui_column_begin(ui, &(cui_box){ .id = "col", .width = CUI_PX(w->width) });
    cui_text(ui, "The quick brown fox jumps over the lazy dog and keeps running far beyond the meadow.");
    cui_box_end(ui);
}

static void test_layout_wrap(void) {
    printf("layout: text wraps and grows taller when narrow\n");
    cui_ctx *ui = new_ctx();
    wrap_ctx w = { 0, 0, 600 };
    run(ui, build_wrap, &w, 2);
    float wide_h = rect_of(ui, w.col).h;
    w.width = 120;
    run(ui, build_wrap, &w, 2);
    float narrow_h = rect_of(ui, w.col).h;
    CHECK(narrow_h > wide_h * 3);
    CHECK_NEAR(rect_of(ui, w.col).w, 120, 0.01);
    cui_destroy(ui);
}

typedef struct { cui_id outer, inner; } center_ctx;
static void build_center(cui_ctx *ui, void *u) {
    center_ctx *c = u;
    c->outer = cui_box_begin(ui, &(cui_box){ .id = "outer", .dir = CUI_ROW, .width = CUI_PX(300), .height = CUI_PX(200),
                                             .justify = CUI_JUSTIFY_CENTER, .align = CUI_ALIGN_CENTER });
    c->inner = cui_box_begin(ui, &(cui_box){ .id = "inner", .width = CUI_PX(100), .height = CUI_PX(50) });
    cui_box_end(ui);
    cui_box_end(ui);
}

static void test_layout_center(void) {
    printf("layout: justify and align center\n");
    cui_ctx *ui = new_ctx();
    center_ctx c;
    run(ui, build_center, &c, 2);
    cui_rect r = rect_of(ui, c.inner);
    CHECK_NEAR(r.x, 100, 0.01);
    CHECK_NEAR(r.y, 75, 0.01);
    cui_destroy(ui);
}

typedef struct { cui_id grid; cui_id cells[5]; } grid_ctx;
static void build_grid(cui_ctx *ui, void *u) {
    grid_ctx *g = u;
    g->grid = cui_box_begin(ui, &(cui_box){ .id = "grid", .dir = CUI_GRID, .columns = 2, .gap = 10, .width = CUI_PX(210) });
    for (int i = 0; i < 5; i++) {
        char id[8];
        snprintf(id, sizeof id, "c%d", i);
        g->cells[i] = cui_box_begin(ui, &(cui_box){ .id = id, .height = CUI_PX(30 + i * 10) });
        cui_box_end(ui);
    }
    cui_box_end(ui);
}

static void test_layout_grid(void) {
    printf("layout: grid\n");
    cui_ctx *ui = new_ctx();
    grid_ctx g;
    run(ui, build_grid, &g, 2);
    CHECK_NEAR(rect_of(ui, g.cells[0]).w, 100, 0.01);
    CHECK_NEAR(rect_of(ui, g.cells[1]).x, 110, 0.01);
    CHECK_NEAR(rect_of(ui, g.cells[2]).y, 40 + 10, 0.01);   /* row 0 height = max(30,40) */
    CHECK_NEAR(rect_of(ui, g.grid).h, 40 + 10 + 60 + 10 + 70, 0.01);
    cui_destroy(ui);
}

/* interaction ---------------------------------------------------------- */

typedef struct { int clicks; cui_id id; bool check; bool sw; float slider; } basic_ctx;
static void build_basic(cui_ctx *ui, void *u) {
    basic_ctx *b = u;
    cui_column(ui, .pad = 20, .gap = 10) {
        if (cui_button(ui, "Press")) b->clicks++;
        b->id = cui_last_id(ui);
        cui_checkbox(ui, "Check", &b->check);
        cui_switch(ui, "Switch", &b->sw);
        cui_slider(ui, "Slide", &b->slider, 0, 100, .width = CUI_PX(300));
    }
}

static void test_button_click(void) {
    printf("interaction: button click, hover and drag-off\n");
    cui_ctx *ui = new_ctx();
    basic_ctx b = { 0 };
    run(ui, build_basic, &b, 2);
    cui_rect r = rect_of(ui, b.id);
    CHECK(r.w > 40 && r.h == 36);
    cui_vec2 c = center(r);
    click_at(ui, build_basic, &b, c.x, c.y);
    CHECK(b.clicks == 1);
    run(ui, build_basic, &b, 3);
    CHECK(b.clicks == 1);
    /* press inside, release outside: no click */
    cui_input_mouse_move(ui, c.x, c.y);
    run(ui, build_basic, &b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_basic, &b, 1);
    cui_input_mouse_move(ui, 700, 500);
    run(ui, build_basic, &b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_basic, &b, 1);
    CHECK(b.clicks == 1);
    /* press and release between two frames still counts */
    cui_input_mouse_move(ui, c.x, c.y);
    run(ui, build_basic, &b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_basic, &b, 1);
    CHECK(b.clicks == 2);
    cui_destroy(ui);
}

static void test_keyboard_nav(void) {
    printf("interaction: tab focus and keyboard activation\n");
    cui_ctx *ui = new_ctx();
    basic_ctx b = { 0 };
    run(ui, build_basic, &b, 2);
    cui_input_key(ui, CUI_KEY_TAB, true, 0); cui_input_key(ui, CUI_KEY_TAB, false, 0);
    run(ui, build_basic, &b, 1);
    cui_input_key(ui, CUI_KEY_ENTER, true, 0); cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    run(ui, build_basic, &b, 1);
    CHECK(b.clicks == 1);
    /* tab to the checkbox and toggle it with space */
    cui_input_key(ui, CUI_KEY_TAB, true, 0); cui_input_key(ui, CUI_KEY_TAB, false, 0);
    run(ui, build_basic, &b, 1);
    cui_input_key(ui, CUI_KEY_SPACE, true, 0); cui_input_key(ui, CUI_KEY_SPACE, false, 0);
    run(ui, build_basic, &b, 1);
    CHECK(b.check == true);
    /* shift+tab goes back */
    cui_input_key(ui, CUI_KEY_TAB, true, CUI_MOD_SHIFT); cui_input_key(ui, CUI_KEY_TAB, false, CUI_MOD_SHIFT);
    run(ui, build_basic, &b, 1);
    cui_input_key(ui, CUI_KEY_ENTER, true, 0); cui_input_key(ui, CUI_KEY_ENTER, false, 0);
    run(ui, build_basic, &b, 1);
    CHECK(b.clicks == 2);
    cui_destroy(ui);
}

static void test_toggles_and_slider(void) {
    printf("interaction: checkbox, switch, slider drag\n");
    cui_ctx *ui = new_ctx();
    basic_ctx b = { 0 };
    run(ui, build_basic, &b, 2);
    /* the checkbox row sits below the button: find it by y */
    click_at(ui, build_basic, &b, 30, 20 + 36 + 10 + 9);
    CHECK(b.check == true);
    click_at(ui, build_basic, &b, 30, 20 + 36 + 10 + 9);
    CHECK(b.check == false);
    click_at(ui, build_basic, &b, 40, 20 + 36 + 10 + 18 + 10 + 10);
    CHECK(b.sw == true);
    /* slider: track starts at x=20, width 300, thumb padding 9 */
    float track_y = 20 + 36 + 10 + 18 + 10 + 20 + 10 + 17 + 6 + 11;
    cui_input_mouse_move(ui, 20 + 9 + 141, track_y);
    run(ui, build_basic, &b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_basic, &b, 1);
    CHECK_NEAR(b.slider, 50, 1.5);
    cui_input_mouse_move(ui, 900, track_y);   /* drag past the end clamps */
    run(ui, build_basic, &b, 1);
    CHECK_NEAR(b.slider, 100, 0.01);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_basic, &b, 2);
    CHECK_NEAR(b.slider, 100, 0.01);
    cui_destroy(ui);
}

/* text editing ---------------------------------------------------------- */

typedef struct { char buf[128]; int flags; cui_id id; } edit_ctx;
static void build_edit(cui_ctx *ui, void *u) {
    edit_ctx *e = u;
    cui_column(ui, .pad = 20) {
        e->flags |= cui_input(ui, "field", e->buf, sizeof e->buf, .placeholder = "Type", .width = CUI_PX(300));
        e->id = cui_last_id(ui);
    }
}

static void key(cui_ctx *ui, build_fn fn, void *u, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    cui_input_key(ui, k, false, mods);
    run(ui, fn, u, 1);
}

static void type(cui_ctx *ui, build_fn fn, void *u, const char *s) {
    cui_input_key(ui, CUI_KEY_NONE, false, 0);   /* typing happens without modifiers */
    cui_input_text(ui, s);
    run(ui, fn, u, 1);
}

static void test_text_input(void) {
    printf("text input: typing, editing, selection, clipboard, undo\n");
    cui_ctx *ui = new_ctx();
    edit_ctx e;
    memset(&e, 0, sizeof e);
    run(ui, build_edit, &e, 2);
    cui_vec2 c = center(rect_of(ui, e.id));
    click_at(ui, build_edit, &e, c.x, c.y);
    CHECK(cui_wants_keyboard(ui));
    type(ui, build_edit, &e, "Hello world");
    CHECK(strcmp(e.buf, "Hello world") == 0);
    CHECK(e.flags & CUI_CHANGED);
    key(ui, build_edit, &e, CUI_KEY_BACKSPACE, 0);
    CHECK(strcmp(e.buf, "Hello worl") == 0);
    key(ui, build_edit, &e, CUI_KEY_BACKSPACE, CUI_MOD_CTRL | CUI_MOD_ALT);   /* word delete on any platform */
    CHECK(strcmp(e.buf, "Hello ") == 0);
    type(ui, build_edit, &e, "there");
    key(ui, build_edit, &e, CUI_KEY_HOME, 0);
    key(ui, build_edit, &e, CUI_KEY_RIGHT, CUI_MOD_SHIFT);
    key(ui, build_edit, &e, CUI_KEY_RIGHT, CUI_MOD_SHIFT);
    key(ui, build_edit, &e, CUI_KEY_C, CUI_MOD_PRIMARY);
    CHECK(strcmp(g_clip, "He") == 0);
    key(ui, build_edit, &e, CUI_KEY_END, 0);
    key(ui, build_edit, &e, CUI_KEY_V, CUI_MOD_PRIMARY);
    CHECK(strcmp(e.buf, "Hello thereHe") == 0);
    key(ui, build_edit, &e, CUI_KEY_A, CUI_MOD_PRIMARY);
    type(ui, build_edit, &e, "\xc3\xa9t\xc3\xa9");   /* UTF-8 replaces the selection */
    CHECK(strcmp(e.buf, "\xc3\xa9t\xc3\xa9") == 0);
    key(ui, build_edit, &e, CUI_KEY_BACKSPACE, 0);
    CHECK(strcmp(e.buf, "\xc3\xa9t") == 0);         /* removes a whole code point */
    key(ui, build_edit, &e, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(strcmp(e.buf, "\xc3\xa9t\xc3\xa9") == 0);
    key(ui, build_edit, &e, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(strcmp(e.buf, "Hello thereHe") == 0);
    key(ui, build_edit, &e, CUI_KEY_Z, CUI_MOD_PRIMARY | CUI_MOD_SHIFT);
    CHECK(strcmp(e.buf, "\xc3\xa9t\xc3\xa9") == 0);
    e.flags = 0;
    key(ui, build_edit, &e, CUI_KEY_ENTER, 0);
    CHECK(e.flags & CUI_SUBMITTED);
    key(ui, build_edit, &e, CUI_KEY_ESCAPE, 0);
    run(ui, build_edit, &e, 1);
    CHECK(!cui_wants_keyboard(ui));
    cui_destroy(ui);
}

static void test_text_capacity(void) {
    printf("text input: respects buffer capacity\n");
    cui_ctx *ui = new_ctx();
    struct { char buf[6]; int f; } small;
    memset(&small, 0, sizeof small);
    edit_ctx e;
    memset(&e, 0, sizeof e);
    run(ui, build_edit, &e, 2);
    cui_vec2 c = center(rect_of(ui, e.id));
    click_at(ui, build_edit, &e, c.x, c.y);
    for (int i = 0; i < 40; i++) type(ui, build_edit, &e, "abcd");
    CHECK(strlen(e.buf) == sizeof e.buf - 1);
    (void)small;
    cui_destroy(ui);
}

/* overlays -------------------------------------------------------------- */

typedef struct { bool open; int under_clicks; cui_id under; } dlg_ctx;
static void build_dialog(cui_ctx *ui, void *u) {
    dlg_ctx *d = u;
    cui_column(ui, .pad = 20) {
        if (cui_button(ui, "Underneath")) d->under_clicks++;
        d->under = cui_last_id(ui);
    }
    cui_dialog(ui, "Confirm", &d->open, .description = "Are you sure?") {
        cui_text(ui, "Body");
    }
}

static void test_dialog(void) {
    printf("overlays: modal blocks input, Esc and scrim close it\n");
    cui_ctx *ui = new_ctx();
    dlg_ctx d = { true, 0, 0 };
    run(ui, build_dialog, &d, 3);
    cui_vec2 c = center(rect_of(ui, d.under));
    click_at(ui, build_dialog, &d, c.x, c.y);   /* lands on the scrim, closes the dialog */
    CHECK(d.under_clicks == 0);
    CHECK(d.open == false);
    run(ui, build_dialog, &d, 3);
    click_at(ui, build_dialog, &d, c.x, c.y);
    CHECK(d.under_clicks == 1);
    d.open = true;
    run(ui, build_dialog, &d, 3);
    key(ui, build_dialog, &d, CUI_KEY_ESCAPE, 0);
    CHECK(d.open == false);
    /* clicking inside the panel keeps it open */
    d.open = true;
    run(ui, build_dialog, &d, 3);
    click_at(ui, build_dialog, &d, 400, 300);
    CHECK(d.open == true);
    cui_destroy(ui);
}

typedef struct { int chosen; cui_id trigger; } menu_ctx;
static void build_menu(cui_ctx *ui, void *u) {
    menu_ctx *m = u;
    cui_column(ui, .pad = 20) {
        cui_menu(ui, "Actions") {
            if (cui_menu_item(ui, "First")) m->chosen = 1;
            if (cui_menu_item(ui, "Second")) m->chosen = 2;
        }
        m->trigger = cui_last_id(ui);
    }
}

static void test_menu(void) {
    printf("overlays: dropdown menu with mouse and keyboard\n");
    cui_ctx *ui = new_ctx();
    menu_ctx m = { 0, 0 };
    run(ui, build_menu, &m, 2);
    cui_rect tr = rect_of(ui, m.trigger);
    CHECK(tr.w > 0);
    click_at(ui, build_menu, &m, tr.x + 10, tr.y + 10);
    run(ui, build_menu, &m, 3);
    /* second item: menu at trigger bottom + 6, panel padding 6, items 32 tall */
    click_at(ui, build_menu, &m, tr.x + 40, tr.y + tr.h + 6 + 6 + 32 + 16);
    CHECK(m.chosen == 2);
    run(ui, build_menu, &m, 3);
    /* open again, choose with arrows + enter */
    click_at(ui, build_menu, &m, tr.x + 10, tr.y + 10);
    run(ui, build_menu, &m, 2);
    cui_input_mouse_move(ui, 700, 550);
    run(ui, build_menu, &m, 1);
    key(ui, build_menu, &m, CUI_KEY_DOWN, 0);
    key(ui, build_menu, &m, CUI_KEY_ENTER, 0);
    CHECK(m.chosen == 1);
    /* click outside closes without choosing */
    m.chosen = 0;
    click_at(ui, build_menu, &m, tr.x + 10, tr.y + 10);
    run(ui, build_menu, &m, 2);
    click_at(ui, build_menu, &m, 700, 550);
    run(ui, build_menu, &m, 2);
    CHECK(m.chosen == 0);
    cui_destroy(ui);
}

typedef struct { bool open; int inside, under; } trap_ctx;
static void build_trap(cui_ctx *ui, void *u) {
    trap_ctx *t = u;
    cui_column(ui, .pad = 20) { if (cui_button(ui, "Under")) t->under++; }
    cui_dialog(ui, "Trap", &t->open) { if (cui_button(ui, "Inside")) t->inside++; }
}

static void test_focus_trap(void) {
    printf("overlays: tab focus stays inside a modal\n");
    cui_ctx *ui = new_ctx();
    trap_ctx t = { true, 0, 0 };
    run(ui, build_trap, &t, 3);
    for (int i = 0; i < 6; i++) {
        key(ui, build_trap, &t, CUI_KEY_TAB, 0);
        if (i % 2 == 1) key(ui, build_trap, &t, CUI_KEY_ENTER, 0);   /* lands on Inside every second tab */
        if (!t.open) break;
    }
    CHECK(t.under == 0);
    CHECK(t.inside == 3);
    CHECK(t.open);
    cui_destroy(ui);
}

typedef struct { char tags[128]; char otp[8]; int otp_done; int held; cui_id tag_id, otp_id, hold_id; } extras_ctx;
static void build_extras(cui_ctx *ui, void *u) {
    extras_ctx *x = u;
    cui_column(ui, .pad = 20, .gap = 16, .width = CUI_PX(500)) {
        cui_tag_input(ui, "tags", x->tags, sizeof x->tags);
        x->tag_id = cui_last_id(ui);
        if (cui_otp_input(ui, "otp", x->otp, 4)) x->otp_done++;
        x->otp_id = cui_last_id(ui);
        if (cui_hold_button(ui, "Hold", .duration = 0.5f)) x->held++;
        x->hold_id = cui_make_id(ui, "Hold");
    }
}

static void test_extras(void) {
    printf("extras: tag input, one time code, hold to confirm\n");
    cui_ctx *ui = new_ctx();
    extras_ctx x;
    memset(&x, 0, sizeof x);
    snprintf(x.tags, sizeof x.tags, "alpha, beta");
    run(ui, build_extras, &x, 3);
    cui_rect tr = rect_of(ui, x.tag_id);
    click_at(ui, build_extras, &x, tr.x + tr.w - 40, tr.y + tr.h / 2);
    type(ui, build_extras, &x, "gamma");
    key(ui, build_extras, &x, CUI_KEY_ENTER, 0);
    CHECK(strcmp(x.tags, "alpha, beta, gamma") == 0);
    type(ui, build_extras, &x, "delta,eps");
    CHECK(strcmp(x.tags, "alpha, beta, gamma, delta") == 0);
    key(ui, build_extras, &x, CUI_KEY_BACKSPACE, 0);   /* edits "eps" */
    key(ui, build_extras, &x, CUI_KEY_BACKSPACE, 0);
    key(ui, build_extras, &x, CUI_KEY_BACKSPACE, 0);
    key(ui, build_extras, &x, CUI_KEY_BACKSPACE, 0);   /* empty field: removes "delta" */
    CHECK(strcmp(x.tags, "alpha, beta, gamma") == 0);
    /* one time code */
    cui_rect orr = rect_of(ui, x.otp_id);
    click_at(ui, build_extras, &x, orr.x + 10, orr.y + 10);
    type(ui, build_extras, &x, "12a");
    CHECK(strcmp(x.otp, "12a") == 0 && x.otp_done == 0);
    type(ui, build_extras, &x, "9x");
    CHECK(strcmp(x.otp, "12a9") == 0 && x.otp_done == 1);
    key(ui, build_extras, &x, CUI_KEY_BACKSPACE, 0);
    CHECK(strcmp(x.otp, "12a") == 0);
    snprintf(g_clip, sizeof g_clip, " 7-7-7-7 ");
    key(ui, build_extras, &x, CUI_KEY_V, CUI_MOD_PRIMARY);
    CHECK(strcmp(x.otp, "7777") == 0 && x.otp_done == 2);
    /* hold: a short press does nothing, a long one fires once */
    cui_vec2 hc = center(rect_of(ui, x.hold_id));
    click_at(ui, build_extras, &x, hc.x, hc.y);
    CHECK(x.held == 0);
    cui_set_reduced_motion(ui, false);
    cui_input_mouse_move(ui, hc.x, hc.y);
    run(ui, build_extras, &x, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_extras, &x, 60);    /* one second at 60 fps */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_extras, &x, 2);
    CHECK(x.held == 1);
    cui_destroy(ui);
}

typedef struct { int order[4]; int changed; cui_id list; } reorder_ctx;
static void build_reorder(cui_ctx *ui, void *u) {
    reorder_ctx *r = u;
    cui_column(ui, .pad = 20, .width = CUI_PX(300)) {
        cui_reorder_begin(ui, "list", r->order, 4);
        for (int i = 0; i < 4; i++) cui_reorder_item(ui, i) { cui_textf(ui, "Item %d", r->order[i]); }
        if (cui_reorder_end(ui)) r->changed++;
    }
}

static void test_reorder(void) {
    printf("data: drag to reorder\n");
    cui_ctx *ui = new_ctx();
    reorder_ctx r = { { 0, 1, 2, 3 }, 0, 0 };
    run(ui, build_reorder, &r, 3);
    /* rows are about 39 points tall with a 6 point gap, starting at y = 20 */
    cui_input_mouse_move(ui, 150, 40);
    run(ui, build_reorder, &r, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_reorder, &r, 1);
    for (int y = 40; y <= 140; y += 10) { cui_input_mouse_move(ui, 150, (float)y); run(ui, build_reorder, &r, 1); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_reorder, &r, 3);
    CHECK(r.changed == 1);
    CHECK(r.order[0] == 1 && r.order[1] == 2 && r.order[2] == 0 && r.order[3] == 3);
    /* a click without movement does not reorder */
    click_at(ui, build_reorder, &r, 150, 40);
    CHECK(r.changed == 1);
    cui_destroy(ui);
}

/* virtual list ---------------------------------------------------------- */

typedef struct { int first, last, built; } vl_ctx;
static void build_vlist(cui_ctx *ui, void *u) {
    vl_ctx *v = u;
    v->built = 0;
    cui_virtual_list(ui, "vl", 100000, 20, CUI_PX(200), &v->first, &v->last) {
        for (int i = v->first; i < v->last; i++) { cui_row(ui, .height = CUI_PX(20)) { cui_textf(ui, "Row %d", i); } v->built++; }
    }
}

static void test_virtual_list(void) {
    printf("data: virtual list builds only visible rows\n");
    cui_ctx *ui = new_ctx();
    vl_ctx v = { 0, 0, 0 };
    run(ui, build_vlist, &v, 3);
    CHECK(v.first == 0);
    CHECK(v.built > 5 && v.built < 30);
    cui_input_mouse_move(ui, 100, 100);
    run(ui, build_vlist, &v, 1);
    for (int i = 0; i < 20; i++) { cui_input_scroll(ui, 0, -10); run(ui, build_vlist, &v, 1); }
    run(ui, build_vlist, &v, 5);
    CHECK(v.first > 300);
    CHECK(v.built < 30);
    cui_destroy(ui);
}

/* theme ----------------------------------------------------------------- */

static void build_empty(cui_ctx *ui, void *u) { (void)u; cui_text(ui, "x"); }

static void test_theme(void) {
    printf("theme: switching modes and accents\n");
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 200, 100, 1);
    run(ui, build_empty, NULL, 2);
    cui_color light_bg = cui_token_color(ui, CUI_TOKEN_BACKGROUND);
    CHECK(light_bg.r > 240);
    cui_set_theme(ui, CUI_THEME_DARK);
    run(ui, build_empty, NULL, 1);
    cui_color mid = cui_token_color(ui, CUI_TOKEN_BACKGROUND);
    CHECK(mid.r < light_bg.r);
    CHECK(cui_needs_redraw(ui));
    run(ui, build_empty, NULL, 60);
    cui_color dark_bg = cui_token_color(ui, CUI_TOKEN_BACKGROUND);
    CHECK(dark_bg.r < 50);
    cui_set_accent(ui, CUI_ACCENT_BLUE);
    run(ui, build_empty, NULL, 60);
    cui_color acc = cui_token_color(ui, CUI_TOKEN_ACCENT);
    CHECK(acc.b > acc.r);
    CHECK(strcmp(cui_accent_name(CUI_ACCENT_ROSE), "rose") == 0);
    /* idle when nothing moves */
    run(ui, build_empty, NULL, 5);
    CHECK(!cui_needs_redraw(ui));
    cui_destroy(ui);
}

/* draw data and renderer ------------------------------------------------ */

static void build_rect(cui_ctx *ui, void *u) {
    (void)u;
    cui_box(ui, .width = CUI_PX(100), .height = CUI_PX(60), .bg = CUI_RGB(0xff0000), .radius = 20) {}
}

static void test_renderer(void) {
    printf("renderer: draw data is valid and rasterizes correctly\n");
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 200, 100, 1);
    run(ui, build_rect, NULL, 2);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    CHECK(dd->index_count % 3 == 0);
    bool idx_ok = true;
    for (int i = 0; i < dd->index_count; i++) if ((int)dd->indices[i] >= dd->vertex_count) idx_ok = false;
    CHECK(idx_ok);
    unsigned char *px = calloc(200 * 100, 4);
    cui_sw_render(g_sw, dd, px, 200, 100, 800, CUI_SW_RGBA);
    unsigned char *center_px = px + (30 * 200 + 50) * 4;
    CHECK(center_px[0] == 255 && center_px[1] == 0 && center_px[2] == 0);
    unsigned char *outside = px + (80 * 200 + 150) * 4;
    CHECK(outside[0] == 255 && outside[1] == 255 && outside[2] == 255);   /* light background */
    unsigned char *corner = px + (1 * 200 + 1) * 4;                       /* rounded away */
    CHECK(corner[1] > 200);
    unsigned char *edge = px + (6 * 200 + 5) * 4;                          /* anti-aliased corner pixel */
    CHECK(edge[1] > 0 && edge[1] < 255);
    /* threaded and single-threaded output are identical */
    unsigned char *px2 = calloc(200 * 100, 4);
    cui_sw_set_threads(g_sw, 1);
    cui_sw_render(g_sw, dd, px2, 200, 100, 800, CUI_SW_RGBA);
    CHECK(memcmp(px, px2, 200 * 100 * 4) == 0);
    free(px); free(px2);
    cui_destroy(ui);
}

typedef struct { int size; } atlas_ctx;
static void build_atlas(cui_ctx *ui, void *u) {
    atlas_ctx *a = u;
    for (int k = 0; k < 6; k++)
        cui_text(ui, "The quick brown fox jumps over the lazy dog 0123456789 !?@#$%&*()[]{}", .size = (float)(a->size + k * 7), .nowrap = true);
}

static void test_atlas_pressure(void) {
    printf("text: glyph atlas grows and recovers under pressure\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 800, 600, 2);
    atlas_ctx a = { 8 };
    for (; a.size < 400; a.size += 9) run(ui, build_atlas, &a, 2);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    CHECK(dd->vertex_count > 0);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    free(px);
    cui_destroy(ui);
}

/* stress ---------------------------------------------------------------- */

typedef struct { int n; } stress_ctx;
static void build_stress(cui_ctx *ui, void *u) {
    stress_ctx *s = u;
    cui_scroll(ui, .id = "s") {
        for (int i = 0; i < s->n; i++) {
            cui_push_id_int(ui, i);
            cui_row(ui, .gap = 8) {
                cui_badge(ui, "Tag");
                cui_button(ui, "Edit", .size = CUI_SIZE_SM);
                cui_textf(ui, "Item %d with some text", i);
            }
            cui_pop_id(ui);
        }
    }
}

static void test_stress(void) {
    printf("stress: many widgets and many frames stay stable\n");
    cui_ctx *ui = new_ctx();
    stress_ctx s = { 2000 };
    run(ui, build_stress, &s, 3);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    CHECK(dd->vertex_count > 0);
    s.n = 50;
    run(ui, build_stress, &s, 400);   /* state for the removed rows gets collected */
    CHECK(cui_frame_count(ui) == 403);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_color();
    test_layout_row();
    test_layout_wrap();
    test_layout_center();
    test_layout_grid();
    test_button_click();
    test_keyboard_nav();
    test_toggles_and_slider();
    test_text_input();
    test_text_capacity();
    test_dialog();
    test_menu();
    test_focus_trap();
    test_extras();
    test_reorder();
    test_virtual_list();
    test_theme();
    test_renderer();
    test_stress();
    test_atlas_pressure();
    cui_sw_destroy(g_sw);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
