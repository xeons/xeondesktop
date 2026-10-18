/* SPDX-License-Identifier: MIT */
#include <gio/gdesktopappinfo.h>
#include <stdio.h>

#include "gpu.h"

#define SWITCHEROO "net.hadess.SwitcherooControl"
#define SWITCHEROO_PATH "/net/hadess/SwitcherooControl"

/* The freedesktop key: the program wants the card that isn't the default. */
#define PREFERS_KEY "PrefersNonDefaultGPU"
#define PREFS_GROUP "Programs"

void xd_gpu_list_parse(GVariant *gpus, xd_gpu_list *out) {
    *out = (xd_gpu_list){0};
    if (gpus == nullptr || !g_variant_is_of_type(gpus, G_VARIANT_TYPE("aa{sv}")))
        return;
    size_t n = g_variant_n_children(gpus);
    out->items = g_new0(xd_gpu, n > 0 ? n : 1);
    for (size_t i = 0; i < n; i++) {
        GVariant *dict = g_variant_get_child_value(gpus, i);
        xd_gpu *g = &out->items[out->count];
        const char *name = nullptr;
        if (!g_variant_lookup(dict, "Name", "&s", &name))
            name = nullptr;
        gboolean is_default = FALSE;
        if (!g_variant_lookup(dict, "Default", "b", &is_default))
            is_default = FALSE;
        char **env = nullptr;
        if (!g_variant_lookup(dict, "Environment", "^as", &env))
            env = nullptr;
        /* Pairs; an odd one out has no value and is dropped. */
        size_t len = env != nullptr ? g_strv_length(env) : 0;
        if (len % 2 != 0) {
            g_free(env[len - 1]);
            env[len - 1] = nullptr;
        }
        g->name = g_strdup(name != nullptr && name[0] != '\0' ? name : "Graphics card");
        g->env = env != nullptr ? env : g_new0(char *, 1);
        g->is_default = is_default;
        out->count++;
        g_variant_unref(dict);
    }
}

void xd_gpu_list_free(xd_gpu_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        g_free(list->items[i].name);
        g_strfreev(list->items[i].env);
    }
    g_free(list->items);
    *list = (xd_gpu_list){0};
}

int xd_gpu_list_default(const xd_gpu_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i].is_default)
            return (int)i;
    }
    return -1;
}

int xd_gpu_list_other(const xd_gpu_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        if (!list->items[i].is_default)
            return (int)i;
    }
    return -1;
}

int xd_gpu_choose(const xd_gpu_list *list, bool prefers_non_default, xd_gpu_pref pref) {
    if (list->count < 2)
        return -1;
    bool other = pref == XD_GPU_PREF_OTHER || (pref == XD_GPU_PREF_AUTO && prefers_non_default);
    return other ? xd_gpu_list_other(list) : -1;
}

/* ---- per-program choices ----------------------------------------------------- */

static char *prefs_path(const char *file) {
    if (file != nullptr)
        return g_strdup(file);
    return g_build_filename(g_get_user_config_dir(), "xeondesktop", "gpu.ini", nullptr);
}

xd_gpu_pref xd_gpu_pref_get(const char *file, const char *app_id) {
    if (app_id == nullptr)
        return XD_GPU_PREF_AUTO;
    char *path = prefs_path(file);
    GKeyFile *kf = g_key_file_new();
    xd_gpu_pref pref = XD_GPU_PREF_AUTO;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, nullptr)) {
        char *v = g_key_file_get_string(kf, PREFS_GROUP, app_id, nullptr);
        if (v != nullptr && g_ascii_strcasecmp(v, "dedicated") == 0)
            pref = XD_GPU_PREF_OTHER;
        else if (v != nullptr && g_ascii_strcasecmp(v, "default") == 0)
            pref = XD_GPU_PREF_DEFAULT;
        g_free(v);
    }
    g_key_file_free(kf);
    g_free(path);
    return pref;
}

bool xd_gpu_pref_set(const char *file, const char *app_id, xd_gpu_pref pref) {
    if (app_id == nullptr || app_id[0] == '\0')
        return false;
    char *path = prefs_path(file);
    GKeyFile *kf = g_key_file_new();
    /* Keeps what else is there, comments included. */
    g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, nullptr);
    switch (pref) {
    case XD_GPU_PREF_AUTO:
        g_key_file_remove_key(kf, PREFS_GROUP, app_id, nullptr);
        break;
    case XD_GPU_PREF_DEFAULT:
        g_key_file_set_string(kf, PREFS_GROUP, app_id, "default");
        break;
    case XD_GPU_PREF_OTHER:
        g_key_file_set_string(kf, PREFS_GROUP, app_id, "dedicated");
        break;
    }
    char *dir = g_path_get_dirname(path);
    GError *error = nullptr;
    bool ok = g_mkdir_with_parents(dir, 0700) == 0 && g_key_file_save_to_file(kf, path, &error);
    if (!ok) {
        fprintf(stderr, "xd-shell: cannot save %s: %s\n", path,
                error != nullptr ? error->message : "cannot create its folder");
        g_clear_error(&error);
    }
    g_free(dir);
    g_key_file_free(kf);
    g_free(path);
    return ok;
}

char *xd_gpu_app_key(GDesktopAppInfo *info) {
    const char *id = g_app_info_get_id(G_APP_INFO(info));
    if (id != nullptr && id[0] != '\0')
        return g_strdup(id);
    const char *file = g_desktop_app_info_get_filename(info);
    return file != nullptr ? g_path_get_basename(file) : nullptr;
}

/* ---- switcheroo-control --------------------------------------------------------- */

static struct {
    GCancellable *cancel;
    GDBusProxy *proxy;
    xd_gpu_list gpus; /* empty unless there are two or more */
} sw;

static void refresh(void) {
    xd_gpu_list_free(&sw.gpus);
    if (sw.proxy == nullptr)
        return;
    char *owner = g_dbus_proxy_get_name_owner(sw.proxy);
    GVariant *dual = owner != nullptr ? g_dbus_proxy_get_cached_property(sw.proxy, "HasDualGpu")
                                      : nullptr;
    GVariant *gpus = owner != nullptr ? g_dbus_proxy_get_cached_property(sw.proxy, "GPUs")
                                      : nullptr;
    if (dual != nullptr && g_variant_is_of_type(dual, G_VARIANT_TYPE_BOOLEAN) &&
        g_variant_get_boolean(dual)) {
        xd_gpu_list_parse(gpus, &sw.gpus);
        if (sw.gpus.count < 2)
            xd_gpu_list_free(&sw.gpus);
    }
    if (dual != nullptr)
        g_variant_unref(dual);
    if (gpus != nullptr)
        g_variant_unref(gpus);
    g_free(owner);
}

static void props_changed([[maybe_unused]] GDBusProxy *proxy, [[maybe_unused]] GVariant *changed,
                          [[maybe_unused]] const char *const *invalidated,
                          [[maybe_unused]] gpointer data) {
    refresh();
}

static void owner_changed([[maybe_unused]] GObject *proxy, [[maybe_unused]] GParamSpec *spec,
                          [[maybe_unused]] gpointer data) {
    refresh();
}

static void got_proxy([[maybe_unused]] GObject *src, GAsyncResult *res,
                      [[maybe_unused]] gpointer data) {
    GError *error = nullptr;
    GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
    if (proxy == nullptr) {
        /* No system bus: one card as far as we know. */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_debug("xd-shell: switcheroo-control: %s", error->message);
        g_error_free(error);
        return;
    }
    sw.proxy = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(props_changed), &sw);
    g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(owner_changed), &sw);
    refresh();
}

void xd_gpu_watch(void) {
    if (sw.cancel != nullptr)
        return;
    sw.cancel = g_cancellable_new();
    /* Not installed (no dual GPU to speak of): the proxy just has no owner. */
    g_dbus_proxy_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, nullptr,
                             SWITCHEROO, SWITCHEROO_PATH, SWITCHEROO, sw.cancel, got_proxy,
                             nullptr);
}

void xd_gpu_unwatch(void) {
    if (sw.cancel != nullptr) {
        g_cancellable_cancel(sw.cancel);
        g_clear_object(&sw.cancel);
    }
    if (sw.proxy != nullptr) {
        g_signal_handlers_disconnect_by_data(sw.proxy, &sw);
        g_clear_object(&sw.proxy);
    }
    xd_gpu_list_free(&sw.gpus);
}

const xd_gpu_list *xd_gpu_current(void) { return &sw.gpus; }

static int app_choice(GDesktopAppInfo *info) {
    if (sw.gpus.count < 2)
        return -1;
    char *key = xd_gpu_app_key(info);
    int gpu = xd_gpu_choose(&sw.gpus, g_desktop_app_info_get_boolean(info, PREFERS_KEY),
                            xd_gpu_pref_get(nullptr, key));
    g_free(key);
    return gpu;
}

bool xd_gpu_app_uses_other(GDesktopAppInfo *info) { return app_choice(info) >= 0; }

void xd_gpu_setup_launch(GAppLaunchContext *ctx, GDesktopAppInfo *info, int gpu) {
    if (gpu < 0)
        gpu = app_choice(info);
    if (gpu < 0 || (size_t)gpu >= sw.gpus.count)
        return;
    char **env = sw.gpus.items[gpu].env;
    for (size_t i = 0; env[i] != nullptr && env[i + 1] != nullptr; i += 2)
        g_app_launch_context_setenv(ctx, env[i], env[i + 1]);
}
