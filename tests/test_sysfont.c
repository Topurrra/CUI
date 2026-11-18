/* test_sysfont.c - cui_font_use_system_fallback: characters of other scripts come from the system's fonts.
 * Windows only (elsewhere the option does nothing and the test passes at once). A script whose font file
 * is not on this machine is skipped with a message. Headless: the software renderer.
 *
 *   test_sysfont                 the checks, and the time and memory each script costs
 *   test_sysfont --shot f.png    mixed-script text in the four text roles, drawn headless (for a look) */
#include <cui/cui.h>
#include <cui/cui_render.h>
#include "../src/cui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_failed;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if !defined(_WIN32)
int main(void) { printf("test_sysfont: skipped (no system font table on this platform)\n"); return 0; }
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>

static cui_sw_renderer *g_sw;

typedef struct { const char *name, *text, *file; } script_t;
static const script_t S[] = {
    { "Cyrillic", "Привет мир", "segoeui.ttf" },
    { "Georgian", "გამარჯობა", "segoeui.ttf" },
    { "Greek", "Γειά σου κόσμε", "segoeui.ttf" },
    { "Hebrew", "שלום עולם", "segoeui.ttf" },
    { "Arabic", "مرحبا بالعالم", "segoeui.ttf" },
    { "Chinese", "中文", "msyh.ttc" },
    { "Japanese", "日本語", "YuGothR.ttc" },
    { "Korean", "한국어", "malgun.ttf" },
    { "Hindi", "नमस्ते", "Nirmala.ttc" },
    { "Thai", "สวัสดี", "LeelawUI.ttf" },
    { "Armenian", "Բարեւ աշխարհ", "segoeui.ttf" },
    { "Ethiopic", "ሰላም", "ebrima.ttf" },
    { "Khmer", "សួស្តី", "LeelawUI.ttf" },
    { "Symbols", "♫ ⌬ ⍝", "seguisym.ttf" },
};
#define NS ((int)(sizeof S / sizeof *S))

static bool have_file(const char *f) {
    char p[300];
    const char *w = getenv("WINDIR");
    snprintf(p, sizeof p, "%s\\Fonts\\%s", w ? w : "C:\\Windows", f);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static cui_ctx *new_ctx(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw) });
    cui_set_viewport(ui, 480, 120, 1);
    cui_set_reduced_motion(ui, true);
    return ui;
}

static float width(cui_ctx *ui, cui_font role, const char *s) { return cui_measure_text(ui, role, 14, s, -1).width; }

/* every character of s that is not a space has a glyph that is not .notdef, in a font other than the role's own */
static int glyphs_ok(cui_ctx *ui, cui_font role, const char *s) {
    int own = cui__resolve_font(ui, role);
    for (int i = 0; s[i];) {
        uint32_t cp;
        i += cui__utf8_decode(s + i, (int)strlen(s + i), &cp);
        if (cp == ' ') continue;
        cui_font f = -1;
        int gi = cui__font_glyph(ui, role, cp, &f);
        if (!gi || f == own || f < 0) return 0;
    }
    return 1;
}

/* the number of pixels that are not the background when the text is drawn alone */
static int ink(cui_ctx *ui, cui_font role, float size, const char *s) {
    for (int k = 0; k < 3; k++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 20) cui_text(ui, s, .font = role, .size = size, .nowrap = true);
        cui_end_frame(ui);
    }
    const cui_draw_data *dd = cui_get_draw_data(ui);
    int w = dd->width_px, h = dd->height_px;
    unsigned char *px = malloc((size_t)w * h * 4);
    cui_sw_render(g_sw, dd, px, w, h, w * 4, CUI_SW_RGBA);
    int n = 0;
    for (int i = 1; i < w * h; i++)
        if (memcmp(px + (size_t)i * 4, px, 3)) n++;
    free(px);
    return n;
}

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}
static size_t priv_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX m;
    memset(&m, 0, sizeof m);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&m, sizeof m);
    return m.PrivateUsage;
}

static const char *LATIN[] = { "Hello, world 123", "The quick brown fox: 3:42 (ok)", "ÀÉÎõü ñ ß" };

static void test_off_and_latin(void) {
    cui_ctx *ui = new_ctx();
    cui_font roles[4] = { CUI_FONT_BODY, CUI_FONT_MEDIUM, CUI_FONT_DISPLAY, CUI_FONT_MONO };
    float lat_off[4][3], cyr_off[4];
    for (int r = 0; r < 4; r++) {
        for (int k = 0; k < 3; k++) lat_off[r][k] = width(ui, roles[r], LATIN[k]);
        cyr_off[r] = width(ui, roles[r], S[0].text);
    }
    int fonts0 = ui->font_count;
    CHECK(!glyphs_ok(ui, CUI_FONT_BODY, S[0].text));            /* off: nothing draws */
    CHECK(ink(ui, CUI_FONT_BODY, 14, S[0].text) == 0);
    cui_font_use_system_fallback(ui, true);
    for (int k = 0; k < 3; k++) (void)width(ui, CUI_FONT_BODY, LATIN[k]);
    CHECK(ui->font_count == fonts0);                            /* Latin never reaches the lookup, nothing is loaded */
    /* load several scripts, then Latin must still measure exactly the same */
    for (int i = 0; i < NS; i++) (void)width(ui, CUI_FONT_BODY, S[i].text);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 3; k++) CHECK(width(ui, roles[r], LATIN[k]) == lat_off[r][k]);
    /* off again: exactly the old widths, and the loaded fonts are not used */
    cui_font_use_system_fallback(ui, false);
    for (int r = 0; r < 4; r++) CHECK(width(ui, roles[r], S[0].text) == cyr_off[r]);
    CHECK(!glyphs_ok(ui, CUI_FONT_BODY, S[0].text));
    CHECK(ink(ui, CUI_FONT_BODY, 14, S[0].text) == 0);
    cui_destroy(ui);
    /* a fresh context that never asked: the same */
    ui = new_ctx();
    for (int r = 0; r < 4; r++) CHECK(width(ui, roles[r], S[0].text) == cyr_off[r]);
    cui_destroy(ui);
}

static void test_scripts(void) {
    cui_ctx *ui = new_ctx();
    cui_font_use_system_fallback(ui, true);
    printf("  %-9s %9s %12s\n", "script", "load ms", "private KB");
    for (int i = 0; i < NS; i++) {
        if (!have_file(S[i].file)) { printf("  %-9s skipped: %s is not on this machine\n", S[i].name, S[i].file); continue; }
        int before = ui->font_count;
        size_t pb = priv_bytes();
        double t0 = now_ms();
        float w = width(ui, CUI_FONT_BODY, S[i].text);
        double ms = now_ms() - t0;
        long dkb = ((long)priv_bytes() - (long)pb) / 1024;
        printf("  %-9s %9.2f %12ld   (%d font%s added)\n", S[i].name, ms, dkb, ui->font_count - before, ui->font_count - before == 1 ? "" : "s");
        CHECK(w > 0);
        CHECK(glyphs_ok(ui, CUI_FONT_BODY, S[i].text));
        CHECK(glyphs_ok(ui, CUI_FONT_MEDIUM, S[i].text));
        CHECK(glyphs_ok(ui, CUI_FONT_DISPLAY, S[i].text));
        CHECK(glyphs_ok(ui, CUI_FONT_MONO, S[i].text));
        CHECK(width(ui, CUI_FONT_MEDIUM, S[i].text) > 0);
        CHECK(width(ui, CUI_FONT_MONO, S[i].text) > 0);
        if (!glyphs_ok(ui, CUI_FONT_BODY, S[i].text)) printf("    no glyph for %s\n", S[i].name);
        /* and it draws */
        cui_ctx *u2 = new_ctx();
        cui_font_use_system_fallback(u2, true);
        int px = ink(u2, CUI_FONT_BODY, 16, S[i].text);
        CHECK(px > 30);
        cui_destroy(u2);
    }
    /* the icons font gets none */
    CHECK(cui__font_glyph(ui, CUI_FONT_ICONS, 0x416, NULL) == 0);
    /* a character nobody has: not found, and asking again loads nothing more */
    int n0 = ui->font_count;
    CHECK(cui__font_glyph(ui, CUI_FONT_BODY, 0x0378, NULL) == 0);
    int n1 = ui->font_count;
    for (int k = 0; k < 2000; k++) (void)cui__font_glyph(ui, CUI_FONT_BODY, 0x0378, NULL);
    CHECK(ui->font_count == n1 && n1 >= n0);
    /* the medium and display roles take the semibold face for Cyrillic, the body role the regular one */
    cui_font fb, fm;
    int gb = cui__font_glyph(ui, CUI_FONT_BODY, 0x416, &fb), gm = cui__font_glyph(ui, CUI_FONT_MEDIUM, 0x416, &fm);
    CHECK(gb && gm);
    if (have_file("seguisb.ttf")) CHECK(fb != fm);
    /* an app's own fallback is tried before the system */
    cui_destroy(ui);
}

/* the app's own fallback font wins over the system one */
static void test_app_fallback_first(void) {
    cui_ctx *ui = new_ctx();
    cui_font own = cui_font_load_file(ui, "C:\\Windows\\Fonts\\segoeui.ttf");
    if (own < 0) { printf("  app fallback: skipped (no segoeui.ttf)\n"); cui_destroy(ui); return; }
    cui_font_add_fallback(ui, CUI_FONT_BODY, own);
    cui_font_use_system_fallback(ui, true);
    int n0 = ui->font_count;
    cui_font f = -1;
    CHECK(cui__font_glyph(ui, CUI_FONT_BODY, 0x416, &f) != 0);
    CHECK(f == cui__resolve_font(ui, own));
    CHECK(ui->font_count == n0);                               /* nothing was loaded for it */
    /* more fonts than the old limit of four fallbacks still leave the system chain working */
    for (int i = 0; i < 6; i++) cui_font_add_fallback(ui, CUI_FONT_BODY, own);
    CHECK(glyphs_ok(ui, CUI_FONT_BODY, "中文") || !have_file("msyh.ttc"));
    cui_destroy(ui);
}

/* text measured before a script was there is measured again: the width changes at once, in the same frame */
static void test_midrun_load(void) {
    cui_ctx *ui = new_ctx();
    float off = width(ui, CUI_FONT_BODY, "Привет");
    cui_font_use_system_fallback(ui, true);
    for (int i = 0; i < 3; i++) { cui_begin_frame(ui, 0.016f); cui_end_frame(ui); }
    float on = width(ui, CUI_FONT_BODY, "Привет");
    CHECK(on > 0 && on != off);
    cui_destroy(ui);
}

/* --shot: mixed scripts in the four text roles; Latin on the same lines for the baseline and the size */
static void shot(const char *path) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_LIGHT });
    cui_set_viewport(ui, 980, 760, 1);
    cui_set_reduced_motion(ui, true);
    cui_font_use_system_fallback(ui, true);
    static const char *lines[] = {
        "Hello Привет мир Γειά σου გამარჯობა Բարեւ",
        "Hello שלום עולם مرحبا بالعالم Hello",
        "Hello 中文 日本語 한국어 Hello",
        "Hello नमस्ते สวัสดี សួស្តី ሰላም Hello",
        "Hxgh Нxgh 中xgh Ωxgh  → ≠ ∑ € ₽ ™ №",
    };
    cui_font roles[4] = { CUI_FONT_BODY, CUI_FONT_MEDIUM, CUI_FONT_DISPLAY, CUI_FONT_MONO };
    static const char *names[4] = { "body 15", "medium 15", "display 28", "mono 15" };
    float sizes[4] = { 15, 15, 28, 15 };
    for (int k = 0; k < 4; k++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        cui_column(ui, .pad = 16, .gap = 4) {
            for (int r = 0; r < 4; r++) {
                cui_text(ui, names[r], .size = 11, .color = CUI_RGB(0x808080));
                int n = r == 2 ? 3 : 5;
                for (int l = 0; l < n; l++) cui_text(ui, lines[l], .font = roles[r], .size = sizes[r], .nowrap = true);
            }
        }
        cui_end_frame(ui);
    }
    const cui_draw_data *dd = cui_get_draw_data(ui);
    unsigned char *px = malloc((size_t)dd->width_px * dd->height_px * 4);
    cui_sw_render(g_sw, dd, px, dd->width_px, dd->height_px, dd->width_px * 4, CUI_SW_RGBA);
    cui_write_png(path, px, dd->width_px, dd->height_px, dd->width_px * 4);
    free(px);
    cui_destroy(ui);
}

int main(int argc, char **argv) {
    g_sw = cui_sw_create();
    if (argc > 2 && !strcmp(argv[1], "--shot")) { shot(argv[2]); cui_sw_destroy(g_sw); return 0; }
    test_off_and_latin();
    test_scripts();
    test_app_fallback_first();
    test_midrun_load();
    cui_sw_destroy(g_sw);
    printf("test_sysfont: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
#endif
