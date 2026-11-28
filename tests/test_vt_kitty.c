/* Tests for the VT screen's kitty and iTerm2 pictures (CUI_VT_FEATURE_KITTY, CUI_VT_FEATURE_ITERM, CUI_VT_FEATURE_PICS_STILL):
 * fixed bytes for every path (RGB, RGBA, PNG made here by hand, zlib, chunks split anywhere and across writes, stored images
 * placed again, every delete, the query's reply and q's silence, errors), where the cursor ends, the scrollback and ESC [ 3 J,
 * both budgets, Unicode placeholders, iTerm2's sizes and multipart files, the exact bytes jnsh sends, and that with the features
 * off the same bytes leave the screen as before. Prints the plain-text parse speed with the features on and off. */
#include "../src/cui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_failed, g_checks;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; long long a_ = (long long)(a), b_ = (long long)(b); if (a_ != b_) { g_failed++; \
    printf("  FAIL %s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); } } while (0)
#define CHECK_STR(a, b) do { g_checks++; if (strcmp((a), (b))) { g_failed++; printf("  FAIL %s:%d: %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, (a), (b)); } } while (0)

#define K CUI_VT_FEATURE_KITTY
#define IT CUI_VT_FEATURE_ITERM
#define STILL CUI_VT_FEATURE_PICS_STILL
#define CL CUI_VT_FEATURE_CLUSTERS
#define RGBA(r, g, b) ((uint32_t)(r) | (uint32_t)(g) << 8 | (uint32_t)(b) << 16 | 0xFF000000u)
#define RED RGBA(255, 0, 0)
#define GREEN RGBA(0, 255, 0)
#define BLUE RGBA(0, 0, 255)
#define WHITE RGBA(255, 255, 255)

int cui__vtk_diacritic(uint32_t cp);                     /* cui_vt_kitty.c */

static void W(cui_vt *v, const char *s) { cui_vt_write(v, s, strlen(s)); }
static const char *row(const cui_vt *v, int y) { static char b[4][1100]; static int i; i = (i + 1) & 3; cui_vt_line_text(v, y, b[i], sizeof b[i]); return b[i]; }
static const char *reply(cui_vt *v) { static char b[8192]; size_t n = 0, k; while ((k = cui_vt_reply(v, b + n, sizeof b - 1 - n)) > 0) n += k; b[n] = 0; return b; }
static cui_vt *mk(int cols, int rows, int sb, unsigned f) { cui_vt *v = cui_vt_create(cols, rows, sb); cui_vt_set_features(v, f); return v; }
static int CX(const cui_vt *v) { int x, y; cui_vt_cursor(v, &x, &y); return x; }
static int CY(const cui_vt *v) { int x, y; cui_vt_cursor(v, &x, &y); return y; }
static uint32_t PX(const cui_vt *v, int r, int x, int y) { return cui__vt_pic_px(v, r, x, y); }

/* ---- encoders ---- */

static size_t b64(const uint8_t *d, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t q = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out[o++] = t[q >> 18 & 63]; out[o++] = t[q >> 12 & 63];
        out[o++] = i + 1 < n ? t[q >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[q & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static uint32_t adler(const uint8_t *d, size_t n) { uint32_t a = 1, b = 0; for (size_t i = 0; i < n; i++) { a = (a + d[i]) % 65521; b = (b + a) % 65521; } return b << 16 | a; }
static uint32_t crc(const uint8_t *d, size_t n, uint32_t c) {
    c = ~c;
    for (size_t i = 0; i < n; i++) { c ^= d[i]; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* a zlib stream of stored blocks */
static size_t zlib(const uint8_t *d, size_t n, uint8_t *out) {
    size_t o = 0;
    out[o++] = 0x78; out[o++] = 0x01;
    size_t i = 0;
    do {
        size_t len = n - i > 65535 ? 65535 : n - i;
        out[o++] = (uint8_t)(i + len == n ? 1 : 0);
        out[o++] = (uint8_t)len; out[o++] = (uint8_t)(len >> 8);
        out[o++] = (uint8_t)~len; out[o++] = (uint8_t)(~len >> 8);
        memcpy(out + o, d + i, len); o += len; i += len;
    } while (i < n);
    be32(out + o, adler(d, n)); o += 4;
    return o;
}

static size_t png_chunk(uint8_t *out, const char *type, const uint8_t *d, size_t n) {
    be32(out, (uint32_t)n);
    memcpy(out + 4, type, 4);
    if (n) memcpy(out + 8, d, n);
    be32(out + 8 + n, crc(out + 4, n + 4, 0));
    return n + 12;
}

/* a PNG of w x h pixels (rgba: 4 bytes each; alpha used when `alpha`), no filters, stored deflate */
static size_t png(const uint32_t *px, int w, int h, bool alpha, uint8_t *out) {
    static uint8_t raw[1 << 16], z[1 << 17];
    int bpp = alpha ? 4 : 3;
    size_t r = 0;
    for (int y = 0; y < h; y++) {
        raw[r++] = 0;
        for (int x = 0; x < w; x++) { uint32_t c = px[y * w + x]; for (int k = 0; k < bpp; k++) raw[r++] = (uint8_t)(c >> (8 * k)); }
    }
    size_t o = 0;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(out, sig, 8); o = 8;
    uint8_t ih[13];
    be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h);
    ih[8] = 8; ih[9] = alpha ? 6 : 2; ih[10] = 0; ih[11] = 0; ih[12] = 0;
    o += png_chunk(out + o, "IHDR", ih, 13);
    o += png_chunk(out + o, "IDAT", z, zlib(raw, r, z));
    o += png_chunk(out + o, "IEND", NULL, 0);
    return o;
}

static char g_buf[1 << 21];
static char g_b64[1 << 21];

/* ESC _ G keys ; base64(data) ESC \ */
static void kitty(cui_vt *v, const char *keys, const void *data, size_t n) {
    size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b_G%s", keys);
    if (data) { g_buf[o++] = ';'; o += b64((const uint8_t *)data, n, g_buf + o); }
    o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b\\");
    cui_vt_write(v, g_buf, o);
}

/* the four colors of a 2 x 2 picture as RGB bytes and as RGBA words */
static const uint8_t g_rgb4[12] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255 };
static const uint32_t g_px4[4] = { RED, GREEN, BLUE, WHITE };

static void check_quad(const cui_vt *v, int r0, int x0px) {   /* the 2 x 2 picture on 2 x 1 cells of 10 x 20 */
    CHECK_EQ(PX(v, r0, x0px + 5, 5), RED);
    CHECK_EQ(PX(v, r0, x0px + 15, 5), GREEN);
    CHECK_EQ(PX(v, r0, x0px + 5, 15), BLUE);
    CHECK_EQ(PX(v, r0, x0px + 15, 15), WHITE);
}

/* ---- off ---- */

static void test_off(void) {
    static const unsigned fs[3] = { 0, CUI_VT_FEATURE_SIXEL | CUI_VT_FEATURE_QUERIES, CL };
    for (int i = 0; i < 3; i++) {
        cui_vt *v = mk(20, 4, 10, fs[i]);
        size_t mem = cui_vt_memory(v);
        W(v, "A");
        kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,i=3", g_rgb4, 12);
        W(v, "B");
        size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b]1337;File=inline=1;width=2;height=1:");
        uint8_t p[4096]; size_t pn = png(g_px4, 2, 2, false, p);
        o += b64(p, pn, g_buf + o);
        g_buf[o++] = 7;
        cui_vt_write(v, g_buf, o);
        W(v, "C\xf4\x8e\xbb\xae" "D");                      /* U+10EEEE is an ordinary character without kitty */
        CHECK_STR(row(v, 0), "ABC\xf4\x8e\xbb\xae" "D");
        CHECK_EQ(CX(v), 5); CHECK_EQ(CY(v), 0);
        CHECK_EQ(cui__vt_pic_count(v), 0);
        CHECK_STR(reply(v), "");
        CHECK_EQ(cui_vt_memory(v), mem);
        cui_vt_destroy(v);
    }
}

/* ---- direct data ---- */

static void test_direct(void) {
    cui_vt *v = mk(20, 5, 10, K);
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1", g_rgb4, 12);
    check_quad(v, 0, 0);
    CHECK_EQ(cui__vt_pic_count(v), 1);
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 0);             /* the column after it, on its last row */
    CHECK_STR(reply(v), "");                             /* no i: no reply */
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K);                                /* RGBA: alpha 0 stays empty; two rows of cells */
    uint32_t px[4] = { RED, 0, BLUE, RGBA(1, 2, 3) };
    kitty(v, "a=T,f=32,s=2,v=2,c=2,r=2,i=1", px, 16);
    CHECK_EQ(PX(v, 0, 5, 10), RED);
    CHECK_EQ(PX(v, 0, 15, 10), 0);
    CHECK_EQ(PX(v, 1, 5, 10), BLUE);
    CHECK_EQ(PX(v, 1, 15, 10), RGBA(1, 2, 3));
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 1);
    CHECK_STR(reply(v), "\x1b_Gi=1;OK\x1b\\");
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K);                                /* the default format is 32; f=32 without enough data */
    kitty(v, "a=T,s=2,v=2,i=2", g_rgb4, 12);
    CHECK_STR(reply(v), "\x1b_Gi=2;ENODATA:insufficient image data\x1b\\");
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K);                                /* no c, r: its pixels over the 10 x 20 cell */
    static uint8_t big[30 * 41 * 3];
    for (size_t i = 0; i < sizeof big; i += 3) { big[i] = 0; big[i + 1] = 0; big[i + 2] = 200; }
    kitty(v, "a=T,f=24,s=30,v=41", big, sizeof big);
    CHECK_EQ(CX(v), 3); CHECK_EQ(CY(v), 2);             /* 3 x 3 cells */
    CHECK_EQ(PX(v, 2, 25, 0), RGBA(0, 0, 200));
    CHECK_EQ(PX(v, 2, 35, 0), 0);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K);                                /* c alone: r from the shape; r alone: c from it */
    kitty(v, "a=T,f=24,s=30,v=41,c=6", big, sizeof big);   /* 60 x 82 px: 4 rows of 20 (82/20 = 4.1) */
    CHECK_EQ(CX(v), 6); CHECK_EQ(CY(v), 3);
    W(v, "\x1b[H");
    cui_vt_destroy(v);
    v = mk(20, 5, 10, K);
    kitty(v, "a=T,f=24,s=30,v=41,r=2", big, sizeof big);   /* 40 px high: 29 px wide, 3 cells */
    CHECK_EQ(CX(v), 3); CHECK_EQ(CY(v), 1);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K);                                /* a source rectangle: the white pixel alone */
    kitty(v, "a=T,f=24,s=2,v=2,x=1,y=1,w=1,h=1,c=1,r=1", g_rgb4, 12);
    CHECK_EQ(PX(v, 0, 5, 5), WHITE);
    CHECK_EQ(PX(v, 0, 0, 0), WHITE);
    cui_vt_destroy(v);
}

static void test_cursor_still(void) {
    cui_vt *v = mk(20, 5, 10, K);
    W(v, "\x1b[2;4H");
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=2,C=1", g_rgb4, 12);
    CHECK_EQ(CX(v), 3); CHECK_EQ(CY(v), 1);             /* C=1: it stays */
    CHECK_EQ(PX(v, 1, 35, 5), RED);
    CHECK_EQ(PX(v, 2, 45, 5), WHITE);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K | STILL);
    W(v, "x\x1b[5;1H");
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=3", g_rgb4, 12);   /* at the bottom, three rows: cut, nothing scrolls */
    CHECK_EQ(CX(v), 0); CHECK_EQ(CY(v), 4);
    CHECK_STR(row(v, 0), "x");
    CHECK_EQ(PX(v, 4, 5, 5), RED);
    CHECK_EQ(cui__vt_pic_count(v), 1);
    cui_vt_destroy(v);

    v = mk(20, 3, 10, K);                                /* moving: at the bottom the screen scrolls so it fits */
    W(v, "x\x1b[3;1H");
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=2", g_rgb4, 12);
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 2);
    CHECK_STR(row(v, -1), "x");
    CHECK_EQ(PX(v, 1, 5, 15), RED);                     /* the top half of the picture on row 1 */
    CHECK_EQ(PX(v, 2, 15, 5), WHITE);
    cui_vt_destroy(v);

    v = mk(4, 3, 10, K);                                 /* the column after it, kept on the screen */
    kitty(v, "a=T,f=24,s=2,v=2,c=6,r=1", g_rgb4, 12);
    CHECK_EQ(CX(v), 3); CHECK_EQ(CY(v), 0);
    cui_vt_destroy(v);
}

/* ---- the query, replies, errors ---- */

static void test_query(void) {
    cui_vt *v = mk(20, 5, 10, K);
    W(v, "\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c");
    CHECK_STR(reply(v), "\x1b_Gi=31;OK\x1b\\\x1b[?1;0c");    /* kitty's answer before the device attributes */
    CHECK_EQ(cui__vt_pic_count(v), 0);
    kitty(v, "a=p,i=31", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=31;ENOENT:no such image\x1b\\");   /* a query keeps nothing */
    W(v, "\x1b_Gi=32,s=1,v=1,a=q,t=d,f=24,q=2;AAAA\x1b\\");
    CHECK_STR(reply(v), "");
    W(v, "\x1b_Gi=33,s=1,v=1,a=q,t=d,f=24,q=1;AAAA\x1b\\");
    CHECK_STR(reply(v), "");
    W(v, "\x1b_Gi=34,s=2,v=2,a=q,f=24,q=1;AAAA\x1b\\");          /* q=1 keeps errors */
    CHECK_STR(reply(v), "\x1b_Gi=34;ENODATA:insufficient image data\x1b\\");
    W(v, "\x1b_Gi=35,s=1,v=1,a=q,f=24;A!!A\x1b\\");
    CHECK_STR(reply(v), "\x1b_Gi=35;EINVAL:bad base64 data\x1b\\");
    W(v, "\x1b_Gi=36,s=1,v=1,a=t,t=f,f=24;L2V0Yy9wYXNzd2Q=\x1b\\");   /* a file name: refused, never opened */
    CHECK_STR(reply(v), "\x1b_Gi=36;EBADF:only direct data is read\x1b\\");
    W(v, "\x1b_Gi=37,a=t,f=100;AAAAAAAA\x1b\\");
    CHECK_STR(reply(v), "\x1b_Gi=37;EBADPNG:the PNG does not decode\x1b\\");
    W(v, "\x1b_Gi=38,a=t,f=7,s=1,v=1;AAAA\x1b\\");
    CHECK_STR(reply(v), "\x1b_Gi=38;EINVAL:unknown format\x1b\\");
    W(v, "\x1b_Gi=39,a=t,f=24,o=z,s=1,v=1;AAAA\x1b\\");
    CHECK_STR(reply(v), "\x1b_Gi=39;EINVAL:the zlib data does not inflate\x1b\\");
    W(v, "\x1b_GI=7,s=1,v=1,a=t,f=24;AAAA\x1b\\");                 /* I alone: an id is given and told */
    CHECK_STR(reply(v), "\x1b_Gi=2147483648,I=7;OK\x1b\\");
    W(v, "\x1b_Ga=p,I=7,p=4\x1b\\");
    CHECK_STR(reply(v), "\x1b_Gi=2147483648,I=7,p=4;OK\x1b\\");
    CHECK_EQ(cui__vt_pic_count(v), 1);
    W(v, "\x1b_Gi=41,a=t,f=32,o=z,s=16384,v=16384;AAAA\x1b\\");   /* hostile sizes: refused before anything is allocated */
    CHECK_STR(reply(v), "\x1b_Gi=41;EFBIG:the picture is too big\x1b\\");
    W(v, "\x1b_Gi=42,a=t,f=32,o=z,s=5000,v=5000;eAEBAAD//wAAAAE=\x1b\\");   /* a tiny zlib stream claiming 100 MB */
    CHECK_STR(reply(v), "\x1b_Gi=42;ENODATA:insufficient image data\x1b\\");
    {
        uint8_t hp[64]; size_t o = 0;
        static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        memcpy(hp, sig, 8); o = 8;
        uint8_t ih[13] = { 0 };
        be32(ih, 20000); be32(ih + 4, 20000); ih[8] = 8; ih[9] = 6;
        o += png_chunk(hp + o, "IHDR", ih, 13);
        o += png_chunk(hp + o, "IEND", NULL, 0);
        kitty(v, "i=43,a=t,f=100", hp, o);
        CHECK_STR(reply(v), "\x1b_Gi=43;EBADPNG:the PNG does not decode\x1b\\");
    }
    W(v, "\x1b_Gi=40,a=x;\x1b\\");                                 /* an unknown action: nothing */
    CHECK_STR(reply(v), "");
    CHECK_STR(row(v, 0), "");
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K | CUI_VT_FEATURE_QUERIES);       /* the other APC strings are still skipped */
    W(v, "a\x1b_Hello\x1b\\b\x1b_\x07" "c\x1b_G");
    W(v, "i=1,a=q,f=24,s=1,v=1;AAAA\x07" "d");
    CHECK_STR(row(v, 0), "abcd");
    CHECK_STR(reply(v), "\x1b_Gi=1;OK\x1b\\");
    cui_vt_destroy(v);
}

/* ---- PNG, zlib, chunks ---- */

static void test_png_zlib_chunks(void) {
    uint8_t p[4096], z[4096];
    cui_vt *v = mk(20, 5, 10, K);
    size_t pn = png(g_px4, 2, 2, false, p);
    kitty(v, "a=T,f=100,c=2,r=1,i=9", p, pn);
    CHECK_STR(reply(v), "\x1b_Gi=9;OK\x1b\\");
    check_quad(v, 0, 0);
    W(v, "\r\n");
    uint32_t px[4] = { RED, 0, 0, BLUE };
    pn = png(px, 2, 2, true, p);
    kitty(v, "a=T,f=100,c=2,r=1", p, pn);
    CHECK_EQ(PX(v, 1, 5, 5), RED); CHECK_EQ(PX(v, 1, 15, 5), 0); CHECK_EQ(PX(v, 1, 15, 15), BLUE);
    W(v, "\r\n");
    size_t zn = zlib(g_rgb4, 12, z);
    kitty(v, "a=T,f=24,o=z,s=2,v=2,c=2,r=1", z, zn);
    check_quad(v, 2, 0);
    W(v, "\r\n");
    pn = png(g_px4, 2, 2, false, p);
    zn = zlib(p, pn, z);
    kitty(v, "a=T,f=100,o=z,c=2,r=1", z, zn);            /* a PNG inside zlib */
    check_quad(v, 3, 0);
    cui_vt_destroy(v);

    /* chunks split at odd places, and the whole thing written a byte at a time */
    for (int pass = 0; pass < 2; pass++) {
        v = mk(20, 5, 10, K);
        char e[64];
        size_t en = b64(g_rgb4, 12, e);                  /* 16 characters */
        size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b_Ga=T,f=24,s=2,v=2,c=2,r=1,i=5,m=1;%.5s\x1b\\", e);
        o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b_Gm=1;%.6s\x1b\\", e + 5);
        o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b_Gm=1;\x1b\\");
        o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b_Gm=0;%s\x1b\\", e + 11);
        (void)en;
        if (pass == 0) cui_vt_write(v, g_buf, o);
        else for (size_t i = 0; i < o; i++) cui_vt_write(v, g_buf + i, 1);
        check_quad(v, 0, 0);
        CHECK_STR(reply(v), "\x1b_Gi=5;OK\x1b\\");
        CHECK_EQ(CX(v), 2);
        cui_vt_destroy(v);
    }

    v = mk(20, 5, 10, K);                                /* a reset in the middle drops the transfer */
    W(v, "\x1b_Ga=T,f=24,s=2,v=2,c=2,r=1,i=5,m=1;AAAA\x1b\\\x1b" "c");
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,i=6", g_rgb4, 12);
    check_quad(v, 0, 0);
    CHECK_STR(reply(v), "\x1b_Gi=6;OK\x1b\\");
    cui_vt_destroy(v);
}

/* ---- stored images, placements, deletes ---- */

static cui_vt *two_placements(void) {
    cui_vt *v = mk(20, 6, 10, K);
    kitty(v, "a=t,f=24,s=2,v=2,i=5,q=1", g_rgb4, 12);
    W(v, "\x1b[1;1H"); kitty(v, "a=p,i=5,c=2,r=1,q=1", NULL, 0);
    W(v, "\x1b[3;5H"); kitty(v, "a=p,i=5,c=2,r=1,q=1", NULL, 0);
    return v;
}

static void test_place_delete(void) {
    cui_vt *v = two_placements();
    check_quad(v, 0, 0);
    check_quad(v, 2, 40);
    CHECK_EQ(cui__vt_pic_count(v), 2);
    CHECK_STR(reply(v), "");
    kitty(v, "a=d,d=i,i=5", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    W(v, "\x1b[H"); kitty(v, "a=p,i=5,c=2,r=1", NULL, 0);    /* the image is still there */
    CHECK_STR(reply(v), "\x1b_Gi=5;OK\x1b\\");
    check_quad(v, 0, 0);
    kitty(v, "a=d,d=I,i=5", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    kitty(v, "a=p,i=5", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=5;ENOENT:no such image\x1b\\");
    cui_vt_destroy(v);

    v = two_placements();                                /* at a cell (1-based) */
    kitty(v, "a=d,d=p,x=6,y=3", NULL, 0);
    check_quad(v, 0, 0);
    CHECK_EQ(PX(v, 2, 45, 5), 0);
    cui_vt_destroy(v);

    v = two_placements();                                /* at the cursor */
    W(v, "\x1b[1;2H");
    kitty(v, "a=d,d=c", NULL, 0);
    CHECK_EQ(PX(v, 0, 5, 5), 0);
    check_quad(v, 2, 40);
    cui_vt_destroy(v);

    v = two_placements();                                /* a column, a row */
    kitty(v, "a=d,d=x,x=6", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 1);
    check_quad(v, 0, 0);
    kitty(v, "a=d,d=y,y=1", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);

    v = two_placements();                                /* everything on the screen, and z as a */
    kitty(v, "a=d", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    W(v, "\x1b[H"); kitty(v, "a=p,i=5,c=2,r=1,q=1", NULL, 0);
    kitty(v, "a=d,d=z,z=-1", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    kitty(v, "a=d,d=A", NULL, 0);                         /* A: the image goes too */
    kitty(v, "a=p,i=5", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=5;ENOENT:no such image\x1b\\");
    cui_vt_destroy(v);

    v = mk(20, 6, 10, K);                                /* by number, by a range of ids */
    kitty(v, "a=T,f=24,s=2,v=2,I=3,c=2,r=1,q=2", g_rgb4, 12);
    W(v, "\r\n");
    kitty(v, "a=T,f=24,s=2,v=2,i=20,c=2,r=1,q=2", g_rgb4, 12);
    W(v, "\r\n");
    kitty(v, "a=T,f=24,s=2,v=2,i=30,c=2,r=1,q=2", g_rgb4, 12);
    CHECK_EQ(cui__vt_pic_count(v), 3);
    kitty(v, "a=d,d=n,I=3", NULL, 0);
    CHECK_EQ(PX(v, 0, 5, 5), 0);
    CHECK_EQ(cui__vt_pic_count(v), 2);
    kitty(v, "a=d,d=r,x=15,y=25", NULL, 0);
    CHECK_EQ(PX(v, 1, 5, 5), 0);
    check_quad(v, 2, 0);
    cui_vt_destroy(v);

    v = mk(20, 6, 10, K);                                /* a placement id moves its placement */
    kitty(v, "a=t,f=24,s=2,v=2,i=5,q=2", g_rgb4, 12);
    kitty(v, "a=p,i=5,p=7,c=2,r=1,q=2", NULL, 0);
    W(v, "\x1b[4;1H");
    kitty(v, "a=p,i=5,p=7,c=2,r=1,q=2", NULL, 0);
    CHECK_EQ(PX(v, 0, 5, 5), 0);
    check_quad(v, 3, 0);
    kitty(v, "a=d,d=i,i=5,p=8", NULL, 0);                 /* another placement id: nothing */
    check_quad(v, 3, 0);
    kitty(v, "a=d,d=i,i=5,p=7", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);

    v = mk(20, 6, 10, K | CUI_VT_FEATURE_SIXEL);         /* a delete leaves sixel pictures, and text over a cell takes it */
    W(v, "\x1b[3;1H\x1bPq#0;2;100;0;0#0!20~\x1b\\");
    W(v, "\x1b[H"); kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,q=2", g_rgb4, 12);
    kitty(v, "a=d", NULL, 0);
    CHECK_EQ(PX(v, 2, 5, 2), RED);
    CHECK_EQ(PX(v, 0, 5, 5), 0);
    W(v, "\x1b[H"); kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,q=2", g_rgb4, 12);
    W(v, "\x1b[1;2Hz");
    CHECK_EQ(PX(v, 0, 5, 5), RED);
    CHECK_EQ(PX(v, 0, 15, 5), 0);
    cui_vt_destroy(v);

    v = mk(20, 3, 400, K);                               /* 300 placements: tags are reused, the old pictures stay */
    kitty(v, "a=t,f=24,s=2,v=2,i=5,q=2", g_rgb4, 12);
    for (int i = 0; i < 300; i++) { W(v, "\x1b[H"); kitty(v, "a=p,i=5,c=2,r=1,q=2", NULL, 0); W(v, "\r\n\r\n\r\n"); }
    CHECK_EQ(PX(v, -3, 5, 5), RED);
    CHECK_EQ(PX(v, -300, 15, 15), WHITE);
    kitty(v, "a=d,d=i,i=5", NULL, 0);
    CHECK(PX(v, -3, 5, 5) == 0);
    CHECK_EQ(PX(v, -300, 15, 15), WHITE);                /* anonymous now: kept */
    cui_vt_destroy(v);
}

/* ---- the scrollback, ESC [ 3 J, budgets ---- */

static void test_scrollback_budget(void) {
    cui_vt *v = mk(20, 3, 50, K);
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,q=2", g_rgb4, 12);
    W(v, "\r\n\r\n\r\n\r\n");
    check_quad(v, -2, 0);
    CHECK_EQ(cui__vt_pic_count(v), 1);
    W(v, "\x1b[3J");
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);

    v = mk(20, 3, 50, K);                                /* the image budget: the oldest goes */
    cui_vt_set_image_budget(v, 2 * 16 + 8);
    kitty(v, "a=t,f=24,s=2,v=2,i=1,q=2", g_rgb4, 12);
    kitty(v, "a=t,f=24,s=2,v=2,i=2,q=2", g_rgb4, 12);
    kitty(v, "a=t,f=24,s=2,v=2,i=3,q=2", g_rgb4, 12);
    kitty(v, "a=p,i=1", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=1;ENOENT:no such image\x1b\\");
    kitty(v, "a=p,i=3,c=1,r=1", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=3;OK\x1b\\");
    kitty(v, "a=T,f=24,s=4,v=4,i=4,c=1,r=1", g_b64, 48); /* too big to keep: drawn all the same */
    CHECK_STR(reply(v), "\x1b_Gi=4;OK\x1b\\");
    kitty(v, "a=p,i=4", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=4;ENOENT:no such image\x1b\\");
    size_t mem = cui_vt_memory(v);
    CHECK(mem > 0);
    cui_vt_destroy(v);

    v = mk(20, 6, 50, K);                                /* the picture budget: a picture is cut off */
    cui_vt_set_picture_budget(v, 3000);                  /* a slice of 2 cells is 2 * (10*20*4 + 1) + header */
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=4,i=8", g_rgb4, 12);
    CHECK_STR(reply(v), "\x1b_Gi=8;ENOSPC:the picture budget is full\x1b\\");
    CHECK(cui__vt_pic_count(v) >= 1 && cui__vt_pic_count(v) < 4);
    cui_vt_destroy(v);

    v = mk(20, 3, 50, K | IT);                           /* features off: everything goes */
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,i=8,q=2", g_rgb4, 12);
    cui_vt_set_features(v, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    kitty(v, "a=p,i=8,q=2", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_set_features(v, K);
    kitty(v, "a=p,i=8", NULL, 0);
    CHECK_STR(reply(v), "\x1b_Gi=8;ENOENT:no such image\x1b\\");
    cui_vt_destroy(v);

    v = mk(20, 3, 50, K);                                /* a resize keeps the rows' pictures */
    kitty(v, "a=T,f=24,s=2,v=2,c=2,r=1,q=2", g_rgb4, 12);
    cui_vt_resize(v, 30, 4);
    check_quad(v, 0, 0);
    cui_vt_destroy(v);
}

/* ---- Unicode placeholders ---- */

static size_t utf8(uint32_t c, char *o) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | c >> 6); o[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { o[0] = (char)(0xE0 | c >> 12); o[1] = (char)(0x80 | (c >> 6 & 63)); o[2] = (char)(0x80 | (c & 63)); return 3; }
    o[0] = (char)(0xF0 | c >> 18); o[1] = (char)(0x80 | (c >> 12 & 63)); o[2] = (char)(0x80 | (c >> 6 & 63)); o[3] = (char)(0x80 | (c & 63));
    return 4;
}
static const uint32_t g_dia[4] = { 0x0305, 0x030D, 0x030E, 0x0310 };   /* 0, 1, 2, 3 */
static size_t ph(char *o, int r, int c, int msb) {      /* a placeholder with r, c, msb diacritics (-1: left out) */
    size_t n = utf8(0x10EEEE, o);
    if (r >= 0) n += utf8(g_dia[r], o + n);
    if (c >= 0) n += utf8(g_dia[c], o + n);
    if (msb >= 0) n += utf8(g_dia[msb], o + n);
    return n;
}

static void test_placeholders(void) {
    CHECK_EQ(cui__vtk_diacritic(0x0305), 0);
    CHECK_EQ(cui__vtk_diacritic(0x0310), 3);
    CHECK_EQ(cui__vtk_diacritic(0x0363), 17);
    CHECK_EQ(cui__vtk_diacritic(0x1D244), 296);           /* the 297th */
    CHECK_EQ(cui__vtk_diacritic(0x0306), -1);
    CHECK_EQ(cui__vtk_diacritic('a'), -1);

    cui_vt *v = mk(20, 5, 10, K | CL);
    kitty(v, "a=t,f=24,s=2,v=2,i=42,q=2", g_rgb4, 12);
    kitty(v, "a=p,U=1,i=42,c=2,r=2,q=2", NULL, 0);
    CHECK_EQ(cui__vt_pic_count(v), 0);                   /* virtual: nothing drawn */
    char s[256]; size_t n = 0;
    n += (size_t)snprintf(s + n, sizeof s - n, "\x1b[38;5;42m");
    n += ph(s + n, 0, 0, -1); n += ph(s + n, 0, 1, -1);
    n += (size_t)snprintf(s + n, sizeof s - n, "\r\n");
    n += ph(s + n, 1, -1, -1); n += ph(s + n, -1, -1, -1);   /* the second takes row 1, column 1 from its left */
    n += (size_t)snprintf(s + n, sizeof s - n, "\x1b[m");
    cui_vt_write(v, s, n);
    /* the picture (2 x 2) on 2 x 2 cells: each cell is one pixel of it */
    CHECK_EQ(PX(v, 0, 5, 10), RED);
    CHECK_EQ(PX(v, 0, 15, 10), GREEN);
    CHECK_EQ(PX(v, 1, 5, 10), BLUE);
    CHECK_EQ(PX(v, 1, 15, 10), WHITE);
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 1);
    kitty(v, "a=d,d=i,i=42", NULL, 0);                    /* deleting the image's placements takes the painted cells */
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K | CL);                           /* a 24-bit id, and the id's high byte */
    kitty(v, "a=t,f=24,s=2,v=2,i=258,q=2", g_rgb4, 12);  /* 0x000102 */
    kitty(v, "a=p,U=1,i=258,c=2,r=2,q=2", NULL, 0);
    kitty(v, "a=t,f=24,s=2,v=2,i=50331906,q=2", g_rgb4, 12);   /* 0x03000102 */
    kitty(v, "a=p,U=1,i=50331906,c=1,r=1,q=2", NULL, 0);
    n = (size_t)snprintf(s, sizeof s, "\x1b[38;2;0;1;2m");
    n += ph(s + n, 1, 1, -1);
    n += ph(s + n, 0, 0, 3);
    cui_vt_write(v, s, n);
    CHECK_EQ(PX(v, 0, 5, 10), WHITE);                   /* row 1, column 1 of the 2 x 2 placement */
    CHECK_EQ(PX(v, 0, 12, 0), RED);                     /* the whole picture on one cell: its top left is red */
    CHECK_EQ(PX(v, 0, 17, 0), GREEN);
    CHECK_EQ(PX(v, 0, 19, 19), WHITE);
    cui_vt_destroy(v);

    v = mk(20, 5, 10, K | CL);                           /* no such image, or outside the placement: nothing */
    kitty(v, "a=t,f=24,s=2,v=2,i=42,q=2", g_rgb4, 12);
    kitty(v, "a=p,U=1,i=42,c=1,r=1,q=2", NULL, 0);
    n = (size_t)snprintf(s, sizeof s, "\x1b[38;5;41m");
    n += ph(s + n, 0, 0, -1);
    n += (size_t)snprintf(s + n, sizeof s - n, "\x1b[38;5;42m");
    n += ph(s + n, 2, 0, -1);
    cui_vt_write(v, s, n);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    CHECK_EQ(CX(v), 2);
    cui_vt_destroy(v);
}

/* ---- iTerm2 ---- */

static void iterm(cui_vt *v, const char *args, const uint32_t *px, int w, int h) {
    static uint8_t p[1 << 16];
    size_t pn = png(px, w, h, false, p);
    size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b]1337;File=%s:", args);
    o += b64(p, pn, g_buf + o);
    g_buf[o++] = 7;
    cui_vt_write(v, g_buf, o);
}

static void test_iterm(void) {
    cui_vt *v = mk(10, 6, 10, IT);
    iterm(v, "inline=1;width=2;height=1;preserveAspectRatio=0", g_px4, 2, 2);
    check_quad(v, 0, 0);
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 0);
    W(v, "\r\n");
    iterm(v, "name=eC5wbmc=;size=99;inline=1;width=20px;height=20px;preserveAspectRatio=0", g_px4, 2, 2);
    check_quad(v, 1, 0);
    W(v, "\r\n");
    iterm(v, "inline=1;width=50%;height=1;preserveAspectRatio=0", g_px4, 2, 2);   /* half of 10 columns */
    CHECK_EQ(CX(v), 5);
    CHECK_EQ(PX(v, 2, 49, 15), WHITE);
    W(v, "\r\n");
    iterm(v, "inline=1;doNotMoveCursor=1", g_px4, 2, 2);          /* auto: its own 2 x 2 pixels, one cell */
    CHECK_EQ(CX(v), 0); CHECK_EQ(CY(v), 3);
    CHECK_EQ(PX(v, 3, 0, 0), RED);
    CHECK_EQ(PX(v, 3, 9, 19), WHITE);
    cui_vt_destroy(v);

    v = mk(10, 6, 10, IT);                               /* preserveAspectRatio: a 2:1 picture in a 4 x 4 cell box */
    static uint32_t wide[40 * 20];
    for (int i = 0; i < 40 * 20; i++) wide[i] = (i % 40) < 20 ? RED : BLUE;
    iterm(v, "inline=1;width=4;height=4", wide, 40, 20);  /* 40 x 20 px in a 40 x 80 box: 4 cells wide, 1 row */
    CHECK_EQ(CX(v), 4); CHECK_EQ(CY(v), 0);
    CHECK_EQ(PX(v, 0, 5, 10), RED);
    CHECK_EQ(PX(v, 0, 35, 10), BLUE);
    CHECK_EQ(PX(v, 1, 5, 10), 0);
    W(v, "\r\n");
    iterm(v, "inline=1;width=2", wide, 40, 20);            /* width alone: 20 px wide, 10 high, one row */
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 1);
    W(v, "\r\n");
    iterm(v, "inline=1;height=2", wide, 40, 20);           /* height alone: 40 px high, 80 wide (8 cells) */
    CHECK_EQ(CX(v), 8); CHECK_EQ(CY(v), 3);
    cui_vt_destroy(v);

    v = mk(10, 6, 10, IT);                               /* auto, wider than the row: made to fit */
    static uint32_t huge[200 * 20];
    for (int i = 0; i < 200 * 20; i++) huge[i] = GREEN;
    iterm(v, "inline=1", huge, 200, 20);
    CHECK_EQ(CX(v), 9); CHECK_EQ(CY(v), 0);             /* 100 px: 10 cells, the cursor kept on the screen */
    CHECK_EQ(PX(v, 0, 95, 5), GREEN);
    cui_vt_destroy(v);

    v = mk(10, 6, 10, IT);                               /* not inline: a download, nothing kept */
    iterm(v, "width=2;height=1", g_px4, 2, 2);
    iterm(v, "inline=0;width=2;height=1", g_px4, 2, 2);
    CHECK_EQ(cui__vt_pic_count(v), 0);
    CHECK_EQ(CX(v), 0);
    W(v, "\x1b]1337;SetMark\x07\x1b]1337;CurrentDir=/x\x1b\\ok");   /* other 1337 commands: skipped */
    CHECK_STR(row(v, 0), "ok");
    cui_vt_destroy(v);

    v = mk(10, 6, 10, IT | STILL);                       /* iTerm2 3.5's multipart file, split anywhere; still */
    static uint8_t p[4096];
    size_t pn = png(g_px4, 2, 2, false, p);
    char e[8192];
    size_t en = b64(p, pn, e);
    size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b]1337;MultipartFile=inline=1;width=2;height=1;preserveAspectRatio=0\x07");
    o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b]1337;FilePart=%.*s\x1b\\", 7, e);
    o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b]1337;FilePart=%s\x07", e + 7);
    o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b]1337;FileEnd\x07");
    (void)en;
    for (size_t i = 0; i < o; i++) cui_vt_write(v, g_buf + i, 1);
    check_quad(v, 0, 0);
    CHECK_EQ(CX(v), 0);
    cui_vt_destroy(v);

    v = mk(10, 6, 10, IT);                               /* broken data: nothing, and the screen goes on */
    W(v, "\x1b]1337;File=inline=1:AAAAAAAA\x07" "a\x1b]1337;File=inline=1:!!\x07" "b");
    CHECK_STR(row(v, 0), "ab");
    CHECK_EQ(cui__vt_pic_count(v), 0);
    cui_vt_destroy(v);
}

/* ---- what jnsh sends ---- */

static void test_jnsh_bytes(void) {
    enum { WD = 120, HT = 60 };                          /* 12 x 3 cells; RGB, zlib, chunks of 4096 */
    static uint8_t rgb[WD * HT * 3], z[WD * HT * 3 + 1024];
    for (int y = 0; y < HT; y++) for (int x = 0; x < WD; x++) {
        uint8_t *p = rgb + (y * WD + x) * 3;
        p[0] = (uint8_t)(x < WD / 2 ? 250 : 0); p[1] = (uint8_t)(y < HT / 2 ? 0 : 250); p[2] = 7;
    }
    size_t zn = zlib(rgb, sizeof rgb, z);
    size_t en = b64(z, zn, g_b64);
    cui_vt *v = mk(40, 10, 50, K | STILL | CL);
    W(v, "\x1b[2;3H");
    size_t o = 0;
    for (size_t at = 0; at < en; at += 4096) {
        size_t len = en - at < 4096 ? en - at : 4096;
        bool last = at + len >= en;
        if (at == 0) o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b_Ga=T,f=24,o=z,s=%d,v=%d,c=12,r=3,C=1,q=2,m=%d;", WD, HT, last ? 0 : 1);
        else o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b_Gm=%d;", last ? 0 : 1);
        memcpy(g_buf + o, g_b64 + at, len); o += len;
        o += (size_t)snprintf(g_buf + o, sizeof g_buf - o, "\x1b\\");
    }
    CHECK(en > 4096 * 2);
    cui_vt_write(v, g_buf, o);
    CHECK_STR(reply(v), "");
    CHECK_EQ(CX(v), 2); CHECK_EQ(CY(v), 1);
    CHECK_EQ(PX(v, 1, 25, 5), RGBA(250, 0, 7));
    CHECK_EQ(PX(v, 3, 135, 15), RGBA(0, 250, 7));
    CHECK_EQ(cui__vt_pic_count(v), 3);
    cui_vt_destroy(v);

    v = mk(40, 10, 50, IT | STILL);                      /* iTerm2 as jnsh writes it */
    static uint32_t px[WD * HT];
    for (int i = 0; i < WD * HT; i++) px[i] = RGBA(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
    static uint8_t p[1 << 16];
    size_t pn = png(px, WD, HT, false, p);
    o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b]1337;File=inline=1;size=%zu;width=12;height=3;preserveAspectRatio=0:", pn);
    o += b64(p, pn, g_buf + o);
    g_buf[o++] = 7;
    cui_vt_write(v, g_buf, o);
    CHECK_EQ(CX(v), 0); CHECK_EQ(CY(v), 0);
    CHECK_EQ(PX(v, 0, 25, 5), RGBA(250, 0, 7));
    CHECK_EQ(PX(v, 2, 115, 15), RGBA(0, 250, 7));
    cui_vt_destroy(v);
}

/* ---- speed ---- */

static double now_s(void) { return (double)clock() / CLOCKS_PER_SEC; }

static void test_speed(void) {
    size_t n = 8u << 20;
    char *text = (char *)malloc(n);
    if (!text) return;
    for (size_t i = 0; i < n; i++) text[i] = (i % 81) == 80 ? '\n' : (char)('a' + i % 26);
    double best[2] = { 1e9, 1e9 };
    for (int rep = 0; rep < 6; rep++) for (int on = 0; on < 2; on++) {
        cui_vt *v = mk(120, 40, 1000, on ? (K | IT | STILL | CL) : CL);
        double t0 = now_s();
        cui_vt_write(v, text, n);
        double t = now_s() - t0;
        if (t < best[on]) best[on] = t;
        cui_vt_destroy(v);
    }
    printf("  plain text: %.1f MB/s with kitty and iTerm2 off, %.1f MB/s on\n", 8 / best[0], 8 / best[1]);
    free(text);

    enum { WD = 960, HT = 480 };
    static uint8_t rgb[WD * HT * 3];
    for (size_t i = 0; i < sizeof rgb; i++) rgb[i] = (uint8_t)(i * 7);
    uint8_t *z = (uint8_t *)malloc(sizeof rgb + 4096);
    size_t zn = zlib(rgb, sizeof rgb, z);
    size_t en = b64(z, zn, g_b64);
    free(z);
    double tb = 1e9;
    for (int rep = 0; rep < 3; rep++) {
        cui_vt *v = mk(120, 40, 1000, K | STILL);
        size_t o = (size_t)snprintf(g_buf, sizeof g_buf, "\x1b_Ga=T,f=24,o=z,s=%d,v=%d,c=120,r=24,C=1,q=2;", WD, HT);
        double t0 = now_s();
        cui_vt_write(v, g_buf, o);
        cui_vt_write(v, g_b64, en);
        W(v, "\x1b\\");
        double t = now_s() - t0;
        if (t < tb) tb = t;
        CHECK_EQ(cui__vt_pic_count(v), 24);
        cui_vt_destroy(v);
    }
    printf("  a 960 x 480 kitty picture (%.1f MB of base64) on 120 x 24 cells: %.1f ms\n", (double)en / 1e6, tb * 1000);
}

int main(void) {
    test_off();
    test_direct();
    test_cursor_still();
    test_query();
    test_png_zlib_chunks();
    test_place_delete();
    test_scrollback_budget();
    test_placeholders();
    test_iterm();
    test_jnsh_bytes();
    test_speed();
    printf("cui_test_vt_kitty: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
