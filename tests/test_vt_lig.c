/* The terminal's ligatures (cui_vt_look.ligatures): the shaper's GSUB-only path (cui__shape_plan, cui__shape_gsub).
 *   - Geist Mono (CUI's own mono font) has no ligatures: its plan covers no character.
 *   - Cascadia Code (on Windows 11; skipped where it is not installed) has them: => != -> === <= www change, the
 *     glyph count stays (calt with spacers), letters and digits start no lookup, the digest never hides a lookup.
 *   - Cascadia Mono and Consolas have none.
 *   - cui_text.c shapes a word in pieces (each run of covering characters with one neighbour on each side): on a
 *     corpus of code, every piece gives the glyphs the whole word gives.
 * Fonts are read from disk; the test is plain C with cui_shape.c, so it runs whatever the build's shaping options. */
#include "../src/cui_shape.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define TAG(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define LATN TAG('l', 'a', 't', 'n')
static const uint32_t LIG[] = { TAG('r','l','i','g'), TAG('c','a','l','t'), TAG('r','c','l','t'), TAG('l','i','g','a'), TAG('c','l','i','g') };

static unsigned char *load(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    unsigned char *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *size = b ? (size_t)n : 0;
    return b;
}

/* glyphs of text, one per cell (0 when a cell has none); returns the glyph count from the shaper */
static int shape(cui_shape_font *f, const cui_shape_plan *p, const char *t, int n, int *cells) {
    cui_shape_glyph g[512];
    for (int i = 0; i < n; i++) g[i] = (cui_shape_glyph){ (uint8_t)t[i], 0, i, 0, 0, 0, 0, 0, 0 };
    int m = cui__shape_gsub(f, p, g, n, 512);
    for (int i = 0; i < n; i++) cells[i] = 0;
    for (int i = 0; i < m; i++) if (g[i].cluster >= 0 && g[i].cluster < n && !cells[g[i].cluster]) cells[g[i].cluster] = g[i].id;
    return m;
}

static bool changes(cui_shape_font *f, const cui_shape_plan *p, const char *t) {
    int cells[64], n = (int)strlen(t), m = shape(f, p, t, n, cells);
    if (m != n) return true;
    for (int i = 0; i < n; i++) if (cells[i] != p->ascii[(uint8_t)t[i]]) return true;
    return false;
}

static void test_geist(const char *dir) {
    char path[600];
    snprintf(path, sizeof path, "%s/assets/fonts/GeistMono-Regular.ttf", dir);
    size_t n;
    unsigned char *b = load(path, &n);
    CHECK(b != NULL);
    if (!b) return;
    cui_shape_font f;
    CHECK(cui__shape_font_init(&f, b, n, 0));
    static cui_shape_plan p;
    cui__shape_plan(&f, LATN, LIG, 5, &p);
    int covered = 0;
    for (int c = 0; c < 128; c++) covered += p.cover[c];
    CHECK(covered == 0);                                    /* nothing to shape: the terminal draws it as before */
    CHECK(!changes(&f, &p, "=>") && !changes(&f, &p, "!=") && !changes(&f, &p, "www"));
    CHECK(p.ascii['A'] == cui__shape_map(&f, 'A'));
    printf("Geist Mono: %d lookups, nothing covered\n", p.count);
    free(b);
}

static const char *CORPUS[] = {
    "if (p->next != NULL && x <= 10) { return a === b ? \"=>\" : www_17->y; } // -- done ...",
    "for (int i = 0; i < n; i++) x[i] <<= 2; y >>= 1; z ||= w; q &&= r; a ?? b ?. c",
    "<!-- html --> <div></div> </a> <a href=\"https://www.example.com/x?y=1&z=2\">x</a> /* c */ /** doc */",
    "fn main() -> Result<(), Box<dyn Error>> { let x = vec![1, 2, 3]; x.iter().map(|v| v * 2); Ok(()) }",
    "a::b::c => d |> e <| f <$> g <*> h >>= i =<< j <=> k ~~ l ~> m -< n >- o ++ p -- q ** r",
    "$ ls -la | grep -v '^d' && echo ok || echo no; [[ -f x ]] && cat x >> y 2>&1; echo $((1+2))",
    "0xFF 0b1010 1_000_000 3.14e-10 x+=1 y-=2 z*=3 w/=4 v%=5 u^=6 #include <stdio.h> #define X(a) a##b",
    "===== ----- ##### ***** ..... ::::: ;;;;; ~~~~~ _____ ||||| &&&&& >>>>> <<<<< ///// \\\\\\",
    "www.wwwww wwww-www w_w_w www123 <=< >=> =/= !== =!= <-> <=> <== ==> -->> <<-- |-> <-| /\\ \\/",
};

/* a word in pieces as cui_text.c draws it: each run of covered characters with one neighbour each side; true when every
 * piece's cells equal the whole word's (and the neighbours come out unchanged) */
static bool pieces_match(cui_shape_font *f, const cui_shape_plan *p, const char *w, int n, int *bad_at) {
    int whole[512], part[512];
    shape(f, p, w, n, whole);
    for (int k = 0; k < n;) {
        if (!p->cover[(uint8_t)w[k] & 127]) { if (whole[k] != p->ascii[(uint8_t)w[k]]) { *bad_at = k; return false; } k++; continue; }
        int r0 = k;
        while (k < n && p->cover[(uint8_t)w[k] & 127]) k++;
        int s0 = r0 > 0 ? r0 - 1 : r0, s1 = k < n ? k + 1 : k;
        shape(f, p, w + s0, s1 - s0, part);
        for (int q = r0; q < k; q++) if (part[q - s0] != whole[q]) { *bad_at = q; return false; }
    }
    return true;
}

static void test_cascadia(void) {
    size_t n;
    unsigned char *b = load("C:/Windows/Fonts/CascadiaCode.ttf", &n);
    if (!b) { printf("Cascadia Code: not installed, skipped\n"); return; }
    cui_shape_font f;
    CHECK(cui__shape_font_init(&f, b, n, 0));
    static cui_shape_plan p;
    CHECK(cui__shape_plan(&f, LATN, LIG, 5, &p) > 0);
    const char *lig[] = { "=>", "!=", "->", "===", "<=", ">=", "==", "&&", "||", "www", "<!--", "/*" };
    for (size_t i = 0; i < sizeof lig / sizeof *lig; i++) {
        int cells[16], len = (int)strlen(lig[i]);
        CHECK(changes(&f, &p, lig[i]));
        CHECK(shape(&f, &p, lig[i], len, cells) == len);    /* calt with spacers: a glyph a cell, the grid stays */
    }
    CHECK(!changes(&f, &p, "abc") && !changes(&f, &p, "0123") && !changes(&f, &p, "x"));
    CHECK(p.cover['='] && p.cover['>'] && p.cover['-'] && p.cover['w']);
    CHECK(!p.cover['a'] && !p.cover['0'] && !p.cover[' '] && !p.cover['"']);
    /* the digest only skips: with every lookup applied by hand (no digest) the glyphs are the same */
    static cui_shape_plan all;
    all = p;
    memset(all.digest, 0xff, sizeof all.digest);
    int words = 0, bad = 0;
    for (size_t c = 0; c < sizeof CORPUS / sizeof *CORPUS; c++) {
        const char *s = CORPUS[c];
        for (int i = 0; s[i];) {
            if (s[i] == ' ') { i++; continue; }
            int a = i;
            while (s[i] && s[i] != ' ') i++;
            int w1[512], w2[512], at = -1;
            shape(&f, &p, s + a, i - a, w1);
            shape(&f, &all, s + a, i - a, w2);
            CHECK(!memcmp(w1, w2, sizeof(int) * (size_t)(i - a)));
            words++;
            if (!pieces_match(&f, &p, s + a, i - a, &at)) { bad++; printf("  pieces differ in \"%.*s\" at %d\n", i - a, s + a, at); }
        }
    }
    printf("Cascadia Code: %d lookups; %d words of code, %d differ in pieces\n", p.count, words, bad);
    CHECK(bad == 0);
    free(b);
}

static void test_none(const char *path, const char *name) {
    size_t n;
    unsigned char *b = load(path, &n);
    if (!b) { printf("%s: not installed, skipped\n", name); return; }
    cui_shape_font f;
    CHECK(cui__shape_font_init(&f, b, n, 0));
    static cui_shape_plan p;
    cui__shape_plan(&f, LATN, LIG, 5, &p);
    const char *probe[] = { "=>", "!=", "->", "===", "<=", "www", "&&" };
    for (size_t i = 0; i < sizeof probe / sizeof *probe; i++) CHECK(!changes(&f, &p, probe[i]));
    printf("%s: no ligatures\n", name);
    free(b);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "..";
    test_geist(dir);
    test_cascadia();
    test_none("C:/Windows/Fonts/CascadiaMono.ttf", "Cascadia Mono");
    test_none("C:/Windows/Fonts/consola.ttf", "Consolas");
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
