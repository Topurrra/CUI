/* test_focus_api.c - keyboard focus by id (cui_focus_get, cui_focus_visible, cui_focus_set): an app moving the focus
 * through hoverable cards itself, as with the arrows in a grid. Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int g_clicked[3];

static void build(cui_ctx *ui) {
    cui_column(ui, .pad = 20, .gap = 8) {
        static const char *const ids[3] = { "c0", "c1", "c2" };
        for (int i = 0; i < 3; i++) {
            if (cui_card_begin(ui, &(cui_card_opts){ .id = ids[i], .hoverable = true, .width = CUI_PX(200) })) g_clicked[i]++;
            cui_text(ui, ids[i]);
            cui_card_end(ui);
        }
    }
}

static void frame(cui_ctx *ui) { cui_begin_frame(ui, 1.0f / 60.0f); build(ui); cui_end_frame(ui); }

static void key(cui_ctx *ui, cui_key k) {
    cui_input_key(ui, k, true, 0);
    frame(ui);
    cui_input_key(ui, k, false, 0);
    frame(ui);
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 400, 400, 1);
    frame(ui);
    frame(ui);
    cui_id c0 = cui_make_id(ui, "c0"), c2 = cui_make_id(ui, "c2");
    CHECK(cui_focus_get(ui) == 0 && !cui_focus_visible(ui, cui_focus_get(ui)));

    key(ui, CUI_KEY_TAB);                       /* Tab: the first card, from the keyboard */
    CHECK(cui_focus_get(ui) == c0);
    CHECK(cui_focus_visible(ui, cui_focus_get(ui)));

    cui_focus_set(ui, c2, true);                /* the app moves it */
    frame(ui);
    CHECK(cui_focus_get(ui) == c2 && cui_focus_visible(ui, cui_focus_get(ui)));
    key(ui, CUI_KEY_ENTER);                     /* Enter clicks the card that has it */
    CHECK(g_clicked[2] == 1 && g_clicked[0] == 0 && g_clicked[1] == 0);

    cui_focus_set(ui, c0, false);               /* as a click gives it: no ring, Enter does nothing */
    frame(ui);
    CHECK(cui_focus_get(ui) == c0 && !cui_focus_visible(ui, cui_focus_get(ui)));

    cui_focus_set(ui, 0, true);                 /* 0 drops it */
    frame(ui);
    CHECK(cui_focus_get(ui) == 0 && !cui_focus_visible(ui, cui_focus_get(ui)));

    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("test_focus_api: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
