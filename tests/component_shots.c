/* Renders small focused scenes of each component for the documentation.
 *   cui_component_shots <out-dir> */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "icon_list.h"

typedef void (*scene_fn)(cui_ctx *ui, int frame);

static bool b_on = true, b_off = false, b_check = true, b_check2 = false;
static float f_vol = 62, f_lo = 25, f_hi = 70;
static int i_seg = 1, i_tab = 0, i_radio = 1, i_rating = 4, i_sel = 2, i_page = 2;
static char s_name[64] = "Ada Lovelace", s_email[64] = "", s_pw[32] = "secret", s_search[32] = "", s_bio[256] = "Writing software that feels calm.";
static double d_qty = 4;
static cui_date d_date = { 2026, 11, 14 };
static bool open_true = true;

static void frame_wrap(cui_ctx *ui, void (*body)(cui_ctx *ui)) {
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .gap = 16, .align = CUI_ALIGN_START, .justify = CUI_JUSTIFY_CENTER) {
        body(ui);
    }
}

#define SCENE(name) static void body_##name(cui_ctx *ui); static void scene_##name(cui_ctx *ui, int f) { if (f < 0) return; frame_wrap(ui, body_##name); } static void body_##name(cui_ctx *ui)

SCENE(buttons) {
    cui_row(ui, .gap = 8) {
        cui_button(ui, "Primary", .variant = CUI_BUTTON_PRIMARY, .icon = CUI_ICON_PLUS);
        cui_button(ui, "Secondary");
        cui_button(ui, "Outline", .variant = CUI_BUTTON_OUTLINE);
        cui_button(ui, "Ghost", .variant = CUI_BUTTON_GHOST);
        cui_button(ui, "Danger", .variant = CUI_BUTTON_DANGER);
        cui_button(ui, "Saving", .variant = CUI_BUTTON_PRIMARY, .loading = true);
        cui_icon_button(ui, CUI_ICON_SETTINGS, .tooltip = "Settings");
    }
}
SCENE(segmented) {
    const char *items[] = { "List", "Board", "Timeline" };
    const char *icons[] = { CUI_ICON_LIST, CUI_ICON_LAYOUT_DASHBOARD, CUI_ICON_HISTORY };
    cui_segmented(ui, "s", items, 3, &i_seg, .icons = icons);
}
SCENE(toggles) {
    cui_row(ui, .gap = 40, .align = CUI_ALIGN_START) {
        cui_column(ui, .gap = 14) {
            cui_checkbox(ui, "Email notifications", &b_check, .description = "Product news, monthly.");
            cui_checkbox(ui, "Marketing", &b_check2);
        }
        cui_column(ui, .gap = 14) {
            cui_switch(ui, "Wi-Fi", &b_on);
            cui_switch(ui, "Bluetooth", &b_off);
        }
        cui_column(ui, .gap = 14) {
            cui_radio(ui, "Monthly", &i_radio, 0);
            cui_radio(ui, "Yearly", &i_radio, 1);
        }
    }
}
SCENE(slider) {
    cui_column(ui, .gap = 20, .width = CUI_PX(420)) {
        cui_slider(ui, "Volume", &f_vol, 0, 100, .show_value = true);
        cui_range_slider(ui, "Price", &f_lo, &f_hi, 0, 100);
    }
}
SCENE(inputs) {
    cui_box(ui, .dir = CUI_GRID, .columns = 2, .gap = 16, .width = CUI_PX(620)) {
        cui_input(ui, "name", s_name, sizeof s_name, .label = "Name");
        cui_input(ui, "email", s_email, sizeof s_email, .label = "Email", .icon = CUI_ICON_MAIL, .placeholder = "you@company.com", .hint = "We never share it.");
        cui_input(ui, "pw", s_pw, sizeof s_pw, .label = "Password", .password = true, .error = "Use 8 or more characters.");
        cui_search(ui, "q", s_search, sizeof s_search, .label = "Search");
    }
}
SCENE(textarea) {
    cui_column(ui, .width = CUI_PX(480)) { cui_textarea(ui, "bio", s_bio, sizeof s_bio, .label = "Bio", .height = 90, .hint = "Shown on your profile."); }
}
SCENE(pickers) {
    static const char *items[] = { "Canada", "Denmark", "Germany", "Japan" };
    cui_row(ui, .gap = 16, .align = CUI_ALIGN_END) {
        cui_select(ui, "country", items, 4, &i_sel, .label = "Country", .width = CUI_PX(200));
        cui_date_picker(ui, "date", &d_date, .label = "Due date", .width = CUI_PX(200));
        cui_number(ui, "qty", &d_qty, 0, 10, .label = "Seats", .width = CUI_PX(160));
    }
}
static void scene_select_open(cui_ctx *ui, int f) {
    static const char *items[] = { "Canada", "Denmark", "Germany", "Japan", "Kenya" };
    if (f < 0) {
        f = -f - 1;
        if (f == 3) cui_input_mouse_move(ui, 60, 75);
        if (f == 4) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        if (f == 5) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        if (f == 10) cui_input_mouse_move(ui, 80, 175);
        return;
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .align = CUI_ALIGN_START) {
        cui_select(ui, "open", items, 5, &i_sel, .label = "Country", .width = CUI_PX(240));
    }
}
static void scene_calendar(cui_ctx *ui, int f) {
    if (f < 0) return;
    (void)f;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .align = CUI_ALIGN_START) {
        cui_card(ui, .pad = 16) { cui_calendar(ui, "cal", &d_date); }
    }
}
SCENE(tabs) {
    const char *t[] = { "Overview", "Analytics", "Reports", "Settings" };
    cui_column(ui, .width = CUI_PX(520), .gap = 20) {
        cui_tabs(ui, "tabs", t, 4, &i_tab);
        const char *t2[] = { "Account", "Password", "Billing" };
        const char *ic[] = { CUI_ICON_USER, CUI_ICON_LOCK, CUI_ICON_CREDIT_CARD };
        int k = 0;
        cui_tabs(ui, "pill", t2, 3, &k, .variant = CUI_TABS_PILL, .icons = ic);
    }
}
SCENE(navigation) {
    cui_column(ui, .gap = 18) {
        const char *c[] = { "Home", "Projects", "CUI" };
        cui_breadcrumb(ui, c, 3);
        cui_pagination(ui, "p", &i_page, 10);
        const char *steps[] = { "Account", "Workspace", "Invite", "Done" };
        cui_box(ui, .width = CUI_PX(560)) { cui_stepper(ui, steps, 4, 1); }
    }
}
static void scene_sidebar(cui_ctx *ui, int f) {
    if (f < 0) return;
    (void)f;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED, .pad = 12, .gap = 2) {
        cui_nav_section(ui, "Workspace");
        cui_nav_item(ui, "Inbox", .icon = CUI_ICON_INBOX, .badge = "12", .selected = true);
        cui_nav_item(ui, "Projects", .icon = CUI_ICON_FOLDER);
        cui_nav_item(ui, "Calendar", .icon = CUI_ICON_CALENDAR);
        cui_nav_item(ui, "Settings", .icon = CUI_ICON_SETTINGS);
    }
}
static void scene_menu(cui_ctx *ui, int f) {
    if (f < 0) {
        f = -f - 1;
        if (f == 3) cui_input_mouse_move(ui, 60, 46);
        if (f == 4) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        if (f == 5) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        if (f == 10) cui_input_mouse_move(ui, 80, 130);
        return;
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .align = CUI_ALIGN_START) {
        cui_menu(ui, "Options", .icon = CUI_ICON_SETTINGS) {
            cui_menu_label(ui, "Project");
            cui_menu_item(ui, "Rename", .icon = CUI_ICON_PENCIL, .shortcut = "F2");
            cui_menu_item(ui, "Duplicate", .icon = CUI_ICON_COPY, .shortcut = "Ctrl D");
            cui_menu_separator(ui);
            cui_menu_item(ui, "Delete", .icon = CUI_ICON_TRASH_2, .danger = true);
        }
    }
}
static void scene_dialog(cui_ctx *ui, int f) {
    if (f < 0) return;
    (void)f;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .gap = 10) {
        cui_heading(ui, "Projects", 3);
        cui_skeleton(ui, CUI_PCT(80), 14); cui_skeleton(ui, CUI_PCT(60), 14); cui_skeleton(ui, CUI_PCT(70), 14);
    }
    cui_dialog(ui, "Delete project?", &open_true, .description = "This removes the project for everyone. You cannot undo this.", .width = 380) {
        cui_dialog_footer(ui) { cui_button(ui, "Cancel"); cui_button(ui, "Delete", .variant = CUI_BUTTON_DANGER); }
    }
}
static void scene_toast(cui_ctx *ui, int f) {
    if (f < 0) return;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {}
    if (f == 1) cui_toast(ui, "Upload failed", .description = "The file is larger than 25 MB.", .tone = CUI_TONE_DANGER);
    if (f == 6) cui_toast(ui, "Changes saved", .description = "Your profile is up to date.", .tone = CUI_TONE_SUCCESS);
}
static void scene_tooltip(cui_ctx *ui, int f) {
    if (f < 0) { if (f == -3) cui_input_mouse_move(ui, 46, 102); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .justify = CUI_JUSTIFY_END) {
        cui_row(ui, .gap = 4) {
            cui_icon_button(ui, CUI_ICON_BOLD, .tooltip = "Bold", .shortcut = "Ctrl B");
            cui_icon_button(ui, CUI_ICON_ITALIC, .tooltip = "Italic");
            cui_icon_button(ui, CUI_ICON_LINK, .tooltip = "Insert link");
        }
    }
}
SCENE(alerts) {
    cui_column(ui, .gap = 10, .width = CUI_PX(560)) {
        cui_alert(ui, "Payment received", .description = "Your invoice was paid on November 5.", .tone = CUI_TONE_SUCCESS);
        cui_alert(ui, "Storage almost full", .description = "You have used 92% of your plan.", .tone = CUI_TONE_WARNING, .dismissible = true);
    }
}
SCENE(feedback) {
    cui_column(ui, .gap = 18, .width = CUI_PX(460)) {
        cui_progress(ui, 0.68f, .label = "Uploading", .show_value = true);
        cui_meter(ui, "Storage", 18.4f, 20, "GB");
        cui_row(ui, .gap = 14) { cui_spinner(ui, 18); cui_spinner(ui, 24); cui_skeleton(ui, CUI_PX(180), 12); }
    }
}
SCENE(badges) {
    cui_column(ui, .gap = 16) {
        cui_row(ui, .gap = 8) {
            cui_badge(ui, "Draft");
            cui_badge(ui, "New", .tone = CUI_TONE_ACCENT);
            cui_badge(ui, "Active", .tone = CUI_TONE_SUCCESS, .dot = true);
            cui_badge(ui, "Pending", .tone = CUI_TONE_WARNING, .dot = true);
            cui_badge(ui, "Failed", .tone = CUI_TONE_DANGER, .dot = true);
            cui_badge(ui, "Beta", .tone = CUI_TONE_INFO, .solid = true);
        }
        cui_row(ui, .gap = 14) {
            cui_avatar(ui, "Grace Hopper", .size = 40, .status = "online");
            cui_avatar(ui, "Alan Turing", .size = 40);
            static const char *team[] = { "Ken T", "Barbara L", "Donald K", "Frances A", "John B", "Edsger D" };
            cui_avatar_group(ui, team, 6, 4, 32);
            cui_kbd(ui, "Ctrl Shift P");
        }
    }
}
SCENE(cards) {
    cui_box(ui, .dir = CUI_GRID, .columns = 2, .gap = 16, .width = CUI_PX(640)) {
        static const float s1[] = { 3, 5, 4, 7, 6, 9, 11 };
        cui_metric(ui, "Revenue", "$48,210", .delta = 12.4f, .spark = s1, .spark_count = 7, .icon = CUI_ICON_DOLLAR_SIGN);
        cui_card(ui, .gap = 8) {
            cui_label(ui, "Card");
            cui_secondary(ui, "A bordered surface for a real boundary. Rests on a hairline, no shadow.");
        }
    }
}
SCENE(empty) {
    cui_card(ui, .pad = 0, .width = CUI_PX(520)) {
        cui_empty_state(ui, "No projects yet", .icon = CUI_ICON_FOLDER_OPEN, .description = "Create a project to get started.", .action = "New project");
    }
}
SCENE(table) {
    cui_table_column cols[] = { { "Name", CUI_FILL, CUI_TEXT_LEFT, true }, { "Role", CUI_PX(100), CUI_TEXT_LEFT, false },
                                { "Status", CUI_PX(110), CUI_TEXT_LEFT, false }, { "Amount", CUI_PX(100), CUI_TEXT_RIGHT, false } };
    static cui_table_sort sort = { 0, false };
    static const char *names[] = { "Grace Hopper", "Alan Turing", "Hedy Lamarr" };
    static const char *roles[] = { "Owner", "Admin", "Editor" };
    static const char *st[] = { "Active", "Active", "Invited" };
    static const char *am[] = { "$12,400", "$9,850", "$1,975" };
    cui_box(ui, .width = CUI_PX(620)) {
        cui_table(ui, "t", cols, 4, .sort = &sort, .selectable = true) {
            for (int i = 0; i < 3; i++) {
                cui_table_row(ui, i == 1);
                cui_table_cell(ui); cui_avatar(ui, names[i], .size = 24); cui_text(ui, names[i], .font = CUI_FONT_MEDIUM, .nowrap = true);
                cui_table_cell_text(ui, roles[i]);
                cui_table_cell(ui); cui_badge(ui, st[i], .tone = i == 2 ? CUI_TONE_WARNING : CUI_TONE_SUCCESS, .dot = true);
                cui_table_cell_text(ui, am[i]);
            }
        }
    }
}
SCENE(tree) {
    static bool a = true, b = true, c = false;
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START) {
        cui_card(ui, .pad = 10, .gap = 1, .width = CUI_PX(260)) {
            if (cui_tree_begin(ui, "src", &a, NULL)) {
                if (cui_tree_begin(ui, "widgets", &b, NULL)) {
                    cui_tree_opts o = { 0 }; o.leaf = true;
                    cui_tree_begin(ui, "button.c", NULL, &o);
                    o.selected = true;
                    cui_tree_begin(ui, "slider.c", NULL, &o);
                    cui_tree_end(ui);
                }
                if (cui_tree_begin(ui, "assets", &c, NULL)) cui_tree_end(ui);
                cui_tree_end(ui);
            }
        }
        cui_card(ui, .pad = 8, .gap = 2, .width = CUI_PX(320)) {
            cui_list_item(ui, "Design review", .avatar = "Grace Hopper", .subtitle = "Tomorrow at 10:00", .trailing = "45m");
            cui_list_item(ui, "Release 1.0", .avatar = "Alan Turing", .subtitle = "Ship it", .trailing = "2h", .selected = true);
        }
    }
}
SCENE(accordion) {
    static bool a = true, b = false;
    cui_card(ui, .pad_y = 4, .gap = 0, .width = CUI_PX(520)) {
        if (cui_accordion_begin(ui, "Is it dependency free?", &a)) { cui_secondary(ui, "Yes. Plain C99, native windowing, embedded fonts."); cui_accordion_end(ui); }
        if (cui_accordion_begin(ui, "Can I bring my own renderer?", &b)) { cui_secondary(ui, "Yes."); cui_accordion_end(ui); }
    }
}
SCENE(chips) {
    static bool c[4] = { true, false, true, false };
    cui_column(ui, .gap = 14) {
        cui_row(ui, .gap = 8) {
            const char *n[] = { "Design", "Engineering", "Marketing", "Sales" };
            for (int i = 0; i < 4; i++) cui_chip(ui, n[i], &c[i]);
        }
        cui_rating(ui, "r", &i_rating, 5);
    }
}
SCENE(line_chart) {
    static const float a[] = { 18, 22, 21, 27, 31, 29, 36, 41, 39, 46, 52, 58 }, b[] = { 12, 13, 15, 14, 18, 19, 21, 22, 24, 25, 27, 29 };
    static const char *m[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    cui_series s[2] = { { "Revenue", a, 12, { 0 } }, { "Costs", b, 12, { 0 } } };
    cui_card(ui, .width = CUI_PX(680)) { cui_line_chart(ui, "l", s, 2, .labels = m, .label_count = 12, .area = true, .smooth = true, .height = 200, .format = "$%.0fk"); }
}
SCENE(bar_chart) {
    static const float a[] = { 42, 51, 38, 64, 58, 71 }, b[] = { 28, 33, 30, 41, 46, 49 };
    static const char *m[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun" };
    cui_series s[2] = { { "Desktop", a, 6, { 0 } }, { "Mobile", b, 6, { 0 } } };
    cui_row(ui, .gap = 16, .align = CUI_ALIGN_STRETCH) {
        cui_card(ui, .width = CUI_PX(380)) { cui_bar_chart(ui, "b", s, 2, .labels = m, .label_count = 6, .height = 180); }
        cui_card(ui, .align = CUI_ALIGN_CENTER) {
            static const float v[] = { 46, 27, 17, 10 };
            static const char *l[] = { "Windows", "macOS", "Linux", "Other" };
            cui_donut_chart(ui, "d", v, l, 4, .height = 150, .format = "%.0f%%");
        }
    }
}
static void scene_palette(cui_ctx *ui, int f) {
    if (f < 0) return;
    static bool open = true;
    (void)f;
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {}
    static const cui_command cmds[] = {
        { "New project", CUI_ICON_PLUS, "Actions", "Ctrl N" }, { "Invite teammate", CUI_ICON_USER_PLUS, "Actions", NULL },
        { "Toggle dark mode", CUI_ICON_MOON, "Preferences", "Ctrl J" }, { "Open settings", CUI_ICON_SETTINGS, "Preferences", NULL },
    };
    cui_command_palette(ui, &open, cmds, 4);
}

static void scene_icons(cui_ctx *ui, int f) {
    if (f < 0) return;
    int n = (int)(sizeof icon_list / sizeof icon_list[0]);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 24) {
        cui_box(ui, .dir = CUI_GRID, .columns = 6, .gap = 4) {
            for (int i = 0; i < n; i++) {
                cui_row(ui, .gap = 10, .pad = 8, .radius = 10) {
                    cui_icon(ui, icon_list[i].icon, 18, CUI_COLOR_FOREGROUND);
                    cui_text(ui, icon_list[i].name, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                }
            }
        }
    }
}

typedef struct { const char *name; scene_fn fn; float w, h; bool dark_too; } scene;

static void render(const char *dir, const scene *s, bool dark, cui_sw_renderer *sw) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, s->w, s->h, 2);
    cui_input_mouse_leave(ui);
    for (int f = 0; f < 70; f++) { s->fn(ui, -(f + 1)); cui_begin_frame(ui, 1.0f / 60.0f); s->fn(ui, f); cui_end_frame(ui); }
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    char path[512];
    snprintf(path, sizeof path, "%s/%s%s.png", dir, s->name, dark ? "-dark" : "");
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    printf("wrote %s\n", path);
    free(px);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    static const scene scenes[] = {
        { "buttons", scene_buttons, 760, 110, true }, { "segmented", scene_segmented, 420, 110, true },
        { "toggles", scene_toggles, 680, 150, true }, { "slider", scene_slider, 480, 170, true },
        { "inputs", scene_inputs, 680, 230, true }, { "textarea", scene_textarea, 540, 200, false },
        { "pickers", scene_pickers, 680, 130, false }, { "select", scene_select_open, 320, 330, true },
        { "calendar", scene_calendar, 340, 360, true }, { "tabs", scene_tabs, 580, 160, true },
        { "navigation", scene_navigation, 620, 200, false }, { "sidebar", scene_sidebar, 260, 210, true },
        { "menu", scene_menu, 300, 290, true }, { "dialog", scene_dialog, 620, 340, true },
        { "toast", scene_toast, 420, 220, true }, { "tooltip", scene_tooltip, 260, 150, false },
        { "alerts", scene_alerts, 620, 210, true }, { "feedback", scene_feedback, 520, 220, false },
        { "badges", scene_badges, 600, 160, true }, { "cards", scene_cards, 700, 230, true },
        { "empty-state", scene_empty, 580, 300, false }, { "table", scene_table, 680, 250, true },
        { "tree-list", scene_tree, 660, 230, false }, { "accordion", scene_accordion, 580, 200, false },
        { "chips", scene_chips, 520, 140, false }, { "line-chart", scene_line_chart, 740, 300, true },
        { "bar-donut", scene_bar_chart, 760, 290, true }, { "command-palette", scene_palette, 680, 460, true },
        { "icons", scene_icons, 1040, 1080, false },
    };
    cui_sw_renderer *sw = cui_sw_create();
    for (size_t i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        render(dir, &scenes[i], false, sw);
        if (scenes[i].dark_too) render(dir, &scenes[i], true, sw);
    }
    cui_sw_destroy(sw);
    return 0;
}
