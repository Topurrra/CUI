/* Extended gallery pages. Each page keeps its own state in statics. */
#include "gallery_more.h"

#include <stdio.h>
#include <string.h>

static void section(cui_ctx *ui, const char *title, const char *desc) {
    cui_column(ui, .gap = 4) {
        cui_heading(ui, title, 4);
        if (desc) cui_secondary(ui, desc);
    }
}

/* ---- actions ---------------------------------------------------------- */

static void page_actions(cui_ctx *ui) {
    static bool fmt[4] = { true, false, false, false };
    static bool align[3] = { true, false, false };
    static cui_status save_status = CUI_STATUS_IDLE;
    static double status_at;
    static cui_reaction reactions[4] = {
        { CUI_ICON_THUMBS_UP, 12, true }, { CUI_ICON_HEART, 4, false }, { CUI_ICON_ROCKET, 7, false }, { CUI_ICON_SMILE, 2, false },
    };
    static int dial_choice = -1;
    static int theme_mode = -1;
    static int deleted;
    static int split_choice = -1;

    section(ui, "Split button", "A main action with related actions one click away.");
    cui_row(ui, .gap = 12) {
        static const char *items[] = { "Save as draft", "Save and publish", "Schedule..." };
        static const char *icons[] = { CUI_ICON_FILE_TEXT, CUI_ICON_SEND, CUI_ICON_CLOCK };
        int r = cui_split_button(ui, "Save", items, 3, .item_icons = icons, .icon = CUI_ICON_SAVE);
        if (r >= 0) split_choice = r;
        cui_split_button(ui, "Export", items, 3, .variant = CUI_BUTTON_OUTLINE, .icon = CUI_ICON_DOWNLOAD);
        if (split_choice >= 0) cui_muted(ui, split_choice == 0 ? "Saved" : items[split_choice - 1]);
    }

    section(ui, "Toggle group", "Independent toggles for toolbars, or exclusive like radio buttons.");
    cui_row(ui, .gap = 12) {
        static const char *labels[] = { "Bold", "Italic", "Underline", "Code" };
        static const char *icons[] = { CUI_ICON_BOLD, CUI_ICON_ITALIC, CUI_ICON_UNDERLINE, CUI_ICON_CODE };
        cui_toggle_group(ui, "format", labels, 4, fmt, .icons = icons, .icon_only = true);
        static const char *alabels[] = { "Left", "Center", "Right" };
        static const char *aicons[] = { CUI_ICON_ALIGN_LEFT, CUI_ICON_LIST, CUI_ICON_MENU };
        cui_toggle_group(ui, "align", alabels, 3, align, .icons = aicons, .exclusive = true);
    }

    section(ui, "Morphing buttons", "Confirm before destroying, and show the result of work in place.");
    cui_row(ui, .gap = 12) {
        if (cui_confirm_button(ui, "Delete project", .icon = CUI_ICON_TRASH_2)) deleted++;
        if (cui_status_button(ui, "Save changes", save_status, .icon = CUI_ICON_SAVE, .success_label = "Saved")) {
            save_status = CUI_STATUS_BUSY;
            status_at = cui_time(ui);
        }
        double age = cui_time(ui) - status_at;
        if (save_status == CUI_STATUS_BUSY && age > 1.4) { save_status = CUI_STATUS_SUCCESS; status_at = cui_time(ui); }
        else if (save_status == CUI_STATUS_SUCCESS && age > 1.6) save_status = CUI_STATUS_IDLE;
        if (save_status != CUI_STATUS_IDLE) cui_request_redraw(ui);
        if (deleted) cui_muted(ui, "Project deleted");
    }

    section(ui, "Reactions", "Counts roll when they change.");
    cui_reactions(ui, "reactions", reactions, 4);

    section(ui, "Theme", "A compact switcher and a single toggle that morphs between sun and moon.");
    cui_row(ui, .gap = 16) {
        cui_theme_switcher(ui, &theme_mode, true);
        cui_theme_toggle(ui);
    }

    section(ui, "User menu", "Account trigger for sidebars and headers.");
    cui_box(ui, .width = CUI_PX(260)) {
        cui_user_menu(ui, "Ada Lovelace", .detail = "ada@analytical.engine", .status = "online", .placement = CUI_PLACE_BOTTOM_START) {
            cui_menu_item(ui, "Profile", .icon = CUI_ICON_USER, .shortcut = "Ctrl P");
            cui_menu_item(ui, "Billing", .icon = CUI_ICON_CREDIT_CARD);
            cui_menu_item(ui, "Settings", .icon = CUI_ICON_SETTINGS, .shortcut = "Ctrl ,");
            cui_menu_separator(ui);
            cui_menu_item(ui, "Log out", .icon = CUI_ICON_LOG_OUT);
        }
    }

    section(ui, "Swipe actions", "Drag a row sideways or swipe with two fingers.");
    cui_card(ui, .pad = 6, .gap = 6) {
        static const cui_swipe_action left[] = { { CUI_ICON_CHECK, "Done", CUI_TONE_SUCCESS } };
        static const cui_swipe_action right[] = { { CUI_ICON_ARCHIVE, "Archive", CUI_TONE_INFO }, { CUI_ICON_TRASH_2, "Delete", CUI_TONE_DANGER } };
        static const char *titles[] = { "Review the design tokens", "Reply to Grace about the release", "Book the offsite venue" };
        for (int i = 0; i < 3; i++) {
            cui_push_id_int(ui, i);
            cui_swipe_item_begin(ui, "row", left, 1, right, 2);
            cui_icon(ui, CUI_ICON_CIRCLE, 16, CUI_COLOR_TEXT_MUTED);
            cui_text(ui, titles[i], .width = CUI_FILL, .nowrap = true);
            cui_text(ui, "Today", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
            int a = cui_swipe_item_end(ui);
            if (a >= 0) cui_toast(ui, a == 0 ? "Marked as done" : a == 100 ? "Archived" : "Deleted", .tone = a == 101 ? CUI_TONE_DANGER : CUI_TONE_SUCCESS);
            cui_pop_id(ui);
        }
    }

    section(ui, "Speed dial", "A floating action button that fans out related actions.");
    cui_row(ui, .gap = 16, .align = CUI_ALIGN_CENTER, .height = CUI_PX(80)) {
        static const cui_speed_action acts[] = { { CUI_ICON_FILE_TEXT, "New note" }, { CUI_ICON_IMAGE, "Upload image" }, { CUI_ICON_MIC, "Record memo" } };
        int c = cui_speed_dial(ui, "dial", acts, 3, .tooltip = "Create");
        if (c >= 0) dial_choice = c;
        if (dial_choice >= 0) cui_muted(ui, acts[dial_choice].label);
    }
}

/* ---- smart inputs ------------------------------------------------------- */

static void page_inputs(cui_ctx *ui) {
    static char title[128] = "Q3 roadmap";
    static char owner[64] = "Grace Hopper";
    static float opacity = 80, radius = 12, blur = 24;
    static double amount = 1249.5, salary = 98000;
    static cui_key_combo combo = { CUI_MOD_PRIMARY | CUI_MOD_SHIFT, CUI_KEY_P };
    static cui_key_combo combo2 = { 0, CUI_KEY_NONE };
    static char url[96] = "cui.dev", weight[16] = "72";

    section(ui, "Inline edit", "Click the text to edit it in place. Enter saves, Esc cancels.");
    cui_column(ui, .gap = 6) {
        cui_inline_edit(ui, "title", title, sizeof title, .font = CUI_FONT_DISPLAY, .size = 26);
        cui_inline_edit(ui, "owner", owner, sizeof owner, .placeholder = "Add an owner");
    }

    section(ui, "Scrub fields", "Drag sideways to change the value, Shift for fine steps, click to type.");
    cui_row(ui, .gap = 12) {
        cui_scrub(ui, "Opacity", &opacity, 0, 100, .suffix = "%");
        cui_scrub(ui, "Radius", &radius, 0, 48, .step = 0.5f, .format = "%.1f", .suffix = "px");
        cui_scrub(ui, "Blur", &blur, 0, 80, .suffix = "px", .width = CUI_PX(140));
    }

    section(ui, "Money", "Grouped digits at rest, plain digits while typing.");
    cui_row(ui, .gap = 12, .align = CUI_ALIGN_START) {
        cui_box(ui, .width = CUI_PX(240)) { cui_money_input(ui, "amount", &amount, .label = "Amount", .code = "USD"); }
        cui_box(ui, .width = CUI_PX(240)) { cui_money_input(ui, "salary", &salary, .label = "Salary", .symbol = "\xe2\x82\xac", .code = "EUR", .decimals = -1, .hint = "Gross per year"); }
    }

    section(ui, "Prefix and suffix", "Any text field can carry muted text before and after the value.");
    cui_row(ui, .gap = 12) {
        cui_input(ui, "url", url, sizeof url, .prefix = "https://", .width = CUI_PX(260));
        cui_input(ui, "weight", weight, sizeof weight, .suffix = "kg", .width = CUI_PX(120));
    }

    section(ui, "Shortcut recorder", "Click, then press a combination. Backspace clears.");
    cui_row(ui, .gap = 12) {
        cui_shortcut_recorder(ui, "palette-key", &combo);
        cui_shortcut_recorder(ui, "search-key", &combo2, .placeholder = "Record shortcut");
    }
}

/* ---- registry ------------------------------------------------------------ */

typedef struct { const char *name, *icon; void (*fn)(cui_ctx *ui); } more_page;

static const more_page pages[] = {
    { "Actions", CUI_ICON_SPARKLES, page_actions },
    { "Smart inputs", CUI_ICON_SQUARE_PEN, page_inputs },
};

int gallery_more_count(void) { return (int)(sizeof pages / sizeof pages[0]); }
const char *gallery_more_name(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].name : ""; }
const char *gallery_more_icon(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].icon : NULL; }
void gallery_more_page(cui_ctx *ui, int page) { if (page >= 0 && page < gallery_more_count()) pages[page].fn(ui); }
