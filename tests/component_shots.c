/* Renders small focused scenes of each component for the documentation.
 *   cui_component_shots <out-dir> [scene] */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include <math.h>
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

SCENE(radio_cards) {
    static int sel = 1;
    static const cui_choice plans[] = {
        { "Hobby", "Side projects. Free forever.", CUI_ICON_ROCKET, NULL },
        { "Pro", "For people who ship weekly.", CUI_ICON_ZAP, "Popular" },
        { "Team", "Roles and audit logs.", CUI_ICON_USERS, NULL },
    };
    cui_box(ui, .width = CUI_PX(720)) { cui_radio_cards(ui, "plans", plans, 3, &sel); }
}
SCENE(tag_otp) {
    static char tags[128] = "design, c99, native";
    static char otp[8] = "418";
    static char pw[32] = "correct horse";
    cui_column(ui, .gap = 16, .width = CUI_PX(460)) {
        cui_tag_input(ui, "tags", tags, sizeof tags, .label = "Topics");
        cui_otp_input(ui, "otp", otp, 6);
        cui_column(ui, .gap = 8) { cui_input(ui, "pw", pw, sizeof pw, .label = "Password", .password = true); cui_password_strength(ui, pw); }
    }
}
SCENE(button_extras) {
    cui_row(ui, .gap = 12) {
        cui_button_group(ui) {
            cui_button(ui, "Bold", .icon = CUI_ICON_BOLD);
            cui_button(ui, "Italic", .icon = CUI_ICON_ITALIC);
            cui_button(ui, "Underline", .icon = CUI_ICON_UNDERLINE);
        }
        cui_copy_button(ui, "copy", "text");
        cui_hold_button(ui, "Hold to delete", .icon = CUI_ICON_TRASH_2);
    }
}
SCENE(timeline) {
    cui_row(ui, .gap = 32, .align = CUI_ALIGN_START) {
        cui_column(ui, .gap = 0, .width = CUI_PX(380)) {
            cui_timeline_item(ui, "Deployed to production", .time = "2m ago", .description = "Release 1.0.3 is live.", .icon = CUI_ICON_ROCKET, .tone = CUI_TONE_SUCCESS);
            cui_timeline_item(ui, "Review requested", .time = "1h ago", .icon = CUI_ICON_MESSAGE_SQUARE, .tone = CUI_TONE_INFO);
            cui_timeline_item(ui, "Build failed", .time = "3h ago", .icon = CUI_ICON_CIRCLE_ALERT, .tone = CUI_TONE_DANGER, .last = true);
        }
        cui_progress_ring(ui, "Onboarding", 0.72f, 110);
    }
}

/* ---- extended components ---- */

static void click_id(cui_ctx *ui, cui_id id, int f, int at) {
    cui_rect r = cui_get_rect(ui, id);
    if (f == at) cui_input_mouse_move(ui, r.x + r.w / 2, r.y + r.h / 2);
    if (f == at + 1) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    if (f == at + 2) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
}

SCENE(split_button) {
    static const char *items[] = { "Save as draft", "Save and publish", "Schedule..." };
    cui_row(ui, .gap = 12) {
        cui_split_button(ui, "Save", items, 3, .icon = CUI_ICON_SAVE);
        cui_split_button(ui, "Export", items, 3, .variant = CUI_BUTTON_OUTLINE, .icon = CUI_ICON_DOWNLOAD);
        cui_split_button(ui, "Run", items, 3, .variant = CUI_BUTTON_SECONDARY, .icon = CUI_ICON_PLAY);
    }
}
SCENE(toggle_group) {
    static bool fmt[4] = { true, true, false, false };
    static bool al[3] = { false, true, false };
    static const char *labels[] = { "Bold", "Italic", "Underline", "Code" };
    static const char *icons[] = { CUI_ICON_BOLD, CUI_ICON_ITALIC, CUI_ICON_UNDERLINE, CUI_ICON_CODE };
    static const char *views[] = { "List", "Board", "Calendar" };
    static const char *vicons[] = { CUI_ICON_LIST, CUI_ICON_LAYOUT_DASHBOARD, CUI_ICON_CALENDAR };
    cui_row(ui, .gap = 14) {
        cui_toggle_group(ui, "fmt", labels, 4, fmt, .icons = icons, .icon_only = true);
        cui_toggle_group(ui, "views", views, 3, al, .icons = vicons, .exclusive = true);
    }
}
static int morph_frame;
static void body_morph(cui_ctx *ui) {
    cui_row(ui, .gap = 12) {
        cui_confirm_button(ui, "Delete project", .icon = CUI_ICON_TRASH_2);
        cui_confirm_button(ui, "Remove member", .icon = CUI_ICON_USER, .timeout = 60);
        cui_status_button(ui, "Save changes", CUI_STATUS_BUSY, .icon = CUI_ICON_SAVE);
        cui_status_button(ui, "Publish", CUI_STATUS_SUCCESS, .success_label = "Published");
    }
}
static void scene_morph(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "Remove member"), -f - 1, 20); return; }
    morph_frame = f;
    frame_wrap(ui, body_morph);
}
static void body_user_menu(cui_ctx *ui) {
    cui_box(ui, .width = CUI_PX(270)) {
        cui_user_menu(ui, "Ada Lovelace", .detail = "ada@analytical.engine", .status = "online", .placement = CUI_PLACE_BOTTOM_START) {
            cui_menu_item(ui, "Profile", .icon = CUI_ICON_USER, .shortcut = "Ctrl P");
            cui_menu_item(ui, "Billing", .icon = CUI_ICON_CREDIT_CARD);
            cui_menu_item(ui, "Settings", .icon = CUI_ICON_SETTINGS, .shortcut = "Ctrl ,");
            cui_menu_separator(ui);
            cui_menu_item(ui, "Log out", .icon = CUI_ICON_LOG_OUT);
        }
    }
}
static void scene_user_menu(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "Ada Lovelace") ^ 0x05E4u, -f - 1, 20); if (-f - 1 == 26) cui_input_mouse_move(ui, 600, 20); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_user_menu(ui); }
}
SCENE(inline_edit) {
    static char title[64] = "Q3 roadmap", owner[64] = "Grace Hopper";
    cui_column(ui, .gap = 4) {
        cui_inline_edit(ui, "t", title, sizeof title, .font = CUI_FONT_DISPLAY, .size = 26);
        cui_inline_edit(ui, "o", owner, sizeof owner);
    }
}
SCENE(scrub) {
    static float a = 80, b = 12, c = 24;
    cui_row(ui, .gap = 12) {
        cui_scrub(ui, "Opacity", &a, 0, 100, .suffix = "%");
        cui_scrub(ui, "Radius", &b, 0, 48, .step = 0.5f, .format = "%.1f", .suffix = "px");
        cui_scrub(ui, "Blur", &c, 0, 80, .suffix = "px", .width = CUI_PX(140));
    }
}
SCENE(shortcut_rec) {
    static cui_key_combo a = { CUI_MOD_CTRL | CUI_MOD_SHIFT, CUI_KEY_P }, b = { 0, CUI_KEY_NONE };
    static double money = 1249.5;
    cui_row(ui, .gap = 12, .align = CUI_ALIGN_END) {
        cui_shortcut_recorder(ui, "a", &a);
        cui_shortcut_recorder(ui, "b", &b, .placeholder = "Record shortcut");
        cui_box(ui, .width = CUI_PX(220)) { cui_money_input(ui, "m", &money, .code = "USD"); }
    }
}

static void body_multi(cui_ctx *ui) {
    static bool sel[6] = { true, false, true, false, false, true };
    static const char *items[] = { "C", "C++", "Rust", "Zig", "Go", "Swift" };
    cui_box(ui, .width = CUI_PX(300)) { cui_multi_select(ui, "langs", items, 6, sel, .label = "Languages"); }
}
static void scene_multi(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "langs"), -f - 1, 20); if (-f - 1 == 26) cui_input_mouse_move(ui, 600, 10); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_multi(ui); }
}
static void body_range(cui_ctx *ui) {
    static cui_date_range r = { { 2026, 11, 3 }, { 2026, 11, 14 } };
    cui_date_range_picker(ui, "range", &r, .width = CUI_PX(300));
}
static void scene_range(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "range"), -f - 1, 20); if (-f - 1 == 26) cui_input_mouse_move(ui, 10, 10); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_range(ui); }
}
static void body_time(cui_ctx *ui) {
    static int h = 9, m = 30, h2 = 18, m2 = 45;
    cui_row(ui, .gap = 12) {
        cui_time_picker(ui, "start", &h, &m);
        cui_time_picker(ui, "end", &h2, &m2, .twelve_hour = true, .minute_step = 15);
    }
}
static void scene_time(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "end"), -f - 1, 20); if (-f - 1 == 26) cui_input_mouse_move(ui, 10, 10); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_time(ui); }
}
SCENE(wheels) {
    static int day = 13, month = 10, year = 6, dh = 7, dm = 25;
    static const char *days[31], *months[12], *years[12];
    static char dbuf[31][4], ybuf[12][6];
    static const char *mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    for (int k = 0; k < 31; k++) { snprintf(dbuf[k], sizeof dbuf[k], "%d", k + 1); days[k] = dbuf[k]; }
    for (int k = 0; k < 12; k++) { months[k] = mn[k]; snprintf(ybuf[k], sizeof ybuf[k], "%d", 2020 + k); years[k] = ybuf[k]; }
    cui_row(ui, .gap = 40, .align = CUI_ALIGN_CENTER) {
        cui_card(ui, .dir = CUI_ROW, .gap = 0, .pad = 8) {
            cui_wheel_picker(ui, "day", days, 31, &day, .width = CUI_PX(64), .loop = true, .join_right = true);
            cui_wheel_picker(ui, "month", months, 12, &month, .width = CUI_PX(128), .loop = true, .join_left = true, .join_right = true);
            cui_wheel_picker(ui, "year", years, 12, &year, .width = CUI_PX(80), .join_left = true);
        }
        cui_time_dial(ui, "dial", &dh, &dm, 232);
    }
}
SCENE(rulers) {
    static float height = 172, lo = 40, hi = 160;
    static const float bins[] = { 2, 4, 7, 12, 18, 26, 31, 29, 34, 40, 37, 30, 24, 22, 18, 15, 11, 9, 7, 5, 4, 3, 2, 1 };
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        cui_box(ui, .width = CUI_GROW(1)) { cui_ruler_slider(ui, "Height", &height, 120, 220, .unit = "cm"); }
        cui_card(ui, .width = CUI_GROW(1), .gap = 6) {
            cui_histogram_slider(ui, "Price", bins, 24, &lo, &hi, 0, 240, .format = "$%.0f");
            cui_muted(ui, "1,204 listings in range");
        }
    }
}
static void body_capture(cui_ctx *ui) {
    static cui_texture avatar;
    cui_row(ui, .gap = 20, .width = CUI_FILL, .align = CUI_ALIGN_START) {
        cui_box(ui, .width = CUI_GROW(1)) { cui_dropzone(ui, "files", .description = "PNG, JPG or PDF, up to 25 MB", .accept = "png;jpg;pdf", .height = 200); }
        cui_column(ui, .width = CUI_GROW(1), .gap = 16) {
            cui_signature_pad(ui, "sig", .height = 140);
            cui_row(ui, .gap = 16, .align = CUI_ALIGN_CENTER) {
                cui_avatar_upload(ui, "avatar", "Ada Lovelace", &avatar, 56);
                cui_column(ui, .gap = 2) { cui_label(ui, "Profile photo"); cui_muted(ui, "Drop an image on the avatar"); }
            }
        }
    }
}
static void scene_capture(cui_ctx *ui, int f) {
    if (f < 0) {
        int fr = -f - 1;
        cui_rect sig = cui_get_rect(ui, cui_make_id(ui, "sig") ^ 0x5161u);
        if (fr >= 10 && fr <= 40 && sig.w > 0) {
            /* write a quick scribble */
            float t = (fr - 10) / 30.0f;
            float x = sig.x + 50 + t * (sig.w - 120);
            float y = sig.y + 70 + sinf(t * 14) * 18 - t * 10;
            cui_input_mouse_move(ui, x, y);
            if (fr == 10) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        }
        if (fr == 41) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        cui_rect dz = cui_get_rect(ui, cui_make_id(ui, "files"));
        if (fr >= 45) cui_input_file_hover(ui, true, dz.x + dz.w / 2, dz.y + dz.h / 2);
        return;
    }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28, .justify = CUI_JUSTIFY_CENTER) { body_capture(ui); }
}

static void body_hover_card(cui_ctx *ui) {
    cui_row(ui, .gap = 6) {
        cui_text(ui, "Assigned to");
        cui_link(ui, "@grace");
        cui_hover_card(ui, "grace-card", .width = 300) {
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_START) {
                cui_avatar(ui, "Grace Hopper", .size = 44, .status = "online");
                cui_column(ui, .gap = 4, .width = CUI_FILL) {
                    cui_text(ui, "Grace Hopper", .font = CUI_FONT_MEDIUM);
                    cui_text(ui, "Compilers, Cobol and very small nanoseconds.", .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_row(ui, .gap = 6, .pad_top = 4) {
                        cui_icon(ui, CUI_ICON_CLOCK, 13, CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, "Usually replies within an hour", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
        }
        cui_text(ui, "and due Friday.");
    }
}
static void scene_hover_card(cui_ctx *ui, int f) {
    if (f < 0) { cui_rect r = cui_get_rect(ui, cui_make_id(ui, "@grace") ^ 0x11AC0009u); if (-f - 1 == 5) cui_input_mouse_move(ui, r.x + r.w / 2, r.y + r.h / 2); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_hover_card(ui); }
}
static void body_confirm_pop(cui_ctx *ui) {
    if (cui_button(ui, "Delete branch", .icon = CUI_ICON_TRASH_2)) cui_popover_open(ui, "confirm-delete");
    cui_confirm_popover(ui, "confirm-delete", .title = "Delete feature/grid?", .description = "The branch and its 12 commits are removed from the remote.",
                        .confirm_label = "Delete", .danger = true);
}
static void scene_confirm_pop(cui_ctx *ui, int f) {
    if (f < 0) { click_id(ui, cui_make_id(ui, "Delete branch"), -f - 1, 10); if (-f - 1 == 16) cui_input_mouse_move(ui, 400, 300); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_confirm_pop(ui); }
}
SCENE(expandables) {
    static bool a = true, b = false;
    cui_column(ui, .gap = 14, .width = CUI_PX(620)) {
        cui_expandable_text(ui, "about",
            "CUI is a component library for C. It draws everything itself, so an app looks the same on Windows, macOS and Linux, and it "
            "ships the parts most apps need: inputs, pickers, menus, dialogs, tables, charts and a command palette.", .lines = 2);
        cui_expandable_card(ui, "Billing", &a, .description = "Pro plan, renews on December 1", .icon = CUI_ICON_CREDIT_CARD, .badge = "Active", .badge_tone = CUI_TONE_SUCCESS) {
            cui_row(ui, .gap = 8) { cui_button(ui, "Change plan", .size = CUI_SIZE_SM); cui_button(ui, "Invoices", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST); }
        }
        cui_expandable_card(ui, "Security", &b, .description = "Two-factor authentication is on", .icon = CUI_ICON_SHIELD) { cui_text(ui, "Recovery codes"); }
    }
}
SCENE(banner_countdown) {
    static bool open = true;
    cui_column(ui, .gap = 20, .width = CUI_FILL) {
        cui_banner(ui, "CUI 1.1 is out: data grid, kanban and forty new components.", &open, .action = "Read the notes", .dismissible = true);
        cui_row(ui, .gap = 28, .align = CUI_ALIGN_CENTER) {
            cui_countdown(ui, "launch", 2 * 86400 + 5 * 3600 + 42 * 60 + 18);
            cui_countdown(ui, "sale", 5 * 3600 + 42 * 60 + 18, .compact = true, .hide_days = true);
        }
    }
}
SCENE(checklist) {
    static cui_check_step steps[] = {
        { "Create your workspace", "Pick a name and invite your team.", "Create", true },
        { "Connect a repository", "Link GitHub or GitLab to sync issues and pull requests.", "Connect", false },
        { "Import your data", "Bring tasks over from a CSV or another tool.", "Import", false },
        { "Turn on notifications", NULL, NULL, false },
    };
    cui_box(ui, .width = CUI_PX(440)) { cui_checklist(ui, "onboarding", steps, 4, .title = "Set up your workspace"); }
}
SCENE(toc) {
    static const cui_toc_item toc[] = {
        { "Introduction", "intro", 0 }, { "Install", "install", 0 }, { "With CMake", "cmake", 1 }, { "Without CMake", "nocmake", 1 },
        { "First window", "window", 0 }, { "Next steps", "next", 0 },
    };
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_scroll(ui, .id = "reader", .height = CUI_PX(260), .width = CUI_FILL, .gap = 10) {
            for (int k = 0; k < 6; k++) {
                cui_toc_anchor(ui, toc[k].anchor);
                cui_heading(ui, toc[k].title, toc[k].level ? 4 : 3);
                cui_push_id_int(ui, k);
                cui_text(ui, "Components are functions. They draw themselves, handle input and return what happened.", .color = CUI_COLOR_TEXT_SECONDARY);
                cui_pop_id(ui);
            }
            cui_gap(ui, 200);
        }
        cui_column(ui, .width = CUI_PX(180), .gap = 10) {
            cui_text(ui, "On this page", .size = 12, .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_TEXT_MUTED);
            cui_toc(ui, "reader", toc, 6);
        }
    }
}
static void scene_toc_input(cui_ctx *ui, int f) {
    if (f < 0) {
        cui_rect r = cui_get_rect(ui, cui_make_id(ui, "reader"));
        if (-f - 1 == 10) cui_input_mouse_move(ui, r.x + 40, r.y + 40);
        if (-f - 1 >= 11 && -f - 1 < 17) cui_input_scroll(ui, 0, -1.2f);
        if (-f - 1 == 18) cui_input_mouse_move(ui, 900, 10);
        return;
    }
    frame_wrap(ui, body_toc);
}
static void body_undo(cui_ctx *ui) { cui_button(ui, "Archive 3 conversations", .icon = CUI_ICON_ARCHIVE); }
static void scene_undo(cui_ctx *ui, int f) {
    if (f < 0) { if (-f - 1 == 5) cui_toast(ui, "3 conversations archived", .action = "Undo", .show_timer = true); return; }
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND, .pad = 28) { body_undo(ui); }
}

/* ---- data, code, conversation, media and planning ---- */
SCENE(data_grid) {
    static const char *const teams[] = { "Engine", "Design", "Platform", "Docs" };
    static const cui_filter_field fields[] = { { "Team", CUI_ICON_USERS, teams, 4 }, { "Status", CUI_ICON_CIRCLE_DOT, teams, 0 } };
    static uint32_t filt[2] = { 1u | 4u, 0 };
    static char search[32];
    cui_filter_bar(ui, "filters", fields, 2, filt, search, sizeof search);
    static const cui_grid_column cols[] = { { "Name", 210, CUI_TEXT_LEFT, true }, { "Team", 120, CUI_TEXT_LEFT, true },
                                            { "Commits", 100, CUI_TEXT_RIGHT, true }, { "Score", 150, CUI_TEXT_RIGHT, true } };
    static const char *names[] = { "Grace Hopper", "Ada Lovelace", "Linus Torvalds", "Margaret Hamilton", "Dennis Ritchie", "Barbara Liskov" };
    static const char *tm[] = { "Engine", "Design", "Platform", "Engine", "Platform", "Docs" };
    static const int commits[] = { 884, 812, 640, 597, 431, 309 };
    static const float score[] = { 97.2f, 93.5f, 88.1f, 84.0f, 71.9f, 66.4f };
    static bool sel[6] = { false, true, true, false, false, false };
    static cui_table_sort sort = { 2, true };
    int f0, l0;
    if (cui_data_grid_begin(ui, "people", cols, 4, 6, &f0, &l0, .height = CUI_PX(282), .selected = sel, .sort = &sort)) {
        for (int r = f0; r < l0; r++) {
            char b1[16], b2[16];
            cui_data_grid_row(ui, r);
            cui_data_grid_cell(ui);
            cui_avatar(ui, names[r], .size = 24);
            cui_text(ui, names[r], .size = 13.5f);
            cui_data_grid_cell(ui);
            cui_badge(ui, tm[r], .dot = true, .tone = (cui_tone)(1 + (tm[r][0] % 7)));
            snprintf(b1, sizeof b1, "%d", commits[r]);
            cui_data_grid_cell_text(ui, b1);
            cui_data_grid_cell(ui);
            cui_progress(ui, score[r] / 100, .width = CUI_PX(70), .height = 5);
            snprintf(b2, sizeof b2, "%.1f", (double)score[r]);
            cui_text(ui, b2, .tabular = true, .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_data_grid_end(ui);
    }
}
SCENE(code_block) {
    cui_code_block(ui, "c",
        "#include <cui/cui.h>\n"
        "\n"
        "/* A counter with a button */\n"
        "static int clicks = 0;\n"
        "\n"
        "void frame(cui_ctx *ui) {\n"
        "    if (cui_button(ui, \"Click me\", .variant = CUI_BUTTON_PRIMARY)) clicks++;\n"
        "    cui_textf(ui, \"Clicked %d times\", clicks);\n"
        "}\n",
        .lang = "c", .title = "counter.c", .line_numbers = true, .highlight_from = 7);
}
SCENE(json_view) {
    cui_json_view(ui, "json",
        "{\"name\": \"cui\", \"version\": \"1.1.0\", \"private\": false, \"downloads\": 128400,\n"
        " \"platforms\": [\"windows\", \"macos\", \"linux\"],\n"
        " \"renderer\": {\"backend\": \"opengl\", \"fallback\": \"software\", \"vsync\": true, \"msaa\": null},\n"
        " \"authors\": [{\"name\": \"Ada\", \"commits\": 412}, {\"name\": \"Grace\", \"commits\": 388}]}",
        .expand_depth = 1);
}
SCENE(chat) {
    cui_card(ui, .gap = 0, .pad = 16, .width = CUI_FILL) {
        cui_chat_divider(ui, "Today");
        cui_chat_message(ui, "Did the grid ship?", .author = "Grace Hopper", .time = "9:41", .last_in_group = false);
        cui_chat_message(ui, "Virtualized, sortable, editable. 2,000 rows scroll at 60 fps.", .author = "Grace Hopper", .grouped = true, .last_in_group = true);
        cui_chat_message(ui, "It did! Keyboard selection too.", .mine = true);
        cui_chat_message(ui, "Shift and Ctrl clicks work like a spreadsheet.", .mine = true, .grouped = true, .last_in_group = true, .status = "Read", .time = "9:43");
        cui_typing_indicator(ui, "Grace");
    }
}
SCENE(comments) {
    static int likes[3] = { 12, 3, 0 };
    static bool liked[3] = { true, false, false };
    cui_card(ui, .gap = 4, .pad = 14, .width = CUI_FILL) {
        cui_comment(ui, "c1", "Katherine Johnson", "The trajectory numbers check out. I would still run the sim once more before launch.", .time = "2h", .likes = &likes[0], .liked = &liked[0], .badge = "Author");
        cui_comment(ui, "c2", "Alan Turing", "Agreed, one more run.", .time = "1h", .depth = 1, .likes = &likes[1], .liked = &liked[1]);
        cui_comment(ui, "c3", "Ada Lovelace", "Scheduled it for 3 pm.", .time = "40m", .depth = 1, .likes = &likes[2], .liked = &liked[2], .edited = true);
    }
}
SCENE(presence_ticker) {
    static const cui_presence_user who[] = {
        { "Ada Lovelace", { 0x7c, 0x5c, 0xff, 255 }, 0, false }, { "Grace Hopper", { 0x22, 0xc5, 0x8b, 255 }, 0, false },
        { "Alan Turing", { 0xf5, 0x9e, 0x0b, 255 }, 0, false }, { "Linus Torvalds", { 0x38, 0xbd, 0xf8, 255 }, 0, true },
        { "Hedy Lamarr", { 0xec, 0x48, 0x99, 255 }, 0, false }, { "Dennis Ritchie", { 0x10, 0xb9, 0x81, 255 }, 0, false },
        { "Barbara Liskov", { 0xa8, 0x55, 0xf7, 255 }, 0, false },
    };
    static const cui_ticker_item tick[] = {
        { "Revenue", "$48.2k", 6.9f }, { "Active users", "12,408", 3.1f }, { "Churn", "1.7%", -0.6f }, { "Latency", "182 ms", -8.4f },
        { "Builds", "1,204", 12.0f }, { "Uptime", "99.98%", 0 },
    };
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_CENTER) {
        cui_presence(ui, "here", who, 7, 5, 30);
        cui_counter(ui, "rev", 48210, .format = "$%.0f", .size = 34, .group_thousands = true);
    }
    cui_ticker(ui, "tick", tick, 6);
}
SCENE(carousel) {
    static int slide = 1;
    static const char *titles[] = { "Calm by default", "Motion with purpose", "Keyboard first" };
    static const char *descs[] = { "One accent, hairline borders, layered shadows only where things float.",
                                   "Springs for presses, highlights that glide, overlays that open from their trigger.",
                                   "Tab, arrows, Enter and Esc work everywhere, with a ring only for the keyboard." };
    static const unsigned tops[] = { 0x3b1d5e, 0x0f3d3e, 0x1d2a5e };
    cui_carousel(ui, "tour", 3, &slide, .height = 200) {
        for (int k = 0; k < 3; k++) cui_carousel_slide(ui) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_RGB(tops[k]), .radius = 16, .pad = 28, .gap = 8, .justify = CUI_JUSTIFY_END) {
                cui_text(ui, titles[k], .font = CUI_FONT_DISPLAY, .size = 24, .color = CUI_RGB(0xffffff));
                cui_text(ui, descs[k], .color = CUI_RGBA(0xffffffc0));
            }
        }
    }
}
static void shot_scene(unsigned char *px, int w, int h, bool color) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w, fy = (float)y / h;
            float r = 0.98f - fy * 0.45f, g = 0.62f - fy * 0.3f + fx * 0.1f, b = 0.38f + fy * 0.4f;
            float m1 = 0.55f + 0.1f * sinf(fx * 7.0f) + 0.04f * sinf(fx * 23.0f);
            float m2 = 0.7f + 0.06f * sinf(fx * 5.0f + 2.1f);
            float dx = fx - 0.7f, dy = fy - 0.32f;
            if (dx * dx + dy * dy < 0.006f) { r = 1; g = 0.92f; b = 0.7f; }
            if (fy > m1) { r = 0.32f - fy * 0.1f; g = 0.24f; b = 0.42f; }
            if (fy > m2) { r = 0.14f; g = 0.12f; b = 0.24f + (1 - fy) * 0.2f; }
            if (!color) { float l = r * 0.3f + g * 0.59f + b * 0.11f; r = g = b = l * 0.85f + 0.05f; }
            unsigned char *p = px + (y * w + x) * 4;
            p[0] = (unsigned char)(fminf(1, fmaxf(0, r)) * 255); p[1] = (unsigned char)(fminf(1, fmaxf(0, g)) * 255);
            p[2] = (unsigned char)(fminf(1, fmaxf(0, b)) * 255); p[3] = 255;
        }
}
SCENE(compare_stack) {
    static cui_ctx *owner;
    static cui_texture before, after;
    static float split = 0.42f;
    if (owner != ui) {
        static unsigned char a[480 * 300 * 4], b[480 * 300 * 4];
        shot_scene(a, 480, 300, false);
        shot_scene(b, 480, 300, true);
        before = cui_image_create(ui, 480, 300, a);
        after = cui_image_create(ui, 480, 300, b);
        owner = ui;
    }
    cui_row(ui, .gap = 40, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
        cui_box(ui, .width = CUI_GROW(1)) { cui_image_compare(ui, "cmp", before, after, &split, .height = 230); }
        static const char *names[] = { "Kyoto", "Lisbon", "Reykjavik" };
        static const unsigned cols[] = { 0x7c3aed, 0x0ea5e9, 0x10b981 };
        cui_card_stack_begin(ui, "deck", 3, &(cui_card_stack_opts){ .width = CUI_PX(240), .height = 200 });
        for (int k = 0; k < 3; k++) cui_card_stack_item(ui, k) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_RGB(cols[k]), .radius = 20, .pad = 20, .justify = CUI_JUSTIFY_END, .gap = 4) {
                cui_text(ui, names[k], .font = CUI_FONT_DISPLAY, .size = 22, .color = CUI_RGB(0xffffff));
                cui_text(ui, "Swipe right to save", .size = 13, .color = CUI_RGBA(0xffffffb0));
            }
        }
        cui_card_stack_end(ui);
    }
}
static cui_id kb_ids[3][3];
static void body_kanban(cui_ctx *ui);
static void scene_kanban(cui_ctx *ui, int f) {
    if (f < 0) {   /* lift the first backlog card and carry it into "In progress" */
        int t = -f - 1;
        cui_rect a = cui_get_rect(ui, kb_ids[0][0]), b = cui_get_rect(ui, kb_ids[1][1]);
        float x0 = a.x + 90, y0 = a.y + 30, x1 = b.x + 110, y1 = b.y - 10;
        if (t == 20) cui_input_mouse_move(ui, x0, y0);
        if (t == 21) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
        if (t >= 22 && t <= 40) { float k = (t - 22) / 18.0f; cui_input_mouse_move(ui, x0 + (x1 - x0) * k, y0 + (y1 - y0) * k); }
        return;
    }
    frame_wrap(ui, body_kanban);
}
static void body_kanban(cui_ctx *ui) {
    static const char *col_names[3] = { "Backlog", "In progress", "Done" };
    static const unsigned dots[3] = { 0x94a3b8, 0x3b82f6, 0x22c55e };
    static const char *titles[3][3] = { { "Audit the color tokens for contrast", "Virtualize the activity feed", "Dark mode screenshots" },
                                        { "Keyboard nav in the date range picker", "IME composition on Windows", NULL },
                                        { "Spring presets", NULL, NULL } };
    static const char *tags[3][3] = { { "Design", "Perf", "Docs" }, { "A11y", "Platform", NULL }, { "Motion", NULL, NULL } };
    static const cui_tone tones[3][3] = { { CUI_TONE_VIOLET, CUI_TONE_TEAL, CUI_TONE_ROSE }, { CUI_TONE_INFO, CUI_TONE_WARNING }, { CUI_TONE_VIOLET } };
    static const char *who[3][3] = { { "Ada Lovelace", "Grace Hopper", "Hedy Lamarr" }, { "Alan Turing", "Dennis Ritchie" }, { "Barbara Liskov" } };
    static const int lens[3] = { 3, 2, 1 };
    cui_kanban_begin(ui, "board");
    for (int c = 0; c < 3; c++) {
        cui_kanban_column_begin(ui, col_names[c], lens[c], .dot = CUI_RGB(dots[c]), .limit = c == 1 ? 3 : 0, .width = 270);
        for (int k = 0; k < lens[c]; k++) {
            cui_kanban_card_begin(ui, c * 10 + k);
            kb_ids[c][k] = cui_last_id(ui);
            cui_badge(ui, tags[c][k], .tone = tones[c][k], .size = CUI_SIZE_SM);
            cui_text(ui, titles[c][k], .font = CUI_FONT_MEDIUM, .size = 13.5f, .line_height = 1.35f);
            if (c == 1) cui_progress(ui, k ? 0.3f : 0.6f, .height = 4);
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                cui_avatar(ui, who[c][k], .size = 22);
                cui_spacer(ui);
                cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 13, CUI_COLOR_TEXT_MUTED);
                cui_text(ui, k == 1 ? "2" : "3", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            }
            cui_kanban_card_end(ui);
        }
        cui_kanban_column_end(ui);
    }
    cui_kanban_end(ui, NULL);
}
SCENE(week_calendar) {
    static cui_cal_event events[] = {
        { "Standup", "Zoom", 0, 9.5f, 9.75f, CUI_TONE_INFO }, { "Design review", "Room 2", 0, 11, 12.5f, CUI_TONE_VIOLET },
        { "Lunch with Ada", NULL, 1, 12.5f, 13.5f, CUI_TONE_SUCCESS }, { "Standup", "Zoom", 1, 9.5f, 9.75f, CUI_TONE_INFO },
        { "Release planning", "Room 1", 2, 10, 11.5f, CUI_TONE_WARNING }, { "1:1 Grace", NULL, 2, 10.5f, 11.5f, CUI_TONE_ROSE },
        { "Focus time", "No meetings", 2, 13.5f, 16, CUI_TONE_TEAL }, { "Standup", "Zoom", 3, 9.5f, 9.75f, CUI_TONE_INFO },
        { "Customer call", "Acme", 3, 14, 15, CUI_TONE_DANGER }, { "Demo day", "All hands", 4, 15, 16.5f, CUI_TONE_VIOLET },
        { "Gym", NULL, 5, 10, 11.5f, CUI_TONE_SUCCESS },
    };
    cui_date wk = { 2026, 11, 2 };
    cui_week_calendar(ui, "week", wk, events, 11, .start_hour = 8, .end_hour = 17, .hour_height = 44, .height = CUI_PX(400));
}
SCENE(gantt) {
    static cui_gantt_task tasks[] = {
        { "Research", "Ada Lovelace", 0, 5, 1, CUI_TONE_VIOLET, -1, false },
        { "Wireframes", "Hedy Lamarr", 4, 10, 1, CUI_TONE_ROSE, 0, false },
        { "Design system", "Barbara Liskov", 8, 16, 0.7f, CUI_TONE_VIOLET, 1, false },
        { "Core engine", "Grace Hopper", 6, 19, 0.55f, CUI_TONE_INFO, 0, false },
        { "Beta", NULL, 21, 21, 0, CUI_TONE_WARNING, 3, true },
        { "Launch", "Alan Turing", 22, 27, 0, CUI_TONE_SUCCESS, 4, false },
    };
    cui_date org = { 2026, 10, 26 };
    cui_gantt(ui, "plan", tasks, 6, org, 28, .day_width = 26, .today = 10);
}

/* A mosaic of real components in cards, for the README. */
static void mosaic_signup(cui_ctx *ui) {
    static char email[64] = "ada@analytical.engine", pw[32] = "correct horse battery";
    cui_card(ui, .gap = 16, .width = CUI_FILL) {
        cui_column(ui, .gap = 4) {
            cui_text(ui, "Create an account", .font = CUI_FONT_DISPLAY, .size = 20);
            cui_text(ui, "Enter your email below to create your account.", .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_box(ui, .dir = CUI_GRID, .columns = 2, .gap = 10, .width = CUI_FILL) {
            cui_button(ui, "GitHub", .icon = CUI_ICON_GIT_BRANCH, .variant = CUI_BUTTON_OUTLINE, .full_width = true);
            cui_button(ui, "Google", .icon = CUI_ICON_GLOBE, .variant = CUI_BUTTON_OUTLINE, .full_width = true);
        }
        cui_row(ui, .gap = 10, .width = CUI_FILL, .align = CUI_ALIGN_CENTER) {
            cui_box(ui, .width = CUI_FILL) { cui_separator(ui); }
            cui_text(ui, "OR CONTINUE WITH", .size = 11, .color = CUI_COLOR_TEXT_MUTED);
            cui_box(ui, .width = CUI_FILL) { cui_separator(ui); }
        }
        cui_input(ui, "m-email", email, sizeof email, .label = "Email", .icon = CUI_ICON_MAIL);
        cui_column(ui, .gap = 8, .width = CUI_FILL) {
            cui_input(ui, "m-pw", pw, sizeof pw, .label = "Password", .password = true);
            cui_password_strength(ui, pw);
        }
        cui_button(ui, "Create account", .variant = CUI_BUTTON_PRIMARY, .full_width = true);
    }
}
static void mosaic_notifications(cui_ctx *ui) {
    static bool a = true, b = false, c = true;
    cui_card(ui, .gap = 16, .width = CUI_FILL) {
        cui_column(ui, .gap = 4) {
            cui_label(ui, "Notifications");
            cui_text(ui, "Choose what you want to hear about.", .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        const char *titles[] = { "Mentions", "Weekly digest", "Security alerts" };
        const char *descs[] = { "When someone @mentions you", "A summary every Monday", "Sign-ins from new devices" };
        bool *vals[] = { &a, &b, &c };
        for (int i = 0; i < 3; i++) {
            cui_push_id_int(ui, i);
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_column(ui, .gap = 1, .width = CUI_FILL) {
                    cui_text(ui, titles[i], .font = CUI_FONT_MEDIUM, .size = 13);
                    cui_text(ui, descs[i], .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                }
                cui_switch(ui, "##n", vals[i]);
            }
            cui_pop_id(ui);
        }
    }
}
static void mosaic_metrics(cui_ctx *ui) {
    static const float s1[] = { 3, 5, 4, 7, 6, 9, 8, 11, 10, 13 };
    static const float s2[] = { 9, 8, 9, 7, 8, 6, 7, 5, 6, 4 };
    cui_box(ui, .dir = CUI_GRID, .columns = 2, .gap = 20, .width = CUI_FILL) {
        cui_metric(ui, "Revenue", "$15,231", .delta = 20.1f, .spark = s1, .spark_count = 10, .icon = CUI_ICON_DOLLAR_SIGN);
        cui_metric(ui, "Latency", "182 ms", .delta = -8.4f, .delta_label = "-8.4% faster", .spark = s2, .spark_count = 10, .icon = CUI_ICON_ZAP);
    }
}
static void mosaic_calendar(cui_ctx *ui) {
    static cui_date d = { 2026, 11, 14 };
    static const char *slots[] = { "09:00", "10:30", "13:00", "15:30" };
    static int slot = 1;
    cui_card(ui, .dir = CUI_ROW, .gap = 20, .width = CUI_FILL) {
        cui_calendar(ui, "m-cal", &d);
        cui_column(ui, .gap = 10, .width = CUI_FILL) {
            cui_label(ui, "Book a call");
            cui_text(ui, "30 minutes with the design team.", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_box(ui, .height = CUI_PX(4)) {}
            for (int i = 0; i < 4; i++) {
                cui_push_id_int(ui, i);
                if (cui_button(ui, slots[i], .variant = slot == i ? CUI_BUTTON_PRIMARY : CUI_BUTTON_OUTLINE, .full_width = true)) slot = i;
                cui_pop_id(ui);
            }
        }
    }
}
static void mosaic_chart(cui_ctx *ui) {
    static const char *days[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const float a[] = { 42, 51, 38, 64, 58, 71, 66 };
    static const float b[] = { 30, 34, 29, 41, 38, 44, 52 };
    cui_card(ui, .gap = 14, .width = CUI_FILL) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_START) {
            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                cui_label(ui, "Active users");
                cui_text(ui, "Desktop and web, last 7 days", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_badge(ui, "+12.5%", .tone = CUI_TONE_SUCCESS, .icon = CUI_ICON_TRENDING_UP);
        }
        cui_series series[2] = { { "Desktop", a, 7, { 0 } }, { "Web", b, 7, { 0 } } };
        cui_bar_chart(ui, "m-bars", series, 2, .labels = days, .label_count = 7, .height = 190, .no_legend = true);
    }
}
static void mosaic_team(cui_ctx *ui) {
    static const char *names[] = { "Grace Hopper", "Alan Turing", "Katherine Johnson" };
    static const char *mails[] = { "grace@navy.mil", "alan@bletchley.uk", "katherine@nasa.gov" };
    static const char *roles[] = { "Owner", "Editor", "Viewer" };
    cui_card(ui, .gap = 16, .width = CUI_FILL) {
        cui_row(ui, .width = CUI_FILL, .align = CUI_ALIGN_START) {
            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                cui_label(ui, "Team members");
                cui_text(ui, "Invite people to collaborate.", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_button(ui, "Invite", .icon = CUI_ICON_USER_PLUS, .size = CUI_SIZE_SM);
        }
        for (int i = 0; i < 3; i++) {
            cui_push_id_int(ui, i);
            cui_row(ui, .width = CUI_FILL, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_avatar(ui, names[i], .size = 36, .status = i == 0 ? "online" : i == 1 ? "away" : NULL);
                cui_column(ui, .gap = 1, .width = CUI_FILL) {
                    cui_text(ui, names[i], .font = CUI_FONT_MEDIUM, .size = 13, .nowrap = true);
                    cui_text(ui, mails[i], .size = 12, .color = CUI_COLOR_TEXT_MUTED, .nowrap = true);
                }
                cui_button(ui, roles[i], .size = CUI_SIZE_SM, .variant = CUI_BUTTON_OUTLINE, .icon_right = CUI_ICON_CHEVRON_DOWN);
            }
            cui_pop_id(ui);
        }
    }
}
static void mosaic_share(cui_ctx *ui) {
    static char link[96] = "https://orbit.dev/doc/q3-roadmap";
    static int access = 1;
    cui_card(ui, .gap = 14, .width = CUI_FILL) {
        cui_column(ui, .gap = 2) {
            cui_label(ui, "Share this document");
            cui_text(ui, "Anyone with the link can view.", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        }
        cui_row(ui, .width = CUI_FILL, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_input(ui, "m-link", link, sizeof link, .read_only = true, .width = CUI_FILL);
            cui_copy_button(ui, "m-copy", link);
        }
        const char *opts[] = { "Can edit", "Can view" };
        cui_segmented(ui, "m-access", opts, 2, &access, .full_width = true);
    }
}
static void mosaic_progress(cui_ctx *ui) {
    static float goal = 350;
    cui_card(ui, .gap = 16, .width = CUI_FILL) {
        cui_row(ui, .width = CUI_FILL, .gap = 18, .align = CUI_ALIGN_CENTER) {
            cui_progress_ring(ui, "##m-ring", 0.72f, 84);
            cui_column(ui, .gap = 4, .width = CUI_FILL) {
                cui_label(ui, "Move goal");
                cui_text(ui, "Set your daily activity goal.", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        cui_slider(ui, "Calories", &goal, 100, 600, .step = 10, .show_value = true, .format = "%.0f kcal");
    }
}
static void mosaic_storage(cui_ctx *ui) {
    cui_card(ui, .gap = 12, .width = CUI_FILL) {
        cui_row(ui, .width = CUI_FILL, .gap = 10, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_DATABASE, 16, CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, "Storage", .font = CUI_FONT_MEDIUM, .width = CUI_FILL);
            cui_button(ui, "Upgrade", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST, .icon_right = CUI_ICON_ARROW_UP_RIGHT);
        }
        cui_meter(ui, "Used", 68.4f, 100, "GB");
    }
}
static void mosaic_topics(cui_ctx *ui) {
    static bool sel[6] = { true, false, true, false, false, true };
    static const char *names[] = { "Design", "C99", "Motion", "Rendering", "Docs", "Native" };
    cui_card(ui, .gap = 12, .width = CUI_FILL) {
        cui_label(ui, "Topics you follow");
        for (int r = 0; r < 2; r++)
            cui_row(ui, .gap = 8) {
                for (int i = r * 3; i < r * 3 + 3; i++) { cui_push_id_int(ui, i); cui_chip(ui, names[i], &sel[i]); cui_pop_id(ui); }
            }
    }
}
SCENE(mosaic) {
    cui_row(ui, .width = CUI_FILL, .gap = 20, .align = CUI_ALIGN_START) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 20) { mosaic_signup(ui); mosaic_notifications(ui); mosaic_storage(ui); }
        cui_column(ui, .width = CUI_GROW(1.25f), .gap = 20) { mosaic_metrics(ui); mosaic_calendar(ui); mosaic_chart(ui); }
        cui_column(ui, .width = CUI_GROW(1), .gap = 20) { mosaic_team(ui); mosaic_share(ui); mosaic_progress(ui); mosaic_topics(ui); }
    }
}

typedef struct { const char *name; scene_fn fn; float w, h; bool dark_too; bool dark_only; cui_accent accent; } scene;

static void render(const char *dir, const scene *s, bool dark, cui_sw_renderer *sw) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT,
                                             .accent = s->accent });
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
        { "icons", scene_icons, 1040, 1120, false },
        { "radio-cards", scene_radio_cards, 780, 170, true }, { "tag-otp", scene_tag_otp, 520, 330, true },
        { "button-extras", scene_button_extras, 640, 110, true }, { "timeline", scene_timeline, 620, 300, true },
        { "mosaic", scene_mosaic, 1440, 900, true, true, CUI_ACCENT_BLUE },
        { "split-button", scene_split_button, 560, 110, true }, { "toggle-group", scene_toggle_group, 560, 110, true },
        { "morph-buttons", scene_morph, 760, 110, true }, { "user-menu", scene_user_menu, 340, 360, true },
        { "inline-edit", scene_inline_edit, 420, 150, true }, { "scrub", scene_scrub, 600, 110, true },
        { "shortcut-recorder", scene_shortcut_rec, 760, 110, true },
        { "multi-select", scene_multi, 380, 420, true }, { "date-range", scene_range, 760, 440, true },
        { "time-picker", scene_time, 420, 330, true }, { "wheels", scene_wheels, 700, 340, true },
        { "ruler-histogram", scene_rulers, 860, 200, true }, { "capture", scene_capture, 900, 300, true },
        { "hover-card", scene_hover_card, 460, 220, true }, { "confirm-popover", scene_confirm_pop, 420, 260, true },
        { "expandables", scene_expandables, 680, 320, true }, { "banner-countdown", scene_banner_countdown, 820, 230, true },
        { "checklist", scene_checklist, 500, 420, true }, { "toc", scene_toc_input, 760, 320, true },
        { "undo-toast", scene_undo, 460, 200, true },
        { "data-grid", scene_data_grid, 760, 390, true }, { "code-block", scene_code_block, 720, 300, true },
        { "json-view", scene_json_view, 560, 330, true }, { "chat", scene_chat, 560, 440, true },
        { "comments", scene_comments, 600, 370, true }, { "presence-ticker", scene_presence_ticker, 640, 170, true },
        { "carousel", scene_carousel, 700, 300, true }, { "compare-stack", scene_compare_stack, 860, 320, true },
        { "kanban", scene_kanban, 920, 480, true }, { "week-calendar", scene_week_calendar, 960, 520, true },
        { "gantt", scene_gantt, 1000, 360, true },
    };
    cui_sw_renderer *sw = cui_sw_create();
    for (size_t i = 0; i < sizeof scenes / sizeof scenes[0]; i++) {
        if (argc > 2 && strcmp(argv[2], scenes[i].name) != 0) continue;   /* optional: one scene */
        if (!scenes[i].dark_only) render(dir, &scenes[i], false, sw);
        if (scenes[i].dark_too) render(dir, &scenes[i], true, sw);
    }
    cui_sw_destroy(sw);
    return 0;
}
