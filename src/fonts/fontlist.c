/* SPDX-License-Identifier: MIT */
#include <fontconfig/fontconfig.h>
#include <stdlib.h>
#include <string.h>

#include "fontlist.h"

const char *xd_font_format_label(const char *fontformat, const char *file) {
    if (fontformat == nullptr)
        return nullptr;
    bool otf = file != nullptr &&
               (g_str_has_suffix(file, ".otf") || g_str_has_suffix(file, ".OTF") ||
                g_str_has_suffix(file, ".otc") || g_str_has_suffix(file, ".OTC"));
    if (strcmp(fontformat, "CFF") == 0 || (strcmp(fontformat, "TrueType") == 0 && otf))
        return "OpenType";
    if (strcmp(fontformat, "TrueType") == 0)
        return "TrueType";
    if (strcmp(fontformat, "Type 1") == 0 || strcmp(fontformat, "CID Type 1") == 0)
        return "Type 1";
    if (strcmp(fontformat, "PCF") == 0 || strcmp(fontformat, "BDF") == 0 ||
        strcmp(fontformat, "Windows FNT") == 0)
        return "Bitmap";
    return nullptr;
}

static bool is_regular(const char *style) {
    static const char *const names[] = {"Regular", "Normal", "Book", "Roman", "Medium"};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (g_ascii_strcasecmp(style, names[i]) == 0)
            return true;
    return false;
}

char *xd_font_display_name(const char *fullname, const char *family, const char *style,
                           const char *format_label) {
    char *base;
    if (fullname != nullptr && fullname[0] != '\0')
        base = g_strdup(fullname);
    else if (style != nullptr && style[0] != '\0' && !is_regular(style))
        base = g_strdup_printf("%s %s", family != nullptr ? family : "", style);
    else
        base = g_strdup(family != nullptr ? family : "");
    if (format_label == nullptr)
        return base;
    char *name = g_strdup_printf("%s (%s)", base, format_label);
    g_free(base);
    return name;
}

bool xd_path_in_dir(const char *path, const char *dir) {
    size_t count = strlen(dir);
    while (count > 1 && dir[count - 1] == '/')
        count--;
    return count > 0 && strncmp(path, dir, count) == 0 && path[count] == '/' &&
           path[count + 1] != '\0';
}

static int distance(const xd_font *font) {
    int score = abs(font->weight - FC_WEIGHT_REGULAR) + abs(font->width - FC_WIDTH_NORMAL);
    if (font->slant != FC_SLANT_ROMAN)
        score += 1000;
    return score;
}

void xd_fonts_mark_primary(xd_font *const *fonts, size_t count) {
    GHashTable *best = g_hash_table_new(g_str_hash, g_str_equal); /* family -> xd_font* */
    for (size_t i = 0; i < count; i++) {
        xd_font *font = fonts[i];
        font->primary = false;
        xd_font *best_font = g_hash_table_lookup(best, font->family);
        if (best_font == nullptr || distance(font) < distance(best_font))
            g_hash_table_insert(best, font->family, font);
    }
    GHashTableIter it;
    gpointer value;
    g_hash_table_iter_init(&it, best);
    while (g_hash_table_iter_next(&it, nullptr, &value))
        ((xd_font *)value)->primary = true;
    g_hash_table_unref(best);
}

char *xd_font_version_text(int fontversion) {
    if (fontversion <= 0)
        return g_strdup("");
    /* 16.16 fixed point, shown to two places as XP does. */
    int major = fontversion >> 16;
    int hundredths = (int)(((fontversion & 0xffff) * 100 + 0x8000) >> 16);
    if (hundredths == 100) {
        major++;
        hundredths = 0;
    }
    return g_strdup_printf("Version %d.%02d", major, hundredths);
}

char *xd_fonts_user_dir(void) { return g_build_filename(g_get_user_data_dir(), "fonts", nullptr); }

bool xd_font_is_user(const char *file) {
    char *dir = xd_fonts_user_dir();
    char *legacy = g_build_filename(g_get_home_dir(), ".fonts", nullptr);
    bool user = xd_path_in_dir(file, dir) || xd_path_in_dir(file, legacy);
    g_free(legacy);
    g_free(dir);
    return user;
}

static char *pattern_string(FcPattern *pattern, const char *object) {
    FcChar8 *text = nullptr;
    return FcPatternGetString(pattern, object, 0, &text) == FcResultMatch
               ? g_strdup((const char *)text)
               : nullptr;
}

static int pattern_int(FcPattern *pattern, const char *object, int fallback) {
    int value = fallback;
    FcPatternGetInteger(pattern, object, 0, &value);
    return value;
}

static void font_free(gpointer data) {
    xd_font *font = data;
    g_free(font->file);
    g_free(font->family);
    g_free(font->style);
    g_free(font->name);
    g_free(font);
}

static int compare_fonts(gconstpointer left, gconstpointer right) {
    const xd_font *left_font = *(xd_font *const *)left, *right_font = *(xd_font *const *)right;
    int order = g_utf8_collate(left_font->name, right_font->name);
    return order != 0 ? order : strcmp(left_font->file, right_font->file);
}

GPtrArray *xd_fonts_list(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(font_free);
    FcPattern *pat = FcPatternCreate();
    FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_FAMILY, FC_STYLE, FC_FULLNAME,
                                       FC_FONTFORMAT, FC_WEIGHT, FC_SLANT, FC_WIDTH, FC_VARIABLE,
                                       nullptr);
    FcFontSet *set = FcFontList(nullptr, pat, os);
    for (int i = 0; set != nullptr && i < set->nfont; i++) {
        FcPattern *pattern = set->fonts[i];
        FcBool variable = FcFalse;
        /* A variable font lists each named instance, and itself. */
        if (FcPatternGetBool(pattern, FC_VARIABLE, 0, &variable) == FcResultMatch && variable)
            continue;
        char *file = pattern_string(pattern, FC_FILE);
        char *family = pattern_string(pattern, FC_FAMILY);
        if (file == nullptr || family == nullptr) {
            g_free(file);
            g_free(family);
            continue;
        }
        xd_font *font = g_new0(xd_font, 1);
        font->file = file;
        font->family = family;
        font->index = pattern_int(pattern, FC_INDEX, 0);
        font->style = pattern_string(pattern, FC_STYLE);
        font->weight = pattern_int(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);
        font->slant = pattern_int(pattern, FC_SLANT, FC_SLANT_ROMAN);
        font->width = pattern_int(pattern, FC_WIDTH, FC_WIDTH_NORMAL);
        char *full = pattern_string(pattern, FC_FULLNAME);
        char *format = pattern_string(pattern, FC_FONTFORMAT);
        font->name =
            xd_font_display_name(full, family, font->style, xd_font_format_label(format, file));
        g_free(full);
        g_free(format);
        font->user = xd_font_is_user(file);
        g_ptr_array_add(out, font);
    }
    if (set != nullptr)
        FcFontSetDestroy(set);
    FcObjectSetDestroy(os);
    FcPatternDestroy(pat);
    g_ptr_array_sort(out, compare_fonts);
    xd_fonts_mark_primary((xd_font *const *)out->pdata, out->len);
    return out;
}

void xd_fonts_rescan(void) {
    char *dir = xd_fonts_user_dir();
    /* Forcing a read scans the folder and writes its cache. */
    FcCache *cache = FcDirCacheRead((const FcChar8 *)dir, FcTrue, nullptr);
    if (cache != nullptr)
        FcDirCacheUnload(cache);
    g_free(dir);
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (config != nullptr) {
        FcConfigSetCurrent(config);
        FcConfigDestroy(config); /* the current configuration holds its own reference */
    }
}
