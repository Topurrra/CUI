/* Extended gallery pages. Each page keeps its own state in statics. */
#include "gallery_more.h"

#include <stdio.h>
#include <stdlib.h>
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


/* ---- disclosure and feedback ---------------------------------------------- */

static void page_disclosure(cui_ctx *ui) {
    static bool banner = true;
    static bool card1 = true, card2 = false;
    static cui_check_step steps[] = {
        { "Create your workspace", "Pick a name and invite your team.", "Create", true },
        { "Connect a repository", "Link GitHub or GitLab to sync issues and pull requests.", "Connect", false },
        { "Import your data", "Bring tasks over from a CSV or another tool.", "Import", false },
        { "Turn on notifications", "Get pinged when something needs you.", "Enable", false },
    };
    static int deleted;
    if (cui_banner(ui, "CUI 1.1 is out: data grid, kanban and forty new components.", &banner, .action = "Read the notes", .dismissible = true))
        cui_toast(ui, "Opening release notes");
    if (!banner && cui_button(ui, "Show the banner again", .size = CUI_SIZE_SM)) banner = true;

    section(ui, "Hover card", "Rest the pointer on a name.");
    cui_row(ui, .gap = 6) {
        cui_text(ui, "Assigned to");
        cui_link(ui, "@grace");
        cui_hover_card(ui, "grace-card", .width = 300) {
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_START) {
                cui_avatar(ui, "Grace Hopper", .size = 44, .status = "online");
                cui_column(ui, .gap = 4, .width = CUI_FILL) {
                    cui_text(ui, "Grace Hopper", .font = CUI_FONT_MEDIUM);
                    cui_text(ui, "Compilers, Cobol and very small nanoseconds. Joined March 2024.", .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_row(ui, .gap = 6, .pad_top = 4) {
                        cui_icon(ui, CUI_ICON_CALENDAR, 13, CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, "Usually replies within an hour", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
        }
        cui_text(ui, "and due Friday.");
    }

    section(ui, "Confirm popover", "Ask right where the action is.");
    cui_row(ui, .gap = 12) {
        if (cui_button(ui, "Delete branch", .icon = CUI_ICON_TRASH_2)) cui_popover_open(ui, "confirm-delete");
        if (cui_confirm_popover(ui, "confirm-delete", .title = "Delete feature/grid?", .description = "The branch and its 12 commits are removed from the remote.",
                                .confirm_label = "Delete", .danger = true)) deleted++;
        if (deleted) cui_muted(ui, "Branch deleted");
    }

    section(ui, "Expandable text", "Long text clamps to a few lines with a fade.");
    cui_expandable_text(ui, "about",
        "CUI is a component library for C. It draws everything itself, so an app looks the same on Windows, macOS and Linux, and it "
        "ships the parts most apps need: inputs, pickers, menus, dialogs, tables, charts and a command palette. Every component "
        "supports the keyboard, light and dark themes and reduced motion. The core is plain C99 with no dependencies, and it "
        "renders through OpenGL or a built-in software rasterizer, so it also runs in virtual machines and over remote desktop.",
        .lines = 2);

    section(ui, "Expandable cards", "A header that opens a body with a spring.");
    cui_column(ui, .gap = 10) {
        cui_expandable_card(ui, "Billing", &card1, .description = "Pro plan, renews on December 1", .icon = CUI_ICON_CREDIT_CARD, .badge = "Active", .badge_tone = CUI_TONE_SUCCESS) {
            cui_row(ui, .gap = 16) {
                cui_metric(ui, "This month", "$84.00", .width = CUI_FILL);
                cui_metric(ui, "Seats", "7 of 10", .width = CUI_FILL);
            }
            cui_row(ui, .gap = 8) { cui_button(ui, "Change plan", .size = CUI_SIZE_SM); cui_button(ui, "Invoices", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST); }
        }
        cui_expandable_card(ui, "Security", &card2, .description = "Two-factor authentication is on", .icon = CUI_ICON_SHIELD) {
            cui_text(ui, "Recovery codes were generated 3 months ago.", .color = CUI_COLOR_TEXT_SECONDARY);
        }
    }

    section(ui, "Countdown", "Digits roll as the time passes.");
    {
        static double target;
        if (target == 0) target = cui_time(ui) + 2 * 86400 + 5 * 3600 + 42 * 60 + 18;
        cui_row(ui, .gap = 24, .align = CUI_ALIGN_CENTER) {
            cui_countdown(ui, "launch", target - cui_time(ui));
            cui_countdown(ui, "sale", target - cui_time(ui) - 86400 * 2, .compact = true, .hide_days = true);
        }
    }

    section(ui, "Onboarding checklist", "Progress, with the next step open.");
    cui_box(ui, .width = CUI_PX(460)) {
        int a = cui_checklist(ui, "onboarding", steps, 4, .title = "Set up your workspace");
        if (a >= 0) { steps[a].done = true; cui_toast(ui, steps[a].title, .tone = CUI_TONE_SUCCESS); }
    }

    section(ui, "Undo toast", "A draining line shows how long Undo stays.");
    if (cui_button(ui, "Archive 3 conversations", .icon = CUI_ICON_ARCHIVE)) cui_toast(ui, "3 conversations archived", .action = "Undo", .show_timer = true);
}

/* ---- docs page with a table of contents ---------------------------------- */

static void page_reader(cui_ctx *ui) {
    static const cui_toc_item toc[] = {
        { "Introduction", "intro", 0 }, { "Install", "install", 0 }, { "With CMake", "cmake", 1 }, { "Without CMake", "nocmake", 1 },
        { "First window", "window", 0 }, { "Immediate mode", "imm", 0 }, { "Layout", "layout", 0 }, { "Next steps", "next", 0 },
    };
    static const char *para = "Components are functions. They draw themselves, handle input and return what happened. Your variables are the "
                              "source of truth, so there is nothing to keep in sync, and conditional interface is just an if statement. ";
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_scroll(ui, .id = "reader", .height = CUI_PX(560), .width = CUI_FILL, .gap = 14) {
            for (int k = 0; k < 8; k++) {
                cui_toc_anchor(ui, toc[k].anchor);
                cui_heading(ui, toc[k].title, toc[k].level ? 4 : 3);
                for (int p = 0; p < 3; p++) { cui_push_id_int(ui, k * 10 + p); cui_text(ui, para, .color = CUI_COLOR_TEXT_SECONDARY); cui_pop_id(ui); }
            }
            cui_gap(ui, 200);
        }
        cui_column(ui, .width = CUI_PX(200), .gap = 10) {
            cui_text(ui, "On this page", .size = 12, .font = CUI_FONT_MEDIUM, .color = CUI_COLOR_TEXT_MUTED);
            cui_toc(ui, "reader", toc, 8);
        }
    }
}


/* ---- data grid and code ---------------------------------------------------- */

typedef struct { char name[48]; char email[64]; const char *team; int commits; float score; } person;
#define PEOPLE 2000
static person people[PEOPLE];
static int order_ix[PEOPLE];
static bool people_sel[PEOPLE];
static cui_table_sort people_sort;

static int cmp_people(const void *a, const void *b) {
    const person *x = &people[*(const int *)a], *y = &people[*(const int *)b];
    int r = 0;
    switch (people_sort.column) {
    case 0: r = strcmp(x->name, y->name); break;
    case 2: r = strcmp(x->team, y->team); break;
    case 3: r = x->commits - y->commits; break;
    default: r = x->score < y->score ? -1 : x->score > y->score; break;
    }
    return people_sort.descending ? -r : r;
}

static void page_grid(cui_ctx *ui) {
    static bool init;
    static cui_table_sort last_sort = { -1, false };
    static const char *first[] = { "Ada", "Alan", "Grace", "Linus", "Margaret", "Katherine", "Hedy", "Dennis", "Barbara", "Edsger", "Frances", "John" };
    static const char *last[] = { "Lovelace", "Turing", "Hopper", "Torvalds", "Hamilton", "Johnson", "Lamarr", "Ritchie", "Liskov", "Dijkstra", "Allen", "Backus" };
    static const char *teams[] = { "Engine", "Design", "Platform", "Docs", "Growth" };
    if (!init) {
        for (int k = 0; k < PEOPLE; k++) {
            snprintf(people[k].name, sizeof people[k].name, "%s %s", first[k % 12], last[(k * 7 + 3) % 12]);
            snprintf(people[k].email, sizeof people[k].email, "%s.%d@cui.dev", first[k % 12], k);
            for (char *c = people[k].email; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
            people[k].team = teams[(k * 3) % 5];
            people[k].commits = (k * 37) % 900 + 12;
            people[k].score = (float)((k * 53) % 1000) / 10.0f;
            order_ix[k] = k;
        }
        people_sort.column = 3; people_sort.descending = true;
        init = true;
    }
    if (people_sort.column != last_sort.column || people_sort.descending != last_sort.descending) {
        qsort(order_ix, PEOPLE, sizeof(int), cmp_people);
        last_sort = people_sort;
    }
    static const char *const team_opts[] = { "Engine", "Design", "Platform", "Docs", "Growth" };
    static const char *const status_opts[] = { "Active", "Away", "Invited" };
    static const char *const role_opts[] = { "Owner", "Admin", "Member" };
    static const cui_filter_field fields[] = {
        { "Team", CUI_ICON_USERS, team_opts, 5 },
        { "Status", CUI_ICON_CIRCLE_DOT, status_opts, 3 },
        { "Role", CUI_ICON_KEY_ROUND, role_opts, 3 },
    };
    static uint32_t filters[3] = { 1u << 0 | 1u << 2, 0, 0 };
    static char search[64];
    section(ui, "Filter bar", "Chips for active filters, values in a list, and a search.");
    cui_filter_bar(ui, "filters", fields, 3, filters, search, sizeof search);

    section(ui, "Data grid", "2,000 rows, virtualized. Sort, resize columns, select with Shift and Ctrl, edit names with a double click.");
    static const cui_grid_column cols[] = {
        { "Name", 200, CUI_TEXT_LEFT, true }, { "Email", 196, CUI_TEXT_LEFT, false }, { "Team", 110, CUI_TEXT_LEFT, true },
        { "Commits", 96, CUI_TEXT_RIGHT, true }, { "Score", 130, CUI_TEXT_RIGHT, true },
    };
    int n_sel = 0;
    for (int k = 0; k < PEOPLE; k++) n_sel += people_sel[k];
    int f0, l0;
    if (cui_data_grid_begin(ui, "people", cols, 5, PEOPLE, &f0, &l0, .height = CUI_PX(420), .selected = people_sel, .sort = &people_sort)) {
        for (int r = f0; r < l0; r++) {
            person *p = &people[order_ix[r]];
            cui_data_grid_row(ui, r);
            cui_data_grid_cell(ui);
            cui_avatar(ui, p->name, .size = 24);
            cui_data_grid_edit(ui, p->name, sizeof p->name);
            cui_data_grid_cell_text(ui, p->email);
            cui_data_grid_cell(ui);
            cui_badge(ui, p->team, .dot = true, .tone = (cui_tone)(1 + (p->team[0] % 7)));
            {
                char buf[16];
                snprintf(buf, sizeof buf, "%d", p->commits);
                cui_data_grid_cell_text(ui, buf);
            }
            cui_data_grid_cell(ui);
            cui_progress(ui, p->score / 100, .width = CUI_PX(70), .height = 5);
            {
                char buf[16];
                snprintf(buf, sizeof buf, "%.1f", (double)p->score);
                cui_text(ui, buf, .tabular = true, .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
            }
        }
        cui_data_grid_end(ui);
    }
    cui_muted(ui, n_sel ? "Rows selected. Press Esc to clear." : "Click a row, Shift+click to select a range, Ctrl+A for all.");

    section(ui, "Tree table", "Tree cells inside a table: disclosure, indentation, totals.");
    {
        static bool open_src = true, open_ui = false;
        cui_table_column tc[] = { { "Name", CUI_FILL, CUI_TEXT_LEFT, false }, { "Files", CUI_PX(80), CUI_TEXT_RIGHT, false }, { "Size", CUI_PX(100), CUI_TEXT_RIGHT, false } };
        cui_table(ui, "tree", tc, 3) {
            cui_table_row(ui, false);
            cui_table_cell(ui); cui_tree_cell(ui, "src", 0, true, &open_src, CUI_ICON_FOLDER);
            cui_table_cell_text(ui, "42"); cui_table_cell_text(ui, "1.2 MB");
            if (open_src) {
                cui_table_row(ui, false);
                cui_table_cell(ui); cui_tree_cell(ui, "ui", 1, true, &open_ui, CUI_ICON_FOLDER);
                cui_table_cell_text(ui, "18"); cui_table_cell_text(ui, "640 KB");
                if (open_ui) {
                    cui_table_row(ui, false);
                    cui_table_cell(ui); cui_tree_cell(ui, "button.c", 2, false, NULL, CUI_ICON_FILE);
                    cui_table_cell_text(ui, "1"); cui_table_cell_text(ui, "24 KB");
                }
                cui_table_row(ui, false);
                cui_table_cell(ui); cui_tree_cell(ui, "main.c", 1, false, NULL, CUI_ICON_FILE);
                cui_table_cell_text(ui, "1"); cui_table_cell_text(ui, "8 KB");
            }
            cui_table_row(ui, false);
            cui_table_cell(ui); cui_tree_cell(ui, "README.md", 0, false, NULL, CUI_ICON_FILE_TEXT);
            cui_table_cell_text(ui, "1"); cui_table_cell_text(ui, "12 KB");
        }
    }
}

static void page_code(cui_ctx *ui) {
    section(ui, "Code block", "Highlighting for C, C++, JavaScript, TypeScript, Python, Rust, Go, JSON and shell.");
    cui_code_block(ui, "c-sample",
        "#include <cui/cui.h>\n"
        "\n"
        "/* A counter with a button */\n"
        "static int clicks = 0;\n"
        "\n"
        "void frame(cui_ctx *ui) {\n"
        "    if (cui_button(ui, \"Click me\", .variant = CUI_BUTTON_PRIMARY)) clicks++;\n"
        "    cui_textf(ui, \"Clicked %d times\", clicks);\n"
        "    float ratio = clicks / 10.0f;   // grows with each click\n"
        "}\n",
        .lang = "c", .title = "counter.c", .line_numbers = true, .highlight_from = 7);
    cui_row(ui, .gap = 16, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_box(ui, .width = CUI_GROW(1)) {
            cui_code_block(ui, "py", "def fib(n: int) -> int:\n    # the slow way, on purpose\n    if n < 2:\n        return n\n    return fib(n - 1) + fib(n - 2)\n\nprint([fib(k) for k in range(10)])\n",
                           .lang = "python", .title = "fib.py");
        }
        cui_box(ui, .width = CUI_GROW(1)) {
            cui_code_block(ui, "sh", "git clone https://github.com/yappologistic/CUI.git\ncmake -S CUI -B build\ncmake --build build\n./build/examples/gallery  # run it\n",
                           .lang = "shell");
        }
    }
    section(ui, "JSON viewer", "Collapsible, with counts. Click a brace to fold it.");
    cui_json_view(ui, "json",
        "{\"name\": \"cui\", \"version\": \"1.1.0\", \"private\": false, \"downloads\": 128400,\n"
        " \"platforms\": [\"windows\", \"macos\", \"linux\"],\n"
        " \"renderer\": {\"backend\": \"opengl\", \"fallback\": \"software\", \"vsync\": true, \"msaa\": null},\n"
        " \"authors\": [{\"name\": \"Ada\", \"commits\": 412}, {\"name\": \"Grace\", \"commits\": 388}]}",
        .expand_depth = 2);
    cui_json_view(ui, "broken", "{\"ok\": true, \"oops\": [1, 2,}");
}

/* ---- registry ------------------------------------------------------------ */

typedef struct { const char *name, *icon; void (*fn)(cui_ctx *ui); } more_page;

static const more_page pages[] = {
    { "Actions", CUI_ICON_SPARKLES, page_actions },
    { "Smart inputs", CUI_ICON_SQUARE_PEN, page_inputs },
    { "Pickers", CUI_ICON_CALENDAR, page_pickers },
    { "Capture", CUI_ICON_UPLOAD, page_capture },
    { "Disclosure", CUI_ICON_LAYERS, page_disclosure },
    { "Reader", CUI_ICON_FILE_TEXT, page_reader },
    { "Data grid", CUI_ICON_GRID_3X3, page_grid },
    { "Code", CUI_ICON_CODE, page_code },
};

int gallery_more_count(void) { return (int)(sizeof pages / sizeof pages[0]); }
const char *gallery_more_name(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].name : ""; }
const char *gallery_more_icon(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].icon : NULL; }
void gallery_more_page(cui_ctx *ui, int page) { if (page >= 0 && page < gallery_more_count()) pages[page].fn(ui); }
