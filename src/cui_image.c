/* Image loading (PNG, JPEG, BMP, GIF, TGA) through stb_image.
 * Define CUI_NO_STB_IMAGE to leave it out. */
#include "cui_internal.h"

#ifndef CUI_NO_STB_IMAGE
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996 4244 4456 4457)
#endif
#include "../third_party/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

cui_texture cui_image_load(cui_ctx *ui, const char *path, int *w, int *h) {
    int iw = 0, ih = 0, comp = 0;
    unsigned char *px = stbi_load(path, &iw, &ih, &comp, 4);
    if (!px) return 0;
    cui_texture t = cui_image_create(ui, iw, ih, px);
    stbi_image_free(px);
    if (w) *w = iw;
    if (h) *h = ih;
    return t;
}
#else
cui_texture cui_image_load(cui_ctx *ui, const char *path, int *w, int *h) {
    (void)ui; (void)path; (void)w; (void)h;
    return 0;
}
#endif
