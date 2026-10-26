/* SPDX-License-Identifier: MIT
 * xd-color: Color Management, after Windows 7's (colorcpl; Windows XP had
 * the same choices as a Color Management tab in a monitor's and a
 * printer's properties). colord (org.freedesktop.ColorManager on the
 * system bus) keeps the devices, profiles and which profile each device
 * uses; this is a client of it, as GNOME's Color panel is.
 *
 *   Devices       a device (monitors, printers, scanners, cameras), the
 *                 profiles associated with it, Add..., Remove and Set as
 *                 Default Profile
 *   All Profiles  every profile colord knows, Add... (install) and Remove
 *
 * colord learns of printers and scanners by itself but not of monitors,
 * which the desktop's session registers: this does it for the monitors
 * the compositor has (wlr-output-management), with the ids
 * gnome-settings-daemon uses. Profiles are installed into the user's
 * store ($XDG_DATA_HOME/icc) and handed to colord by file descriptor, as
 * it can't read our home; at start the store's files are handed over
 * again, since colord forgets them when it restarts and there's no
 * session daemon here to do it.
 *
 * A monitor's default profile goes into desktop.ini's [ColorProfiles] too,
 * from which the compositor loads its calibration curves (vcgt) into the
 * monitor's gamma ramp.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gdk/gdkwayland.h>
#include <gio/gunixfdlist.h>
#include <glib/gstdio.h>

#include "../gtkapp/gtkapp.h"
#include "../settings/monitors.h"
#include "../settings/settings.h"
#include "colorlib.h"
#include "../ui/protocols.h"

static const char CD_NAME[] = "org.freedesktop.ColorManager";
static const char CD_PATH[] = "/org/freedesktop/ColorManager";
static const char CD_DEVICE[] = "org.freedesktop.ColorManager.Device";
static const char CD_PROFILE[] = "org.freedesktop.ColorManager.Profile";
static constexpr int CALL_TIMEOUT_MS = 10000;

enum { DEV_LABEL, DEV_PATH, DEV_COUNT };
enum { PRO_NAME, PRO_FILE, PRO_TYPE, PRO_PATH, PRO_COUNT };

/* A monitor we registered with colord, and its monitors.ini key. */
typedef struct display_link {
    char *device; /* object path */
    char *monitor;
} display_link;

typedef struct cm {
    GtkWidget *window, *notebook;
    GtkWidget *unavailable;
    GtkWidget *device_combo;
    GtkListStore *devices;
    GtkWidget *device_view;
    GtkListStore *device_profiles;
    GtkWidget *add, *remove, *make_default;
    GtkWidget *all_view;
    GtkListStore *all_profiles;
    GtkWidget *all_add, *all_remove;
    GDBusConnection *bus;
    bool available;
    GPtrArray *displays; /* display_link */
    char *store;         /* $XDG_DATA_HOME/icc */
    guint signal_id;
    guint refresh_idle;
} cm;

static void display_link_free(gpointer user_data) {
    display_link *link = user_data;
    g_free(link->device);
    g_free(link->monitor);
    g_free(link);
}

/* ---- colord --------------------------------------------------------------------- */

static GVariant *cd_call(cm *color_manager, const char *path, const char *iface, const char *method,
                         GVariant *args, const GVariantType *reply, GError **error) {
    if (color_manager->bus == nullptr) {
        if (args != nullptr)
            g_variant_unref(g_variant_ref_sink(args));
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "No system bus.");
        return nullptr;
    }
    return g_dbus_connection_call_sync(color_manager->bus, CD_NAME, path, iface, method, args,
                                       reply, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, nullptr,
                                       error);
}

/* An object's properties (a{sv}), or nullptr. */
static GVariant *cd_props(cm *color_manager, const char *path, const char *iface) {
    GVariant *variant = cd_call(color_manager, path, "org.freedesktop.DBus.Properties", "GetAll",
                                g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"), nullptr);
    if (variant == nullptr)
        return nullptr;
    GVariant *props = g_variant_get_child_value(variant, 0);
    g_variant_unref(variant);
    return props;
}

static char *prop_string(GVariant *props, const char *key) {
    const char *value = nullptr;
    return props != nullptr && g_variant_lookup(props, key, "&s", &value) ? g_strdup(value)
                                                                          : g_strdup("");
}

/* A method returning one object path ("(o)"), or nullptr. */
static char *cd_call_path(cm *color_manager, const char *path, const char *iface,
                          const char *method, GVariant *args, GError **error) {
    GVariant *variant =
        cd_call(color_manager, path, iface, method, args, G_VARIANT_TYPE("(o)"), error);
    if (variant == nullptr)
        return nullptr;
    char *out = nullptr;
    g_variant_get(variant, "(o)", &out);
    g_variant_unref(variant);
    return out;
}

/* The object paths a method returns ("(ao)"); never nullptr. */
static char **cd_call_paths(cm *color_manager, const char *method) {
    GVariant *variant =
        cd_call(color_manager, CD_PATH, CD_NAME, method, nullptr, G_VARIANT_TYPE("(ao)"), nullptr);
    if (variant == nullptr)
        return g_new0(char *, 1);
    GVariant *list = g_variant_get_child_value(variant, 0);
    char **out = g_variant_dup_objv(list, nullptr);
    g_variant_unref(list);
    g_variant_unref(variant);
    return out;
}

/* The device's profiles, the default first, as colord orders them. */
static char **device_profiles(cm *color_manager, const char *device) {
    GVariant *props = cd_props(color_manager, device, CD_DEVICE);
    GVariant *list = props != nullptr ? g_variant_lookup_value(props, "Profiles",
                                                               G_VARIANT_TYPE("ao"))
                                      : nullptr;
    char **out = list != nullptr ? g_variant_dup_objv(list, nullptr) : g_new0(char *, 1);
    if (list != nullptr)
        g_variant_unref(list);
    if (props != nullptr)
        g_variant_unref(props);
    return out;
}

static void error_box(cm *color_manager, const char *what, const GError *error) {
    char *text = g_strdup_printf("%s\n\n%s", what, error != nullptr ? error->message : "");
    xd_message_box(GTK_WINDOW(color_manager->window), "Color Management", XD_MB_ERROR, XD_MB_OK,
                   FALSE, g_strstrip(text));
    g_free(text);
}

/* Hands an ICC file to colord as a profile (by descriptor: colord runs as
 * its own user). Returns its object path, or nullptr. */
static char *install_profile(cm *color_manager, const char *path, const char *id, GError **error) {
    char *found = cd_call_path(color_manager, CD_PATH, CD_NAME, "FindProfileById",
                               g_variant_new("(s)", id), nullptr);
    if (found != nullptr)
        return found;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        GIOErrorEnum code = g_io_error_from_errno(errno);
        g_set_error(error, G_IO_ERROR, (gint)code, "%s can't be read.", path);
        return nullptr;
    }
    GUnixFDList *fds = g_unix_fd_list_new_from_array(&fd, 1); /* takes fd */
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{ss}"));
    g_variant_builder_add(&props, "{ss}", "Filename", path);
    g_variant_builder_add(&props, "{ss}", "FILE_checksum", id + strlen("icc-"));
    GVariant *variant = g_dbus_connection_call_with_unix_fd_list_sync(
        color_manager->bus, CD_NAME, CD_PATH, CD_NAME, "CreateProfileWithFd",
        g_variant_new("(ssha{ss})", id, "normal", 0, &props), G_VARIANT_TYPE("(o)"),
        G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, fds, nullptr, nullptr, error);
    g_object_unref(fds);
    if (variant == nullptr)
        return nullptr;
    char *out = nullptr;
    g_variant_get(variant, "(o)", &out);
    g_variant_unref(variant);
    return out;
}

/* Copies a file into the store and installs it. */
static char *import_profile(cm *color_manager, const char *path, GError **error) {
    char *id = nullptr;
    char *stored = xd_color_import(path, color_manager->store, &id, error);
    if (stored == nullptr)
        return nullptr;
    char *profile = install_profile(color_manager, stored, id, error);
    g_free(stored);
    g_free(id);
    return profile;
}

/* The store's profiles, which colord forgets when it restarts. */
static void install_store(cm *color_manager) {
    GDir *dir = g_dir_open(color_manager->store, 0, nullptr);
    if (dir == nullptr)
        return;
    const char *name;
    while ((name = g_dir_read_name(dir)) != nullptr) {
        if (!g_str_has_suffix(name, ".icc") && !g_str_has_suffix(name, ".icm"))
            continue;
        char *path = g_build_filename(color_manager->store, name, nullptr);
        char *known = cd_call_path(color_manager, CD_PATH, CD_NAME, "FindProfileByFilename",
                                   g_variant_new("(s)", path), nullptr);
        char *data = nullptr;
        gsize len = 0;
        if (known == nullptr && g_file_get_contents(path, &data, &len, nullptr) &&
            xd_color_looks_like_icc((const guint8 *)data, len)) {
            char *id = xd_color_profile_id((const guint8 *)data, len);
            GError *error = nullptr;
            char *profile = install_profile(color_manager, path, id, &error);
            if (profile == nullptr) {
                g_message("%s: %s", path, error->message);
                g_error_free(error);
            }
            g_free(profile);
            g_free(id);
        }
        g_free(data);
        g_free(known);
        g_free(path);
    }
    g_dir_close(dir);
}

/* ---- the compositor's monitors ------------------------------------------------------ */

typedef struct heads_query {
    GPtrArray *heads;   /* head_info */
    GPtrArray *proxies; /* heads, to destroy */
    GPtrArray *modes;   /* their modes, to destroy first */
    struct zwlr_output_manager_v1 *manager;
    bool done;
} heads_query;

typedef struct head_info {
    heads_query *q;
    char *name, *make, *model, *serial;
    bool enabled;
} head_info;

static void head_info_free(gpointer user_data) {
    head_info *info = user_data;
    g_free(info->name);
    g_free(info->make);
    g_free(info->model);
    g_free(info->serial);
    g_free(info);
}

static void set_str(char **field, const char *value) {
    g_free(*field);
    *field = g_strdup(value);
}

static void head_name(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                      const char *name) {
    set_str(&((head_info *)data)->name, name);
}
static void head_make(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                      const char *make) {
    set_str(&((head_info *)data)->make, make);
}
static void head_model(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                       const char *model) {
    set_str(&((head_info *)data)->model, model);
}
static void head_serial(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                        const char *serial) {
    set_str(&((head_info *)data)->serial, serial);
}
static void head_enabled(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                         int32_t enabled) {
    ((head_info *)data)->enabled = enabled != 0;
}
static void head_ignore_str([[maybe_unused]] void *data,
                            [[maybe_unused]] struct zwlr_output_head_v1 *head,
                            [[maybe_unused]] const char *text) {}
static void head_ignore_size([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_output_head_v1 *head,
                             [[maybe_unused]] int32_t first, [[maybe_unused]] int32_t second) {}
static void head_ignore_int([[maybe_unused]] void *data,
                            [[maybe_unused]] struct zwlr_output_head_v1 *head,
                            [[maybe_unused]] int32_t value) {}
static void head_ignore_uint([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_output_head_v1 *head,
                             [[maybe_unused]] uint32_t value) {}
static void head_ignore_fixed([[maybe_unused]] void *data,
                              [[maybe_unused]] struct zwlr_output_head_v1 *head,
                              [[maybe_unused]] wl_fixed_t value) {}
static void head_ignore_mode([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_output_head_v1 *head,
                             [[maybe_unused]] struct zwlr_output_mode_v1 *mode) {}
static void head_ignore([[maybe_unused]] void *data,
                        [[maybe_unused]] struct zwlr_output_head_v1 *head) {}
/* Modes are objects of their own, ours to destroy. */
static void head_mode(void *data, [[maybe_unused]] struct zwlr_output_head_v1 *head,
                      struct zwlr_output_mode_v1 *mode) {
    g_ptr_array_add(((head_info *)data)->q->modes, mode);
}

static const struct zwlr_output_head_v1_listener head_listener = {
    .name = head_name,
    .description = head_ignore_str,
    .physical_size = head_ignore_size,
    .mode = head_mode,
    .enabled = head_enabled,
    .current_mode = head_ignore_mode,
    .position = head_ignore_size,
    .transform = head_ignore_int,
    .scale = head_ignore_fixed,
    .finished = head_ignore,
    .make = head_make,
    .model = head_model,
    .serial_number = head_serial,
    .adaptive_sync = head_ignore_uint,
};

static void manager_head(void *data, [[maybe_unused]] struct zwlr_output_manager_v1 *output_manager,
                         struct zwlr_output_head_v1 *head) {
    heads_query *query = data;
    head_info *info = g_new0(head_info, 1);
    info->q = query;
    g_ptr_array_add(query->heads, info);
    g_ptr_array_add(query->proxies, head);
    zwlr_output_head_v1_add_listener(head, &head_listener, info);
}

static void manager_done(void *data, [[maybe_unused]] struct zwlr_output_manager_v1 *output_manager,
                         [[maybe_unused]] uint32_t serial) {
    ((heads_query *)data)->done = true;
}

static void manager_finished([[maybe_unused]] void *data,
                             [[maybe_unused]] struct zwlr_output_manager_v1 *output_manager) {}

static const struct zwlr_output_manager_v1_listener manager_listener = {
    .head = manager_head,
    .done = manager_done,
    .finished = manager_finished,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *iface, uint32_t version) {
    heads_query *query = data;
    if (strcmp(iface, zwlr_output_manager_v1_interface.name) == 0 && query->manager == nullptr) {
        /* Version 2 has make, model and serial number. */
        query->manager =
            wl_registry_bind(registry, name, &zwlr_output_manager_v1_interface, MIN(version, 2u));
        zwlr_output_manager_v1_add_listener(query->manager, &manager_listener, query);
    }
}

static void registry_remove([[maybe_unused]] void *data,
                            [[maybe_unused]] struct wl_registry *registry,
                            [[maybe_unused]] uint32_t name) {}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

/* The compositor's enabled monitors (head_info), through
 * wlr-output-management on GTK's own connection (a queue of our own, so
 * GTK's events wait). Empty outside a wlroots compositor. */
static GPtrArray *list_monitors(void) {
    GPtrArray *out = g_ptr_array_new_with_free_func(head_info_free);
    GdkDisplay *gdk = gdk_display_get_default();
    if (gdk == nullptr || !GDK_IS_WAYLAND_DISPLAY(gdk))
        return out;
    struct wl_display *display = gdk_wayland_display_get_wl_display(gdk);
    struct wl_event_queue *queue = wl_display_create_queue(display);
    struct wl_display *wrapped = wl_proxy_create_wrapper(display);
    wl_proxy_set_queue((struct wl_proxy *)wrapped, queue);
    heads_query query = {.heads = out, .proxies = g_ptr_array_new(), .modes = g_ptr_array_new()};
    struct wl_registry *registry = wl_display_get_registry(wrapped);
    wl_registry_add_listener(registry, &registry_listener, &query);
    wl_display_roundtrip_queue(display, queue);
    for (int i = 0; query.manager != nullptr && !query.done && i < 5; i++)
        wl_display_roundtrip_queue(display, queue);
    /* Version 2 has no release requests: the proxies just go, and the
     * compositor stops sending once it has seen stop. */
    if (query.manager != nullptr)
        zwlr_output_manager_v1_stop(query.manager);
    for (guint i = 0; i < query.modes->len; i++)
        zwlr_output_mode_v1_destroy(g_ptr_array_index(query.modes, i));
    for (guint i = 0; i < query.proxies->len; i++)
        zwlr_output_head_v1_destroy(g_ptr_array_index(query.proxies, i));
    if (query.manager != nullptr)
        zwlr_output_manager_v1_destroy(query.manager);
    wl_registry_destroy(registry);
    wl_proxy_wrapper_destroy(wrapped);
    wl_display_flush(display);
    wl_event_queue_destroy(queue);
    g_ptr_array_free(query.modes, TRUE);
    g_ptr_array_free(query.proxies, TRUE);
    for (guint i = out->len; i-- > 0;) {
        if (!((head_info *)g_ptr_array_index(out, i))->enabled)
            g_ptr_array_remove_index(out, i);
    }
    return out;
}

/* Registers the monitors with colord (or finds them registered). */
static void register_displays(cm *color_manager) {
    GPtrArray *heads = list_monitors();
    for (guint i = 0; i < heads->len; i++) {
        const head_info *info = g_ptr_array_index(heads, i);
        char *id = xd_color_display_id(info->make, info->model, info->serial, info->name);
        char *path = cd_call_path(color_manager, CD_PATH, CD_NAME, "FindDeviceById",
                                  g_variant_new("(s)", id), nullptr);
        if (path == nullptr) {
            GVariantBuilder props;
            g_variant_builder_init(&props, G_VARIANT_TYPE("a{ss}"));
            g_variant_builder_add(&props, "{ss}", "Kind", "display");
            g_variant_builder_add(&props, "{ss}", "Mode", "physical");
            g_variant_builder_add(&props, "{ss}", "Colorspace", "rgb");
            if (info->make != nullptr)
                g_variant_builder_add(&props, "{ss}", "Vendor", info->make);
            if (info->model != nullptr)
                g_variant_builder_add(&props, "{ss}", "Model", info->model);
            if (info->serial != nullptr)
                g_variant_builder_add(&props, "{ss}", "Serial", info->serial);
            if (info->name != nullptr)
                g_variant_builder_add(&props, "{ss}", "XRANDR_name", info->name);
            GError *error = nullptr;
            path = cd_call_path(color_manager, CD_PATH, CD_NAME, "CreateDevice",
                                g_variant_new("(ssa{ss})", id, "normal", &props), &error);
            if (path == nullptr) {
                g_message("%s: %s", id, error->message);
                g_error_free(error);
            }
        }
        if (path != nullptr) {
            display_link *link = g_new0(display_link, 1);
            link->device = path;
            char key[192];
            xd_monitor_key(key, sizeof(key), info->make != nullptr ? info->make : "",
                           info->model != nullptr ? info->model : "",
                           info->serial != nullptr ? info->serial : "",
                           info->name != nullptr ? info->name : "");
            link->monitor = g_strdup(key);
            g_ptr_array_add(color_manager->displays, link);
        }
        g_free(id);
    }
    g_ptr_array_unref(heads);
}

/* Each registered monitor's default profile into [ColorProfiles], for the
 * compositor. */
static void sync_settings(cm *color_manager) {
    if (color_manager->displays->len == 0)
        return;
    xd_settings settings;
    xd_settings_load(&settings);
    bool changed = false;
    for (guint i = 0; i < color_manager->displays->len; i++) {
        const display_link *link = g_ptr_array_index(color_manager->displays, i);
        char **profiles = device_profiles(color_manager, link->device);
        char *file = nullptr;
        if (profiles[0] != nullptr) {
            GVariant *props = cd_props(color_manager, profiles[0], CD_PROFILE);
            file = prop_string(props, "Filename");
            if (props != nullptr)
                g_variant_unref(props);
        }
        g_strfreev(profiles);
        const char *now = xd_settings_color_profile(&settings, link->monitor);
        const char *want = file != nullptr ? file : "";
        if (g_strcmp0(now != nullptr ? now : "", want) != 0) {
            if (xd_settings_set_color_profile(&settings, link->monitor, want))
                changed = true;
            else
                g_message("%s: can't keep %s in desktop.ini", link->monitor, want);
        }
        g_free(file);
    }
    if (changed && !xd_settings_save(&settings))
        g_message("desktop.ini couldn't be saved");
}

/* ---- the lists ---------------------------------------------------------------------- */

static char *selected_path(GtkWidget *view, int column) {
    GtkTreeModel *model;
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model,
                                        &it))
        gtk_tree_model_get(model, &it, column, &path, -1);
    return path;
}

static char *selected_device(cm *color_manager) {
    GtkTreeIter it;
    char *path = nullptr;
    if (gtk_combo_box_get_active_iter(GTK_COMBO_BOX(color_manager->device_combo), &it))
        gtk_tree_model_get(GTK_TREE_MODEL(color_manager->devices), &it, DEV_PATH, &path, -1);
    return path;
}

static const char *profile_type(const char *kind) {
    if (g_strcmp0(kind, "display-device") == 0)
        return "Display";
    if (g_strcmp0(kind, "output-device") == 0)
        return "Printer";
    if (g_strcmp0(kind, "input-device") == 0)
        return "Scanner or camera";
    if (g_strcmp0(kind, "colorspace-conversion") == 0 || g_strcmp0(kind, "abstract") == 0)
        return "Color space";
    return "";
}

/* Adds a profile's row; `default_mark` adds Windows 7's "(default)". */
static void add_profile_row(cm *color_manager, GtkListStore *store, const char *path,
                            bool default_mark) {
    GVariant *props = cd_props(color_manager, path, CD_PROFILE);
    char *title = prop_string(props, "Title");
    char *file = prop_string(props, "Filename");
    char *kind = prop_string(props, "Kind");
    char *base = file[0] != '\0' ? g_path_get_basename(file) : g_strdup("");
    char *name = g_strdup_printf("%s%s", title[0] != '\0' ? title : base,
                                 default_mark ? " (default)" : "");
    gtk_list_store_insert_with_values(store, nullptr, -1, PRO_NAME, name, PRO_FILE, base,
                                      PRO_TYPE, profile_type(kind), PRO_PATH, path, -1);
    g_free(name);
    g_free(base);
    g_free(kind);
    g_free(file);
    g_free(title);
    if (props != nullptr)
        g_variant_unref(props);
}

static void update_buttons(cm *color_manager) {
    char *device = selected_device(color_manager);
    char *profile = selected_path(color_manager->device_view, PRO_PATH);
    gtk_widget_set_sensitive(color_manager->add, color_manager->available && device != nullptr);
    gtk_widget_set_sensitive(color_manager->remove, color_manager->available && profile != nullptr);
    /* The default is the first row. */
    GtkTreeIter first;
    char *first_path = nullptr;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(color_manager->device_profiles), &first))
        gtk_tree_model_get(GTK_TREE_MODEL(color_manager->device_profiles), &first, PRO_PATH,
                           &first_path, -1);
    gtk_widget_set_sensitive(color_manager->make_default, color_manager->available &&
                                                              profile != nullptr &&
                                                              g_strcmp0(profile, first_path) != 0);
    g_free(first_path);
    char *any = selected_path(color_manager->all_view, PRO_PATH);
    bool ours = false;
    if (any != nullptr) {
        GVariant *props = cd_props(color_manager, any, CD_PROFILE);
        char *file = prop_string(props, "Filename");
        char *dir = g_path_get_dirname(file);
        ours = strcmp(dir, color_manager->store) == 0;
        g_free(dir);
        g_free(file);
        if (props != nullptr)
            g_variant_unref(props);
    }
    gtk_widget_set_sensitive(color_manager->all_add, color_manager->available);
    /* Only what was installed here: system profiles belong to packages. */
    gtk_widget_set_sensitive(color_manager->all_remove, color_manager->available && ours);
    g_free(any);
    g_free(profile);
    g_free(device);
}

static void fill_device_profiles(cm *color_manager) {
    gtk_list_store_clear(color_manager->device_profiles);
    char *device = selected_device(color_manager);
    if (device != nullptr) {
        char **profiles = device_profiles(color_manager, device);
        for (char **profile = profiles; *profile != nullptr; profile++)
            add_profile_row(color_manager, color_manager->device_profiles, *profile,
                            profile == profiles);
        g_strfreev(profiles);
    }
    g_free(device);
    update_buttons(color_manager);
}

static void select_row(GtkWidget *view, GtkTreeModel *model, int column, const char *path) {
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok && path != nullptr;
         ok = gtk_tree_model_iter_next(model, &it)) {
        char *row_path = nullptr;
        gtk_tree_model_get(model, &it, column, &row_path, -1);
        bool match = g_strcmp0(row_path, path) == 0;
        g_free(row_path);
        if (match) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &it);
            return;
        }
    }
}

/* Reads everything from colord again, keeping the selections. */
static void refresh(cm *color_manager) {
    char *device = selected_device(color_manager);
    char *profile = selected_path(color_manager->device_view, PRO_PATH);
    char *any = selected_path(color_manager->all_view, PRO_PATH);

    gtk_list_store_clear(color_manager->devices);
    char **devices = cd_call_paths(color_manager, "GetDevices");
    /* The device shown before, else the first monitor, as Windows 7's
     * opens on the display. */
    int active = -1, first_display = -1, count = 0;
    for (char **device_path = devices; *device_path != nullptr; device_path++, count++) {
        GVariant *props = cd_props(color_manager, *device_path, CD_DEVICE);
        char *kind = prop_string(props, "Kind");
        char *vendor = prop_string(props, "Vendor");
        char *model = prop_string(props, "Model");
        char *id = prop_string(props, "DeviceId");
        char *label = xd_color_device_label(kind, vendor, model, id);
        gtk_list_store_insert_with_values(color_manager->devices, nullptr, -1, DEV_LABEL, label,
                                          DEV_PATH, *device_path, -1);
        if (g_strcmp0(*device_path, device) == 0)
            active = count;
        if (first_display < 0 && strcmp(kind, "display") == 0)
            first_display = count;
        g_free(label);
        g_free(id);
        g_free(model);
        g_free(vendor);
        g_free(kind);
        if (props != nullptr)
            g_variant_unref(props);
    }
    g_strfreev(devices);
    if (active < 0)
        active = MAX(first_display, 0);
    if (count > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(color_manager->device_combo),
                                 active); /* fills the list */
    else
        fill_device_profiles(color_manager);
    select_row(color_manager->device_view, GTK_TREE_MODEL(color_manager->device_profiles), PRO_PATH,
               profile);

    gtk_list_store_clear(color_manager->all_profiles);
    char **profiles = cd_call_paths(color_manager, "GetProfiles");
    for (char **profile_path = profiles; *profile_path != nullptr; profile_path++)
        add_profile_row(color_manager, color_manager->all_profiles, *profile_path, false);
    g_strfreev(profiles);
    select_row(color_manager->all_view, GTK_TREE_MODEL(color_manager->all_profiles), PRO_PATH, any);
    update_buttons(color_manager);
    g_free(any);
    g_free(profile);
    g_free(device);
}

static gboolean refresh_idle(gpointer data) {
    cm *color_manager = data;
    color_manager->refresh_idle = 0;
    refresh(color_manager);
    sync_settings(color_manager);
    return G_SOURCE_REMOVE;
}

/* colord's DeviceChanged, ProfileAdded and the rest: another program (or
 * a printer coming) changed something. */
static void on_colord_signal([[maybe_unused]] GDBusConnection *bus,
                             [[maybe_unused]] const char *sender,
                             [[maybe_unused]] const char *path,
                             [[maybe_unused]] const char *iface,
                             [[maybe_unused]] const char *signal,
                             [[maybe_unused]] GVariant *params, gpointer data) {
    cm *color_manager = data;
    if (color_manager->refresh_idle == 0)
        color_manager->refresh_idle = g_timeout_add(100, refresh_idle, color_manager);
}

/* ---- actions ------------------------------------------------------------------------ */

/* A device's profiles changed: for a monitor, the compositor's copy too. */
static void after_change(cm *color_manager) {
    refresh(color_manager);
    sync_settings(color_manager);
}

static char *choose_icc_file(cm *color_manager, const char *title) {
    GtkFileChooserNative *file_chooser =
        gtk_file_chooser_native_new(title, GTK_WINDOW(color_manager->window),
                                    GTK_FILE_CHOOSER_ACTION_OPEN, "_Add", "_Cancel");
    GtkFileFilter *icc_filter = gtk_file_filter_new();
    gtk_file_filter_set_name(icc_filter, "ICC Profiles (*.icm, *.icc)");
    gtk_file_filter_add_pattern(icc_filter, "*.icc");
    gtk_file_filter_add_pattern(icc_filter, "*.icm");
    gtk_file_filter_add_pattern(icc_filter, "*.ICC");
    gtk_file_filter_add_pattern(icc_filter, "*.ICM");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), icc_filter);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All Files (*.*)");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), all);
    char *path = nullptr;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(file_chooser)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(file_chooser));
    g_object_unref(file_chooser);
    return path;
}

static bool associate(cm *color_manager, const char *device, const char *profile) {
    GError *error = nullptr;
    GVariant *variant = cd_call(color_manager, device, CD_DEVICE, "AddProfile",
                                g_variant_new("(so)", "hard", profile), nullptr, &error);
    if (variant == nullptr) {
        /* Already associated: nothing to do. */
        char *remote = g_dbus_error_get_remote_error(error);
        bool already = remote != nullptr && g_str_has_suffix(remote, ".AlreadyExists");
        g_free(remote);
        if (!already)
            error_box(color_manager, "The profile could not be associated with the device.", error);
        g_error_free(error);
        return already;
    }
    g_variant_unref(variant);
    return true;
}

static void on_associate_row(GtkDialog *gtk_dialog) {
    gtk_dialog_response(gtk_dialog, GTK_RESPONSE_OK);
}

/* Windows 7's Associate Color Profile: the installed profiles, and
 * Browse... for a file. */
static void on_add(GtkWidget *widget, gpointer data) {
    (void)widget;
    cm *color_manager = data;
    char *device = selected_device(color_manager);
    if (device == nullptr)
        return;
    GtkWidget *associate_dialog = gtk_dialog_new_with_buttons(
        "Associate Color Profile", GTK_WINDOW(color_manager->window), GTK_DIALOG_MODAL,
        "_Browse...", 1, "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(associate_dialog), 460, 320);
    gtk_window_set_resizable(GTK_WINDOW(associate_dialog), FALSE);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(associate_dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Select a color profile:");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 4);
    GtkListStore *store = gtk_list_store_new(PRO_COUNT, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    char **profiles = cd_call_paths(color_manager, "GetProfiles");
    for (char **profile_path = profiles; *profile_path != nullptr; profile_path++)
        add_profile_row(color_manager, store, *profile_path, false);
    g_strfreev(profiles);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), view);
    const char *titles[] = {"Name", "File name"};
    for (int i = 0; i < 2; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            titles[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(associate_dialog), GTK_RESPONSE_OK);
    /* A double-click (or Enter) on a profile picks it. */
    g_signal_connect_swapped(view, "row-activated", G_CALLBACK(on_associate_row), associate_dialog);
    gtk_widget_show_all(associate_dialog);
    gtk_widget_grab_focus(view);
    bool changed = false;
    for (;;) {
        int response = gtk_dialog_run(GTK_DIALOG(associate_dialog));
        if (response == 1) {
            char *file = choose_icc_file(color_manager, "Associate Color Profile");
            if (file == nullptr)
                continue;
            GError *error = nullptr;
            char *profile = import_profile(color_manager, file, &error);
            if (profile == nullptr) {
                error_box(color_manager, "The profile could not be installed.", error);
                g_clear_error(&error);
            } else {
                changed = associate(color_manager, device, profile);
            }
            g_free(profile);
            g_free(file);
            if (changed)
                break;
            continue;
        }
        if (response == GTK_RESPONSE_OK) {
            char *profile = selected_path(view, PRO_PATH);
            if (profile == nullptr)
                continue;
            changed = associate(color_manager, device, profile);
            g_free(profile);
        }
        break;
    }
    gtk_widget_destroy(associate_dialog);
    g_free(device);
    if (changed)
        after_change(color_manager);
}

static void on_remove(GtkWidget *widget, gpointer data) {
    (void)widget;
    cm *color_manager = data;
    char *device = selected_device(color_manager);
    char *profile = selected_path(color_manager->device_view, PRO_PATH);
    if (device != nullptr && profile != nullptr) {
        GError *error = nullptr;
        GVariant *variant = cd_call(color_manager, device, CD_DEVICE, "RemoveProfile",
                                    g_variant_new("(o)", profile), nullptr, &error);
        if (variant == nullptr) {
            error_box(color_manager, "The profile could not be removed from the device.", error);
            g_error_free(error);
        } else {
            g_variant_unref(variant);
            after_change(color_manager);
        }
    }
    g_free(profile);
    g_free(device);
}

static void on_make_default(GtkWidget *widget, gpointer data) {
    (void)widget;
    cm *color_manager = data;
    char *device = selected_device(color_manager);
    char *profile = selected_path(color_manager->device_view, PRO_PATH);
    if (device != nullptr && profile != nullptr) {
        GError *error = nullptr;
        GVariant *variant = cd_call(color_manager, device, CD_DEVICE, "MakeProfileDefault",
                                    g_variant_new("(o)", profile), nullptr, &error);
        if (variant == nullptr) {
            error_box(color_manager, "The default profile could not be set.", error);
            g_error_free(error);
        } else {
            g_variant_unref(variant);
            after_change(color_manager);
        }
    }
    g_free(profile);
    g_free(device);
}

static void on_all_add(GtkWidget *widget, gpointer data) {
    (void)widget;
    cm *color_manager = data;
    char *file = choose_icc_file(color_manager, "Install Profile");
    if (file == nullptr)
        return;
    GError *error = nullptr;
    char *profile = import_profile(color_manager, file, &error);
    if (profile == nullptr) {
        error_box(color_manager, "The profile could not be installed.", error);
        g_error_free(error);
    } else {
        refresh(color_manager);
        select_row(color_manager->all_view, GTK_TREE_MODEL(color_manager->all_profiles), PRO_PATH,
                   profile);
    }
    g_free(profile);
    g_free(file);
}

/* Uninstalls one of the user's profiles: colord forgets it, the file goes
 * to the Recycle Bin. */
static void on_all_remove(GtkWidget *widget, gpointer data) {
    (void)widget;
    cm *color_manager = data;
    char *profile = selected_path(color_manager->all_view, PRO_PATH);
    if (profile == nullptr)
        return;
    GVariant *props = cd_props(color_manager, profile, CD_PROFILE);
    char *title = prop_string(props, "Title");
    char *file = prop_string(props, "Filename");
    if (props != nullptr)
        g_variant_unref(props);
    char *question = g_strdup_printf("Are you sure you want to remove the color profile "
                                     "\"%s\"? Devices that use it will no longer use it.",
                                     title[0] != '\0' ? title : file);
    if (xd_message_box(GTK_WINDOW(color_manager->window), "Remove Profile", XD_MB_QUESTION,
                       XD_MB_YES_NO, TRUE, question) == GTK_RESPONSE_YES) {
        GError *error = nullptr;
        GVariant *variant = cd_call(color_manager, CD_PATH, CD_NAME, "DeleteProfile",
                                    g_variant_new("(o)", profile), nullptr, &error);
        if (variant == nullptr) {
            error_box(color_manager, "The profile could not be removed.", error);
            g_error_free(error);
        } else {
            g_variant_unref(variant);
            GFile *trash_file = g_file_new_for_path(file);
            if (!g_file_trash(trash_file, nullptr, &error)) {
                g_message("%s: %s", file, error->message);
                g_error_free(error);
            }
            g_object_unref(trash_file);
            after_change(color_manager);
        }
    }
    g_free(question);
    g_free(file);
    g_free(title);
    g_free(profile);
}

static void on_device_changed(GtkComboBox *combo, gpointer data) {
    (void)combo;
    fill_device_profiles(data);
}

static void on_selection_changed(GtkTreeSelection *sel, gpointer data) {
    (void)sel;
    update_buttons(data);
}

/* ---- the window --------------------------------------------------------------------- */

static GtkWidget *profile_view(cm *color_manager, GtkListStore *store, bool type_column) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    const char *titles[] = {"Name", "File name", "Type"};
    int widths[] = {220, 150, 90};
    for (int i = 0; i < (type_column ? 3 : 2); i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
        GtkTreeViewColumn *col =
            gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_column_set_sizing(col, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(col, widths[i]);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), "changed",
                     G_CALLBACK(on_selection_changed), color_manager);
    return view;
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *scrolled_window = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scrolled_window), GTK_SHADOW_IN);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scrolled_window), child);
    return scrolled_window;
}

static GtkWidget *button_row(GtkWidget **buttons, int count) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = 0; i < count; i++)
        gtk_box_pack_start(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    return row;
}

static GtkWidget *devices_page(cm *color_manager) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    color_manager->unavailable =
        gtk_label_new("The color management service (colord) isn't running, so "
                      "devices and their color profiles can't be shown or changed.");
    gtk_label_set_line_wrap(GTK_LABEL(color_manager->unavailable), TRUE);
    gtk_label_set_xalign(GTK_LABEL(color_manager->unavailable), 0);
    gtk_widget_set_no_show_all(color_manager->unavailable, TRUE);
    gtk_box_pack_start(GTK_BOX(page), color_manager->unavailable, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *label = gtk_label_new_with_mnemonic("_Device:");
    color_manager->devices = gtk_list_store_new(DEV_COUNT, G_TYPE_STRING, G_TYPE_STRING);
    color_manager->device_combo =
        gtk_combo_box_new_with_model(GTK_TREE_MODEL(color_manager->devices));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(color_manager->device_combo), renderer, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(color_manager->device_combo), renderer, "text",
                                  DEV_LABEL);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), color_manager->device_combo);
    g_signal_connect(color_manager->device_combo, "changed", G_CALLBACK(on_device_changed),
                     color_manager);
    gtk_box_pack_start(GTK_BOX(row), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), color_manager->device_combo, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);

    GtkWidget *frame = gtk_frame_new("Profiles associated with this device:");
    GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(inner), 8);
    color_manager->device_profiles =
        gtk_list_store_new(PRO_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    color_manager->device_view = profile_view(color_manager, color_manager->device_profiles, false);
    gtk_box_pack_start(GTK_BOX(inner), scrolled(color_manager->device_view), TRUE, TRUE, 0);
    color_manager->add = gtk_button_new_with_mnemonic("_Add...");
    color_manager->remove = gtk_button_new_with_mnemonic("_Remove");
    color_manager->make_default = gtk_button_new_with_mnemonic("_Set as Default Profile");
    g_signal_connect(color_manager->add, "clicked", G_CALLBACK(on_add), color_manager);
    g_signal_connect(color_manager->remove, "clicked", G_CALLBACK(on_remove), color_manager);
    g_signal_connect(color_manager->make_default, "clicked", G_CALLBACK(on_make_default),
                     color_manager);
    gtk_box_pack_start(GTK_BOX(inner),
                       button_row((GtkWidget *[]){color_manager->add, color_manager->remove,
                                                  color_manager->make_default},
                                  3),
                       FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), inner);
    gtk_box_pack_start(GTK_BOX(page), frame, TRUE, TRUE, 0);
    return page;
}

static GtkWidget *all_profiles_page(cm *color_manager) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    GtkWidget *label = gtk_label_new("Color profiles installed on this computer:");
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_box_pack_start(GTK_BOX(page), label, FALSE, FALSE, 0);
    color_manager->all_profiles =
        gtk_list_store_new(PRO_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    color_manager->all_view = profile_view(color_manager, color_manager->all_profiles, true);
    gtk_box_pack_start(GTK_BOX(page), scrolled(color_manager->all_view), TRUE, TRUE, 0);
    color_manager->all_add = gtk_button_new_with_mnemonic("A_dd...");
    color_manager->all_remove = gtk_button_new_with_mnemonic("R_emove");
    g_signal_connect(color_manager->all_add, "clicked", G_CALLBACK(on_all_add), color_manager);
    g_signal_connect(color_manager->all_remove, "clicked", G_CALLBACK(on_all_remove),
                     color_manager);
    gtk_box_pack_start(
        GTK_BOX(page),
        button_row((GtkWidget *[]){color_manager->all_add, color_manager->all_remove}, 2), FALSE,
        FALSE, 0);
    return page;
}

static void on_response(GtkDialog *gtk_dialog, [[maybe_unused]] int response,
                        [[maybe_unused]] gpointer data) {
    gtk_widget_destroy(GTK_WIDGET(gtk_dialog));
}

int main(int argc, char **argv) {
    int page = 0;
    GOptionEntry entries[] = {
        {"page", 0, 0, G_OPTION_ARG_INT, &page, "Open on tab N (0 Devices, 1 All Profiles)", "N"},
        G_OPTION_ENTRY_NULL,
    };
    xd_gtk_app_preinit("org.xeondesktop.ColorManagement");
    GError *error = nullptr;
    if (!gtk_init_with_args(&argc, &argv, "- Color Management", entries, nullptr, &error)) {
        g_printerr("xd-color: %s\n", error->message);
        g_error_free(error);
        return EXIT_FAILURE;
    }
    xd_gtk_app_follow_theme();
    static cm color_manager;
    color_manager.displays = g_ptr_array_new_with_free_func(display_link_free);
    color_manager.store = g_build_filename(g_get_user_data_dir(), "icc", nullptr);
    color_manager.bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (color_manager.bus == nullptr) {
        g_message("system bus: %s", error->message);
        g_clear_error(&error);
    }
    /* Is colord there (activatable or running)? */
    GVariant *variant = cd_call(&color_manager, CD_PATH, "org.freedesktop.DBus.Peer", "Ping",
                                nullptr, nullptr, &error);
    color_manager.available = variant != nullptr;
    if (variant != nullptr)
        g_variant_unref(variant);
    else {
        g_message("colord: %s", error->message);
        g_clear_error(&error);
    }

    color_manager.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(color_manager.window), "Color Management");
    gtk_window_set_icon_name(GTK_WINDOW(color_manager.window), "preferences-desktop-color");
    gtk_window_set_default_size(GTK_WINDOW(color_manager.window), 520, 440);
    gtk_window_set_resizable(GTK_WINDOW(color_manager.window), FALSE);
    gtk_dialog_add_button(GTK_DIALOG(color_manager.window), "Close", GTK_RESPONSE_CLOSE);
    gtk_dialog_set_default_response(GTK_DIALOG(color_manager.window), GTK_RESPONSE_CLOSE);
    color_manager.notebook = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(color_manager.notebook), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(color_manager.notebook), devices_page(&color_manager),
                             gtk_label_new("Devices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(color_manager.notebook),
                             all_profiles_page(&color_manager), gtk_label_new("All Profiles"));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(color_manager.window))),
                       color_manager.notebook, TRUE, TRUE, 0);
    xd_gtk_notebook_ctrl_tab(color_manager.window, GTK_NOTEBOOK(color_manager.notebook));
    g_signal_connect(color_manager.window, "response", G_CALLBACK(on_response), &color_manager);
    g_signal_connect(color_manager.window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    if (color_manager.available) {
        register_displays(&color_manager);
        install_store(&color_manager);
        sync_settings(&color_manager);
        color_manager.signal_id = g_dbus_connection_signal_subscribe(
            color_manager.bus, CD_NAME, CD_NAME, nullptr, CD_PATH, nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, on_colord_signal, &color_manager, nullptr);
    } else {
        gtk_widget_show(color_manager.unavailable);
    }
    refresh(&color_manager);
    gtk_widget_show_all(color_manager.window);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(color_manager.notebook), CLAMP(page, 0, 1));
    gtk_main();

    if (color_manager.signal_id != 0)
        g_dbus_connection_signal_unsubscribe(color_manager.bus, color_manager.signal_id);
    if (color_manager.refresh_idle != 0)
        g_source_remove(color_manager.refresh_idle);
    g_clear_object(&color_manager.bus);
    g_ptr_array_unref(color_manager.displays);
    g_free(color_manager.store);
    return EXIT_SUCCESS;
}
