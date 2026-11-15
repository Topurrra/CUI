/* Two windows on one thread, and a dockspace whose panels have headers of their own.
 *
 * The first window docks three panels. A leaf with one panel has no tab strip: the app draws its
 * header (cui_dock_panel_header_begin) and dragging the header's empty space moves the panel, with the
 * usual drop targets. Drop a header onto another and the leaf shows tabs. The second window is a small
 * palette, made on the same thread with the software renderer and placed beside the first. One loop
 * serves both: cui_app_frame_poll steps a window that has something to do, cui_app_wait sleeps for all.
 *
 *     two_windows [--seconds N]     quit after N seconds (for tests) */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_icons.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { bool open[3]; int clicks[3]; } dock_state;
static const char *const panel_title[3] = { "Chat###pane-1", "Notes###pane-2", "Log###pane-3" };
static const char *const panel_name[3] = { "Chat", "Notes", "Log" };

static void draw_dock(cui_ctx *ui, dock_state *s, char *layout, size_t cap) {
    cui_dockspace(ui, "main", .layout = "dock1 h0.5([*pane-1],v0.5([*pane-2],[*pane-3]))", .leaf_header = true) {
        for (int k = 0; k < 3; k++) {
            if (!cui_dock_panel_begin(ui, panel_title[k], &s->open[k])) continue;
            cui_dock_panel_header(ui) {   // false (no body) when the leaf shows a tab strip instead
                cui_row(ui, .width = CUI_FILL, .pad_x = 10, .gap = 8, .align = CUI_ALIGN_CENTER) {
                    cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 14, CUI_COLOR_TEXT_MUTED);
                    cui_text(ui, panel_name[k]);
                    cui_spacer(ui);
                    if (cui_button(ui, "Go", .variant = CUI_BUTTON_GHOST, .size = CUI_SIZE_SM)) s->clicks[k]++;
                }
            }
            cui_column(ui, .pad = 12, .gap = 8, .width = CUI_FILL) {
                cui_textf(ui, "%s: pressed Go %d times", panel_name[k], s->clicks[k]);
                cui_secondary(ui, "Drag the header's empty space to dock this panel somewhere else.");
            }
            cui_dock_panel_end(ui);
        }
    }
    cui_dock_save(ui, "main", layout, cap);
}

static void draw_palette(cui_ctx *ui, int *count, const char *layout) {
    cui_column(ui, .pad = 16, .gap = 10, .width = CUI_FILL) {
        cui_heading(ui, "Palette", 3);
        cui_textf(ui, "Counter: %d", *count);
        if (cui_button(ui, "Count", .variant = CUI_BUTTON_PRIMARY)) (*count)++;
        cui_secondary(ui, "Layout of the first window:");
        cui_code(ui, layout);
    }
}

int main(int argc, char **argv) {
    double seconds = 0;
    for (int i = 1; i + 1 < argc; i++) if (!strcmp(argv[i], "--seconds")) seconds = atof(argv[i + 1]);
    cui_app *a = cui_app_create(&(cui_app_desc){ .title = "Two windows: dock", .width = 760, .height = 520 });
    if (!a) return 1;
    int ax = 100, ay = 100;
    cui_app_position(a, &ax, &ay);
    /* the second window is created in place, no flash at the default spot; software, as a second window on the thread is */
    cui_app *b = cui_app_create(&(cui_app_desc){ .title = "Two windows: palette", .width = 360, .height = 300,
                                                 .x = ax + 780, .y = ay, .has_pos = true });
    if (!b) { cui_app_destroy(a); return 1; }
    cui_app *apps[2] = { a, b };
    dock_state ds = { { true, true, true }, { 0 } };
    char layout[256] = "";
    int count = 0;
    time_t start = time(NULL);
    cui_app_frame_poll(a); cui_app_frame_poll(b);   /* the first frames */
    bool live[2] = { true, true };
    bool first[2] = { true, true };
    for (;;) {
        for (int i = 0; i < 2; i++) {
            if (!apps[i]) continue;
            if (!first[i]) {
                if (!cui_app_pending(apps[i])) continue;
                if (!cui_app_frame_poll(apps[i])) { cui_app_destroy(apps[i]); apps[i] = NULL; live[i] = false; continue; }
            }
            first[i] = false;
            cui_ctx *ui = cui_app_ui(apps[i]);
            if (i == 0) draw_dock(ui, &ds, layout, sizeof layout);
            else draw_palette(ui, &count, layout);
        }
        if (!live[0] || !live[1]) break;   /* either window closed: done */
        if (seconds > 0) {
            if (difftime(time(NULL), start) > seconds) break;
            cui_request_wakeup(cui_app_ui(a), 0.5f);   /* the test run looks at the clock now and then */
        }
        cui_app_wait(apps, 2);
    }
    for (int i = 0; i < 2; i++) if (apps[i]) cui_app_destroy(apps[i]);
    return 0;
}
