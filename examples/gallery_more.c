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


/* ---- pickers ------------------------------------------------------------- */

static void page_pickers(cui_ctx *ui) {
    static bool langs[8] = { true, false, true, false, false, true, false, false };
    static cui_date_range range = { { 2026, 11, 3 }, { 2026, 11, 14 } };
    static cui_date_range stay = { { 2026, 12, 18 }, { 2026, 12, 27 } };
    static int hour = 9, minute = 30, h2 = 18, m2 = 45;
    static int day = 13, month = 10, year = 6;
    static int dh = 7, dm = 25;
    static float height = 172, temp = 21.5f;
    static float lo = 40, hi = 160;
    static const float bins[] = { 2, 4, 7, 12, 18, 26, 31, 29, 34, 40, 37, 30, 24, 22, 18, 15, 11, 9, 7, 5, 4, 3, 2, 1 };

    section(ui, "Multi-select", "Chips for the chosen items, checkboxes in the list.");
    static const char *items[] = { "C", "C++", "Rust", "Zig", "Go", "Swift", "Kotlin", "Odin" };
    cui_row(ui, .gap = 12, .align = CUI_ALIGN_START) {
        cui_box(ui, .width = CUI_PX(320)) { cui_multi_select(ui, "langs", items, 8, langs, .label = "Languages", .searchable = true); }
    }

    section(ui, "Date range", "Presets on the left, two months to pick a start and an end.");
    cui_row(ui, .gap = 12) {
        cui_date_range_picker(ui, "range", &range, .width = CUI_PX(300));
        cui_date_range_picker(ui, "stay", &stay, .width = CUI_PX(300), .no_presets = true);
    }
    cui_card(ui, .pad = 16) { cui_range_calendar(ui, "inline-range", &stay, 2); }

    section(ui, "Time", "A field with hour and minute columns, 24 hour or 12 hour.");
    cui_row(ui, .gap = 12) {
        cui_time_picker(ui, "start", &hour, &minute);
        cui_time_picker(ui, "end", &h2, &m2, .twelve_hour = true, .minute_step = 15);
    }

    section(ui, "Wheels and dial", "Drag, scroll or use the arrow keys. Several wheels make a date reel.");
    cui_row(ui, .gap = 32, .align = CUI_ALIGN_CENTER) {
        static const char *days[31], *months[12], *years[12];
        static char dbuf[31][4], ybuf[12][6];
        static const char *mn[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        for (int k = 0; k < 31; k++) { snprintf(dbuf[k], sizeof dbuf[k], "%d", k + 1); days[k] = dbuf[k]; }
        for (int k = 0; k < 12; k++) { months[k] = mn[k]; snprintf(ybuf[k], sizeof ybuf[k], "%d", 2020 + k); years[k] = ybuf[k]; }
        cui_card(ui, .dir = CUI_ROW, .gap = 0, .pad = 8) {
            cui_wheel_picker(ui, "day", days, 31, &day, .width = CUI_PX(64), .loop = true, .join_right = true);
            cui_wheel_picker(ui, "month", months, 12, &month, .width = CUI_PX(128), .loop = true, .join_left = true, .join_right = true);
            cui_wheel_picker(ui, "year", years, 12, &year, .width = CUI_PX(80), .join_left = true);
        }
        cui_time_dial(ui, "dial", &dh, &dm, 232);
    }

    section(ui, "Ruler and histogram", "A tape that slides under a fixed marker, and a range over its data.");
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START) {
        cui_column(ui, .width = CUI_FILL, .gap = 20) {
            cui_ruler_slider(ui, "Height", &height, 120, 220, .unit = "cm");
            cui_ruler_slider(ui, "Temperature", &temp, 10, 30, .step = 0.5f, .major_every = 4, .spacing = 14, .format = "%.1f", .unit = "\xc2\xb0" "C");
        }
        cui_card(ui, .width = CUI_FILL, .gap = 6) {
            cui_histogram_slider(ui, "Price", bins, 24, &lo, &hi, 0, 240, .format = "$%.0f");
            cui_muted(ui, "1,204 listings in range");
        }
    }
}

/* ---- capture ------------------------------------------------------------- */

static void page_capture(cui_ctx *ui) {
    static cui_texture avatar;
    static char last[256] = "";
    section(ui, "Dropzone", "Drop files from your desktop, or click to browse.");
    int n = cui_dropzone(ui, "files", .description = "PNG, JPG or PDF, up to 25 MB", .accept = "png;jpg;jpeg;pdf");
    if (n > 0) snprintf(last, sizeof last, "%s", cui_dropped_file(ui, 0));
    cui_row(ui, .gap = 12) {
        cui_box(ui, .width = CUI_FILL) { cui_dropzone(ui, "compact", .compact = true, .title = "Attach a file", .description = "Any type"); }
        cui_box(ui, .width = CUI_FILL) { cui_dropzone(ui, "images", .compact = true, .title = "Add images", .description = "PNG or JPG", .icon = CUI_ICON_IMAGE, .accept = "png;jpg;jpeg"); }
    }
    if (last[0]) cui_muted(ui, last);

    section(ui, "Avatar upload", "Drop an image on the avatar.");
    cui_row(ui, .gap = 20, .align = CUI_ALIGN_CENTER) {
        cui_avatar_upload(ui, "avatar", "Ada Lovelace", &avatar, 88);
        cui_column(ui, .gap = 4) {
            cui_label(ui, "Profile photo");
            cui_secondary(ui, "At least 256 by 256 pixels.");
        }
    }

    section(ui, "Signature", "Draw with the mouse or a pen. Faster strokes are thinner, like ink.");
    cui_box(ui, .width = CUI_PX(520)) { cui_signature_pad(ui, "signature", .height = 180); }
}

/* ---- registry ------------------------------------------------------------ */

typedef struct { const char *name, *icon; void (*fn)(cui_ctx *ui); } more_page;

static const more_page pages[] = {
    { "Actions", CUI_ICON_SPARKLES, page_actions },
    { "Smart inputs", CUI_ICON_SQUARE_PEN, page_inputs },
    { "Pickers", CUI_ICON_CALENDAR, page_pickers },
    { "Capture", CUI_ICON_UPLOAD, page_capture },
};

int gallery_more_count(void) { return (int)(sizeof pages / sizeof pages[0]); }
const char *gallery_more_name(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].name : ""; }
const char *gallery_more_icon(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].icon : NULL; }
void gallery_more_page(cui_ctx *ui, int page) { if (page >= 0 && page < gallery_more_count()) pages[page].fn(ui); }
