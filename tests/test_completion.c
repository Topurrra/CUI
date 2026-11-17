/* test_completion.c - the code editor's hover, caret box and Ctrl+click results, the completion list (filter and
 * order, keys taken from the editor, clicks, placement) and the signature tip. Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_reduced_motion(ui, true);
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

/* ---- the editor's results ---- */

typedef struct {
    char buf[512];
    cui_code_editor_result r;
    bool hovered_once, ctrl_once;
    int hover_off, ctrl_off;
    /* a completion list over the editor */
    bool list;
    const cui_completion_item *items;
    int n_items;
    const char *filter;
    cui_completion_result c;
    int accepted;
    bool closed;
    bool tip;
} ed_ctx;

static void build(cui_ctx *ui, void *u) {
    ed_ctx *c = u;
    if (c->list) {
        c->c = cui_completion(ui, "complete", c->items, c->n_items, .x = c->r.caret_x, .y = c->r.caret_y, .line_h = c->r.caret_h,
                              .filter = c->filter);
        if (c->c.accepted >= 0) c->accepted = c->c.accepted;
        if (c->c.closed) c->closed = true;
    }
    if (c->tip) cui_signature_tip(ui, "sig", "int printf(const char *fmt, ...)", 11, 26, .x = c->r.caret_x, .y = c->r.caret_y,
                                  .line_h = c->r.caret_h, .doc = "Prints **formatted** text.");
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->buf, sizeof c->buf, .lang = "c", .height = 300, .width = CUI_PX(600));
    }
    if (c->r.hover) { c->hovered_once = true; c->hover_off = c->r.hover_offset; }
    if (c->r.ctrl_click) { c->ctrl_once = true; c->ctrl_off = c->r.ctrl_click_offset; }
}

static int at(const ed_ctx *c, const char *needle) { const char *p = strstr(c->buf, needle); return p ? (int)(p - c->buf) : -1; }

static void test_editor_results(void) {
    printf("editor: hover, caret box, ctrl+click\n");
    cui_ctx *ui = new_ctx();
    ed_ctx c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "int alpha;\nbeta();\n");
    run(ui, build, &c, 3);
    /* the caret box follows the caret */
    int a = at(&c, "alpha"), b = at(&c, "beta");
    cui_code_editor_select(ui, "ed", a, a);
    run(ui, build, &c, 2);
    float ax = c.r.caret_x, ay = c.r.caret_y, h = c.r.caret_h;
    CHECK(h > 8 && ax > 20 && ay > 20);
    cui_code_editor_select(ui, "ed", b, b);
    run(ui, build, &c, 2);
    CHECK(c.r.caret_y > ay + h * 0.5f && c.r.caret_x < ax);     /* the next line, its start */
    float bx = c.r.caret_x, by = c.r.caret_y;

    /* hover: only after the pointer rests half a second on text */
    cui_input_mouse_move(ui, ax + 3, ay + h * 0.5f);
    run(ui, build, &c, 10);
    CHECK(!c.hovered_once);
    run(ui, build, &c, 25);
    CHECK(c.hovered_once && (c.hover_off == a || c.hover_off == a + 1));
    CHECK(c.r.hover && fabsf(c.r.hover_x - (ax + 3)) < 0.5f);
    c.hovered_once = false;
    cui_input_mouse_move(ui, ax + 30, ay + h * 0.5f);              /* moved: the rest starts again */
    run(ui, build, &c, 5);
    CHECK(!c.hovered_once && !c.r.hover);
    run(ui, build, &c, 30);
    CHECK(c.hovered_once && c.hover_off > a && c.hover_off <= a + 6);
    c.hovered_once = false;
    cui_input_mouse_move(ui, ax + 400, ay + h * 0.5f);             /* past the end of the line: no hover */
    run(ui, build, &c, 40);
    CHECK(!c.hovered_once);
    cui_input_mouse_move(ui, 790, 590);                            /* off the editor */
    run(ui, build, &c, 40);
    CHECK(!c.hovered_once);

    /* ctrl+click reports the byte; a plain click does not */
    cui_input_mouse_move(ui, bx + 10, by + h * 0.5f);
    run(ui, build, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build, &c, 1);
    CHECK(!c.ctrl_once);
    cui_input_key(ui, CUI_KEY_NONE, false, CUI_MOD_CTRL);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
    run(ui, build, &c, 1);
    CHECK(c.ctrl_once && c.ctrl_off >= b && c.ctrl_off < b + 4);
    cui_destroy(ui);
}

/* ---- the completion list ---- */

static const cui_completion_item ITEMS[] = {
    { "sprintf", "int (char *, const char *, ...)", NULL, 3 },
    { "pointer", "void *", NULL, 6 },
    { "print_line", "void (const char *)", "Prints a line.", 3 },
    { "PrintMode", "enum", NULL, 13 },
    { "printf", "int (const char *, ...)", "Prints **formatted** text.", 3 },
};
enum { N_ITEMS = (int)(sizeof ITEMS / sizeof ITEMS[0]) };

static void key(cui_ctx *ui, ed_ctx *c, cui_key k) {
    cui_input_key(ui, k, true, 0);
    run(ui, build, c, 1);
    cui_input_key(ui, k, false, 0);
    run(ui, build, c, 1);
}

static void test_completion(void) {
    printf("completion list\n");
    cui_ctx *ui = new_ctx();
    ed_ctx c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "int x;\npr\n");
    run(ui, build, &c, 2);
    int end = (int)strlen("int x;\npr");
    cui_code_editor_select(ui, "ed", end, end);
    run(ui, build, &c, 2);
    c.list = true;
    c.items = ITEMS;
    c.n_items = N_ITEMS;
    c.filter = "pr";
    c.accepted = -1;
    run(ui, build, &c, 2);
    /* prefixes first (the same case before), then the rest that has the letters in order */
    CHECK(c.c.shown == 5 && c.c.selected == 4 && !c.closed);         /* printf */
    key(ui, &c, CUI_KEY_DOWN);
    CHECK(c.c.selected == 2);                                        /* print_line */
    key(ui, &c, CUI_KEY_DOWN);
    CHECK(c.c.selected == 3);                                        /* PrintMode */
    key(ui, &c, CUI_KEY_UP);
    key(ui, &c, CUI_KEY_ENTER);
    CHECK(c.accepted == 2);
    CHECK(!strcmp(c.buf, "int x;\npr\n"));                           /* the editor never saw Enter or the arrows */
    c.accepted = -1;
    key(ui, &c, CUI_KEY_UP);                                         /* print_line -> printf, the top */
    CHECK(c.c.selected == 4);
    key(ui, &c, CUI_KEY_UP);                                         /* from the top: wraps to the bottom */
    key(ui, &c, CUI_KEY_TAB);
    CHECK(c.accepted == 0);                                          /* sprintf: letters in order, last */
    CHECK(!strcmp(c.buf, "int x;\npr\n"));
    /* a click on the third row */
    c.accepted = -1;
    c.filter = "print";                                              /* a new filter: the best match again */
    run(ui, build, &c, 2);
    CHECK(c.c.shown == 4 && c.c.selected == 4);                      /* printf, print_line, PrintMode, sprintf */
    float row_h = 13.0f + 10.0f, y0 = c.r.caret_y + c.r.caret_h + 2.0f + 4.0f;
    cui_input_mouse_move(ui, c.r.caret_x + 40, y0 + row_h * 2.5f);
    run(ui, build, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build, &c, 1);
    CHECK(c.accepted == 3);                                          /* PrintMode */
    /* nothing matches: closed; Escape: closed and the editor does not see it */
    c.filter = "zzz";
    run(ui, build, &c, 1);
    CHECK(c.closed && c.c.shown == 0);
    c.closed = false;
    c.filter = "pr";
    run(ui, build, &c, 1);
    key(ui, &c, CUI_KEY_ESCAPE);
    CHECK(c.closed);
    /* the signature tip draws above the caret */
    c.list = false;
    c.tip = true;
    run(ui, build, &c, 2);
    cui_destroy(ui);
}

/* the list goes above the line when there is no room below */
typedef struct { cui_completion_result r; float y; } low_ctx;
static void build_low(cui_ctx *ui, void *u) {
    low_ctx *c = u;
    c->r = cui_completion(ui, "low", ITEMS, N_ITEMS, .x = 790, .y = c->y, .line_h = 18);
}

static void test_placement(void) {
    printf("completion placement\n");
    cui_ctx *ui = new_ctx();
    low_ctx c = { .y = 580 };
    run(ui, build_low, &c, 2);
    cui_rect top = cui_get_rect(ui, cui_make_id(ui, "low"));
    (void)top;
    /* a click above the line, where the flipped list is, picks a row; below the line there is no room */
    float row_h = 23.0f, h = 5 * row_h + 8.0f, y_list = 580 - h - 2.0f;
    cui_input_mouse_move(ui, 700, y_list + 4 + row_h * 0.5f);
    run(ui, build_low, &c, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    run(ui, build_low, &c, 1);
    CHECK(c.r.accepted == 0);                                        /* the first row: no filter, the server's order */
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    run(ui, build_low, &c, 1);
    cui_destroy(ui);
}

/* --shot <file.png>: the editor with the list and the signature tip open, dark, drawn headless (for a look) */
static void shot(const char *path) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 900, 460, 1);
    cui_set_reduced_motion(ui, true);
    ed_ctx c;
    memset(&c, 0, sizeof c);
    snprintf(c.buf, sizeof c.buf, "#include <stdio.h>\n\nint main(void) {\n    int n = 3;\n    printf(\"%%d\\n\", n);\n    pr\n    return 0;\n}\n");
    run(ui, build, &c, 2);
    int end = (int)(strstr(c.buf, "    pr\n") - c.buf) + 6;
    cui_code_editor_select(ui, "ed", end, end);
    run(ui, build, &c, 2);
    c.list = true;
    c.items = ITEMS;
    c.n_items = N_ITEMS;
    c.filter = "pr";
    c.tip = true;
    run(ui, build, &c, 4);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--shot")) { shot(argv[2]); cui_sw_destroy(g_sw); return 0; }
    test_editor_results();
    test_completion();
    test_placement();
    cui_sw_destroy(g_sw);
    printf("test_completion: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
