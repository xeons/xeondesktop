/* SPDX-License-Identifier: MIT
 * Theme packages. See pkg.h and docs/THEMES.md.
 */
#include "pkg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

#include "scale.h"
#include "style.h"

/* Caps on what a zip may hold: a theme's images are small. */
enum { ZIP_ENTRY_MAX = 64 << 20, ZIP_TOTAL_MAX = 256 << 20 };

struct xd_pkg {
    char *name;
    char *variant;
    xd_visual_style base;
    bool has_filter;
    xd_image_filter filter;
    char *root;      /* a folder, or nullptr for a zip */
    GHashTable *zip; /* path -> GBytes */
    /* "class\x1fpart\x1fstate\x1fprop", lowercase, parts and states as
     * numbers where uxtheme.ini knows them -> value. */
    GHashTable *props;
    /* "class\x1fpart" -> TRUE for parts with any property. */
    GHashTable *parts;
    GHashTable *sys; /* "kind:name" lowercase -> value */
    /* uxtheme.ini: "class.part" -> number, "class.part.state" -> number. */
    GHashTable *part_numbers;
    GHashTable *state_numbers;
    /* Filled while drawing, through a const package: behind a pointer. */
    struct pkg_cache *cache;
    /* [SysMetrics] over the colour scheme, as a visual style's are in XP. */
    uint64_t color_mask;
    xd_rgb colors[XD_COLOR_COUNT];
    uint32_t metric_mask;
    int metrics[XD_METRIC_COUNT];
    uint32_t font_mask;
    xd_font_spec fonts[XD_FONT_COUNT];
};

typedef struct pkg_cache {
    GMutex lock;
    GHashTable *images; /* path -> cairo_surface_t (as loaded) */
    GHashTable *scaled; /* key -> cairo_surface_t (a scaled strip) */
    GHashTable *faces;  /* font face strings handed out */
} pkg_cache;

/* ---- the active package ---------------------------------------------------------- */

static const xd_pkg *active_pkg;
static xd_image_filter active_filter = XD_FILTER_SMOOTH;
static bool active_integer = true;
/* The environment's package was looked at, or the program chose one. */
static bool chosen;
/* $XD_THEME_PACKAGE's, kept for the process's life even once replaced:
 * whoever replaces it may not know it's still drawn with somewhere. */
static xd_pkg *env_pkg;

void xd_theme_use_package(const xd_pkg *pkg, xd_image_filter filter, bool integer_scaling) {
    chosen = true;
    active_pkg = pkg;
    active_filter = filter;
    active_integer = integer_scaling;
}

/* uxtheme.ini: $XD_UXTHEME_NAMES, the source tree beside a build
 * directory, or the install prefix. */
static char *names_path(void) {
    const char *env = getenv("XD_UXTHEME_NAMES");
    if (env != nullptr && *env != '\0')
        return g_strdup(env);
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    if (self != nullptr) {
        char *dir = g_path_get_dirname(self);
        g_free(self);
        char *path = g_build_filename(dir, "..", "data", "theme", "uxtheme.ini", nullptr);
        g_free(dir);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return path;
        g_free(path);
    }
    return g_build_filename(XD_THEME_DATADIR, "theme", "uxtheme.ini", nullptr);
}

/* For programs that don't choose a package from the settings themselves
 * (those that do take these as an override, settings/themepkg.c):
 * $XD_THEME_PACKAGE (a .xdtheme or folder), $XD_THEME_VARIANT (its id),
 * $XD_IMAGE_FILTER (a filter's name; "Automatic" or unset: the
 * package's own, else Smooth). Loaded once, on the first theme. */
static void package_from_environment(void) {
    if (chosen)
        return;
    chosen = true;
    const char *path = getenv("XD_THEME_PACKAGE");
    if (path == nullptr || *path == '\0')
        return;
    char *names = names_path();
    GError *error = nullptr;
    env_pkg = xd_pkg_load(path, getenv("XD_THEME_VARIANT"), names, &error);
    g_free(names);
    if (env_pkg == nullptr) {
        fprintf(stderr, "XD_THEME_PACKAGE: %s\n", error->message);
        g_error_free(error);
        return;
    }
    xd_image_filter image_filter;
    const char *fname = getenv("XD_IMAGE_FILTER");
    if (fname == nullptr || !xd_image_filter_from_name(fname, &image_filter)) {
        if (!xd_pkg_filter(env_pkg, &image_filter))
            image_filter = XD_FILTER_SMOOTH;
    }
    xd_theme_use_package(env_pkg, image_filter, true);
}

static void set_package(xd_theme *theme, const xd_pkg *package, xd_image_filter filter,
                        bool integer) {
    theme->pkg = package;
    theme->image_filter = filter;
    theme->integer_scaling = integer;
    if (package == nullptr)
        return;
    /* What the package doesn't draw, its base style does, with its
     * [SysMetrics] over the scheme. */
    theme->style = package->base;
    for (int i = 0; i < XD_COLOR_COUNT; i++)
        if (package->color_mask & (1ull << i))
            theme->scheme.colors[i] = package->colors[i];
    for (int i = 0; i < XD_METRIC_COUNT; i++)
        if (package->metric_mask & (1u << i))
            theme->scheme.metrics[i] = package->metrics[i];
    for (int i = 0; i < XD_FONT_COUNT; i++)
        if (package->font_mask & (1u << i))
            theme->scheme.fonts[i] = package->fonts[i];
}

void xd_theme_attach_package(xd_theme *theme) {
    package_from_environment();
    set_package(theme, active_pkg, active_filter, active_integer);
}

void xd_theme_init_package(xd_theme *theme, xd_visual_style style, const xd_scheme *scheme,
                           double scale, const xd_pkg *pkg, xd_image_filter filter,
                           bool integer_scaling) {
    xd_theme_init(theme, style, scheme, scale);
    /* Without the active package's base style and [SysMetrics]. */
    theme->style = style;
    theme->scheme = *scheme;
    set_package(theme, pkg, filter, integer_scaling);
}

static size_t numbers(const char *text, int *out, size_t max);
static bool parse_color(const char *text, xd_rgba *out);

/* XP's [SysMetrics] names where they differ from the registry's. */
static const char *const color_names[][2] = {
    {"btnface", "ButtonFace"},
    {"btnshadow", "ButtonShadow"},
    {"btnhighlight", "ButtonHilight"},
    {"btntext", "ButtonText"},
    {"dkshadow3d", "ButtonDkShadow"},
    {"light3d", "ButtonLight"},
    {"highlight", "Hilight"},
    {"highlighttext", "HilightText"},
    {"activecaption", "ActiveTitle"},
    {"inactivecaption", "InactiveTitle"},
    {"captiontext", "TitleText"},
    {"inactivecaptiontext", "InactiveTitleText"},
    {"gradientactivecaption", "GradientActiveTitle"},
    {"gradientinactivecaption", "GradientInactiveTitle"},
    {"infobk", "InfoWindow"},
    {"hottracking", "HotTrackingColor"},
};

static const struct {
    const char *name;
    xd_metric metric;
} metric_names[] = {
    {"captionbarheight", XD_METRIC_CAPTION_HEIGHT},
    {"smcaptionbarheight", XD_METRIC_SM_CAPTION_HEIGHT},
    {"captionbarwidth", XD_METRIC_CAPTION_BUTTON_WIDTH},
    {"scrollbarwidth", XD_METRIC_SCROLL_WIDTH},
    {"menubarheight", XD_METRIC_MENU_HEIGHT},
};

static const struct {
    const char *name;
    xd_font_role role;
} font_names[] = {
    {"captionfont", XD_FONT_CAPTION}, {"smallcaptionfont", XD_FONT_SMCAPTION},
    {"menufont", XD_FONT_MENU},       {"statusfont", XD_FONT_TOOLTIP},
    {"icontitlefont", XD_FONT_ICON},
    /* Not MsgBoxFont: the message font is our dialogs' font, which
     * Windows doesn't take from the style. Its dialogs stay in MS Shell
     * Dlg (Tahoma 8) under 7's Segoe UI 9 and XP's Large Fonts, and are
     * laid out for it. */
};

/* Windows' fonts become the free ones standing in for them (and the
 * real ones when installed), as the schemes' do. */
static void font_family(const char *face, char *out, size_t cap) {
    static const char *const known[][2] = {
        {"tahoma", XD_FONT_FAMILY_TAHOMA},
        {"trebuchet ms", XD_FONT_FAMILY_TREBUCHET},
        {"segoe ui", XD_FONT_FAMILY_SEGOE},
        {"franklin gothic medium", XD_FONT_FAMILY_FRANKLIN},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(known); i++)
        if (g_ascii_strcasecmp(face, known[i][0]) == 0) {
            g_strlcpy(out, known[i][1], cap);
            return;
        }
    g_snprintf(out, cap, "%s, sans-serif", face);
}

static void sysmetrics_overlay(xd_pkg *package) {
    GHashTableIter it;
    gpointer hash_key, hash_value;
    g_hash_table_iter_init(&it, package->sys);
    while (g_hash_table_iter_next(&it, &hash_key, &hash_value)) {
        const char *key = hash_key;
        if (g_str_has_prefix(key, "color:")) {
            const char *name = key + 6;
            for (size_t i = 0; i < G_N_ELEMENTS(color_names); i++)
                if (strcmp(name, color_names[i][0]) == 0)
                    name = color_names[i][1];
            xd_syscolor color_index;
            xd_rgba rgba;
            if (xd_syscolor_from_name(xd_str_from_cstr(name), &color_index) &&
                parse_color(hash_value, &rgba)) {
                package->colors[color_index] =
                    (xd_rgb){(uint8_t)lround(rgba.r * 255), (uint8_t)lround(rgba.g * 255),
                             (uint8_t)lround(rgba.b * 255)};
                package->color_mask |= 1ull << color_index;
            }
        } else if (g_str_has_prefix(key, "value:")) {
            int values[1];
            for (size_t i = 0; i < G_N_ELEMENTS(metric_names); i++)
                if (strcmp(key + 6, metric_names[i].name) == 0 &&
                    numbers(hash_value, values, 1) == 1) {
                    package->metrics[metric_names[i].metric] = values[0];
                    package->metric_mask |= 1u << metric_names[i].metric;
                }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(font_names); i++) {
        const char *face;
        int points;
        bool bold, italic;
        if (!xd_pkg_sys_font(package, font_names[i].name, &face, &points, &bold, &italic) ||
            points <= 0)
            continue;
        xd_font_spec *font = &package->fonts[font_names[i].role];
        font_family(face, font->family, sizeof(font->family));
        font->size_pt = points;
        font->bold = bold;
        font->italic = italic;
        package->font_mask |= 1u << font_names[i].role;
    }
}

/* ---- names ----------------------------------------------------------------------- */

static char *lower(const char *text) { return g_ascii_strdown(text, -1); }

/* "TaskBand::Toolbar" -> "toolbar"; the class uxtheme's names are for. */
static char *base_class(const char *cls) {
    const char *colon = strstr(cls, "::");
    return lower(colon != nullptr ? colon + 2 : cls);
}

static bool all_digits(const char *text) {
    if (text == nullptr || *text == '\0')
        return false;
    for (; *text != '\0'; text++)
        if (!g_ascii_isdigit(*text))
            return false;
    return true;
}

/* A part as stored: its number where uxtheme.ini knows it, else its name
 * (lowercase); "0" for none. */
static char *part_key(const xd_pkg *package, const char *cls, const char *part) {
    if (part == nullptr || *part == '\0')
        return g_strdup("0");
    if (all_digits(part))
        return g_strdup(part);
    char *base = base_class(cls);
    char *name = lower(part);
    char *key = g_strdup_printf("%s.%s", base, name);
    gpointer number = g_hash_table_lookup(package->part_numbers, key);
    if (number == nullptr) {
        /* Vista's ButtonStyle has Button's parts. */
        char *style_key = g_strdup_printf("%sstyle.%s", base, name);
        number = g_hash_table_lookup(package->part_numbers, style_key);
        g_free(style_key);
    }
    char *out = number != nullptr ? g_strdup_printf("%d", GPOINTER_TO_INT(number)) : g_strdup(name);
    g_free(key);
    g_free(name);
    g_free(base);
    return out;
}

/* A part's name, from its stored key (a number or a name). */
static char *part_name(const xd_pkg *package, const char *cls, const char *pkey) {
    if (!all_digits(pkey))
        return g_strdup(pkey);
    char *base = base_class(cls);
    char *prefix = g_strdup_printf("%s.", base);
    char *found = nullptr;
    GHashTableIter it;
    gpointer hash_key, hash_value;
    g_hash_table_iter_init(&it, package->part_numbers);
    while (found == nullptr && g_hash_table_iter_next(&it, &hash_key, &hash_value)) {
        const char *key = hash_key;
        if (g_str_has_prefix(key, prefix) && strchr(key + strlen(prefix), '.') == nullptr &&
            GPOINTER_TO_INT(hash_value) == atoi(pkey))
            found = g_strdup(key + strlen(prefix));
    }
    g_free(prefix);
    g_free(base);
    return found;
}

static char *state_key(const xd_pkg *package, const char *cls, const char *pkey,
                       const char *state) {
    if (state == nullptr || *state == '\0')
        return g_strdup("0");
    if (all_digits(state))
        return g_strdup(state);
    char *name = lower(state);
    char *base = base_class(cls);
    char *pname = part_name(package, cls, pkey);
    gpointer number = nullptr;
    if (pname != nullptr) {
        char *key = g_strdup_printf("%s.%s.%s", base, pname, name);
        number = g_hash_table_lookup(package->state_numbers, key);
        g_free(key);
        if (number == nullptr) {
            /* Vista's HeaderStyle has more of Header's states. */
            key = g_strdup_printf("%sstyle.%s.%s", base, pname, name);
            number = g_hash_table_lookup(package->state_numbers, key);
            g_free(key);
        }
    }
    if (number == nullptr) {
        /* A state on the class itself ("Toolbar(Hot)"), or on a part whose
         * states aren't listed: any of the class's parts that has it. */
        char *prefix = g_strdup_printf("%s.", base);
        char *suffix = g_strdup_printf(".%s", name);
        GHashTableIter it;
        gpointer hash_key, hash_value;
        g_hash_table_iter_init(&it, package->state_numbers);
        while (number == nullptr && g_hash_table_iter_next(&it, &hash_key, &hash_value))
            if (g_str_has_prefix(hash_key, prefix) && g_str_has_suffix(hash_key, suffix))
                number = hash_value;
        g_free(suffix);
        g_free(prefix);
    }
    char *out = number != nullptr ? g_strdup_printf("%d", GPOINTER_TO_INT(number)) : g_strdup(name);
    g_free(pname);
    g_free(base);
    g_free(name);
    return out;
}

static bool load_names(xd_pkg *package, const char *path, GError **error) {
    GKeyFile *kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, error)) {
        g_key_file_free(kf);
        return false;
    }
    gsize ngroups = 0;
    char **groups = g_key_file_get_groups(kf, &ngroups);
    for (gsize i = 0; i < ngroups; i++) {
        bool parts = g_str_has_prefix(groups[i], "Parts ");
        bool states = g_str_has_prefix(groups[i], "States ");
        if (!parts && !states)
            continue;
        char *what = lower(groups[i] + (parts ? 6 : 7));
        gsize nkeys = 0;
        char **keys = g_key_file_get_keys(kf, groups[i], &nkeys, nullptr);
        for (gsize k = 0; k < nkeys; k++) {
            int number = g_key_file_get_integer(kf, groups[i], keys[k], nullptr);
            char *name = lower(keys[k]);
            g_hash_table_insert(parts ? package->part_numbers : package->state_numbers,
                                g_strdup_printf("%s.%s", what, name), GINT_TO_POINTER(number));
            g_free(name);
        }
        g_strfreev(keys);
        g_free(what);
    }
    g_strfreev(groups);
    g_key_file_free(kf);
    return true;
}

/* ---- reading the package's files ------------------------------------------------- */

static GBytes *read_file(const xd_pkg *package, const char *path) {
    /* Package paths are relative, without "..": nothing outside it. */
    if (path == nullptr || path[0] == '/' || strstr(path, "..") != nullptr)
        return nullptr;
    if (package->zip != nullptr) {
        GBytes *bytes = g_hash_table_lookup(package->zip, path);
        return bytes != nullptr ? g_bytes_ref(bytes) : nullptr;
    }
    return xd_pkg_read_folder_file(package->root, path, ZIP_ENTRY_MAX, nullptr);
}

#if XD_HAVE_LIBARCHIVE
static bool read_zip(xd_pkg *package, const char *path, GError **error) {
    struct archive *reader = archive_read_new();
    archive_read_support_format_zip(reader);
    bool ok = archive_read_open_filename(reader, path, 64 * 1024) == ARCHIVE_OK;
    gsize total = 0;
    struct archive_entry *archive_member;
    while (ok && archive_read_next_header(reader, &archive_member) == ARCHIVE_OK) {
        la_int64_t size = archive_entry_size(archive_member);
        const char *name = archive_entry_pathname(archive_member);
        if (archive_entry_filetype(archive_member) != AE_IFREG || name == nullptr || size < 0 ||
            size > ZIP_ENTRY_MAX || total + (gsize)size > ZIP_TOTAL_MAX) {
            archive_read_data_skip(reader);
            continue;
        }
        char *buf = g_malloc((gsize)size + 1);
        la_ssize_t bytes_read = archive_read_data(reader, buf, (size_t)size);
        if (bytes_read != size) {
            g_free(buf);
            ok = false;
            break;
        }
        total += (gsize)size;
        g_hash_table_insert(package->zip, g_strdup(name), g_bytes_new_take(buf, (gsize)size));
    }
    if (!ok)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", path,
                    archive_error_string(reader) != nullptr ? archive_error_string(reader)
                                                            : "not a theme package");
    archive_read_free(reader);
    return ok;
}
#endif

/* ---- theme.xml and the style ---------------------------------------------------- */

typedef struct parse_ctx {
    xd_pkg *pkg;
    const char *want_variant;
    char *style_path; /* theme.xml: the chosen variant's style */
    char *shell_path; /* and its Explorer panes (shell/NAME.xml), if any */
    char *cls;        /* style: the class being read */
} parse_ctx;

static const char *attr(const char **names, const char **values, const char *name) {
    for (size_t i = 0; names[i] != nullptr; i++)
        if (g_ascii_strcasecmp(names[i], name) == 0)
            return values[i];
    return nullptr;
}

static void theme_start(G_GNUC_UNUSED GMarkupParseContext *mc, const char *element,
                        const char **names, const char **values, gpointer data,
                        G_GNUC_UNUSED GError **error) {
    parse_ctx *parse_state = data;
    if (strcmp(element, "xdtheme") == 0) {
        const char *name = attr(names, values, "name");
        g_free(parse_state->pkg->name);
        parse_state->pkg->name = g_strdup(name != nullptr ? name : "");
        const char *base = attr(names, values, "base");
        if (base == nullptr ||
            !xd_visual_style_from_name(xd_str_from_cstr(base), &parse_state->pkg->base))
            parse_state->pkg->base = XD_STYLE_CLASSIC;
    } else if (strcmp(element, "variant") == 0) {
        const char *id = attr(names, values, "id");
        const char *style = attr(names, values, "style");
        if (id == nullptr || style == nullptr)
            return;
        bool wanted =
            parse_state->want_variant != nullptr && strcmp(id, parse_state->want_variant) == 0;
        if (parse_state->style_path == nullptr || wanted) {
            if (parse_state->style_path != nullptr && !wanted)
                return;
            g_free(parse_state->style_path);
            g_free(parse_state->shell_path);
            g_free(parse_state->pkg->variant);
            parse_state->style_path = g_strdup(style);
            parse_state->shell_path = g_strdup(attr(names, values, "shell"));
            parse_state->pkg->variant = g_strdup(id);
            if (wanted)
                parse_state->want_variant = nullptr; /* found: keep it */
        }
    } else if (strcmp(element, "scaling") == 0) {
        const char *filter_name = attr(names, values, "filter");
        parse_state->pkg->has_filter =
            filter_name != nullptr &&
            xd_image_filter_from_name(filter_name, &parse_state->pkg->filter);
    }
}

static void add_prop(xd_pkg *package, const char *cls, const char *part, const char *state,
                     const char *prop, const char *value) {
    char *lc = lower(cls);
    char *pk = part_key(package, cls, part);
    char *sk = state_key(package, cls, pk, state);
    char *lp = lower(prop);
    g_hash_table_replace(package->props, g_strdup_printf("%s\x1f%s\x1f%s\x1f%s", lc, pk, sk, lp),
                         g_strdup(value));
    g_hash_table_replace(package->parts, g_strdup_printf("%s\x1f%s", lc, pk), GINT_TO_POINTER(1));
    g_free(lp);
    g_free(sk);
    g_free(pk);
    g_free(lc);
}

static void style_start(G_GNUC_UNUSED GMarkupParseContext *mc, const char *element,
                        const char **names, const char **values, gpointer data,
                        G_GNUC_UNUSED GError **error) {
    parse_ctx *parse_state = data;
    if (strcmp(element, "class") == 0) {
        g_free(parse_state->cls);
        const char *name = attr(names, values, "name");
        parse_state->cls = g_strdup(name != nullptr ? name : "");
    } else if (strcmp(element, "part") == 0 && parse_state->cls != nullptr) {
        const char *part = attr(names, values, "name");
        const char *state = attr(names, values, "state");
        for (size_t i = 0; names[i] != nullptr; i++) {
            if (g_ascii_strcasecmp(names[i], "name") == 0 ||
                g_ascii_strcasecmp(names[i], "state") == 0)
                continue;
            add_prop(parse_state->pkg, parse_state->cls, part, state, names[i], values[i]);
        }
    } else if (strcmp(element, "color") == 0 || strcmp(element, "font") == 0 ||
               strcmp(element, "size") == 0 || strcmp(element, "bool") == 0 ||
               strcmp(element, "value") == 0) {
        const char *name = attr(names, values, "name");
        if (name == nullptr)
            return;
        char *ln = lower(name);
        if (strcmp(element, "font") == 0) {
            const char *face = attr(names, values, "face");
            const char *size = attr(names, values, "size");
            const char *bold = attr(names, values, "bold");
            const char *italic = attr(names, values, "italic");
            g_hash_table_replace(parse_state->pkg->sys, g_strdup_printf("font:%s", ln),
                                 g_strdup_printf("%s\x1f%s\x1f%d\x1f%d", face ? face : "",
                                                 size ? size : "0",
                                                 bold != nullptr && strcmp(bold, "true") == 0,
                                                 italic != nullptr && strcmp(italic, "true") == 0));
        } else {
            const char *value = attr(names, values, "value");
            g_hash_table_replace(
                parse_state->pkg->sys,
                g_strdup_printf("%s:%s", strcmp(element, "color") == 0 ? "color" : "value", ln),
                g_strdup(value != nullptr ? value : ""));
        }
        g_free(ln);
    }
}

static bool parse(xd_pkg *package, const char *path, const GMarkupParser *parser,
                  parse_ctx *parse_state, GError **error) {
    GBytes *bytes = read_file(package, path);
    if (bytes == nullptr) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s is missing", path);
        return false;
    }
    gsize len = 0;
    const char *text = g_bytes_get_data(bytes, &len);
    GMarkupParseContext *mc = g_markup_parse_context_new(parser, 0, parse_state, nullptr);
    bool ok = g_markup_parse_context_parse(mc, text, (gssize)len, error) &&
              g_markup_parse_context_end_parse(mc, error);
    g_markup_parse_context_free(mc);
    g_bytes_unref(bytes);
    if (!ok)
        g_prefix_error(error, "%s: ", path);
    return ok;
}

xd_pkg *xd_pkg_load(const char *path, const char *variant, const char *names_path,
                    GError **error) {
    xd_pkg *package = g_new0(xd_pkg, 1);
    package->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    package->parts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    package->sys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    package->part_numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    package->state_numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    package->cache = g_new0(pkg_cache, 1);
    package->cache->images = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)cairo_surface_destroy);
    package->cache->scaled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify)cairo_surface_destroy);
    package->cache->faces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    g_mutex_init(&package->cache->lock);
    package->name = g_strdup("");
    if (!load_names(package, names_path, error)) {
        xd_pkg_free(package);
        return nullptr;
    }
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        package->root = g_strdup(path);
    } else {
#if XD_HAVE_LIBARCHIVE
        package->zip =
            g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
        if (!read_zip(package, path, error)) {
            xd_pkg_free(package);
            return nullptr;
        }
#else
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                    "%s: theme packages must be folders (built without libarchive)", path);
        xd_pkg_free(package);
        return nullptr;
#endif
    }
    parse_ctx parse_state = {.pkg = package, .want_variant = variant};
    static const GMarkupParser theme_parser = {.start_element = theme_start};
    static const GMarkupParser style_parser = {.start_element = style_start};
    bool ok = parse(package, "theme.xml", &theme_parser, &parse_state, error);
    if (ok && parse_state.style_path == nullptr) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: no variant", path);
        ok = false;
    }
    if (ok)
        ok = parse(package, parse_state.style_path, &style_parser, &parse_state, error);
    /* Explorer's panes: classes and parts as in the style, over it (XP's
     * shellstyle.dll is what its task pane is drawn from). */
    if (ok && parse_state.shell_path != nullptr) {
        g_free(parse_state.cls);
        parse_state.cls = nullptr;
        ok = parse(package, parse_state.shell_path, &style_parser, &parse_state, error);
    }
    g_free(parse_state.style_path);
    g_free(parse_state.shell_path);
    g_free(parse_state.cls);
    if (!ok) {
        xd_pkg_free(package);
        return nullptr;
    }
    sysmetrics_overlay(package);
    return package;
}

void xd_pkg_free(xd_pkg *package) {
    if (package == nullptr)
        return;
    g_hash_table_unref(package->props);
    g_hash_table_unref(package->parts);
    g_hash_table_unref(package->sys);
    g_hash_table_unref(package->part_numbers);
    g_hash_table_unref(package->state_numbers);
    g_hash_table_unref(package->cache->images);
    g_hash_table_unref(package->cache->scaled);
    g_hash_table_unref(package->cache->faces);
    g_mutex_clear(&package->cache->lock);
    g_free(package->cache);
    if (package->zip != nullptr)
        g_hash_table_unref(package->zip);
    g_free(package->root);
    g_free(package->name);
    g_free(package->variant);
    g_free(package);
}

const char *xd_pkg_name(const xd_pkg *package) { return package->name; }
const char *xd_pkg_variant(const xd_pkg *package) { return package->variant; }
xd_visual_style xd_pkg_base(const xd_pkg *package) { return package->base; }

bool xd_pkg_filter(const xd_pkg *package, xd_image_filter *out) {
    if (package->has_filter)
        *out = package->filter;
    return package->has_filter;
}

/* ---- properties ------------------------------------------------------------------ */

static const char *lookup(const xd_pkg *package, const char *lc, const char *pk, const char *sk,
                          const char *lp) {
    char *key = g_strdup_printf("%s\x1f%s\x1f%s\x1f%s", lc, pk, sk, lp);
    const char *value = g_hash_table_lookup(package->props, key);
    g_free(key);
    return value;
}

/* Part and state, then part, then class, in one class. */
static const char *lookup_class(const xd_pkg *package, const char *cls, const char *part,
                                const char *state, const char *lp) {
    char *lc = lower(cls);
    char *pk = part_key(package, cls, part);
    char *sk = state_key(package, cls, pk, state);
    const char *value = lookup(package, lc, pk, sk, lp);
    if (value == nullptr && strcmp(sk, "0") != 0)
        value = lookup(package, lc, pk, "0", lp);
    if (value == nullptr && strcmp(pk, "0") != 0)
        value = lookup(package, lc, "0", "0", lp);
    g_free(sk);
    g_free(pk);
    g_free(lc);
    return value;
}

const char *xd_pkg_prop(const xd_pkg *package, const char *cls, const char *part, const char *state,
                        const char *prop) {
    if (package == nullptr)
        return nullptr;
    char *lp = lower(prop);
    const char *value = lookup_class(package, cls, part, state, lp);
    const char *colon = strstr(cls, "::");
    char *base = g_strdup(colon != nullptr ? colon + 2 : cls);
    if (value == nullptr && colon != nullptr)
        value = lookup_class(package, base, part, state, lp);
    if (value == nullptr) {
        char *style = g_strdup_printf("%sStyle", base);
        value = lookup_class(package, style, part, state, lp);
        g_free(style);
    }
    if (value == nullptr)
        value = lookup(package, "globals", "0", "0", lp);
    g_free(base);
    g_free(lp);
    return value;
}

static bool has_in_class(const xd_pkg *package, const char *cls, const char *part) {
    char *lc = lower(cls);
    char *pk = part_key(package, cls, part);
    char *key = g_strdup_printf("%s\x1f%s", lc, pk);
    bool has = g_hash_table_contains(package->parts, key);
    g_free(key);
    g_free(pk);
    g_free(lc);
    return has;
}

bool xd_pkg_has_own(const xd_pkg *package, const char *cls, const char *part) {
    return package != nullptr && has_in_class(package, cls, part);
}

/* In the classes xd_pkg_prop looks in: App::Class, Class, ClassStyle. */
bool xd_pkg_has(const xd_pkg *package, const char *cls, const char *part) {
    if (package == nullptr)
        return false;
    if (has_in_class(package, cls, part))
        return true;
    const char *colon = strstr(cls, "::");
    const char *base = colon != nullptr ? colon + 2 : cls;
    if (colon != nullptr && has_in_class(package, base, part))
        return true;
    char *style = g_strdup_printf("%sStyle", base);
    bool has = has_in_class(package, style, part);
    g_free(style);
    return has;
}

/* Whole numbers in a value ("8 8 9 9", "0 84 227", "8, 8, 9, 9"). */
static size_t numbers(const char *text, int *out, size_t max) {
    size_t count = 0;
    while (text != nullptr && *text != '\0' && count < max) {
        while (*text != '\0' && *text != '-' && !g_ascii_isdigit(*text))
            text++;
        if (*text == '\0')
            break;
        char *end;
        long x = strtol(text, &end, 10);
        if (end == text)
            break;
        out[count++] = (int)CLAMP(x, -100000, 100000);
        text = end;
    }
    return count;
}

bool xd_pkg_int(const xd_pkg *package, const char *cls, const char *part, const char *state,
                const char *prop, int *out) {
    int values[1];
    if (numbers(xd_pkg_prop(package, cls, part, state, prop), values, 1) != 1)
        return false;
    *out = values[0];
    return true;
}

bool xd_pkg_bool(const xd_pkg *package, const char *cls, const char *part, const char *state,
                 const char *prop, bool *out) {
    const char *value = xd_pkg_prop(package, cls, part, state, prop);
    if (value == nullptr)
        return false;
    *out = g_ascii_strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0;
    return true;
}

static bool parse_color(const char *text, xd_rgba *out) {
    if (text == nullptr)
        return false;
    if (text[0] == '#' && strlen(text) == 7) {
        unsigned long x = strtoul(text + 1, nullptr, 16);
        *out = xd_rgba_hex((uint32_t)x & 0xFFFFFFu);
        return true;
    }
    int values[3];
    if (numbers(text, values, 3) != 3)
        return false;
    *out =
        xd_rgba_hex((uint32_t)CLAMP(values[0], 0, 255) << 16 |
                    (uint32_t)CLAMP(values[1], 0, 255) << 8 | (uint32_t)CLAMP(values[2], 0, 255));
    return true;
}

bool xd_pkg_color(const xd_pkg *package, const char *cls, const char *part, const char *state,
                  const char *prop, xd_rgba *out) {
    return parse_color(xd_pkg_prop(package, cls, part, state, prop), out);
}

bool xd_pkg_margins(const xd_pkg *package, const char *cls, const char *part, const char *state,
                    const char *prop, xd_insets *out) {
    int values[4];
    if (numbers(xd_pkg_prop(package, cls, part, state, prop), values, 4) != 4)
        return false;
    *out =
        (xd_insets){.left = values[0], .right = values[1], .top = values[2], .bottom = values[3]};
    return true;
}

/* A FONT value as msstyles write them: "Tahoma, 8, Bold, Underline,
 * Quality:ClearType" (face, points, then flags in any order). */
bool xd_pkg_font(const xd_pkg *package, const char *cls, const char *part, const char *state,
                 const char *prop, xd_font_spec *out, bool *underline) {
    const char *value = xd_pkg_prop(package, cls, part, state, prop);
    if (value == nullptr)
        return false;
    char **fields = g_strsplit(value, ",", -1);
    bool ok = fields[0] != nullptr && fields[1] != nullptr;
    if (ok) {
        /* Within what a scheme allows: "inf" or 1e308 points would draw
         * nothing sensible, and overflow converting to pixels. */
        double size = g_ascii_strtod(g_strstrip(fields[1]), nullptr);
        xd_font_spec font = {.size_pt = isfinite(size) ? CLAMP(size, 1.0, 96.0) : 0};
        font_family(g_strstrip(fields[0]), font.family, sizeof(font.family));
        bool under = false;
        for (size_t i = 2; fields[i] != nullptr; i++) {
            const char *flag = g_strstrip(fields[i]);
            if (g_ascii_strcasecmp(flag, "bold") == 0)
                font.bold = true;
            else if (g_ascii_strcasecmp(flag, "italic") == 0)
                font.italic = true;
            else if (g_ascii_strcasecmp(flag, "underline") == 0)
                under = true;
        }
        ok = font.size_pt > 0;
        if (ok) {
            *out = font;
            if (underline != nullptr)
                *underline = under;
        }
    }
    g_strfreev(fields);
    return ok;
}

static const char *sys(const xd_pkg *package, const char *kind, const char *name) {
    if (package == nullptr)
        return nullptr;
    char *ln = lower(name);
    char *key = g_strdup_printf("%s:%s", kind, ln);
    const char *value = g_hash_table_lookup(package->sys, key);
    g_free(key);
    g_free(ln);
    return value;
}

bool xd_pkg_sys_color(const xd_pkg *package, const char *name, xd_rgba *out) {
    return parse_color(sys(package, "color", name), out);
}

bool xd_pkg_sys_int(const xd_pkg *package, const char *name, int *out) {
    int values[1];
    if (numbers(sys(package, "value", name), values, 1) != 1)
        return false;
    *out = values[0];
    return true;
}

bool xd_pkg_sys_bool(const xd_pkg *package, const char *name, bool *out) {
    const char *value = sys(package, "value", name);
    if (value == nullptr)
        return false;
    *out = g_ascii_strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0;
    return true;
}

bool xd_pkg_sys_font(const xd_pkg *package, const char *name, const char **face, int *points,
                     bool *bold, bool *italic) {
    const char *value = sys(package, "font", name);
    if (value == nullptr)
        return false;
    /* "face\x1fsize\x1fbold\x1fitalic", the face ending at the first
     * separator. */
    static_assert(sizeof(char) == 1);
    const char *s1 = strchr(value, '\x1f');
    if (s1 == nullptr)
        return false;
    int values[3];
    if (numbers(s1, values, 3) != 3)
        return false;
    /* The caller gets the face up to the separator: copy it into the
     * package's string table so it outlives this call. */
    char *face_copy = g_strndup(value, (gsize)(s1 - value));
    g_mutex_lock(&package->cache->lock);
    const char *kept = g_hash_table_lookup(package->cache->faces, face_copy);
    if (kept == nullptr) {
        g_hash_table_insert(package->cache->faces, face_copy, face_copy);
        kept = face_copy;
    } else {
        g_free(face_copy);
    }
    g_mutex_unlock(&package->cache->lock);
    *face = kept;
    *points = values[0];
    *bold = values[1] != 0;
    *italic = values[2] != 0;
    return true;
}

/* ---- images ---------------------------------------------------------------------- */

typedef struct png_reader {
    const uint8_t *data;
    gsize len, pos;
} png_reader;

static cairo_status_t png_read(void *closure, unsigned char *buf, unsigned int length) {
    png_reader *reader = closure;
    if (reader->len - reader->pos < length)
        return CAIRO_STATUS_READ_ERROR;
    memcpy(buf, reader->data + reader->pos, length);
    reader->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

/* An ARGB32 surface (what the scalers take), whatever the PNG was. */
static cairo_surface_t *argb32(cairo_surface_t *surface) {
    if (cairo_image_surface_get_format(surface) == CAIRO_FORMAT_ARGB32)
        return surface;
    int width = cairo_image_surface_get_width(surface),
        height = cairo_image_surface_get_height(surface);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(out);
    cairo_set_source_surface(cairo, surface, 0, 0);
    cairo_paint(cairo);
    cairo_destroy(cairo);
    cairo_surface_destroy(surface);
    return out;
}

/* A PNG's size from its header (the IHDR chunk, first after the
 * signature), before decoding it: a few kilobytes of compressed pixels
 * can say gigabytes. */
static bool png_size(const unsigned char *data, gsize len, uint32_t *width, uint32_t *height) {
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (len < 24 || memcmp(data, signature, sizeof(signature)) != 0 ||
        memcmp(data + 12, "IHDR", 4) != 0)
        return false;
    *width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 |
             (uint32_t)data[19];
    *height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 |
              (uint32_t)data[23];
    return true;
}

/* A package's images are at most this many pixels (Windows' largest, the
 * Start menu's and the taskbar's at 192 dpi, are well under a million). */
static constexpr uint64_t IMAGE_MAX_PIXELS = 4 * 1024 * 1024;

/* The package's image at `path` as loaded (cached; the package owns it). */
static cairo_surface_t *image(const xd_pkg *package, const char *path) {
    cairo_surface_t *surface = g_hash_table_lookup(package->cache->images, path);
    if (surface != nullptr)
        return surface;
    GBytes *bytes = read_file(package, path);
    if (bytes == nullptr)
        return nullptr;
    gsize len = 0;
    png_reader reader = {.data = g_bytes_get_data(bytes, &len)};
    reader.len = len;
    uint32_t width = 0, height = 0;
    if (!png_size(reader.data, len, &width, &height) ||
        (uint64_t)width * height > IMAGE_MAX_PIXELS) {
        g_bytes_unref(bytes);
        return nullptr;
    }
    surface = cairo_image_surface_create_from_png_stream(png_read, &reader);
    g_bytes_unref(bytes);
    if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surface);
        return nullptr;
    }
    surface = argb32(surface);
    g_hash_table_insert(package->cache->images, g_strdup(path), surface);
    return surface;
}

/* The filter the theme's image settings give ("Automatic" is resolved to
 * the package's own by whoever chose the package: a Smooth chosen by the
 * user stays Smooth). */
static xd_image_filter filter_for(const xd_theme *theme, double factor) {
    xd_image_filter image_filter = theme->image_filter;
    /* Whole-number scaling: smooth scaling at 200% would blur what can be
     * exact. */
    if (image_filter == XD_FILTER_SMOOTH && theme->integer_scaling &&
        fabs(factor - round(factor)) < 1e-6 && factor >= 1.0)
        image_filter = XD_FILTER_INTEGER;
    return image_filter;
}

/* One part's image strip as drawn: an ImageFile (or an atlas's rectangle)
 * scaled by `factor`, `count` frames. Cached; owned by the package. */
static cairo_surface_t *strip(const xd_theme *theme, const char *path, const int *atlas, int count,
                              bool vertical, double factor) {
    const xd_pkg *package = theme->pkg;
    xd_image_filter image_filter = filter_for(theme, factor);
    char *key = atlas != nullptr ? g_strdup_printf("%s#%d,%d,%d,%d@%d%c%.4f/%d", path, atlas[0],
                                                   atlas[1], atlas[2], atlas[3], count,
                                                   vertical ? 'v' : 'h', factor, (int)image_filter)
                                 : g_strdup_printf("%s@%d%c%.4f/%d", path, count,
                                                   vertical ? 'v' : 'h', factor, (int)image_filter);
    g_mutex_lock(&package->cache->lock);
    cairo_surface_t *surface = g_hash_table_lookup(package->cache->scaled, key);
    if (surface == nullptr) {
        cairo_surface_t *src = image(package, path);
        cairo_surface_t *cut = nullptr;
        if (src != nullptr && atlas != nullptr) {
            int width = atlas[2] - atlas[0], height = atlas[3] - atlas[1];
            if (width > 0 && height > 0 && atlas[0] >= 0 && atlas[1] >= 0 &&
                atlas[2] <= cairo_image_surface_get_width(src) &&
                atlas[3] <= cairo_image_surface_get_height(src)) {
                cut = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
                cairo_t *cairo = cairo_create(cut);
                cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
                cairo_set_source_surface(cairo, src, -atlas[0], -atlas[1]);
                cairo_paint(cairo);
                cairo_destroy(cairo);
            }
            src = cut;
        }
        if (src != nullptr) {
            surface = fabs(factor - 1.0) < 1e-6 && cut == nullptr
                          ? cairo_surface_reference(src)
                          : xd_image_scale_frames(src, count, vertical, factor, image_filter);
            if (surface != nullptr) { /* nullptr: too big, or frames that don't divide */
                g_hash_table_insert(package->cache->scaled, key, surface);
                key = nullptr;
            }
        }
        if (cut != nullptr)
            cairo_surface_destroy(cut);
    }
    g_mutex_unlock(&package->cache->lock);
    g_free(key);
    return surface;
}

typedef struct part_image {
    cairo_surface_t *strip;
    int count;
    bool vertical;
    int frame; /* the state's */
    int fw, fh; /* one frame, device px */
    double factor;
} part_image;

static bool enum_is(const xd_pkg *package, const char *cls, const char *part, const char *state,
                    const char *prop, const char *value) {
    const char *prop_value = xd_pkg_prop(package, cls, part, state, prop);
    return prop_value != nullptr && g_ascii_strcasecmp(prop_value, value) == 0;
}

/* The state's number (its frame is number - 1). */
static int state_number(const xd_pkg *package, const char *cls, const char *part,
                        const char *state) {
    char *pk = part_key(package, cls, part);
    char *sk = state_key(package, cls, pk, state);
    int number = all_digits(sk) ? atoi(sk) : 0;
    g_free(sk);
    g_free(pk);
    return number;
}

/* Finds the part's image for the state at the theme's DPI: ImageFile, or
 * with ImageSelectType the ImageFileN for the DPI (by MinDpiN) or size (by
 * MinSizeN), scaled from the DPI it was drawn for, as uxtheme does.
 * `glyph`: the GlyphImageFile instead. */
static bool part_image_for(const xd_theme *theme, const char *cls, const char *part,
                           const char *state, xd_rect rect, bool glyph, part_image *out) {
    const xd_pkg *package = theme->pkg;
    const char *file =
        xd_pkg_prop(package, cls, part, state, glyph ? "GlyphImageFile" : "ImageFile");
    double target_dpi = 96.0 * theme->scale;
    double drawn_dpi = 96.0;
    int count = 1;
    (void)xd_pkg_int(package, cls, part, state, "ImageCount", &count);
    count = CLAMP(count, 1, 64);
    /* An image glyph without a GlyphImageFile has its sizes in
     * ImageFile1..N (XP's caption buttons): the largest that fits, as it
     * is. They're never the background. */
    bool glyph_sizes = enum_is(package, cls, part, state, "GlyphType", "ImageGlyph") &&
                       xd_pkg_prop(package, cls, part, state, "GlyphImageFile") == nullptr;
    if (glyph && glyph_sizes) {
        int best = -1;
        for (int i = 1; i <= 7; i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "ImageFile%d", i);
            const char *image_file = xd_pkg_prop(package, cls, part, state, prop);
            cairo_surface_t *img = image_file != nullptr ? image(package, image_file) : nullptr;
            if (img == nullptr)
                continue;
            int fw = cairo_image_surface_get_width(img);
            int fh = cairo_image_surface_get_height(img) / count;
            if (fw <= rect.w && fh <= rect.h && fw > best) {
                best = fw;
                file = image_file;
                drawn_dpi = target_dpi;
            } else if (file == nullptr) {
                file = image_file; /* nothing fits: the smallest, shrunk */
            }
        }
    }
    if (!glyph && !glyph_sizes && !enum_is(package, cls, part, state, "ImageSelectType", "None")) {
        bool by_dpi = enum_is(package, cls, part, state, "ImageSelectType", "Dpi");
        bool by_size = enum_is(package, cls, part, state, "ImageSelectType", "Size");
        for (int i = 1; i <= 7 && (by_dpi || by_size); i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "ImageFile%d", i);
            const char *image_file = xd_pkg_prop(package, cls, part, state, prop);
            if (image_file == nullptr)
                break;
            if (by_dpi) {
                int dpi = 96;
                g_snprintf(prop, sizeof(prop), "MinDpi%d", i);
                (void)xd_pkg_int(package, cls, part, state, prop, &dpi);
                /* Windows' own go from 96 to 192 dpi; a package's own
                 * numbers can't make an image hundreds of times larger. */
                dpi = CLAMP(dpi, 48, 480);
                if (dpi <= target_dpi + 0.5 || file == nullptr) {
                    file = image_file;
                    drawn_dpi = dpi;
                }
            } else {
                int min_size[2] = {0, 0};
                g_snprintf(prop, sizeof(prop), "MinSize%d", i);
                if (numbers(xd_pkg_prop(package, cls, part, state, prop), min_size, 2) >= 1 &&
                    min_size[0] <= rect.w && (min_size[1] == 0 || min_size[1] <= rect.h)) {
                    file = image_file;
                    drawn_dpi = target_dpi; /* chosen to fit: not scaled */
                } else if (file == nullptr) {
                    file = image_file;
                }
            }
        }
    }
    const char *atlas_file = nullptr;
    int atlas[4];
    bool has_atlas = numbers(xd_pkg_prop(package, cls, part, state, "AtlasRect"), atlas, 4) == 4;
    if (has_atlas && !glyph) {
        atlas_file = xd_pkg_prop(package, cls, part, state, "AtlasImage");
        if (atlas_file == nullptr)
            atlas_file = xd_pkg_prop(package, cls, nullptr, nullptr, "DiskStream");
        if (atlas_file != nullptr)
            file = atlas_file;
        else
            has_atlas = false;
    }
    if (file == nullptr)
        return false;
    /* Without an ImageLayout, uxtheme lays frames out side by side (XP's
     * size grips: two 16 px frames in a 32 x 17 image); DWM's atlas
     * rectangles stack them. */
    const char *layout = xd_pkg_prop(package, cls, part, state, "ImageLayout");
    bool vertical = layout != nullptr ? g_ascii_strcasecmp(layout, "Horizontal") != 0 : has_atlas;
    double factor = target_dpi / drawn_dpi;
    /* Within a few percent of drawn: as drawn (no blur for a 25 px check
     * box drawn at 185 DPI shown at 192). */
    if (fabs(factor - 1.0) < 0.15)
        factor = 1.0;
    cairo_surface_t *surface =
        strip(theme, file, has_atlas ? atlas : nullptr, count, vertical, factor);
    if (surface == nullptr)
        return false;
    int sw = cairo_image_surface_get_width(surface), sh = cairo_image_surface_get_height(surface);
    out->strip = surface;
    out->count = count;
    out->vertical = vertical;
    out->fw = vertical ? sw : sw / count;
    out->fh = vertical ? sh / count : sh;
    out->factor = factor;
    int number = state_number(package, cls, part, state);
    out->frame = CLAMP(number > 0 ? number - 1 : 0, 0, count - 1);
    return out->fw > 0 && out->fh > 0;
}

/* Paints the `src` rectangle of a frame into `dst`, stretched or tiled,
 * reading nothing outside `src` (no bleeding between slices). */
static void blit(cairo_t *cairo, const part_image *im, xd_rect src, xd_rect dst, bool tile) {
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
        return;
    int fx = im->vertical ? 0 : im->frame * im->fw;
    int fy = im->vertical ? im->frame * im->fh : 0;
    cairo_surface_t *sub = cairo_surface_create_for_rectangle(im->strip, fx + src.x, fy + src.y,
                                                              src.w, src.h);
    cairo_save(cairo);
    cairo_rectangle(cairo, dst.x, dst.y, dst.w, dst.h);
    cairo_clip(cairo);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(sub);
    cairo_matrix_t matrix;
    if (tile) {
        cairo_matrix_init_translate(&matrix, -dst.x, -dst.y);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    } else {
        cairo_matrix_init_scale(&matrix, (double)src.w / dst.w, (double)src.h / dst.h);
        cairo_matrix_translate(&matrix, -dst.x, -dst.y);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
        /* Slices are stretched along one axis, where they don't change:
         * nearest keeps their edges as drawn. */
        cairo_pattern_set_filter(pat, (src.w == dst.w || src.h == dst.h)
                                          ? CAIRO_FILTER_NEAREST
                                          : CAIRO_FILTER_BILINEAR);
    }
    cairo_pattern_set_matrix(pat, &matrix);
    cairo_set_source(cairo, pat);
    cairo_paint(cairo);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(sub);
    cairo_restore(cairo);
}

static int scaled(double value, double factor) { return (int)lround(value * factor); }

/* uxtheme's 9-grid: corners as they are, edges and middle stretched or
 * tiled; margins larger than the target shrink to fit. */
static void nine_grid(cairo_t *cairo, const part_image *im, xd_insets insets, xd_rect rect,
                      bool tile, bool border_only) {
    int left = CLAMP(scaled(insets.left, im->factor), 0, im->fw),
        rr = CLAMP(scaled(insets.right, im->factor), 0, im->fw - left);
    int top = CLAMP(scaled(insets.top, im->factor), 0, im->fh),
        bottom = CLAMP(scaled(insets.bottom, im->factor), 0, im->fh - top);
    int dl = left, dr = rr, dt = top, db = bottom;
    if (dl + dr > rect.w && dl + dr > 0) {
        dl = rect.w * dl / (dl + dr);
        dr = rect.w - dl;
    }
    if (dt + db > rect.h && dt + db > 0) {
        dt = rect.h * dt / (dt + db);
        db = rect.h - dt;
    }
    int sx[3] = {0, left, im->fw - rr}, sw[3] = {left, im->fw - left - rr, rr};
    int sy[3] = {0, top, im->fh - bottom}, sh[3] = {top, im->fh - top - bottom, bottom};
    int dx[3] = {rect.x, rect.x + dl, rect.x + rect.w - dr}, dw[3] = {dl, rect.w - dl - dr, dr};
    int dy[3] = {rect.y, rect.y + dt, rect.y + rect.h - db}, dh[3] = {dt, rect.h - dt - db, db};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (border_only && i == 1 && j == 1)
                continue;
            bool corner = i != 1 && j != 1;
            blit(cairo, im, (xd_rect){sx[i], sy[j], sw[i], sh[j]},
                 (xd_rect){dx[i], dy[j], dw[i], dh[j]}, tile && !corner);
        }
}

/* TrueSize: the frame as it is, aligned in `rect` (centred by default);
 * shrunk uniformly if it doesn't fit, stretched past TrueSizeStretchMark. */
static void true_size(const xd_theme *theme, cairo_t *cairo, const part_image *im, const char *cls,
                      const char *part, const char *state, xd_rect rect) {
    int width = im->fw, height = im->fh;
    int mark = 0;
    if (xd_pkg_int(theme->pkg, cls, part, state, "TrueSizeStretchMark", &mark) && mark > 0 &&
        ((int64_t)rect.w > (int64_t)width * (100 + mark) / 100 ||
         (int64_t)rect.h > (int64_t)height * (100 + mark) / 100)) {
        double k = fmin((double)rect.w / width, (double)rect.h / height);
        width = (int)(width * k);
        height = (int)(height * k);
    }
    if (width > rect.w || height > rect.h) {
        double k = fmin((double)rect.w / width, (double)rect.h / height);
        width = MAX(1, (int)(width * k));
        height = MAX(1, (int)(height * k));
    }
    int x = rect.x + (rect.w - width) / 2, y = rect.y + (rect.h - height) / 2;
    if (enum_is(theme->pkg, cls, part, state, "HAlign", "Left"))
        x = rect.x;
    else if (enum_is(theme->pkg, cls, part, state, "HAlign", "Right"))
        x = rect.x + rect.w - width;
    if (enum_is(theme->pkg, cls, part, state, "VAlign", "Top"))
        y = rect.y;
    else if (enum_is(theme->pkg, cls, part, state, "VAlign", "Bottom"))
        y = rect.y + rect.h - height;
    blit(cairo, im, (xd_rect){0, 0, im->fw, im->fh}, (xd_rect){x, y, width, height}, false);
}

static void border_fill(const xd_theme *theme, cairo_t *cairo, const char *cls, const char *part,
                        const char *state, xd_rect rect) {
    const xd_pkg *package = theme->pkg;
    int border = 1;
    (void)xd_pkg_int(package, cls, part, state, "BorderSize", &border);
    border = scaled(MAX(border, 0), theme->scale);
    xd_rgba bc, fc, g1, g2;
    bool has_fill = xd_pkg_color(package, cls, part, state, "FillColor", &fc);
    if (!enum_is(package, cls, part, state, "FillType", "Solid") &&
        xd_pkg_color(package, cls, part, state, "GradientColor1", &g1) &&
        xd_pkg_color(package, cls, part, state, "GradientColor2", &g2)) {
        bool horz = enum_is(package, cls, part, state, "FillType", "HorzGradient");
        cairo_pattern_t *pat = horz ? cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0)
                                    : cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
        cairo_pattern_add_color_stop_rgba(pat, 0, g1.r, g1.g, g1.b, g1.a);
        cairo_pattern_add_color_stop_rgba(pat, 1, g2.r, g2.g, g2.b, g2.a);
        cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
        cairo_set_source(cairo, pat);
        cairo_fill(cairo);
        cairo_pattern_destroy(pat);
    } else if (has_fill) {
        xd_fill_rect(cairo, rect, fc);
    }
    if (border > 0 && xd_pkg_color(package, cls, part, state, "BorderColor", &bc)) {
        xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, rect.w, border}, bc);
        xd_fill_rect(cairo, (xd_rect){rect.x, rect.y + rect.h - border, rect.w, border}, bc);
        xd_fill_rect(cairo, (xd_rect){rect.x, rect.y, border, rect.h}, bc);
        xd_fill_rect(cairo, (xd_rect){rect.x + rect.w - border, rect.y, border, rect.h}, bc);
    }
}

bool xd_pkg_draw_background(const xd_theme *theme, cairo_t *cairo, const char *cls,
                            const char *part, const char *state, xd_rect rect) {
    const xd_pkg *package = theme->pkg;
    if (package == nullptr || rect.w <= 0 || rect.h <= 0 || !xd_pkg_has(package, cls, part))
        return false;
    bool glyph_only = false;
    (void)xd_pkg_bool(package, cls, part, state, "GlyphOnly", &glyph_only);
    if (glyph_only || enum_is(package, cls, part, state, "BgType", "None"))
        return false;
    if (enum_is(package, cls, part, state, "BgType", "BorderFill")) {
        border_fill(theme, cairo, cls, part, state, rect);
        return true;
    }
    part_image im;
    if (!part_image_for(theme, cls, part, state, rect, false, &im))
        return false;
    if (enum_is(package, cls, part, state, "SizingType", "TrueSize")) {
        true_size(theme, cairo, &im, cls, part, state, rect);
    } else {
        xd_insets insets = {0};
        (void)xd_pkg_margins(package, cls, part, state, "SizingMargins", &insets);
        bool border_only = false;
        (void)xd_pkg_bool(package, cls, part, state, "BorderOnly", &border_only);
        nine_grid(cairo, &im, insets, rect,
                  enum_is(package, cls, part, state, "SizingType", "Tile"), border_only);
    }
    return true;
}

/* An image glyph: centred in the content area, as it is. */
bool xd_pkg_draw_glyph(const xd_theme *theme, cairo_t *cairo, const char *cls, const char *part,
                       const char *state, xd_rect rect) {
    const xd_pkg *package = theme->pkg;
    if (package == nullptr || rect.w <= 0 || rect.h <= 0 || !xd_pkg_has(package, cls, part) ||
        !enum_is(package, cls, part, state, "GlyphType", "ImageGlyph"))
        return false;
    part_image glyph;
    xd_rect content_rect = xd_pkg_content_rect(theme, cls, part, state, rect);
    if (!part_image_for(theme, cls, part, state, content_rect, true, &glyph))
        return false;
    true_size(theme, cairo, &glyph, cls, part, state, content_rect);
    return true;
}

bool xd_pkg_draw(const xd_theme *theme, cairo_t *cairo, const char *cls, const char *part,
                 const char *state, xd_rect rect) {
    bool bg = xd_pkg_draw_background(theme, cairo, cls, part, state, rect);
    bool glyph = xd_pkg_draw_glyph(theme, cairo, cls, part, state, rect);
    return bg || glyph;
}

bool xd_pkg_draw_slice(const xd_theme *theme, cairo_t *cairo, const char *cls, const char *part,
                       const char *state, xd_rect src, xd_rect dst) {
    if (theme->pkg == nullptr || dst.w <= 0 || dst.h <= 0 || !xd_pkg_has(theme->pkg, cls, part))
        return false;
    part_image im;
    if (!part_image_for(theme, cls, part, state, dst, false, &im))
        return false;
    /* The frame's own pixels, scaled as the image was. */
    int x0 = CLAMP(scaled(src.x, im.factor), 0, im.fw);
    int y0 = CLAMP(scaled(src.y, im.factor), 0, im.fh);
    int x1 = CLAMP(scaled(src.x + src.w, im.factor), x0, im.fw);
    int y1 = CLAMP(scaled(src.y + src.h, im.factor), y0, im.fh);
    if (x1 <= x0 || y1 <= y0)
        return false;
    blit(cairo, &im, (xd_rect){x0, y0, x1 - x0, y1 - y0}, dst, false);
    return true;
}

bool xd_pkg_glyph_size(const xd_theme *theme, const char *cls, const char *part, const char *state,
                       int *width, int *height) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, cls, part) ||
        !enum_is(theme->pkg, cls, part, state, "GlyphType", "ImageGlyph"))
        return false;
    part_image glyph;
    if (!part_image_for(theme, cls, part, state, (xd_rect){0, 0, 1 << 16, 1 << 16}, true, &glyph))
        return false;
    *width = glyph.fw;
    *height = glyph.fh;
    return true;
}

bool xd_pkg_part_size(const xd_theme *theme, const char *cls, const char *part, const char *state,
                      int *width, int *height) {
    if (theme->pkg == nullptr || !xd_pkg_has(theme->pkg, cls, part))
        return false;
    part_image im;
    if (!part_image_for(theme, cls, part, state, (xd_rect){0, 0, 1 << 16, 1 << 16}, false, &im))
        return false;
    *width = im.fw;
    *height = im.fh;
    return true;
}

xd_rect xd_pkg_content_rect(const xd_theme *theme, const char *cls, const char *part,
                            const char *state, xd_rect rect) {
    xd_insets insets;
    if (theme->pkg == nullptr ||
        !xd_pkg_margins(theme->pkg, cls, part, state, "ContentMargins", &insets))
        return rect;
    int left = scaled(insets.left, theme->scale), rr = scaled(insets.right, theme->scale);
    int top = scaled(insets.top, theme->scale), bottom = scaled(insets.bottom, theme->scale);
    xd_rect content_rect = {rect.x + left, rect.y + top, rect.w - left - rr, rect.h - top - bottom};
    if (content_rect.w < 0 || content_rect.h < 0)
        return rect;
    return content_rect;
}
