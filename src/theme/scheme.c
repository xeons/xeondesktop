/* SPDX-License-Identifier: MIT */
#include "scheme.h"
#include <glib.h>

static const char *const color_names[XD_COLOR_COUNT] = {
#define XD_X(id, name) [XD_COLOR_##id] = name,
    XD_SYSCOLORS(XD_X)
#undef XD_X
};

static const char *const metric_names[XD_METRIC_COUNT] = {
#define XD_X(id, name, def) [XD_METRIC_##id] = name,
    XD_METRICS(XD_X)
#undef XD_X
};

static const int metric_defaults[XD_METRIC_COUNT] = {
#define XD_X(id, name, def) [XD_METRIC_##id] = def,
    XD_METRICS(XD_X)
#undef XD_X
};

static const char *const font_names[XD_FONT_COUNT] = {
    [XD_FONT_CAPTION] = "CaptionFont", [XD_FONT_MENU] = "MenuFont",
    [XD_FONT_MESSAGE] = "MessageFont", [XD_FONT_ICON] = "IconFont",
    [XD_FONT_TOOLTIP] = "StatusFont",
};

const char *xd_syscolor_name(xd_syscolor c) {
    XD_ASSERT((unsigned)c < XD_COLOR_COUNT);
    return color_names[c];
}

bool xd_syscolor_from_name(xd_str name, xd_syscolor *out) {
    for (size_t i = 0; i < XD_COLOR_COUNT; i++) {
        if (xd_str_eq_nocase(name, xd_str_from_cstr(color_names[i]))) {
            *out = (xd_syscolor)i;
            return true;
        }
    }
    return false;
}

static void set_font(xd_font_spec *f, const char *family, double pt, bool bold) {
    snprintf(f->family, sizeof(f->family), "%s", family);
    f->size_pt = pt;
    f->bold = bold;
    f->italic = false;
}

void xd_scheme_init_default(xd_scheme *s) {
    *s = (xd_scheme){0};
    snprintf(s->name, sizeof(s->name), "%s", "Windows Standard");

    static const xd_rgb standard[XD_COLOR_COUNT] = {
        [XD_COLOR_SCROLLBAR] = {212, 208, 200},
        [XD_COLOR_BACKGROUND] = {58, 110, 165},
        [XD_COLOR_ACTIVE_TITLE] = {10, 36, 106},
        [XD_COLOR_INACTIVE_TITLE] = {128, 128, 128},
        [XD_COLOR_MENU] = {212, 208, 200},
        [XD_COLOR_WINDOW] = {255, 255, 255},
        [XD_COLOR_WINDOW_FRAME] = {0, 0, 0},
        [XD_COLOR_MENU_TEXT] = {0, 0, 0},
        [XD_COLOR_WINDOW_TEXT] = {0, 0, 0},
        [XD_COLOR_TITLE_TEXT] = {255, 255, 255},
        [XD_COLOR_ACTIVE_BORDER] = {212, 208, 200},
        [XD_COLOR_INACTIVE_BORDER] = {212, 208, 200},
        [XD_COLOR_APP_WORKSPACE] = {128, 128, 128},
        [XD_COLOR_HILIGHT] = {10, 36, 106},
        [XD_COLOR_HILIGHT_TEXT] = {255, 255, 255},
        [XD_COLOR_BUTTON_FACE] = {212, 208, 200},
        [XD_COLOR_BUTTON_SHADOW] = {128, 128, 128},
        [XD_COLOR_GRAY_TEXT] = {128, 128, 128},
        [XD_COLOR_BUTTON_TEXT] = {0, 0, 0},
        [XD_COLOR_INACTIVE_TITLE_TEXT] = {212, 208, 200},
        [XD_COLOR_BUTTON_HILIGHT] = {255, 255, 255},
        [XD_COLOR_BUTTON_DK_SHADOW] = {64, 64, 64},
        [XD_COLOR_BUTTON_LIGHT] = {212, 208, 200},
        [XD_COLOR_INFO_TEXT] = {0, 0, 0},
        [XD_COLOR_INFO_WINDOW] = {255, 255, 225},
        [XD_COLOR_HOT_TRACKING] = {0, 0, 128},
        [XD_COLOR_GRADIENT_ACTIVE_TITLE] = {166, 202, 240},
        [XD_COLOR_GRADIENT_INACTIVE_TITLE] = {192, 192, 192},
        [XD_COLOR_MENU_HILIGHT] = {10, 36, 106},
        [XD_COLOR_MENU_BAR] = {212, 208, 200},
    };
    memcpy(s->colors, standard, sizeof(standard));
    memcpy(s->metrics, metric_defaults, sizeof(metric_defaults));

    /* Tahoma isn't shipped on Linux; fontconfig falls back through the list. */
    const char *ui = XD_FONT_FAMILY_TAHOMA;
    set_font(&s->fonts[XD_FONT_CAPTION], ui, 8, true);
    set_font(&s->fonts[XD_FONT_MENU], ui, 8, false);
    set_font(&s->fonts[XD_FONT_MESSAGE], ui, 8, false);
    set_font(&s->fonts[XD_FONT_ICON], ui, 8, false);
    set_font(&s->fonts[XD_FONT_TOOLTIP], ui, 8, false);
    s->gradient_captions = true;
}

static bool parse_rgb(xd_str v, xd_rgb *out) {
    int64_t parts[3];
    size_t n = 0;
    xd_str rest = xd_str_trim(v);
    while (rest.len > 0) {
        if (n == 3)
            return false;
        size_t i = 0;
        while (i < rest.len && rest.ptr[i] != ' ' && rest.ptr[i] != ',')
            i++;
        if (!xd_str_parse_i64(xd_str_sub(rest, 0, i), 0, 255, &parts[n]))
            return false;
        n++;
        while (i < rest.len && (rest.ptr[i] == ' ' || rest.ptr[i] == ','))
            i++;
        rest = xd_str_sub(rest, i, rest.len);
    }
    if (n != 3)
        return false;
    *out = (xd_rgb){(uint8_t)parts[0], (uint8_t)parts[1], (uint8_t)parts[2]};
    return true;
}

static bool parse_bool(xd_str v, bool *out) {
    if (xd_str_eq_nocase(v, XD_STR("1")) || xd_str_eq_nocase(v, XD_STR("true")) ||
        xd_str_eq_nocase(v, XD_STR("yes"))) {
        *out = true;
        return true;
    }
    if (xd_str_eq_nocase(v, XD_STR("0")) || xd_str_eq_nocase(v, XD_STR("false")) ||
        xd_str_eq_nocase(v, XD_STR("no"))) {
        *out = false;
        return true;
    }
    return false;
}

/* Splits off the last comma-separated token of *rest. */
static xd_str pop_last_token(xd_str *rest) {
    size_t i = rest->len;
    while (i > 0 && rest->ptr[i - 1] != ',')
        i--;
    xd_str tok = xd_str_trim(xd_str_sub(*rest, i, rest->len - i));
    *rest = xd_str_sub(*rest, 0, i > 0 ? i - 1 : 0);
    return tok;
}

/* Font value: "Family[, Fallback...], size[, bold][, italic]". The family
 * may itself be a comma-separated fallback list, so parse from the end. */
static bool parse_font(xd_str v, xd_font_spec *out) {
    xd_font_spec f = {0};
    xd_str rest = xd_str_trim(v);

    xd_str tok = pop_last_token(&rest);
    for (int flags = 0; flags < 2; flags++) {
        if (xd_str_eq_nocase(tok, XD_STR("bold")) && !f.bold)
            f.bold = true;
        else if (xd_str_eq_nocase(tok, XD_STR("italic")) && !f.italic)
            f.italic = true;
        else
            break;
        tok = pop_last_token(&rest);
    }
    if (!xd_str_parse_double(tok, &f.size_pt) || f.size_pt < 4 || f.size_pt > 72)
        return false;

    xd_str family = xd_str_trim(rest);
    if (family.len == 0 || family.len >= sizeof(f.family))
        return false;
    memcpy(f.family, family.ptr, family.len);
    *out = f;
    return true;
}

typedef struct parse_ctx {
    xd_scheme *scheme;
    xd_strbuf *err;
} parse_ctx;

static bool reject(parse_ctx *ctx, xd_str section, xd_str key, const char *why) {
    if (ctx->err != nullptr)
        xd_strbuf_appendf(ctx->err, "[%.*s] %.*s: %s", (int)section.len, section.ptr,
                          (int)key.len, key.ptr, why);
    return false;
}

static bool on_entry(void *user, xd_str section, xd_str key, xd_str value) {
    parse_ctx *ctx = user;
    xd_scheme *s = ctx->scheme;

    if (xd_str_eq_nocase(section, XD_STR("Scheme"))) {
        if (xd_str_eq_nocase(key, XD_STR("Name"))) {
            if (value.len == 0 || value.len >= sizeof(s->name))
                return reject(ctx, section, key, "name must be 1-63 bytes");
            memset(s->name, 0, sizeof(s->name));
            memcpy(s->name, value.ptr, value.len);
            return true;
        }
        if (xd_str_eq_nocase(key, XD_STR("GradientCaptions")))
            return parse_bool(value, &s->gradient_captions) ||
                   reject(ctx, section, key, "expected a boolean");
        if (xd_str_eq_nocase(key, XD_STR("HighContrast")))
            return parse_bool(value, &s->high_contrast) ||
                   reject(ctx, section, key, "expected a boolean");
        if (xd_str_eq_nocase(key, XD_STR("FlatMenus")))
            return parse_bool(value, &s->flat_menus) ||
                   reject(ctx, section, key, "expected a boolean");
        if (xd_str_eq_nocase(key, XD_STR("LunaColor"))) {
            static const char *const variants[] = {
                [XD_LUNA_BLUE] = "NormalColor",
                [XD_LUNA_OLIVE] = "HomeStead",
                [XD_LUNA_SILVER] = "Metallic",
                [XD_LUNA_ENERGY] = "EnergyBlue",
            };
            for (size_t i = XD_LUNA_BLUE; i < XD_ARRAY_LEN(variants); i++) {
                if (xd_str_eq_nocase(value, xd_str_from_cstr(variants[i]))) {
                    s->luna_color = (xd_luna_color)i;
                    return true;
                }
            }
            return reject(ctx, section, key, "expected NormalColor, HomeStead, Metallic or EnergyBlue");
        }
        return reject(ctx, section, key, "unknown key");
    }

    if (xd_str_eq_nocase(section, XD_STR("Colors"))) {
        xd_syscolor c;
        if (!xd_syscolor_from_name(key, &c))
            return reject(ctx, section, key, "unknown color");
        return parse_rgb(value, &s->colors[c]) ||
               reject(ctx, section, key, "expected \"R G B\" with values 0-255");
    }

    if (xd_str_eq_nocase(section, XD_STR("Metrics"))) {
        for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(metric_names[i]))) {
                int64_t v;
                if (!xd_str_parse_i64(value, 0, 256, &v))
                    return reject(ctx, section, key, "expected an integer 0-256");
                s->metrics[i] = (int)v;
                return true;
            }
        }
        return reject(ctx, section, key, "unknown metric");
    }

    if (xd_str_eq_nocase(section, XD_STR("Fonts"))) {
        for (size_t i = 0; i < XD_FONT_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(font_names[i])))
                return parse_font(value, &s->fonts[i]) ||
                       reject(ctx, section, key, "expected \"Family, size[, bold][, italic]\"");
        }
        return reject(ctx, section, key, "unknown font");
    }

    return reject(ctx, section, key, "unknown section");
}

/* Feeds every key of a key file to `fn`, group by group, in file order. */
static bool keyfile_foreach(GKeyFile *kf,
                            bool (*fn)(void *user, xd_str group, xd_str key, xd_str value),
                            void *user) {
    gsize ngroups = 0;
    gchar **groups = g_key_file_get_groups(kf, &ngroups);
    bool ok = true;
    for (gsize g = 0; ok && g < ngroups; g++) {
        gsize nkeys = 0;
        gchar **keys = g_key_file_get_keys(kf, groups[g], &nkeys, nullptr);
        for (gsize k = 0; ok && k < nkeys; k++) {
            /* Unescaped string; the raw value if it has a bad escape. */
            gchar *v = g_key_file_get_string(kf, groups[g], keys[k], nullptr);
            if (v == nullptr)
                v = g_key_file_get_value(kf, groups[g], keys[k], nullptr);
            ok = fn(user, xd_str_from_cstr(groups[g]), xd_str_from_cstr(keys[k]),
                    xd_str_from_cstr(v));
            g_free(v);
        }
        g_strfreev(keys);
    }
    g_strfreev(groups);
    return ok;
}

bool xd_scheme_parse(xd_str text, xd_scheme *scheme, xd_strbuf *err) {
    xd_scheme_init_default(scheme);
    parse_ctx ctx = {.scheme = scheme, .err = err};
    GKeyFile *kf = g_key_file_new();
    GError *gerr = nullptr;
    bool ok = g_key_file_load_from_data(kf, text.ptr, text.len, G_KEY_FILE_NONE, &gerr);
    if (!ok) {
        if (err != nullptr)
            xd_strbuf_appendf(err, "%s", gerr != nullptr ? gerr->message : "not a key file");
        g_clear_error(&gerr);
    } else {
        ok = keyfile_foreach(kf, on_entry, &ctx);
    }
    g_key_file_free(kf);
    if (!ok)
        xd_scheme_init_default(scheme);
    return ok;
}

bool xd_scheme_load(const char *path, xd_scheme *scheme, xd_strbuf *err) {
    xd_strbuf text = {0};
    if (!xd_read_file(path, 1u << 20, &text)) {
        if (err != nullptr)
            xd_strbuf_appendf(err, "cannot read %s", path);
        xd_strbuf_free(&text);
        xd_scheme_init_default(scheme);
        return false;
    }
    bool ok = xd_scheme_parse(xd_strbuf_view(&text), scheme, err);
    xd_strbuf_free(&text);
    return ok;
}
