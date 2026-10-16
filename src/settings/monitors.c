/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../core/alloc.h"
#include "monitors.h"

static char *monitors_path(void) {
    const char *cfg = getenv("XDG_CONFIG_HOME");
    if (cfg != nullptr && cfg[0] == '/')
        return g_build_filename(cfg, "xeondesktop", "monitors.ini", nullptr);
    const char *home = getenv("HOME");
    return g_build_filename(home != nullptr ? home : "/tmp", ".config", "xeondesktop",
                            "monitors.ini", nullptr);
}

static void append_part(char *out, size_t cap, const char *part) {
    if (part == nullptr || part[0] == '\0' || strcmp(part, "Unknown") == 0)
        return;
    if (out[0] != '\0')
        g_strlcat(out, " ", cap);
    g_strlcat(out, part, cap);
}

void xd_monitor_key(char *out, size_t cap, const char *make, const char *model,
                    const char *serial, const char *connector) {
    if (cap == 0)
        return;
    out[0] = '\0';
    append_part(out, cap, make);
    append_part(out, cap, model);
    append_part(out, cap, serial);
    const char *conn = connector != nullptr ? connector : "Monitor";
    if (out[0] == '\0') {
        g_strlcpy(out, conn, cap);
    } else if (serial == nullptr || serial[0] == '\0' || strcmp(serial, "Unknown") == 0) {
        /* Identical monitors without serial numbers: tell them apart by
         * where they're plugged in. */
        g_strlcat(out, " on ", cap);
        g_strlcat(out, conn, cap);
    }
}

static GKeyFile *load(void) {
    GKeyFile *kf = g_key_file_new();
    char *path = monitors_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    g_free(path);
    return kf;
}

bool xd_monitors_lookup(const char *key, xd_monitor_config *out) {
    GKeyFile *kf = load();
    bool found = g_key_file_has_group(kf, key);
    if (found) {
        *out = (xd_monitor_config){.enabled = true};
        g_strlcpy(out->key, key, sizeof(out->key));
        GError *err = nullptr;
        bool enabled = g_key_file_get_boolean(kf, key, "Enabled", &err);
        if (err == nullptr)
            out->enabled = enabled;
        g_clear_error(&err);
        out->width = g_key_file_get_integer(kf, key, "Width", nullptr);
        out->height = g_key_file_get_integer(kf, key, "Height", nullptr);
        out->refresh_mhz = g_key_file_get_integer(kf, key, "Refresh", nullptr);
        out->x = g_key_file_get_integer(kf, key, "X", nullptr);
        out->y = g_key_file_get_integer(kf, key, "Y", nullptr);
        out->scale = g_key_file_get_double(kf, key, "Scale", nullptr);
        out->transform = g_key_file_get_integer(kf, key, "Transform", nullptr) & 7;
        if (out->scale < 0.25 || out->scale > 8.0)
            out->scale = 0;
    }
    g_key_file_free(kf);
    return found;
}

bool xd_monitors_store(const xd_monitor_config *configs, size_t count) {
    GKeyFile *kf = load();
    for (size_t i = 0; i < count; i++) {
        const xd_monitor_config *c = &configs[i];
        g_key_file_remove_group(kf, c->key, nullptr);
        g_key_file_set_boolean(kf, c->key, "Enabled", c->enabled);
        g_key_file_set_integer(kf, c->key, "Width", c->width);
        g_key_file_set_integer(kf, c->key, "Height", c->height);
        g_key_file_set_integer(kf, c->key, "Refresh", c->refresh_mhz);
        g_key_file_set_integer(kf, c->key, "X", c->x);
        g_key_file_set_integer(kf, c->key, "Y", c->y);
        g_key_file_set_double(kf, c->key, "Scale", c->scale);
        g_key_file_set_integer(kf, c->key, "Transform", c->transform);
    }
    char *path = monitors_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *err = nullptr;
    bool ok = g_key_file_save_to_file(kf, path, &err);
    if (!ok) {
        fprintf(stderr, "xeondesktop: cannot save %s: %s\n", path,
                err != nullptr ? err->message : "unknown error");
        g_clear_error(&err);
    }
    g_free(path);
    g_key_file_free(kf);
    return ok;
}
