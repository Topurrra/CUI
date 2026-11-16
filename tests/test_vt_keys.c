/* Tests for the Windows key records of the VT view (mode 9001, `.win32_keys`): the exact bytes of a table of keys and
 * modifiers, text (composed, beyond the BMP, AltGr), repeat and release, and that everything else is unchanged:
 * without the option or the mode the bytes of a terminal, mouse, focus and paste as before. Runs headless. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)

enum { SH = 16, CT = 8, AL = 2, RAL = 1, ENH = 256 };

static cui_sw_renderer *g_sw;
typedef struct { cui_vt *vt; cui_vt_result r; bool focus, keys, mouse, freports; int mods; char acc[32768]; int acc_n; char paste_copy[64]; } scene;

static void build(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60.0f);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) { s->r = cui_vt_view(ui, "t", s->vt, .focus = s->focus, .win32_keys = s->keys, .mouse = s->mouse, .focus_reports = s->freports); }
    cui_end_frame(ui);
    s->focus = false;
}

/* a frame; what it sent is appended to acc */
static void frame(cui_ctx *ui, scene *s) {
    build(ui, s);
    if (s->r.input_len && s->acc_n + s->r.input_len < (int)sizeof s->acc) { memcpy(s->acc + s->acc_n, s->r.input, (size_t)s->r.input_len); s->acc_n += s->r.input_len; s->acc[s->acc_n] = 0; }
}
static void clear(scene *s) { s->acc_n = 0; s->acc[0] = 0; }

static void set_mods(cui_ctx *ui, scene *s, int m) { cui_input_key(ui, CUI_KEY_NONE, true, m); s->mods = m; frame(ui, s); }

/* the key goes down (with text, if any), a frame, then up, a frame */
static void tap(cui_ctx *ui, scene *s, cui_key k, int mods, const char *text) {
    if (mods != s->mods) set_mods(ui, s, mods);
    clear(s);
    cui_input_key(ui, k, true, mods);
    if (text) cui_input_text(ui, text);
    frame(ui, s);
    cui_input_key(ui, k, false, mods);
    frame(ui, s);
}

static char want[256];
static const char *rec1(int vk, int sc, int uc, int kd, int cs) { static char b[4][64]; static int i; i = (i + 1) & 3; snprintf(b[i], sizeof b[i], "\x1b[%d;%d;%d;%d;%d;1_", vk, sc, uc, kd, cs); return b[i]; }
/* a press and its release */
static const char *pair(int vk, int sc, int uc, int cs) { snprintf(want, sizeof want, "%s", rec1(vk, sc, uc, 1, cs)); strncat(want, rec1(vk, sc, uc, 0, cs), sizeof want - strlen(want) - 1); return want; }
static bool is(const scene *s, const char *bytes) { return s->acc_n == (int)strlen(bytes) && !memcmp(s->acc, bytes, (size_t)s->acc_n); }
static void show(const scene *s, const char *what) { printf("  %s: got ", what); for (int i = 0; i < s->acc_n; i++) printf(s->acc[i] == 0x1b ? "^[" : "%c", s->acc[i]); printf("\n"); }

static cui_ctx *make(scene *s, bool keys) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(g_sw), .theme = CUI_THEME_DARK });
    cui_set_viewport(ui, 640, 400, 1);
    cui_set_reduced_motion(ui, true);
    memset(s, 0, sizeof *s);
    s->vt = cui_vt_create(80, 24, 1000); s->focus = true; s->keys = keys; s->mouse = true; s->freports = true;
    for (int i = 0; i < 3; i++) build(ui, s);
    return ui;
}

typedef struct { cui_key k; int mods; const char *text; int vk, sc, uc, cs; const char *name; } row_t;

static void test_table(void) {
    scene s; cui_ctx *ui = make(&s, true);
    CHECK(!cui_vt_win32_input(s.vt));
    cui_vt_write(s.vt, "\x1b[?9001h", 8);
    CHECK(cui_vt_win32_input(s.vt));
    static const row_t t[] = {
        { CUI_KEY_ENTER, 0, NULL, 13, 28, 13, 0, "Enter" },
        { CUI_KEY_ENTER, CUI_MOD_SHIFT, NULL, 13, 28, 13, SH, "Shift+Enter" },
        { CUI_KEY_ENTER, CUI_MOD_CTRL, NULL, 13, 28, 10, CT, "Ctrl+Enter" },
        { CUI_KEY_ENTER, CUI_MOD_ALT, NULL, 13, 28, 13, AL, "Alt+Enter" },
        { CUI_KEY_BACKSPACE, 0, NULL, 8, 14, 8, 0, "Backspace" },
        { CUI_KEY_BACKSPACE, CUI_MOD_CTRL, NULL, 8, 14, 127, CT, "Ctrl+Backspace" },
        { CUI_KEY_BACKSPACE, CUI_MOD_ALT, NULL, 8, 14, 8, AL, "Alt+Backspace" },
        { CUI_KEY_TAB, 0, NULL, 9, 15, 9, 0, "Tab" },
        { CUI_KEY_TAB, CUI_MOD_SHIFT, NULL, 9, 15, 9, SH, "Shift+Tab" },
        { CUI_KEY_TAB, CUI_MOD_CTRL, NULL, 9, 15, 0, CT, "Ctrl+Tab" },
        { CUI_KEY_ESCAPE, 0, NULL, 27, 1, 27, 0, "Escape" },
        { CUI_KEY_SPACE, CUI_MOD_CTRL, NULL, 32, 57, 0, CT, "Ctrl+Space" },
        { CUI_KEY_SPACE, 0, " ", 32, 57, 32, 0, "Space" },
        { CUI_KEY_SPACE, CUI_MOD_SHIFT, " ", 32, 57, 32, SH, "Shift+Space" },
        { CUI_KEY_LEFT, 0, NULL, 37, 75, 0, ENH, "Left" },
        { CUI_KEY_LEFT, CUI_MOD_CTRL, NULL, 37, 75, 0, CT | ENH, "Ctrl+Left" },
        { CUI_KEY_RIGHT, CUI_MOD_SHIFT | CUI_MOD_CTRL, NULL, 39, 77, 0, SH | CT | ENH, "Ctrl+Shift+Right" },
        { CUI_KEY_UP, 0, NULL, 38, 72, 0, ENH, "Up" },
        { CUI_KEY_DOWN, CUI_MOD_ALT, NULL, 40, 80, 0, AL | ENH, "Alt+Down" },
        { CUI_KEY_HOME, 0, NULL, 36, 71, 0, ENH, "Home" },
        { CUI_KEY_END, 0, NULL, 35, 79, 0, ENH, "End" },
        { CUI_KEY_PAGE_UP, 0, NULL, 33, 73, 0, ENH, "PageUp" },
        { CUI_KEY_PAGE_DOWN, 0, NULL, 34, 81, 0, ENH, "PageDown" },
        { CUI_KEY_DELETE, 0, NULL, 46, 83, 0, ENH, "Delete" },
        { CUI_KEY_F1, 0, NULL, 112, 59, 0, 0, "F1" },
        { CUI_KEY_F5, 0, NULL, 116, 63, 0, 0, "F5" },
        { CUI_KEY_F5, CUI_MOD_CTRL, NULL, 116, 63, 0, CT, "Ctrl+F5" },
        { CUI_KEY_F10, CUI_MOD_SHIFT, NULL, 121, 68, 0, SH, "Shift+F10" },
        { CUI_KEY_F11, 0, NULL, 122, 87, 0, 0, "F11" },
        { CUI_KEY_F12, CUI_MOD_ALT, NULL, 123, 88, 0, AL, "Alt+F12" },
        { CUI_KEY_A, 0, "a", 65, 30, 97, 0, "a" },
        { CUI_KEY_A, CUI_MOD_SHIFT, "A", 65, 30, 65, SH, "A" },
        { CUI_KEY_Z, 0, "z", 90, 44, 122, 0, "z" },
        { CUI_KEY_C, CUI_MOD_CTRL, NULL, 67, 46, 3, CT, "Ctrl+C" },
        { CUI_KEY_A, CUI_MOD_CTRL, NULL, 65, 30, 1, CT, "Ctrl+A" },
        { CUI_KEY_Z, CUI_MOD_CTRL | CUI_MOD_SHIFT, NULL, 90, 44, 26, CT | SH, "Ctrl+Shift+Z" },
        { CUI_KEY_F, CUI_MOD_ALT, NULL, 70, 33, 102, AL, "Alt+f" },
        { CUI_KEY_F, CUI_MOD_ALT | CUI_MOD_SHIFT, NULL, 70, 33, 70, AL | SH, "Alt+Shift+F" },
        { CUI_KEY_1, 0, "1", 49, 2, 49, 0, "1" },
        { CUI_KEY_1, CUI_MOD_SHIFT, "!", 49, 2, 33, SH, "Shift+1" },
        { CUI_KEY_0, 0, "0", 48, 11, 48, 0, "0" },
        { CUI_KEY_9, CUI_MOD_CTRL, NULL, 57, 10, 0, CT, "Ctrl+9" },
        { CUI_KEY_MINUS, 0, "-", 189, 12, 45, 0, "-" },
        { CUI_KEY_MINUS, CUI_MOD_CTRL, NULL, 189, 12, 31, CT, "Ctrl+-" },
        { CUI_KEY_EQUAL, CUI_MOD_SHIFT, "+", 187, 13, 43, SH, "+" },
        { CUI_KEY_COMMA, 0, ",", 188, 51, 44, 0, "," },
        { CUI_KEY_PERIOD, 0, ".", 190, 52, 46, 0, "." },
        { CUI_KEY_SLASH, 0, "/", 191, 53, 47, 0, "/" },
        { CUI_KEY_SLASH, CUI_MOD_SHIFT, "?", 191, 53, 63, SH, "?" },
        { CUI_KEY_BACKSLASH, 0, "\\", 220, 43, 92, 0, "\\" },
        { CUI_KEY_BACKSLASH, CUI_MOD_CTRL, NULL, 220, 43, 28, CT, "Ctrl+\\" },
        { CUI_KEY_GRAVE, CUI_MOD_SHIFT, "~", 192, 41, 126, SH, "~" },
        { CUI_KEY_A, 0, NULL, 65, 30, 0, 0, "a key with no text (a dead key)" },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        tap(ui, &s, t[i].k, t[i].mods, t[i].text);
        if (!is(&s, pair(t[i].vk, t[i].sc, t[i].uc, t[i].cs))) { g_failed++; printf("  FAIL %s\n  expected ", t[i].name); const char *w = want; for (; *w; w++) printf(*w == 0x1b ? "^[" : "%c", *w); printf("\n"); show(&s, t[i].name); }
        g_checks++;
    }
    /* one literal, so the table's helper is not checking itself */
    set_mods(ui, &s, 0);
    tap(ui, &s, CUI_KEY_ENTER, CUI_MOD_SHIFT, NULL);
    CHECK(is(&s, "\x1b[13;28;13;1;16;1_\x1b[13;28;13;0;16;1_"));
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

static void test_modifiers(void) {
    scene s; cui_ctx *ui = make(&s, true);
    cui_vt_write(s.vt, "\x1b[?9001h", 8);
    clear(&s);
    set_mods(ui, &s, CUI_MOD_SHIFT); CHECK(is(&s, rec1(16, 42, 0, 1, SH)));
    clear(&s); set_mods(ui, &s, 0);  CHECK(is(&s, rec1(16, 42, 0, 0, 0)));
    clear(&s); set_mods(ui, &s, CUI_MOD_CTRL); CHECK(is(&s, rec1(17, 29, 0, 1, CT)));
    clear(&s); set_mods(ui, &s, CUI_MOD_CTRL | CUI_MOD_ALT); CHECK(is(&s, rec1(18, 56, 0, 1, CT | AL)));
    clear(&s); set_mods(ui, &s, 0);
    { char e[160]; snprintf(e, sizeof e, "%s", rec1(17, 29, 0, 0, AL)); strncat(e, rec1(18, 56, 0, 0, 0), sizeof e - strlen(e) - 1); CHECK(is(&s, e)); }       /* releases in the order Shift, Ctrl, Alt */
    s.acc_n = 0;
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

static void test_release_and_repeat(void) {
    scene s; cui_ctx *ui = make(&s, true);
    cui_vt_write(s.vt, "\x1b[?9001h", 8);
    /* a held key: the press now, the release when it goes up, nothing between */
    clear(&s);
    cui_input_key(ui, CUI_KEY_LEFT, true, 0); frame(ui, &s);
    CHECK(is(&s, rec1(37, 75, 0, 1, ENH)));
    clear(&s); frame(ui, &s); CHECK_EQ(s.acc_n, 0);
    cui_input_key(ui, CUI_KEY_LEFT, true, 0); cui_input_key(ui, CUI_KEY_LEFT, true, 0); frame(ui, &s);      /* auto repeat: two more presses, no release */
    CHECK(is(&s, "\x1b[37;75;0;1;256;1_\x1b[37;75;0;1;256;1_"));
    clear(&s); cui_input_key(ui, CUI_KEY_LEFT, false, 0); frame(ui, &s);
    CHECK(is(&s, rec1(37, 75, 0, 0, ENH)));
    /* three presses in one frame are three records and one release */
    clear(&s);
    for (int i = 0; i < 3; i++) cui_input_key(ui, CUI_KEY_BACKSPACE, true, 0);
    cui_input_key(ui, CUI_KEY_BACKSPACE, false, 0);
    frame(ui, &s);
    CHECK(is(&s, "\x1b[8;14;8;1;0;1_\x1b[8;14;8;1;0;1_\x1b[8;14;8;1;0;1_\x1b[8;14;8;0;0;1_"));
    /* the release repeats the character of the press */
    tap(ui, &s, CUI_KEY_A, 0, "a");
    CHECK(is(&s, "\x1b[65;30;97;1;0;1_\x1b[65;30;97;0;0;1_"));
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

static void test_text(void) {
    scene s; cui_ctx *ui = make(&s, true);
    cui_vt_write(s.vt, "\x1b[?9001h", 8);
    /* text with no key (an IME, a composed character): Vk 0, down and up */
    clear(&s); cui_input_text(ui, "\xc3\xa9"); frame(ui, &s);
    CHECK(is(&s, "\x1b[0;0;233;1;0;1_\x1b[0;0;233;0;0;1_"));
    /* a character beyond the BMP is two UTF-16 records */
    clear(&s); cui_input_text(ui, "\xf0\x9f\x98\x80"); frame(ui, &s);
    CHECK(is(&s, "\x1b[0;0;55357;1;0;1_\x1b[0;0;55357;0;0;1_\x1b[0;0;56832;1;0;1_\x1b[0;0;56832;0;0;1_"));
    /* a key that types another character (another layout) takes its text: once, in its record */
    tap(ui, &s, CUI_KEY_MINUS, 0, "\xc3\x9f");
    CHECK(is(&s, pair(189, 12, 223, 0)));
    /* AltGr arrives as Ctrl+Alt and a character: Left Ctrl and Right Alt, the character once */
    set_mods(ui, &s, CUI_MOD_CTRL | CUI_MOD_ALT);
    tap(ui, &s, CUI_KEY_Q, CUI_MOD_CTRL | CUI_MOD_ALT, "@");
    if (!is(&s, pair(81, 16, 64, CT | RAL))) show(&s, "altgr"); CHECK(is(&s, pair(81, 16, 64, CT | RAL)));
    set_mods(ui, &s, 0);
    /* two characters in one frame, typed fast, each with its key in the order of the text */
    clear(&s);
    cui_input_key(ui, CUI_KEY_B, true, 0); cui_input_key(ui, CUI_KEY_A, true, 0); cui_input_text(ui, "ba");
    cui_input_key(ui, CUI_KEY_B, false, 0); cui_input_key(ui, CUI_KEY_A, false, 0);
    frame(ui, &s);
    CHECK(is(&s, "\x1b[66;48;98;1;0;1_\x1b[65;30;97;1;0;1_\x1b[65;30;97;0;0;1_\x1b[66;48;98;0;0;1_") || is(&s, "\x1b[66;48;98;1;0;1_\x1b[65;30;97;1;0;1_\x1b[66;48;98;0;0;1_\x1b[65;30;97;0;0;1_"));
    /* a long text goes out whole, over frames if it does not fit one */
    clear(&s);
    char big[201]; memset(big, 'x', 200); big[200] = 0;
    cui_input_text(ui, big);
    int downs = 0;
    for (int f = 0; f < 20; f++) frame(ui, &s);
    for (const char *p = s.acc; (p = strstr(p, ";120;")) != NULL; p += 5) downs++;
    CHECK_EQ(downs, 400);                                         /* a down and an up record per character, each with Uc 120 */
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

static void test_unchanged(void) {
    scene s; cui_ctx *ui = make(&s, true);
    /* the mode is not set: the bytes of a terminal, with the option on */
    tap(ui, &s, CUI_KEY_ENTER, 0, NULL); CHECK(is(&s, "\r"));
    tap(ui, &s, CUI_KEY_ENTER, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\n"));
    tap(ui, &s, CUI_KEY_BACKSPACE, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\x08"));
    tap(ui, &s, CUI_KEY_TAB, CUI_MOD_SHIFT, NULL); CHECK(is(&s, "\x1b[Z"));
    tap(ui, &s, CUI_KEY_LEFT, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\x1b[1;5D"));
    tap(ui, &s, CUI_KEY_F5, 0, NULL); CHECK(is(&s, "\x1b[15~"));
    tap(ui, &s, CUI_KEY_F, CUI_MOD_ALT, NULL); CHECK(is(&s, "\x1b" "f"));
    tap(ui, &s, CUI_KEY_C, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\x03"));
    tap(ui, &s, CUI_KEY_SPACE, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\x01" + 1) || s.acc_n == 1);
    set_mods(ui, &s, 0);
    clear(&s); cui_input_text(ui, "ab"); frame(ui, &s); CHECK(is(&s, "ab"));
    /* the mode set, the option off: the same bytes */
    cui_vt_write(s.vt, "\x1b[?9001h", 8);
    s.keys = false;
    tap(ui, &s, CUI_KEY_ENTER, CUI_MOD_CTRL, NULL); CHECK(is(&s, "\n"));
    tap(ui, &s, CUI_KEY_F5, 0, NULL); CHECK(is(&s, "\x1b[15~"));
    clear(&s); cui_input_text(ui, "ab"); frame(ui, &s); CHECK(is(&s, "ab"));
    /* on again, then ?9001l: plain again, and nothing of the records left over */
    s.keys = true;
    tap(ui, &s, CUI_KEY_F5, 0, NULL); CHECK(is(&s, pair(116, 63, 0, 0)));
    cui_input_key(ui, CUI_KEY_LEFT, true, 0); frame(ui, &s);                 /* held when the mode goes off */
    cui_vt_write(s.vt, "\x1b[?9001l", 8);
    CHECK(!cui_vt_win32_input(s.vt));
    clear(&s); cui_input_key(ui, CUI_KEY_LEFT, false, 0); frame(ui, &s); CHECK_EQ(s.acc_n, 0);
    tap(ui, &s, CUI_KEY_ENTER, 0, NULL); CHECK(is(&s, "\r"));
    /* a full reset turns it off; a soft reset leaves it (the console host asks once) */
    cui_vt_write(s.vt, "\x1b[?9001h\x1b[!p", 12);
    CHECK(cui_vt_win32_input(s.vt));
    cui_vt_write(s.vt, "\x1b" "c", 2);
    CHECK(!cui_vt_win32_input(s.vt));
    tap(ui, &s, CUI_KEY_ENTER, 0, NULL); CHECK(is(&s, "\r"));
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

static void set_clip(void *user, const char *text) { snprintf((char *)user, 64, "%s", text); }
static const char *get_clip(void *user) { return (const char *)user; }

static void test_mouse_focus_paste(void) {
    scene s; cui_ctx *ui = make(&s, true);
    cui_vt_write(s.vt, "\x1b[?9001h\x1b[?1000h\x1b[?1006h\x1b[?1004h\x1b[?2004h", 40);
    /* the mouse: the same report as without the records */
    clear(&s);
    cui_input_mouse_move(ui, 10 + 5.5f * 7.8f, 6 + 3.5f * 19.0f); frame(ui, &s);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, &s);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &s);
    CHECK(is(&s, "\x1b[<0;6;4M\x1b[<0;6;4m"));
    /* focus: ESC [ O and ESC [ I as always */
    clear(&s); cui_input_focus(ui, false); frame(ui, &s); CHECK(is(&s, "\x1b[O"));
    clear(&s); cui_input_focus(ui, true); frame(ui, &s); CHECK(is(&s, "\x1b[I"));
    /* paste: Ctrl+V and a right click hand the text over, bracketed as before, and send no record of the V */
    char clip[64] = "pasted text";
    { cui_platform_iface pf = { 0 }; pf.set_clipboard = set_clip; pf.get_clipboard = get_clip; pf.user = clip; cui_set_platform(ui, &pf); }
    clear(&s);
    cui_input_key(ui, CUI_KEY_V, true, CUI_MOD_CTRL); frame(ui, &s);
    CHECK(s.r.paste && s.r.paste_len == 11 && !memcmp(s.r.paste, "pasted text", 11));
    CHECK(s.r.paste_bracketed);
    CHECK(strstr(s.acc, "\x1b[86;") == NULL);
    cui_input_key(ui, CUI_KEY_V, false, CUI_MOD_CTRL); frame(ui, &s);
    set_mods(ui, &s, 0);
    /* Shift+Page Up is the view's (scrollback), not the program's */
    clear(&s);
    set_mods(ui, &s, CUI_MOD_SHIFT);
    cui_input_key(ui, CUI_KEY_PAGE_UP, true, CUI_MOD_SHIFT); frame(ui, &s);
    CHECK(strstr(s.acc, "\x1b[33;") == NULL);
    cui_input_key(ui, CUI_KEY_PAGE_UP, false, CUI_MOD_SHIFT); frame(ui, &s);
    set_mods(ui, &s, 0);
    /* Ctrl+Shift+C without a selection is a copy key: not sent */
    clear(&s);
    set_mods(ui, &s, CUI_MOD_CTRL | CUI_MOD_SHIFT);
    cui_input_key(ui, CUI_KEY_C, true, CUI_MOD_CTRL | CUI_MOD_SHIFT); frame(ui, &s);
    CHECK(strstr(s.acc, "\x1b[67;") == NULL);
    cui_destroy(ui); cui_vt_destroy(s.vt);
}

int main(void) {
    g_sw = cui_sw_create();
    test_table();
    test_modifiers();
    test_release_and_repeat();
    test_text();
    test_unchanged();
    test_mouse_focus_paste();
    cui_sw_destroy(g_sw);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
