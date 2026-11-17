/* File-sidebar regressions at the rendered browser/picker boundary.
 * The optional Linux fixture replaces only the OS mount-table input in an
 * isolated mount namespace; no production injection hooks are needed. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(_MSC_VER)
#pragma warning(disable: 4996) /* Portable C stdio, as used by the other test programs. */
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static cui_sw_renderer *sw;
typedef struct {
    char path[1024], out[1024];
    char listed_path[1024];
    const char *const *places;
    int count, listings;
    bool picker, open;
    float height;
} browser;

/** Supply neutral content without reading the developer's directories. */
static int listing(void *user, const char *path, cui_file_entry *out, int cap, char *names, size_t nc, bool hidden) {
    (void)names; (void)nc; (void)hidden;
    browser *b = user;
    b->listings++;
    snprintf(b->listed_path, sizeof b->listed_path, "%s", path);
    const char *const folders[] = { "Albums", "Artists", "Playlists" };
    int n = cap < 3 ? cap : 3;
    for (int i = 0; i < n; i++) out[i] = (cui_file_entry){ folders[i], true, 0, 0 };
    return n;
}

/** Deliver input before beginning a frame, as the native app loop does. */
static void frame(cui_ctx *ui, browser *b, float dt) {
    cui_begin_frame(ui, dt);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 24, .bg = CUI_COLOR_BACKGROUND) {
        if (b->picker) {
            cui_file_picker(ui, "pick", &b->open, b->out, sizeof b->out, .title = "Choose a folder", .folders = true,
                .start = b->path, .list = listing, .list_user = b);
        } else {
            cui_file_browser(ui, "files", b->path, sizeof b->path, .list = listing, .list_user = b,
                .places = b->places, .place_count = b->count, .height = CUI_PX(b->height > 0 ? b->height : 400));
        }
    }
    cui_end_frame(ui);
}

/** Build enough frames for layout, hit rectangles and modal animations. */
static void frames(cui_ctx *ui, browser *b, int n) {
    for (int i = 0; i < n; i++) frame(ui, b, 1.0f / 60);
}

/** Locate visible labels, without relying on the sidebar's private state. */
static cui_node *label(cui_ctx *ui, const char *text) {
    size_t len = strlen(text);
    for (int i = ui->node_count - 1; i >= 0; i--) {
        cui_node *n = &ui->nodes[i];
        if (n->text && n->text_len == (int)len && !memcmp(n->text, text, len)) return n;
    }
    return NULL;
}

/** Activate a visible sidebar label through mouse input. */
static void click_label(cui_ctx *ui, browser *b, const char *text) {
    cui_node *n = label(ui, text);
    CHECK(n != NULL);
    if (!n) return;
    cui_rect r = n->rect;
    cui_input_mouse_move(ui, r.x + 8, r.y + r.h / 2);
    frames(ui, b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frames(ui, b, 1);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frames(ui, b, 1);
}

/** Make a deterministic context with real CUI layout and font metrics. */
static cui_ctx *context(bool dark) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .theme = dark ? CUI_THEME_DARK : CUI_THEME_LIGHT });
    cui_set_viewport(ui, 900, 640, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

/** Protect matching toolbar heights and navigation to entries beyond six. */
static void custom_places(void) {
    cui_ctx *ui = context(false);
    static const char *const places[] = { "/one", "/two", "/three", "/four", "/five", "/six", "/seven", "/eight", "/nine", "/ten" };
    browser b = { "/one" }; b.places = places; b.count = 10; b.height = 240;
    frames(ui, &b, 3);
    cui_push_id(ui, "files");
    cui_rect path = cui_get_rect(ui, cui_make_id(ui, "path"));
    cui_rect filter = cui_get_rect(ui, cui_make_id(ui, "filter"));
    cui_pop_id(ui);
    printf("toolbar: path %.0f, filter %.0f\n", path.h, filter.h);
    CHECK(path.h > 0 && fabsf(path.h - filter.h) < 0.1f);
    CHECK(fabsf(path.y - filter.y) < 0.1f);
    CHECK(label(ui, "ten") != NULL);
    cui_input_mouse_move(ui, 90, 170);
    cui_input_scroll(ui, 0, -20);
    frames(ui, &b, 10);
    cui_node *last = label(ui, "ten");
    CHECK(last && last->rect.y >= 24 && last->rect.y + last->rect.h < 264);
    click_label(ui, &b, "ten");
    CHECK(!strcmp(b.path, "/ten"));
    cui_destroy(ui);
}

/** Refresh through the existing keyboard action, with the file list focused. */
static void refresh(cui_ctx *ui, browser *b) {
    click_label(ui, b, "Albums");
    cui_input_key(ui, CUI_KEY_F5, true, 0); frames(ui, b, 1);
    cui_input_key(ui, CUI_KEY_F5, false, 0); frames(ui, b, 2);
}

/** Verify default places without printing any real path or drive label. */
static void native_places(void) {
    cui_ctx *ui = context(false);
    browser b = { "/" };
    frames(ui, &b, 3);
#if defined(_WIN32)
    DWORD drives = GetLogicalDrives();
    int found = 0, expected = 0;
    for (int k = 0; k < 26; k++) if (drives & ((DWORD)1 << k)) {
        expected++;
        char suffix[] = "(A:)"; suffix[1] += (char)k;
        for (int j = 0; j < ui->node_count; j++) {
            cui_node *n = &ui->nodes[j];
            if (n->text && n->text_len >= 4 && !memcmp(n->text + n->text_len - 4, suffix, 4)) { found++; break; }
        }
    }
    printf("native drive entries: %d of %d\n", found, expected);
    CHECK(found == expected && expected > 0);
#else
    CHECK(label(ui, "File system") != NULL);
#endif
    int read = b.listings;
    frames(ui, &b, 5); CHECK(b.listings == read);
    refresh(ui, &b); CHECK(b.listings > read);
    click_label(ui, &b, "Home"); frames(ui, &b, 2);
    cui_node *home = label(ui, "Home");
    CHECK(home && home->font == CUI_FONT_MEDIUM);
#if defined(_WIN32)
    char home_drive[] = "(A:)"; home_drive[1] = b.path[0];
    for (int j = 0; j < ui->node_count; j++) {
        cui_node *n = &ui->nodes[j];
        if (n->text && n->text_len >= 4 && !memcmp(n->text + n->text_len - 4, home_drive, 4))
            CHECK(n->font != CUI_FONT_MEDIUM);
    }
#else
    cui_node *root = label(ui, "File system");
    CHECK(root && root->font != CUI_FONT_MEDIUM);
#endif
    click_label(ui, &b, "Documents"); frames(ui, &b, 2);
    home = label(ui, "Home");
    cui_node *documents = label(ui, "Documents");
    CHECK(home && home->font != CUI_FONT_MEDIUM && documents && documents->font == CUI_FONT_MEDIUM);
    size_t len = strlen(b.path);
    snprintf(b.path + len, sizeof b.path - len, "/Albums"); frames(ui, &b, 2);
    documents = label(ui, "Documents"); home = label(ui, "Home");
    CHECK(documents && documents->font == CUI_FONT_MEDIUM && home && home->font != CUI_FONT_MEDIUM);
    cui_destroy(ui);
}

/** A double-click after an idle interval opens the folder on the second click. */
static void idle_double_click(void) {
    const struct { float first_dt, second_dt; bool batched, opens; } cases[] = {
        { 1.0f / 60, 1.0f / 60, false, true },
        { 1.0f, 1.0f / 60, false, true },
        { 1.0f / 60, 1.0f, false, false },
        { 1.0f, 0, true, true }
    };
    for (int modal = 0; modal < 2; modal++) for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        cui_ctx *ui = context(false);
        browser b = { "/one" }; b.picker = modal != 0; b.open = b.picker;
        frames(ui, &b, 5);
        cui_node *album = label(ui, "Albums"); CHECK(album != NULL);
        if (album) {
            cui_rect r = album->rect;
            cui_input_mouse_move(ui, r.x + 8, r.y + r.h / 2);
            for (int click = 0; click < 2; click++) {
                cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
                if (!cases[k].batched) frame(ui, &b, click ? cases[k].second_dt : cases[k].first_dt);
                cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
                if (!cases[k].batched) frames(ui, &b, 1);
            }
            if (cases[k].batched) frame(ui, &b, cases[k].first_dt);
            frames(ui, &b, 2);
            printf("double-click: modal=%d case=%u\n", modal, (unsigned)k);
            const char *expected = cases[k].opens ? "/one/Albums" : "/one";
            CHECK(!strcmp(b.listed_path, expected));
            if (b.picker && cases[k].opens) {
                click_label(ui, &b, "Choose");
                CHECK(!b.open && !strcmp(b.out, "/one/Albums"));
            } else if (!b.picker) CHECK(!strcmp(b.path, expected));
        }
        cui_destroy(ui);
    }
}

/** Exercise escaped mount names, filtering, refresh and picker reopening. */
static void mount_fixture(const char *fixture) {
    cui_ctx *ui = context(false);
    browser b = { "/mnt/Music/Albums" };
    frames(ui, &b, 3);
    CHECK(label(ui, "Music") != NULL);
    cui_node *music = label(ui, "Music"), *root = label(ui, "File system");
    CHECK(music && music->font == CUI_FONT_MEDIUM && root && root->font != CUI_FONT_MEDIUM);
    CHECK(label(ui, "USB Drive") != NULL);
    CHECK(label(ui, "kernel") == NULL);
    CHECK(label(ui, "containers") == NULL);
    CHECK(label(ui, "hosts") == NULL);   /* a container's configuration file, bound from the host disk */
    click_label(ui, &b, "USB Drive");
    CHECK(!strcmp(b.path, "/mnt/USB Drive"));
    FILE *fp = fopen(fixture, "w"); CHECK(fp != NULL);
    if (fp) { fputs("/dev/demo /mnt/Backup ext4 rw 0 0\n", fp); fclose(fp); }
    frames(ui, &b, 2); CHECK(label(ui, "Music") != NULL); /* cached */
    refresh(ui, &b);
    CHECK(label(ui, "Backup") != NULL && label(ui, "Music") == NULL);
    b.picker = true; b.open = true; frames(ui, &b, 5);
    CHECK(label(ui, "Backup") != NULL);
    b.open = false; frames(ui, &b, 2);
    fp = fopen(fixture, "w"); CHECK(fp != NULL);
    if (fp) { fputs("/dev/demo /mnt/Reconnected ext4 rw 0 0\n", fp); fclose(fp); }
    b.open = true; frames(ui, &b, 5);
    CHECK(label(ui, "Reconnected") != NULL && label(ui, "Backup") == NULL);
    cui_destroy(ui);
}

/** Save actual CUI picker pixels from the isolated, neutral fixture. */
static void screenshot(const char *dir, bool dark, bool home) {
    cui_ctx *ui = context(dark);
    browser b = { "/mnt/Music" }; b.picker = true; b.open = true;
    if (home) snprintf(b.path, sizeof b.path, "/home/demo");
    frames(ui, &b, 8);
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * (size_t)dd->height_px * 4);
    CHECK(px != NULL);
    if (px) {
        cui_sw_render(sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
        char path[2048]; snprintf(path, sizeof path, "%s/%s%s.png", dir, home ? "home" : "file-picker", dark ? "-dark" : "");
        CHECK(cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4));
        free(px);
    }
    cui_destroy(ui);
}

/** Run regressions, or render the same fixture against a before/after library. */
int main(int argc, char **argv) {
    sw = cui_sw_create();
    if (argc == 3 && (!strcmp(argv[1], "--shots") || !strcmp(argv[1], "--home-shots"))) {
        bool home = !strcmp(argv[1], "--home-shots");
        screenshot(argv[2], false, home); screenshot(argv[2], true, home);
    }
    else {
        custom_places(); native_places(); idle_double_click();
        const char *fixture = getenv("CUI_TEST_MOUNTS");
        if (fixture) mount_fixture(fixture);
    }
    cui_sw_destroy(sw);
    printf("file places: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
