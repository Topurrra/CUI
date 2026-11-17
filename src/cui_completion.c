/* cui_completion.c - the completion list and the signature tip a code editor shows for a language server
 * (cui.h, "Completion list and signature tip"). Built from the public pieces (an overlay layer, rows, icons, text,
 * markdown) plus a little state per id: the selected item and the first row in view. */
#include "cui_internal.h"

#include <ctype.h>
#include <string.h>

#define CUI__CP_MAX 4096                 /* items matched and sorted per frame */

typedef struct cui__cp_state {
    int sel;                             /* selected position in the filtered list */
    int top;                             /* first row in view */
    char filter[64];                     /* the filter it was for: a new filter selects the best match again */
} cui__cp_state;

/* how well filter matches label: -1 none; higher is better. A prefix beats a match at word starts beats any
 * subsequence; case matching adds a little. */
static int cui__cp_score(const char *label, const char *filter)
{
    if (!filter || !filter[0]) return 0;
    size_t fl = strlen(filter);
    if (!_strnicmp(label, filter, fl)) return 3000 + (strncmp(label, filter, fl) == 0 ? 100 : 0);   /* among prefix matches the server's order stands (it ranks by relevance) */
    int score = 1000, fi = 0;
    bool word = true;                    /* at the start of a word: the start, after _ or -, a capital after a small letter */
    char prev = 0;
    for (const char *p = label; *p && filter[fi]; p++) {
        char c = *p;
        bool start = p == label || prev == '_' || prev == '-' || prev == '.' || (isupper((unsigned char)c) && islower((unsigned char)prev));
        if (tolower((unsigned char)c) == tolower((unsigned char)filter[fi])) {
            score += start ? 20 : 1;
            if (c == filter[fi]) score += 1;
            fi++;
        } else if (start) {
            word = false;
        }
        prev = c;
    }
    (void)word;
    return filter[fi] ? -1 : score;
}

/* LSP CompletionItemKind (1 based) -> an icon */
static const char *cui__cp_icon(int kind)
{
    switch (kind) {
    case 2: case 3: case 4: return CUI_ICON_BOX;           /* method, function, constructor */
    case 5: case 10: return CUI_ICON_TAG;                  /* field, property */
    case 6: return CUI_ICON_HASH;                          /* variable */
    case 7: case 8: case 22: case 25: return CUI_ICON_BRACES;   /* class, interface, struct, type parameter */
    case 9: return CUI_ICON_LAYERS;                        /* module */
    case 13: case 20: return CUI_ICON_LIST;                /* enum, enum member */
    case 14: return CUI_ICON_KEY_ROUND;                    /* keyword */
    case 15: return CUI_ICON_CODE;                         /* snippet */
    case 17: return CUI_ICON_FILE;
    case 19: return CUI_ICON_FOLDER;
    case 21: return CUI_ICON_ZAP;                          /* constant */
    default: return CUI_ICON_TYPE;                         /* text and the rest */
    }
}

typedef struct { int index, score; } cui__cp_hit;

static void cui__cp_sort(cui__cp_hit *h, int n)
{
    for (int i = 1; i < n; i++) {                          /* insertion sort, stable: the server's order breaks ties */
        cui__cp_hit x = h[i];
        int j = i - 1;
        while (j >= 0 && h[j].score < x.score) { h[j + 1] = h[j]; j--; }
        h[j + 1] = x;
    }
}

cui_completion_result cui_completion_ex(cui_ctx *ui, const char *id_str, const cui_completion_item *items, int count,
                                        const cui_completion_opts *o)
{
    static const cui_completion_opts none;
    if (!o) o = &none;
    cui_completion_result r;
    memset(&r, 0, sizeof r);
    r.accepted = -1;
    r.selected = -1;
    cui_id id = cui_make_id(ui, id_str);
    cui__cp_state *st = cui__state_lazy(ui, id, sizeof *st, true);
    if (!st || !items || count <= 0) { r.closed = count <= 0; return r; }

    /* filter and sort */
    static cui__cp_hit hits[CUI__CP_MAX];
    int n = 0;
    for (int i = 0; i < count && n < CUI__CP_MAX; i++) {
        int s = cui__cp_score(items[i].label ? items[i].label : "", o->filter);
        if (s >= 0) { hits[n].index = i; hits[n].score = s; n++; }
    }
    cui__cp_sort(hits, n);
    r.shown = n;
    const char *f = o->filter ? o->filter : "";
    if (strncmp(st->filter, f, sizeof st->filter - 1)) {   /* typed on: the best match again */
        snprintf(st->filter, sizeof st->filter, "%s", f);
        st->sel = 0;
        st->top = 0;
    }
    if (n == 0) { r.closed = true; return r; }
    int rows = o->max_rows > 0 ? o->max_rows : 10;
    if (rows > n) rows = n;

    /* the keys, before the editor sees them */
    if (!o->no_keys) {
        int mv = (int)ui->kpressed[CUI_KEY_DOWN] - (int)ui->kpressed[CUI_KEY_UP] +
                 rows * ((int)ui->kpressed[CUI_KEY_PAGE_DOWN] - (int)ui->kpressed[CUI_KEY_PAGE_UP]);
        if (mv) {
            st->sel += mv;
            if (st->sel < 0) st->sel = mv < -1 && st->sel > -rows ? 0 : (st->sel % n + n) % n;
            if (st->sel >= n) st->sel = mv > 1 ? n - 1 : st->sel % n;
        }
        if (ui->kpressed[CUI_KEY_ENTER] || ui->kpressed[CUI_KEY_TAB]) r.accepted = hits[st->sel].index;
        if (ui->kpressed[CUI_KEY_ESCAPE]) r.closed = true;
        ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
        ui->kpressed[CUI_KEY_PAGE_UP] = ui->kpressed[CUI_KEY_PAGE_DOWN] = 0;
        ui->kpressed[CUI_KEY_ENTER] = ui->kpressed[CUI_KEY_TAB] = ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
    if (st->sel >= n) st->sel = n - 1;
    if (st->sel < st->top) st->top = st->sel;
    if (st->sel >= st->top + rows) st->top = st->sel - rows + 1;
    if (st->top > n - rows) st->top = n - rows;
    if (st->top < 0) st->top = 0;

    /* where: under the caret's line, or above it when it does not fit below; inside the window sideways */
    float size = o->size > 0 ? o->size : 13.0f;
    float row_h = size + 10.0f, w = o->width > 0 ? o->width : 380.0f;
    float h = (float)rows * row_h + 8.0f;
    float lh = o->line_h > 0 ? o->line_h : size * 1.4f;
    float x = o->x, y = o->y + lh + 2.0f;
    if (y + h > ui->vh - 4.0f && o->y - h - 2.0f >= 4.0f) y = o->y - h - 2.0f;
    if (x + w > ui->vw - 4.0f) x = ui->vw - 4.0f - w;
    if (x < 4.0f) x = 4.0f;

    /* the wheel scrolls the rows; a click picks one */
    cui_rect box = { x, y, w, h };
    bool inside = ui->mx >= box.x && ui->mx < box.x + box.w && ui->my >= box.y && ui->my < box.y + box.h;
    if (inside && ui->scroll_dy != 0) {
        st->top -= (int)(ui->scroll_dy > 0 ? 3 : -3);
        if (st->top > n - rows) st->top = n - rows;
        if (st->top < 0) st->top = 0;
        ui->scroll_dy = 0;
    }
    int hovered = -1;
    if (inside) {
        int k = (int)((ui->my - y - 4.0f) / row_h);
        if (k >= 0 && k < rows) hovered = st->top + k;
        if (hovered >= 0 && ui->mpressed[0]) { st->sel = hovered; r.accepted = hits[hovered].index; ui->mpressed[0] = 0; }
    }
    r.selected = hits[st->sel].index;

    const cui_completion_item *cur = &items[hits[st->sel].index];
    bool doc = !o->no_doc && cur->doc && cur->doc[0];
    float doc_w = doc ? (o->doc_width > 0 ? o->doc_width : 340.0f) : 0;
    bool doc_left = doc && x + w + doc_w + 6 > ui->vw - 4.0f;
    cui_rect layer = { doc_left ? x - doc_w - 6 : x, y, w + (doc ? doc_w + 6 : 0), h };
    if (layer.x < 0) layer.x = 0;
    cui_overlay_begin(ui, id_str, layer);
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL, .gap = 6, .align = CUI_ALIGN_START) {
        if (doc && doc_left) {
            cui_column(ui, .width = CUI_PX(doc_w), .max_height = h, .pad = 8, .bg = CUI_COLOR_SURFACE_RAISED, .border = 1,
                       .radius = 6, .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
                cui_markdown(ui, "doc", cur->doc, .compact = true, .size = size - 1);
            }
        }
        cui_column(ui, .width = CUI_PX(w), .height = CUI_PX(h), .pad = 4, .bg = CUI_COLOR_SURFACE_RAISED, .border = 1, .radius = 6,
                   .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
            for (int k = 0; k < rows; k++) {
                int i = st->top + k;
                const cui_completion_item *it = &items[hits[i].index];
                cui_color bg = i == st->sel ? CUI_COLOR_ACCENT_SUBTLE : i == hovered ? CUI_COLOR_HOVER : (cui_color){ 0 };
                cui_push_id_int(ui, k);
                cui_row(ui, .width = CUI_FILL, .height = CUI_PX(row_h), .pad_x = 6, .gap = 6, .align = CUI_ALIGN_CENTER, .bg = bg, .radius = 4) {
                    cui_icon(ui, cui__cp_icon(it->kind), size, CUI_COLOR_TEXT_SECONDARY);
                    cui_text(ui, it->label ? it->label : "", .size = size, .nowrap = true, .width = CUI_GROW(1), .font = CUI_FONT_MONO);
                    if (it->detail && it->detail[0])
                        cui_text(ui, it->detail, .size = size - 1, .nowrap = true, .color = CUI_COLOR_TEXT_MUTED, .width = CUI_PX(w * 0.4f));
                }
                cui_pop_id(ui);
            }
        }
        if (doc && !doc_left) {
            cui_column(ui, .width = CUI_PX(doc_w), .max_height = h, .pad = 8, .bg = CUI_COLOR_SURFACE_RAISED, .border = 1,
                       .radius = 6, .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
                cui_markdown(ui, "doc", cur->doc, .compact = true, .size = size - 1);
            }
        }
    }
    cui_overlay_end(ui);
    return r;
}

void cui_signature_tip_ex(cui_ctx *ui, const char *id_str, const char *label, int active_start, int active_end,
                          const cui_signature_tip_opts *o)
{
    static const cui_signature_tip_opts none;
    if (!o) o = &none;
    if (!label || !label[0]) return;
    float size = o->size > 0 ? o->size : 13.0f;
    float lh = o->line_h > 0 ? o->line_h : size * 1.4f;
    int len = (int)strlen(label);
    if (active_start < 0 || active_end <= active_start || active_end > len) active_start = active_end = len;
    float w = o->width > 0 ? o->width : 520.0f;
    bool doc = o->doc && o->doc[0];
    float h = size + 16.0f + (doc ? 70.0f : 0.0f);
    float x = o->x, y = o->y - h - 4.0f;                  /* above the caret's line, below it when there is no room */
    if (y < 4.0f) y = o->y + lh + 4.0f;
    if (x + w > ui->vw - 4.0f) x = ui->vw - 4.0f - w;
    if (x < 4.0f) x = 4.0f;
    cui_overlay_begin(ui, id_str, (cui_rect){ x, y, w, h });
    cui_column(ui, .width = CUI_FIT, .max_width = w, .pad = 8, .gap = 4, .bg = CUI_COLOR_SURFACE_RAISED, .border = 1, .radius = 6,
               .elevation = CUI_ELEVATION_FLOATING, .clip = true) {
        cui_row(ui, .gap = 0) {
            /* the active parameter in full color and underlined, the rest dimmed (an accent alone is too faint in
             * some themes) */
            bool act = active_end > active_start;
            cui_color rest = act ? CUI_COLOR_TEXT_SECONDARY : CUI_COLOR_FOREGROUND;
            if (active_start > 0)
                cui_text(ui, label, .length = active_start, .size = size, .font = CUI_FONT_MONO, .nowrap = true, .color = rest);
            if (act) {
                cui_column(ui, .gap = 0) {
                    cui_text(ui, label + active_start, .length = active_end - active_start, .size = size, .font = CUI_FONT_MONO,
                             .nowrap = true, .color = CUI_COLOR_FOREGROUND);
                    cui_box(ui, .width = CUI_FILL, .height = CUI_PX(1), .bg = CUI_COLOR_ACCENT);
                }
            }
            if (active_end < len)
                cui_text(ui, label + active_end, .size = size, .font = CUI_FONT_MONO, .nowrap = true, .color = rest);
        }
        if (doc) cui_markdown(ui, "doc", o->doc, .compact = true, .size = size - 1);
    }
    cui_overlay_end(ui);
}
