/* System fonts by script (cui_font_use_system_fallback).
 *
 * CUI's own fonts cover Latin. With the option on, the core asks this file's hook for a character that no
 * font of the app has (cui__find_glyph in cui_text.c: the font, its fallbacks, the icons, every font the
 * app loaded, then this). The hook picks the fonts of the character's script from a table of code point
 * ranges (a short chain of system fonts per range), loads the first one the first time it is needed and
 * answers with a glyph from the first font of the chain that has the character.
 *
 * Windows: the files in the Fonts folder are mapped read only (CreateFileMapping, MapViewOfFile) and given
 * to the core without copying, so the outlines stay shared file pages of the system and cost almost no
 * private memory, even for a 20 MB CJK collection. A collection is opened at the right face. Elsewhere
 * there is no table yet and the option does nothing.
 *
 * Emoji stay with the emoji font: a code point the emoji font would draw is refused here. What was
 * found, and what no chain has, is remembered per code point in a small table, so a missing character
 * does not walk the chain every frame. */
#include "cui_internal.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

/* One system font: the file in the Fonts folder, the face in it, and the semibold file for the medium and
 * display roles (NULL when the family has none). The letter names it in the chains below. */
typedef struct { char key; const char *file, *semibold; int face; } cui__sf_slot;
static const cui__sf_slot cui__sf_slots[] = {
    { 'S', "segoeui.ttf", "seguisb.ttf", 0 },   /* Segoe UI: Latin extended, IPA, Greek, Cyrillic, Georgian, Hebrew, Arabic, punctuation */
    { 'N', "Nirmala.ttc", NULL, 0 },            /* Nirmala UI: Devanagari, Bengali, Tamil, Telugu and the other Indic scripts */
    { 'L', "LeelawUI.ttf", NULL, 0 },           /* Leelawadee UI: Thai, Lao, Khmer */
    { 'Y', "msyh.ttc", NULL, 0 },               /* Microsoft YaHei: Han (simplified), kana */
    { 'G', "YuGothR.ttc", NULL, 0 },            /* Yu Gothic: kana, Han (Japanese) */
    { 'K', "malgun.ttf", NULL, 0 },             /* Malgun Gothic: Hangul */
    { 'J', "msjh.ttc", NULL, 0 },               /* Microsoft JhengHei: Han (traditional) */
    { 'U', "simsun.ttc", NULL, 0 },             /* SimSun: rarer Han */
    { 'X', "simsunb.ttf", NULL, 0 },            /* SimSun ExtB: Han extension B and up */
    { 'E', "ebrima.ttf", NULL, 0 },             /* Ebrima: Ethiopic, N'Ko, Vai, Tifinagh, Adlam */
    { 'B', "seguisym.ttf", NULL, 0 },           /* Segoe UI Symbol: arrows, math, shapes, music, game pieces */
    { 'H', "seguihis.ttf", NULL, 0 },           /* Segoe UI Historic: Runic, Linear B, Phoenician, cuneiform and so on */
    { 'F', "sylfaen.ttf", NULL, 0 },            /* Sylfaen: Georgian and Armenian when Segoe UI lacks a letter */
    { 'M', "mvboli.ttf", NULL, 0 },             /* MV Boli: Thaana */
    { 'R', "mmrtext.ttf", NULL, 0 },            /* Myanmar Text */
    { 'T', "himalaya.ttf", NULL, 0 },           /* Microsoft Himalaya: Tibetan */
    { 'O', "monbaiti.ttf", NULL, 0 },           /* Mongolian Baiti */
    { 'I', "msyi.ttf", NULL, 0 },               /* Microsoft Yi Baiti */
    { 'C', "gadugi.ttf", NULL, 0 },             /* Gadugi: Cherokee, Canadian syllabics */
    { 'V', "javatext.ttf", NULL, 0 },           /* Javanese Text */
};
#define CUI__SF_SLOTS ((int)(sizeof cui__sf_slots / sizeof *cui__sf_slots))

/* Code point ranges to chains, first match wins (so the narrow ranges come before the wide ones around them). */
typedef struct { uint32_t lo, hi; const char *chain; } cui__sf_range;
static const cui__sf_range cui__sf_ranges[] = {
    { 0x0080, 0x052F, "SB" },     /* Latin extended, IPA, combining marks, Greek, Cyrillic */
    { 0x0530, 0x058F, "SFB" },    /* Armenian */
    { 0x0590, 0x06FF, "SB" },     /* Hebrew, Arabic */
    { 0x0700, 0x074F, "SH" },     /* Syriac */
    { 0x0750, 0x077F, "SB" },     /* Arabic supplement */
    { 0x0780, 0x07BF, "M" },      /* Thaana */
    { 0x07C0, 0x07FF, "E" },      /* N'Ko */
    { 0x0800, 0x08FF, "SBH" },    /* Samaritan, Arabic extended */
    { 0x0900, 0x0DFF, "N" },      /* Devanagari to Sinhala */
    { 0x0E00, 0x0EFF, "L" },      /* Thai, Lao */
    { 0x0F00, 0x0FFF, "T" },      /* Tibetan */
    { 0x1000, 0x109F, "R" },      /* Myanmar */
    { 0x10A0, 0x10FF, "SFH" },    /* Georgian */
    { 0x1100, 0x11FF, "KY" },     /* Hangul jamo */
    { 0x1200, 0x139F, "E" },      /* Ethiopic */
    { 0x13A0, 0x167F, "CE" },     /* Cherokee, Canadian syllabics */
    { 0x1680, 0x169F, "SH" },     /* Ogham */
    { 0x16A0, 0x177F, "H" },      /* Runic and the Philippine scripts */
    { 0x1780, 0x17FF, "L" },      /* Khmer */
    { 0x1800, 0x18AF, "O" },      /* Mongolian */
    { 0x18B0, 0x19DF, "CH" },     /* more Canadian syllabics, Limbu, Tai Le, New Tai Lue */
    { 0x19E0, 0x19FF, "L" },      /* Khmer symbols */
    { 0x1A00, 0x1C8F, "NH" },     /* Buginese, Balinese, Sundanese, Ol Chiki */
    { 0x1C90, 0x1CBF, "SFH" },    /* Georgian extended */
    { 0x1CC0, 0x1CFF, "NH" },     /* Vedic extensions */
    { 0x1D00, 0x1FFF, "SB" },     /* phonetic extensions, Greek extended */
    { 0x2000, 0x2BFF, "SBH" },    /* punctuation, scripts, currency, letterlike, arrows, math, shapes, dingbats */
    { 0x2C00, 0x2DDF, "HE" },     /* Glagolitic, Coptic, Tifinagh, Ethiopic extended */
    { 0x2DE0, 0x2E7F, "SB" },     /* Cyrillic extended-A, supplemental punctuation */
    { 0x2E80, 0x303F, "YGKJ" },   /* CJK radicals and punctuation */
    { 0x3040, 0x30FF, "GYKJ" },   /* hiragana, katakana */
    { 0x3100, 0x312F, "YJ" },     /* Bopomofo */
    { 0x3130, 0x318F, "KYG" },    /* Hangul compatibility jamo */
    { 0x3190, 0x31FF, "YGKJ" },
    { 0x3200, 0x33FF, "KYG" },    /* enclosed and compatibility CJK */
    { 0x3400, 0x9FFF, "YJGKU" },  /* Han */
    { 0xA000, 0xA4CF, "I" },      /* Yi */
    { 0xA4D0, 0xA4FF, "SH" },     /* Lisu */
    { 0xA500, 0xA63F, "E" },      /* Vai */
    { 0xA640, 0xA7FF, "SB" },     /* Cyrillic extended-B, Latin extended-D */
    { 0xA800, 0xA8FF, "NH" },     /* Syloti Nagri, Phags-pa, Saurashtra, Devanagari extended */
    { 0xA900, 0xA95F, "H" },
    { 0xA960, 0xA97F, "K" },      /* Hangul jamo extended-A */
    { 0xA980, 0xA9DF, "V" },      /* Javanese */
    { 0xA9E0, 0xA9FF, "R" },      /* Myanmar extended-B */
    { 0xAA00, 0xAAFF, "H" },
    { 0xAB00, 0xAB2F, "E" },      /* Ethiopic extended-A */
    { 0xAB30, 0xAB6F, "SB" },     /* Latin extended-E */
    { 0xAB70, 0xABBF, "C" },      /* Cherokee supplement */
    { 0xAC00, 0xD7FF, "KY" },     /* Hangul syllables */
    { 0xF900, 0xFAFF, "YJGK" },   /* CJK compatibility ideographs */
    { 0xFB00, 0xFEFF, "SB" },     /* presentation forms, variation selectors */
    { 0xFF00, 0xFFEF, "YGKJ" },   /* half and full width forms */
    { 0xFFF0, 0xFFFF, "SB" },
    { 0x1D000, 0x1DFFF, "BH" },   /* music, math alphanumerics */
    { 0x1E800, 0x1EFFF, "E" },    /* Mende, Adlam, Arabic mathematical */
    { 0x1F000, 0x1FFFF, "B" },    /* pictographs, game tiles (emoji go to the emoji font first) */
    { 0x20000, 0x2FFFF, "XYJ" },  /* Han extensions */
    { 0x10000, 0x1CFFF, "HB" },   /* historic scripts */
};
/* what no range names */
#define CUI__SF_DEFAULT "SBH"

#define CUI__SF_CACHE 1024   /* a power of two */
typedef struct { uint32_t cp; int16_t font; uint8_t weight; int gi; } cui__sf_entry;

typedef struct cui__sysfont {
    bool on;
    int font[CUI__SF_SLOTS][2];          /* internal font index per slot and weight; -1 not (yet) there */
    uint8_t tried[CUI__SF_SLOTS][2];     /* the load was attempted */
    void *views[CUI__SF_SLOTS * 2];      /* the mapped files, let go at the end */
    int view_count;
    cui__sf_entry cache[CUI__SF_CACHE];  /* by code point: found (font >= 0) or known missing (font < 0) */
} cui__sysfont;

/* maps a file of the Fonts folder read only; NULL when it is not there or cannot be mapped */
static void *cui__sf_map(const char *file, size_t *size) {
    wchar_t dir[MAX_PATH], path[MAX_PATH * 2], wf[64];
    DWORD n = GetEnvironmentVariableW(L"WINDIR", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) wcscpy(dir, L"C:\\Windows");
    if (!MultiByteToWideChar(CP_UTF8, 0, file, -1, wf, 64)) return NULL;
    _snwprintf(path, MAX_PATH * 2, L"%ls\\Fonts\\%ls", dir, wf);
    path[MAX_PATH * 2 - 1] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    void *p = NULL;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 12 && sz.QuadPart < 0x7FFFFFFF) {
        HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) {
            p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(m);
            *size = (size_t)sz.QuadPart;
        }
    }
    CloseHandle(f);
    return p;
}

/* the internal font index of a slot at a weight (1: semibold where the slot has it), loading it the first time; -1 for none */
static int cui__sf_font(cui_ctx *ui, cui__sysfont *sf, int slot, int weight) {
    int v = weight && cui__sf_slots[slot].semibold ? 1 : 0;
    if (sf->tried[slot][v]) return sf->font[slot][v];
    if (ui->font_count >= ui->font_cap) return -1;   /* no room that keeps the array still: try again later */
    sf->tried[slot][v] = 1;
    size_t size = 0;
    void *p = cui__sf_map(v ? cui__sf_slots[slot].semibold : cui__sf_slots[slot].file, &size);
    if (!p) return -1;
    int id = cui__font_add(ui, p, size, false, cui__sf_slots[slot].face, true);
    if (id < 0) { UnmapViewOfFile(p); return -1; }
    sf->views[sf->view_count++] = p;
    sf->font[slot][v] = id;
    ui->settle_frames = CUI_MAX(ui->settle_frames, 2);   /* a script just arrived: lay out and draw again */
    ui->animating = true;
    return id;
}

static void cui__sf_free(cui_ctx *ui, cui__sysfont *sf) {
    for (int i = 0; i < sf->view_count; i++) UnmapViewOfFile(sf->views[i]);
    cui__free(ui, sf);
}

static const char *cui__sf_chain(uint32_t cp) {
    for (size_t i = 0; i < sizeof cui__sf_ranges / sizeof *cui__sf_ranges; i++)
        if (cp >= cui__sf_ranges[i].lo && cp <= cui__sf_ranges[i].hi) return cui__sf_ranges[i].chain;
    return CUI__SF_DEFAULT;
}

static int cui__sf_slot_of(char key) {
    for (int i = 0; i < CUI__SF_SLOTS; i++) if (cui__sf_slots[i].key == key) return i;
    return -1;
}

static int cui__sysfont_hook(cui_ctx *ui, int what, cui_font font, uint32_t cp, int *gi) {
    cui__sysfont *sf = ui->sysfont;
    if (what == 2) {
        if (sf) cui__sf_free(ui, sf);
        ui->sysfont = NULL; ui->sysfont_hook = NULL;
        return -1;
    }
    if (!sf || !sf->on || cp < 0x80 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xF8FF)) return -1;
    /* a character the emoji font draws stays with it, however the symbol fonts draw it */
    if (ui->emoji_text && cp >= 0x203C) {
        char u[8];
        int n = cui__utf8_encode(cp, u);
        if (n > 0 && ui->emoji_text(ui, u, n, NULL, 0) > 0) return -1;
    }
    int weight = font >= 0 && (font == ui->roles[CUI_FONT_MEDIUM] || font == ui->roles[CUI_FONT_DISPLAY]);
    cui__sf_entry *e = &sf->cache[(cp * 2654435761u >> 12) & (CUI__SF_CACHE - 1)];
    if (e->cp == cp && e->weight == weight) {
        if (e->font < 0) return -1;
        *gi = e->gi;
        return e->font;
    }
    int found = -1, g = 0;
    for (const char *c = cui__sf_chain(cp); *c && found < 0; c++) {
        int slot = cui__sf_slot_of(*c);
        int f = slot >= 0 ? cui__sf_font(ui, sf, slot, weight) : -1;
        if (f < 0) continue;
        g = cui__font_own_glyph(ui, f, cp);
        if (g) found = f;
    }
    e->cp = cp; e->weight = (uint8_t)weight; e->font = (int16_t)found; e->gi = g;
    if (found < 0) return -1;
    *gi = g;
    return found;
}

/* ---- an installed family by name (cui_font_load_system) ---- */

/* One value name of the Fonts key ("Cascadia Mono Regular (TrueType)", "MS Gothic & MS UI Gothic & MS PGothic (TrueType)"):
 * the face of the collection whose name is `family` or "`family` Regular" (the names are in face order), or -1. */
static int cui__sf_named_face(const wchar_t *value, const wchar_t *family) {
    wchar_t buf[256];
    if (wcslen(value) >= 256) return -1;
    wcscpy(buf, value);
    wchar_t *br = wcsrchr(buf, L'(');
    if (br) *br = 0;
    size_t fl = wcslen(family);
    int face = 0;
    for (wchar_t *s = buf; s && *s; face++) {
        wchar_t *amp = wcsstr(s, L" & ");
        if (amp) *amp = 0;
        while (*s == L' ') s++;
        size_t len = wcslen(s);
        while (len && s[len - 1] == L' ') s[--len] = 0;
        if (!_wcsicmp(s, family)) return face;
        if (len > fl && !_wcsnicmp(s, family, fl) && !_wcsicmp(s + fl, L" Regular")) return face;
        s = amp ? amp + 3 : NULL;
    }
    return -1;
}

/* The file and face of `family` in one Fonts key (read only); false when it is not there. */
static bool cui__sf_find_in(HKEY root, const wchar_t *family, wchar_t *file, DWORD file_cap, int *face) {
    HKEY k;
    if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    bool found = false;
    for (DWORD i = 0; !found && i < 100000; i++) {
        wchar_t name[256];
        DWORD nl = 256, type = 0, dl = (file_cap - 1) * (DWORD)sizeof(wchar_t);
        LONG r = RegEnumValueW(k, i, name, &nl, NULL, &type, (LPBYTE)file, &dl);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        file[CUI_MIN(dl / sizeof(wchar_t), file_cap - 1)] = 0;
        int f = cui__sf_named_face(name, family);
        if (f >= 0) { *face = f; found = true; }
    }
    RegCloseKey(k);
    return found;
}

/* Files mapped for cui_font_load_system, kept for the life of the process and shared by every context (window). */
static struct { wchar_t path[MAX_PATH * 2]; void *p; size_t n; } cui__sf_maps[16];

static void *cui__sf_map_path(const wchar_t *path, size_t *size) {
    for (int i = 0; i < 16; i++) if (cui__sf_maps[i].p && !_wcsicmp(cui__sf_maps[i].path, path)) { *size = cui__sf_maps[i].n; return cui__sf_maps[i].p; }
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    void *p = NULL;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 12 && sz.QuadPart < 0x7FFFFFFF) {
        HANDLE m = CreateFileMappingW(f, NULL, PAGE_READONLY, 0, 0, NULL);
        if (m) { p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0); CloseHandle(m); *size = (size_t)sz.QuadPart; }
    }
    CloseHandle(f);
    if (!p) return NULL;
    for (int i = 0; i < 16; i++) if (!cui__sf_maps[i].p) {
        wcsncpy(cui__sf_maps[i].path, path, MAX_PATH * 2 - 1);
        cui__sf_maps[i].p = p; cui__sf_maps[i].n = *size;
        break;
    }
    return p;
}

/* Last resort: the font GDI picks for the name, copied with GetFontData; only a single font file (not a collection), and
 * only when GDI really has the family (for a name it does not know it substitutes another one). */
static cui_font cui__sf_gdi_copy(cui_ctx *ui, const wchar_t *family) {
    cui_font id = -1;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return -1;
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -64; lf.lfCharSet = DEFAULT_CHARSET; lf.lfWeight = FW_NORMAL;
    wcsncpy(lf.lfFaceName, family, LF_FACESIZE - 1);
    HFONT hf = CreateFontIndirectW(&lf);
    if (hf) {
        HGDIOBJ old = SelectObject(dc, hf);
        wchar_t face[LF_FACESIZE] = { 0 };
        GetTextFaceW(dc, LF_FACESIZE, face);
        if (!_wcsicmp(face, family) && GetFontData(dc, 0x66637474u /* 'ttcf' */, 0, NULL, 0) == GDI_ERROR) {
            DWORD n = GetFontData(dc, 0, 0, NULL, 0);
            if (n != GDI_ERROR && n > 12 && n < (64u << 20)) {
                void *buf = malloc(n);
                if (buf && GetFontData(dc, 0, 0, buf, n) == n) id = cui__font_load_face(ui, buf, n, true, 0);
                free(buf);
            }
        }
        SelectObject(dc, old);
        DeleteObject(hf);
    }
    DeleteDC(dc);
    return id;
}

cui_font cui_font_load_system(cui_ctx *ui, const char *family) {
    if (!ui || !family || !*family) return -1;
    wchar_t wf[LF_FACESIZE * 2], file[MAX_PATH * 2];
    if (!MultiByteToWideChar(CP_UTF8, 0, family, -1, wf, LF_FACESIZE * 2)) return -1;
    int face = 0;
    if (cui__sf_find_in(HKEY_CURRENT_USER, wf, file, MAX_PATH * 2, &face) || cui__sf_find_in(HKEY_LOCAL_MACHINE, wf, file, MAX_PATH * 2, &face)) {
        wchar_t path[MAX_PATH * 2];
        if (wcschr(file, L'\\') || wcschr(file, L'/')) { wcsncpy(path, file, MAX_PATH * 2 - 1); path[MAX_PATH * 2 - 1] = 0; }
        else {
            wchar_t dir[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"WINDIR", dir, MAX_PATH);
            if (!n || n >= MAX_PATH) wcscpy(dir, L"C:\\Windows");
            _snwprintf(path, MAX_PATH * 2, L"%ls\\Fonts\\%ls", dir, file);
            path[MAX_PATH * 2 - 1] = 0;
        }
        size_t size = 0;
        void *p = cui__sf_map_path(path, &size);
        cui_font id = p ? cui__font_load_face(ui, p, size, false, face) : -1;
        if (id >= 0) { cui__fonts_changed(ui); return id; }
    }
    cui_font id = cui__sf_gdi_copy(ui, wf);
    if (id >= 0) cui__fonts_changed(ui);
    return id;
}

void cui_font_use_system_fallback(cui_ctx *ui, bool on) {
    if (!ui) return;
    cui__sysfont *sf = ui->sysfont;
    if (!sf) {
        if (!on) return;
        sf = cui__alloc(ui, sizeof *sf);
        if (!sf) return;
        memset(sf, 0, sizeof *sf);
        for (int i = 0; i < CUI__SF_SLOTS; i++) sf->font[i][0] = sf->font[i][1] = -1;
        /* every system font that can be added, so that the array of fonts never moves in the middle of a lookup */
        if (!cui__fonts_reserve(ui, CUI__SF_SLOTS + 1)) { cui__free(ui, sf); return; }
        ui->sysfont = sf;
        ui->sysfont_hook = cui__sysfont_hook;
    }
    if (sf->on == on) return;
    sf->on = on;
    cui__fonts_changed(ui);
}

#else   /* no table of system fonts for this platform yet: the option does nothing */

void cui_font_use_system_fallback(cui_ctx *ui, bool on) { (void)ui; (void)on; }
cui_font cui_font_load_system(cui_ctx *ui, const char *family) { (void)ui; (void)family; return -1; }

#endif
