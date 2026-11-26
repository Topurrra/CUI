/* Internal, font-unit OpenType shaping. Input is one font/script/direction run. */
#ifndef CUI_SHAPE_H
#define CUI_SHAPE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    uint32_t codepoint;
    int id, cluster, advance, x, y;
    int form, ligature, component;
} cui_shape_glyph;
typedef struct {
    const unsigned char *data;
    size_t size;
    uint32_t gsub, gpos, gdef, cmap, hmtx;
    int metrics;
} cui_shape_font;
int cui__shape_font_init(cui_shape_font *f, const void *data, size_t size, int offset);
int cui__shape_map(const cui_shape_font *f, uint32_t cp);
int cui__shape(cui_shape_font *f, cui_shape_glyph *glyphs, int count, int capacity,
               uint32_t script, int rtl, int tabular);
/* Terminal cells (cui_vt's ligatures). A plan is the lookups of a set of GSUB features (script, else DFLT) in the order
 * of the font's lookup list (as OpenType applies them), each with a digest of the glyphs that can start it, so a glyph
 * no lookup starts with costs one bit test; cover[c] is 1 for each ASCII character c that starts one (text with none of
 * them comes out as it went in). About 17 KB. */
#define CUI_SHAPE_PLAN_MAX 512
typedef struct {
    int count;
    unsigned short lookup[CUI_SHAPE_PLAN_MAX];
    uint64_t digest[CUI_SHAPE_PLAN_MAX][4];   /* glyph id mod 256 */
    unsigned char cover[128];
    unsigned short ascii[128];                /* the font's glyph of each ASCII character */
} cui_shape_plan;
int cui__shape_plan(const cui_shape_font *f, uint32_t script, const uint32_t *tags, int ntags, cui_shape_plan *plan);
/* Left to right, GSUB only, no GPOS: each glyph stays in its cell. advance is the glyph's own (font units); cluster is
 * kept from the input. Returns the glyph count, -1 on failure. */
int cui__shape_gsub(cui_shape_font *f, const cui_shape_plan *plan, cui_shape_glyph *glyphs, int count, int capacity);
#endif
