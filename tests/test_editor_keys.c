/* test_editor_keys.c - the code editor's own commands rebound through cui_code_editor_opts.keys: without it Ctrl+Z
 * undoes and Ctrl+A selects all as always; with undo on Alt+U and select all on Ctrl+Shift+L, Ctrl+Z and Ctrl+A do
 * nothing and the new keys do; cui_editor_keys_default gives the usual keys, which behave as no table at all.
 * Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static cui_sw_renderer *g_sw;

typedef struct {
    char buf[256];
    const cui_editor_keys *keys;
    cui_code_editor_result r;
} ed;

static void build(cui_ctx *ui, void *u) {
    ed *c = u;
    cui_column(ui, .pad = 20) {
        c->r = cui_code_editor(ui, "ed", c->buf, sizeof c->buf, .lang = "c", .height = 200, .width = CUI_PX(400),
                               .autofocus = true, .keys = c->keys);
    }
}

static void run(cui_ctx *ui, ed *c, int frames) {
    for (int i = 0; i < frames; i++) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui, c); cui_end_frame(ui); }
}

static void key(cui_ctx *ui, ed *c, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods);
    run(ui, c, 1);
    cui_input_key(ui, k, false, mods);
    run(ui, c, 1);
}

static void type(cui_ctx *ui, ed *c, const char *s) {
    cui_input_text(ui, s);
    run(ui, c, 2);
}

/* undo: "keys" says which combination should undo; the other one must not */
static void one(const cui_editor_keys *keys, cui_key undo_key, int undo_mods, cui_key other_key, int other_mods,
                cui_key all_key, int all_mods, cui_key other_all, int other_all_mods) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 600, 300, 1);
    cui_set_reduced_motion(ui, true);
    ed c;
    memset(&c, 0, sizeof c);
    c.keys = keys;
    snprintf(c.buf, sizeof c.buf, "int a;");
    run(ui, &c, 3);
    cui_code_editor_select(ui, "ed", 6, 6);
    run(ui, &c, 2);
    type(ui, &c, "x");
    CHECK(!strcmp(c.buf, "int a;x"));
    if (other_key) {   /* the old key: nothing */
        key(ui, &c, other_key, other_mods);
        CHECK(!strcmp(c.buf, "int a;x"));
    }
    key(ui, &c, undo_key, undo_mods);
    CHECK(!strcmp(c.buf, "int a;"));
    if (other_all) {
        key(ui, &c, other_all, other_all_mods);
        CHECK(c.r.anchor == c.r.cursor);
    }
    key(ui, &c, all_key, all_mods);
    CHECK(c.r.anchor == 0 && c.r.cursor == 6);
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    int P = CUI_MOD_PRIMARY;
    /* no table: the usual keys */
    one(NULL, CUI_KEY_Z, P, CUI_KEY_NONE, 0, CUI_KEY_A, P, CUI_KEY_NONE, 0);
    /* the defaults as a table: the same */
    cui_editor_keys k;
    cui_editor_keys_default(&k);
    CHECK(k.combo[CUI_EDK_UNDO][0].key == CUI_KEY_Z && k.combo[CUI_EDK_REDO][1].mods == (P | CUI_MOD_SHIFT));
    one(&k, CUI_KEY_Z, P, CUI_KEY_NONE, 0, CUI_KEY_A, P, CUI_KEY_NONE, 0);
    /* rebound: undo on Alt+U, select all on Ctrl+Shift+L; the old keys do nothing */
    k.combo[CUI_EDK_UNDO][0] = (cui_key_combo){ CUI_MOD_ALT, CUI_KEY_U };
    k.combo[CUI_EDK_SELECT_ALL][0] = (cui_key_combo){ P | CUI_MOD_SHIFT, CUI_KEY_L };
    one(&k, CUI_KEY_U, CUI_MOD_ALT, CUI_KEY_Z, P, CUI_KEY_L, P | CUI_MOD_SHIFT, CUI_KEY_A, P);
    cui_sw_destroy(g_sw);
    printf("test_editor_keys: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
