/* Metrics dashboard: sidebar, top bar, KPIs, charts and a table. */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    int nav;
    int range;
    char search[64];
    int page;
    cui_table_sort sort;
    int selected;
    bool dark;
    bool palette;
} app_state;

typedef struct { const char *id, *customer, *email, *date, *status; int tone; float amount; } order;

static const order orders[] = {
    { "#3021", "Nora Lindqvist", "nora@lindqvist.se", "Nov 5", "Paid", CUI_TONE_SUCCESS, 184.20f },
    { "#3020", "Kwame Asante", "kwame@asante.co", "Nov 5", "Pending", CUI_TONE_WARNING, 42.75f },
    { "#3019", "Hana Novak", "hana@novak.cz", "Nov 4", "Paid", CUI_TONE_SUCCESS, 318.40f },
    { "#3018", "Felix Moreau", "felix@moreau.fr", "Nov 4", "Refunded", CUI_TONE_DANGER, 96.00f },
    { "#3017", "Iris Coleman", "iris@coleman.org", "Nov 3", "Paid", CUI_TONE_SUCCESS, 471.15f },
    { "#3016", "Ravi Shah", "ravi@shah.in", "Nov 3", "Paid", CUI_TONE_SUCCESS, 1236.80f },
};

static void sidebar(cui_ctx *ui, app_state *s) {
    static const char *items[] = { "Overview", "Orders", "Customers", "Products", "Analytics", "Settings" };
    static const char *icons[] = { CUI_ICON_LAYOUT_DASHBOARD, CUI_ICON_SHOPPING_CART, CUI_ICON_USERS, CUI_ICON_PACKAGE,
                                   CUI_ICON_CHART_BAR, CUI_ICON_SETTINGS };
    static const char *badges[] = { NULL, "12", NULL, NULL, NULL, NULL };
    cui_column(ui, .width = CUI_PX(232), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED, .pad = 12, .gap = 2) {
        cui_row(ui, .gap = 10, .pad_x = 8, .pad_top = 6, .pad_bottom = 16) {
            cui_box(ui, .width = CUI_PX(28), .height = CUI_PX(28), .radius = 9, .bg = CUI_COLOR_ACCENT, .dir = CUI_STACK,
                    .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
                cui_icon(ui, CUI_ICON_ZAP, 15, CUI_COLOR_ACCENT_FOREGROUND);
            }
            cui_text(ui, "Northwind", .font = CUI_FONT_MEDIUM, .size = 15, .nowrap = true);
        }
        for (int i = 0; i < 6; i++)
            if (cui_nav_item(ui, items[i], .icon = icons[i], .selected = s->nav == i, .badge = badges[i])) s->nav = i;
        cui_spacer(ui);
        cui_card(ui, .pad = 16, .gap = 10) {
            cui_text(ui, "Next payout", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            cui_text(ui, "$4,812.30", .font = CUI_FONT_DISPLAY, .size = 20, .tabular = true);
            cui_text(ui, "To the account ending 4417 on Nov 8.", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
        }
    }
}

static void topbar(cui_ctx *ui, app_state *s) {
    cui_row(ui, .gap = 12, .pad_x = 32, .pad_y = 14, .width = CUI_FILL) {
        cui_search(ui, "search", s->search, sizeof s->search, .placeholder = "Search orders, customers", .width = CUI_PX(320));
        cui_spacer(ui);
        if (cui_icon_button(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_ICON_SUN : CUI_ICON_MOON, .tooltip = "Toggle theme")) {
            s->dark = cui_get_theme_mode(ui) != CUI_THEME_DARK;
            cui_set_theme(ui, s->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
        }
        cui_icon_button(ui, CUI_ICON_BELL, .tooltip = "Notifications");
        cui_menu(ui, "Account", .icon = CUI_ICON_CIRCLE_USER, .variant = CUI_BUTTON_GHOST, .placement = CUI_PLACE_BOTTOM_END) {
            cui_menu_label(ui, "nora@northwind.co");
            cui_menu_item(ui, "Profile", .icon = CUI_ICON_USER);
            cui_menu_item(ui, "Billing", .icon = CUI_ICON_CREDIT_CARD);
            cui_menu_item(ui, "Keyboard shortcuts", .icon = CUI_ICON_KEYBOARD, .shortcut = "?");
            cui_menu_separator(ui);
            cui_menu_item(ui, "Log out", .icon = CUI_ICON_LOG_OUT);
        }
    }
}

static void content(cui_ctx *ui, app_state *s) {
    static const float rev[] = { 31, 34, 33, 38, 42, 40, 46, 52, 49, 55, 61, 64 };
    static const float users[] = { 120, 132, 128, 140, 151, 149, 162, 170, 168, 181, 190, 204 };
    static const float conv[] = { 3.1f, 3.0f, 3.2f, 3.4f, 3.3f, 3.5f, 3.6f, 3.4f, 3.7f, 3.8f, 3.9f, 4.1f };
    static const float churn[] = { 2.4f, 2.3f, 2.5f, 2.2f, 2.1f, 2.2f, 2.0f, 1.9f, 2.0f, 1.8f, 1.8f, 1.7f };
    static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

    cui_row(ui, .gap = 12) {
        cui_column(ui, .gap = 4, .width = CUI_FILL) {
            cui_heading(ui, "Good morning, Nora", 2);
            cui_secondary(ui, "Here is what happened in your store this month.");
        }
        const char *ranges[] = { "7 days", "30 days", "12 months" };
        cui_segmented(ui, "range", ranges, 3, &s->range, .size = CUI_SIZE_SM);
        cui_button(ui, "Export", .icon = CUI_ICON_DOWNLOAD, .size = CUI_SIZE_SM);
    }
    cui_box(ui, .dir = CUI_GRID, .columns = 4, .gap = 16) {
        cui_metric(ui, "Revenue", "$64,210", .delta = 12.4f, .spark = rev, .spark_count = 12, .icon = CUI_ICON_DOLLAR_SIGN);
        cui_metric(ui, "Customers", "2,040", .delta = 7.3f, .spark = users, .spark_count = 12, .icon = CUI_ICON_USERS);
        cui_metric(ui, "Conversion", "4.1%", .delta = 0.4f, .delta_label = "+0.4 pts", .spark = conv, .spark_count = 12, .icon = CUI_ICON_TARGET);
        cui_metric(ui, "Churn", "1.7%", .delta = -0.6f, .delta_label = "-0.6 pts", .lower_is_better = true, .spark = churn, .spark_count = 12, .icon = CUI_ICON_TRENDING_DOWN);
    }
    cui_row(ui, .gap = 16, .align = CUI_ALIGN_STRETCH) {
        cui_card(ui, .gap = 16, .width = CUI_GROW(2)) {
            cui_row(ui) {
                cui_column(ui, .gap = 2, .width = CUI_FILL) {
                    cui_label(ui, "Revenue");
                    cui_text(ui, "Compared with last year", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
                }
                cui_badge(ui, "+18%", .tone = CUI_TONE_SUCCESS, .icon = CUI_ICON_TRENDING_UP);
            }
            static const float last[] = { 24, 26, 27, 29, 31, 30, 34, 36, 35, 39, 41, 44 };
            cui_series series[2] = { { "This year", rev, 12, { 0 } }, { "Last year", last, 12, { 0 } } };
            cui_line_chart(ui, "rev", series, 2, .labels = months, .label_count = 12, .area = true, .smooth = true,
                           .height = 230, .format = "$%.0fk");
        }
        cui_card(ui, .gap = 24, .width = CUI_GROW(1)) {
            cui_label(ui, "Traffic sources");
            static const float src[] = { 42, 26, 18, 14 };
            static const char *names[] = { "Search", "Direct", "Social", "Email" };
            cui_donut_chart(ui, "src", src, names, 4, .height = 150, .format = "%.0f%%");
        }
    }
    cui_card(ui, .gap = 14) {
        cui_row(ui) {
            cui_column(ui, .gap = 2, .width = CUI_FILL) {
                cui_label(ui, "Recent orders");
                cui_text(ui, "6 orders need your attention", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
            }
            cui_button(ui, "View all", .variant = CUI_BUTTON_GHOST, .icon_right = CUI_ICON_ARROW_RIGHT, .size = CUI_SIZE_SM);
        }
        cui_table_column cols[] = {
            { "Order", CUI_PX(90), CUI_TEXT_LEFT, true },
            { "Customer", CUI_FILL, CUI_TEXT_LEFT, true },
            { "Date", CUI_PX(90), CUI_TEXT_LEFT, false },
            { "Status", CUI_PX(120), CUI_TEXT_LEFT, true },
            { "Amount", CUI_PX(110), CUI_TEXT_RIGHT, true },
        };
        cui_table(ui, "orders", cols, 5, .sort = &s->sort, .selectable = true) {
            for (int i = 0; i < 6; i++) {
                const order *o = &orders[i];
                if (s->search[0] && !strstr(o->customer, s->search) && !strstr(o->id, s->search)) continue;
                if (cui_table_row(ui, s->selected == i)) s->selected = i;
                cui_table_cell_text(ui, o->id);
                cui_table_cell(ui);
                cui_avatar(ui, o->customer, .size = 28);
                cui_column(ui, .gap = 0, .width = CUI_FILL) {
                    cui_text(ui, o->customer, .font = CUI_FONT_MEDIUM, .nowrap = true);
                    cui_text(ui, o->email, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .nowrap = true);
                }
                cui_table_cell_text(ui, o->date);
                cui_table_cell(ui);
                cui_badge(ui, o->status, .tone = (cui_tone)o->tone, .dot = true);
                cui_table_cell_textf(ui, "$%.2f", (double)o->amount);
            }
        }
        cui_row(ui) {
            cui_text(ui, "Showing 1 to 6 of 128 orders", .size = 12, .color = CUI_COLOR_TEXT_SECONDARY, .width = CUI_FILL);
            cui_pagination(ui, "pages", &s->page, 22);
        }
    }
}

static void frame(cui_ctx *ui, void *user) {
    app_state *s = user;
    if (cui_shortcut(ui, CUI_MOD_PRIMARY, CUI_KEY_K)) s->palette = true;
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .align = CUI_ALIGN_STRETCH, .bg = CUI_COLOR_BACKGROUND) {
        sidebar(ui, s);
        cui_separator(ui);
        cui_column(ui, .width = CUI_FILL) {
            topbar(ui, s);
            cui_separator(ui);
            cui_scroll(ui, .id = "content", .pad = 32, .gap = 24, .fade_edges = true) {
                content(ui, s);
            }
        }
    }
    static const cui_command cmds[] = {
        { "Create order", CUI_ICON_PLUS, "Actions", "Ctrl N" },
        { "Add customer", CUI_ICON_USER_PLUS, "Actions", NULL },
        { "Export report", CUI_ICON_DOWNLOAD, "Actions", NULL },
        { "Go to analytics", CUI_ICON_CHART_BAR, "Navigation", NULL },
    };
    int c = cui_command_palette(ui, &s->palette, cmds, 4);
    if (c >= 0) cui_toast(ui, cmds[c].title, .tone = CUI_TONE_SUCCESS);
}

int main(void) {
    static app_state s = { .sort = { 0, true }, .selected = 0, .range = 1 };
    return cui_app_run(&(cui_app_desc){ .title = "Northwind Dashboard", .width = 1360, .height = 900, .accent = CUI_ACCENT_BLUE }, frame, &s);
}
