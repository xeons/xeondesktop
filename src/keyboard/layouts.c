/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <string.h>

#include "layouts.h"

size_t xd_kbd_layouts_parse(const char *layouts, const char *variants,
                            xd_kbd_layout out[XD_KBD_LAYOUTS_MAX]) {
    gchar **names = g_strsplit(layouts != nullptr ? layouts : "", ",", -1);
    gchar **vars = g_strsplit(variants != nullptr ? variants : "", ",", -1);
    size_t nvars = g_strv_length(vars);
    size_t count = 0;
    for (size_t i = 0; names[i] != nullptr && count < XD_KBD_LAYOUTS_MAX; i++) {
        const char *name = g_strstrip(names[i]);
        if (name[0] == '\0')
            continue;
        out[count] = (xd_kbd_layout){0};
        g_strlcpy(out[count].name, name, sizeof(out[count].name));
        if (i < nvars)
            g_strlcpy(out[count].variant, g_strstrip(vars[i]), sizeof(out[count].variant));
        count++;
    }
    g_strfreev(names);
    g_strfreev(vars);
    return count;
}

bool xd_kbd_layouts_format(const xd_kbd_layout *layout_list, size_t count, char *layouts,
                           size_t lcap, char *variants, size_t vcap) {
    GString *names = g_string_new(nullptr);
    GString *vars = g_string_new(nullptr);
    bool any_variant = false;
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            g_string_append_c(names, ',');
            g_string_append_c(vars, ',');
        }
        g_string_append(names, layout_list[i].name);
        g_string_append(vars, layout_list[i].variant);
        any_variant = any_variant || layout_list[i].variant[0] != '\0';
    }
    if (!any_variant)
        g_string_truncate(vars, 0);
    bool ok = names->len < lcap && vars->len < vcap;
    g_strlcpy(layouts, ok ? names->str : "", lcap);
    g_strlcpy(variants, ok ? vars->str : "", vcap);
    g_string_free(names, TRUE);
    g_string_free(vars, TRUE);
    return ok;
}

static bool in_group(const char *option, const char *group) {
    size_t count = strlen(group);
    return strncmp(option, group, count) == 0 && option[count] == ':';
}

/* Rebuilds the list, keeping what `keep` says and appending `add`. */
static char *filter(const char *options, bool (*keep)(const char *opt, const void *arg),
                    const void *arg, const char *add) {
    gchar **opts = g_strsplit(options != nullptr ? options : "", ",", -1);
    GString *out = g_string_new(nullptr);
    for (size_t i = 0; opts[i] != nullptr; i++) {
        const char *option = g_strstrip(opts[i]);
        if (option[0] == '\0' || !keep(option, arg))
            continue;
        if (out->len > 0)
            g_string_append_c(out, ',');
        g_string_append(out, option);
    }
    if (add != nullptr && add[0] != '\0') {
        if (out->len > 0)
            g_string_append_c(out, ',');
        g_string_append(out, add);
    }
    g_strfreev(opts);
    return g_string_free(out, FALSE);
}

static bool not_this(const char *opt, const void *arg) { return strcmp(opt, arg) != 0; }

static bool not_in_group(const char *opt, const void *arg) { return !in_group(opt, arg); }

char *xd_kbd_option_in_group(const char *options, const char *group) {
    gchar **opts = g_strsplit(options != nullptr ? options : "", ",", -1);
    char *found = nullptr;
    for (size_t i = 0; opts[i] != nullptr && found == nullptr; i++) {
        const char *option = g_strstrip(opts[i]);
        if (in_group(option, group))
            found = g_strdup(option);
    }
    g_strfreev(opts);
    return found;
}

char *xd_kbd_option_set(const char *options, const char *option, bool on) {
    /* Removed first so it's never listed twice. */
    return filter(options, not_this, option, on ? option : nullptr);
}

char *xd_kbd_options_clear_group(const char *options, const char *group) {
    return filter(options, not_in_group, group, nullptr);
}
