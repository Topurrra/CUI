/* Tests for the source control lists: the changes list, the commit log with its lane graph and the branch picker.
 * Runs headless with the software renderer. Interactions use the layout the components have (rows of a known
 * height at a known place), so the tests do not depend on pixel positions beyond that. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; const char *a_ = (a), *b_ = (b); if (!a_ || strcmp(a_, b_)) { g_failed++; \
    printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, a_ ? a_ : "(null)", b_); } } while (0)

static cui_sw_renderer *g_sw;

static cui_ctx *new_ctx(float w, float h) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, w, h, 1);
    cui_set_reduced_motion(ui, true);
    cui_input_mouse_leave(ui);
    return ui;
}

typedef void (*build_fn)(cui_ctx *ui, void *user);
static void run(cui_ctx *ui, build_fn fn, void *user, int frames) {
    for (int i = 0; i < frames; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 20, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) fn(ui, user);
        cui_end_frame(ui);
    }
}
static void move(cui_ctx *ui, build_fn fn, void *u, float x, float y) { cui_input_mouse_move(ui, x, y); run(ui, fn, u, 1); }
static void click_btn(cui_ctx *ui, build_fn fn, void *u, float x, float y, cui_mouse_button b) {
    move(ui, fn, u, x, y);
    cui_input_mouse_button(ui, b, true); run(ui, fn, u, 1);
    cui_input_mouse_button(ui, b, false); run(ui, fn, u, 1);
}
static void click(cui_ctx *ui, build_fn fn, void *u, float x, float y) { click_btn(ui, fn, u, x, y, CUI_MOUSE_LEFT); }
static void key(cui_ctx *ui, build_fn fn, void *u, cui_key k) {
    cui_input_key(ui, k, true, 0); cui_input_key(ui, k, false, 0);
    run(ui, fn, u, 1);
    cui_input_key(ui, CUI_KEY_NONE, false, 0);
}
static void type(cui_ctx *ui, build_fn fn, void *u, const char *s) { cui_input_text(ui, s); run(ui, fn, u, 1); }
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }

/* ------------------------------------------------------------------------ */
/* changes list                                                             */

enum { A_OPEN = 1, A_DISCARD, A_STAGE, A_UNSTAGE };
static const cui_change_action g_row_acts[3] = {
    { CUI_ICON_FILE, "Open", A_OPEN }, { CUI_ICON_UNDO_2, "Discard", A_DISCARD }, { CUI_ICON_PLUS, "Stage", A_STAGE } };
static const cui_change_action g_head_acts[1] = { { CUI_ICON_PLUS, "Stage all", A_STAGE } };

static const cui_change_row g_staged[2] = {
    { .name = "a.c", .dir = "src", .state = 'M', .state_tone = CUI_TONE_WARNING, .actions = g_row_acts, .nactions = 3 },
    { .name = "b.c", .dir = "src", .state = 'A', .state_tone = CUI_TONE_SUCCESS, .actions = g_row_acts, .nactions = 3 } };

typedef struct { int calls; } row_calls;
static bool big_row(void *user, int i, cui_change_row *out) {
    static char name[32];
    ((row_calls *)user)->calls++;
    snprintf(name, sizeof name, "file%05d.txt", i);
    out->name = name; out->dir = "some/folder"; out->state = 'M'; out->state_tone = CUI_TONE_WARNING;
    out->actions = g_row_acts; out->nactions = 3;
    return true;
}

typedef struct {
    cui_change_section sec[2];
    int nsec;
    bool collapsed[2];
    cui_change_ref cursor;
    cui_changes_result r;
    int clicks, actions, spaces, folds;
    cui_change_ref last_click, last_action;
    int last_action_id;
    row_calls calls;
    bool use_own_state;
} chg_case;

static void build_chg(cui_ctx *ui, void *u) {
    chg_case *c = u;
    cui_changes_opts o = { .height = CUI_PX(300) };
    if (c->use_own_state) { o.collapsed = c->collapsed; o.cursor = &c->cursor; }
    c->r = cui_changes_list_ex(ui, "chg", c->sec, c->nsec, &o);
    if (c->r.clicked.section >= 0) { c->clicks++; c->last_click = c->r.clicked; }
    if (c->r.action) { c->actions++; c->last_action = c->r.action_at; c->last_action_id = c->r.action; }
    if (c->r.space.section >= 0) c->spaces++;
    if (c->r.folded >= 0) c->folds++;
}

static void chg_init(chg_case *c) {
    memset(c, 0, sizeof *c);
    c->cursor = (cui_change_ref){ -1, -1 };
    c->sec[0] = (cui_change_section){ .title = "Staged", .count = 2, .rows = g_staged, .actions = g_head_acts, .nactions = 1 };
    c->sec[1] = (cui_change_section){ .title = "Changes", .count = 3, .row_at = big_row, .user = &c->calls };
    c->nsec = 2;
}

static void test_changes_empty(void) {
    printf("changes list: zero and empty\n");
    cui_ctx *ui = new_ctx(400, 400);
    chg_case c;
    chg_init(&c);
    c.nsec = 0;
    run(ui, build_chg, &c, 3);
    CHECK_EQ(c.r.clicked.section, -1); CHECK_EQ(c.r.cursor.section, -1); CHECK_EQ(c.r.folded, -1); CHECK_EQ(c.r.action, 0);
    c.nsec = 2; c.sec[0].count = 0; c.sec[1].count = 0;   /* sections without rows draw nothing */
    run(ui, build_chg, &c, 3);
    CHECK_EQ(c.r.clicked.section, -1);
    c.sec[0].count = 2; c.sec[0].rows = NULL; c.sec[0].row_at = NULL;   /* rows with no source are blank, not a crash */
    run(ui, build_chg, &c, 3);
    CHECK_EQ(c.r.clicked.section, -1);
    cui_changes_result r = { 0 };
    cui_begin_frame(ui, 1.0f / 60.0f);
    r = cui_changes_list_ex(ui, "x", NULL, 0, NULL);   /* all defaults */
    cui_end_frame(ui);
    CHECK_EQ(r.clicked.section, -1);
    cui_destroy(ui);
}

/* the list sits at (20, 20); flat index i is at y = 20 + 22 i + 11 */
#define ROW_Y(i) (20.0f + 22.0f * (float)(i) + 11.0f)

static void test_changes_clicks(void) {
    printf("changes list: row clicks, actions, folding\n");
    cui_ctx *ui = new_ctx(400, 400);
    chg_case c;
    chg_init(&c);
    c.use_own_state = true;
    run(ui, build_chg, &c, 3);
    click(ui, build_chg, &c, 150, ROW_Y(1));   /* staged row 0 */
    CHECK_EQ(c.clicks, 1); CHECK_EQ(c.last_click.section, 0); CHECK_EQ(c.last_click.row, 0);
    CHECK_EQ(c.cursor.section, 0); CHECK_EQ(c.cursor.row, 0);
    run(ui, build_chg, &c, 1);
    CHECK(c.r.focused);
    click(ui, build_chg, &c, 150, ROW_Y(5));   /* changes row 1 (the header of Changes is index 3) */
    CHECK_EQ(c.last_click.section, 1); CHECK_EQ(c.last_click.row, 1);
    /* action buttons: the last is the rightmost, 30 + 22 k from the right edge of the 360 wide list */
    float right = 20 + 360;
    float bx_stage = right - 30 - 22 * 1 + 11, bx_discard = right - 30 - 22 * 2 + 11, bx_open = right - 30 - 22 * 3 + 11;
    int clicks = c.clicks;
    click(ui, build_chg, &c, bx_stage, ROW_Y(2));
    CHECK_EQ(c.actions, 1); CHECK_EQ(c.last_action_id, A_STAGE); CHECK_EQ(c.last_action.section, 0); CHECK_EQ(c.last_action.row, 1);
    CHECK_EQ(c.clicks, clicks);   /* a button is not a row click */
    click(ui, build_chg, &c, bx_discard, ROW_Y(1));
    CHECK_EQ(c.last_action_id, A_DISCARD);
    click(ui, build_chg, &c, bx_open, ROW_Y(4));
    CHECK_EQ(c.last_action_id, A_OPEN); CHECK_EQ(c.last_action.section, 1); CHECK_EQ(c.last_action.row, 0);
    /* the header's button */
    click(ui, build_chg, &c, bx_stage, ROW_Y(0));
    CHECK_EQ(c.last_action_id, A_STAGE); CHECK_EQ(c.last_action.section, 0); CHECK_EQ(c.last_action.row, -1);
    CHECK(!c.collapsed[0]);
    /* a header click folds the section and says so */
    click(ui, build_chg, &c, 100, ROW_Y(0));
    CHECK(c.collapsed[0]); CHECK_EQ(c.folds, 1);
    CHECK_EQ(c.last_click.section, 0); CHECK_EQ(c.last_click.row, -1);
    click(ui, build_chg, &c, 150, ROW_Y(1));   /* what was row 0 of staged is now the Changes header */
    CHECK_EQ(c.last_click.section, 1); CHECK_EQ(c.last_click.row, -1);
    CHECK(c.collapsed[1]); CHECK_EQ(c.folds, 2);
    cui_destroy(ui);
}

static void test_changes_keys(void) {
    printf("changes list: cursor keys, Space, Enter, fold keys\n");
    cui_ctx *ui = new_ctx(400, 400);
    chg_case c;
    chg_init(&c);
    c.use_own_state = true;
    run(ui, build_chg, &c, 3);
    click(ui, build_chg, &c, 150, ROW_Y(1));    /* cursor on staged row 0, the list focused */
    key(ui, build_chg, &c, CUI_KEY_DOWN);
    CHECK_EQ(c.cursor.section, 0); CHECK_EQ(c.cursor.row, 1);
    key(ui, build_chg, &c, CUI_KEY_DOWN);       /* the next header */
    CHECK_EQ(c.cursor.section, 1); CHECK_EQ(c.cursor.row, -1);
    key(ui, build_chg, &c, CUI_KEY_UP);
    CHECK_EQ(c.cursor.section, 0); CHECK_EQ(c.cursor.row, 1);
    key(ui, build_chg, &c, CUI_KEY_END);
    CHECK_EQ(c.cursor.section, 1); CHECK_EQ(c.cursor.row, 2);
    key(ui, build_chg, &c, CUI_KEY_DOWN);       /* stays at the end */
    CHECK_EQ(c.cursor.section, 1); CHECK_EQ(c.cursor.row, 2);
    key(ui, build_chg, &c, CUI_KEY_HOME);
    CHECK_EQ(c.cursor.section, 0); CHECK_EQ(c.cursor.row, -1);
    int spaces = c.spaces, clicks = c.clicks;
    key(ui, build_chg, &c, CUI_KEY_SPACE);
    CHECK_EQ(c.spaces, spaces + 1);
    run(ui, build_chg, &c, 1);
    CHECK_EQ(c.r.space.section, -1);            /* reported for one frame only */
    key(ui, build_chg, &c, CUI_KEY_DOWN);
    key(ui, build_chg, &c, CUI_KEY_ENTER);      /* Enter on a row is a click */
    CHECK_EQ(c.clicks, clicks + 1); CHECK_EQ(c.last_click.row, 0);
    key(ui, build_chg, &c, CUI_KEY_HOME);
    key(ui, build_chg, &c, CUI_KEY_LEFT);       /* Left folds a header, Right unfolds it */
    CHECK(c.collapsed[0]);
    key(ui, build_chg, &c, CUI_KEY_RIGHT);
    CHECK(!c.collapsed[0]);
    key(ui, build_chg, &c, CUI_KEY_ENTER);      /* Enter on a header folds */
    CHECK(c.collapsed[0]);
    cui_destroy(ui);
}

static void test_changes_big(void) {
    printf("changes list: 10,000 rows ask for the rows in view only\n");
    cui_ctx *ui = new_ctx(400, 400);
    chg_case c;
    chg_init(&c);
    c.sec[1].count = 10000;
    c.sec[0].count = 0;
    run(ui, build_chg, &c, 3);
    c.calls.calls = 0;
    int frames = 200;
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        cui_input_scroll(ui, 0, -3);
        run(ui, build_chg, &c, 1);
    }
    double ms = (now_ms() - t0) / frames;
    printf("  %.3f ms a frame (build, layout and paint) over %d frames of scrolling, %d row calls\n", ms, frames, c.calls.calls);
    CHECK(c.calls.calls / frames < 40);
    CHECK(ms < 8.0);
    cui_scroll_info si = cui_scroll_get(ui, "rows");
    (void)si;
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* commit log                                                               */

static const cui_lane_edge e0[1] = { { 0, 0, CUI_LANE_OUT, 0 } };
static const cui_lane_edge e1[2] = { { 0, 0, CUI_LANE_PASS, 0 }, { 1, 0, CUI_LANE_OUT, 1 } };
static const cui_lane_edge e2[2] = { { 1, 0, CUI_LANE_IN, 1 }, { 0, 0, CUI_LANE_OUT, 0 } };
static const cui_log_chip g_chips[2] = { { .text = "main", .tone = CUI_TONE_SUCCESS, .strong = true }, { .text = "origin/dev", .suffix = "origin", .tone = CUI_TONE_INFO } };
static const cui_log_row g_log[3] = {
    { .hash = "aaaaaaa", .subject = "tip of main", .author = "Ann", .date = "1 hour ago", .edges = e0, .nedges = 1, .lane = 0, .color = 0, .tip = true, .head = false,
      .chips = g_chips, .nchips = 2 },
    { .hash = "bbbbbbb", .subject = "work on a branch", .author = "Bob", .date = "2 hours ago", .edges = e1, .nedges = 2, .lane = 1, .color = 1, .tip = true },
    { .hash = "ccccccc", .subject = "merge", .author = "Ann", .date = "3 hours ago", .edges = e2, .nedges = 2, .lane = 0, .color = 0, .merge = true } };

typedef struct {
    cui_commit_log_opts o;
    cui_commit_log_result r;
    int clicks, ctx, acts, calls;
    int last_click, last_ctx, last_act;
    int sel;
    int max_want;
} log_case;

static bool big_log(void *user, int i, cui_log_row *out) {
    static char subj[48], hash[16];
    log_case *c = user;
    c->calls++;
    snprintf(subj, sizeof subj, "commit number %d", i);
    snprintf(hash, sizeof hash, "%07x", i);
    out->hash = hash; out->subject = subj; out->author = "Ann"; out->date = "1 day ago";
    out->lane = i % 3; out->color = (uint16_t)(i % 3); out->tip = i == 0;
    return true;
}

static void build_log(cui_ctx *ui, void *u) {
    log_case *c = u;
    c->o.height = CUI_PX(300);
    c->o.selected = &c->sel;
    c->r = cui_commit_log_ex(ui, "log", &c->o);
    if (c->r.clicked >= 0) { c->clicks++; c->last_click = c->r.clicked; }
    if (c->r.context >= 0) { c->ctx++; c->last_ctx = c->r.context; }
    if (c->r.activated >= 0) { c->acts++; c->last_act = c->r.activated; }
    if (c->r.want > c->max_want) c->max_want = c->r.want;
}

static void log_init(log_case *c) {
    memset(c, 0, sizeof *c);
    c->sel = -1;
    c->o.count = 3; c->o.rows = g_log; c->o.lanes = 2; c->o.font_size = 13; c->o.keys = true;
}

static void test_log_empty(void) {
    printf("commit log: zero and empty\n");
    cui_ctx *ui = new_ctx(500, 400);
    log_case c;
    log_init(&c);
    c.o.count = 0;
    run(ui, build_log, &c, 3);
    CHECK_EQ(c.r.clicked, -1); CHECK_EQ(c.r.selected, -1); CHECK_EQ(c.r.want, 0);
    key(ui, build_log, &c, CUI_KEY_DOWN);      /* nothing to move to */
    CHECK_EQ(c.sel, -1); CHECK(!c.r.moved);
    log_init(&c);
    c.o.rows = NULL; c.o.row_at = NULL;        /* a count with no source draws nothing */
    run(ui, build_log, &c, 3);
    CHECK_EQ(c.r.clicked, -1);
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_commit_log_result r = cui_commit_log_ex(ui, "d", NULL);
    cui_end_frame(ui);
    CHECK_EQ(r.selected, -1);
    log_init(&c);
    c.o.count = 3; c.o.loaded = 1;             /* rows not loaded are blank and cannot be clicked */
    run(ui, build_log, &c, 3);
    click(ui, build_log, &c, 200, 20 + 26 * 2 + 13);
    CHECK_EQ(c.clicks, 0);
    cui_destroy(ui);
}

static void test_log_input(void) {
    printf("commit log: click, double click, context request, keys\n");
    cui_ctx *ui = new_ctx(500, 400);
    log_case c;
    log_init(&c);
    run(ui, build_log, &c, 3);
    CHECK_EQ(c.r.last, 3);
    click(ui, build_log, &c, 300, 20 + 26 + 13);
    CHECK_EQ(c.clicks, 1); CHECK_EQ(c.last_click, 1); CHECK_EQ(c.sel, 1); CHECK_EQ(c.r.selected, 1);
    click_btn(ui, build_log, &c, 300, 20 + 13, CUI_MOUSE_RIGHT);
    CHECK_EQ(c.ctx, 1); CHECK_EQ(c.last_ctx, 0); CHECK_EQ(c.sel, 0);
    key(ui, build_log, &c, CUI_KEY_DOWN);
    CHECK_EQ(c.sel, 1); CHECK(c.r.moved || c.sel == 1);
    key(ui, build_log, &c, CUI_KEY_END);
    CHECK_EQ(c.sel, 2);
    key(ui, build_log, &c, CUI_KEY_DOWN);
    CHECK_EQ(c.sel, 2);
    key(ui, build_log, &c, CUI_KEY_HOME);
    CHECK_EQ(c.sel, 0);
    key(ui, build_log, &c, CUI_KEY_UP);
    CHECK_EQ(c.sel, 0);
    key(ui, build_log, &c, CUI_KEY_ENTER);
    CHECK_EQ(c.acts, 1); CHECK_EQ(c.last_act, 0);
    cui_destroy(ui);
    /* keys off: nothing moves */
    ui = new_ctx(500, 400);
    log_init(&c);
    c.o.keys = false;
    run(ui, build_log, &c, 3);
    key(ui, build_log, &c, CUI_KEY_DOWN);
    CHECK_EQ(c.sel, -1);
    cui_destroy(ui);
}

static void test_log_paging(void) {
    printf("commit log: 20,000 rows, rows asked for, paging, frame cost\n");
    cui_ctx *ui = new_ctx(600, 400);
    log_case c;
    log_init(&c);
    c.o.count = 20000; c.o.rows = NULL; c.o.row_at = big_log; c.o.user = &c; c.o.lanes = 3;
    c.o.loaded = 200;
    run(ui, build_log, &c, 3);
    CHECK(c.r.want > 200);                     /* near the end of the first page: asks for more */
    c.calls = 0;
    run(ui, build_log, &c, 1);
    CHECK(c.calls <= 20);                      /* the rows in view only (rows past `loaded` are not asked for) */
    c.o.loaded = 20000;
    run(ui, build_log, &c, 3);
    CHECK_EQ(c.r.want, 0);                     /* all loaded: nothing wanted */
    c.calls = 0;
    int frames = 200;
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        cui_input_scroll(ui, 0, -5);
        run(ui, build_log, &c, 1);
    }
    double ms = (now_ms() - t0) / frames;
    printf("  %.3f ms a frame (build, layout and paint) over %d frames of scrolling, %d row calls\n", ms, frames, c.calls);
    CHECK(c.calls / frames < 40);
    CHECK(ms < 8.0);
    cui_destroy(ui);
}

static bool near_color(const unsigned char *p, cui_color c, int tol) {
    return abs(p[0] - c.r) <= tol && abs(p[1] - c.g) <= tol && abs(p[2] - c.b) <= tol;
}

static void test_log_graph_paint(void) {
    printf("commit log: the graph paint of a known lane layout\n");
    for (int dark = 0; dark < 2; dark++) {
        cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
        cui_set_viewport(ui, 400, 200, 1);
        cui_set_reduced_motion(ui, true);
        cui_input_mouse_leave(ui);
        log_case c;
        log_init(&c);
        run(ui, build_log, &c, 3);
        const cui_draw_data *dd = cui_get_draw_data(ui);
        int w = dd->width_px, h = dd->height_px;
        unsigned char *px = malloc((size_t)w * h * 4);
        cui_sw_render(g_sw, dd, px, w, h, w * 4, CUI_SW_RGBA);
        #define PX(x, y) (px + ((size_t)(y) * w + (x)) * 4)
        cui_color p0 = cui_lane_color(ui, 0), p1 = cui_lane_color(ui, 1);
        cui_color bg = cui_token_color(ui, CUI_TOKEN_BACKGROUND);
        /* rows are 26 high from y = 20; lane l is at x = 20 + 8 + 13 l + 6.5 */
        CHECK(near_color(PX(34, 33), p0, 8));                  /* row 0: the dot in lane 0 */
        CHECK(near_color(PX(34, 20 + 26 + 13 - 1), p0, 8));    /* row 1: lane 0 passes behind (a line, not a dot) */
        CHECK(near_color(PX(47, 20 + 26 + 13), p1, 8));        /* row 1: the dot in lane 1 */
        CHECK(near_color(PX(34, 20 + 52 + 4), p0, 8));         /* row 2: the line of its own lane above the merge ring */
        CHECK(near_color(PX(34, 20 + 52 + 13), bg, 8));        /* ...and the ring's hole, empty */
        CHECK(near_color(PX(34 + 4, 20 + 52 + 13), p0, 20));   /* ...inside its ring */
        CHECK(!near_color(PX(34, 20 + 13 - 8), p0, 8));        /* row 0 is a tip: nothing above its dot */
        CHECK(near_color(PX(34, 20 + 13 + 8), p0, 8));         /* its line leaves downward */
        CHECK(!near_color(PX(47, 20 + 13 + 8), p1, 8));        /* lane 1 is empty in row 0 */
        CHECK(near_color(PX(8, 8), bg, 2));                    /* the page */
        /* the lane palette: eight colors apart from each other */
        for (int a = 0; a < 8; a++)
            for (int b = a + 1; b < 8; b++) {
                cui_color ca = cui_lane_color(ui, a), cb = cui_lane_color(ui, b);
                CHECK(abs(ca.r - cb.r) + abs(ca.g - cb.g) + abs(ca.b - cb.b) > 20);
            }
        CHECK(near_color((unsigned char *)&(cui_color){ cui_lane_color(ui, 8).r, cui_lane_color(ui, 8).g, cui_lane_color(ui, 8).b, 255 }, p0, 0));   /* wraps */
        /* light and dark palettes differ in lightness */
        cui_color l = cui_lane_color(ui, 0);
        int lum = (l.r * 299 + l.g * 587 + l.b * 114) / 1000;
        if (dark) CHECK(lum > 140); else CHECK(lum < 140);
        #undef PX
        free(px);
        cui_destroy(ui);
    }
}

/* ------------------------------------------------------------------------ */
/* branch picker                                                            */

static const cui_pick_item g_local[3] = { { "main", true }, { "dev", false }, { "feature/x", false } };
static const cui_pick_item g_remote[2] = { { "origin/main", false }, { "origin/dev", false } };
static const cui_pick_section g_pick[2] = {
    { .title = "Local branches", .icon = CUI_ICON_GIT_BRANCH, .count = 3, .items = g_local },
    { .title = "Remote branches", .icon = CUI_ICON_CLOUD, .count = 2, .items = g_remote } };

typedef struct {
    cui_branch_picker_result r;
    const cui_pick_section *sec;
    int nsec;
    int picks, creates;
    char picked[64], created[64];
    bool created_named;
} pick_case;

static void build_pick(cui_ctx *ui, void *u) {
    pick_case *c = u;
    cui_button(ui, "Branch");
    c->r = cui_branch_picker_ex(ui, "bp", c->sec, c->nsec, NULL);
    if (c->r.picked) { c->picks++; snprintf(c->picked, sizeof c->picked, "%s", c->r.picked); }
    if (c->r.create) { c->creates++; c->created_named = c->r.create_name != NULL; snprintf(c->created, sizeof c->created, "%s", c->r.create_name ? c->r.create_name : ""); }
}

/* clicks down the popover until something is chosen or it closed; true when it was */
static bool pick_scan(cui_ctx *ui, pick_case *c) {
    int p0 = c->picks, c0 = c->creates;
    for (float y = 70; y < 430; y += 5) {
        click(ui, build_pick, c, 60, y);
        if (c->picks > p0 || c->creates > c0) return true;
    }
    return false;
}

static void test_picker(void) {
    printf("branch picker: closed draws nothing, filter, pick, create\n");
    pick_case c;
    memset(&c, 0, sizeof c);
    c.sec = g_pick; c.nsec = 2;
    cui_ctx *ui = new_ctx(400, 760);
    run(ui, build_pick, &c, 3);
    CHECK(!cui_popover_is_open(ui, "bp"));
    CHECK(c.r.picked == NULL); CHECK(!c.r.create); CHECK_EQ(c.r.section, -1);
    /* no sections at all: still the filter and the create row, and nothing crashes */
    pick_case e;
    memset(&e, 0, sizeof e);
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &e, 3);
    CHECK(cui_popover_is_open(ui, "bp"));
    CHECK(pick_scan(ui, &e));
    CHECK_EQ(e.creates, 1); CHECK(!e.created_named);   /* an empty filter: the app asks for a name itself */
    CHECK(!cui_popover_is_open(ui, "bp"));             /* choosing closes it */
    cui_destroy(ui);

    /* a filter that matches one name */
    ui = new_ctx(400, 760);
    memset(&c, 0, sizeof c);
    c.sec = g_pick; c.nsec = 2;
    run(ui, build_pick, &c, 3);
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &c, 3);
    type(ui, build_pick, &c, "FEAT");                 /* case does not matter, any part of the name matches */
    CHECK(pick_scan(ui, &c));
    CHECK_EQ(c.picks, 1); CHECK_STR(c.picked, "feature/x");
    run(ui, build_pick, &c, 1);
    CHECK_EQ(c.r.section, -1);                        /* reported for one frame only */
    CHECK(!cui_popover_is_open(ui, "bp"));
    /* reopening starts with an empty filter */
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &c, 3);
    type(ui, build_pick, &c, "origin/d");
    CHECK(pick_scan(ui, &c));
    CHECK_STR(c.picked, "origin/dev");
    cui_destroy(ui);

    /* a new name: the create row carries it */
    ui = new_ctx(400, 760);
    memset(&c, 0, sizeof c);
    c.sec = g_pick; c.nsec = 2;
    run(ui, build_pick, &c, 3);
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &c, 3);
    type(ui, build_pick, &c, "fresh-name");
    CHECK(pick_scan(ui, &c));
    CHECK_EQ(c.picks, 0); CHECK_EQ(c.creates, 1); CHECK(c.created_named); CHECK_STR(c.created, "fresh-name");
    cui_destroy(ui);

    /* an exact existing name: the create row asks for a name in a dialog, it does not create "dev" again */
    ui = new_ctx(400, 760);
    memset(&c, 0, sizeof c);
    c.sec = g_pick; c.nsec = 2;
    run(ui, build_pick, &c, 3);
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &c, 3);
    type(ui, build_pick, &c, "feature/x");
    int before = 0;
    (void)before;
    bool got = false;
    for (float y = 420; y > 70 && !got; y -= 5) {      /* from the bottom: the create row is the last */
        click(ui, build_pick, &c, 60, y);
        got = c.creates > 0 || c.picks > 0;
    }
    CHECK(got); CHECK_EQ(c.creates, 1); CHECK(!c.created_named);
    cui_destroy(ui);

    /* the current branch is shown checked and choosing it does nothing */
    ui = new_ctx(400, 760);
    memset(&c, 0, sizeof c);
    c.sec = g_pick; c.nsec = 2;
    run(ui, build_pick, &c, 3);
    cui_popover_toggle(ui, "bp");
    run(ui, build_pick, &c, 3);
    type(ui, build_pick, &c, "main");                 /* main and origin/main */
    CHECK(!pick_scan(ui, &c));                        /* the first hit is "main", the current one: closes, picks nothing */
    CHECK_EQ(c.picks, 0); CHECK_EQ(c.creates, 0);
    CHECK(!cui_popover_is_open(ui, "bp"));
    cui_destroy(ui);
}

int main(void) {
    g_sw = cui_sw_create();
    test_changes_empty();
    test_changes_clicks();
    test_changes_keys();
    test_changes_big();
    test_log_empty();
    test_log_input();
    test_log_paging();
    test_log_graph_paint();
    test_picker();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
