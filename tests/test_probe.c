/* The frame probe (cui_set_probe): marks come in the order of a frame's phases, nothing is
 * called when no hook is set, and cui_frame_memory reports the frame arena. Headless. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

typedef struct { int marks[64]; int n; } log_t;
static void hook(void *user, cui_probe_mark m) { log_t *l = user; if (l->n < 64) l->marks[l->n++] = (int)m; }

static void one_frame(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .pad = 8, .width = CUI_FILL, .height = CUI_FILL) { cui_label(ui, "hello"); }
    cui_end_frame(ui);
}

int main(void) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 320, 200, 1);

    log_t l = { { 0 }, 0 };
    one_frame(ui);                                  /* no hook: nothing recorded, nothing crashes */
    CHECK(l.n == 0);

    cui_set_probe(hook, &l);
    one_frame(ui);
    const int want[] = { CUI_PROBE_BEGIN, CUI_PROBE_BUILD, CUI_PROBE_LAYOUT, CUI_PROBE_PAINT, CUI_PROBE_PAINT_END };
    CHECK(l.n == 5);
    for (int i = 0; i < 5 && i < l.n; i++) CHECK(l.marks[i] == want[i]);

    one_frame(ui);                                  /* a second frame appends the same five */
    CHECK(l.n == 10 && l.marks[5] == CUI_PROBE_BEGIN && l.marks[9] == CUI_PROBE_PAINT_END);

    cui_set_probe(NULL, NULL);
    one_frame(ui);
    CHECK(l.n == 10);                               /* removed: silent again */

    size_t used = 0, cap = 0, peak = 0;
    cui_frame_memory(ui, &used, &cap, &peak);
    CHECK(used > 0 && cap >= used && peak >= used);
    cui_frame_memory(ui, NULL, NULL, NULL);         /* every output is optional */

    cui_destroy(ui);
    cui_sw_destroy(sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
