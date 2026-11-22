/* test_input_peek.c - cui_key_presses, cui_typed_text and cui_typed_text_consume: what a frame's components will read,
 * seen by the app before them (a keyboard macro recorder), and typed text taken away before a code editor sees it.
 * Headless: the software renderer. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char g_buf[256];
static int g_presses, g_len;
static char g_seen[64];
static bool g_consume;

static void frame(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    g_presses = cui_key_presses(ui, CUI_KEY_LEFT);
    const char *t = cui_typed_text(ui, &g_len);
    snprintf(g_seen, sizeof g_seen, "%s", t ? t : "(null)");
    if (g_consume) cui_typed_text_consume(ui);
    cui_column(ui, .pad = 10) {
        cui_code_editor(ui, "ed", g_buf, sizeof g_buf, .lang = "text", .height = 120, .width = CUI_PX(300), .autofocus = true);
    }
    cui_end_frame(ui);
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 400, 200, 1);
    cui_set_reduced_motion(ui, true);
    frame(ui);
    frame(ui);

    /* nothing pressed, nothing typed */
    CHECK(g_presses == 0 && g_len == 0 && !strcmp(g_seen, ""));
    CHECK(cui_key_presses(ui, CUI_KEY_NONE) == 0 && cui_key_presses(ui, CUI_KEY_COUNT) == 0 && cui_key_presses(ui, (cui_key)-3) == 0);

    /* typed text reaches the app and then the editor */
    cui_input_text(ui, "abc");
    frame(ui);
    CHECK(g_len == 3 && !strcmp(g_seen, "abc"));
    CHECK(!strcmp(g_buf, "abc"));
    frame(ui);
    CHECK(g_len == 0);                          /* gone with the frame */

    /* three presses of one key in one frame (key repeat) are counted, and the editor moves three times */
    for (int i = 0; i < 3; i++) { cui_input_key(ui, CUI_KEY_LEFT, true, 0); cui_input_key(ui, CUI_KEY_LEFT, false, 0); }
    frame(ui);
    CHECK(g_presses == 3);
    cui_input_text(ui, "X");
    frame(ui);
    CHECK(!strcmp(g_buf, "Xabc"));

    /* consumed text never reaches the editor; control characters were never text */
    g_consume = true;
    cui_input_text(ui, "zz\t\x01");
    frame(ui);
    CHECK(g_len == 2 && !strcmp(g_seen, "zz"));
    CHECK(!strcmp(g_buf, "Xabc"));
    g_consume = false;
    frame(ui);
    CHECK(g_len == 0);

    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("input_peek: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
