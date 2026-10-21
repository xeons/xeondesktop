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
    xd_image_filter f;
    const char *fname = getenv("XD_IMAGE_FILTER");
    if (fname == nullptr || !xd_image_filter_from_name(fname, &f)) {
        if (!xd_pkg_filter(env_pkg, &f))
            f = XD_FILTER_SMOOTH;
    }
    xd_theme_use_package(env_pkg, f, true);
}

static void set_package(xd_theme *t, const xd_pkg *p, xd_image_filter filter, bool integer) {
    t->pkg = p;
    t->image_filter = filter;
    t->integer_scaling = integer;
    if (p == nullptr)
        return;
    /* What the package doesn't draw, its base style does, with its
     * [SysMetrics] over the scheme. */
    t->style = p->base;
    for (int i = 0; i < XD_COLOR_COUNT; i++)
        if (p->color_mask & (1ull << i))
            t->scheme.colors[i] = p->colors[i];
    for (int i = 0; i < XD_METRIC_COUNT; i++)
        if (p->metric_mask & (1u << i))
            t->scheme.metrics[i] = p->metrics[i];
    for (int i = 0; i < XD_FONT_COUNT; i++)
        if (p->font_mask & (1u << i))
            t->scheme.fonts[i] = p->fonts[i];
}

void xd_theme_attach_package(xd_theme *t) {
    package_from_environment();
    set_package(t, active_pkg, active_filter, active_integer);
}

void xd_theme_init_package(xd_theme *t, xd_visual_style style, const xd_scheme *scheme,
                           double scale, const xd_pkg *pkg, xd_image_filter filter,
                           bool integer_scaling) {
    xd_theme_init(t, style, scheme, scale);
    /* Without the active package's base style and [SysMetrics]. */
    t->style = style;
    t->scheme = *scheme;
    set_package(t, pkg, filter, integer_scaling);
}

static size_t numbers(const char *v, int *out, size_t max);
static bool parse_color(const char *v, xd_rgba *out);

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
    {"menufont", XD_FONT_MENU},       {"msgboxfont", XD_FONT_MESSAGE},
    {"statusfont", XD_FONT_TOOLTIP},  {"icontitlefont", XD_FONT_ICON},
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

static void sysmetrics_overlay(xd_pkg *p) {
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, p->sys);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *key = k;
        if (g_str_has_prefix(key, "color:")) {
            const char *name = key + 6;
            for (size_t i = 0; i < G_N_ELEMENTS(color_names); i++)
                if (strcmp(name, color_names[i][0]) == 0)
                    name = color_names[i][1];
            xd_syscolor c;
            xd_rgba rgba;
            if (xd_syscolor_from_name(xd_str_from_cstr(name), &c) && parse_color(v, &rgba)) {
                p->colors[c] = (xd_rgb){(uint8_t)lround(rgba.r * 255), (uint8_t)lround(rgba.g * 255),
                                        (uint8_t)lround(rgba.b * 255)};
                p->color_mask |= 1ull << c;
            }
        } else if (g_str_has_prefix(key, "value:")) {
            int n[1];
            for (size_t i = 0; i < G_N_ELEMENTS(metric_names); i++)
                if (strcmp(key + 6, metric_names[i].name) == 0 && numbers(v, n, 1) == 1) {
                    p->metrics[metric_names[i].metric] = n[0];
                    p->metric_mask |= 1u << metric_names[i].metric;
                }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(font_names); i++) {
        const char *face;
        int points;
        bool bold, italic;
        if (!xd_pkg_sys_font(p, font_names[i].name, &face, &points, &bold, &italic) ||
            points <= 0)
            continue;
        xd_font_spec *f = &p->fonts[font_names[i].role];
        font_family(face, f->family, sizeof(f->family));
        f->size_pt = points;
        f->bold = bold;
        f->italic = italic;
        p->font_mask |= 1u << font_names[i].role;
    }
}

/* ---- names ----------------------------------------------------------------------- */

static char *lower(const char *s) { return g_ascii_strdown(s, -1); }

/* "TaskBand::Toolbar" -> "toolbar"; the class uxtheme's names are for. */
static char *base_class(const char *cls) {
    const char *colon = strstr(cls, "::");
    return lower(colon != nullptr ? colon + 2 : cls);
}

static bool all_digits(const char *s) {
    if (s == nullptr || *s == '\0')
        return false;
    for (; *s != '\0'; s++)
        if (!g_ascii_isdigit(*s))
            return false;
    return true;
}

/* A part as stored: its number where uxtheme.ini knows it, else its name
 * (lowercase); "0" for none. */
static char *part_key(const xd_pkg *p, const char *cls, const char *part) {
    if (part == nullptr || *part == '\0')
        return g_strdup("0");
    if (all_digits(part))
        return g_strdup(part);
    char *base = base_class(cls);
    char *name = lower(part);
    char *key = g_strdup_printf("%s.%s", base, name);
    gpointer n = g_hash_table_lookup(p->part_numbers, key);
    if (n == nullptr) {
        /* Vista's ButtonStyle has Button's parts. */
        char *style_key = g_strdup_printf("%sstyle.%s", base, name);
        n = g_hash_table_lookup(p->part_numbers, style_key);
        g_free(style_key);
    }
    char *out = n != nullptr ? g_strdup_printf("%d", GPOINTER_TO_INT(n)) : g_strdup(name);
    g_free(key);
    g_free(name);
    g_free(base);
    return out;
}

/* A part's name, from its stored key (a number or a name). */
static char *part_name(const xd_pkg *p, const char *cls, const char *pkey) {
    if (!all_digits(pkey))
        return g_strdup(pkey);
    char *base = base_class(cls);
    char *prefix = g_strdup_printf("%s.", base);
    char *found = nullptr;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, p->part_numbers);
    while (found == nullptr && g_hash_table_iter_next(&it, &k, &v)) {
        const char *key = k;
        if (g_str_has_prefix(key, prefix) && strchr(key + strlen(prefix), '.') == nullptr &&
            GPOINTER_TO_INT(v) == atoi(pkey))
            found = g_strdup(key + strlen(prefix));
    }
    g_free(prefix);
    g_free(base);
    return found;
}

static char *state_key(const xd_pkg *p, const char *cls, const char *pkey, const char *state) {
    if (state == nullptr || *state == '\0')
        return g_strdup("0");
    if (all_digits(state))
        return g_strdup(state);
    char *name = lower(state);
    char *base = base_class(cls);
    char *pname = part_name(p, cls, pkey);
    gpointer n = nullptr;
    if (pname != nullptr) {
        char *key = g_strdup_printf("%s.%s.%s", base, pname, name);
        n = g_hash_table_lookup(p->state_numbers, key);
        g_free(key);
        if (n == nullptr) {
            /* Vista's HeaderStyle has more of Header's states. */
            key = g_strdup_printf("%sstyle.%s.%s", base, pname, name);
            n = g_hash_table_lookup(p->state_numbers, key);
            g_free(key);
        }
    }
    if (n == nullptr) {
        /* A state on the class itself ("Toolbar(Hot)"), or on a part whose
         * states aren't listed: any of the class's parts that has it. */
        char *prefix = g_strdup_printf("%s.", base);
        char *suffix = g_strdup_printf(".%s", name);
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, p->state_numbers);
        while (n == nullptr && g_hash_table_iter_next(&it, &k, &v))
            if (g_str_has_prefix(k, prefix) && g_str_has_suffix(k, suffix))
                n = v;
        g_free(suffix);
        g_free(prefix);
    }
    char *out = n != nullptr ? g_strdup_printf("%d", GPOINTER_TO_INT(n)) : g_strdup(name);
    g_free(pname);
    g_free(base);
    g_free(name);
    return out;
}

static bool load_names(xd_pkg *p, const char *path, GError **error) {
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
            int n = g_key_file_get_integer(kf, groups[i], keys[k], nullptr);
            char *name = lower(keys[k]);
            g_hash_table_insert(parts ? p->part_numbers : p->state_numbers,
                                g_strdup_printf("%s.%s", what, name), GINT_TO_POINTER(n));
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

static GBytes *read_file(const xd_pkg *p, const char *path) {
    /* Package paths are relative, without "..": nothing outside it. */
    if (path == nullptr || path[0] == '/' || strstr(path, "..") != nullptr)
        return nullptr;
    if (p->zip != nullptr) {
        GBytes *b = g_hash_table_lookup(p->zip, path);
        return b != nullptr ? g_bytes_ref(b) : nullptr;
    }
    char *full = g_build_filename(p->root, path, nullptr);
    char *data = nullptr;
    gsize len = 0;
    bool ok = g_file_get_contents(full, &data, &len, nullptr);
    g_free(full);
    return ok ? g_bytes_new_take(data, len) : nullptr;
}

#if XD_HAVE_LIBARCHIVE
static bool read_zip(xd_pkg *p, const char *path, GError **error) {
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    bool ok = archive_read_open_filename(a, path, 64 * 1024) == ARCHIVE_OK;
    gsize total = 0;
    struct archive_entry *e;
    while (ok && archive_read_next_header(a, &e) == ARCHIVE_OK) {
        la_int64_t size = archive_entry_size(e);
        const char *name = archive_entry_pathname(e);
        if (archive_entry_filetype(e) != AE_IFREG || name == nullptr || size < 0 ||
            size > ZIP_ENTRY_MAX || total + (gsize)size > ZIP_TOTAL_MAX) {
            archive_read_data_skip(a);
            continue;
        }
        char *buf = g_malloc((gsize)size + 1);
        la_ssize_t n = archive_read_data(a, buf, (size_t)size);
        if (n != size) {
            g_free(buf);
            ok = false;
            break;
        }
        total += (gsize)size;
        g_hash_table_insert(p->zip, g_strdup(name), g_bytes_new_take(buf, (gsize)size));
    }
    if (!ok)
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", path,
                    archive_error_string(a) != nullptr ? archive_error_string(a)
                                                        : "not a theme package");
    archive_read_free(a);
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
    parse_ctx *c = data;
    if (strcmp(element, "xdtheme") == 0) {
        const char *name = attr(names, values, "name");
        g_free(c->pkg->name);
        c->pkg->name = g_strdup(name != nullptr ? name : "");
        const char *base = attr(names, values, "base");
        if (base == nullptr ||
            !xd_visual_style_from_name(xd_str_from_cstr(base), &c->pkg->base))
            c->pkg->base = XD_STYLE_CLASSIC;
    } else if (strcmp(element, "variant") == 0) {
        const char *id = attr(names, values, "id");
        const char *style = attr(names, values, "style");
        if (id == nullptr || style == nullptr)
            return;
        bool wanted = c->want_variant != nullptr && strcmp(id, c->want_variant) == 0;
        if (c->style_path == nullptr || wanted) {
            if (c->style_path != nullptr && !wanted)
                return;
            g_free(c->style_path);
            g_free(c->shell_path);
            g_free(c->pkg->variant);
            c->style_path = g_strdup(style);
            c->shell_path = g_strdup(attr(names, values, "shell"));
            c->pkg->variant = g_strdup(id);
            if (wanted)
                c->want_variant = nullptr; /* found: keep it */
        }
    } else if (strcmp(element, "scaling") == 0) {
        const char *f = attr(names, values, "filter");
        c->pkg->has_filter = f != nullptr && xd_image_filter_from_name(f, &c->pkg->filter);
    }
}

static void add_prop(xd_pkg *p, const char *cls, const char *part, const char *state,
                     const char *prop, const char *value) {
    char *lc = lower(cls);
    char *pk = part_key(p, cls, part);
    char *sk = state_key(p, cls, pk, state);
    char *lp = lower(prop);
    g_hash_table_replace(p->props, g_strdup_printf("%s\x1f%s\x1f%s\x1f%s", lc, pk, sk, lp),
                         g_strdup(value));
    g_hash_table_replace(p->parts, g_strdup_printf("%s\x1f%s", lc, pk), GINT_TO_POINTER(1));
    g_free(lp);
    g_free(sk);
    g_free(pk);
    g_free(lc);
}

static void style_start(G_GNUC_UNUSED GMarkupParseContext *mc, const char *element,
                        const char **names, const char **values, gpointer data,
                        G_GNUC_UNUSED GError **error) {
    parse_ctx *c = data;
    if (strcmp(element, "class") == 0) {
        g_free(c->cls);
        const char *name = attr(names, values, "name");
        c->cls = g_strdup(name != nullptr ? name : "");
    } else if (strcmp(element, "part") == 0 && c->cls != nullptr) {
        const char *part = attr(names, values, "name");
        const char *state = attr(names, values, "state");
        for (size_t i = 0; names[i] != nullptr; i++) {
            if (g_ascii_strcasecmp(names[i], "name") == 0 ||
                g_ascii_strcasecmp(names[i], "state") == 0)
                continue;
            add_prop(c->pkg, c->cls, part, state, names[i], values[i]);
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
            g_hash_table_replace(c->pkg->sys, g_strdup_printf("font:%s", ln),
                                 g_strdup_printf("%s\x1f%s\x1f%d\x1f%d", face ? face : "",
                                                 size ? size : "0",
                                                 bold != nullptr && strcmp(bold, "true") == 0,
                                                 italic != nullptr &&
                                                     strcmp(italic, "true") == 0));
        } else {
            const char *value = attr(names, values, "value");
            g_hash_table_replace(c->pkg->sys,
                                 g_strdup_printf("%s:%s",
                                                 strcmp(element, "color") == 0 ? "color" : "value",
                                                 ln),
                                 g_strdup(value != nullptr ? value : ""));
        }
        g_free(ln);
    }
}

static bool parse(xd_pkg *p, const char *path, const GMarkupParser *parser, parse_ctx *c,
                  GError **error) {
    GBytes *b = read_file(p, path);
    if (b == nullptr) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, "%s is missing", path);
        return false;
    }
    gsize len = 0;
    const char *text = g_bytes_get_data(b, &len);
    GMarkupParseContext *mc = g_markup_parse_context_new(parser, 0, c, nullptr);
    bool ok = g_markup_parse_context_parse(mc, text, (gssize)len, error) &&
              g_markup_parse_context_end_parse(mc, error);
    g_markup_parse_context_free(mc);
    g_bytes_unref(b);
    if (!ok)
        g_prefix_error(error, "%s: ", path);
    return ok;
}

xd_pkg *xd_pkg_load(const char *path, const char *variant, const char *names_path,
                    GError **error) {
    xd_pkg *p = g_new0(xd_pkg, 1);
    p->props = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    p->parts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    p->sys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    p->part_numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    p->state_numbers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    p->cache = g_new0(pkg_cache, 1);
    p->cache->images = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)cairo_surface_destroy);
    p->cache->scaled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)cairo_surface_destroy);
    p->cache->faces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);
    g_mutex_init(&p->cache->lock);
    p->name = g_strdup("");
    if (!load_names(p, names_path, error)) {
        xd_pkg_free(p);
        return nullptr;
    }
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        p->root = g_strdup(path);
    } else {
#if XD_HAVE_LIBARCHIVE
        p->zip = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                       (GDestroyNotify)g_bytes_unref);
        if (!read_zip(p, path, error)) {
            xd_pkg_free(p);
            return nullptr;
        }
#else
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                    "%s: theme packages must be folders (built without libarchive)", path);
        xd_pkg_free(p);
        return nullptr;
#endif
    }
    parse_ctx c = {.pkg = p, .want_variant = variant};
    static const GMarkupParser theme_parser = {.start_element = theme_start};
    static const GMarkupParser style_parser = {.start_element = style_start};
    bool ok = parse(p, "theme.xml", &theme_parser, &c, error);
    if (ok && c.style_path == nullptr) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL, "%s: no variant", path);
        ok = false;
    }
    if (ok)
        ok = parse(p, c.style_path, &style_parser, &c, error);
    /* Explorer's panes: classes and parts as in the style, over it (XP's
     * shellstyle.dll is what its task pane is drawn from). */
    if (ok && c.shell_path != nullptr) {
        g_free(c.cls);
        c.cls = nullptr;
        ok = parse(p, c.shell_path, &style_parser, &c, error);
    }
    g_free(c.style_path);
    g_free(c.shell_path);
    g_free(c.cls);
    if (!ok) {
        xd_pkg_free(p);
        return nullptr;
    }
    sysmetrics_overlay(p);
    return p;
}

void xd_pkg_free(xd_pkg *p) {
    if (p == nullptr)
        return;
    g_hash_table_unref(p->props);
    g_hash_table_unref(p->parts);
    g_hash_table_unref(p->sys);
    g_hash_table_unref(p->part_numbers);
    g_hash_table_unref(p->state_numbers);
    g_hash_table_unref(p->cache->images);
    g_hash_table_unref(p->cache->scaled);
    g_hash_table_unref(p->cache->faces);
    g_mutex_clear(&p->cache->lock);
    g_free(p->cache);
    if (p->zip != nullptr)
        g_hash_table_unref(p->zip);
    g_free(p->root);
    g_free(p->name);
    g_free(p->variant);
    g_free(p);
}

const char *xd_pkg_name(const xd_pkg *p) { return p->name; }
const char *xd_pkg_variant(const xd_pkg *p) { return p->variant; }
xd_visual_style xd_pkg_base(const xd_pkg *p) { return p->base; }

bool xd_pkg_filter(const xd_pkg *p, xd_image_filter *out) {
    if (p->has_filter)
        *out = p->filter;
    return p->has_filter;
}

/* ---- properties ------------------------------------------------------------------ */

static const char *lookup(const xd_pkg *p, const char *lc, const char *pk, const char *sk,
                          const char *lp) {
    char *key = g_strdup_printf("%s\x1f%s\x1f%s\x1f%s", lc, pk, sk, lp);
    const char *v = g_hash_table_lookup(p->props, key);
    g_free(key);
    return v;
}

/* Part and state, then part, then class, in one class. */
static const char *lookup_class(const xd_pkg *p, const char *cls, const char *part,
                                const char *state, const char *lp) {
    char *lc = lower(cls);
    char *pk = part_key(p, cls, part);
    char *sk = state_key(p, cls, pk, state);
    const char *v = lookup(p, lc, pk, sk, lp);
    if (v == nullptr && strcmp(sk, "0") != 0)
        v = lookup(p, lc, pk, "0", lp);
    if (v == nullptr && strcmp(pk, "0") != 0)
        v = lookup(p, lc, "0", "0", lp);
    g_free(sk);
    g_free(pk);
    g_free(lc);
    return v;
}

const char *xd_pkg_prop(const xd_pkg *p, const char *cls, const char *part,
                        const char *state, const char *prop) {
    if (p == nullptr)
        return nullptr;
    char *lp = lower(prop);
    const char *v = lookup_class(p, cls, part, state, lp);
    const char *colon = strstr(cls, "::");
    char *base = g_strdup(colon != nullptr ? colon + 2 : cls);
    if (v == nullptr && colon != nullptr)
        v = lookup_class(p, base, part, state, lp);
    if (v == nullptr) {
        char *style = g_strdup_printf("%sStyle", base);
        v = lookup_class(p, style, part, state, lp);
        g_free(style);
    }
    if (v == nullptr)
        v = lookup(p, "globals", "0", "0", lp);
    g_free(base);
    g_free(lp);
    return v;
}

static bool has_in_class(const xd_pkg *p, const char *cls, const char *part) {
    char *lc = lower(cls);
    char *pk = part_key(p, cls, part);
    char *key = g_strdup_printf("%s\x1f%s", lc, pk);
    bool has = g_hash_table_contains(p->parts, key);
    g_free(key);
    g_free(pk);
    g_free(lc);
    return has;
}

bool xd_pkg_has_own(const xd_pkg *p, const char *cls, const char *part) {
    return p != nullptr && has_in_class(p, cls, part);
}

/* In the classes xd_pkg_prop looks in: App::Class, Class, ClassStyle. */
bool xd_pkg_has(const xd_pkg *p, const char *cls, const char *part) {
    if (p == nullptr)
        return false;
    if (has_in_class(p, cls, part))
        return true;
    const char *colon = strstr(cls, "::");
    const char *base = colon != nullptr ? colon + 2 : cls;
    if (colon != nullptr && has_in_class(p, base, part))
        return true;
    char *style = g_strdup_printf("%sStyle", base);
    bool has = has_in_class(p, style, part);
    g_free(style);
    return has;
}

/* Whole numbers in a value ("8 8 9 9", "0 84 227", "8, 8, 9, 9"). */
static size_t numbers(const char *v, int *out, size_t max) {
    size_t n = 0;
    while (v != nullptr && *v != '\0' && n < max) {
        while (*v != '\0' && *v != '-' && !g_ascii_isdigit(*v))
            v++;
        if (*v == '\0')
            break;
        char *end;
        long x = strtol(v, &end, 10);
        if (end == v)
            break;
        out[n++] = (int)CLAMP(x, -100000, 100000);
        v = end;
    }
    return n;
}

bool xd_pkg_int(const xd_pkg *p, const char *cls, const char *part, const char *state,
                const char *prop, int *out) {
    int n[1];
    if (numbers(xd_pkg_prop(p, cls, part, state, prop), n, 1) != 1)
        return false;
    *out = n[0];
    return true;
}

bool xd_pkg_bool(const xd_pkg *p, const char *cls, const char *part, const char *state,
                 const char *prop, bool *out) {
    const char *v = xd_pkg_prop(p, cls, part, state, prop);
    if (v == nullptr)
        return false;
    *out = g_ascii_strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0;
    return true;
}

static bool parse_color(const char *v, xd_rgba *out) {
    if (v == nullptr)
        return false;
    if (v[0] == '#' && strlen(v) == 7) {
        unsigned long x = strtoul(v + 1, nullptr, 16);
        *out = xd_rgba_hex((uint32_t)x & 0xFFFFFFu);
        return true;
    }
    int n[3];
    if (numbers(v, n, 3) != 3)
        return false;
    *out = xd_rgba_hex((uint32_t)CLAMP(n[0], 0, 255) << 16 |
                       (uint32_t)CLAMP(n[1], 0, 255) << 8 | (uint32_t)CLAMP(n[2], 0, 255));
    return true;
}

bool xd_pkg_color(const xd_pkg *p, const char *cls, const char *part, const char *state,
                  const char *prop, xd_rgba *out) {
    return parse_color(xd_pkg_prop(p, cls, part, state, prop), out);
}

bool xd_pkg_margins(const xd_pkg *p, const char *cls, const char *part, const char *state,
                    const char *prop, xd_insets *out) {
    int n[4];
    if (numbers(xd_pkg_prop(p, cls, part, state, prop), n, 4) != 4)
        return false;
    *out = (xd_insets){.left = n[0], .right = n[1], .top = n[2], .bottom = n[3]};
    return true;
}

/* A FONT value as msstyles write them: "Tahoma, 8, Bold, Underline,
 * Quality:ClearType" (face, points, then flags in any order). */
bool xd_pkg_font(const xd_pkg *p, const char *cls, const char *part, const char *state,
                 const char *prop, xd_font_spec *out, bool *underline) {
    const char *v = xd_pkg_prop(p, cls, part, state, prop);
    if (v == nullptr)
        return false;
    char **fields = g_strsplit(v, ",", -1);
    bool ok = fields[0] != nullptr && fields[1] != nullptr;
    if (ok) {
        xd_font_spec f = {.size_pt = g_ascii_strtod(g_strstrip(fields[1]), nullptr)};
        font_family(g_strstrip(fields[0]), f.family, sizeof(f.family));
        bool under = false;
        for (size_t i = 2; fields[i] != nullptr; i++) {
            const char *flag = g_strstrip(fields[i]);
            if (g_ascii_strcasecmp(flag, "bold") == 0)
                f.bold = true;
            else if (g_ascii_strcasecmp(flag, "italic") == 0)
                f.italic = true;
            else if (g_ascii_strcasecmp(flag, "underline") == 0)
                under = true;
        }
        ok = f.size_pt > 0;
        if (ok) {
            *out = f;
            if (underline != nullptr)
                *underline = under;
        }
    }
    g_strfreev(fields);
    return ok;
}

static const char *sys(const xd_pkg *p, const char *kind, const char *name) {
    if (p == nullptr)
        return nullptr;
    char *ln = lower(name);
    char *key = g_strdup_printf("%s:%s", kind, ln);
    const char *v = g_hash_table_lookup(p->sys, key);
    g_free(key);
    g_free(ln);
    return v;
}

bool xd_pkg_sys_color(const xd_pkg *p, const char *name, xd_rgba *out) {
    return parse_color(sys(p, "color", name), out);
}

bool xd_pkg_sys_int(const xd_pkg *p, const char *name, int *out) {
    int n[1];
    if (numbers(sys(p, "value", name), n, 1) != 1)
        return false;
    *out = n[0];
    return true;
}

bool xd_pkg_sys_bool(const xd_pkg *p, const char *name, bool *out) {
    const char *v = sys(p, "value", name);
    if (v == nullptr)
        return false;
    *out = g_ascii_strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0;
    return true;
}

bool xd_pkg_sys_font(const xd_pkg *p, const char *name, const char **face, int *points,
                     bool *bold, bool *italic) {
    const char *v = sys(p, "font", name);
    if (v == nullptr)
        return false;
    /* "face\x1fsize\x1fbold\x1fitalic", the face ending at the first
     * separator. */
    static_assert(sizeof(char) == 1);
    const char *s1 = strchr(v, '\x1f');
    if (s1 == nullptr)
        return false;
    int n[3];
    if (numbers(s1, n, 3) != 3)
        return false;
    /* The caller gets the face up to the separator: copy it into the
     * package's string table so it outlives this call. */
    char *f = g_strndup(v, (gsize)(s1 - v));
    g_mutex_lock(&p->cache->lock);
    const char *kept = g_hash_table_lookup(p->cache->faces, f);
    if (kept == nullptr) {
        g_hash_table_insert(p->cache->faces, f, f);
        kept = f;
    } else {
        g_free(f);
    }
    g_mutex_unlock(&p->cache->lock);
    *face = kept;
    *points = n[0];
    *bold = n[1] != 0;
    *italic = n[2] != 0;
    return true;
}

/* ---- images ---------------------------------------------------------------------- */

typedef struct png_reader {
    const uint8_t *data;
    gsize len, pos;
} png_reader;

static cairo_status_t png_read(void *closure, unsigned char *buf, unsigned int length) {
    png_reader *r = closure;
    if (r->len - r->pos < length)
        return CAIRO_STATUS_READ_ERROR;
    memcpy(buf, r->data + r->pos, length);
    r->pos += length;
    return CAIRO_STATUS_SUCCESS;
}

/* An ARGB32 surface (what the scalers take), whatever the PNG was. */
static cairo_surface_t *argb32(cairo_surface_t *s) {
    if (cairo_image_surface_get_format(s) == CAIRO_FORMAT_ARGB32)
        return s;
    int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
    cairo_surface_t *out = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(out);
    cairo_set_source_surface(cr, s, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(s);
    return out;
}

/* The package's image at `path` as loaded (cached; the package owns it). */
static cairo_surface_t *image(const xd_pkg *p, const char *path) {
    cairo_surface_t *s = g_hash_table_lookup(p->cache->images, path);
    if (s != nullptr)
        return s;
    GBytes *b = read_file(p, path);
    if (b == nullptr)
        return nullptr;
    gsize len = 0;
    png_reader r = {.data = g_bytes_get_data(b, &len)};
    r.len = len;
    s = cairo_image_surface_create_from_png_stream(png_read, &r);
    g_bytes_unref(b);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return nullptr;
    }
    s = argb32(s);
    g_hash_table_insert(p->cache->images, g_strdup(path), s);
    return s;
}

/* The filter the theme's image settings give ("Automatic" is resolved to
 * the package's own by whoever chose the package: a Smooth chosen by the
 * user stays Smooth). */
static xd_image_filter filter_for(const xd_theme *t, double factor) {
    xd_image_filter f = t->image_filter;
    /* Whole-number scaling: smooth scaling at 200% would blur what can be
     * exact. */
    if (f == XD_FILTER_SMOOTH && t->integer_scaling && fabs(factor - round(factor)) < 1e-6 &&
        factor >= 1.0)
        f = XD_FILTER_INTEGER;
    return f;
}

/* One part's image strip as drawn: an ImageFile (or an atlas's rectangle)
 * scaled by `factor`, `count` frames. Cached; owned by the package. */
static cairo_surface_t *strip(const xd_theme *t, const char *path, const int *atlas,
                              int count, bool vertical, double factor) {
    const xd_pkg *p = t->pkg;
    xd_image_filter f = filter_for(t, factor);
    char *key = atlas != nullptr ? g_strdup_printf("%s#%d,%d,%d,%d@%d%c%.4f/%d", path, atlas[0],
                                                   atlas[1], atlas[2], atlas[3], count,
                                                   vertical ? 'v' : 'h', factor, (int)f)
                                 : g_strdup_printf("%s@%d%c%.4f/%d", path, count,
                                                   vertical ? 'v' : 'h', factor, (int)f);
    g_mutex_lock(&p->cache->lock);
    cairo_surface_t *s = g_hash_table_lookup(p->cache->scaled, key);
    if (s == nullptr) {
        cairo_surface_t *src = image(p, path);
        cairo_surface_t *cut = nullptr;
        if (src != nullptr && atlas != nullptr) {
            int w = atlas[2] - atlas[0], h = atlas[3] - atlas[1];
            if (w > 0 && h > 0 && atlas[0] >= 0 && atlas[1] >= 0 &&
                atlas[2] <= cairo_image_surface_get_width(src) &&
                atlas[3] <= cairo_image_surface_get_height(src)) {
                cut = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
                cairo_t *cr = cairo_create(cut);
                cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
                cairo_set_source_surface(cr, src, -atlas[0], -atlas[1]);
                cairo_paint(cr);
                cairo_destroy(cr);
            }
            src = cut;
        }
        if (src != nullptr) {
            s = fabs(factor - 1.0) < 1e-6 && cut == nullptr
                    ? cairo_surface_reference(src)
                    : xd_image_scale_frames(src, count, vertical, factor, f);
            if (s != nullptr) { /* nullptr: too big, or frames that don't divide */
                g_hash_table_insert(p->cache->scaled, key, s);
                key = nullptr;
            }
        }
        if (cut != nullptr)
            cairo_surface_destroy(cut);
    }
    g_mutex_unlock(&p->cache->lock);
    g_free(key);
    return s;
}

typedef struct part_image {
    cairo_surface_t *strip;
    int count;
    bool vertical;
    int frame; /* the state's */
    int fw, fh; /* one frame, device px */
    double factor;
} part_image;

static bool enum_is(const xd_pkg *p, const char *cls, const char *part, const char *state,
                    const char *prop, const char *value) {
    const char *v = xd_pkg_prop(p, cls, part, state, prop);
    return v != nullptr && g_ascii_strcasecmp(v, value) == 0;
}

/* The state's number (its frame is number - 1). */
static int state_number(const xd_pkg *p, const char *cls, const char *part, const char *state) {
    char *pk = part_key(p, cls, part);
    char *sk = state_key(p, cls, pk, state);
    int n = all_digits(sk) ? atoi(sk) : 0;
    g_free(sk);
    g_free(pk);
    return n;
}

/* Finds the part's image for the state at the theme's DPI: ImageFile, or
 * with ImageSelectType the ImageFileN for the DPI (by MinDpiN) or size (by
 * MinSizeN), scaled from the DPI it was drawn for, as uxtheme does.
 * `glyph`: the GlyphImageFile instead. */
static bool part_image_for(const xd_theme *t, const char *cls, const char *part,
                           const char *state, xd_rect r, bool glyph, part_image *out) {
    const xd_pkg *p = t->pkg;
    const char *file = xd_pkg_prop(p, cls, part, state, glyph ? "GlyphImageFile" : "ImageFile");
    double target_dpi = 96.0 * t->scale;
    double drawn_dpi = 96.0;
    int count = 1;
    (void)xd_pkg_int(p, cls, part, state, "ImageCount", &count);
    count = CLAMP(count, 1, 64);
    /* An image glyph without a GlyphImageFile has its sizes in
     * ImageFile1..N (XP's caption buttons): the largest that fits, as it
     * is. They're never the background. */
    bool glyph_sizes = enum_is(p, cls, part, state, "GlyphType", "ImageGlyph") &&
                       xd_pkg_prop(p, cls, part, state, "GlyphImageFile") == nullptr;
    if (glyph && glyph_sizes) {
        int best = -1;
        for (int i = 1; i <= 7; i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "ImageFile%d", i);
            const char *f = xd_pkg_prop(p, cls, part, state, prop);
            cairo_surface_t *img = f != nullptr ? image(p, f) : nullptr;
            if (img == nullptr)
                continue;
            int fw = cairo_image_surface_get_width(img);
            int fh = cairo_image_surface_get_height(img) / count;
            if (fw <= r.w && fh <= r.h && fw > best) {
                best = fw;
                file = f;
                drawn_dpi = target_dpi;
            } else if (file == nullptr) {
                file = f; /* nothing fits: the smallest, shrunk */
            }
        }
    }
    if (!glyph && !glyph_sizes && !enum_is(p, cls, part, state, "ImageSelectType", "None")) {
        bool by_dpi = enum_is(p, cls, part, state, "ImageSelectType", "Dpi");
        bool by_size = enum_is(p, cls, part, state, "ImageSelectType", "Size");
        for (int i = 1; i <= 7 && (by_dpi || by_size); i++) {
            char prop[16];
            g_snprintf(prop, sizeof(prop), "ImageFile%d", i);
            const char *f = xd_pkg_prop(p, cls, part, state, prop);
            if (f == nullptr)
                break;
            if (by_dpi) {
                int dpi = 96;
                g_snprintf(prop, sizeof(prop), "MinDpi%d", i);
                (void)xd_pkg_int(p, cls, part, state, prop, &dpi);
                if (dpi <= target_dpi + 0.5 || file == nullptr) {
                    file = f;
                    drawn_dpi = dpi > 0 ? dpi : 96.0;
                }
            } else {
                int n[2] = {0, 0};
                g_snprintf(prop, sizeof(prop), "MinSize%d", i);
                if (numbers(xd_pkg_prop(p, cls, part, state, prop), n, 2) >= 1 &&
                    n[0] <= r.w && (n[1] == 0 || n[1] <= r.h)) {
                    file = f;
                    drawn_dpi = target_dpi; /* chosen to fit: not scaled */
                } else if (file == nullptr) {
                    file = f;
                }
            }
        }
    }
    const char *atlas_file = nullptr;
    int atlas[4];
    bool has_atlas = numbers(xd_pkg_prop(p, cls, part, state, "AtlasRect"), atlas, 4) == 4;
    if (has_atlas && !glyph) {
        atlas_file = xd_pkg_prop(p, cls, part, state, "AtlasImage");
        if (atlas_file == nullptr)
            atlas_file = xd_pkg_prop(p, cls, nullptr, nullptr, "DiskStream");
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
    const char *layout = xd_pkg_prop(p, cls, part, state, "ImageLayout");
    bool vertical = layout != nullptr ? g_ascii_strcasecmp(layout, "Horizontal") != 0 : has_atlas;
    double factor = target_dpi / drawn_dpi;
    /* Within a few percent of drawn: as drawn (no blur for a 25 px check
     * box drawn at 185 DPI shown at 192). */
    if (fabs(factor - 1.0) < 0.15)
        factor = 1.0;
    cairo_surface_t *s = strip(t, file, has_atlas ? atlas : nullptr, count, vertical, factor);
    if (s == nullptr)
        return false;
    int sw = cairo_image_surface_get_width(s), sh = cairo_image_surface_get_height(s);
    out->strip = s;
    out->count = count;
    out->vertical = vertical;
    out->fw = vertical ? sw : sw / count;
    out->fh = vertical ? sh / count : sh;
    out->factor = factor;
    int n = state_number(p, cls, part, state);
    out->frame = CLAMP(n > 0 ? n - 1 : 0, 0, count - 1);
    return out->fw > 0 && out->fh > 0;
}

/* Paints the `src` rectangle of a frame into `dst`, stretched or tiled,
 * reading nothing outside `src` (no bleeding between slices). */
static void blit(cairo_t *cr, const part_image *im, xd_rect src, xd_rect dst, bool tile) {
    if (src.w <= 0 || src.h <= 0 || dst.w <= 0 || dst.h <= 0)
        return;
    int fx = im->vertical ? 0 : im->frame * im->fw;
    int fy = im->vertical ? im->frame * im->fh : 0;
    cairo_surface_t *sub = cairo_surface_create_for_rectangle(im->strip, fx + src.x, fy + src.y,
                                                              src.w, src.h);
    cairo_save(cr);
    cairo_rectangle(cr, dst.x, dst.y, dst.w, dst.h);
    cairo_clip(cr);
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(sub);
    cairo_matrix_t m;
    if (tile) {
        cairo_matrix_init_translate(&m, -dst.x, -dst.y);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_REPEAT);
        cairo_pattern_set_filter(pat, CAIRO_FILTER_NEAREST);
    } else {
        cairo_matrix_init_scale(&m, (double)src.w / dst.w, (double)src.h / dst.h);
        cairo_matrix_translate(&m, -dst.x, -dst.y);
        cairo_pattern_set_extend(pat, CAIRO_EXTEND_PAD);
        /* Slices are stretched along one axis, where they don't change:
         * nearest keeps their edges as drawn. */
        cairo_pattern_set_filter(pat, (src.w == dst.w || src.h == dst.h)
                                          ? CAIRO_FILTER_NEAREST
                                          : CAIRO_FILTER_BILINEAR);
    }
    cairo_pattern_set_matrix(pat, &m);
    cairo_set_source(cr, pat);
    cairo_paint(cr);
    cairo_pattern_destroy(pat);
    cairo_surface_destroy(sub);
    cairo_restore(cr);
}

static int scaled(double v, double factor) { return (int)lround(v * factor); }

/* uxtheme's 9-grid: corners as they are, edges and middle stretched or
 * tiled; margins larger than the target shrink to fit. */
static void nine_grid(cairo_t *cr, const part_image *im, xd_insets m, xd_rect r, bool tile,
                      bool border_only) {
    int l = CLAMP(scaled(m.left, im->factor), 0, im->fw), rr = CLAMP(scaled(m.right, im->factor), 0, im->fw - l);
    int top = CLAMP(scaled(m.top, im->factor), 0, im->fh), b = CLAMP(scaled(m.bottom, im->factor), 0, im->fh - top);
    int dl = l, dr = rr, dt = top, db = b;
    if (dl + dr > r.w && dl + dr > 0) {
        dl = r.w * dl / (dl + dr);
        dr = r.w - dl;
    }
    if (dt + db > r.h && dt + db > 0) {
        dt = r.h * dt / (dt + db);
        db = r.h - dt;
    }
    int sx[3] = {0, l, im->fw - rr}, sw[3] = {l, im->fw - l - rr, rr};
    int sy[3] = {0, top, im->fh - b}, sh[3] = {top, im->fh - top - b, b};
    int dx[3] = {r.x, r.x + dl, r.x + r.w - dr}, dw[3] = {dl, r.w - dl - dr, dr};
    int dy[3] = {r.y, r.y + dt, r.y + r.h - db}, dh[3] = {dt, r.h - dt - db, db};
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) {
            if (border_only && i == 1 && j == 1)
                continue;
            bool corner = i != 1 && j != 1;
            blit(cr, im, (xd_rect){sx[i], sy[j], sw[i], sh[j]},
                 (xd_rect){dx[i], dy[j], dw[i], dh[j]}, tile && !corner);
        }
}

/* TrueSize: the frame as it is, aligned in `r` (centred by default);
 * shrunk uniformly if it doesn't fit, stretched past TrueSizeStretchMark. */
static void true_size(const xd_theme *t, cairo_t *cr, const part_image *im, const char *cls,
                      const char *part, const char *state, xd_rect r) {
    int w = im->fw, h = im->fh;
    int mark = 0;
    if (xd_pkg_int(t->pkg, cls, part, state, "TrueSizeStretchMark", &mark) && mark > 0 &&
        (r.w > w * (100 + mark) / 100 || r.h > h * (100 + mark) / 100)) {
        double k = fmin((double)r.w / w, (double)r.h / h);
        w = (int)(w * k);
        h = (int)(h * k);
    }
    if (w > r.w || h > r.h) {
        double k = fmin((double)r.w / w, (double)r.h / h);
        w = MAX(1, (int)(w * k));
        h = MAX(1, (int)(h * k));
    }
    int x = r.x + (r.w - w) / 2, y = r.y + (r.h - h) / 2;
    if (enum_is(t->pkg, cls, part, state, "HAlign", "Left"))
        x = r.x;
    else if (enum_is(t->pkg, cls, part, state, "HAlign", "Right"))
        x = r.x + r.w - w;
    if (enum_is(t->pkg, cls, part, state, "VAlign", "Top"))
        y = r.y;
    else if (enum_is(t->pkg, cls, part, state, "VAlign", "Bottom"))
        y = r.y + r.h - h;
    blit(cr, im, (xd_rect){0, 0, im->fw, im->fh}, (xd_rect){x, y, w, h}, false);
}

static void border_fill(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                        const char *state, xd_rect r) {
    const xd_pkg *p = t->pkg;
    int border = 1;
    (void)xd_pkg_int(p, cls, part, state, "BorderSize", &border);
    border = scaled(MAX(border, 0), t->scale);
    xd_rgba bc, fc, g1, g2;
    bool has_fill = xd_pkg_color(p, cls, part, state, "FillColor", &fc);
    if (!enum_is(p, cls, part, state, "FillType", "Solid") &&
        xd_pkg_color(p, cls, part, state, "GradientColor1", &g1) &&
        xd_pkg_color(p, cls, part, state, "GradientColor2", &g2)) {
        bool horz = enum_is(p, cls, part, state, "FillType", "HorzGradient");
        cairo_pattern_t *pat = horz ? cairo_pattern_create_linear(r.x, 0, r.x + r.w, 0)
                                    : cairo_pattern_create_linear(0, r.y, 0, r.y + r.h);
        cairo_pattern_add_color_stop_rgba(pat, 0, g1.r, g1.g, g1.b, g1.a);
        cairo_pattern_add_color_stop_rgba(pat, 1, g2.r, g2.g, g2.b, g2.a);
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        cairo_set_source(cr, pat);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    } else if (has_fill) {
        xd_fill_rect(cr, r, fc);
    }
    if (border > 0 && xd_pkg_color(p, cls, part, state, "BorderColor", &bc)) {
        xd_fill_rect(cr, (xd_rect){r.x, r.y, r.w, border}, bc);
        xd_fill_rect(cr, (xd_rect){r.x, r.y + r.h - border, r.w, border}, bc);
        xd_fill_rect(cr, (xd_rect){r.x, r.y, border, r.h}, bc);
        xd_fill_rect(cr, (xd_rect){r.x + r.w - border, r.y, border, r.h}, bc);
    }
}

bool xd_pkg_draw_background(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                            const char *state, xd_rect r) {
    const xd_pkg *p = t->pkg;
    if (p == nullptr || r.w <= 0 || r.h <= 0 || !xd_pkg_has(p, cls, part))
        return false;
    bool glyph_only = false;
    (void)xd_pkg_bool(p, cls, part, state, "GlyphOnly", &glyph_only);
    if (glyph_only || enum_is(p, cls, part, state, "BgType", "None"))
        return false;
    if (enum_is(p, cls, part, state, "BgType", "BorderFill")) {
        border_fill(t, cr, cls, part, state, r);
        return true;
    }
    part_image im;
    if (!part_image_for(t, cls, part, state, r, false, &im))
        return false;
    if (enum_is(p, cls, part, state, "SizingType", "TrueSize")) {
        true_size(t, cr, &im, cls, part, state, r);
    } else {
        xd_insets m = {0};
        (void)xd_pkg_margins(p, cls, part, state, "SizingMargins", &m);
        bool border_only = false;
        (void)xd_pkg_bool(p, cls, part, state, "BorderOnly", &border_only);
        nine_grid(cr, &im, m, r, enum_is(p, cls, part, state, "SizingType", "Tile"),
                  border_only);
    }
    return true;
}

/* An image glyph: centred in the content area, as it is. */
bool xd_pkg_draw_glyph(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                       const char *state, xd_rect r) {
    const xd_pkg *p = t->pkg;
    if (p == nullptr || r.w <= 0 || r.h <= 0 || !xd_pkg_has(p, cls, part) ||
        !enum_is(p, cls, part, state, "GlyphType", "ImageGlyph"))
        return false;
    part_image g;
    xd_rect c = xd_pkg_content_rect(t, cls, part, state, r);
    if (!part_image_for(t, cls, part, state, c, true, &g))
        return false;
    true_size(t, cr, &g, cls, part, state, c);
    return true;
}

bool xd_pkg_draw(const xd_theme *t, cairo_t *cr, const char *cls, const char *part,
                 const char *state, xd_rect r) {
    bool bg = xd_pkg_draw_background(t, cr, cls, part, state, r);
    bool glyph = xd_pkg_draw_glyph(t, cr, cls, part, state, r);
    return bg || glyph;
}

bool xd_pkg_glyph_size(const xd_theme *t, const char *cls, const char *part, const char *state,
                       int *w, int *h) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, cls, part) ||
        !enum_is(t->pkg, cls, part, state, "GlyphType", "ImageGlyph"))
        return false;
    part_image g;
    if (!part_image_for(t, cls, part, state, (xd_rect){0, 0, 1 << 16, 1 << 16}, true, &g))
        return false;
    *w = g.fw;
    *h = g.fh;
    return true;
}

bool xd_pkg_part_size(const xd_theme *t, const char *cls, const char *part, const char *state,
                      int *w, int *h) {
    if (t->pkg == nullptr || !xd_pkg_has(t->pkg, cls, part))
        return false;
    part_image im;
    if (!part_image_for(t, cls, part, state, (xd_rect){0, 0, 1 << 16, 1 << 16}, false, &im))
        return false;
    *w = im.fw;
    *h = im.fh;
    return true;
}

xd_rect xd_pkg_content_rect(const xd_theme *t, const char *cls, const char *part,
                            const char *state, xd_rect r) {
    xd_insets m;
    if (t->pkg == nullptr || !xd_pkg_margins(t->pkg, cls, part, state, "ContentMargins", &m))
        return r;
    int l = scaled(m.left, t->scale), rr = scaled(m.right, t->scale);
    int top = scaled(m.top, t->scale), b = scaled(m.bottom, t->scale);
    xd_rect c = {r.x + l, r.y + top, r.w - l - rr, r.h - top - b};
    if (c.w < 0 || c.h < 0)
        return r;
    return c;
}
