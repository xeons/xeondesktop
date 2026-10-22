/* SPDX-License-Identifier: MIT */
#include "scheme.h"
#include <glib.h>
#include <math.h>

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
    [XD_FONT_TOOLTIP] = "StatusFont", [XD_FONT_SMCAPTION] = "SmCaptionFont",
};

static_assert(XD_COLOR_COUNT <= 32 && XD_METRIC_COUNT <= 32 && XD_FONT_COUNT <= 32,
              "xd_scheme_custom's masks hold a bit each");

const char *xd_syscolor_name(xd_syscolor color_index) {
    XD_ASSERT((unsigned)color_index < XD_COLOR_COUNT);
    return color_names[color_index];
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

static void set_font(xd_font_spec *font, const char *family, double pt, bool bold) {
    snprintf(font->family, sizeof(font->family), "%s", family);
    font->size_pt = pt;
    font->bold = bold;
    font->italic = false;
}

void xd_scheme_init_default(xd_scheme *scheme) {
    *scheme = (xd_scheme){0};
    snprintf(scheme->name, sizeof(scheme->name), "%s", "Windows Standard");

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
    memcpy(scheme->colors, standard, sizeof(standard));
    memcpy(scheme->metrics, metric_defaults, sizeof(metric_defaults));

    /* Tahoma isn't shipped on Linux; fontconfig falls back through the list. */
    const char *ui = XD_FONT_FAMILY_TAHOMA;
    set_font(&scheme->fonts[XD_FONT_CAPTION], ui, 8, true);
    set_font(&scheme->fonts[XD_FONT_MENU], ui, 8, false);
    set_font(&scheme->fonts[XD_FONT_MESSAGE], ui, 8, false);
    set_font(&scheme->fonts[XD_FONT_ICON], ui, 8, false);
    set_font(&scheme->fonts[XD_FONT_TOOLTIP], ui, 8, false);
    set_font(&scheme->fonts[XD_FONT_SMCAPTION], ui, 8, true);
    scheme->gradient_captions = true;
    scheme->glass_color = XD_GLASS_DEFAULT;
}

static bool parse_rgb(xd_str text, xd_rgb *out) {
    int64_t parts[3];
    size_t count = 0;
    xd_str rest = xd_str_trim(text);
    while (rest.len > 0) {
        if (count == 3)
            return false;
        size_t i = 0;
        while (i < rest.len && rest.ptr[i] != ' ' && rest.ptr[i] != ',')
            i++;
        if (!xd_str_parse_i64(xd_str_sub(rest, 0, i), 0, 255, &parts[count]))
            return false;
        count++;
        while (i < rest.len && (rest.ptr[i] == ' ' || rest.ptr[i] == ','))
            i++;
        rest = xd_str_sub(rest, i, rest.len);
    }
    if (count != 3)
        return false;
    *out = (xd_rgb){(uint8_t)parts[0], (uint8_t)parts[1], (uint8_t)parts[2]};
    return true;
}

static bool parse_bool(xd_str text, bool *out) {
    if (xd_str_eq_nocase(text, XD_STR("1")) || xd_str_eq_nocase(text, XD_STR("true")) ||
        xd_str_eq_nocase(text, XD_STR("yes"))) {
        *out = true;
        return true;
    }
    if (xd_str_eq_nocase(text, XD_STR("0")) || xd_str_eq_nocase(text, XD_STR("false")) ||
        xd_str_eq_nocase(text, XD_STR("no"))) {
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
static bool parse_font(xd_str text, xd_font_spec *out) {
    xd_font_spec font = {0};
    xd_str rest = xd_str_trim(text);

    xd_str tok = pop_last_token(&rest);
    for (int flags = 0; flags < 2; flags++) {
        if (xd_str_eq_nocase(tok, XD_STR("bold")) && !font.bold)
            font.bold = true;
        else if (xd_str_eq_nocase(tok, XD_STR("italic")) && !font.italic)
            font.italic = true;
        else
            break;
        tok = pop_last_token(&rest);
    }
    if (!xd_str_parse_double(tok, &font.size_pt) || font.size_pt < 4 || font.size_pt > 72)
        return false;

    xd_str family = xd_str_trim(rest);
    if (family.len == 0 || family.len >= sizeof(font.family))
        return false;
    memcpy(font.family, family.ptr, family.len);
    *out = font;
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
    xd_scheme *scheme = ctx->scheme;

    if (xd_str_eq_nocase(section, XD_STR("Scheme"))) {
        if (xd_str_eq_nocase(key, XD_STR("Name"))) {
            if (value.len == 0 || value.len >= sizeof(scheme->name))
                return reject(ctx, section, key, "name must be 1-63 bytes");
            memset(scheme->name, 0, sizeof(scheme->name));
            memcpy(scheme->name, value.ptr, value.len);
            return true;
        }
        if (xd_str_eq_nocase(key, XD_STR("GradientCaptions")))
            return parse_bool(value, &scheme->gradient_captions) ||
                   reject(ctx, section, key, "expected a boolean");
        if (xd_str_eq_nocase(key, XD_STR("HighContrast")))
            return parse_bool(value, &scheme->high_contrast) ||
                   reject(ctx, section, key, "expected a boolean");
        if (xd_str_eq_nocase(key, XD_STR("FlatMenus")))
            return parse_bool(value, &scheme->flat_menus) ||
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
                    scheme->luna_color = (xd_luna_color)i;
                    return true;
                }
            }
            return reject(ctx, section, key, "expected NormalColor, HomeStead, Metallic or EnergyBlue");
        }
        return reject(ctx, section, key, "unknown key");
    }

    if (xd_str_eq_nocase(section, XD_STR("Colors"))) {
        xd_syscolor color_index;
        if (!xd_syscolor_from_name(key, &color_index))
            return reject(ctx, section, key, "unknown color");
        return parse_rgb(value, &scheme->colors[color_index]) ||
               reject(ctx, section, key, "expected \"R G B\" with values 0-255");
    }

    if (xd_str_eq_nocase(section, XD_STR("Metrics"))) {
        for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(metric_names[i]))) {
                int64_t number;
                if (!xd_str_parse_i64(value, 0, 256, &number))
                    return reject(ctx, section, key, "expected an integer 0-256");
                scheme->metrics[i] = (int)number;
                return true;
            }
        }
        return reject(ctx, section, key, "unknown metric");
    }

    if (xd_str_eq_nocase(section, XD_STR("Fonts"))) {
        for (size_t i = 0; i < XD_FONT_COUNT; i++) {
            if (xd_str_eq_nocase(key, xd_str_from_cstr(font_names[i])))
                return parse_font(value, &scheme->fonts[i]) ||
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
    for (gsize group_index = 0; ok && group_index < ngroups; group_index++) {
        gsize nkeys = 0;
        gchar **keys = g_key_file_get_keys(kf, groups[group_index], &nkeys, nullptr);
        for (gsize k = 0; ok && k < nkeys; k++) {
            /* Unescaped string; the raw value if it has a bad escape. */
            gchar *value_text = g_key_file_get_string(kf, groups[group_index], keys[k], nullptr);
            if (value_text == nullptr)
                value_text = g_key_file_get_value(kf, groups[group_index], keys[k], nullptr);
            ok = fn(user, xd_str_from_cstr(groups[group_index]), xd_str_from_cstr(keys[k]),
                    xd_str_from_cstr(value_text));
            g_free(value_text);
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

/* ---- custom schemes ---------------------------------------------------------- */

void xd_scheme_apply_custom(xd_scheme *scheme, const xd_scheme_custom *scheme_custom) {
    for (size_t i = 0; i < XD_COLOR_COUNT; i++) {
        if (scheme_custom->color_mask & (1u << i))
            scheme->colors[i] = scheme_custom->colors[i];
    }
    for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
        if (scheme_custom->metric_mask & (1u << i))
            scheme->metrics[i] = scheme_custom->metrics[i];
    }
    for (size_t i = 0; i < XD_FONT_COUNT; i++) {
        if (scheme_custom->font_mask & (1u << i))
            scheme->fonts[i] = scheme_custom->fonts[i];
    }
}

bool xd_scheme_custom_set(xd_scheme_custom *scheme_custom, xd_str key, xd_str value) {
    xd_syscolor color;
    if (xd_syscolor_from_name(key, &color)) {
        if (!parse_rgb(value, &scheme_custom->colors[color]))
            return false;
        scheme_custom->color_mask |= 1u << color;
        return true;
    }
    for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
        if (xd_str_eq_nocase(key, xd_str_from_cstr(metric_names[i]))) {
            int64_t number;
            if (!xd_str_parse_i64(value, 0, 256, &number))
                return false;
            scheme_custom->metrics[i] = (int)number;
            scheme_custom->metric_mask |= 1u << i;
            return true;
        }
    }
    for (size_t i = 0; i < XD_FONT_COUNT; i++) {
        if (xd_str_eq_nocase(key, xd_str_from_cstr(font_names[i]))) {
            if (!parse_font(value, &scheme_custom->fonts[i]))
                return false;
            scheme_custom->font_mask |= 1u << i;
            return true;
        }
    }
    return false;
}

void xd_font_spec_format(const xd_font_spec *font, char *out, size_t cap) {
    /* Not %g: the locale could make the size "8,5", which reads back as
     * another token. */
    char size[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(size, sizeof(size), "%g", font->size_pt);
    snprintf(out, cap, "%s, %s%s%s", font->family, size, font->bold ? ", bold" : "",
             font->italic ? ", italic" : "");
}

void xd_scheme_custom_foreach(const xd_scheme_custom *scheme_custom,
                              void (*fn)(void *user, const char *key, const char *value),
                              void *user) {
    char buf[256];
    for (size_t i = 0; i < XD_COLOR_COUNT; i++) {
        if (scheme_custom->color_mask & (1u << i)) {
            snprintf(buf, sizeof(buf), "%u %u %u", scheme_custom->colors[i].r,
                     scheme_custom->colors[i].g, scheme_custom->colors[i].b);
            fn(user, color_names[i], buf);
        }
    }
    for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
        if (scheme_custom->metric_mask & (1u << i)) {
            snprintf(buf, sizeof(buf), "%d", scheme_custom->metrics[i]);
            fn(user, metric_names[i], buf);
        }
    }
    for (size_t i = 0; i < XD_FONT_COUNT; i++) {
        if (scheme_custom->font_mask & (1u << i)) {
            xd_font_spec_format(&scheme_custom->fonts[i], buf, sizeof(buf));
            fn(user, font_names[i], buf);
        }
    }
}

static bool same_font(const xd_font_spec *left, const xd_font_spec *right) {
    return strcmp(left->family, right->family) == 0 &&
           fabs(left->size_pt - right->size_pt) < 0.01 && left->bold == right->bold &&
           left->italic == right->italic;
}

void xd_scheme_custom_trim(xd_scheme_custom *scheme_custom, const xd_scheme *base) {
    for (size_t i = 0; i < XD_COLOR_COUNT; i++) {
        const xd_rgb *custom_color = &scheme_custom->colors[i], *base_color = &base->colors[i];
        if (custom_color->r == base_color->r && custom_color->g == base_color->g &&
            custom_color->b == base_color->b)
            scheme_custom->color_mask &= ~(1u << i);
    }
    for (size_t i = 0; i < XD_METRIC_COUNT; i++) {
        if (scheme_custom->metrics[i] == base->metrics[i])
            scheme_custom->metric_mask &= ~(1u << i);
    }
    for (size_t i = 0; i < XD_FONT_COUNT; i++) {
        if (same_font(&scheme_custom->fonts[i], &base->fonts[i]))
            scheme_custom->font_mask &= ~(1u << i);
    }
}
