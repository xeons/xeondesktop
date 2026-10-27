/* SPDX-License-Identifier: MIT
 * Desktop Effects' table and settings (effects.h): effects.ini through
 * GKeyFile, as monitors.ini is, its values checked against the table.
 */
#include "effects.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "../core/alloc.h"
#include "../core/common.h"

static const xd_effect_info *const builtin_table[] = {
#define XD_EFFECT(name) &xd_effect_##name,
#include "effects_list.h"
#undef XD_EFFECT
};

static constexpr char FILE_NAME[] = "effects.ini";
static constexpr char PREFERENCES_GROUP[] = "Preferences";

struct xd_effects_settings {
    const xd_effect_info *const *table;
    size_t count;
    bool *enabled;
    xd_effect_value **values; /* [effect][option] */
    bool warn_conflicts;
};

size_t xd_effects_get_builtin_count(void) {
    return XD_ARRAY_LEN(builtin_table);
}

const xd_effect_info *const *xd_effects_get_builtin_table(void) {
    return builtin_table;
}

const char *xd_effects_get_category_name(xd_effect_category category) {
    switch (category) {
    case XD_EFFECT_CATEGORY_EFFECTS:
        return "Effects";
    case XD_EFFECT_CATEGORY_WINDOW_MANAGEMENT:
        return "Window Management";
    case XD_EFFECT_CATEGORY_DESKTOP:
        return "Desktop";
    case XD_EFFECT_CATEGORY_ACCESSIBILITY:
        return "Accessibility";
    case XD_EFFECT_CATEGORY_UTILITY:
        return "Utility";
    case XD_EFFECT_CATEGORY_COUNT:
        break;
    }
    return "Other";
}

const char *xd_effects_get_option_group(const xd_effect_option *option) {
    return option->group != nullptr ? option->group : "General";
}

static bool has_text(xd_effect_option_type type) {
    return type == XD_EFFECT_OPTION_KEY || type == XD_EFFECT_OPTION_BUTTON ||
           type == XD_EFFECT_OPTION_MATCH;
}

static size_t choice_count(const xd_effect_option *option) {
    size_t count = 0;
    while (option->choice_names != nullptr && option->choice_names[count] != nullptr)
        count++;
    return count;
}

/* ---- values --------------------------------------------------------------------- */

static bool parse_bool(const char *text, bool *out) {
    static const char *const yes[] = {"true", "1", "yes", "on"};
    static const char *const no[] = {"false", "0", "no", "off"};
    for (size_t i = 0; i < XD_ARRAY_LEN(yes); i++) {
        if (g_ascii_strcasecmp(text, yes[i]) == 0) {
            *out = true;
            return true;
        }
        if (g_ascii_strcasecmp(text, no[i]) == 0) {
            *out = false;
            return true;
        }
    }
    return false;
}

static bool parse_color(const char *text, uint32_t *out) {
    size_t length = strlen(text);
    if (text[0] != '#' || (length != 7 && length != 9))
        return false;
    uint32_t color = 0;
    for (size_t i = 1; i < length; i++) {
        int digit = g_ascii_xdigit_value(text[i]);
        if (digit < 0)
            return false;
        color = color << 4 | (uint32_t)digit;
    }
    *out = length == 7 ? color << 8 | 0xffu : color;
    return true;
}

static const struct {
    const char *name;
    const char *label;
    xd_screen_edge edge;
} edge_names[XD_SCREEN_EDGES] = {
    {"TopLeft", "Top left corner", XD_SCREEN_EDGE_TOP_LEFT},
    {"Top", "Top edge", XD_SCREEN_EDGE_TOP},
    {"TopRight", "Top right corner", XD_SCREEN_EDGE_TOP_RIGHT},
    {"Left", "Left edge", XD_SCREEN_EDGE_LEFT},
    {"Right", "Right edge", XD_SCREEN_EDGE_RIGHT},
    {"BottomLeft", "Bottom left corner", XD_SCREEN_EDGE_BOTTOM_LEFT},
    {"Bottom", "Bottom edge", XD_SCREEN_EDGE_BOTTOM},
    {"BottomRight", "Bottom right corner", XD_SCREEN_EDGE_BOTTOM_RIGHT},
};

static bool parse_edges(const char *text, uint32_t *out) {
    uint32_t edges = 0;
    g_auto(GStrv) parts = g_strsplit_set(text, ";,", -1);
    for (size_t i = 0; parts[i] != nullptr; i++) {
        const char *part = g_strstrip(parts[i]);
        if (part[0] == '\0' || g_ascii_strcasecmp(part, "None") == 0)
            continue;
        bool known = false;
        for (size_t j = 0; j < XD_ARRAY_LEN(edge_names); j++) {
            if (g_ascii_strcasecmp(part, edge_names[j].name) == 0) {
                edges |= edge_names[j].edge;
                known = true;
            }
        }
        if (!known)
            return false;
    }
    *out = edges;
    return true;
}

bool xd_effects_parse_binding(const char *text, uint32_t *modifiers, char *key, size_t cap) {
    static const struct {
        const char *name;
        xd_binding_modifier modifier;
    } names[] = {
        {"Shift", XD_BINDING_SHIFT},     {"Control", XD_BINDING_CONTROL},
        {"Ctrl", XD_BINDING_CONTROL},    {"Primary", XD_BINDING_CONTROL},
        {"Alt", XD_BINDING_ALT},         {"Mod1", XD_BINDING_ALT},
        {"Super", XD_BINDING_SUPER},     {"Mod4", XD_BINDING_SUPER},
        {"Meta", XD_BINDING_SUPER},      {"Hyper", XD_BINDING_SUPER},
    };
    uint32_t mods = 0;
    const char *cursor = text;
    while (*cursor == ' ')
        cursor++;
    while (*cursor == '<') {
        const char *close = strchr(cursor, '>');
        if (close == nullptr)
            return false;
        size_t length = (size_t)(close - cursor - 1);
        bool known = false;
        for (size_t i = 0; i < XD_ARRAY_LEN(names); i++) {
            if (strlen(names[i].name) == length &&
                g_ascii_strncasecmp(cursor + 1, names[i].name, length) == 0) {
                mods |= names[i].modifier;
                known = true;
            }
        }
        if (!known)
            return false;
        cursor = close + 1;
    }
    size_t length = strlen(cursor);
    while (length > 0 && cursor[length - 1] == ' ')
        length--;
    if (length == 0 || length >= cap || memchr(cursor, '<', length) != nullptr ||
        memchr(cursor, '>', length) != nullptr || memchr(cursor, ' ', length) != nullptr)
        return false;
    if (g_ascii_strncasecmp(cursor, "Disabled", length) == 0 && length == 8)
        return false;
    memcpy(key, cursor, length);
    key[length] = '\0';
    *modifiers = mods;
    return true;
}

/* "Button1".."Button9": the number, or 0. */
static int button_number(const char *key) {
    if (g_ascii_strncasecmp(key, "Button", 6) != 0 || key[6] < '1' || key[6] > '9' ||
        key[7] != '\0')
        return 0;
    return key[6] - '0';
}

bool xd_effects_parse_value(const xd_effect_option *option, const char *text,
                            xd_effect_value *out) {
    g_autofree char *trimmed = g_strstrip(g_strdup(text != nullptr ? text : ""));
    switch (option->type) {
    case XD_EFFECT_OPTION_BOOL:
        return parse_bool(trimmed, &out->boolean);
    case XD_EFFECT_OPTION_INT: {
        char *end = nullptr;
        errno = 0;
        long long number = g_ascii_strtoll(trimmed, &end, 10);
        if (trimmed[0] == '\0' || *end != '\0' || errno != 0)
            return false;
        double clamped = XD_CLAMP((double)number, option->min, option->max);
        out->integer = (int)clamped;
        return true;
    }
    case XD_EFFECT_OPTION_DOUBLE: {
        char *end = nullptr;
        double number = g_ascii_strtod(trimmed, &end);
        if (trimmed[0] == '\0' || *end != '\0' || !isfinite(number))
            return false;
        out->number = XD_CLAMP(number, option->min, option->max);
        return true;
    }
    case XD_EFFECT_OPTION_COLOR:
        return parse_color(trimmed, &out->color);
    case XD_EFFECT_OPTION_CHOICE:
        for (size_t i = 0; option->choice_names != nullptr && option->choice_names[i] != nullptr;
             i++) {
            if (g_ascii_strcasecmp(trimmed, option->choice_names[i]) == 0) {
                out->choice = (int)i;
                return true;
            }
        }
        return false;
    case XD_EFFECT_OPTION_EDGE:
        return parse_edges(trimmed, &out->edges);
    case XD_EFFECT_OPTION_KEY:
    case XD_EFFECT_OPTION_BUTTON: {
        if (trimmed[0] != '\0') {
            uint32_t modifiers = 0;
            char key[64];
            if (!xd_effects_parse_binding(trimmed, &modifiers, key, sizeof(key)))
                return false;
            if ((option->type == XD_EFFECT_OPTION_BUTTON) != (button_number(key) > 0))
                return false;
        }
        out->text = g_steal_pointer(&trimmed);
        return true;
    }
    case XD_EFFECT_OPTION_MATCH:
        out->text = g_steal_pointer(&trimmed);
        return true;
    }
    return false;
}

char *xd_effects_format_value(const xd_effect_option *option, const xd_effect_value *value) {
    switch (option->type) {
    case XD_EFFECT_OPTION_BOOL:
        return g_strdup(value->boolean ? "true" : "false");
    case XD_EFFECT_OPTION_INT:
        return g_strdup_printf("%d", value->integer);
    case XD_EFFECT_OPTION_DOUBLE: {
        char buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_autofree char *format = g_strdup_printf("%%.%df", XD_CLAMP(option->digits, 0, 6));
        return g_strdup(g_ascii_formatd(buf, sizeof(buf), format, value->number));
    }
    case XD_EFFECT_OPTION_COLOR:
        return g_strdup_printf("#%08X", value->color);
    case XD_EFFECT_OPTION_CHOICE: {
        size_t count = choice_count(option);
        size_t index = value->choice >= 0 && (size_t)value->choice < count ? (size_t)value->choice
                                                                            : 0;
        return g_strdup(count > 0 ? option->choice_names[index] : "");
    }
    case XD_EFFECT_OPTION_EDGE: {
        GString *text = g_string_new(nullptr);
        for (size_t i = 0; i < XD_ARRAY_LEN(edge_names); i++) {
            if (value->edges & edge_names[i].edge) {
                if (text->len > 0)
                    g_string_append_c(text, ';');
                g_string_append(text, edge_names[i].name);
            }
        }
        return g_string_free(text, false);
    }
    case XD_EFFECT_OPTION_KEY:
    case XD_EFFECT_OPTION_BUTTON:
    case XD_EFFECT_OPTION_MATCH:
        return g_strdup(value->text != nullptr ? value->text : "");
    }
    return g_strdup("");
}

static void free_value(const xd_effect_option *option, xd_effect_value *value) {
    if (has_text(option->type)) {
        g_free(value->text);
        value->text = nullptr;
    }
}

/* The option's default; a table whose default doesn't parse is a bug. */
static xd_effect_value default_value(const xd_effect_option *option) {
    xd_effect_value value = {0};
    bool parsed = xd_effects_parse_value(option, option->default_value, &value);
    XD_ASSERT(parsed);
    return value;
}

static bool values_equal(const xd_effect_option *option, const xd_effect_value *left,
                         const xd_effect_value *right) {
    switch (option->type) {
    case XD_EFFECT_OPTION_BOOL:
        return left->boolean == right->boolean;
    case XD_EFFECT_OPTION_INT:
        return left->integer == right->integer;
    case XD_EFFECT_OPTION_DOUBLE: {
        /* Equal as the file and the page show them. */
        g_autofree char *left_text = xd_effects_format_value(option, left);
        g_autofree char *right_text = xd_effects_format_value(option, right);
        return strcmp(left_text, right_text) == 0;
    }
    case XD_EFFECT_OPTION_COLOR:
        return left->color == right->color;
    case XD_EFFECT_OPTION_CHOICE:
        return left->choice == right->choice;
    case XD_EFFECT_OPTION_EDGE:
        return left->edges == right->edges;
    case XD_EFFECT_OPTION_KEY:
    case XD_EFFECT_OPTION_BUTTON:
    case XD_EFFECT_OPTION_MATCH:
        return strcmp(left->text, right->text) == 0;
    }
    return false;
}

/* ---- settings ------------------------------------------------------------------- */

xd_effects_settings *xd_effects_create_settings_for(const xd_effect_info *const *table,
                                                    size_t count) {
    xd_effects_settings *settings = XD_NEW(xd_effects_settings);
    settings->table = table;
    settings->count = count;
    settings->enabled = XD_NEW_ARRAY(bool, XD_MAX(count, 1));
    settings->values = XD_NEW_ARRAY(xd_effect_value *, XD_MAX(count, 1));
    settings->warn_conflicts = true;
    for (size_t effect = 0; effect < count; effect++) {
        const xd_effect_info *info = table[effect];
        settings->values[effect] = XD_NEW_ARRAY(xd_effect_value, XD_MAX(info->option_count, 1));
        for (size_t option = 0; option < info->option_count; option++)
            settings->values[effect][option] = default_value(&info->options[option]);
    }
    return settings;
}

xd_effects_settings *xd_effects_create_settings(void) {
    return xd_effects_create_settings_for(builtin_table, XD_ARRAY_LEN(builtin_table));
}

void xd_effects_destroy_settings(xd_effects_settings *settings) {
    if (settings == nullptr)
        return;
    for (size_t effect = 0; effect < settings->count; effect++) {
        const xd_effect_info *info = settings->table[effect];
        for (size_t option = 0; option < info->option_count; option++)
            free_value(&info->options[option], &settings->values[effect][option]);
        xd_free(settings->values[effect]);
    }
    xd_free(settings->values);
    xd_free(settings->enabled);
    xd_free(settings);
}

size_t xd_effects_get_count(const xd_effects_settings *settings) {
    return settings->count;
}

const xd_effect_info *xd_effects_get_info(const xd_effects_settings *settings, size_t effect) {
    XD_ASSERT(effect < settings->count);
    return settings->table[effect];
}

bool xd_effects_find(const xd_effects_settings *settings, const char *id, size_t *effect) {
    for (size_t i = 0; i < settings->count; i++) {
        if (strcmp(settings->table[i]->id, id) == 0) {
            *effect = i;
            return true;
        }
    }
    return false;
}

bool xd_effects_find_option(const xd_effect_info *info, const char *key, size_t *option) {
    for (size_t i = 0; i < info->option_count; i++) {
        if (strcmp(info->options[i].key, key) == 0) {
            *option = i;
            return true;
        }
    }
    return false;
}

bool xd_effects_is_enabled(const xd_effects_settings *settings, size_t effect) {
    XD_ASSERT(effect < settings->count);
    return settings->enabled[effect];
}

void xd_effects_set_enabled(xd_effects_settings *settings, size_t effect, bool enabled) {
    XD_ASSERT(effect < settings->count);
    settings->enabled[effect] = enabled;
}

const xd_effect_value *xd_effects_get_value(const xd_effects_settings *settings, size_t effect,
                                            size_t option) {
    XD_ASSERT(effect < settings->count && option < settings->table[effect]->option_count);
    return &settings->values[effect][option];
}

/* Takes `value` (a text value's string) into the settings. */
static bool store_value(xd_effects_settings *settings, size_t effect, size_t option,
                        xd_effect_value value) {
    const xd_effect_option *info = &settings->table[effect]->options[option];
    xd_effect_value *slot = &settings->values[effect][option];
    if (values_equal(info, slot, &value)) {
        free_value(info, &value);
        return false;
    }
    free_value(info, slot);
    *slot = value;
    return true;
}

bool xd_effects_set_value(xd_effects_settings *settings, size_t effect, size_t option,
                          const xd_effect_value *value) {
    XD_ASSERT(effect < settings->count && option < settings->table[effect]->option_count);
    const xd_effect_option *info = &settings->table[effect]->options[option];
    /* Through the file's syntax, which clamps and checks it as a loaded
     * value is. */
    xd_effect_value parsed = {0};
    if (info->type == XD_EFFECT_OPTION_INT) {
        parsed.integer = (int)XD_CLAMP((double)value->integer, info->min, info->max);
    } else if (info->type == XD_EFFECT_OPTION_DOUBLE) {
        if (!isfinite(value->number))
            return false;
        parsed.number = XD_CLAMP(value->number, info->min, info->max);
    } else {
        g_autofree char *text = xd_effects_format_value(info, value);
        if (!xd_effects_parse_value(info, text, &parsed))
            return false;
    }
    return store_value(settings, effect, option, parsed);
}

bool xd_effects_is_default(const xd_effects_settings *settings, size_t effect, size_t option) {
    const xd_effect_option *info = &settings->table[effect]->options[option];
    xd_effect_value fallback = default_value(info);
    bool same = values_equal(info, &settings->values[effect][option], &fallback);
    free_value(info, &fallback);
    return same;
}

void xd_effects_reset_value(xd_effects_settings *settings, size_t effect, size_t option) {
    XD_ASSERT(effect < settings->count && option < settings->table[effect]->option_count);
    store_value(settings, effect, option, default_value(&settings->table[effect]->options[option]));
}

void xd_effects_reset_all(xd_effects_settings *settings) {
    for (size_t effect = 0; effect < settings->count; effect++) {
        settings->enabled[effect] = false;
        for (size_t option = 0; option < settings->table[effect]->option_count; option++)
            xd_effects_reset_value(settings, effect, option);
    }
}

bool xd_effects_get_warn_conflicts(const xd_effects_settings *settings) {
    return settings->warn_conflicts;
}

void xd_effects_set_warn_conflicts(xd_effects_settings *settings, bool warn) {
    settings->warn_conflicts = warn;
}

static const xd_effect_value *value_by_key(const xd_effects_settings *settings, size_t effect,
                                           const char *key, xd_effect_option_type type) {
    size_t option = 0;
    bool found = xd_effects_find_option(xd_effects_get_info(settings, effect), key, &option);
    XD_ASSERT(found && settings->table[effect]->options[option].type == type);
    return &settings->values[effect][option];
}

bool xd_effects_get_bool(const xd_effects_settings *settings, size_t effect, const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_BOOL)->boolean;
}

int xd_effects_get_int(const xd_effects_settings *settings, size_t effect, const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_INT)->integer;
}

double xd_effects_get_double(const xd_effects_settings *settings, size_t effect,
                             const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_DOUBLE)->number;
}

uint32_t xd_effects_get_color(const xd_effects_settings *settings, size_t effect,
                              const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_COLOR)->color;
}

int xd_effects_get_choice(const xd_effects_settings *settings, size_t effect, const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_CHOICE)->choice;
}

uint32_t xd_effects_get_edges(const xd_effects_settings *settings, size_t effect,
                              const char *key) {
    return value_by_key(settings, effect, key, XD_EFFECT_OPTION_EDGE)->edges;
}

const char *xd_effects_get_text(const xd_effects_settings *settings, size_t effect,
                                const char *key) {
    size_t option = 0;
    bool found = xd_effects_find_option(xd_effects_get_info(settings, effect), key, &option);
    XD_ASSERT(found && has_text(settings->table[effect]->options[option].type));
    return settings->values[effect][option].text;
}

/* ---- the file ------------------------------------------------------------------- */

char *xd_effects_get_default_path(void) {
    const char *config = getenv("XDG_CONFIG_HOME");
    if (config != nullptr && config[0] == '/')
        return g_build_filename(config, "xeondesktop", FILE_NAME, nullptr);
    const char *home = getenv("HOME");
    return g_build_filename(home != nullptr ? home : "/tmp", ".config", "xeondesktop", FILE_NAME,
                            nullptr);
}

void xd_effects_load_file(xd_effects_settings *settings, const char *path) {
    g_autoptr(GKeyFile) file = g_key_file_new();
    bool loaded = g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr);
    for (size_t effect = 0; effect < settings->count; effect++) {
        const xd_effect_info *info = settings->table[effect];
        settings->enabled[effect] = false;
        for (size_t option = 0; option < info->option_count; option++)
            xd_effects_reset_value(settings, effect, option);
        if (!loaded || !g_key_file_has_group(file, info->id))
            continue;
        g_autofree char *enabled = g_key_file_get_string(file, info->id, "Enabled", nullptr);
        bool on = false;
        if (enabled != nullptr && parse_bool(g_strstrip(enabled), &on))
            settings->enabled[effect] = on;
        for (size_t option = 0; option < info->option_count; option++) {
            g_autofree char *text =
                g_key_file_get_string(file, info->id, info->options[option].key, nullptr);
            xd_effect_value value = {0};
            if (text != nullptr && xd_effects_parse_value(&info->options[option], text, &value))
                store_value(settings, effect, option, value);
        }
    }
    settings->warn_conflicts = true;
    g_autofree char *warn = loaded ? g_key_file_get_string(file, PREFERENCES_GROUP,
                                                           "WarnConflicts", nullptr)
                                   : nullptr;
    bool warn_conflicts = true;
    if (warn != nullptr && parse_bool(g_strstrip(warn), &warn_conflicts))
        settings->warn_conflicts = warn_conflicts;
}

void xd_effects_load(xd_effects_settings *settings) {
    g_autofree char *path = xd_effects_get_default_path();
    xd_effects_load_file(settings, path);
}

bool xd_effects_save_file(const xd_effects_settings *settings, const char *path) {
    g_autoptr(GKeyFile) file = g_key_file_new();
    /* What's there now, so groups and keys we don't know stay. */
    g_key_file_load_from_file(file, path, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS,
                              nullptr);
    for (size_t effect = 0; effect < settings->count; effect++) {
        const xd_effect_info *info = settings->table[effect];
        bool any_set = settings->enabled[effect];
        for (size_t option = 0; option < info->option_count; option++)
            any_set = any_set || !xd_effects_is_default(settings, effect, option);
        if (!any_set && !g_key_file_has_group(file, info->id))
            continue;
        g_key_file_set_boolean(file, info->id, "Enabled", settings->enabled[effect]);
        for (size_t option = 0; option < info->option_count; option++) {
            const char *key = info->options[option].key;
            if (xd_effects_is_default(settings, effect, option)) {
                g_key_file_remove_key(file, info->id, key, nullptr);
                continue;
            }
            g_autofree char *text =
                xd_effects_format_value(&info->options[option], &settings->values[effect][option]);
            g_key_file_set_string(file, info->id, key, text);
        }
    }
    if (!settings->warn_conflicts)
        g_key_file_set_boolean(file, PREFERENCES_GROUP, "WarnConflicts", false);
    else if (g_key_file_has_group(file, PREFERENCES_GROUP))
        g_key_file_remove_key(file, PREFERENCES_GROUP, "WarnConflicts", nullptr);

    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_autoptr(GError) error = nullptr;
    /* g_file_set_contents underneath: written beside it, then renamed. */
    if (!g_key_file_save_to_file(file, path, &error)) {
        fprintf(stderr, "xeondesktop: cannot save %s: %s\n", path, error->message);
        return false;
    }
    return true;
}

bool xd_effects_save(const xd_effects_settings *settings) {
    g_autofree char *path = xd_effects_get_default_path();
    return xd_effects_save_file(settings, path);
}

/* ---- conflicts ------------------------------------------------------------------ */

static bool is_binding(xd_effect_option_type type) {
    return type == XD_EFFECT_OPTION_KEY || type == XD_EFFECT_OPTION_BUTTON ||
           type == XD_EFFECT_OPTION_EDGE;
}

bool xd_effects_bindings_overlap(xd_effect_option_type type, const xd_effect_value *left,
                                 const xd_effect_value *right) {
    if (type == XD_EFFECT_OPTION_EDGE)
        return (left->edges & right->edges) != 0;
    if (type != XD_EFFECT_OPTION_KEY && type != XD_EFFECT_OPTION_BUTTON)
        return false;
    uint32_t left_mods = 0, right_mods = 0;
    char left_key[64], right_key[64];
    if (!xd_effects_parse_binding(left->text, &left_mods, left_key, sizeof(left_key)) ||
        !xd_effects_parse_binding(right->text, &right_mods, right_key, sizeof(right_key)))
        return false;
    return left_mods == right_mods && g_ascii_strcasecmp(left_key, right_key) == 0;
}

size_t xd_effects_find_conflicts(const xd_effects_settings *settings, size_t effect,
                                 xd_effect_conflict *out, size_t cap) {
    const xd_effect_info *info = xd_effects_get_info(settings, effect);
    size_t found = 0;
    for (size_t option = 0; option < info->option_count; option++) {
        xd_effect_option_type type = info->options[option].type;
        if (!is_binding(type))
            continue;
        for (size_t other = 0; other < settings->count; other++) {
            if (other == effect || !settings->enabled[other])
                continue;
            const xd_effect_info *other_info = settings->table[other];
            for (size_t other_option = 0; other_option < other_info->option_count;
                 other_option++) {
                if (other_info->options[other_option].type != type ||
                    !xd_effects_bindings_overlap(type, &settings->values[effect][option],
                                                 &settings->values[other][other_option]))
                    continue;
                if (found < cap)
                    out[found] = (xd_effect_conflict){option, other, other_option};
                found++;
            }
        }
    }
    return found;
}

static const char *button_label(int number) {
    switch (number) {
    case 1:
        return "Left button";
    case 2:
        return "Middle button";
    case 3:
        return "Right button";
    case 4:
        return "Wheel up";
    case 5:
        return "Wheel down";
    default:
        return nullptr;
    }
}

char *xd_effects_describe_value(const xd_effect_option *option, const xd_effect_value *value) {
    if (option->type == XD_EFFECT_OPTION_EDGE) {
        if (value->edges == 0)
            return g_strdup("None");
        GString *text = g_string_new(nullptr);
        for (size_t i = 0; i < XD_ARRAY_LEN(edge_names); i++) {
            if (value->edges & edge_names[i].edge) {
                if (text->len > 0)
                    g_string_append(text, ", ");
                g_string_append(text, edge_names[i].label);
            }
        }
        return g_string_free(text, false);
    }
    if (option->type != XD_EFFECT_OPTION_KEY && option->type != XD_EFFECT_OPTION_BUTTON)
        return xd_effects_format_value(option, value);
    uint32_t modifiers = 0;
    char key[64];
    if (!xd_effects_parse_binding(value->text, &modifiers, key, sizeof(key)))
        return g_strdup("None");
    /* In Windows' order and words: Ctrl+Alt+Shift, and Win for the
     * Windows key. */
    GString *text = g_string_new(nullptr);
    if (modifiers & XD_BINDING_CONTROL)
        g_string_append(text, "Ctrl+");
    if (modifiers & XD_BINDING_ALT)
        g_string_append(text, "Alt+");
    if (modifiers & XD_BINDING_SHIFT)
        g_string_append(text, "Shift+");
    if (modifiers & XD_BINDING_SUPER)
        g_string_append(text, "Win+");
    int button = button_number(key);
    const char *label = button > 0 ? button_label(button) : nullptr;
    if (label != nullptr)
        g_string_append(text, label);
    else if (button > 0)
        g_string_append_printf(text, "Button %d", button);
    else if (strlen(key) == 1)
        g_string_append_c(text, g_ascii_toupper(key[0]));
    else
        g_string_append(text, key);
    return g_string_free(text, false);
}

/* ---- window matches ------------------------------------------------------------- */

static bool condition_matches(const char *condition, const char *app_id, const char *title,
                              bool is_dialog) {
    bool negate = false;
    while (*condition == '!' || *condition == ' ') {
        negate = negate != (*condition == '!');
        condition++;
    }
    bool result = false;
    const char *equals = strchr(condition, '=');
    if (g_ascii_strcasecmp(condition, "any") == 0) {
        result = true;
    } else if (equals != nullptr) {
        g_autofree char *key = g_strstrip(g_strndup(condition, (size_t)(equals - condition)));
        g_autofree char *pattern = g_strstrip(g_strdup(equals + 1));
        if (g_ascii_strcasecmp(key, "app_id") == 0 || g_ascii_strcasecmp(key, "class") == 0)
            result = g_pattern_match_simple(pattern, app_id != nullptr ? app_id : "");
        else if (g_ascii_strcasecmp(key, "title") == 0)
            result = g_pattern_match_simple(pattern, title != nullptr ? title : "");
        else if (g_ascii_strcasecmp(key, "type") == 0)
            result = g_ascii_strcasecmp(pattern, is_dialog ? "dialog" : "normal") == 0;
    }
    return negate ? !result : result;
}

bool xd_effects_match_window(const char *match, const char *app_id, const char *title,
                             bool is_dialog) {
    g_autofree char *whole = g_strstrip(g_strdup(match != nullptr ? match : ""));
    if (whole[0] == '\0')
        return true;
    g_auto(GStrv) terms = g_strsplit(whole, "|", -1);
    for (size_t i = 0; terms[i] != nullptr; i++) {
        g_auto(GStrv) conditions = g_strsplit(terms[i], "&", -1);
        bool all = true, any_condition = false;
        for (size_t j = 0; conditions[j] != nullptr && all; j++) {
            const char *condition = g_strstrip(conditions[j]);
            if (condition[0] == '\0')
                continue;
            any_condition = true;
            all = condition_matches(condition, app_id, title, is_dialog);
        }
        if (all && any_condition)
            return true;
    }
    return false;
}

/* ---- watching ------------------------------------------------------------------- */

int xd_effects_watch(void) {
    g_autofree char *path = xd_effects_get_default_path();
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    /* The folder: an atomic save replaces the file, which a watch on the
     * file itself would lose. */
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd >= 0 && inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

bool xd_effects_has_changed(int fd) {
    bool changed = false;
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t bytes_read = read(fd, buf, sizeof(buf));
        if (bytes_read <= 0)
            break;
        size_t offset = 0;
        while (offset + sizeof(struct inotify_event) <= (size_t)bytes_read) {
            struct inotify_event event;
            memcpy(&event, buf + offset, sizeof(event));
            size_t total = sizeof(event) + event.len;
            if (total > (size_t)bytes_read - offset)
                break;
            const char *name = buf + offset + sizeof(event);
            if (event.len > 0 && strnlen(name, event.len) == sizeof(FILE_NAME) - 1 &&
                memcmp(name, FILE_NAME, sizeof(FILE_NAME) - 1) == 0)
                changed = true;
            offset += total;
        }
    }
    return changed;
}

/* ---- the support file ----------------------------------------------------------- */

static char *support_path(const char *display) {
    g_autofree char *safe = g_strdup(display != nullptr && display[0] != '\0' ? display : "none");
    g_strcanon(safe, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-", '_');
    g_autofree char *name = g_strdup_printf("effects-%s.ini", safe);
    /* The variable as it is now: GLib's own answer is the first one it
     * saw. */
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr || runtime[0] != '/')
        runtime = g_get_user_runtime_dir();
    return g_build_filename(runtime, "xeondesktop", name, nullptr);
}

bool xd_effects_write_support(const char *display, const xd_effects_support *support) {
    g_autofree char *path = support_path(display);
    g_autofree char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_autoptr(GKeyFile) file = g_key_file_new();
    g_key_file_set_boolean(file, "Support", "OpenGL", support->opengl);
    g_key_file_set_string(file, "Support", "Reason", support->reason);
    g_key_file_set_string(file, "Support", "Renderer", support->renderer);
    g_autoptr(GError) error = nullptr;
    if (!g_key_file_save_to_file(file, path, &error)) {
        fprintf(stderr, "xeondesktop: cannot save %s: %s\n", path, error->message);
        return false;
    }
    return true;
}

bool xd_effects_read_support(const char *display, xd_effects_support *support) {
    *support = (xd_effects_support){0};
    g_autofree char *path = support_path(display);
    g_autoptr(GKeyFile) file = g_key_file_new();
    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, nullptr))
        return false;
    support->opengl = g_key_file_get_boolean(file, "Support", "OpenGL", nullptr);
    g_autofree char *reason = g_key_file_get_string(file, "Support", "Reason", nullptr);
    g_autofree char *renderer = g_key_file_get_string(file, "Support", "Renderer", nullptr);
    g_strlcpy(support->reason, reason != nullptr ? reason : "", sizeof(support->reason));
    g_strlcpy(support->renderer, renderer != nullptr ? renderer : "", sizeof(support->renderer));
    return true;
}

void xd_effects_remove_support(const char *display) {
    g_autofree char *path = support_path(display);
    g_unlink(path);
}
