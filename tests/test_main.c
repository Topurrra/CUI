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
    /* cut and paste in one frame: the paste lands where the cut text was */
    key(ui, build_edit, &e, CUI_KEY_A, CUI_MOD_PRIMARY);
    cui_input_key(ui, CUI_KEY_X, true, CUI_MOD_PRIMARY);
    cui_input_key(ui, CUI_KEY_X, false, CUI_MOD_PRIMARY);
    cui_input_key(ui, CUI_KEY_V, true, CUI_MOD_PRIMARY);
    cui_input_key(ui, CUI_KEY_V, false, CUI_MOD_PRIMARY);
    run(ui, build_edit, &e, 1);
    CHECK(strcmp(e.buf, "\xc3\xa9t\xc3\xa9") == 0);
    e.flags = 0;
    key(ui, build_edit, &e, CUI_KEY_ENTER, 0);
    CHECK(e.flags & CUI_SUBMITTED);
    key(ui, build_edit, &e, CUI_KEY_ESCAPE, 0);
    run(ui, build_edit, &e, 1);
    CHECK(!cui_wants_keyboard(ui));
    cui_destroy(ui);
}

typedef struct { char buf[512]; char other[32]; int flags; cui_id id, other_id; } area_ctx;
static void build_area(cui_ctx *ui, void *u) {
    area_ctx *a = u;
    cui_column(ui, .pad = 20, .gap = 12) {
        a->flags |= cui_textarea(ui, "notes", a->buf, sizeof a->buf, .height = 80, .width = CUI_PX(300));
        a->id = cui_last_id(ui);
        cui_input(ui, "other", a->other, sizeof a->other, .clearable = true, .width = CUI_PX(300));
        a->other_id = cui_last_id(ui);
    }
}

static uint32_t frame_hash(cui_ctx *ui) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    size_t n = (size_t)dd->width_px * dd->height_px * 4;
    unsigned char *px = malloc(n);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ px[i]) * 16777619u;
    free(px);
    return h;
}

static void test_textarea_and_clear(void) {
    printf("text input: textarea wheel, focus flags, clear button undo\n");
    cui_ctx *ui = new_ctx();
    area_ctx a;
    memset(&a, 0, sizeof a);
    for (int i = 0; i < 20; i++) { char l[16]; snprintf(l, sizeof l, "line %d\n", i); strcat(a.buf, l); }
    run(ui, build_area, &a, 2);
    cui_vec2 c = center(rect_of(ui, a.id));
    cui_input_mouse_move(ui, c.x, c.y);
    run(ui, build_area, &a, 1);
    uint32_t before = frame_hash(ui);
    cui_input_scroll(ui, 0, -3);                 /* unfocused: the wheel still scrolls */
    run(ui, build_area, &a, 2);
    uint32_t after = frame_hash(ui);
    CHECK(before != after);
    CHECK(!cui_wants_keyboard(ui));

    a.flags = 0;
    click_at(ui, build_area, &a, c.x, c.y);
    CHECK(a.flags & CUI_FOCUSED);
    a.flags = 0;
    key(ui, build_area, &a, CUI_KEY_ESCAPE, 0);
    run(ui, build_area, &a, 1);
    CHECK(a.flags & CUI_BLURRED);
    CHECK(!(a.flags & CUI_FOCUSED));

    /* clear button: empties the field and Ctrl+Z restores it */
    strcpy(a.other, "keep me");
    run(ui, build_area, &a, 2);
    cui_rect fr = rect_of(ui, a.other_id);
    click_at(ui, build_area, &a, fr.x + fr.w / 3, fr.y + fr.h / 2);   /* focus the field */
    click_at(ui, build_area, &a, fr.x + fr.w - 16, fr.y + fr.h / 2);  /* the x button */
    CHECK(a.other[0] == 0);
    click_at(ui, build_area, &a, fr.x + fr.w / 3, fr.y + fr.h / 2);
    key(ui, build_area, &a, CUI_KEY_Z, CUI_MOD_PRIMARY);
    CHECK(strcmp(a.other, "keep me") == 0);
    cui_destroy(ui);
}

/* more actions ------------------------------------------------------------ */

typedef struct {
    int split; bool fmt[3], al[3]; int confirm_fires; bool status_click; cui_status status;
    cui_reaction re[2]; int dial; cui_id dial_id, split_id, tg_id, tg2_id, confirm_id, status_id, re_id;
} act_ctx;
static void build_actions(cui_ctx *ui, void *u) {
    act_ctx *a = u;
    cui_column(ui, .pad = 20, .gap = 16) {
        static const char *items[] = { "Draft", "Publish" };
        int r = cui_split_button(ui, "Save", items, 2);
        if (r >= 0) a->split = r;
        a->split_id = cui_last_id(ui);
        static const char *labels[] = { "B", "I", "U" };
        cui_toggle_group(ui, "fmt", labels, 3, a->fmt);
        a->tg_id = cui_last_id(ui);
        cui_toggle_group(ui, "al", labels, 3, a->al, .exclusive = true);
        a->tg2_id = cui_last_id(ui);
        if (cui_confirm_button(ui, "Delete")) a->confirm_fires++;
        a->confirm_id = cui_last_id(ui);
        if (cui_status_button(ui, "Save now", a->status)) a->status_click = true;
        a->status_id = cui_last_id(ui);
        cui_reactions(ui, "re", a->re, 2);
        a->re_id = cui_last_id(ui);
        cui_row(ui, .height = CUI_PX(200), .align = CUI_ALIGN_END) {
            static const cui_speed_action acts[] = { { CUI_ICON_FILE, "Note" }, { CUI_ICON_IMAGE, "Image" } };
            int d = cui_speed_dial(ui, "dial", acts, 2);
            if (d >= 0) a->dial = d;
            a->dial_id = cui_last_id(ui);
        }
    }
}

static void test_more_actions(void) {
    printf("actions: split button, toggle group, confirm, status, reactions, speed dial\n");
    cui_ctx *ui = new_ctx();
    act_ctx a;
    memset(&a, 0, sizeof a);
    a.split = -1; a.dial = -1;
    a.re[0] = (cui_reaction){ CUI_ICON_HEART, 3, false };
    a.re[1] = (cui_reaction){ CUI_ICON_STAR, 1, true };
    a.al[0] = true;
    run(ui, build_actions, &a, 3);
    /* split: main part, then the menu by keyboard */
    cui_rect sr = rect_of(ui, a.split_id);
    click_at(ui, build_actions, &a, sr.x + 20, sr.y + sr.h / 2);
    CHECK(a.split == 0);
    click_at(ui, build_actions, &a, sr.x + sr.w - 10, sr.y + sr.h / 2);
    run(ui, build_actions, &a, 3);
    key(ui, build_actions, &a, CUI_KEY_DOWN, 0);
    key(ui, build_actions, &a, CUI_KEY_DOWN, 0);
    key(ui, build_actions, &a, CUI_KEY_ENTER, 0);
    CHECK(a.split == 2);
    run(ui, build_actions, &a, 20);
    /* toggles */
    cui_rect tr = rect_of(ui, a.tg_id);
    click_at(ui, build_actions, &a, tr.x + 12, tr.y + tr.h / 2);
    CHECK(a.fmt[0] && !a.fmt[1]);
    click_at(ui, build_actions, &a, tr.x + tr.w - 12, tr.y + tr.h / 2);
    CHECK(a.fmt[0] && a.fmt[2]);
    cui_rect t2 = rect_of(ui, a.tg2_id);
    click_at(ui, build_actions, &a, t2.x + t2.w - 12, t2.y + t2.h / 2);
    CHECK(!a.al[0] && !a.al[1] && a.al[2]);
    /* confirm: arm, then confirm; an armed button disarms after its timeout */
    cui_vec2 cc = center(rect_of(ui, a.confirm_id));
    click_at(ui, build_actions, &a, cc.x, cc.y);
    CHECK(a.confirm_fires == 0);
    run(ui, build_actions, &a, 20);
    cc = center(rect_of(ui, a.confirm_id));
    click_at(ui, build_actions, &a, cc.x, cc.y);
    CHECK(a.confirm_fires == 1);
    run(ui, build_actions, &a, 20);
    cc = center(rect_of(ui, a.confirm_id));
    click_at(ui, build_actions, &a, cc.x, cc.y);
    run(ui, build_actions, &a, 240);   /* 4 seconds */
    cc = center(rect_of(ui, a.confirm_id));
    click_at(ui, build_actions, &a, cc.x, cc.y);
    CHECK(a.confirm_fires == 1);
    /* status: clicks only count while idle */
    cui_vec2 sc = center(rect_of(ui, a.status_id));
    click_at(ui, build_actions, &a, sc.x, sc.y);
    CHECK(a.status_click);
    a.status_click = false; a.status = CUI_STATUS_BUSY;
    run(ui, build_actions, &a, 30);
    sc = center(rect_of(ui, a.status_id));
    click_at(ui, build_actions, &a, sc.x, sc.y);
    CHECK(!a.status_click);
    /* reactions */
    cui_rect rr = rect_of(ui, a.re_id);
    click_at(ui, build_actions, &a, rr.x + 14, rr.y + rr.h / 2);
    CHECK(a.re[0].active && a.re[0].count == 4);
    click_at(ui, build_actions, &a, rr.x + 14, rr.y + rr.h / 2);
    CHECK(!a.re[0].active && a.re[0].count == 3);
    /* speed dial: open, pick the nearest action */
    cui_rect dr = rect_of(ui, a.dial_id);
    click_at(ui, build_actions, &a, dr.x + dr.w / 2, dr.y + dr.h / 2);
    run(ui, build_actions, &a, 40);
    click_at(ui, build_actions, &a, dr.x + dr.w / 2, dr.y - 12 - 21);
    CHECK(a.dial == 0);
    cui_destroy(ui);
}

typedef struct { char title[64]; float v; double money; cui_key_combo combo; int edit_flags; cui_id title_id, scrub_id, money_id, rec_id, other_id; char other[16]; } inp_ctx;
static void build_more_inputs(cui_ctx *ui, void *u) {
    inp_ctx *a = u;
    cui_column(ui, .pad = 20, .gap = 16) {
        a->edit_flags |= cui_inline_edit(ui, "title", a->title, sizeof a->title);
        a->title_id = cui_last_id(ui);
        cui_scrub(ui, "Size", &a->v, 0, 100);
        a->scrub_id = cui_last_id(ui);
        cui_money_input(ui, "money", &a->money, .width = CUI_PX(240));
        a->money_id = cui_last_id(ui);
        cui_shortcut_recorder(ui, "rec", &a->combo);
        a->rec_id = cui_last_id(ui);
        cui_input(ui, "other", a->other, sizeof a->other, .width = CUI_PX(200));
        a->other_id = cui_last_id(ui);
    }
}

static void test_more_inputs(void) {
    printf("inputs: inline edit, scrub, money, shortcut recorder\n");
    cui_ctx *ui = new_ctx();
    inp_ctx a;
    memset(&a, 0, sizeof a);
    strcpy(a.title, "Roadmap");
    a.v = 20;
    a.money = 1234.5;
    run(ui, build_more_inputs, &a, 3);
    /* inline edit: Enter saves, Esc restores */
    cui_vec2 t = center(rect_of(ui, a.title_id));
    click_at(ui, build_more_inputs, &a, t.x, t.y);
    run(ui, build_more_inputs, &a, 1);
    CHECK(cui_wants_keyboard(ui));
    type(ui, build_more_inputs, &a, "Plan");        /* text starts selected, typing replaces it */
    key(ui, build_more_inputs, &a, CUI_KEY_ENTER, 0);
    CHECK(strcmp(a.title, "Plan") == 0);
    CHECK(a.edit_flags & CUI_CHANGED);
    run(ui, build_more_inputs, &a, 2);
    t = center(rect_of(ui, a.title_id));
    click_at(ui, build_more_inputs, &a, t.x, t.y);
    run(ui, build_more_inputs, &a, 1);
    type(ui, build_more_inputs, &a, "Oops");
    key(ui, build_more_inputs, &a, CUI_KEY_ESCAPE, 0);
    CHECK(strcmp(a.title, "Plan") == 0);
    run(ui, build_more_inputs, &a, 2);
    /* scrub: drag 50 points to the right adds 50 steps of 1 */
    cui_vec2 s = center(rect_of(ui, a.scrub_id));
    cui_input_mouse_move(ui, s.x, s.y);
    run(ui, build_more_inputs, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_more_inputs, &a, 1);
    cui_input_mouse_move(ui, s.x + 25, s.y);
    run(ui, build_more_inputs, &a, 1);
    cui_input_mouse_move(ui, s.x + 50, s.y);
    run(ui, build_more_inputs, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_more_inputs, &a, 1);
    CHECK_NEAR(a.v, 70, 0.01);
    /* a click without a drag types a value */
    s = center(rect_of(ui, a.scrub_id));
    click_at(ui, build_more_inputs, &a, s.x, s.y);
    run(ui, build_more_inputs, &a, 1);
    type(ui, build_more_inputs, &a, "33");
    key(ui, build_more_inputs, &a, CUI_KEY_ENTER, 0);
    CHECK_NEAR(a.v, 33, 0.01);
    run(ui, build_more_inputs, &a, 2);
    /* money: plain digits while typing, filtered to two decimals */
    cui_vec2 m = center(rect_of(ui, a.money_id));
    click_at(ui, build_more_inputs, &a, m.x, m.y);
    key(ui, build_more_inputs, &a, CUI_KEY_A, CUI_MOD_PRIMARY);
    type(ui, build_more_inputs, &a, "99.999x");
    CHECK_NEAR(a.money, 99.99, 0.0001);
    cui_vec2 o = center(rect_of(ui, a.other_id));
    click_at(ui, build_more_inputs, &a, o.x, o.y);   /* blur */
    CHECK_NEAR(a.money, 99.99, 0.0001);
    /* shortcut recorder */
    cui_vec2 r = center(rect_of(ui, a.rec_id));
    click_at(ui, build_more_inputs, &a, r.x, r.y);
    key(ui, build_more_inputs, &a, CUI_KEY_K, CUI_MOD_CTRL | CUI_MOD_SHIFT);
    CHECK(a.combo.key == CUI_KEY_K && a.combo.mods == (CUI_MOD_CTRL | CUI_MOD_SHIFT));
    CHECK(strcmp(cui_shortcut_label(ui, a.combo), "Ctrl Shift K") == 0);
    click_at(ui, build_more_inputs, &a, r.x, r.y);
    key(ui, build_more_inputs, &a, CUI_KEY_BACKSPACE, 0);
    CHECK(a.combo.key == CUI_KEY_NONE);
    cui_destroy(ui);
}

typedef struct { int result; cui_id row; } swipe_ctx;
static void build_swipe(cui_ctx *ui, void *u) {
    swipe_ctx *a = u;
    cui_column(ui, .pad = 20, .width = CUI_PX(600)) {
        static const cui_swipe_action left[] = { { CUI_ICON_CHECK, "Done", CUI_TONE_SUCCESS } };
        static const cui_swipe_action right[] = { { CUI_ICON_ARCHIVE, "Archive", CUI_TONE_INFO }, { CUI_ICON_TRASH_2, "Delete", CUI_TONE_DANGER } };
        cui_swipe_item_begin(ui, "row", left, 1, right, 2);
        cui_text(ui, "Swipe me", .width = CUI_FILL);
        int r = cui_swipe_item_end(ui);
        if (r >= 0) a->result = r;
        a->row = cui_make_id(ui, "row");
    }
}

static void drag(cui_ctx *ui, build_fn fn, void *u, float x0, float y, float x1, int steps) {
    cui_input_mouse_move(ui, x0, y);
    run(ui, fn, u, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, fn, u, 1);
    for (int i = 1; i <= steps; i++) { cui_input_mouse_move(ui, x0 + (x1 - x0) * i / steps, y); run(ui, fn, u, 1); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, fn, u, 1);
}

static void test_swipe(void) {
    printf("swipe actions: long swipe triggers, short swipe reveals\n");
    cui_ctx *ui = new_ctx();
    swipe_ctx a = { -1, 0 };
    run(ui, build_swipe, &a, 3);
    cui_rect r = rect_of(ui, a.row);
    CHECK(r.w > 500);
    drag(ui, build_swipe, &a, r.x + r.w - 40, r.y + r.h / 2, r.x + r.w - 40 - 420, 8);
    CHECK(a.result == 100);
    run(ui, build_swipe, &a, 60);
    a.result = -1;
    drag(ui, build_swipe, &a, r.x + 300, r.y + r.h / 2, r.x + 300 - 120, 6);
    CHECK(a.result == -1);
    run(ui, build_swipe, &a, 60);
    /* now open on the right: the delete button sits at the far right */
    click_at(ui, build_swipe, &a, r.x + r.w - 30, r.y + r.h / 2);
    CHECK(a.result == 101);
    cui_destroy(ui);
}

/* pickers ------------------------------------------------------------------ */

static void test_date_math(void) {
    printf("dates: day numbers round trip\n");
    CHECK(cui_date_to_days((cui_date){ 1970, 1, 1 }) == 0);
    CHECK(cui_date_to_days((cui_date){ 2000, 3, 1 }) == 11017);
    cui_date d = cui_date_from_days(cui_date_to_days((cui_date){ 2024, 2, 29 }) + 1);
    CHECK(d.year == 2024 && d.month == 3 && d.day == 1);
    int bad = 0;
    for (int z = -800000; z < 800000; z += 37) {
        cui_date x = cui_date_from_days(z);
        if (cui_date_to_days(x) != z) bad++;
    }
    CHECK(bad == 0);
}

typedef struct {
    bool sel[4]; cui_date_range range; int wheel; float ruler; int dh, dm; int dropped; int drop_click;
    cui_id ms_id, cal_id, wheel_id, ruler_id, dial_id, drop_id, sig_id;
} pick_ctx;
static void build_pickers(cui_ctx *ui, void *u) {
    pick_ctx *a = u;
    cui_column(ui, .pad = 20, .gap = 16) {
        static const char *items[] = { "Alpha", "Beta", "Gamma", "Delta" };
        cui_multi_select(ui, "ms", items, 4, a->sel, .width = CUI_PX(300));
        a->ms_id = cui_last_id(ui);
        cui_range_calendar(ui, "cal", &a->range, 1);
        a->cal_id = cui_last_id(ui);
        static const char *w[] = { "One", "Two", "Three", "Four", "Five" };
        cui_wheel_picker(ui, "wheel", w, 5, &a->wheel);
        a->wheel_id = cui_last_id(ui);
        cui_ruler_slider(ui, "Ruler", &a->ruler, 0, 100, .width = CUI_PX(300));
        a->ruler_id = cui_last_id(ui);
    }
    cui_column(ui, .pad = 20, .gap = 16) {
        cui_time_dial(ui, "dial", &a->dh, &a->dm, 200);
        a->dial_id = cui_make_id(ui, "dial") ^ 0xFACEu;
        int n = cui_dropzone(ui, "drop", .accept = "png;txt", .width = CUI_PX(300));
        if (n > 0) a->dropped = n;
        if (n < 0) a->drop_click++;
        a->drop_id = cui_last_id(ui);
        cui_signature_pad(ui, "sig", .width = CUI_PX(300), .height = 120);
        a->sig_id = cui_make_id(ui, "sig") ^ 0x5161u;
    }
}
static void build_pickers_root(cui_ctx *ui, void *u) {
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START) { build_pickers(ui, u); }
}

static void test_pickers(void) {
    printf("pickers: multi-select, range calendar, wheel, ruler, dial, dropzone, signature\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 1000, 900, 1);
    pick_ctx a;
    memset(&a, 0, sizeof a);
    a.range.start = (cui_date){ 2026, 11, 20 };   /* shows November 2026 */
    a.ruler = 50;
    a.dh = 9; a.dm = 0;
    run(ui, build_pickers_root, &a, 3);
    /* multi-select: open, then toggle items with the keyboard; the list stays open */
    cui_vec2 m = center(rect_of(ui, a.ms_id));
    click_at(ui, build_pickers_root, &a, m.x, m.y);
    run(ui, build_pickers_root, &a, 3);
    key(ui, build_pickers_root, &a, CUI_KEY_DOWN, 0);
    key(ui, build_pickers_root, &a, CUI_KEY_ENTER, 0);
    key(ui, build_pickers_root, &a, CUI_KEY_DOWN, 0);
    key(ui, build_pickers_root, &a, CUI_KEY_DOWN, 0);
    key(ui, build_pickers_root, &a, CUI_KEY_SPACE, 0);
    CHECK(a.sel[0] && !a.sel[1] && a.sel[2] && !a.sel[3]);
    key(ui, build_pickers_root, &a, CUI_KEY_ESCAPE, 0);
    run(ui, build_pickers_root, &a, 20);
    /* range calendar: November 2026 starts on a Sunday; click the 3rd, then the 14th */
    cui_rect cr = rect_of(ui, a.cal_id);
    float top = cr.y + 30 + 8 + 12 * 2.2f;
    a.range.start = (cui_date){ 0, 0, 0 };
    int days[2] = { 14, 3 };   /* end first: the calendar orders them */
    for (int k = 0; k < 2; k++) {
        int cell = days[k] - 1 + 6;
        click_at(ui, build_pickers_root, &a, cr.x + (cell % 7) * 36 + 18, top + (cell / 7) * 36 + 18);
    }
    CHECK(a.range.start.day == 3 && a.range.end.day == 14 && a.range.end.month == 11);
    /* wheel: arrow keys and a drag */
    cui_vec2 w = center(rect_of(ui, a.wheel_id));
    click_at(ui, build_pickers_root, &a, w.x, w.y);
    key(ui, build_pickers_root, &a, CUI_KEY_DOWN, 0);
    key(ui, build_pickers_root, &a, CUI_KEY_DOWN, 0);
    CHECK(a.wheel == 2);
    drag(ui, build_pickers_root, &a, w.x, w.y, w.x, 1);   /* no move: stays */
    CHECK(a.wheel == 2);
    cui_input_mouse_move(ui, w.x, w.y);
    run(ui, build_pickers_root, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_pickers_root, &a, 1);
    for (int i = 1; i <= 6; i++) { cui_input_mouse_move(ui, w.x, w.y + 34.0f * i / 6); run(ui, build_pickers_root, &a, 1); }
    run(ui, build_pickers_root, &a, 10);   /* hold still so the throw has no speed */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_pickers_root, &a, 30);
    CHECK(a.wheel == 1);
    /* ruler: dragging left by 9 points per step moves the value up */
    cui_vec2 rl = center(rect_of(ui, a.ruler_id));
    cui_input_mouse_move(ui, rl.x, rl.y);
    run(ui, build_pickers_root, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_pickers_root, &a, 1);
    for (int i = 1; i <= 6; i++) { cui_input_mouse_move(ui, rl.x - 90.0f * i / 6, rl.y); run(ui, build_pickers_root, &a, 1); }
    run(ui, build_pickers_root, &a, 10);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_pickers_root, &a, 30);
    CHECK_NEAR(a.ruler, 60, 0.01);
    /* dial: drag to 3 o'clock on the outer ring sets 3, then minutes */
    cui_rect dr = rect_of(ui, a.dial_id);
    cui_vec2 dc = center(dr);
    drag(ui, build_pickers_root, &a, dc.x + dr.w * 0.42f, dc.y, dc.x + dr.w * 0.42f, 1);
    CHECK(a.dh == 3);
    drag(ui, build_pickers_root, &a, dc.x, dc.y + dr.h * 0.42f, dc.x, 1);   /* six o'clock = 30 minutes */
    CHECK(a.dm == 30);
    drag(ui, build_pickers_root, &a, dc.x - dr.w * 0.42f, dc.y, dc.x - dr.w * 0.42f, 1);
    CHECK(a.dm == 45);
    /* dropzone: files dropped on it are filtered; elsewhere they are left alone */
    cui_vec2 z = center(rect_of(ui, a.drop_id));
    const char *files[] = { "/tmp/a.png", "/tmp/b.exe", "/tmp/c.TXT" };
    cui_input_file_drop(ui, files, 3, z.x, z.y);
    run(ui, build_pickers_root, &a, 1);
    CHECK(a.dropped == 2);
    cui_input_file_drop(ui, files, 1, 5, 5);
    cui_begin_frame(ui, 1.0f / 60.0f);
    build_pickers_root(ui, &a);
    CHECK(cui_dropped_file_count(ui) == 1 && strcmp(cui_dropped_file(ui, 0), "/tmp/a.png") == 0);
    cui_end_frame(ui);
    CHECK(cui_dropped_file_count(ui) == 0);
    click_at(ui, build_pickers_root, &a, z.x, z.y);
    CHECK(a.drop_click == 1);
    CHECK(cui_path_has_extension("photo.JPeG", "png;jpeg") && !cui_path_has_extension("dir.png/file", "png"));
    /* signature: a drag makes a stroke */
    cui_rect sr = rect_of(ui, a.sig_id);
    drag(ui, build_pickers_root, &a, sr.x + 40, sr.y + 60, sr.x + 200, 12);
    const cui_vec2 *pts; const int *ends; int nstroke = 0;
    int np = cui_signature_points(ui, "sig", &pts, &ends, &nstroke);
    CHECK(np > 5 && nstroke == 1);
    cui_signature_clear(ui, "sig");
    CHECK(cui_signature_points(ui, "sig", NULL, NULL, NULL) == 0);
    cui_destroy(ui);
}

/* disclosure ---------------------------------------------------------------- */

typedef struct {
    bool card_built, card_open, banner, confirmed, et_toggled; int step_action, active;
    cui_check_step steps[3]; cui_id link_id, del_id, card_id, banner_id, et_id, list_id, scroll_id;
} disc_ctx;
static void build_disclosure(cui_ctx *ui, void *u) {
    disc_ctx *a = u;
    a->card_built = false;
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_START, .pad = 20, .gap = 20) {
        cui_column(ui, .width = CUI_PX(420), .gap = 14) {
            cui_banner(ui, "Update ready", &a->banner, .dismissible = true);
            a->banner_id = cui_make_id(ui, "Update ready") ^ 0xBA22u;
            cui_link(ui, "@jonas");
            a->link_id = cui_last_id(ui);
            cui_hover_card(ui, "card") { a->card_built = true; cui_text(ui, "Jonas Berg"); }
            if (cui_button(ui, "Delete")) cui_popover_open(ui, "confirm");
            a->del_id = cui_last_id(ui);
            if (cui_confirm_popover(ui, "confirm", .title = "Sure?")) a->confirmed = true;
            if (cui_expandable_text(ui, "et", "one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen "
                                    "seventeen eighteen nineteen twenty twenty-one twenty-two twenty-three twenty-four twenty-five", .lines = 1)) a->et_toggled = true;
            a->et_id = cui_make_id(ui, "et");
            cui_expandable_card(ui, "Billing", &a->card_open) { cui_text(ui, "Body"); }
            a->card_id = cui_make_id(ui, "Billing") ^ 0xEC4Du;
            int r = cui_checklist(ui, "steps", a->steps, 3);
            if (r >= 0) a->step_action = r;
            a->list_id = cui_make_id(ui, "steps");
        }
        static const cui_toc_item toc[] = { { "One", "one", 0 }, { "Two", "two", 0 }, { "Three", "three", 0 } };
        cui_scroll(ui, .id = "doc", .height = CUI_PX(200), .width = CUI_PX(300)) {
            for (int k = 0; k < 3; k++) {
                cui_toc_anchor(ui, toc[k].anchor);
                cui_box(ui, .height = CUI_PX(300)) { cui_text(ui, toc[k].title); }
            }
        }
        a->scroll_id = cui_make_id(ui, "doc");
        cui_column(ui, .width = CUI_PX(160)) { a->active = cui_toc(ui, "doc", toc, 3); }
    }
}

static void test_disclosure(void) {
    printf("disclosure: hover card, confirm popover, expandables, toc, banner, checklist\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 1000, 1100, 1);
    disc_ctx a;
    memset(&a, 0, sizeof a);
    a.banner = true; a.step_action = -1;
    a.steps[0] = (cui_check_step){ "First", "Do it", "Go", true };
    a.steps[1] = (cui_check_step){ "Second", "Then this", "Next", false };
    a.steps[2] = (cui_check_step){ "Third", NULL, NULL, false };
    run(ui, build_disclosure, &a, 3);
    /* hover card: needs a short rest on the anchor, closes once the pointer leaves */
    cui_vec2 l = center(rect_of(ui, a.link_id));
    cui_input_mouse_move(ui, l.x, l.y);
    run(ui, build_disclosure, &a, 5);
    CHECK(!a.card_built);
    run(ui, build_disclosure, &a, 40);
    CHECK(a.card_built);
    cui_input_mouse_move(ui, 990, 1090);
    run(ui, build_disclosure, &a, 40);
    CHECK(!a.card_built);
    /* confirm popover */
    cui_vec2 dl = center(rect_of(ui, a.del_id));
    click_at(ui, build_disclosure, &a, dl.x, dl.y);
    run(ui, build_disclosure, &a, 10);
    key(ui, build_disclosure, &a, CUI_KEY_ENTER, 0);
    CHECK(a.confirmed);
    run(ui, build_disclosure, &a, 20);
    /* expandable text: clamped, then expanded */
    cui_rect er = rect_of(ui, a.et_id);
    click_at(ui, build_disclosure, &a, er.x + 20, er.y + er.h - 8);
    CHECK(a.et_toggled);
    run(ui, build_disclosure, &a, 40);
    CHECK(rect_of(ui, a.et_id).h > er.h + 10);
    /* expandable card */
    cui_vec2 cc = center(rect_of(ui, a.card_id));
    click_at(ui, build_disclosure, &a, cc.x, cc.y);
    CHECK(a.card_open);
    /* checklist: the open step's action, and toggling a circle */
    run(ui, build_disclosure, &a, 40);
    cui_rect lr = rect_of(ui, a.list_id);
    (void)lr;
    key(ui, build_disclosure, &a, CUI_KEY_TAB, 0);
    /* banner dismiss: the X is at the far right of the banner */
    cui_rect br = rect_of(ui, a.banner_id);
    float link_y = rect_of(ui, a.link_id).y;
    click_at(ui, build_disclosure, &a, br.x + br.w - 22, br.y + br.h / 2);
    CHECK(!a.banner);
    run(ui, build_disclosure, &a, 40);
    CHECK_NEAR(rect_of(ui, a.link_id).y, link_y - br.h - 14, 1.0);   /* the banner and its gap collapsed away */
    /* table of contents follows the scroll position */
    CHECK(a.active == 0);
    cui_vec2 sc = center(rect_of(ui, a.scroll_id));
    cui_input_mouse_move(ui, sc.x, sc.y);
    for (int i = 0; i < 12; i++) { cui_input_scroll(ui, 0, -2); run(ui, build_disclosure, &a, 3); }
    run(ui, build_disclosure, &a, 40);
    CHECK(a.active >= 1);
    cui_destroy(ui);
}

static void test_checklist(void) {
    printf("checklist: action of the open step, circle toggles\n");
    cui_ctx *ui = new_ctx();
    disc_ctx a;
    memset(&a, 0, sizeof a);
    a.step_action = -1;
    a.steps[0] = (cui_check_step){ "First", "Do it", "Go", true };
    a.steps[1] = (cui_check_step){ "Second", "Then this", "Next", false };
    a.steps[2] = (cui_check_step){ "Third", NULL, NULL, false };
    run(ui, build_disclosure, &a, 40);
    /* the Next button is the only primary button in the checklist; find it by its label id */
    cui_rect lr = rect_of(ui, a.list_id);
    /* click through the list rows until the action fires */
    for (float y = lr.y + 60; y < lr.y + lr.h && a.step_action < 0; y += 6) click_at(ui, build_disclosure, &a, lr.x + 90, y);
    CHECK(a.step_action == 1);
    cui_destroy(ui);
}

/* data grid ------------------------------------------------------------------ */

typedef struct { bool sel[500]; cui_table_sort sort; char names[500][16]; int committed; int cursor; bool open; uint32_t filt[2]; char q[32]; int json_ok, json_bad; cui_id grid_id, tree_id, fb_id; } dgrid_ctx;
static void build_dgrid(cui_ctx *ui, void *u) {
    dgrid_ctx *a = u;
    cui_column(ui, .pad = 20, .gap = 12, .width = CUI_FILL, .height = CUI_FILL) {
        static const char *const o1[] = { "Red", "Green", "Blue" };
        static const cui_filter_field fields[] = { { "Color", NULL, o1, 3 }, { "Size", NULL, o1, 3 } };
        cui_filter_bar(ui, "fb", fields, 2, a->filt, a->q, sizeof a->q);
        a->fb_id = cui_last_id(ui);
        static const cui_grid_column cols[] = { { "Name", 200, CUI_TEXT_LEFT, true }, { "Value", 120, CUI_TEXT_RIGHT, true } };
        int f, l;
        if (cui_data_grid_begin(ui, "grid", cols, 2, 500, &f, &l, .height = CUI_PX(300), .selected = a->sel, .sort = &a->sort, .cursor = &a->cursor)) {
            for (int r = f; r < l; r++) {
                cui_data_grid_row(ui, r);
                if (cui_data_grid_cell_edit(ui, a->names[r], sizeof a->names[r])) a->committed++;
                char v[16];
                snprintf(v, sizeof v, "%d", r * 3);
                cui_data_grid_cell_text(ui, v);
            }
            cui_data_grid_end(ui);
        }
        a->grid_id = cui_make_id(ui, "grid");
        cui_tree_cell(ui, "folder", 0, true, &a->open, NULL);
        a->tree_id = cui_make_id(ui, "folder") ^ 0x7CE1u;
        a->json_ok = cui_json_view(ui, "j1", "{\"a\": [1, 2, {\"b\": null}], \"c\": \"x\\\"y\", \"d\": -1.5e3, \"e\": true}");
        a->json_bad = cui_json_view(ui, "j2", "{\"a\": [1, 2,}");
        cui_code_block(ui, "code", "int main(void) {\n    return 0; /* done */\n}\n", .lang = "c", .line_numbers = true);
    }
}

static void test_data_grid(void) {
    printf("data grid: virtual rows, selection, sort, keyboard, editing; filter bar, tree cell, json\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 900, 1100, 1);
    dgrid_ctx a;
    memset(&a, 0, sizeof a);
    a.cursor = -1;
    for (int k = 0; k < 500; k++) snprintf(a.names[k], sizeof a.names[k], "row %d", k);
    run(ui, build_dgrid, &a, 3);
    CHECK(a.json_ok == 1 && a.json_bad == 0);
    cui_rect g = rect_of(ui, a.grid_id);
    CHECK(g.h > 290 && g.h < 310);
    /* only the visible rows are built: far fewer nodes than 500 rows would need */
    /* click row 1 (header 40, rows 40) */
    float row_y = g.y + 40 + 40 * 1 + 20;
    click_at(ui, build_dgrid, &a, g.x + 120, row_y);
    CHECK(a.sel[1] && !a.sel[0] && a.cursor == 1);
    /* shift click row 4 selects 1..4 */
    cui_input_key(ui, CUI_KEY_NONE, false, CUI_MOD_SHIFT);
    cui_input_mouse_move(ui, g.x + 120, g.y + 40 + 40 * 4 + 20);
    run(ui, build_dgrid, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_dgrid, &a, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_dgrid, &a, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    int n = 0;
    for (int k = 0; k < 500; k++) n += a.sel[k];
    CHECK(n == 4 && a.sel[1] && a.sel[4]);
    /* keyboard: Down moves the cursor, Escape clears, Ctrl+A selects all */
    key(ui, build_dgrid, &a, CUI_KEY_DOWN, 0);
    CHECK(a.cursor == 5);
    key(ui, build_dgrid, &a, CUI_KEY_ESCAPE, 0);
    n = 0;
    for (int k = 0; k < 500; k++) n += a.sel[k];
    CHECK(n == 0);
    key(ui, build_dgrid, &a, CUI_KEY_A, CUI_MOD_PRIMARY);
    n = 0;
    for (int k = 0; k < 500; k++) n += a.sel[k];
    CHECK(n == 500);
    key(ui, build_dgrid, &a, CUI_KEY_END, 0);
    run(ui, build_dgrid, &a, 40);
    CHECK(a.cursor == 499);
    key(ui, build_dgrid, &a, CUI_KEY_HOME, 0);
    run(ui, build_dgrid, &a, 40);
    /* sort: clicking the Value header sorts by it */
    click_at(ui, build_dgrid, &a, g.x + 44 + 200 + 60, g.y + 20);
    CHECK(a.sort.column == 1 && !a.sort.descending);
    click_at(ui, build_dgrid, &a, g.x + 44 + 200 + 60, g.y + 20);
    CHECK(a.sort.descending);
    /* editing: Enter on the cursor row edits, typing replaces, Enter commits */
    click_at(ui, build_dgrid, &a, g.x + 300, g.y + 40 + 20);   /* back to the grid, row 0 */
    key(ui, build_dgrid, &a, CUI_KEY_ENTER, 0);
    run(ui, build_dgrid, &a, 1);
    type(ui, build_dgrid, &a, "first");
    key(ui, build_dgrid, &a, CUI_KEY_ENTER, 0);
    CHECK(strcmp(a.names[0], "first") == 0 && a.committed == 1);
    /* a double click on a cell edits it too */
    cui_input_mouse_move(ui, g.x + 120, g.y + 40 + 40 * 2 + 20);
    run(ui, build_dgrid, &a, 1);
    for (int c = 0; c < 2; c++) {
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        run(ui, build_dgrid, &a, 1);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        run(ui, build_dgrid, &a, 1);
    }
    run(ui, build_dgrid, &a, 1);
    type(ui, build_dgrid, &a, "second");
    CHECK(cui_wants_keyboard(ui) && strcmp(a.names[2], "second") == 0);   /* the double click opened an editor */
    key(ui, build_dgrid, &a, CUI_KEY_ESCAPE, 0);   /* Esc reverts */
    CHECK(strcmp(a.names[2], "row 2") == 0);
    /* column resize: drag the Name header's right edge */
    float edge = g.x + 44 + 200;
    drag(ui, build_dgrid, &a, edge, g.y + 20, edge + 60, 6);
    run(ui, build_dgrid, &a, 2);
    click_at(ui, build_dgrid, &a, g.x + 44 + 260 + 60, g.y + 20);   /* Value header moved right by 60 */
    CHECK(!a.sort.descending);
    /* tree cell */
    cui_vec2 tc = center(rect_of(ui, a.tree_id));
    click_at(ui, build_dgrid, &a, tc.x, tc.y);
    CHECK(a.open);
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
    /* a small upward drag that does not pass the row above keeps the order */
    cui_input_mouse_move(ui, 150, 130);
    run(ui, build_reorder, &r, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_reorder, &r, 1);
    for (int y = 130; y >= 120; y -= 2) { cui_input_mouse_move(ui, 150, (float)y); run(ui, build_reorder, &r, 1); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_reorder, &r, 3);
    CHECK(r.changed == 1);
    cui_destroy(ui);

    /* a list that first appears under the pointer does not reorder itself */
    ui = new_ctx();
    reorder_ctx r2 = { { 0, 1, 2, 3 }, 0, 0 };
    cui_input_mouse_move(ui, 150, 190);
    run(ui, build_reorder, &r2, 5);
    CHECK(r2.changed == 0 && r2.order[0] == 0 && r2.order[3] == 3);
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

/* pretty wrapping -------------------------------------------------------- */

/* the library's internal line record; same tag and members, so a compatible type */
struct cui_text_layout_line { int start, end; float width; };
int cui__text_wrap(cui_ctx *ui, cui_font font, float size, const char *text, int len, float width,
                   struct cui_text_layout_line *lines, int max_lines);

static int last_line_words(const char *t, struct cui_text_layout_line *l) {
    int w = 1;
    for (int i = l->start; i < l->end; i++) w += t[i] == ' ';
    return w;
}

static void test_pretty_wrap(void) {
    printf("text: wrapped paragraphs do not end on a lone word\n");
    cui_ctx *ui = new_ctx();
    run(ui, build_empty, NULL, 1);
    const char *t = "Keyboard navigation in the date range picker";
    float full = cui_measure_text(ui, CUI_FONT_MEDIUM, 14, t, -1).width;
    struct cui_text_layout_line lines[8];
    /* a width that greedily leaves only the last word on the second line */
    float w = full - 4;
    int n = cui__text_wrap(ui, CUI_FONT_MEDIUM, 14, t, (int)strlen(t), w, lines, 8);
    CHECK(n == 2);
    CHECK(last_line_words(t, &lines[n - 1]) >= 2);
    /* the line count is kept: a width that needs three lines still gets three */
    int n3 = cui__text_wrap(ui, CUI_FONT_MEDIUM, 14, t, (int)strlen(t), full * 0.4f, lines, 8);
    CHECK(n3 >= 3);
    CHECK(last_line_words(t, &lines[n3 - 1]) >= 2);
    /* a single paragraph line is untouched */
    CHECK(cui__text_wrap(ui, CUI_FONT_MEDIUM, 14, t, (int)strlen(t), full + 10, lines, 8) == 1);
    cui_destroy(ui);
}

/* truncated labels -------------------------------------------------------- */

typedef struct { float w; int clicks; } trunc_ctx;
static void build_trunc(cui_ctx *ui, void *u) {
    trunc_ctx *t = u;
    cui_column(ui, .pad = 20) {
        if (cui_card_begin(ui, &(cui_card_opts){ .id = "row", .pad = 8, .hoverable = true, .width = CUI_PX(t->w) })) t->clicks++;
        cui_text(ui, "A rather long file name that will not fit.pdf", .nowrap = true, .width = CUI_FILL);
        cui_card_end(ui);
    }
}

typedef struct { const char *v; } roll_ctx;
static void build_roll(cui_ctx *ui, void *user) {
    roll_ctx *r = user;
    cui_row(ui) { cui_number_roll(ui, "n", r->v, .size = 20); }
}
static void test_number_roll(void) {
    printf("motion: a rolling number settles on its text, keeps a fixed prefix and fits its width\n");
    cui_ctx *ui = new_ctx();
    cui_set_reduced_motion(ui, false);
    roll_ctx r = { "$64,210" };
    run(ui, build_roll, &r, 3);
    cui_rect a = rect_of(ui, cui_make_id(ui, "n"));
    CHECK(a.w > 40);
    r.v = "$64,987";
    run(ui, build_roll, &r, 2);
    CHECK(cui_needs_redraw(ui));
    run(ui, build_roll, &r, 90);
    CHECK(!cui_needs_redraw(ui));
    cui_rect b = rect_of(ui, cui_make_id(ui, "n"));
    CHECK(fabsf(b.w - a.w) < 0.5f);   /* tabular digits: same count, same width */
    r.v = "$9,870";
    run(ui, build_roll, &r, 90);
    cui_rect c = rect_of(ui, cui_make_id(ui, "n"));
    CHECK(c.w < b.w - 5);
    cui_set_reduced_motion(ui, true);
    r.v = "$12";
    run(ui, build_roll, &r, 2);
    CHECK(!cui_needs_redraw(ui));
    cui_destroy(ui);
}

static void test_truncated_tooltip(void) {
    printf("text: a truncated label shows its whole text on hover, and its row still clicks\n");
    cui_ctx *ui = new_ctx();
    trunc_ctx t = { 600, 0 };
    cui_input_mouse_move(ui, 60, 36);
    run(ui, build_trunc, &t, 70);
    int wide = cui_get_draw_data(ui)->vertex_count;
    t.w = 140;
    run(ui, build_trunc, &t, 3);
    int before = cui_get_draw_data(ui)->vertex_count;
    run(ui, build_trunc, &t, 70);
    int after = cui_get_draw_data(ui)->vertex_count;
    CHECK(after > before);      /* the tooltip appeared */
    (void)wide;
    /* the row under the label keeps its click */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_trunc, &t, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_trunc, &t, 1);
    CHECK(t.clicks == 1);
    /* a label that fits gets no tooltip */
    t.w = 600;
    cui_input_mouse_move(ui, 400, 400);
    run(ui, build_trunc, &t, 30);
    cui_input_mouse_move(ui, 60, 36);
    run(ui, build_trunc, &t, 3);
    int fit0 = cui_get_draw_data(ui)->vertex_count;
    run(ui, build_trunc, &t, 70);
    CHECK(cui_get_draw_data(ui)->vertex_count == fit0);
    cui_destroy(ui);
}

/* scroll edge fades ------------------------------------------------------ */

typedef struct { bool fade; int rows; } fade_ctx;
static void build_fade(cui_ctx *ui, void *u) {
    fade_ctx *f = u;
    cui_scroll(ui, .id = "f", .height = CUI_PX(120), .width = CUI_PX(200), .fade_edges = f->fade) {
        for (int i = 0; i < f->rows; i++) { cui_push_id_int(ui, i); cui_text(ui, "Row"); cui_pop_id(ui); }
    }
}

static void test_scroll_fade(void) {
    printf("scroll: edge fades only where there is more to scroll\n");
    cui_ctx *ui = new_ctx();
    fade_ctx f = { false, 30 };
    run(ui, build_fade, &f, 3);
    int plain = cui_get_draw_data(ui)->vertex_count;
    f.fade = true;
    run(ui, build_fade, &f, 3);
    int faded = cui_get_draw_data(ui)->vertex_count;
    CHECK(faded > plain);              /* a bottom fade, none at the top yet */
    f.rows = 2;
    run(ui, build_fade, &f, 3);
    int short_fade = cui_get_draw_data(ui)->vertex_count;
    f.fade = false;
    run(ui, build_fade, &f, 3);
    CHECK(cui_get_draw_data(ui)->vertex_count == short_fade);   /* nothing to scroll, nothing drawn */
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

/* ------------------------------------------------------------------------ */

static void drag2(cui_ctx *ui, build_fn fn, void *u, float x0, float y0, float x1, float y1, int steps) {
    cui_input_mouse_move(ui, x0, y0);
    run(ui, fn, u, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, fn, u, 1);
    for (int i = 1; i <= steps; i++) { cui_input_mouse_move(ui, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps); run(ui, fn, u, 1); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, fn, u, 1);
}

typedef struct {
    int cols[3][6], len[3];
    cui_id card_ids[10];
    bool moved, added, clicked;
    cui_kanban_move mv;
    cui_cal_event ev[2];
    int ev_clicked;
    cui_id week_id;
    cui_gantt_task tasks[2];
    int task_clicked;
    cui_id gantt_id;
    int mode;   /* 0 kanban, 1 week, 2 gantt */
} plan_ctx;

static void build_plan(cui_ctx *ui, void *u) {
    plan_ctx *a = u;
    cui_column(ui, .pad = 16, .gap = 16, .width = CUI_FILL) {
        if (a->mode == 0) {
            cui_kanban_begin(ui, "kb");
            for (int c = 0; c < 3; c++) {
                if (cui_kanban_column_begin(ui, "Col", a->len[c], .width = 220)) a->added = true;
                for (int k = 0; k < a->len[c]; k++) {
                    int id = a->cols[c][k];
                    if (cui_kanban_card_begin(ui, id)) a->clicked = true;
                    a->card_ids[id] = cui_last_id(ui);
                    cui_text(ui, "A card with a title");
                    cui_kanban_card_end(ui);
                }
                cui_kanban_column_end(ui);
            }
            if (cui_kanban_end(ui, &a->mv)) a->moved = true;
        } else if (a->mode == 1) {
            cui_date wk = { 2026, 3, 2 };
            int r = cui_week_calendar(ui, "wk", wk, a->ev, 2, .start_hour = 8, .end_hour = 18, .hour_height = 40, .height = CUI_PX(500));
            if (r >= 0) a->ev_clicked = r;
            a->week_id = cui_last_id(ui);
        } else {
            cui_date org = { 2026, 3, 2 };
            int r = cui_gantt(ui, "gt", a->tasks, 2, org, 30, .day_width = 30, .row_height = 40);
            if (r >= 0) a->task_clicked = r;
            a->gantt_id = cui_last_id(ui);
        }
    }
}

static void kb_move(plan_ctx *a) {
    cui_kanban_move *m = &a->mv;
    int at = -1;
    for (int k = 0; k < a->len[m->from_column]; k++) if (a->cols[m->from_column][k] == m->card) at = k;
    for (int k = at; k < a->len[m->from_column] - 1; k++) a->cols[m->from_column][k] = a->cols[m->from_column][k + 1];
    a->len[m->from_column]--;
    for (int k = a->len[m->to_column]; k > m->to_index; k--) a->cols[m->to_column][k] = a->cols[m->to_column][k - 1];
    a->cols[m->to_column][m->to_index] = m->card;
    a->len[m->to_column]++;
}

static void test_planning(void) {
    printf("planning: kanban drag between and within columns, week calendar move and resize, gantt reschedule\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 900, 800, 1);
    plan_ctx a;
    memset(&a, 0, sizeof a);
    a.cols[0][0] = 1; a.cols[0][1] = 2; a.cols[0][2] = 3; a.len[0] = 3;
    a.cols[1][0] = 4; a.len[1] = 1;
    run(ui, build_plan, &a, 3);
    cui_rect c1 = rect_of(ui, a.card_ids[1]), c4 = rect_of(ui, a.card_ids[4]);
    CHECK(c1.w > 150 && c4.x > c1.x + c1.w);
    /* card 1 goes below card 4 */
    drag2(ui, build_plan, &a, c1.x + 40, c1.y + 20, c4.x + 60, c4.y + c4.h + 20, 8);
    CHECK(a.moved && a.mv.card == 1 && a.mv.from_column == 0 && a.mv.to_column == 1 && a.mv.to_index == 1);
    CHECK(!a.clicked);
    kb_move(&a);
    a.moved = false;
    run(ui, build_plan, &a, 2);
    /* card 3 goes above card 2 in the same column */
    cui_rect c2 = rect_of(ui, a.card_ids[2]), c3 = rect_of(ui, a.card_ids[3]);
    CHECK(c3.y > c2.y);
    drag2(ui, build_plan, &a, c3.x + 40, c3.y + 20, c2.x + 40, c2.y + 6, 8);
    CHECK(a.moved && a.mv.card == 3 && a.mv.to_column == 0 && a.mv.to_index == 0);
    kb_move(&a);
    CHECK(a.cols[0][0] == 3 && a.cols[0][1] == 2 && a.cols[1][1] == 1);
    /* the empty third column accepts a drop */
    a.moved = false;
    run(ui, build_plan, &a, 2);
    cui_rect c2b = rect_of(ui, a.card_ids[2]);
    drag2(ui, build_plan, &a, c2b.x + 40, c2b.y + 20, c4.x + 260, c4.y + 30, 8);
    CHECK(a.moved && a.mv.card == 2 && a.mv.to_column == 2 && a.mv.to_index == 0);
    /* a plain click on a card is a click, not a move */
    a.moved = false;
    run(ui, build_plan, &a, 2);
    cui_rect c4b = rect_of(ui, a.card_ids[4]);
    click_at(ui, build_plan, &a, c4b.x + 30, c4b.y + 20);
    CHECK(a.clicked && !a.moved);
    /* keyboard: Space lifts the focused card, arrows pick the slot, Enter drops */
    a.moved = false; a.clicked = false;
    key(ui, build_plan, &a, CUI_KEY_TAB, CUI_MOD_SHIFT);
    key(ui, build_plan, &a, CUI_KEY_TAB, 0);            /* card 4 again, now with a visible focus */
    key(ui, build_plan, &a, CUI_KEY_SPACE, 0);
    CHECK(!a.moved && !a.clicked);
    key(ui, build_plan, &a, CUI_KEY_LEFT, 0);
    key(ui, build_plan, &a, CUI_KEY_DOWN, 0);
    run(ui, build_plan, &a, 2);
    key(ui, build_plan, &a, CUI_KEY_ENTER, 0);
    CHECK(a.moved && a.mv.card == 4 && a.mv.from_column == 1 && a.mv.to_column == 0 && a.mv.to_index == 1);
    CHECK(!a.clicked);
    /* Escape puts a lifted card back */
    a.moved = false;
    key(ui, build_plan, &a, CUI_KEY_SPACE, 0);
    key(ui, build_plan, &a, CUI_KEY_RIGHT, 0);
    key(ui, build_plan, &a, CUI_KEY_ESCAPE, 0);
    run(ui, build_plan, &a, 2);
    CHECK(!a.moved);

    /* week calendar */
    a.mode = 1;
    a.ev[0] = (cui_cal_event){ "Review", NULL, 1, 10, 11, CUI_TONE_INFO };
    a.ev[1] = (cui_cal_event){ "Lunch", NULL, 3, 12, 13, CUI_TONE_SUCCESS };
    a.ev_clicked = -1;
    run(ui, build_plan, &a, 3);
    cui_rect w = rect_of(ui, a.week_id);
    float dw = (w.w - 60) / 7, top = w.y + 57;
    CHECK(w.h > 500 && dw > 80);
    float ex = w.x + 60 + 1.5f * dw, ey = top + (10.5f - 8) * 40;
    click_at(ui, build_plan, &a, ex, ey);
    CHECK(a.ev_clicked == 0);
    a.ev_clicked = -1;
    drag2(ui, build_plan, &a, ex, ey, ex + dw, ey + 40, 6);
    CHECK(a.ev[0].day == 2 && a.ev[0].start == 11 && a.ev[0].end == 12);
    CHECK(a.ev_clicked == -1);
    run(ui, build_plan, &a, 2);
    /* the bottom edge resizes by half an hour */
    float bx = w.x + 60 + 2.5f * dw, by = top + (12 - 8) * 40 - 4;
    drag2(ui, build_plan, &a, bx, by, bx, by + 20, 4);
    CHECK(a.ev[0].start == 11 && a.ev[0].end == 12.5f);
    /* events cannot be dragged past the last hour */
    run(ui, build_plan, &a, 2);
    float lx = w.x + 60 + 3.5f * dw, ly = top + (12.5f - 8) * 40;
    drag2(ui, build_plan, &a, lx, ly, lx, ly + 400, 6);
    CHECK(a.ev[1].end == 18 && a.ev[1].start == 17);
    /* keyboard: Tab to the first event, Down moves it 15 minutes, Shift+Down stretches it, Left moves a day */
    cui_input_mouse_move(ui, 5, 5);
    run(ui, build_plan, &a, 1);
    key(ui, build_plan, &a, CUI_KEY_ESCAPE, 0);
    key(ui, build_plan, &a, CUI_KEY_TAB, 0);
    {
        float s0 = a.ev[0].start, e0 = a.ev[0].end; int d0 = a.ev[0].day;
        key(ui, build_plan, &a, CUI_KEY_DOWN, 0);
        CHECK(a.ev[0].start == s0 + 0.25f && a.ev[0].end == e0 + 0.25f);
        key(ui, build_plan, &a, CUI_KEY_DOWN, CUI_MOD_SHIFT);
        CHECK(a.ev[0].start == s0 + 0.25f && a.ev[0].end == e0 + 0.5f);
        key(ui, build_plan, &a, CUI_KEY_LEFT, 0);
        CHECK(a.ev[0].day == d0 - 1);
    }

    /* gantt */
    a.mode = 2;
    a.tasks[0] = (cui_gantt_task){ "Build", "Maya", 2, 6, 0.5f, CUI_TONE_INFO, -1, false };
    a.tasks[1] = (cui_gantt_task){ "Ship", NULL, 10, 10, 0, CUI_TONE_SUCCESS, 0, true };
    a.task_clicked = -1;
    run(ui, build_plan, &a, 3);
    cui_rect g = rect_of(ui, a.gantt_id);
    float x0 = g.x + 231, y0 = g.y + 53;
    CHECK(g.h > 120);
    click_at(ui, build_plan, &a, x0 + 4 * 30, y0 + 20);
    CHECK(a.task_clicked == 0);
    a.task_clicked = -1;
    drag2(ui, build_plan, &a, x0 + 4 * 30, y0 + 20, x0 + 6 * 30, y0 + 20, 6);
    CHECK(a.tasks[0].start == 4 && a.tasks[0].end == 8 && a.task_clicked == -1);
    run(ui, build_plan, &a, 2);
    /* pull the right end out two days */
    drag2(ui, build_plan, &a, x0 + 8 * 30 - 4, y0 + 20, x0 + 10 * 30 - 4, y0 + 20, 6);
    CHECK(a.tasks[0].start == 4 && a.tasks[0].end == 10);
    run(ui, build_plan, &a, 2);
    /* and the left end in one day */
    drag2(ui, build_plan, &a, x0 + 4 * 30 + 3, y0 + 20, x0 + 5 * 30 + 3, y0 + 20, 6);
    CHECK(a.tasks[0].start == 5 && a.tasks[0].end == 10);
    /* keyboard: arrows move a day, Shift+arrows change the length */
    click_at(ui, build_plan, &a, x0 + 7 * 30, y0 + 20);   /* focus the bar, then make the focus visible */
    key(ui, build_plan, &a, CUI_KEY_TAB, 0);
    key(ui, build_plan, &a, CUI_KEY_TAB, CUI_MOD_SHIFT);
    key(ui, build_plan, &a, CUI_KEY_RIGHT, 0);
    CHECK(a.tasks[0].start == 6 && a.tasks[0].end == 11);
    key(ui, build_plan, &a, CUI_KEY_LEFT, CUI_MOD_SHIFT);
    CHECK(a.tasks[0].start == 6 && a.tasks[0].end == 10);
    cui_destroy(ui);
}

typedef struct {
    int likes; bool liked; int flags;
    int slide; float split; int thrown;
    cui_id car_id, cmp_id, stack_id;
    cui_texture img;
} media_ctx;

static void build_media(cui_ctx *ui, void *u) {
    media_ctx *a = u;
    cui_column(ui, .pad = 16, .gap = 16, .width = CUI_FILL) {
        a->flags |= cui_comment(ui, "c", "Maya Okafor", "Looks good to me.", .likes = &a->likes, .liked = &a->liked);
        cui_chat_message(ui, "Hello", .author = "Jonas");
        cui_chat_message(ui, "Hi!", .mine = true, .status = "Read");
        cui_carousel(ui, "car", 3, &a->slide, .height = 120) {
            for (int k = 0; k < 3; k++) cui_carousel_slide(ui) { cui_text(ui, "Slide"); }
        }
        a->car_id = cui_last_id(ui);
        cui_image_compare(ui, "cmp", a->img, a->img, &a->split, .width = CUI_PX(400), .height = 120);
        a->cmp_id = cui_last_id(ui);
        cui_card_stack_begin(ui, "stk", 3, &(cui_card_stack_opts){ .width = CUI_PX(240), .height = 120 });
        for (int k = 0; k < 3; k++) cui_card_stack_item(ui, k) { cui_text(ui, "Card"); }
        int d = cui_card_stack_end(ui);
        if (d) a->thrown = d;
        a->stack_id = cui_last_id(ui);
    }
}

static void test_media(void) {
    printf("social and media: comment likes, carousel drag and keys, compare slider, card stack throw\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 900, 1000, 1);
    media_ctx a;
    memset(&a, 0, sizeof a);
    a.likes = 4; a.split = 0.5f;
    static unsigned char px[16 * 16 * 4];
    a.img = cui_image_create(ui, 16, 16, px);
    run(ui, build_media, &a, 3);
    /* Tab reaches the like button first; Enter toggles it */
    key(ui, build_media, &a, CUI_KEY_TAB, 0);
    key(ui, build_media, &a, CUI_KEY_ENTER, 0);
    CHECK(a.liked && a.likes == 5 && (a.flags & CUI_COMMENT_LIKE));
    key(ui, build_media, &a, CUI_KEY_ENTER, 0);
    CHECK(!a.liked && a.likes == 4);
    /* carousel: drag left past half a slide */
    cui_rect cr = rect_of(ui, a.car_id);
    CHECK(cr.w > 600 && cr.h >= 120);
    drag2(ui, build_media, &a, cr.x + cr.w * 0.7f, cr.y + 50, cr.x + cr.w * 0.1f, cr.y + 50, 8);
    run(ui, build_media, &a, 20);
    CHECK(a.slide == 1);
    key(ui, build_media, &a, CUI_KEY_RIGHT, 0);
    CHECK(a.slide == 2);
    key(ui, build_media, &a, CUI_KEY_RIGHT, 0);
    CHECK(a.slide == 2);   /* stops at the end without loop */
    /* compare: drag the divider to three quarters */
    cui_rect pr = rect_of(ui, a.cmp_id);
    CHECK(pr.w > 390 && pr.w < 410);
    drag2(ui, build_media, &a, pr.x + pr.w * 0.5f, pr.y + 60, pr.x + pr.w * 0.75f, pr.y + 60, 6);
    CHECK(a.split > 0.72f && a.split < 0.78f);
    /* card stack: a long drag right throws the top card */
    cui_rect sr = rect_of(ui, a.stack_id);
    CHECK(sr.w > 230 && sr.w < 250);
    drag2(ui, build_media, &a, sr.x + 120, sr.y + 50, sr.x + 320, sr.y + 50, 10);
    run(ui, build_media, &a, 60);
    CHECK(a.thrown == 1);
    /* a short drag springs back */
    a.thrown = 0;
    drag2(ui, build_media, &a, sr.x + 120, sr.y + 50, sr.x + 140, sr.y + 50, 10);
    run(ui, build_media, &a, 60);
    CHECK(a.thrown == 0);
    cui_destroy(ui);
}

typedef struct { int mode; int hit; cui_id id; int empty; } viz_ctx;

static void build_viz(cui_ctx *ui, void *u) {
    viz_ctx *a = u;
    static const float hm[3 * 4] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    static const cui_viz_item items[] = { { "a", 10 }, { "b", 40 }, { "c", 25 }, { "d", 25 } };
    static const char *nodes[] = { "A", "B", "C", "D" };
    static const cui_sankey_link links[] = { { 0, 2, 10 }, { 1, 2, 5 }, { 2, 3, 12 }, { 3, 0, 1 } };   /* with a cycle */
    static float days[60];
    static const cui_vec2 pts[] = { { 1, 1 }, { 5, 5 }, { 9, 9 } };
    static const float rv[] = { 1, 2, 3, 4, 5 };
    static const char *axes[] = { "p", "q", "r", "s", "t" };
    for (int k = 0; k < 60; k++) days[k] = (float)(k % 5);
    int r = -1;
    cui_column(ui, .pad = 20, .width = CUI_FILL) {
        if (a->empty) {
            /* nothing to show, zeros and NaN: no crash, no NaN geometry */
            float nanv = NAN;
            cui_viz_item z[2] = { { "z", 0 }, { "n", nanv } };
            cui_heatmap(ui, "h0", NULL, 0, 0);
            cui_treemap(ui, "t0", z, 2);
            cui_funnel(ui, "f0", z, 2);
            cui_waffle(ui, "w0", z, 2);
            cui_split_bar(ui, "s0", z, 2);
            cui_bar_list(ui, "b0", z, 2);
            cui_activity_rings(ui, "r0", z, 2);
            cui_sankey(ui, "k0", nodes, 4, NULL, 0);
            cui_scatter_chart(ui, "c0", NULL, 0);
            cui_radar_chart(ui, "d0", NULL, 0, axes, 2);
            cui_date d0 = { 2026, 1, 1 };
            cui_calendar_heatmap(ui, "y0", d0, days, 0);
        } else switch (a->mode) {
        case 0: r = cui_heatmap(ui, "hm", hm, 3, 4, .cell = 30); break;
        case 1: r = cui_treemap(ui, "tm", items, 4, .width = CUI_PX(400), .height = 200); break;
        case 2: r = cui_funnel(ui, "fn", items, 4, .width = CUI_PX(400)); break;
        case 3: r = cui_bar_list(ui, "bl", items, 4, .width = CUI_PX(400)); break;
        case 4: r = cui_split_bar(ui, "sb", items, 4, .width = CUI_PX(400)); break;
        case 5: r = cui_waffle(ui, "wf", items, 4, .cell = 10); break;
        case 6: r = cui_sankey(ui, "sk", nodes, 4, links, 4, .width = CUI_PX(500), .height = 200); break;
        case 7: r = cui_activity_rings(ui, "ar", items, 2, .size = 120, .thickness = 12); break;
        case 8: { cui_scatter_series ss = { "s", pts, NULL, 3, { 0 } }; r = cui_scatter_chart(ui, "sc", &ss, 1, .width = CUI_PX(400), .height = 240); } break;
        case 9: { cui_date d1 = { 2026, 3, 2 }; r = cui_calendar_heatmap(ui, "cal", d1, days, 60); } break;   /* a Monday */
        case 10: { cui_series rs = { "r", rv, 5, { 0 } }; r = cui_radar_chart(ui, "rd", &rs, 1, axes, 5, .size = 200); } break;
        }
        a->id = cui_last_id(ui);
    }
    if (r >= 0) a->hit = r;
}

static void test_viz(void) {
    printf("more charts: hit testing for every chart, empty and degenerate data\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 900, 800, 1);
    viz_ctx a;
    memset(&a, 0, sizeof a);
    a.empty = 1;
    run(ui, build_viz, &a, 4);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    bool finite = true;
    for (int v = 0; v < dd->vertex_count; v++) if (!isfinite(dd->vertices[v].x) || !isfinite(dd->vertices[v].y)) finite = false;
    CHECK(finite);
    a.empty = 0;
    cui_rect r;
    /* heatmap: cell (row 1, col 2) is index 6 */
    a.mode = 0; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + 2 * 33 + 15, r.y + 33 + 15);
    CHECK(a.hit == 6);
    /* treemap: the largest item fills the top left */
    a.mode = 1; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + 10, r.y + 10);
    CHECK(a.hit == 1);
    CHECK(r.w == 400 && r.h == 200);
    /* funnel: third row */
    a.mode = 2; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + r.w / 2, r.y + 2 * 54 + 20);
    CHECK(a.hit == 2);
    /* bar list: ranked, the first row is the largest item */
    a.mode = 3; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + 20, r.y + 15);
    CHECK(a.hit == 1);
    click_at(ui, build_viz, &a, r.x + 20, r.y + 3 * 40 + 15);
    CHECK(a.hit == 0);
    /* split bar: shares 10/100, then 40: x at 30% is item b */
    a.mode = 4; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + r.w * 0.3f, r.y + 5);
    CHECK(a.hit == 1);
    /* waffle: the first column is item a (10 cells) */
    a.mode = 5; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + 5, r.y + 5);
    CHECK(a.hit == 0);
    click_at(ui, build_viz, &a, r.x + 13 * 3 + 5, r.y + 5);
    CHECK(a.hit == 1);
    /* sankey: node C sits in the middle column */
    a.mode = 6; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    /* scan the middle for node C by clicking on a grid; only nodes report clicks */
    int got_c = 0, other = 0;
    for (float x = r.x + r.w * 0.2f; x < r.x + r.w * 0.8f && !got_c; x += 4)
        for (float y = r.y + 4; y < r.y + r.h && !got_c; y += 12) {
            a.hit = -1;
            click_at(ui, build_viz, &a, x, y);
            if (a.hit == 2) got_c = 1;
            else if (a.hit >= 0) other = 1;
        }
    CHECK(got_c && !other);
    /* rings: the outer ring is item 0 */
    a.mode = 7; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + r.w / 2, r.y + 6);
    CHECK(a.hit == 0);
    click_at(ui, build_viz, &a, r.x + r.w / 2, r.y + 6 + 15);
    CHECK(a.hit == 1);
    /* scatter: the nearest point */
    a.mode = 8; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    /* the middle point (5, 5) is near the plot's center */
    click_at(ui, build_viz, &a, r.x + r.w * 0.52f, r.y + r.h * 0.5f);
    CHECK(a.hit == 1);
    /* calendar: March 2 2026 is a Monday, so day 8 is the second column's Tuesday */
    a.mode = 9; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + 32 + 14 + 5, r.y + 20 + 14 + 5);
    CHECK(a.hit == 8);
    /* radar: the axis straight up is 0 */
    a.mode = 10; a.hit = -1; run(ui, build_viz, &a, 3); r = rect_of(ui, a.id);
    click_at(ui, build_viz, &a, r.x + r.w / 2, r.y + 40);
    CHECK(a.hit == 0);
    cui_destroy(ui);
}

typedef struct {
    char text[256]; bool done; int clicks, dock_hits; bool tile; int tab; bool tab_changed;
    float progress; bool seeked; cui_now_playing np; int np_flags;
    cui_id tile_id, tab_id, wave_id, np_id, dock_id, spot_id;
} motion_ctx;

static void build_motion(cui_ctx *ui, void *u) {
    motion_ctx *a = u;
    static const cui_tab_bar_item tabs[] = { { CUI_ICON_HOUSE, "Home", 0 }, { CUI_ICON_SEARCH, "Search", 0 }, { CUI_ICON_INBOX, "Inbox", 2 } };
    static float peaks[40];
    for (int k = 0; k < 40; k++) peaks[k] = 0.5f;
    cui_column(ui, .pad = 20, .gap = 12, .width = CUI_FILL) {
        a->done = cui_typewriter(ui, "tw", a->text, .speed = 60);
        cui_shimmer_text(ui, "Thinking");
        cui_text_reveal(ui, "rv", "Words settle into place");
        static const char *words[] = { "one", "two" };
        cui_word_rotate(ui, "wr", words, 2, 1.0f);
        cui_loader(ui, .style = CUI_LOADER_GRID);
        cui_row(ui, .gap = 10) {
            if (cui_control_tile(ui, "Wi-Fi", &a->tile, .width = CUI_PX(140))) a->clicks++;
            a->tile_id = cui_last_id(ui);
        }
        if (cui_tab_bar(ui, "tabs", tabs, 3, &a->tab, .width = CUI_PX(300))) a->tab_changed = true;
        a->tab_id = cui_last_id(ui);
        cui_box(ui, .width = CUI_PX(400)) { if (cui_waveform(ui, "wave", peaks, 40, &a->progress)) a->seeked = true; a->wave_id = cui_last_id(ui); }
        a->np_flags |= cui_now_playing(ui, "np", &a->np);
        a->np_id = cui_last_id(ui);
        cui_gradient_text(ui, "Gradient", .animate = true);
        cui_spotlight(ui, "spot", .width = CUI_PX(200), .beam = true) { cui_text(ui, "Inside"); a->spot_id = cui_last_id(ui); }
        cui_dock(ui, "dock") {
            if (cui_dock_item(ui, CUI_ICON_FOLDER, "Files")) a->dock_hits++;
            if (cui_dock_item(ui, CUI_ICON_MAIL, "Mail", .badge = 2)) a->dock_hits += 10;
        }
        a->dock_id = cui_last_id(ui);
    }
}

static void test_motion(void) {
    printf("motion: typewriter streams and restarts, tiles, tab bar, waveform seek, player, dock\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 900, 1000, 1);
    cui_set_reduced_motion(ui, false);
    motion_ctx a;
    memset(&a, 0, sizeof a);
    strcpy(a.text, "Hello there");   /* 11 characters at 60 a second */
    a.np = (cui_now_playing){ "Song", "Artist", 0, { 0 }, 30, 200, false };
    run(ui, build_motion, &a, 2);
    CHECK(!a.done);
    run(ui, build_motion, &a, 30);
    CHECK(a.done);
    /* streamed: more text carries on without starting over */
    strcat(a.text, ", and more");
    run(ui, build_motion, &a, 1);
    CHECK(!a.done);
    run(ui, build_motion, &a, 30);
    CHECK(a.done);
    /* a different text starts over */
    strcpy(a.text, "Something else entirely");
    run(ui, build_motion, &a, 1);
    CHECK(!a.done);
    cui_set_reduced_motion(ui, true);
    run(ui, build_motion, &a, 1);
    CHECK(a.done);   /* still motion shows everything at once */
    cui_set_reduced_motion(ui, false);
    /* tile toggles on click and on Space */
    cui_rect tr = rect_of(ui, a.tile_id);
    CHECK(tr.w == 140);
    click_at(ui, build_motion, &a, tr.x + 20, tr.y + 20);
    CHECK(a.tile && a.clicks == 1);
    key(ui, build_motion, &a, CUI_KEY_TAB, 0);
    key(ui, build_motion, &a, CUI_KEY_TAB, CUI_MOD_SHIFT);   /* back to the tile, with a visible focus */
    key(ui, build_motion, &a, CUI_KEY_SPACE, 0);
    CHECK(!a.tile && a.clicks == 2);
    /* tab bar: click the third item, then the arrow key goes back */
    cui_rect br = rect_of(ui, a.tab_id);
    click_at(ui, build_motion, &a, br.x + br.w * 5 / 6, br.y + br.h / 2);
    CHECK(a.tab == 2 && a.tab_changed);
    key(ui, build_motion, &a, CUI_KEY_LEFT, 0);
    CHECK(a.tab == 1);
    /* waveform: drag seeks */
    cui_rect wr = rect_of(ui, a.wave_id);
    drag(ui, build_motion, &a, wr.x + 10, wr.y + wr.h / 2, wr.x + wr.w * 0.75f, 5);
    CHECK(a.seeked && a.progress > 0.72f && a.progress < 0.78f);
    /* player: the play button toggles, the scrubber seeks */
    cui_rect nr = rect_of(ui, a.np_id);
    bool found_toggle = false;
    for (float x = nr.x + nr.w - 120; x < nr.x + nr.w && !found_toggle; x += 6) {
        a.np_flags = 0;
        click_at(ui, build_motion, &a, x, nr.y + 16 + 32);
        if (a.np_flags & CUI_PLAYER_TOGGLE) found_toggle = true;
    }
    CHECK(found_toggle && a.np.playing);
    a.np_flags = 0;
    drag(ui, build_motion, &a, nr.x + nr.w * 0.5f, nr.y + nr.h - 16 - 7, nr.x + nr.w * 0.5f + 40, 4);
    CHECK((a.np_flags & CUI_PLAYER_SEEK) && a.np.position > 30);
    /* spotlight: the card wraps its content */
    {
        cui_rect sr = rect_of(ui, cui_make_id(ui, "spot"));
        CHECK(sr.w == 200 && sr.h > 40);
        cui_input_mouse_move(ui, sr.x + 30, sr.y + 20);
        run(ui, build_motion, &a, 5);
        CHECK(cui_frame_count(ui) > 0);
    }
    /* dock: clicking an item */
    cui_rect dr = rect_of(ui, a.dock_id);
    click_at(ui, build_motion, &a, dr.x + 8 + 23, dr.y + dr.h - 12 - 23);
    run(ui, build_motion, &a, 20);
    CHECK(a.dock_hits == 1);
    cui_destroy(ui);
}

typedef struct {
    bool thinking, body; char draft[128]; int flags; bool busy; int removed; int model; int vote; int resp;
    int plan_hit, sugg_hit; bool orb, cite; bool picked;
    cui_id think_id, tool_id, plan_id, comp_id, sugg_id, orb_id, cite_id, picker_id;
    const char *files[2]; int file_count;
} ai_ctx;

static const cui_model ai_models[] = { { "Atlas", "Big", NULL, NULL }, { "Atlas Fast", "Quick", NULL, NULL } };

static void build_ai(cui_ctx *ui, void *u) {
    ai_ctx *a = u;
    static const cui_plan_step steps[] = { { "One", NULL, CUI_TASK_DONE }, { "Two", "detail", CUI_TASK_RUNNING }, { "Three", NULL, CUI_TASK_PENDING } };
    static const cui_prompt_suggestion sugg[] = { { CUI_ICON_CODE, "Explain", "code" }, { CUI_ICON_BUG, "Debug", "error" } };
    static const cui_source src = { "Title", "https://www.example.com/a", "Snippet" };
    a->body = false;
    cui_column(ui, .pad = 20, .gap = 10, .width = CUI_PX(600)) {
        cui_reasoning(ui, "think", .thinking = a->thinking, .seconds = 3) { a->body = true; cui_text(ui, "thoughts"); }
        a->think_id = cui_make_id(ui, "think");
        cui_tool_call(ui, "tool", "web_search", .status = CUI_TASK_DONE, .input = "{\"q\": 1}", .output = "{\"r\": 2}");
        a->tool_id = cui_last_id(ui);
        int ph = cui_agent_plan(ui, "plan", steps, 3);
        if (ph >= 0) a->plan_hit = ph;
        a->plan_id = cui_last_id(ui);
        cui_agent_status(ui, "Working", .status = CUI_TASK_RUNNING, .seconds = 75);
        if (cui_citation(ui, 1, &src)) a->cite = true;
        a->cite_id = cui_last_id(ui);
        cui_context_meter(ui, "ctx", 90, 100);
        a->resp |= cui_response_actions(ui, "resp", .copy_text = "x", .vote = &a->vote);
        int sg = cui_prompt_suggestions(ui, "sugg", sugg, 2);
        if (sg >= 0) a->sugg_hit = sg;
        a->sugg_id = cui_last_id(ui);
        if (cui_model_picker(ui, "picker", ai_models, 2, &a->model, .placement = CUI_PLACE_BOTTOM_START)) a->picked = true;
        a->picker_id = cui_last_id(ui);
        a->flags |= cui_chat_composer(ui, "comp", a->draft, sizeof a->draft, .busy = a->busy, .attachments = a->files, .attachment_count = a->file_count, .removed = &a->removed);
        a->comp_id = cui_last_id(ui);
        if (cui_voice_orb(ui, "orb", CUI_VOICE_LISTENING, 0.5f, .size = 80)) a->orb = true;
        a->orb_id = cui_last_id(ui);
    }
}

static void test_ai(void) {
    printf("ai: reasoning folds, tool call opens, plan, citation, actions, suggestions, model picker, composer, orb\n");
    cui_ctx *ui = new_ctx();
    cui_set_viewport(ui, 700, 1600, 1);
    ai_ctx a;
    memset(&a, 0, sizeof a);
    a.thinking = true; a.plan_hit = a.sugg_hit = -1; a.removed = -1;
    a.files[0] = "a.pdf"; a.files[1] = "b.csv"; a.file_count = 2;
    run(ui, build_ai, &a, 3);
    CHECK(a.body);                /* open while thinking */
    a.thinking = false;
    run(ui, build_ai, &a, 3);
    CHECK(!a.body);               /* folds away when done */
    cui_rect hr = rect_of(ui, a.think_id);
    click_at(ui, build_ai, &a, hr.x + 10, hr.y + hr.h / 2);
    run(ui, build_ai, &a, 2);
    CHECK(a.body);                /* and opens on request */
    a.thinking = true; run(ui, build_ai, &a, 2); a.thinking = false; run(ui, build_ai, &a, 2);
    CHECK(a.body);                /* a choice the user made sticks */
    /* tool call opens to show its payloads */
    cui_rect tr = rect_of(ui, a.tool_id);
    float plan_y = rect_of(ui, a.plan_id).y;
    click_at(ui, build_ai, &a, tr.x + tr.w / 2, tr.y + tr.h / 2);
    run(ui, build_ai, &a, 3);
    CHECK(rect_of(ui, a.plan_id).y > plan_y + 100);   /* the payloads pushed the plan down */
    /* plan: clicking a step reports it */
    cui_rect pr = rect_of(ui, a.plan_id);
    click_at(ui, build_ai, &a, pr.x + 80, pr.y + 50 + 38 + 10);
    CHECK(a.plan_hit == 1);
    /* citation */
    cui_rect cr = rect_of(ui, a.cite_id);
    click_at(ui, build_ai, &a, cr.x + cr.w / 2, cr.y + cr.h / 2);
    CHECK(a.cite);
    /* suggestions */
    cui_rect sr = rect_of(ui, a.sugg_id);
    click_at(ui, build_ai, &a, sr.x + sr.w * 0.75f, sr.y + 20);
    CHECK(a.sugg_hit == 1);
    /* model picker: open, choose the second */
    cui_rect mr = rect_of(ui, a.picker_id);
    click_at(ui, build_ai, &a, mr.x + mr.w / 2, mr.y + mr.h / 2);
    run(ui, build_ai, &a, 2);
    key(ui, build_ai, &a, CUI_KEY_DOWN, 0);
    key(ui, build_ai, &a, CUI_KEY_DOWN, 0);
    key(ui, build_ai, &a, CUI_KEY_ENTER, 0);
    CHECK(a.picked && a.model == 1);
    /* composer: type, Shift+Enter adds a line, Enter sends */
    cui_rect qr = rect_of(ui, a.comp_id);
    click_at(ui, build_ai, &a, qr.x + 60, qr.y + 10 + 34 + 4 + 14);
    type(ui, build_ai, &a, "hello");
    CHECK(strcmp(a.draft, "hello") == 0);
    key(ui, build_ai, &a, CUI_KEY_ENTER, CUI_MOD_SHIFT);
    CHECK(strcmp(a.draft, "hello\n") == 0 && !(a.flags & CUI_COMPOSER_SEND));
    type(ui, build_ai, &a, "there");
    key(ui, build_ai, &a, CUI_KEY_ENTER, 0);
    CHECK((a.flags & CUI_COMPOSER_SEND) && strcmp(a.draft, "hello\nthere") == 0);
    /* empty drafts do not send */
    a.flags = 0; a.draft[0] = 0;
    key(ui, build_ai, &a, CUI_KEY_ENTER, 0);
    CHECK(!(a.flags & CUI_COMPOSER_SEND));
    /* busy: the button stops */
    a.busy = true; a.flags = 0;
    run(ui, build_ai, &a, 2);
    qr = rect_of(ui, a.comp_id);
    click_at(ui, build_ai, &a, qr.x + qr.w - 10 - 16, qr.y + qr.h - 10 - 16);
    CHECK(a.flags & CUI_COMPOSER_STOP);
    /* removing an attachment reports which */
    a.flags = 0;
    bool removed = false;
    for (float x = qr.x + 60; x < qr.x + 200 && !removed; x += 3) {
        click_at(ui, build_ai, &a, x, qr.y + 10 + 2 + 18);
        if (a.flags & CUI_COMPOSER_REMOVE) removed = true;
    }
    CHECK(removed && a.removed == 0);
    /* orb */
    cui_rect orr = rect_of(ui, a.orb_id);
    click_at(ui, build_ai, &a, orr.x + orr.w / 2, orr.y + orr.h / 2);
    CHECK(a.orb);
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
    test_textarea_and_clear();
    test_more_actions();
    test_more_inputs();
    test_swipe();
    test_date_math();
    test_pickers();
    test_disclosure();
    test_checklist();
    test_data_grid();
    test_planning();
    test_media();
    test_viz();
    test_motion();
    test_ai();
    test_text_capacity();
    test_dialog();
    test_menu();
    test_focus_trap();
    test_extras();
    test_reorder();
    test_virtual_list();
    test_theme();
    test_renderer();
    test_scroll_fade();
    test_pretty_wrap();
    test_truncated_tooltip();
    test_number_roll();
    test_stress();
    test_atlas_pressure();
    cui_sw_destroy(g_sw);
    printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
