/* The markdown view's big path (src/cui_markdown.c, "Big texts") against the small one, which
 * parses and builds every block every frame:
 *
 *   - a corpus (the docs folder, the small view's own edge cases, generated notes, plain text) drawn
 *     both ways: the same vertices at many scroll offsets once the big path has seen every block,
 *     the same headings, and the same text copied by Ctrl+A, Ctrl+C before it has seen them;
 *   - a jump to a heading the big path never built lands on it, at the top, the middle and the end;
 *   - a big text cut on a worker shows its top in the first frame and settles on the same frame;
 *   - an edit above the window keeps what is on screen in place; a task box toggles the right byte;
 *   - many small lists or quotes (once quadratic in frame memory) stay linear.
 *
 *     cui_test_markdown_big [docs folder]
 */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static char *g_clip;
static void clip_set(void *u, const char *s) {
    (void)u;
    free(g_clip);
    size_t n = strlen(s);
    g_clip = malloc(n + 1);
    if (g_clip) memcpy(g_clip, s, n + 1);
}
static const char *clip_get(void *u) { (void)u; return g_clip ? g_clip : ""; }

static cui_sw_renderer *g_sw;
static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_platform(ui, &(cui_platform_iface){ .set_clipboard = clip_set, .get_clipboard = clip_get });
    cui_set_reduced_motion(ui, true);
    return ui;
}

typedef struct {
    const char *text; int len;
    bool plain;
    int revision;
    cui_markdown_result res;
} view;

static void build(cui_ctx *ui, view *v) {
    cui_scroll(ui, .id = "s", .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .pad = 24, .width = CUI_FILL) {
            v->res = cui_markdown(ui, "md", v->text, .length = v->len, .plain = v->plain, .revision = v->revision);
        }
    }
}
static void frame(cui_ctx *ui, view *v) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    build(ui, v);
    cui_end_frame(ui);
}
/* frames until nothing more is asked for (a worker's wakeup included) */
static int settle(cui_ctx *ui, view *v) {
    int n = 0;
    for (; n < 400; n++) {
        frame(ui, v);
        float w = cui_next_wakeup(ui);
        if (!cui_needs_redraw(ui) && !(w >= 0 && w < 0.05f)) break;
    }
    return n + 1;
}
static void small_path(void) { cui__md_big_bytes = 0x7FFFFFFF; }
static void big_path(int sync) { cui__md_big_bytes = 0; cui__md_sync_bytes = sync; }

/* ---- frames ---- */

typedef struct { float *v; int n; } shot;   /* x, y and color of every vertex, and the index count */
static shot take(cui_ctx *ui) {
    const cui_draw_data *dd = cui_get_draw_data(ui);
    shot s;
    s.n = dd->vertex_count;
    s.v = malloc(sizeof(float) * 3 * (size_t)(s.n + 1));
    for (int k = 0; s.v && k < s.n; k++) {
        const cui_vertex *x = &dd->vertices[k];
        uint32_t c = (uint32_t)x->color.r | (uint32_t)x->color.g << 8 | (uint32_t)x->color.b << 16 | (uint32_t)x->color.a << 24;
        s.v[k * 3] = x->x; s.v[k * 3 + 1] = x->y; s.v[k * 3 + 2] = (float)(c & 0xFFFFFF);
    }
    return s;
}
/* the same drawing: as many vertices, each within a twentieth of a pixel, the same colors. Loose, deep in a
 * long text: the big path places a block after a spacer whose height is the sum of the blocks above in another
 * order of float additions than the layout's, so a line can land one pixel off where its position rounds
 * (and a shape at the window's edge can be culled or not): within a pixel, a few shapes either way. */
static bool same_shot(shot a, shot b, float *worst, bool loose) {
    *worst = 0;
    if (!a.v || !b.v) { *worst = 1e9f; return false; }
    if (a.n != b.n) { *worst = 1e9f; return loose && abs(a.n - b.n) <= 24; }
    for (int k = 0; k < a.n; k++) {
        float d = fmaxf(fabsf(a.v[k * 3] - b.v[k * 3]), fabsf(a.v[k * 3 + 1] - b.v[k * 3 + 1]));
        if (a.v[k * 3 + 2] != b.v[k * 3 + 2]) d = 1e9f;
        if (d > *worst) *worst = d;
    }
    return *worst <= (loose ? 1.0f : 0.05f);
}

static void scroll_to(cui_ctx *ui, view *v, float y) {
    cui_scroll_set(ui, "s", y, true);
    settle(ui, v);
}

static void key(cui_ctx *ui, view *v, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods); frame(ui, v);
    cui_input_key(ui, k, false, mods); frame(ui, v);
}
/* a drag from (x0, y0) to (x1, y1), then Ctrl+C: what reached the clipboard */
static char *copy_drag(cui_ctx *ui, view *v, float x0, float y0, float x1, float y1) {
    for (int k = 0; k < 30; k++) frame(ui, v);   /* not a double click */
    cui_input_mouse_move(ui, x0, y0); frame(ui, v);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, v);
    for (int k = 1; k <= 4; k++) { cui_input_mouse_move(ui, x0 + (x1 - x0) * (float)k / 4, y0 + (y1 - y0) * (float)k / 4); frame(ui, v); }
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, v);
    free(g_clip); g_clip = NULL;
    key(ui, v, CUI_KEY_C, CUI_MOD_PRIMARY);
    char *out = g_clip;
    g_clip = NULL;
    return out;
}
/* a click in the view takes the keys, then Ctrl+A, Ctrl+C: what reached the clipboard */
static char *copy_all(cui_ctx *ui, view *v) {
    cui_input_mouse_move(ui, 40, 30); frame(ui, v);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, v);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, v);
    cui_input_mouse_move(ui, 790, 590); frame(ui, v);
    free(g_clip); g_clip = NULL;
    key(ui, v, CUI_KEY_A, CUI_MOD_PRIMARY);
    key(ui, v, CUI_KEY_C, CUI_MOD_PRIMARY);
    char *out = g_clip;
    g_clip = NULL;
    return out;
}

/* ---- the corpus ---- */

static char *read_file(const char *path, int *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); s = NULL; }
    fclose(f);
    if (s) { s[n] = 0; *len = (int)n; }
    return s;
}

static unsigned rng = 0x2545F491u;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
typedef struct { char *s; int n, cap; } buf;
static void put(buf *b, const char *s) {
    int l = (int)strlen(s);
    if (b->n + l + 1 > b->cap) {
        int nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + l + 1) nc *= 2;
        b->s = realloc(b->s, (size_t)nc);
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, (size_t)l + 1);
    b->n += l;
}
static const char *const words[] = { "alpha", "beta", "**gamma**", "*delta*", "`eps`", "[zeta](https://e.org/z)", "eta", "theta",
                                     "iota", "kappa", "~~lambda~~", "mu", "nu", "xi", "omicron", "pi", "rho", "sigma", "tau" };
static void words_n(buf *b, int n) { for (int k = 0; k < n; k++) { if (k) put(b, " "); put(b, words[rnd() % (sizeof words / sizeof words[0])]); } }
/* a note of every kind of block, kind 0 mixed, 1 small lists, 2 small quotes, 3 plain lines */
static char *gen(int kind, int bytes, int *len) {
    buf b = { 0 };
    char t[96];
    int sec = 0;
    if (kind == 0) put(&b, "---\ntitle: Generated\ntags: [a, b]\n---\n\n");
    while (b.n < bytes) {
        int r = kind == 1 ? 1 : kind == 2 ? 2 : kind == 3 ? 9 : (int)(rnd() % 9);
        if (kind != 3 && rnd() % 3 == 0) { snprintf(t, sizeof t, "%s Heading %d\n\n", rnd() % 2 ? "##" : "###", ++sec); put(&b, t); }
        switch (r) {
        case 1:
            for (int i = 0, n = 1 + (int)(rnd() % 3); i < n; i++) {
                put(&b, rnd() % 4 ? "- " : "- [ ] "); words_n(&b, 3 + (int)(rnd() % 10)); put(&b, "\n");
                if (rnd() % 4 == 0) { put(&b, "  1. "); words_n(&b, 4); put(&b, "\n"); }
                if (rnd() % 9 == 0) { put(&b, "  ### Nested heading\n"); }
            }
            put(&b, rnd() % 3 ? "\n" : "\nA paragraph between lists.\n\n");
            break;
        case 2:
            if (rnd() % 5 == 0) put(&b, rnd() % 2 ? "> [!tip]- Folded\n" : "> [!note]+ Open\n");
            for (int i = 0, n = 1 + (int)(rnd() % 3); i < n; i++) { put(&b, i == 1 && rnd() % 3 == 0 ? "> > " : "> "); words_n(&b, 6 + (int)(rnd() % 12)); put(&b, "\n"); }
            if (rnd() % 6 == 0) put(&b, "> ## Quoted heading\n");
            put(&b, "\n");
            break;
        case 3:
            put(&b, "| A | B |\n|---|--:|\n");
            for (int i = 0, n = 2 + (int)(rnd() % 4); i < n; i++) { put(&b, "| "); words_n(&b, 2); snprintf(t, sizeof t, " | %u |\n", rnd() % 999); put(&b, t); }
            put(&b, "\n");
            break;
        case 4:
            put(&b, "```c\n");
            for (int i = 0, n = 1 + (int)(rnd() % 6); i < n; i++) { snprintf(t, sizeof t, "int v%d = %u;\n", i, rnd() % 100); put(&b, t); }
            put(&b, "```\n\n");
            break;
        case 5: words_n(&b, 6); put(&b, "\n---\n\n"); break;   /* a setext heading */
        case 6: put(&b, "***\n\n"); break;
        case 7: put(&b, "    indented code\n    more code\n\n"); break;
        case 9:
            for (int i = 0, n = 1 + (int)(rnd() % 4); i < n; i++) { words_n(&b, 3 + (int)(rnd() % 12)); put(&b, "\n"); }
            put(&b, rnd() % 4 ? "\n" : "\n\n\n");
            break;
        default: words_n(&b, 20 + (int)(rnd() % 50)); put(&b, "\n\n"); break;
        }
    }
    *len = b.n;
    return b.s;
}

/* The small path, then the big one over the same text in the same context (one glyph atlas),
 * after the big one has scrolled through every block: the same vertices at each offset, the
 * same headings, the same text copied. */
static void compare(const char *name, const char *text, int len, bool plain) {
    cui_ctx *ui = new_ctx();
    view v = { text, len, plain, 0, { 0 } };
    enum { SAMPLES = 7 };
    shot a[SAMPLES];
    float at[SAMPLES];
    small_path();
    settle(ui, &v);
    frame(ui, &v);   /* the headings' y come from the latest layout: one frame more */
    cui_scroll_info si = cui_scroll_get(ui, "s");
    float content = si.content_h;
    cui_markdown_heading *heads = v.res.heading_count ? malloc(sizeof *heads * (size_t)v.res.heading_count) : NULL;
    int nheads = v.res.heading_count;
    if (heads) memcpy(heads, v.res.headings, sizeof *heads * (size_t)nheads);
    for (int k = 0; k < SAMPLES; k++) {
        at[k] = k == SAMPLES - 1 ? si.max_y : floorf(si.max_y * (float)k / (SAMPLES - 1) * 0.97f);
        scroll_to(ui, &v, at[k]);
        a[k] = take(ui);
    }
    scroll_to(ui, &v, 0);
    char *drag_a = copy_drag(ui, &v, 60, 190, 500, 470);
    char *copy_a = copy_all(ui, &v);
    cui_destroy(ui);

    /* the big path copies before it has built most blocks */
    ui = new_ctx();
    big_path(0x7FFFFFFF);
    settle(ui, &v);
    CHECK(v.res.heading_count == nheads);
    char *drag_b = copy_drag(ui, &v, 60, 190, 500, 470);
    CHECK(drag_a && drag_a[0] && drag_b && !strcmp(drag_a, drag_b));
    if (!drag_a || !drag_b || strcmp(drag_a, drag_b) || !drag_a[0]) printf("  %s: a drag copied %zu bytes small, %zu big\n", name, drag_a ? strlen(drag_a) : 0, drag_b ? strlen(drag_b) : 0);
    free(drag_a); free(drag_b);
    char *copy_b = copy_all(ui, &v);
    cui_destroy(ui);   /* a fresh view: no selection drawn */
    ui = new_ctx();
    settle(ui, &v);
    bool copy_ok = copy_a && copy_b && !strcmp(copy_a, copy_b);
    CHECK(copy_ok);
    if (!copy_ok) printf("  %s: copied %zu bytes small, %zu big\n", name, copy_a ? strlen(copy_a) : 0, copy_b ? strlen(copy_b) : 0);
    /* every block seen, then the same frames */
    for (int guard = 0; guard < 20000; guard++) {
        si = cui_scroll_get(ui, "s");
        if (si.y >= si.max_y - 0.5f) break;
        scroll_to(ui, &v, si.y + si.view_h * 0.5f);
    }
    scroll_to(ui, &v, 0);
    frame(ui, &v);
    si = cui_scroll_get(ui, "s");
    CHECK(fabsf(si.content_h - content) < 0.5f);
    if (fabsf(si.content_h - content) >= 0.5f) printf("  %s: content %.2f small, %.2f big\n", name, content, si.content_h);
    int bad = 0;
    for (int k = 0; k < SAMPLES; k++) {
        scroll_to(ui, &v, at[k]);
        shot b = take(ui);
        float worst;
        if (!same_shot(a[k], b, &worst, k > 0)) {
            if (!bad) printf("  %s: at %.0f of %.0f: %d vertices small, %d big, off by %.3f\n", name, at[k], content, a[k].n, b.n, worst);
            if (!bad && worst > 1.5f && getenv("MD_DEBUG")) {   /* both frames as pictures */
                const cui_draw_data *dd = cui_get_draw_data(ui);
                unsigned char *px = malloc((size_t)dd->width_px * (size_t)dd->height_px * 4);
                cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
                cui_write_png("md-big.png", px, dd->width_px, dd->height_px, dd->width_px * 4);
                cui_ctx *u2 = new_ctx();
                small_path();
                settle(u2, &v);
                scroll_to(u2, &v, at[k]);
                dd = cui_get_draw_data(u2);
                cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
                cui_write_png("md-small.png", px, dd->width_px, dd->height_px, dd->width_px * 4);
                big_path(0x7FFFFFFF);
                cui_destroy(u2);
                free(px);
            }
            bad++;
        }
        free(b.v);
        free(a[k].v);
    }
    CHECK(bad == 0);
    bool heads_ok = v.res.heading_count == nheads;
    for (int k = 0; heads_ok && k < nheads; k++) {
        const cui_markdown_heading *h = &v.res.headings[k];
        heads_ok = h->level == heads[k].level && h->line == heads[k].line && h->text == heads[k].text && h->text_len == heads[k].text_len &&
                   fabsf(h->y - heads[k].y) < 0.5f;
        if (!heads_ok) printf("  %s: heading %d: line %d/%d y %.1f/%.1f\n", name, k, heads[k].line, h->line, heads[k].y, h->y);
    }
    CHECK(heads_ok);
    printf("  %-28s %7d bytes, %4d headings, %8.0f pt: %s\n", name, len, nheads, content, bad || !copy_ok || !heads_ok ? "DIFFERS" : "same");
    free(heads); free(copy_a); free(copy_b);
    cui_destroy(ui);
}

/* A heading the big path has not built: jumping to its reported y lands on it, near the top. */
static void test_jumps(void) {
    printf("big markdown: jumps to headings it never built\n");
    int len;
    char *text = gen(0, 400 * 1024, &len);
    cui_ctx *ui = new_ctx();
    view v = { text, len, false, 0, { 0 } };
    big_path(0x7FFFFFFF);
    settle(ui, &v);
    int n = v.res.heading_count;
    CHECK(n > 100);
    int picks[] = { n / 2, n - 1, n / 5, 3 };
    for (int p = 0; p < 4; p++) {
        int k = picks[p];
        scroll_to(ui, &v, v.res.headings[k].y + 24);   /* the column's padding: the view starts 24 down */
        settle(ui, &v);
        cui_scroll_info si = cui_scroll_get(ui, "s");
        float on_screen = v.res.headings[k].y + 24 - si.y;   /* where the latest layout put it, in the window */
        bool ok = on_screen > -2 && on_screen < si.view_h - 20;
        CHECK(ok);
        if (!ok) printf("  heading %d of %d: %.1f in the window (scroll %.0f of %.0f)\n", k, n, on_screen, si.y, si.max_y);
    }
    cui_destroy(ui);
    free(text);
}

/* A text cut on a worker shows its first part in the first frame and ends as the cut made at once. */
static void test_worker(void) {
    printf("big markdown: cut on a worker, the first frame, edits\n");
    int len;
    char *text = gen(0, 600 * 1024, &len);
    view v = { text, len, false, 0, { 0 } };
    cui_ctx *ui = new_ctx();
    big_path(0x7FFFFFFF);
    settle(ui, &v);
    scroll_to(ui, &v, 3000);
    shot ref = take(ui);
    cui_destroy(ui);
    ui = new_ctx();
    big_path(16 * 1024);
    frame(ui, &v);
    CHECK(cui_get_rect(ui, cui_make_id(ui, "md")).h > 600);   /* the top shows at once */
    settle(ui, &v);
    scroll_to(ui, &v, 3000);
    shot got = take(ui);
    float worst;
    CHECK(same_shot(ref, got, &worst, false));
    free(ref.v); free(got.v);
    /* an edit far above the window: what is on screen stays where it was */
    int hk = v.res.heading_count * 6 / 10;   /* a heading past the middle, 200 points down the window */
    scroll_to(ui, &v, v.res.headings[hk].y + 24 - 200);
    settle(ui, &v);
    cui_scroll_info si = cui_scroll_get(ui, "s");
    CHECK(hk >= 0);
    if (hk >= 0) {
        float before = v.res.headings[hk].y - si.y;
        int hline = v.res.headings[hk].line;
        static const char ins[] = "Inserted paragraph near the top, long enough to wrap onto a few lines of the column at this width, "
                                  "so that the blocks under it move down by more than a line or two in the layout.\n\n";
        int il = (int)sizeof ins - 1;
        char *t2 = malloc((size_t)len + (size_t)il + 1);
        int cut = 400;
        while (cut < len && text[cut - 1] != '\n') cut++;
        memcpy(t2, text, (size_t)cut);
        memcpy(t2 + cut, ins, (size_t)il);
        memcpy(t2 + cut + il, text + cut, (size_t)(len - cut + 1));
        v.text = t2; v.len = len + il;
        settle(ui, &v);
        settle(ui, &v);
        si = cui_scroll_get(ui, "s");
        int hk2 = -1;
        for (int k = 0; k < v.res.heading_count && hk2 < 0; k++) if (v.res.headings[k].line == hline + 2) hk2 = k;
        CHECK(hk2 >= 0);
        if (hk2 >= 0) {
            float after = v.res.headings[hk2].y - si.y;
            CHECK(fabsf(after - before) < 1.0f);
            if (fabsf(after - before) >= 1.0f) printf("  the heading moved from %.1f to %.1f in the window\n", before, after);
        }
        /* and the top shows the new paragraph: the small path draws the same */
        scroll_to(ui, &v, 0);
        shot b = take(ui);
        cui_ctx *u2 = new_ctx();
        small_path();
        settle(u2, &v);
        shot a = take(u2);
        big_path(16 * 1024);
        CHECK(a.n == b.n);
        free(a.v); free(b.v);
        cui_destroy(u2);
        v.text = text; v.len = len;
        free(t2);
    }
    /* .revision: a promise kept skips the compare; a new number reads it */
    v.revision = 7;
    settle(ui, &v);
    text[0] = '#'; text[1] = '#';   /* "## A big..." would change the first heading */
    settle(ui, &v);
    v.revision = 8;
    settle(ui, &v);
    CHECK(v.res.heading_count > 0);
    cui_destroy(ui);
    free(text);
}

/* A task box in a big text reports the byte between its brackets. */
static void test_tasks(void) {
    printf("big markdown: task boxes\n");
    buf b = { 0 };
    for (int k = 0; k < 3000; k++) { char t[64]; snprintf(t, sizeof t, "- [ ] task number %d\n", k); put(&b, t); }
    view v = { b.s, b.n, false, 0, { 0 } };
    cui_ctx *ui = new_ctx();
    big_path(0x7FFFFFFF);
    settle(ui, &v);
    scroll_to(ui, &v, 20000);
    settle(ui, &v);
    int found = 0;
    for (float y = 100; y < 500 && !found; y += 4) {
        cui_input_mouse_move(ui, 24 + 13, y); frame(ui, &v);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, &v);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &v);
        if (v.res.task_offset >= 0) {
            found = 1;
            int off = v.res.task_offset;
            CHECK(b.s[off - 1] == '[' && b.s[off + 1] == ']');
            int line = 1;
            for (int q = 0; q < off; q++) line += b.s[q] == '\n';
            CHECK(v.res.task_line == line && line > 100);
            CHECK(cui_markdown_apply_task(b.s, &v.res) && b.s[off] == 'x');
        }
    }
    CHECK(found);
    cui_destroy(ui);
    free(b.s);
}

/* Many small lists or quotes: frame memory (and time) grow with the text, not its square. */
static void test_linear(void) {
    printf("markdown: many small lists and quotes stay linear\n");
    for (int kind = 1; kind <= 2; kind++) {
        size_t peak[2];
        for (int s = 0; s < 2; s++) {
            int len;
            char *text = gen(kind, s ? 400 * 1024 : 200 * 1024, &len);
            view v = { text, len, false, 0, { 0 } };
            cui_ctx *ui = new_ctx();
            small_path();
            frame(ui, &v);
            frame(ui, &v);
            size_t used, cap;
            cui_frame_memory(ui, &used, &cap, &peak[s]);
            cui_destroy(ui);
            free(text);
        }
        CHECK(peak[1] < peak[0] * 5 / 2);
        printf("  %s: frame memory %.1f MB for 200 KB, %.1f MB for 400 KB\n", kind == 1 ? "lists" : "quotes", peak[0] / 1048576.0, peak[1] / 1048576.0);
    }
}

/* Random markdown-ish text on both paths, at random widths: the same headings and the same text
 * copied by Ctrl+A, Ctrl+C, and no crash (some of it cut on a worker). */
static void test_fuzz(int iters) {
    printf("big markdown: random text, both paths\n");
    static const char *const bits[] = { "*", "**", "_", "~~", "`", "```", "~~~", "[", "](", ")", "<", ">", "> ", "# ", "## ", "- ", "1. ", "|",
                                        "\n", "\n\n", "  ", "    ", "\t", "\\", "&amp;", "[ ]", "[x]", "https://", "www.", "word ", "\xc3\xa9",
                                        "\xff", "---", "===", "\r\n", "!", "[!note]- ", "[[", "]]", "|---|", "***" };
    int bad = 0;
    for (int it = 0; it < iters; it++) {
        buf b = { 0 };
        int parts = 20 + (int)(rnd() % 400);
        for (int k = 0; k < parts; k++) put(&b, bits[rnd() % (sizeof bits / sizeof bits[0])]);
        view v = { b.s, b.n, rnd() % 5 == 0, 0, { 0 } };
        float w = (float)(200 + rnd() % 900);
        cui_ctx *ui = new_ctx();
        cui_set_viewport(ui, w, 400, 1);
        small_path();
        settle(ui, &v);
        int nh = v.res.heading_count;
        char *a = copy_all(ui, &v);
        cui_destroy(ui);
        ui = new_ctx();
        cui_set_viewport(ui, w, 400, 1);
        big_path(rnd() % 3 == 0 ? 256 : 0x7FFFFFFF);
        settle(ui, &v);
        bool ok = v.res.heading_count == nh;
        for (int s = 0; s < 3; s++) { scroll_to(ui, &v, (float)(rnd() % 3000)); }
        char *c = copy_all(ui, &v);
        ok = ok && ((!a && !c) || (a && c && !strcmp(a, c)));
        if (!ok && bad++ < 3) printf("  iteration %d (%d bytes, width %.0f%s): headings %d/%d, copied %zu/%zu bytes\n", it, b.n, w,
                                     v.plain ? ", plain" : "", nh, v.res.heading_count, a ? strlen(a) : 0, c ? strlen(c) : 0);
        cui_destroy(ui);
        free(a); free(c); free(b.s);
    }
    CHECK(bad == 0);
}

static const char *const edge[] = {
    "# Title\n\nSome *plain* text with `code` and a [link](x).\n\n| A | B |\n|--|--|\n| a | b |\n\n- item\n",
    "Title\n===\nSub\n---\n- - -\n***\n___\n",
    "> quote\nlazy\n> > nested\n>\n> - item\n>   ```\n>   code\n>   ```\n\nafter\n",
    "1. one\n2. two\n   - sub\n     1) deep\n10. ten\n- other\n\n- [ ] task\n- [x] done\n",
    "    indented code\n\n    more\n\ntext\n```c\nint x;\n```\n~~~\nunclosed fence\n\n# not a heading\n",
    "> [!warning]- Folded title\n> ## hidden heading\n> body\n\n> [!tip] Open\n> ## shown heading\n\n> [!note]+ Plus\n> text\n",
    "---\ntitle: Front\ntags: [x, y]\n---\n\n# After front matter\n\ntext\n",
    "a\r\nb  \r\nc\\\r\nd<br>e\r\n\r\n## crlf heading\r\n\r\n- crlf item\r\n",
    "![img](missing.png) text with an image\n\n[[wiki link]] and [[target|label]] and ![[embed.png]]\n",
};

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--worker")) {   /* stress: the worker test, again and again */
        for (int k = atoi(argv[2]); k > 0; k--) test_worker();
        printf("%d checks, %d failed\n", g_checks, g_failed);
        return g_failed ? 1 : 0;
    }
    const char *docs = argc > 1 ? argv[1] : "docs";
    printf("big markdown: the same drawing, headings and copy as the small path\n");
    static const char *const files[] = { "README.md", "architecture.md", "components.md", "performance.md", "integration.md",
                                         "layout.md", "theming.md", "tutorial.md", "recipes.md", "components-more.md" };
    for (int k = 0; k < (int)(sizeof files / sizeof files[0]); k++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", docs, files[k]);
        int len;
        char *t = read_file(path, &len);
        if (!t) { printf("  (no %s)\n", path); continue; }
        compare(files[k], t, len, false);
        free(t);
    }
    {   /* the small view's edge cases, repeated into one text */
        buf b = { 0 };
        for (int r = 0; r < 20; r++) for (int k = 0; k < (int)(sizeof edge / sizeof edge[0]); k++) { put(&b, edge[k]); put(&b, "\n"); }
        compare("edge cases", b.s, b.n, false);
        free(b.s);
    }
    static const char *const gnames[] = { "generated mixed", "generated small lists", "generated small quotes", "generated plain" };
    for (int k = 0; k < 4; k++) {
        int len;
        char *t = gen(k, 90 * 1024, &len);
        compare(gnames[k], t, len, k == 3);
        free(t);
    }
    test_jumps();
    test_worker();
    test_tasks();
    test_linear();
    test_fuzz(300);
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
