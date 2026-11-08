/* Extended gallery pages. Each page keeps its own state in statics. */
#include "gallery_more.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void section(cui_ctx *ui, const char *title, const char *desc) {
    /* more room above a heading than below it, so it reads as the start of what follows */
    cui_column(ui, .gap = 4, .pad_top = 18) {
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
    cui_row(ui, .gap = 12, .wrap = true) {
        static const char *items[] = { "Save as draft", "Save and publish", "Schedule\xe2\x80\xa6" };
        static const char *icons[] = { CUI_ICON_FILE_TEXT, CUI_ICON_SEND, CUI_ICON_CLOCK };
        int r = cui_split_button(ui, "Save", items, 3, .item_icons = icons, .icon = CUI_ICON_SAVE);
        if (r >= 0) split_choice = r;
        cui_split_button(ui, "Export", items, 3, .variant = CUI_BUTTON_OUTLINE, .icon = CUI_ICON_DOWNLOAD);
        if (split_choice >= 0) cui_muted(ui, split_choice == 0 ? "Saved" : items[split_choice - 1]);
    }

    section(ui, "Toggle group", "Independent toggles for toolbars, or exclusive like radio buttons.");
    cui_row(ui, .gap = 12, .wrap = true) {
        static const char *labels[] = { "Bold", "Italic", "Underline", "Code" };
        static const char *icons[] = { CUI_ICON_BOLD, CUI_ICON_ITALIC, CUI_ICON_UNDERLINE, CUI_ICON_CODE };
        cui_toggle_group(ui, "format", labels, 4, fmt, .icons = icons, .icon_only = true);
        static const char *alabels[] = { "Left", "Center", "Right" };
        static const char *aicons[] = { CUI_ICON_ALIGN_LEFT, CUI_ICON_LIST, CUI_ICON_MENU };
        cui_toggle_group(ui, "align", alabels, 3, align, .icons = aicons, .exclusive = true);
    }

    section(ui, "Morphing buttons", "Confirm before destroying, and show the result of work in place.");
    cui_row(ui, .gap = 12, .wrap = true) {
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
    cui_row(ui, .gap = 16, .wrap = true) {
        cui_theme_switcher(ui, &theme_mode, true);
        cui_theme_toggle(ui);
    }

    section(ui, "User menu", "Account trigger for sidebars and headers.");
    cui_box(ui, .width = CUI_PX(260)) {
        cui_user_menu(ui, "Maya Okafor", .detail = "maya@fieldwork.studio", .status = "online", .placement = CUI_PLACE_BOTTOM_START) {
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
        static const char *titles[] = { "Review the design tokens", "Reply to Jonas about the release", "Book the offsite venue" };
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
    static char owner[64] = "Jonas Berg";
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
    cui_row(ui, .gap = 12, .wrap = true) {
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
    cui_row(ui, .gap = 12, .wrap = true) {
        cui_input(ui, "url", url, sizeof url, .prefix = "https://", .width = CUI_PX(260));
        cui_input(ui, "weight", weight, sizeof weight, .suffix = "kg", .width = CUI_PX(120));
    }

    section(ui, "Shortcut recorder", "Click, then press a combination. Backspace clears.");
    cui_row(ui, .gap = 12, .wrap = true) {
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
    cui_row(ui, .gap = 12, .wrap = true) {
        cui_date_range_picker(ui, "range", &range, .width = CUI_PX(300));
        cui_date_range_picker(ui, "stay", &stay, .width = CUI_PX(300), .no_presets = true);
    }
    cui_row(ui) { cui_card(ui, .pad = 16) { cui_range_calendar(ui, "inline-range", &stay, 2); } }   /* as wide as its months */

    section(ui, "Time", "A field with hour and minute columns, 24 hour or 12 hour.");
    cui_row(ui, .gap = 12, .wrap = true) {
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
    cui_row(ui, .gap = 12, .wrap = true) {
        cui_box(ui, .width = CUI_FILL) { cui_dropzone(ui, "compact", .compact = true, .title = "Attach a file", .description = "Any type"); }
        cui_box(ui, .width = CUI_FILL) { cui_dropzone(ui, "images", .compact = true, .title = "Add images", .description = "PNG or JPG", .icon = CUI_ICON_IMAGE, .accept = "png;jpg;jpeg"); }
    }
    if (last[0]) cui_muted(ui, last);

    section(ui, "Avatar upload", "Drop an image on the avatar.");
    cui_row(ui, .gap = 20, .align = CUI_ALIGN_CENTER) {
        cui_avatar_upload(ui, "avatar", "Maya Okafor", &avatar, 88);
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
    cui_row(ui, .gap = 6, .wrap = true) {
        cui_text(ui, "Assigned to");
        cui_link(ui, "@jonas");
        cui_hover_card(ui, "grace-card", .width = 300) {
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_START) {
                cui_avatar(ui, "Jonas Berg", .size = 44, .status = "online");
                cui_column(ui, .gap = 4, .width = CUI_FILL) {
                    cui_text(ui, "Jonas Berg", .font = CUI_FONT_MEDIUM);
                    cui_text(ui, "Engineer on the rendering team. Joined March 2024.", .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
                    cui_row(ui, .gap = 6, .pad_top = 4, .wrap = true) {
                        cui_icon(ui, CUI_ICON_CALENDAR, 12, CUI_COLOR_TEXT_MUTED);
                        cui_text(ui, "Usually replies within an hour", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                    }
                }
            }
        }
        cui_text(ui, "and due Friday.");
    }

    section(ui, "Confirm popover", "Ask right where the action is.");
    cui_row(ui, .gap = 12, .wrap = true) {
        if (cui_button(ui, "Delete branch", .icon = CUI_ICON_TRASH_2)) cui_popover_open(ui, "confirm-delete");
        if (cui_confirm_popover(ui, "confirm-delete", .title = "Delete feature/grid?", .description = "The branch and its 12 commits are removed from the remote.",
                                .confirm_label = "Delete branch", .danger = true)) deleted++;
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
            /* plain label and value pairs: a card inside the card would only add edges */
            cui_row(ui, .gap = 40, .wrap = true) {
                cui_column(ui, .gap = 2) { cui_muted(ui, "This month"); cui_text(ui, "$84.00", .font = CUI_FONT_DISPLAY, .size = 22, .tabular = true); }
                cui_column(ui, .gap = 2) { cui_muted(ui, "Seats"); cui_text(ui, "7 of 10", .font = CUI_FONT_DISPLAY, .size = 22, .tabular = true); }
            }
            cui_row(ui, .gap = 8, .wrap = true) { cui_button(ui, "Change plan", .size = CUI_SIZE_SM); cui_button(ui, "Invoices", .size = CUI_SIZE_SM, .variant = CUI_BUTTON_GHOST); }
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
        cui_scroll(ui, .id = "reader", .height = CUI_PX(560), .width = CUI_FILL, .gap = 14, .fade_edges = true) {
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
    static const char *first[] = { "Maya", "Priya", "Jonas", "Lena", "Sam", "Tomas", "Aiko", "Daniel",
                                    "Rosa", "Omar", "Elif", "Ben", "Nora", "Kwame", "Hana", "Felix" };
    static const char *last[] = { "Okafor", "Nair", "Berg", "Fischer", "Whitfield", "Rivera", "Tanaka", "Mensah",
                                   "Delgado", "Haddad", "Kaya", "Carter", "Lindqvist", "Asante", "Novak", "Moreau" };
    static const char *teams[] = { "Engine", "Design", "Platform", "Docs", "Growth" };
    if (!init) {
        for (int k = 0; k < PEOPLE; k++) {
            /* a cheap hash, so neighbouring rows do not repeat each other */
            uint32_t h = (uint32_t)k * 2654435761u;
            h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
            const char *fn = first[h % 16], *ln = last[(h >> 4) % 16];
            snprintf(people[k].name, sizeof people[k].name, "%s %s", fn, ln);
            snprintf(people[k].email, sizeof people[k].email, "%s.%c%u@cui.dev", fn, ln[0], (h >> 8) % 90 + 10);
            for (char *c = people[k].email; *c; c++) if (*c >= 'A' && *c <= 'Z') *c += 32;
            people[k].team = teams[(h >> 12) % 5];
            people[k].commits = (int)((h >> 16) % 1400) + 12;
            people[k].score = (float)((h >> 3) % 1000) / 10.0f;
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
            cui_badge(ui, p->team);
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
        " \"authors\": [{\"name\": \"Maya\", \"commits\": 412}, {\"name\": \"Jonas\", \"commits\": 388}]}",
        .expand_depth = 2);
    cui_json_view(ui, "broken", "{\"ok\": true, \"oops\": [1, 2,}");
}


/* ---- conversation and media ------------------------------------------------- */

static void make_scene(unsigned char *px, int w, int h, bool color) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float fx = (float)x / w, fy = (float)y / h;
            float r = 0.56f + fy * 0.34f, g = 0.72f + fy * 0.2f, b = 0.88f + fy * 0.06f;   /* clear daytime sky */
            float sun = (fx - 0.72f) * (fx - 0.72f) * 15.4f + (fy - 0.24f) * (fy - 0.24f) * 6.0f;   /* round at a 1.6 aspect */
            if (sun < 0.024f) { r = 1.0f; g = 0.97f; b = 0.86f; }
            float m1 = 0.5f + 0.1f * sinf(fx * 6.0f + 0.6f) + 0.03f * sinf(fx * 23.0f);
            float m2 = 0.68f + 0.06f * sinf(fx * 4.0f + 2.1f) + 0.02f * sinf(fx * 31.0f);
            if (fy > m1) { r = 0.5f; g = 0.6f; b = 0.62f - fy * 0.08f; }        /* far hills in haze */
            if (fy > m2) { r = 0.3f - (fy - m2) * 0.2f; g = 0.45f - (fy - m2) * 0.25f; b = 0.3f; }   /* near fields */
            if (!color) { float l = r * 0.3f + g * 0.59f + b * 0.11f; l = l * 0.85f + 0.05f; r = g = b = l; }
            unsigned char *p = px + (y * w + x) * 4;
            p[0] = (unsigned char)(fminf(1, fmaxf(0, r)) * 255); p[1] = (unsigned char)(fminf(1, fmaxf(0, g)) * 255);
            p[2] = (unsigned char)(fminf(1, fmaxf(0, b)) * 255); p[3] = 255;
        }
}

static void page_social(cui_ctx *ui) {
    static int likes[3] = { 12, 3, 0 };
    static bool liked[3] = { true, false, false };
    static int slide = 1;
    static cui_texture before, after;
    static float split = 0.45f;
    static int deck = 0;
    static double revenue = 48210;
    static const cui_presence_user who[] = {
        { "Maya Okafor", { 0x7c, 0x5c, 0xff, 255 }, 0, false }, { "Jonas Berg", { 0x22, 0xc5, 0x8b, 255 }, 0, false },
        { "Priya Nair", { 0xf5, 0x9e, 0x0b, 255 }, 0, false }, { "Lena Fischer", { 0x38, 0xbd, 0xf8, 255 }, 0, true },
        { "Aiko Tanaka", { 0xec, 0x48, 0x99, 255 }, 0, false }, { "Daniel Mensah", { 0x10, 0xb9, 0x81, 255 }, 0, false },
        { "Rosa Delgado", { 0xa8, 0x55, 0xf7, 255 }, 0, false },
    };
    /* textures belong to one context: a flag in its state store says whether this one has them */
    bool *made = cui_state(ui, cui_make_id(ui, "cmp-textures"), sizeof *made);
    if (!*made) {
        *made = true;
        static unsigned char a[640 * 400 * 4], b[640 * 400 * 4];
        make_scene(a, 640, 400, false);
        make_scene(b, 640, 400, true);
        before = cui_image_create(ui, 640, 400, a);
        after = cui_image_create(ui, 640, 400, b);
    }
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Chat", "Grouped bubbles, read receipts and a typing indicator.");
            cui_card(ui, .gap = 0, .pad = 16) {
                cui_chat_divider(ui, "Today");
                cui_chat_message(ui, "Did the data grid make it into 1.1?", .author = "Jonas Berg", .time = "9:41", .last_in_group = false);
                cui_chat_message(ui, "I need it for the billing page.", .author = "Jonas Berg", .grouped = true, .last_in_group = true);
                cui_chat_message(ui, "It did. 2,000 rows scroll at 60 fps on my laptop.", .mine = true);
                cui_chat_message(ui, "Shift and Ctrl clicks select like a spreadsheet.", .mine = true, .grouped = true, .last_in_group = true, .status = "Read", .time = "9:43");
                cui_typing_indicator(ui, "Jonas");
            }
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Comments", "Threads with replies, likes and badges.");
            cui_card(ui, .gap = 4, .pad = 14) {
                cui_comment(ui, "c1", "Tomas Rivera", "The new numbers match the staging run. I would still test the import on a large file before Friday.", .time = "2h", .likes = &likes[0], .liked = &liked[0], .badge = "Author");
                cui_comment(ui, "c2", "Priya Nair", "Agreed. I have a 2 GB export we can use.", .time = "1h", .depth = 1, .likes = &likes[1], .liked = &liked[1]);
                cui_comment(ui, "c3", "Maya Okafor", "Scheduled it for 3 pm.", .time = "40m", .depth = 1, .likes = &likes[2], .liked = &liked[2], .edited = true);
            }
        }
    }
    section(ui, "Presence", "Who is here now. Hover to spread them out.");
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_CENTER) {
        cui_presence(ui, "here", who, 7, 5, 30);
        cui_counter(ui, "rev", revenue, .format = "$%.0f", .size = 34, .group_thousands = true);
        if (cui_button(ui, "Add a sale", .size = CUI_SIZE_SM, .icon = CUI_ICON_PLUS)) revenue += 1287;
    }

    section(ui, "Ticker", "A continuous strip of figures. It pauses under the pointer.");
    static const cui_ticker_item tick[] = {
        { "Revenue", "$48.2k", 6.9f }, { "Active users", "12,408", 3.1f }, { "Churn", "1.7%", -0.6f, true }, { "Latency", "182 ms", -8.4f, true },
        { "Builds", "1,204", 12.0f }, { "Uptime", "99.98%", 0 },
    };
    cui_ticker(ui, "tick", tick, 6);

    section(ui, "Carousel", "Drag, flick, scroll sideways or use the arrows and dots.");
    cui_carousel(ui, "tour", 4, &slide, .height = 200, .auto_advance = 6) {
        static const char *titles[] = { "Faster exports", "ARM64 runners", "Shortcuts everywhere", "Smaller downloads" };
        static const char *descs[] = { "A 900 file project now exports in 40 seconds, down from four minutes.",
                                       "Linux and macOS jobs can run on ARM64 machines in every plan.",
                                       "Every action in the editor has a key, listed under Help, Shortcuts.",
                                       "The desktop app is 38 MB lighter after dropping the bundled browser." };
        static const unsigned tops[] = { 0x1f2a37, 0x0f3d3e, 0x3a2e22, 0x2b2f3a };
        for (int k = 0; k < 4; k++) cui_carousel_slide(ui) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_RGB(tops[k]), .radius = 16, .pad = 28, .gap = 8, .justify = CUI_JUSTIFY_END) {
                cui_text(ui, titles[k], .font = CUI_FONT_DISPLAY, .size = 24, .color = CUI_RGB(0xffffff));
                cui_text(ui, descs[k], .color = CUI_RGBA(0xffffffc0));
            }
        }
    }

    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Before and after", "Drag the divider.");
            cui_image_compare(ui, "cmp", before, after, &split, .height = 230, .aspect = 640.0f / 400.0f);
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10, .align = CUI_ALIGN_CENTER) {
            section(ui, "Card stack", "Flick the top card away, left or right.");
            static const char *names[] = { "Kyoto", "Lisbon", "Reykjavik", "Oaxaca", "Hanoi" };
            static const unsigned cols[] = { 0xc8553d, 0x2d3047, 0x5b8e7d, 0xb7791f, 0x1f3a5f };
            cui_card_stack_begin(ui, "deck", 3, &(cui_card_stack_opts){ .width = CUI_PX(260), .height = 200 });
            for (int k = 0; k < 3; k++) {
                int idx = (deck + k) % 5;
                cui_card_stack_item(ui, k) {
                    cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_RGB(cols[idx]), .radius = 25, .pad = 20, .justify = CUI_JUSTIFY_END, .gap = 4) {
                        cui_text(ui, names[idx], .font = CUI_FONT_DISPLAY, .size = 22, .color = CUI_RGB(0xffffff));
                        cui_text(ui, "Swipe right to save", .size = 13, .font = CUI_FONT_MEDIUM, .color = CUI_RGBA(0xfffffff0));
                    }
                }
            }
            int dir = cui_card_stack_end(ui);
            if (dir) { deck = (deck + 1) % 5; cui_toast(ui, dir > 0 ? "Saved" : "Skipped"); }
        }
    }
}

/* ---- planning ------------------------------------------------------------ */

typedef struct { int id; const char *title, *tag, *who; cui_tone tone; int comments, files; float progress; } kb_card;

static kb_card kb_cards[] = {
    { 1, "Audit the color tokens for contrast", "Design", "Maya Okafor", CUI_TONE_VIOLET, 3, 0, 0 },
    { 2, "Virtualize the activity feed", "Perf", "Jonas Berg", CUI_TONE_TEAL, 1, 2, 0 },
    { 3, "Dark mode screenshots for docs", "Docs", "Aiko Tanaka", CUI_TONE_ROSE, 0, 4, 0 },
    { 4, "Keyboard nav in the date range picker", "A11y", "Priya Nair", CUI_TONE_INFO, 5, 0, 0.6f },
    { 5, "IME composition on Windows", "Platform", "Daniel Mensah", CUI_TONE_WARNING, 2, 1, 0.3f },
    { 6, "Spring presets", "Motion", "Rosa Delgado", CUI_TONE_VIOLET, 4, 0, 1 },
    { 7, "Glyph atlas eviction", "Perf", "Lena Fischer", CUI_TONE_TEAL, 0, 0, 1 },
    { 8, "Retina screenshots", "Docs", "Aiko Tanaka", CUI_TONE_ROSE, 1, 3, 1 },
};
static int kb_cols[4][8] = { { 1, 2, 3 }, { 4, 5 }, { 6 }, { 7, 8 } };
static int kb_len[4] = { 3, 2, 1, 2 };

static kb_card *kb_find(int id) {
    for (size_t k = 0; k < sizeof kb_cards / sizeof kb_cards[0]; k++) if (kb_cards[k].id == id) return &kb_cards[k];
    return NULL;
}

static void kb_apply(const cui_kanban_move *m) {
    int *from = kb_cols[m->from_column], *to = kb_cols[m->to_column];
    int at = -1;
    for (int k = 0; k < kb_len[m->from_column]; k++) if (from[k] == m->card) at = k;
    if (at < 0 || (m->to_column != m->from_column && kb_len[m->to_column] >= 8)) return;
    for (int k = at; k < kb_len[m->from_column] - 1; k++) from[k] = from[k + 1];
    kb_len[m->from_column]--;
    int idx = m->to_index < 0 ? 0 : m->to_index > kb_len[m->to_column] ? kb_len[m->to_column] : m->to_index;
    for (int k = kb_len[m->to_column]; k > idx; k--) to[k] = to[k - 1];
    to[idx] = m->card;
    kb_len[m->to_column]++;
}

static void page_planning(cui_ctx *ui) {
    static const char *col_names[4] = { "Backlog", "In progress", "Review", "Done" };
    static const unsigned col_dots[4] = { 0x94a3b8, 0x3b82f6, 0xf59e0b, 0x22c55e };
    section(ui, "Kanban", "Drag cards within and between columns. Columns can carry a work-in-progress limit.");
    cui_kanban_begin(ui, "board");
    for (int c = 0; c < 4; c++) {
        if (cui_kanban_column_begin(ui, col_names[c], kb_len[c], .dot = CUI_RGB(col_dots[c]), .limit = c == 1 ? 3 : 0, .width = 264))
            cui_toast(ui, "New card", .description = col_names[c]);
        for (int k = 0; k < kb_len[c]; k++) {
            kb_card *cd = kb_find(kb_cols[c][k]);
            if (!cd) continue;
            if (cui_kanban_card_begin(ui, cd->id)) cui_toast(ui, cd->title);
            cui_badge(ui, cd->tag, .tone = cd->tone, .size = CUI_SIZE_SM);
            cui_text(ui, cd->title, .font = CUI_FONT_MEDIUM, .size = 13.5f, .line_height = 1.35f);
            if (cd->progress > 0 && cd->progress < 1) cui_progress(ui, cd->progress, .height = 4);
            cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                cui_avatar(ui, cd->who, .size = 22);
                cui_spacer(ui);
                static const char *nums[] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };
                if (cd->files) { cui_icon(ui, CUI_ICON_PAPERCLIP, 12, CUI_COLOR_TEXT_MUTED); cui_text(ui, nums[cd->files % 10], .size = 12, .color = CUI_COLOR_TEXT_MUTED); }
                if (cd->comments) { cui_icon(ui, CUI_ICON_MESSAGE_SQUARE, 12, CUI_COLOR_TEXT_MUTED); cui_text(ui, nums[cd->comments % 10], .size = 12, .color = CUI_COLOR_TEXT_MUTED); }
                if (cd->progress >= 1) cui_icon(ui, CUI_ICON_CIRCLE_CHECK, 14, CUI_COLOR_SUCCESS);
            }
            cui_kanban_card_end(ui);
        }
        cui_kanban_column_end(ui);
    }
    cui_kanban_move mv;
    if (cui_kanban_end(ui, &mv)) kb_apply(&mv);

    section(ui, "Week calendar", "Drag events to move them in 15 minute steps, or pull the bottom edge to resize.");
    static cui_cal_event events[] = {
        { "Standup", "Zoom", 0, 9.5f, 9.75f, CUI_TONE_INFO }, { "Design review", "Room 2", 0, 11, 12.5f, CUI_TONE_VIOLET },
        { "Lunch with Maya", NULL, 1, 12.5f, 13.5f, CUI_TONE_SUCCESS }, { "Standup", "Zoom", 1, 9.5f, 9.75f, CUI_TONE_INFO },
        { "Release planning", "Room 1", 2, 10, 11.5f, CUI_TONE_WARNING }, { "1:1 Jonas", NULL, 2, 10.5f, 11, CUI_TONE_ROSE },
        { "Focus time", "No meetings", 2, 14, 17, CUI_TONE_TEAL }, { "Standup", "Zoom", 3, 9.5f, 9.75f, CUI_TONE_INFO },
        { "Customer call", "Northlight", 3, 15, 16, CUI_TONE_DANGER }, { "Demo day", "All hands", 4, 16, 17.5f, CUI_TONE_VIOLET },
        { "Gym", NULL, 5, 10, 11.5f, CUI_TONE_SUCCESS },
    };
    cui_date today = cui_date_today();
    int td = cui_date_to_days(today);
    int monday = td - ((td + 3) % 7 + 7) % 7;
    int ev = cui_week_calendar(ui, "week", cui_date_from_days(monday), events, (int)(sizeof events / sizeof events[0]), .start_hour = 8, .end_hour = 19, .height = CUI_PX(420));
    if (ev >= 0) cui_toast(ui, events[ev].title, .description = events[ev].detail);

    section(ui, "Gantt", "Drag bars to reschedule, pull either end to change the length. Arrows follow dependencies.");
    static cui_gantt_task tasks[] = {
        { "Research", "Maya Okafor", 0, 5, 1, CUI_TONE_TEAL, -1, false },
        { "Wireframes", "Aiko Tanaka", 4, 10, 1, CUI_TONE_TEAL, 0, false },
        { "Design system", "Rosa Delgado", 8, 17, 0.7f, CUI_TONE_TEAL, 1, false },
        { "Core engine", "Jonas Berg", 6, 20, 0.55f, CUI_TONE_INFO, 0, false },
        { "Platform backends", "Daniel Mensah", 14, 26, 0.2f, CUI_TONE_TEAL, 3, false },
        { "Beta", NULL, 27, 27, 0, CUI_TONE_WARNING, 4, true },
        { "Docs and launch", "Priya Nair", 28, 36, 0, CUI_TONE_SUCCESS, 5, false },
    };
    cui_date origin = cui_date_from_days(monday - 7);
    int tk = cui_gantt(ui, "plan", tasks, (int)(sizeof tasks / sizeof tasks[0]), origin, 42, .today = 7 + (td - monday));
    if (tk >= 0) {
        static char dur[32];
        snprintf(dur, sizeof dur, "%d days", tasks[tk].end - tasks[tk].start);
        cui_toast(ui, tasks[tk].name, .description = tasks[tk].milestone ? "Milestone" : dur);
    }
}

/* ---- more charts --------------------------------------------------------- */

static void page_viz(cui_ctx *ui) {
    static float traffic[7 * 24];
    static float days[364];
    static bool init;
    if (!init) {
        for (int d = 0; d < 7; d++)
            for (int h = 0; h < 24; h++) {
                float work = (d < 5) ? 1.0f : 0.45f;
                float day = expf(-powf((h - 13.5f) / 4.2f, 2)) * 90 * work + expf(-powf((h - 20.5f) / 2.0f, 2)) * 35;
                traffic[d * 24 + h] = day + (float)((d * 31 + h * 17) % 11);
            }
        unsigned s = 7;
        for (int k = 0; k < 364; k++) {
            s = s * 1103515245u + 12345u;
            int wd = (k + ((cui_date_to_days(cui_date_today()) - 363) % 7 + 10) % 7) % 7;
            float base = (wd >= 5 ? 0.6f : 3.2f) * (0.6f + 0.4f * sinf(k / 40.0f) * sinf(k / 40.0f));
            float r = (float)((s >> 16) % 1000) / 1000.0f;
            days[k] = r < 0.18f ? 0 : floorf(base * r * 4);
        }
        init = true;
    }
    static const char *wd[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    static const char *hours[24] = { "0h", NULL, NULL, "3h", NULL, NULL, "6h", NULL, NULL, "9h", NULL, NULL, "12h", NULL, NULL, "15h", NULL, NULL, "18h", NULL, NULL, "21h", NULL, NULL };
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Heatmap", "Requests by weekday and hour.");
            cui_card(ui, .pad = 18, .width = CUI_FILL) { cui_heatmap(ui, "traffic", traffic, 7, 24, .row_labels = wd, .col_labels = hours, .format = "%.0f req"); }
        }
    }
    section(ui, "Calendar heatmap", "A year of activity, one square a day.");
    cui_card(ui, .pad = 18) {
        cui_date start = cui_date_from_days(cui_date_to_days(cui_date_today()) - 363);
        cui_calendar_heatmap(ui, "activity", start, days, 364, .unit = "contributions");
    }
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1.3f), .gap = 10) {
            section(ui, "Treemap", "Bundle size by module.");
            static const cui_viz_item mods[] = {
                { "Layout", 55 }, { "Text", 42 }, { "Draw", 35 }, { "Controls", 31 }, { "Charts", 23 },
                { "Overlays", 19 }, { "Platform", 16 }, { "Theme", 9 }, { "Icons", 7 }, { "Grid", 6 },
            };
            cui_treemap(ui, "bundle", mods, 10, .height = 280, .format = "%.0f KB");
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10, .align = CUI_ALIGN_CENTER) {
            section(ui, "Radar", "Two releases across six qualities.");
            static const float a[] = { 82, 64, 90, 58, 75, 88 }, b[] = { 70, 85, 72, 80, 60, 79 };
            static const char *axes[] = { "Speed", "Size", "Polish", "Docs", "Platforms", "Tests" };
            cui_series rs[] = { { "1.0", a, 6, { 0 } }, { "1.1", b, 6, { 0 } } };
            cui_radar_chart(ui, "radar", rs, 2, axes, 6, .size = 220, .max = 100);
        }
    }
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Funnel", "Sign up conversion.");
            static const cui_viz_item stages[] = { { "Visited", 12840 }, { "Signed up", 4210 }, { "Activated", 2380 }, { "Subscribed", 860 } };
            cui_funnel(ui, "funnel", stages, 4);
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Split bar", "Where the time goes.");
            static const cui_viz_item parts[] = { { "Layout", 38 }, { "Text", 24 }, { "Paint", 21 }, { "Input", 9 }, { "Other", 8 } };
            cui_split_bar(ui, "split", parts, 5, .format = "%.0f ms");
            cui_gap(ui, 18);
            section(ui, "Waffle", "Platforms of the last 100 installs.");
            static const cui_viz_item plats[] = { { "Windows", 46 }, { "macOS", 31 }, { "Linux", 23 } };
            cui_waffle(ui, "waffle", plats, 3, .cell = 14);
        }
    }
    section(ui, "Sankey", "Where sign ups come from and where they end up.");
    static const char *nodes[] = { "Search", "Social", "Referral", "Sign up", "Bounced", "Trial", "Paid", "Churned" };
    static const cui_sankey_link links[] = {
        { 0, 3, 420 }, { 1, 3, 260 }, { 2, 3, 180 }, { 0, 4, 300 }, { 1, 4, 220 },
        { 3, 5, 860 }, { 5, 6, 580 }, { 5, 7, 280 },
    };
    cui_card(ui, .pad = 18, .width = CUI_FILL) { cui_sankey(ui, "flow", nodes, 8, links, 8, .height = 280); }
    cui_row(ui, .gap = 24, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1.4f), .gap = 10) {
            section(ui, "Bar list", "Ranked, keyed by label: press Shuffle and the rows slide to their new rank.");
            static cui_viz_item pages[] = { { "/docs", 4820 }, { "/", 3920 }, { "/gallery", 2410 }, { "/blog/fast-ui-in-c", 1830 }, { "/download", 1210 }, { "/pricing", 640 } };
            cui_bar_list(ui, "pages", pages, 6);
            if (cui_button(ui, "Shuffle", .size = CUI_SIZE_SM, .icon = CUI_ICON_SHUFFLE, .variant = CUI_BUTTON_OUTLINE))
                for (int k = 0; k < 6; k++) pages[k].value = (float)(500 + rand() % 5000);
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10, .align = CUI_ALIGN_CENTER) {
            section(ui, "Activity rings", "Past one, a ring keeps winding.");
            static const cui_viz_item rings[] = { { "Move", 0.82f }, { "Exercise", 1.24f }, { "Stand", 0.58f } };
            cui_activity_rings(ui, "rings", rings, 3, .size = 170, .center = "82%", .caption = "of goal");
        }
    }
    section(ui, "Scatter", "Frame time against node count, sized by draw calls.");
    {
        static cui_vec2 a[40], b[40];
        static float sa[40], sb[40];
        static bool pinit;
        if (!pinit) {
            for (int k = 0; k < 40; k++) {
                float n = 200 + k * 95.0f;
                a[k] = (cui_vec2){ n, 0.08f + n * 0.00006f + (float)((k * 37) % 13) * 0.004f };
                b[k] = (cui_vec2){ n * 0.9f + 150, 0.15f + n * 0.00011f + (float)((k * 53) % 17) * 0.006f };
                sa[k] = (float)(10 + (k * 7) % 40); sb[k] = (float)(20 + (k * 11) % 50);
            }
            pinit = true;
        }
        cui_scatter_series ss[] = { { "Software", a, sa, 40, { 0 } }, { "OpenGL", b, sb, 40, { 0 } } };
        cui_card(ui, .pad = 18, .width = CUI_FILL) {
            cui_scatter_chart(ui, "scatter", ss, 2, .height = 300, .x_label = "Nodes", .y_label = "Frame time (ms)", .y_format = "%.2f");
        }
    }
}

/* ---- motion -------------------------------------------------------------- */

static void page_motion(cui_ctx *ui) {
    static int replay;
    static float peaks[96];
    static float progress = 0.36f;
    static bool wifi = true, bt = false, focus = true, airdrop = false, dark_tile = true;
    static int tab = 0;
    static cui_now_playing np = { "Low Tide", "Marlow Avenue", 0, { 0xc8, 0x55, 0x3d, 255 }, 76, 222, true };
    if (!peaks[0]) for (int k = 0; k < 96; k++) peaks[k] = 0.18f + 0.8f * fabsf(sinf(k * 0.37f) * sinf(k * 0.11f + 1.3f));
    section(ui, "Text effects", "A shimmer for pending work, a typewriter for streamed text, words that settle into place.");
    cui_card(ui, .gap = 18, .pad = 20, .width = CUI_FILL) {
        cui_row(ui, .gap = 10, .align = CUI_ALIGN_CENTER) {
            cui_loader(ui, .style = CUI_LOADER_DOTS, .size = 22);
            cui_shimmer_text(ui, "Thinking about your question\xe2\x80\xa6", .size = 15);
        }
        cui_push_id_int(ui, replay);
        cui_typewriter(ui, "tw", "CUI draws a full frame in a fraction of a millisecond, so text can stream in as fast as it arrives.", .caret = true, .size = 15);
        cui_text_reveal(ui, "rv", "The release build finished in 3 minutes 42 seconds.", .size = 20, .font = CUI_FONT_DISPLAY);
        cui_pop_id(ui);
        cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER) {
            cui_text(ui, "Deploying to", .font = CUI_FONT_DISPLAY, .size = 26);
            static const char *words[] = { "staging.", "eu-west.", "us-east.", "all regions." };
            cui_word_rotate(ui, "rot", words, 4, 2.2f, .font = CUI_FONT_DISPLAY, .size = 26);
        }
        if (cui_button(ui, "Replay", .size = CUI_SIZE_SM, .icon = CUI_ICON_ROTATE_CCW, .variant = CUI_BUTTON_OUTLINE)) replay++;
    }
    section(ui, "Gradient text and spotlight cards", "A light follows the pointer across a card and along its edge. A beam marks the one card that is live.");
    cui_gradient_text(ui, "Kiln 2.4", .font = CUI_FONT_DISPLAY, .size = 34, .animate = true);
    cui_row(ui, .gap = 14, .align = CUI_ALIGN_STRETCH, .width = CUI_FILL) {
        static const char *titles[] = { "Software", "OpenGL 3.3", "Your own" };
        static const char *blurbs[] = { "Plain C into a pixel buffer. No GPU needed.", "The default on desktop. Deploying now.", "The core emits triangles; bring any GPU API." };
        for (int k = 0; k < 3; k++) {
            cui_push_id_int(ui, k);
            cui_spotlight(ui, "spot", .width = CUI_GROW(1), .beam = k == 1) {
                cui_text(ui, titles[k], .font = CUI_FONT_MEDIUM, .size = 15);
                cui_text(ui, blurbs[k], .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_pop_id(ui);
        }
    }
    section(ui, "Loaders", "Dots, bars, pulse, orbit and grid.");
    cui_row(ui, .gap = 36, .align = CUI_ALIGN_CENTER) {
        for (int k = 0; k < 5; k++) cui_loader(ui, .style = (cui_loader_style)k, .size = 28);
    }
    section(ui, "Dock", "Icons grow as the pointer passes.");
    cui_row(ui, .width = CUI_FILL, .justify = CUI_JUSTIFY_CENTER, .height = CUI_PX(110), .align = CUI_ALIGN_END) {
        cui_dock(ui, "dock") {
            if (cui_dock_item(ui, CUI_ICON_FOLDER, "Files", .color = CUI_RGB(0x3b82f6), .running = true)) cui_toast(ui, "Files");
            cui_dock_item(ui, CUI_ICON_MAIL, "Mail", .color = CUI_RGB(0x0ea5e9), .badge = 3, .running = true);
            cui_dock_item(ui, CUI_ICON_CALENDAR, "Calendar", .color = CUI_RGB(0xef4444));
            cui_dock_item(ui, CUI_ICON_MUSIC, "Music", .color = CUI_RGB(0xec4899), .running = true);
            cui_dock_item(ui, CUI_ICON_SQUARE_TERMINAL, "Terminal", .color = CUI_RGB(0x27272a));
            cui_dock_item(ui, CUI_ICON_PALETTE, "Design", .color = CUI_RGB(0x8b5cf6));
            cui_dock_separator(ui);
            cui_dock_item(ui, CUI_ICON_SETTINGS, "Settings");
            cui_dock_item(ui, CUI_ICON_TRASH_2, "Trash");
        }
    }
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Control tiles", "Big toggles for quick settings.");
            cui_box(ui, .dir = CUI_GRID, .columns = 2, .gap = 10, .width = CUI_PX(300)) {
                cui_control_tile(ui, "Wi-Fi", &wifi, .icon = CUI_ICON_WIFI, .detail = wifi ? "Home" : "Off");
                cui_control_tile(ui, "Hotspot", &bt, .icon = CUI_ICON_RADIO);
                cui_control_tile(ui, "Focus", &focus, .icon = CUI_ICON_MOON, .detail = focus ? "Until 6 pm" : "Off");
                cui_control_tile(ui, "AirShare", &airdrop, .icon = CUI_ICON_SHARE_2, .detail = airdrop ? "Everyone" : "Contacts only");
            }
            cui_control_tile(ui, "Dark appearance", &dark_tile, .icon = CUI_ICON_SUN, .wide = true, .width = CUI_PX(300));
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 10) {
            section(ui, "Now playing", "Artwork, a scrubber and transport.");
            int ev = cui_now_playing(ui, "np", &np);
            if (ev & CUI_PLAYER_NEXT) cui_toast(ui, "Next track");
            if (np.playing) { np.position += cui_delta_time(ui); if (np.position > np.duration) np.position = 0; }
            section(ui, "Waveform", "Click or drag to seek.");
            cui_card(ui, .pad = 14, .gap = 8, .width = CUI_PX(360)) {
                cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                    cui_icon_button(ui, CUI_ICON_PLAY, .size = CUI_SIZE_SM, .variant = CUI_BUTTON_SECONDARY);
                    cui_box(ui, .width = CUI_FILL) { cui_waveform(ui, "wave", peaks, 96, &progress, .height = 34); }
                    cui_text(ui, "0:42", .size = 12, .tabular = true, .color = CUI_COLOR_TEXT_MUTED);
                }
            }
            section(ui, "Tab bar", "For compact layouts.");
            static const cui_tab_bar_item tabs[] = { { CUI_ICON_HOUSE, "Home", 0 }, { CUI_ICON_SEARCH, "Search", 0 }, { CUI_ICON_INBOX, "Inbox", 4 }, { CUI_ICON_USER, "Profile", 0 } };
            cui_tab_bar(ui, "tabs", tabs, 4, &tab, .width = CUI_PX(360));
        }
    }
}

/* ---- AI ------------------------------------------------------------------ */

static void page_ai(cui_ctx *ui) {
    static char draft[512] = "";
    static int model = 0, vote = 0, orb_state = 1, removed = -1;
    static bool busy = false;
    static double started = -1;
    static const char *files[] = { "quarterly-report.pdf", "metrics.csv" };
    static int file_count = 2;
    static const cui_model models[] = {
        { "Atlas", "Longer answers, reads files and code", "New", CUI_ICON_LAYERS },
        { "Atlas Fast", "Short answers in about a second", NULL, CUI_ICON_ZAP },
        { "Atlas Mini", "Runs on your own machine", NULL, CUI_ICON_BOX },
    };
    static const cui_source sources[] = {
        { "Immediate mode GUIs and why they keep coming back", "https://fieldnotes.dev/immediate-mode", "A look at retained and immediate mode toolkits and the trade offs between them." },
        { "Notes on subpixel text positioning", "https://glyphlab.io/notes/subpixel", "Shaping, hinting and subpixel positioning explained with examples." },
        { "Spring physics for interface motion", "https://motionbook.dev/springs", "Why spring physics feel more natural than cubic beziers for interface motion." },
    };
    double now = cui_time(ui);
    if (busy && started < 0) started = now;
    if (!busy) started = -1;
    float elapsed = busy ? (float)(now - started) : 0;
    if (busy && elapsed > 9) busy = false;
    cui_row(ui, .gap = 28, .align = CUI_ALIGN_START, .width = CUI_FILL) {
        cui_column(ui, .width = CUI_GROW(1.6f), .gap = 14) {
            section(ui, "Conversation", "Reasoning, tool calls, an answer with sources, and the composer.");
            cui_chat_message(ui, "Why do immediate mode toolkits feel so fast? Cite a few sources.", .mine = true);
            cui_reasoning(ui, "think", .thinking = busy, .seconds = busy ? elapsed : 8) {
                cui_text(ui, "The question is about perceived speed. Two parts: how little work a frame does, and how quickly input shows up on screen.", .size = 13.5f, .color = CUI_COLOR_TEXT_SECONDARY);
                cui_text(ui, "Look for sources on retained versus immediate mode, text rendering cost and motion.", .size = 13.5f, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_tool_call(ui, "search", "web_search", .icon = CUI_ICON_GLOBE, .detail = "immediate mode gui performance", .status = CUI_TASK_DONE, .seconds = 1.4f,
                          .input = "{\"query\": \"immediate mode gui performance\", \"limit\": 5}",
                          .output = "{\"results\": [{\"title\": \"Immediate mode GUIs\", \"url\": \"https://fieldnotes.dev/immediate-mode\"}], \"took_ms\": 1380}");
            cui_tool_call(ui, "read", "read_file", .icon = CUI_ICON_FILE_TEXT, .detail = "quarterly-report.pdf", .status = busy ? CUI_TASK_RUNNING : CUI_TASK_DONE, .seconds = busy ? elapsed : 2.1f);
            if (busy) cui_agent_status(ui, "Reading quarterly-report.pdf", .status = CUI_TASK_RUNNING, .seconds = elapsed);
            cui_typewriter(ui, "answer", "They do very little work per frame: the interface is rebuilt from your data, laid out once and drawn in a handful of batches, so there is no tree of objects to keep in sync. Text is the expensive part, which is why glyphs are cached in an atlas, and motion feels quick when it is driven by springs instead of fixed curves.", .size = 15);
            cui_row(ui, .gap = 6, .align = CUI_ALIGN_CENTER) {
                cui_text(ui, "Sources", .size = 12, .color = CUI_COLOR_TEXT_MUTED);
                for (int k = 0; k < 3; k++) cui_citation(ui, k + 1, &sources[k]);
            }
            cui_source_list(ui, "sources", sources, 3);
            cui_row(ui, .gap = 8, .align = CUI_ALIGN_CENTER, .width = CUI_FILL) {
                int ra = cui_response_actions(ui, "actions", .copy_text = "They do very little work per frame.", .vote = &vote);
                if (ra & CUI_RESPONSE_REGENERATE) busy = true;
                cui_spacer(ui);
                static const cui_viz_item parts[] = { { "System", 3200 }, { "Files", 41800 }, { "Messages", 68400 } };
                cui_context_meter(ui, "ctx", 113400, 200000, .parts = parts, .part_count = 3);
            }
            int ev = cui_chat_composer(ui, "composer", draft, sizeof draft, .placeholder = "Ask a follow up", .busy = busy,
                                       .attachments = files, .attachment_count = file_count, .removed = &removed,
                                       .models = models, .model_count = 3, .model = &model);
            if (ev & CUI_COMPOSER_SEND) { draft[0] = 0; busy = true; }
            if (ev & CUI_COMPOSER_STOP) busy = false;
            if ((ev & CUI_COMPOSER_REMOVE) && removed >= 0 && removed < file_count) {
                for (int k = removed; k < file_count - 1; k++) files[k] = files[k + 1];
                file_count--;
            }
            if (ev & CUI_COMPOSER_ATTACH) cui_toast(ui, "Attach files", .description = "Drop files on the window, too.");
        }
        cui_column(ui, .width = CUI_GROW(1), .gap = 14) {
            section(ui, "Agent plan", "Steps and their status.");
            cui_plan_step steps[] = {
                { "Search for sources", "3 results", CUI_TASK_DONE },
                { "Read the attached report", busy ? "Page 12 of 40" : "40 pages", busy ? CUI_TASK_RUNNING : CUI_TASK_DONE },
                { "Compare the numbers with last quarter", NULL, busy ? CUI_TASK_PENDING : CUI_TASK_DONE },
                { "Draft the summary", NULL, busy ? CUI_TASK_PENDING : CUI_TASK_RUNNING },
                { "Check the citations", NULL, CUI_TASK_PENDING },
            };
            cui_agent_plan(ui, "plan", steps, 5, .title = "Research plan");
            section(ui, "Voice", "Idle, listening, thinking and speaking.");
            cui_column(ui, .align = CUI_ALIGN_CENTER, .gap = 12, .width = CUI_FILL) {
                float lvl = 0.35f + 0.35f * sinf((float)now * 5.1f) * sinf((float)now * 1.7f);
                if (cui_voice_orb(ui, "orb", (cui_voice_state)orb_state, lvl, .size = 170)) orb_state = orb_state == 1 ? 0 : 1;
                static const char *states[] = { "Idle", "Listening", "Thinking", "Speaking" };
                cui_segmented(ui, "orb-state", states, 4, &orb_state, .size = CUI_SIZE_SM);
            }
            section(ui, "Suggestions", "Ways to start.");
            static const cui_prompt_suggestion sugg[] = {
                { CUI_ICON_CODE, "Explain this code", "Walk through what it does" },
                { CUI_ICON_CHART_BAR, "Analyze a dataset", "Find trends and outliers" },
                { CUI_ICON_PENCIL, "Draft an email", "Clear and friendly" },
                { CUI_ICON_BUG, "Debug an error", "Paste the stack trace" },
            };
            int sg = cui_prompt_suggestions(ui, "sugg", sugg, 4, .columns = 1);
            if (sg >= 0) snprintf(draft, sizeof draft, "%s: ", sugg[sg].title);
        }
    }
}

/* ---- audio ---------------------------------------------------------------- */

static void page_audio(cui_ctx *ui) {
    static float cutoff = 1800, res = 0.3f, attack = 12, pan = 0.15f, faders[6] = { -4, -9, 0, -15, -6, -2 };
    static int tick;
    tick++;
    static const char *names[6] = { "Kick", "Snare", "Bass", "Keys", "Vocals", "FX" };
    static const float marks[] = { 6, 0, -6, -12, -24, -48 };
    float lv[6][2];
    for (int k = 0; k < 6; k++)
        for (int c = 0; c < 2; c++) {
            float t = (tick + c * 5) / 60.0f, beat = fmodf(t * (1.8f + 0.2f * k) + k * 0.31f, 1.0f);
            lv[k][c] = (0.3f + 0.1f * k) * (0.2f + 0.8f * expf(-beat * (2.5f + k)));
        }
    cui_row(ui, .gap = 16, .wrap = true, .align = CUI_ALIGN_START) {
        cui_card(ui, .gap = 16, .min_width = 280) {
            cui_heading(ui, "Synth", 4);
            cui_row(ui, .gap = 14, .wrap = true) {
                cui_knob(ui, "Cutoff", &cutoff, 20, 20000, .unit = "Hz", .log_scale = true, .reset = 1000);
                cui_knob(ui, "Resonance", &res, 0, 1, .format = "%.2f");
                cui_knob(ui, "Attack", &attack, 1, 2000, .unit = "ms", .log_scale = true, .reset = 10);
                cui_knob(ui, "Pan", &pan, -1, 1, .bipolar = true, .format = "%+.2f");
            }
            float master[2] = { lv[0][0] * 1.4f, lv[4][1] * 1.3f };
            cui_level_meter(ui, "master", master, 2, .horizontal = true, .length = 240, .scale = true, .segmented = true);
        }
        cui_card(ui, .dir = CUI_ROW, .gap = 8, .wrap = true) {
            for (int k = 0; k < 6; k++) {
                cui_push_id_int(ui, k);
                cui_fader(ui, names[k], &faders[k], -48, 6, .unit = "dB", .length = 180, .ticks = k == 0 ? marks : NULL,
                          .tick_count = k == 0 ? 6 : 0, .meter = lv[k], .meter_channels = 2);
                cui_pop_id(ui);
            }
        }
    }
}

/* ---- registry ------------------------------------------------------------ */

typedef struct { const char *name, *icon; void (*fn)(cui_ctx *ui); } more_page;

static const more_page pages[] = {
    { "Actions", CUI_ICON_COMMAND, page_actions },
    { "Smart inputs", CUI_ICON_SQUARE_PEN, page_inputs },
    { "Pickers", CUI_ICON_CALENDAR, page_pickers },
    { "Capture", CUI_ICON_UPLOAD, page_capture },
    { "Disclosure", CUI_ICON_LAYERS, page_disclosure },
    { "Reader", CUI_ICON_FILE_TEXT, page_reader },
    { "Data grid", CUI_ICON_GRID_3X3, page_grid },
    { "Code", CUI_ICON_CODE, page_code },
    { "Social and media", CUI_ICON_MESSAGE_SQUARE, page_social },
    { "Planning", CUI_ICON_LIST_CHECKS, page_planning },
    { "More charts", CUI_ICON_CHART_PIE, page_viz },
    { "Motion", CUI_ICON_ACTIVITY, page_motion },
    { "AI", CUI_ICON_MESSAGE_SQUARE, page_ai },
    { "Audio", CUI_ICON_AUDIO_LINES, page_audio },
};

int gallery_more_count(void) { return (int)(sizeof pages / sizeof pages[0]); }
const char *gallery_more_name(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].name : ""; }
const char *gallery_more_icon(int page) { return page >= 0 && page < gallery_more_count() ? pages[page].icon : NULL; }
void gallery_more_page(cui_ctx *ui, int page) {
    if (page < 0 || page >= gallery_more_count()) return;
    cui_push_id(ui, pages[page].name);   /* ids on different pages never meet */
    pages[page].fn(ui);
    cui_pop_id(ui);
}
