/* Text the components show: English by default, or an app's table (cui_set_strings).
 * Only the components that show dates link this file. */
#include "cui_internal.h"

static const cui_strings cui__english = {
    { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" },
    { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" },
    { "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday" },
    { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" },
    { "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" },
    "Today",
};

void cui_set_strings(cui_ctx *ui, const cui_strings *strings) { ui->strings = strings; }
const cui_strings *cui_get_strings(cui_ctx *ui) { return ui->strings; }

/* the app's entry when it set one, else English; i is wrapped into range */
#define CUI__STR(field, n, i) \
    int k = ((i) % (n) + (n)) % (n); \
    const char *s = ui->strings ? ui->strings->field[k] : NULL; \
    return s ? s : cui__english.field[k]

const char *cui__month_name(cui_ctx *ui, int m) { CUI__STR(month, 12, m); }
const char *cui__month_short(cui_ctx *ui, int m) { CUI__STR(month_short, 12, m); }
const char *cui__weekday_name(cui_ctx *ui, int d) { CUI__STR(weekday, 7, d); }
const char *cui__weekday_short(cui_ctx *ui, int d) { CUI__STR(weekday_short, 7, d); }
const char *cui__weekday_min(cui_ctx *ui, int d) { CUI__STR(weekday_min, 7, d); }
const char *cui__today_text(cui_ctx *ui) { return ui->strings && ui->strings->today ? ui->strings->today : cui__english.today; }

/* a byte count: "512 bytes", "4.2 KB", "18 MB" (powers of 1000) */
const char *cui__size_text(cui_ctx *ui, double v) {
    if (v < 1000) return cui__frame_printf(ui, "%.0f bytes", v);
    static const char *u[] = { "KB", "MB", "GB", "TB" };
    int k = 0;
    v /= 1000;   /* at least 1 KB here */
    while (v >= 1000 && k < 3) { v /= 1000; k++; }
    return cui__frame_printf(ui, v < 10 ? "%.1f %s" : "%.0f %s", v, u[k]);
}
