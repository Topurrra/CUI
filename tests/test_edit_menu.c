/* test_edit_menu.c - the standard edit items (cui_edit_items, cui_edit_command, cui_input_use_edit_menu and the code
 * editor's .edit_menu): which items are disabled for a text input, a code editor and a markdown view; what each
 * command does to them, undo included (a paste is one undo step); the menu CUI opens on a right click, only when asked
 * for, with the caret moved to the click unless it lands in the selection. Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;
static char g_clip[256];
static void clip_set(void *u, const char *s) { (void)u; snprintf(g_clip, sizeof g_clip, "%s", s); }
static const char *clip_get(void *u) { (void)u; return g_clip; }

typedef struct {
    char in[64];               /* the text input */
    char ed[128];              /* the code editor */
    const char *md;            /* the markdown view (NULL: none) */
    bool in_ro, ed_ro, ed_menu, no_perform, copy_only;
    bool open, close;          /* this frame: open the app's menu "m" at (px, py), or close it */
    float px, py;
    int chosen;                /* the last item cui_edit_items returned */
    int cmd;                   /* this frame: cui_edit_command(cmd) from code */
    bool cmd_ok;
    bool sel; int sel_a, sel_c;   /* this frame: select in the editor */
    cui_code_editor_result r;
    int ctx_reports;           /* context_clicked on the editor's text */
} T;

static void build(cui_ctx *ui, void *u) {
    T *t = u;
    if (t->cmd) { t->cmd_ok = cui_edit_command(ui, (cui_edit_cmd)t->cmd); t->cmd = 0; }
    if (t->sel) { cui_code_editor_select(ui, "ed", t->sel_a, t->sel_c); t->sel = false; }
    cui_column(ui, .pad = 20, .gap = 12) {
        cui_input(ui, "field", t->in, sizeof t->in, .read_only = t->in_ro, .width = CUI_PX(300));
        t->r = cui_code_editor(ui, "ed", t->ed, sizeof t->ed, .lang = "text", .height = 120, .width = CUI_PX(400),
                               .read_only = t->ed_ro, .edit_menu = t->ed_menu);
        if (t->md) cui_markdown(ui, "md", t->md);
    }
    if (t->r.context_clicked && !t->r.context_gutter) t->ctx_reports++;
    if (t->open) { cui_context_menu_open_at(ui, "m", t->px, t->py); t->open = false; }
    if (t->close) { cui_context_menu_close(ui, "m"); t->close = false; }
    cui_context_menu(ui, "m") {
        cui_edit_cmd c = cui_edit_items(ui, .no_perform = t->no_perform, .copy_only = t->copy_only);
        if (c) t->chosen = c;
    }
}

static void run(cui_ctx *ui, T *t, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, t); cui_end_frame(ui); }
}

static void click(cui_ctx *ui, T *t, cui_mouse_button b, float x, float y, bool menu) {
    cui_input_mouse_move(ui, x, y);
    run(ui, t, 1);
    cui_input_mouse_button(ui, b, true);
    if (menu) { t->open = true; t->px = x; t->py = y; }
    run(ui, t, 1);
    cui_input_mouse_button(ui, b, false);
    run(ui, t, 2);
}

static void key(cui_ctx *ui, T *t, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    run(ui, t, 1);
    cui_input_key(ui, k, false, mods);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);   /* the modifiers let go too */
    run(ui, t, 1);
}

/* the rect of an item of the popup pid (as laid out last frame); w 0 when it is not showing */
static cui_rect item_rect(cui_ctx *ui, cui_id pid, const char *label) {
    cui_push_id_int(ui, (int)pid);
    cui_id id = cui_make_id(ui, label);
    cui_pop_id(ui);
    return cui_get_rect(ui, id);
}
static cui_id menu_m(cui_ctx *ui) { return cui_make_id(ui, "m") ^ 0xC7Cu; }
static cui_id own_menu(cui_ctx *ui, const char *widget) { return cui_make_id(ui, widget) ^ 0xED17E3u; }

/* clicks an item of popup pid; false when it is not showing */
static bool pick(cui_ctx *ui, T *t, cui_id pid, const char *label) {
    cui_rect r = item_rect(ui, pid, label);
    if (r.w <= 0) return false;
    click(ui, t, CUI_MOUSE_LEFT, r.x + r.w * 0.5f, r.y + r.h * 0.5f, false);
    run(ui, t, 2);
    return true;
}

/* the app's menu at (x, y), then a click on label: whether it was chosen (it is when it is enabled) */
static bool enabled(cui_ctx *ui, T *t, float x, float y, const char *label, int cmd) {
    t->chosen = 0;
    click(ui, t, CUI_MOUSE_RIGHT, x, y, true);
    bool shown = pick(ui, t, menu_m(ui), label);
    bool ok = shown && t->chosen == cmd;
    if (!shown) printf("  (item %s not shown)\n", label);
    t->close = true;                 /* a disabled item leaves the menu open (Esc would reach the field too) */
    run(ui, t, 4);
    return ok;
}

/* the app's menu at (x, y) and a click on label, performed */
static void menu(cui_ctx *ui, T *t, float x, float y, const char *label) {
    click(ui, t, CUI_MOUSE_RIGHT, x, y, true);
    CHECK(pick(ui, t, menu_m(ui), label));
    run(ui, t, 3);
}

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 800, 700, 1);
    cui_set_platform(ui, &(cui_platform_iface){ .set_clipboard = clip_set, .get_clipboard = clip_get });
    cui_set_reduced_motion(ui, true);
    return ui;
}

/* the field's middle, its text's start and a point past its text's end */
static void field_points(cui_ctx *ui, float *x_end, float *x_start, float *y) {
    cui_rect fr = cui_get_rect(ui, cui_make_id(ui, "field"));
    *x_end = fr.x + fr.w - 24;
    *x_start = fr.x + 15;
    *y = fr.y + fr.h * 0.5f;
}

static void test_states(void) {
    printf("edit items: disabled when they can't act\n");
    cui_ctx *ui = new_ctx();
    T t;
    memset(&t, 0, sizeof t);
    snprintf(t.in, sizeof t.in, "hello world");
    snprintf(t.ed, sizeof t.ed, "alpha beta\n");
    t.no_perform = true;
    run(ui, &t, 4);
    float fx, fx0, fy;
    field_points(ui, &fx, &fx0, &fy);
    CHECK(fy > 0);
    /* a field with nothing selected and no history: Paste and Select all */
    CHECK(!enabled(ui, &t, fx, fy, "Undo", CUI_EDIT_UNDO));
    CHECK(!enabled(ui, &t, fx, fy, "Redo", CUI_EDIT_REDO));
    CHECK(!enabled(ui, &t, fx, fy, "Cut", CUI_EDIT_CUT));
    CHECK(!enabled(ui, &t, fx, fy, "Copy", CUI_EDIT_COPY));
    CHECK(!enabled(ui, &t, fx, fy, "Delete", CUI_EDIT_DELETE));
    CHECK(enabled(ui, &t, fx, fy, "Paste", CUI_EDIT_PASTE));
    CHECK(enabled(ui, &t, fx, fy, "Select all", CUI_EDIT_SELECT_ALL));
    /* all of it selected: Cut, Copy and Delete too; still nothing to undo */
    click(ui, &t, CUI_MOUSE_LEFT, fx, fy, false);
    key(ui, &t, CUI_KEY_A, CUI_MOD_PRIMARY);
    CHECK(enabled(ui, &t, fx, fy, "Cut", CUI_EDIT_CUT));
    CHECK(enabled(ui, &t, fx, fy, "Copy", CUI_EDIT_COPY));
    CHECK(enabled(ui, &t, fx, fy, "Delete", CUI_EDIT_DELETE));
    CHECK(!enabled(ui, &t, fx, fy, "Undo", CUI_EDIT_UNDO));
    /* typed once over the selection: Undo, not Redo */
    cui_input_text(ui, "!");
    run(ui, &t, 2);
    CHECK(!strcmp(t.in, "!"));
    CHECK(enabled(ui, &t, fx, fy, "Undo", CUI_EDIT_UNDO));
    CHECK(!enabled(ui, &t, fx, fy, "Redo", CUI_EDIT_REDO));
    /* read only, all selected: Copy and Select all only */
    snprintf(t.in, sizeof t.in, "read me");
    t.in_ro = true;
    click(ui, &t, CUI_MOUSE_LEFT, fx, fy, false);
    key(ui, &t, CUI_KEY_A, CUI_MOD_PRIMARY);
    CHECK(enabled(ui, &t, fx, fy, "Copy", CUI_EDIT_COPY));
    CHECK(enabled(ui, &t, fx, fy, "Select all", CUI_EDIT_SELECT_ALL));
    CHECK(!enabled(ui, &t, fx, fy, "Cut", CUI_EDIT_CUT));
    CHECK(!enabled(ui, &t, fx, fy, "Paste", CUI_EDIT_PASTE));
    CHECK(!enabled(ui, &t, fx, fy, "Delete", CUI_EDIT_DELETE));
    CHECK(!enabled(ui, &t, fx, fy, "Undo", CUI_EDIT_UNDO));
    /* a read-only code editor with a selection: the same */
    t.ed_ro = true;
    t.sel = true; t.sel_a = 6; t.sel_c = 10;
    run(ui, &t, 3);
    float ex = t.r.caret_x - 4, ey = t.r.caret_y + t.r.caret_h * 0.5f;   /* on the selected "beta" */
    CHECK(enabled(ui, &t, ex, ey, "Copy", CUI_EDIT_COPY));
    CHECK(enabled(ui, &t, ex, ey, "Select all", CUI_EDIT_SELECT_ALL));
    CHECK(!enabled(ui, &t, ex, ey, "Cut", CUI_EDIT_CUT));
    CHECK(!enabled(ui, &t, ex, ey, "Paste", CUI_EDIT_PASTE));
    CHECK(!enabled(ui, &t, ex, ey, "Delete", CUI_EDIT_DELETE));
    CHECK(!enabled(ui, &t, ex, ey, "Redo", CUI_EDIT_REDO));
    /* editable, nothing selected: Cut, Copy and Delete off, Paste on */
    t.ed_ro = false;
    t.sel = true; t.sel_a = t.sel_c = 3;
    run(ui, &t, 3);
    ex = t.r.caret_x; ey = t.r.caret_y + t.r.caret_h * 0.5f;
    CHECK(!enabled(ui, &t, ex, ey, "Copy", CUI_EDIT_COPY));
    CHECK(!enabled(ui, &t, ex, ey, "Cut", CUI_EDIT_CUT));
    CHECK(enabled(ui, &t, ex, ey, "Paste", CUI_EDIT_PASTE));
    CHECK(t.ctx_reports > 0);   /* an editor without .edit_menu still reports the press for the app's menu */
    cui_destroy(ui);
}

static void test_field_commands(void) {
    printf("edit commands on a text input, undo included\n");
    cui_ctx *ui = new_ctx();
    T t;
    memset(&t, 0, sizeof t);
    snprintf(t.in, sizeof t.in, "hello world");
    snprintf(t.ed, sizeof t.ed, "x\n");
    run(ui, &t, 4);
    float fx, fx0, fy;
    field_points(ui, &fx, &fx0, &fy);
    click(ui, &t, CUI_MOUSE_LEFT, fx, fy, false);   /* the caret at the end */
    key(ui, &t, CUI_KEY_END, 0);
    key(ui, &t, CUI_KEY_LEFT, CUI_MOD_SHIFT | CUI_MOD_CTRL);   /* "world" */
    g_clip[0] = 0;
    menu(ui, &t, fx, fy, "Copy");
    CHECK(!strcmp(g_clip, "world"));
    CHECK(!strcmp(t.in, "hello world"));
    snprintf(g_clip, sizeof g_clip, "?");
    menu(ui, &t, fx, fy, "Cut");
    CHECK(!strcmp(t.in, "hello "));
    CHECK(!strcmp(g_clip, "world"));
    menu(ui, &t, fx, fy, "Undo");
    CHECK(!strcmp(t.in, "hello world"));
    menu(ui, &t, fx, fy, "Redo");
    CHECK(!strcmp(t.in, "hello "));
    snprintf(g_clip, sizeof g_clip, "there");
    menu(ui, &t, fx, fy, "Paste");
    CHECK(!strcmp(t.in, "hello there"));
    menu(ui, &t, fx, fy, "Undo");                 /* the paste was one step */
    CHECK(!strcmp(t.in, "hello "));
    /* from code, no menu: the focused field */
    t.cmd = CUI_EDIT_SELECT_ALL;
    run(ui, &t, 3);
    CHECK(t.cmd_ok);
    t.cmd = CUI_EDIT_DELETE;
    run(ui, &t, 3);
    CHECK(t.in[0] == 0);
    t.cmd = CUI_EDIT_UNDO;
    run(ui, &t, 3);
    CHECK(!strcmp(t.in, "hello "));
    cui_destroy(ui);
}

static void test_editor_commands(void) {
    printf("edit commands on a code editor, undo included\n");
    cui_ctx *ui = new_ctx();
    T t;
    memset(&t, 0, sizeof t);
    snprintf(t.ed, sizeof t.ed, "alpha beta\n");
    run(ui, &t, 4);
    t.sel = true; t.sel_a = 6; t.sel_c = 10;      /* "beta" */
    run(ui, &t, 3);
    float ex = t.r.caret_x - 4, ey = t.r.caret_y + t.r.caret_h * 0.5f;
    g_clip[0] = 0;
    menu(ui, &t, ex, ey, "Copy");
    CHECK(!strcmp(g_clip, "beta"));
    menu(ui, &t, ex, ey, "Cut");
    CHECK(!strcmp(t.ed, "alpha \n"));
    menu(ui, &t, ex, ey, "Undo");
    CHECK(!strcmp(t.ed, "alpha beta\n"));
    menu(ui, &t, ex, ey, "Redo");
    CHECK(!strcmp(t.ed, "alpha \n"));
    t.sel = true; t.sel_a = t.sel_c = 6;
    run(ui, &t, 3);
    snprintf(g_clip, sizeof g_clip, "gamma");
    menu(ui, &t, t.r.caret_x, ey, "Paste");
    CHECK(!strcmp(t.ed, "alpha gamma\n"));
    menu(ui, &t, ex, ey, "Undo");                 /* one step */
    CHECK(!strcmp(t.ed, "alpha \n"));
    t.sel = true; t.sel_a = 0; t.sel_c = 6;
    run(ui, &t, 3);
    menu(ui, &t, t.r.caret_x - 20, ey, "Delete");
    CHECK(!strcmp(t.ed, "\n"));
    menu(ui, &t, ex, ey, "Undo");
    CHECK(!strcmp(t.ed, "alpha \n"));
    menu(ui, &t, ex, ey, "Select all");
    menu(ui, &t, ex, ey, "Copy");
    CHECK(!strcmp(g_clip, "alpha \n"));
    cui_destroy(ui);
}

static void test_own_menu(void) {
    printf("CUI's own menu: only when asked for; the caret goes to the click\n");
    cui_ctx *ui = new_ctx();
    T t;
    memset(&t, 0, sizeof t);
    snprintf(t.in, sizeof t.in, "hello world");
    snprintf(t.ed, sizeof t.ed, "alpha beta\n");
    run(ui, &t, 4);
    float fx, fx0, fy;
    field_points(ui, &fx, &fx0, &fy);
    /* off (the default): a right click opens nothing */
    click(ui, &t, CUI_MOUSE_RIGHT, fx, fy, false);
    run(ui, &t, 2);
    CHECK(item_rect(ui, own_menu(ui, "field"), "Paste").w == 0);
    CHECK(cui_focus_get(ui) != cui_make_id(ui, "field"));
    /* on: it opens, the field takes the keyboard and the caret goes to the click (past the end) */
    cui_input_use_edit_menu(ui, true);
    click(ui, &t, CUI_MOUSE_LEFT, fx, fy, false);
    key(ui, &t, CUI_KEY_HOME, 0);                 /* the caret at the start */
    click(ui, &t, CUI_MOUSE_LEFT, 700, 650, false);   /* the field lets go of the keyboard */
    click(ui, &t, CUI_MOUSE_RIGHT, fx, fy, false);
    run(ui, &t, 2);
    CHECK(item_rect(ui, own_menu(ui, "field"), "Paste").w > 0);
    CHECK(cui_focus_get(ui) == cui_make_id(ui, "field"));
    snprintf(g_clip, sizeof g_clip, "!");
    CHECK(pick(ui, &t, own_menu(ui, "field"), "Paste"));
    run(ui, &t, 2);
    CHECK(!strcmp(t.in, "hello world!"));
    /* a click inside the selection keeps it */
    key(ui, &t, CUI_KEY_A, CUI_MOD_PRIMARY);
    click(ui, &t, CUI_MOUSE_RIGHT, fx0 + 20, fy, false);
    run(ui, &t, 2);
    g_clip[0] = 0;
    CHECK(pick(ui, &t, own_menu(ui, "field"), "Copy"));
    run(ui, &t, 2);
    CHECK(!strcmp(g_clip, "hello world!"));
    /* a code editor without .edit_menu: still only reported to the app */
    t.sel = true; t.sel_a = t.sel_c = 10;
    run(ui, &t, 3);
    float ex = t.r.caret_x, ey = t.r.caret_y + t.r.caret_h * 0.5f;
    int before = t.ctx_reports;
    click(ui, &t, CUI_MOUSE_RIGHT, ex, ey, false);
    run(ui, &t, 2);
    CHECK(t.ctx_reports == before + 1);
    CHECK(item_rect(ui, own_menu(ui, "ed"), "Paste").w == 0);
    /* with .edit_menu: CUI opens it, the caret moves to the click, the text press is not reported */
    t.ed_menu = true;
    t.sel = true; t.sel_a = t.sel_c = 0;
    run(ui, &t, 3);
    click(ui, &t, CUI_MOUSE_RIGHT, ex + 1, ey, false);
    run(ui, &t, 2);
    CHECK(t.ctx_reports == before + 1);
    CHECK(item_rect(ui, own_menu(ui, "ed"), "Paste").w > 0);
    snprintf(g_clip, sizeof g_clip, "Z");
    CHECK(pick(ui, &t, own_menu(ui, "ed"), "Paste"));
    run(ui, &t, 2);
    CHECK(!strcmp(t.ed, "alpha betaZ\n"));
    /* from the keyboard: Down to Paste, Enter; the editor does not take the keys while its menu shows */
    click(ui, &t, CUI_MOUSE_RIGHT, ex + 30, ey, false);
    run(ui, &t, 2);
    for (int i = 0; i < 5; i++) key(ui, &t, CUI_KEY_DOWN, 0);
    snprintf(g_clip, sizeof g_clip, "Q");
    key(ui, &t, CUI_KEY_ENTER, 0);
    run(ui, &t, 3);
    CHECK(!strcmp(t.ed, "alpha betaZQ\n"));
    /* Esc closes it and leaves the selection alone */
    t.sel = true; t.sel_a = 0; t.sel_c = 5;
    run(ui, &t, 3);
    click(ui, &t, CUI_MOUSE_RIGHT, ex - 60, ey, false);   /* on "alpha": inside the selection */
    run(ui, &t, 2);
    key(ui, &t, CUI_KEY_ESCAPE, 0);
    run(ui, &t, 3);
    CHECK(t.r.anchor == 0 && t.r.cursor == 5);
    cui_input_use_edit_menu(ui, false);
    cui_destroy(ui);
}

static void test_markdown(void) {
    printf("edit items on a markdown view: Select all, then Copy\n");
    cui_ctx *ui = new_ctx();
    T t;
    memset(&t, 0, sizeof t);
    snprintf(t.ed, sizeof t.ed, "x\n");
    t.md = "Some **bold** words.\n";
    run(ui, &t, 4);
    cui_rect mr = cui_get_rect(ui, cui_make_id(ui, "md"));
    CHECK(mr.w > 0);
    float mx = mr.x + 20, my = mr.y + mr.h * 0.5f;
    /* .copy_only: Copy and Select all, nothing else (first: a rect is kept once an item was shown) */
    t.copy_only = true;
    click(ui, &t, CUI_MOUSE_RIGHT, mx, my, true);
    run(ui, &t, 2);
    CHECK(item_rect(ui, menu_m(ui), "Copy").w > 0);
    CHECK(item_rect(ui, menu_m(ui), "Select all").w > 0);
    CHECK(item_rect(ui, menu_m(ui), "Paste").w == 0);
    CHECK(item_rect(ui, menu_m(ui), "Undo").w == 0);
    t.close = true;
    t.copy_only = false;
    run(ui, &t, 4);
    t.no_perform = true;
    CHECK(!enabled(ui, &t, mx, my, "Copy", CUI_EDIT_COPY));   /* nothing selected */
    CHECK(!enabled(ui, &t, mx, my, "Paste", CUI_EDIT_PASTE));  /* read only */
    CHECK(!enabled(ui, &t, mx, my, "Cut", CUI_EDIT_CUT));
    CHECK(enabled(ui, &t, mx, my, "Select all", CUI_EDIT_SELECT_ALL));
    t.no_perform = false;
    menu(ui, &t, mx, my, "Select all");
    g_clip[0] = 0;
    menu(ui, &t, mx, my, "Copy");
    CHECK(strstr(g_clip, "Some") && strstr(g_clip, "bold") && strstr(g_clip, "words"));
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_states();
    test_field_commands();
    test_editor_commands();
    test_own_menu();
    test_markdown();
    cui_sw_destroy(g_sw);
    printf("test_edit_menu: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
