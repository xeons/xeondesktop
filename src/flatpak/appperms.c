/* SPDX-License-Identifier: MIT
 * An app's permissions dialog: what the app's metadata and the overrides
 * below the user's give it ("base"), and the user's override on top, which
 * every change edits in memory until OK or Apply writes it.
 *
 *   General      shared subsystems and sockets
 *   Devices      devices and features
 *   File System  filesystems (Add, Edit, Remove) and persistent folders
 *   Environment  variables set or unset
 *   D-Bus        session and system bus names the app may see, talk to or
 *                own
 *
 * Whatever the user changed is shown in bold; Restore Defaults drops all
 * of it. A change that matches what the app has anyway leaves no entry,
 * so the override holds only real differences, as Flatseal's do.
 */
#include "flatpak.h"
#include "perms.h"

typedef struct dialog {
    const xd_fp_app *app;
    GtkWidget *window, *apply;
    xd_fp_perms *base, *mine, *effective;
    GKeyFile *override;
    char *path;
    bool updating, saved;
    /* General and Devices: a check box per name, by list */
    GHashTable *checks[XD_FP_LIST_COUNT]; /* name -> GtkWidget */
    GtkWidget *check_boxes[XD_FP_LIST_COUNT];
    /* File System */
    GtkListStore *fs, *persist;
    GtkWidget *fs_view, *fs_edit, *fs_remove, *persist_view, *persist_remove;
    /* Environment */
    GtkListStore *env;
    GtkWidget *env_view, *env_edit, *env_remove;
    /* D-Bus */
    GtkListStore *bus[XD_FP_BUS_COUNT];
    GtkWidget *bus_view[XD_FP_BUS_COUNT], *bus_edit[XD_FP_BUS_COUNT],
        *bus_remove[XD_FP_BUS_COUNT];
} dialog;

/* Every list's columns: what's shown, the key, the row's weight. */
enum { ROW_TEXT, ROW_VALUE, ROW_KEY, ROW_WEIGHT, ROW_COUNT };

static void refresh(dialog *app_dialog);

static void changed(dialog *app_dialog) {
    xd_fp_perms_free(app_dialog->mine);
    xd_fp_perms_free(app_dialog->effective);
    app_dialog->mine = xd_fp_perms_new();
    xd_fp_perms_merge(app_dialog->mine, app_dialog->override);
    app_dialog->effective = xd_fp_perms_copy(app_dialog->base);
    xd_fp_perms_merge(app_dialog->effective, app_dialog->override);
    if (app_dialog->apply != nullptr && !app_dialog->updating)
        gtk_widget_set_sensitive(app_dialog->apply, TRUE);
    refresh(app_dialog);
}

static int weight(bool mine) { return mine ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL; }

/* ---- the layers -------------------------------------------------------------- */

static void merge_bytes(xd_fp_perms *permissions, GBytes *bytes) {
    if (bytes == nullptr)
        return;
    GKeyFile *kf = g_key_file_new();
    gsize length;
    const char *data = g_bytes_get_data(bytes, &length);
    if (g_key_file_load_from_data(kf, data, length, G_KEY_FILE_NONE, nullptr))
        xd_fp_perms_merge(permissions, kf);
    g_key_file_free(kf);
}

static void merge_file(xd_fp_perms *permissions, const char *path) {
    GKeyFile *kf = xd_fp_override_load(path, nullptr);
    if (kf != nullptr) {
        xd_fp_perms_merge(permissions, kf);
        g_key_file_free(kf);
    }
}

static char *installation_dir(FlatpakInstallation *inst) {
    GFile *file = flatpak_installation_get_path(inst);
    char *path = g_file_get_path(file);
    g_object_unref(file);
    return path;
}

/* The runtime's metadata: from the app's own installation first, as
 * flatpak looks for it. */
static GBytes *runtime_metadata(const xd_fp_app *app, GKeyFile *metadata) {
    char *runtime = g_key_file_get_string(metadata, "Application", "runtime", nullptr);
    char **parts = runtime != nullptr ? g_strsplit(runtime, "/", 3) : nullptr;
    g_free(runtime);
    if (parts == nullptr || g_strv_length(parts) != 3) {
        g_strfreev(parts);
        return nullptr;
    }
    GPtrArray *insts = xd_fp_installations();
    g_ptr_array_insert(insts, 0, g_object_ref(app->inst));
    GBytes *bytes = nullptr;
    for (guint i = 0; bytes == nullptr && i < insts->len; i++) {
        FlatpakInstalledRef *installed = flatpak_installation_get_installed_ref(
            g_ptr_array_index(insts, i), FLATPAK_REF_KIND_RUNTIME, parts[0], parts[1], parts[2],
            nullptr, nullptr);
        if (installed != nullptr) {
            bytes = flatpak_installed_ref_load_metadata(installed, nullptr, nullptr);
            g_object_unref(installed);
        }
    }
    g_ptr_array_unref(insts);
    g_strfreev(parts);
    return bytes;
}

/* Everything below the user's own override for the app, in flatpak's
 * order (perms.h). */
static xd_fp_perms *load_base(const xd_fp_app *app) {
    xd_fp_perms *permissions = xd_fp_perms_new();
    GBytes *bytes = flatpak_installed_ref_load_metadata(app->ref, nullptr, nullptr);
    GKeyFile *metadata = g_key_file_new();
    if (bytes != nullptr) {
        gsize length;
        const char *data = g_bytes_get_data(bytes, &length);
        g_key_file_load_from_data(metadata, data, length, G_KEY_FILE_NONE, nullptr);
    }
    GBytes *runtime = runtime_metadata(app, metadata);
    merge_bytes(permissions, runtime);
    if (runtime != nullptr)
        g_bytes_unref(runtime);
    merge_bytes(permissions, bytes);
    if (bytes != nullptr)
        g_bytes_unref(bytes);
    g_key_file_free(metadata);
    /* System overrides count for apps of a system installation only. */
    if (!flatpak_installation_get_is_user(app->inst)) {
        FlatpakInstallation *system = flatpak_installation_new_system(nullptr, nullptr);
        if (system != nullptr) {
            char *dir = installation_dir(system);
            char *global = xd_fp_override_path(dir, "global");
            char *own = xd_fp_override_path(dir, app->id);
            merge_file(permissions, global);
            merge_file(permissions, own);
            g_free(global);
            g_free(own);
            g_free(dir);
            g_object_unref(system);
        }
    }
    char *mine = xd_fp_user_override_path(app->id);
    char *dir = g_path_get_dirname(mine);
    char *global = g_build_filename(dir, "global", nullptr);
    merge_file(permissions, global);
    g_free(global);
    g_free(dir);
    g_free(mine);
    return permissions;
}

/* ---- General and Devices ----------------------------------------------------- */

typedef struct name_label {
    const char *name, *label;
} name_label;

static const name_label labels[] = {
    {"network", "_Network"},
    {"ipc", "_Inter-process communication"},
    {"x11", "_X11 windowing system"},
    {"wayland", "_Wayland windowing system"},
    {"fallback-x11", "X11 only wit_hout Wayland"},
    {"pulseaudio", "_Sound (PulseAudio)"},
    {"session-bus", "All of the D-Bus s_ession bus"},
    {"system-bus", "All of the D-Bus s_ystem bus"},
    {"ssh-auth", "Secure Shell a_gent"},
    {"pcsc", "S_mart cards"},
    {"cups", "_Printing"},
    {"gpg-agent", "GnuPG agen_t"},
    {"inherit-wayland-socket", "The Wayland socket it was starte_d with"},
    {"dri", "_Graphics acceleration"},
    {"input", "_Input devices (joysticks, game controllers)"},
    {"usb", "_USB devices"},
    {"kvm", "_Virtualization (KVM)"},
    {"shm", "S_hared memory (/dev/shm)"},
    {"all", "_All devices (webcams, for example)"},
    {"devel", "_Development tools (debuggers, profilers)"},
    {"multiarch", "Programs for _other processors (32-bit)"},
    {"bluetooth", "_Bluetooth"},
    {"canbus", "_CAN bus"},
    {"per-app-dev-shm", "Shared memory of its o_wn"},
};

static const char *label_for(const char *name) {
    for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
        if (strcmp(labels[i].name, name) == 0)
            return labels[i].label;
    }
    return nullptr;
}

/* flatpak-metadata(5)'s conditions, said plainly. */
static char *conditions_text(const GPtrArray *conds) {
    GString *string = g_string_new("Only ");
    for (guint i = 0; conds != nullptr && i < conds->len; i++) {
        const char *condition = g_ptr_array_index(conds, i);
        bool not = condition[0] == '!';
        const char *bare = not ? condition + 1 : condition;
        const char *what = strcmp(bare, "has-wayland") == 0      ? "in a Wayland session"
                           : strcmp(bare, "has-input-device") == 0 ? "where Flatpak has input devices"
                           : strcmp(bare, "has-usb-device") == 0   ? "where Flatpak has USB devices"
                           : strcmp(bare, "has-usb-portal") == 0   ? "with the USB portal"
                           : strcmp(bare, "true") == 0             ? "always"
                           : strcmp(bare, "false") == 0            ? "never"
                                                                   : bare;
        if (i > 0)
            g_string_append(string, ", or ");
        if (not)
            g_string_append(string, "when not ");
        else if (strcmp(bare, "true") != 0 && strcmp(bare, "false") != 0)
            g_string_append(string, "when ");
        g_string_append(string, what);
    }
    g_string_append(string, ". Click to decide yourself.");
    return g_string_free(string, FALSE);
}

static void on_check(GtkToggleButton *toggle, gpointer data) {
    dialog *app_dialog = data;
    if (app_dialog->updating)
        return;
    xd_fp_list list = (xd_fp_list)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(toggle), "list"));
    const char *name = g_object_get_data(G_OBJECT(toggle), "name");
    gtk_toggle_button_set_inconsistent(toggle, FALSE);
    bool on = gtk_toggle_button_get_active(toggle);
    xd_fp_state base = xd_fp_perms_state(app_dialog->base, list, name);
    /* What the app has anyway leaves no entry. */
    bool same = on ? base == XD_FP_ON : base == XD_FP_UNSET || base == XD_FP_OFF;
    xd_fp_override_set_state(app_dialog->override, list, name,
                             same ? XD_FP_UNSET
                             : on ? XD_FP_ON
                                  : XD_FP_OFF);
    changed(app_dialog);
}

static GtkWidget *add_check(dialog *app_dialog, xd_fp_list list, const char *name) {
    const char *text = label_for(name);
    GtkWidget *button = text != nullptr ? gtk_check_button_new_with_mnemonic(text)
                                        : gtk_check_button_new_with_label(name);
    g_object_set_data(G_OBJECT(button), "list", GINT_TO_POINTER(list));
    g_object_set_data_full(G_OBJECT(button), "name", g_strdup(name), g_free);
    g_signal_connect(button, "toggled", G_CALLBACK(on_check), app_dialog);
    g_hash_table_insert(app_dialog->checks[list], g_strdup(name), button);
    GtkWidget *grid = app_dialog->check_boxes[list];
    int position = (int)g_hash_table_size(app_dialog->checks[list]) - 1;
    /* Two columns, down then across, for the long lists. */
    int columns = list == XD_FP_SHARED ? 1 : 2;
    int known = 0;
    for (const char *const *known_name = xd_fp_list_names(list); *known_name != nullptr;
         known_name++)
        known++;
    int rows = (known + 1) / columns;
    gtk_grid_attach(GTK_GRID(grid), button, position / MAX(rows, 1), position % MAX(rows, 1), 1, 1);
    gtk_widget_show(button);
    return button;
}

static void show_checks(dialog *app_dialog) {
    for (int list = 0; list < XD_FP_LIST_COUNT; list++) {
        /* Names a newer Flatpak knows get a box too, under their own name. */
        GPtrArray *other = xd_fp_perms_other_names(app_dialog->effective, (xd_fp_list)list);
        for (guint i = 0; i < other->len; i++) {
            const char *name = g_ptr_array_index(other, i);
            if (!g_hash_table_contains(app_dialog->checks[list], name))
                add_check(app_dialog, (xd_fp_list)list, name);
        }
        g_ptr_array_unref(other);
        GHashTableIter it;
        gpointer key, value;
        g_hash_table_iter_init(&it, app_dialog->checks[list]);
        while (g_hash_table_iter_next(&it, &key, &value)) {
            xd_fp_state fp_state = xd_fp_perms_state(app_dialog->effective, (xd_fp_list)list, key);
            GtkToggleButton *toggle = value;
            gtk_toggle_button_set_active(toggle, fp_state == XD_FP_ON);
            gtk_toggle_button_set_inconsistent(toggle, fp_state == XD_FP_CONDITIONAL);
            if (fp_state == XD_FP_CONDITIONAL) {
                char *tip = conditions_text(
                    xd_fp_perms_conditions(app_dialog->effective, (xd_fp_list)list, key));
                gtk_widget_set_tooltip_text(GTK_WIDGET(toggle), tip);
                g_free(tip);
            } else {
                gtk_widget_set_tooltip_text(GTK_WIDGET(toggle), key);
            }
            /* Bold when it's the user's doing. */
            GtkWidget *label = gtk_bin_get_child(GTK_BIN(toggle));
            PangoAttrList *attrs = pango_attr_list_new();
            if (xd_fp_perms_state(app_dialog->mine, (xd_fp_list)list, key) != XD_FP_UNSET)
                pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            gtk_label_set_attributes(GTK_LABEL(label), attrs);
            pango_attr_list_unref(attrs);
        }
    }
}

static GtkWidget *check_frame(dialog *app_dialog, xd_fp_list list, const char *title) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), list != XD_FP_SHARED);
    app_dialog->check_boxes[list] = grid;
    for (const char *const *name = xd_fp_list_names(list); *name != nullptr; name++)
        add_check(app_dialog, list, *name);
    return xd_fp_frame(title, grid);
}

static GtkWidget *page_box(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    return page;
}

static GtkWidget *general_page(dialog *app_dialog) {
    GtkWidget *page = page_box();
    GtkWidget *head = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(head), 12);
    GIcon *icon = app_dialog->app->icon != nullptr ? g_object_ref(app_dialog->app->icon)
                                                   : g_themed_icon_new("application-x-executable");
    GtkWidget *img = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    g_object_unref(icon);
    gtk_grid_attach(GTK_GRID(head), img, 0, 0, 1, 2);
    GtkWidget *name = gtk_label_new(app_dialog->app->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_selectable(GTK_LABEL(name), TRUE);
    gtk_grid_attach(GTK_GRID(head), name, 1, 0, 1, 1);
    char *who = g_ascii_strdown(xd_fp_installation_label(app_dialog->app->inst), -1);
    char *where =
        g_strdup_printf("%s, branch %s, installed for %s", app_dialog->app->id,
                        app_dialog->app->branch != nullptr ? app_dialog->app->branch : "?", who);
    g_free(who);
    GtkWidget *id = gtk_label_new(where);
    gtk_label_set_xalign(GTK_LABEL(id), 0);
    gtk_label_set_selectable(GTK_LABEL(id), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(id), PANGO_ELLIPSIZE_END);
    g_free(where);
    gtk_grid_attach(GTK_GRID(head), id, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       check_frame(app_dialog, XD_FP_SHARED, "Share with this computer"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), check_frame(app_dialog, XD_FP_SOCKETS, "Sockets"), FALSE,
                       FALSE, 0);
    return page;
}

static GtkWidget *devices_page(dialog *app_dialog) {
    GtkWidget *page = page_box();
    gtk_box_pack_start(GTK_BOX(page), check_frame(app_dialog, XD_FP_DEVICES, "Devices"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), check_frame(app_dialog, XD_FP_FEATURES, "Features"), FALSE,
                       FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page),
                       xd_fp_label("Devices are shared only as far as your own account may use "
                                   "them. Bluetooth and CAN bus need the network too."),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- lists ---------------------------------------------------------------------- */

static GtkListStore *list_store(void) {
    return gtk_list_store_new(ROW_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
}

/* The selected row's key, or nullptr. g_free it. */
static char *selected_key(GtkWidget *view) {
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &model,
                                         &it))
        return nullptr;
    char *key;
    gtk_tree_model_get(model, &it, ROW_KEY, &key, -1);
    return key;
}

static void select_key(GtkWidget *view, const char *key) {
    if (key == nullptr)
        return;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    GtkTreeIter it;
    for (bool ok = gtk_tree_model_get_iter_first(model, &it); ok;
         ok = gtk_tree_model_iter_next(model, &it)) {
        char *row_key;
        gtk_tree_model_get(model, &it, ROW_KEY, &row_key, -1);
        bool match = g_strcmp0(row_key, key) == 0;
        g_free(row_key);
        if (match) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(view)), &it);
            return;
        }
    }
}

static GtkWidget *list_view(GtkListStore *store, const char *first, const char *second) {
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    xd_fp_text_column(view, first, ROW_TEXT, ROW_WEIGHT);
    if (second != nullptr)
        xd_fp_text_column(view, second, ROW_VALUE, ROW_WEIGHT);
    return view;
}

/* A list with its buttons on the right of a row below it. */
static GtkWidget *list_with_buttons(GtkWidget *label, GtkWidget *view, int height,
                                    GtkWidget *const *buttons, int button_count) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (label != nullptr) {
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), view);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), xd_fp_scrolled(view, height), TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = button_count; i-- > 0;)
        gtk_box_pack_end(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

/* A small modal dialog with OK and Cancel around `content`. */
static GtkWidget *small_dialog(dialog *app_dialog, const char *title, GtkWidget *content) {
    GtkWidget *widget = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(widget), title);
    gtk_window_set_transient_for(GTK_WINDOW(widget), GTK_WINDOW(app_dialog->window));
    gtk_window_set_modal(GTK_WINDOW(widget), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), content, TRUE,
                       TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    return widget;
}

static GtkWidget *grid_row(GtkWidget *grid, int row, const char *mnemonic, GtkWidget *field) {
    GtkWidget *label = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), field);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
    return label;
}

static GtkWidget *form_grid(void) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    return grid;
}

static char *entry_text(GtkWidget *entry) {
    return g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
}

/* ---- File System ------------------------------------------------------------- */

typedef struct fs_keyword {
    const char *keyword, *label;
} fs_keyword;

static const fs_keyword fs_keywords[] = {
    {"home", "Home folder"},
    {"host", "All files"},
    {"host-os", "System programs and libraries"},
    {"host-etc", "System settings (/etc)"},
    {"host-root", "The whole system"},
    {"xdg-desktop", "Desktop"},
    {"xdg-documents", "Documents"},
    {"xdg-download", "Downloads"},
    {"xdg-music", "Music"},
    {"xdg-pictures", "Pictures"},
    {"xdg-public-share", "Public"},
    {"xdg-videos", "Videos"},
    {"xdg-templates", "Templates"},
    {"xdg-cache", "Cache folder"},
    {"xdg-config", "Settings folder"},
    {"xdg-data", "Data folder"},
};

static char *fs_text(const char *fs) {
    for (size_t i = 0; i < G_N_ELEMENTS(fs_keywords); i++) {
        if (strcmp(fs_keywords[i].keyword, fs) == 0)
            return g_strdup_printf("%s (%s)", fs_keywords[i].label, fs);
    }
    return g_strdup(fs);
}

static const char *const mode_names[] = {
    [XD_FP_FS_NONE] = "No access",
    [XD_FP_FS_RO] = "Read only",
    [XD_FP_FS_RW] = "Read and write",
    [XD_FP_FS_CREATE] = "Read and write, created if missing",
};

static void show_filesystems(dialog *app_dialog) {
    char *keep = selected_key(app_dialog->fs_view);
    gtk_list_store_clear(app_dialog->fs);
    GPtrArray *all = xd_fp_perms_filesystems(app_dialog->effective);
    for (guint i = 0; i < all->len; i++) {
        const char *fs = g_ptr_array_index(all, i);
        bool found, mine;
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(app_dialog->effective, fs, &found);
        xd_fp_perms_filesystem(app_dialog->mine, fs, &mine);
        char *text = fs_text(fs);
        gtk_list_store_insert_with_values(app_dialog->fs, nullptr, -1, ROW_TEXT, text, ROW_VALUE,
                                          mode_names[mode], ROW_KEY, fs, ROW_WEIGHT, weight(mine),
                                          -1);
        g_free(text);
    }
    g_ptr_array_unref(all);
    select_key(app_dialog->fs_view, keep);
    g_free(keep);

    keep = selected_key(app_dialog->persist_view);
    gtk_list_store_clear(app_dialog->persist);
    GPtrArray *dirs = xd_fp_perms_persistent(app_dialog->effective);
    GPtrArray *mine = xd_fp_perms_persistent(app_dialog->mine);
    for (guint i = 0; i < dirs->len; i++) {
        const char *dir = g_ptr_array_index(dirs, i);
        bool own = g_ptr_array_find_with_equal_func(mine, dir, g_str_equal, nullptr);
        char *text = g_strconcat("~/", dir, nullptr);
        gtk_list_store_insert_with_values(app_dialog->persist, nullptr, -1, ROW_TEXT, text, ROW_KEY,
                                          dir, ROW_WEIGHT, weight(own), -1);
        g_free(text);
    }
    g_ptr_array_unref(mine);
    g_ptr_array_unref(dirs);
    select_key(app_dialog->persist_view, keep);
    g_free(keep);
}

static void fs_buttons(dialog *app_dialog) {
    char *fs = selected_key(app_dialog->fs_view);
    bool found = false, mine = false;
    if (fs != nullptr) {
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(app_dialog->effective, fs, &found);
        xd_fp_perms_filesystem(app_dialog->mine, fs, &mine);
        /* A filesystem the app is denied anyway has nothing to remove. */
        found = mode != XD_FP_FS_NONE || mine;
    }
    gtk_widget_set_sensitive(app_dialog->fs_edit, fs != nullptr);
    gtk_widget_set_sensitive(app_dialog->fs_remove, found);
    g_free(fs);
    char *dir = selected_key(app_dialog->persist_view);
    GPtrArray *base = xd_fp_perms_persistent(app_dialog->base);
    /* Only the user's own can go: flatpak has no "!" for these. */
    gtk_widget_set_sensitive(
        app_dialog->persist_remove,
        dir != nullptr && !g_ptr_array_find_with_equal_func(base, dir, g_str_equal, nullptr));
    g_ptr_array_unref(base);
    g_free(dir);
}

static void on_fs_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    fs_buttons(data);
}

/* Sets a filesystem's access, leaving no entry when the app has it so. */
static void set_filesystem(dialog *app_dialog, const char *fs, xd_fp_fs_mode mode) {
    bool found;
    xd_fp_fs_mode base = xd_fp_perms_filesystem(app_dialog->base, fs, &found);
    bool same = found ? base == mode : mode == XD_FP_FS_NONE;
    xd_fp_override_set_filesystem(app_dialog->override, fs, mode, same);
    changed(app_dialog);
}

static void on_browse(GtkButton *button, gpointer data) {
    GtkWidget *entry = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Browse for Folder", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button))),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL, "OK",
        GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        const char *home = g_get_home_dir();
        /* Inside the home folder: ~/path, which follows the user. */
        char *text = path != nullptr && g_str_has_prefix(path, home) &&
                             path[strlen(home)] == '/'
                         ? g_strconcat("~", path + strlen(home), nullptr)
                         : g_strdup(path);
        gtk_entry_set_text(GTK_ENTRY(entry), text != nullptr ? text : "");
        g_free(text);
        g_free(path);
    }
    gtk_widget_destroy(chooser);
}

/* Add (fs nullptr) or Edit. */
static void edit_filesystem(dialog *app_dialog, const char *fs) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = form_grid();
    GtkWidget *where = gtk_combo_box_text_new_with_entry();
    for (size_t i = 0; i < G_N_ELEMENTS(fs_keywords); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(where), fs_keywords[i].keyword);
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(where));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_size_request(where, 260, -1);
    grid_row(grid, 0, "_Location:", where);
    GtkWidget *browse = gtk_button_new_with_mnemonic("_Browse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse), entry);
    gtk_grid_attach(GTK_GRID(grid), browse, 2, 0, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       xd_fp_label("A folder (/path or ~/path), or one of Flatpak's names: home, "
                                   "host, xdg-download, xdg-config/app and so on."),
                       FALSE, FALSE, 0);
    GtkWidget *modes = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *radio[4] = {nullptr};
    static const char *const mnemonics[] = {
        [XD_FP_FS_NONE] = "_No access",
        [XD_FP_FS_RO] = "Read _only",
        [XD_FP_FS_RW] = "Read and _write",
        [XD_FP_FS_CREATE] = "Read and write, _create it if missing",
    };
    static const xd_fp_fs_mode order[] = {XD_FP_FS_RW, XD_FP_FS_RO, XD_FP_FS_CREATE,
                                          XD_FP_FS_NONE};
    for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
        radio[order[i]] = gtk_radio_button_new_with_mnemonic_from_widget(
            i > 0 ? GTK_RADIO_BUTTON(radio[order[0]]) : nullptr, mnemonics[order[i]]);
        gtk_box_pack_start(GTK_BOX(modes), radio[order[i]], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), xd_fp_frame("Access", modes), FALSE, FALSE, 0);
    if (fs != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(entry), fs);
        gtk_widget_set_sensitive(where, FALSE);
        gtk_widget_set_sensitive(browse, FALSE);
        bool found;
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(app_dialog->effective, fs, &found);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio[mode]), TRUE);
    }
    GtkWidget *widget =
        small_dialog(app_dialog, fs == nullptr ? "Add Location" : "Edit Location", box);
    gtk_widget_show_all(widget);
    while (gtk_dialog_run(GTK_DIALOG(widget)) == GTK_RESPONSE_OK) {
        char *text = entry_text(entry);
        if (!xd_fp_filesystem_valid(text)) {
            xd_message_box(GTK_WINDOW(widget), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a folder that starts with / or ~/, or one of Flatpak's names "
                           "for folders, such as home or xdg-download.");
            g_free(text);
            continue;
        }
        xd_fp_fs_mode mode = XD_FP_FS_RW;
        for (int mode_index = 0; mode_index < 4; mode_index++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio[mode_index])))
                mode = (xd_fp_fs_mode)mode_index;
        }
        xd_fp_fs_mode ignored;
        char *canon = xd_fp_filesystem_parse(text, &ignored);
        set_filesystem(app_dialog, canon, mode);
        select_key(app_dialog->fs_view, canon);
        g_free(canon);
        g_free(text);
        break;
    }
    gtk_widget_destroy(widget);
}

static void on_fs_add([[maybe_unused]] GtkButton *button, gpointer data) {
    edit_filesystem(data, nullptr);
}

static void on_fs_edit([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    char *fs = selected_key(app_dialog->fs_view);
    if (fs != nullptr)
        edit_filesystem(app_dialog, fs);
    g_free(fs);
}

static void on_fs_activated([[maybe_unused]] GtkTreeView *tree_view,
                            [[maybe_unused]] GtkTreePath *path,
                            [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_fs_edit(nullptr, data);
}

/* Remove: the user's own entry goes; one the app has is taken away. */
static void on_fs_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    char *fs = selected_key(app_dialog->fs_view);
    if (fs == nullptr)
        return;
    bool found;
    xd_fp_fs_mode base = xd_fp_perms_filesystem(app_dialog->base, fs, &found);
    if (found && base != XD_FP_FS_NONE) {
        set_filesystem(app_dialog, fs, XD_FP_FS_NONE);
    } else {
        xd_fp_override_set_filesystem(app_dialog->override, fs, XD_FP_FS_NONE, true);
        changed(app_dialog);
    }
    g_free(fs);
    fs_buttons(app_dialog);
}

static void on_persist_add([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = form_grid();
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_widget_set_size_request(entry, 240, -1);
    grid_row(grid, 0, "_Folder:", entry);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       xd_fp_label("A folder in your home folder, such as .myapp. The "
                                   "application keeps it in its own data folder instead."),
                       FALSE, FALSE, 0);
    GtkWidget *widget = small_dialog(app_dialog, "Add Persistent Folder", box);
    gtk_widget_show_all(widget);
    while (gtk_dialog_run(GTK_DIALOG(widget)) == GTK_RESPONSE_OK) {
        char *text = entry_text(entry);
        const char *rel = g_str_has_prefix(text, "~/") ? text + 2 : text;
        if (*rel == '\0' || *rel == '/' || strstr(rel, "..") != nullptr) {
            xd_message_box(GTK_WINDOW(widget), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a folder inside your home folder, without a leading /.");
            g_free(text);
            continue;
        }
        xd_fp_override_set_persistent(app_dialog->override, rel, true);
        changed(app_dialog);
        select_key(app_dialog->persist_view, rel);
        g_free(text);
        break;
    }
    gtk_widget_destroy(widget);
}

static void on_persist_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    char *dir = selected_key(app_dialog->persist_view);
    if (dir != nullptr) {
        xd_fp_override_set_persistent(app_dialog->override, dir, false);
        changed(app_dialog);
    }
    g_free(dir);
    fs_buttons(app_dialog);
}

static GtkWidget *filesystem_page(dialog *app_dialog) {
    GtkWidget *page = page_box();
    app_dialog->fs = list_store();
    app_dialog->fs_view = list_view(app_dialog->fs, "Location", "Access");
    GtkWidget *add = xd_fp_button("A_dd...");
    app_dialog->fs_edit = xd_fp_button("_Edit...");
    app_dialog->fs_remove = xd_fp_button("_Remove");
    g_signal_connect(add, "clicked", G_CALLBACK(on_fs_add), app_dialog);
    g_signal_connect(app_dialog->fs_edit, "clicked", G_CALLBACK(on_fs_edit), app_dialog);
    g_signal_connect(app_dialog->fs_remove, "clicked", G_CALLBACK(on_fs_remove), app_dialog);
    g_signal_connect(app_dialog->fs_view, "row-activated", G_CALLBACK(on_fs_activated), app_dialog);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app_dialog->fs_view)), "changed",
                     G_CALLBACK(on_fs_selected), app_dialog);
    GtkWidget *buttons[] = {add, app_dialog->fs_edit, app_dialog->fs_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic("F_iles and folders:"),
                                         app_dialog->fs_view, 150, buttons, 3),
                       TRUE, TRUE, 0);

    app_dialog->persist = list_store();
    app_dialog->persist_view = list_view(app_dialog->persist, "Folder", nullptr);
    GtkWidget *padd = xd_fp_button("Add...");
    app_dialog->persist_remove = xd_fp_button("Remove");
    g_signal_connect(padd, "clicked", G_CALLBACK(on_persist_add), app_dialog);
    g_signal_connect(app_dialog->persist_remove, "clicked", G_CALLBACK(on_persist_remove),
                     app_dialog);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app_dialog->persist_view)),
                     "changed", G_CALLBACK(on_fs_selected), app_dialog);
    GtkWidget *pbuttons[] = {padd, app_dialog->persist_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic(
                                             "_Persistent folders (kept in the application's "
                                             "own data folder):"),
                                         app_dialog->persist_view, 70, pbuttons, 2),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Environment -------------------------------------------------------------- */

static void show_env(dialog *app_dialog) {
    char *keep = selected_key(app_dialog->env_view);
    gtk_list_store_clear(app_dialog->env);
    GPtrArray *names = xd_fp_perms_env_names(app_dialog->effective);
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        bool found, mine;
        const char *value = xd_fp_perms_env(app_dialog->effective, name, &found);
        xd_fp_perms_env(app_dialog->mine, name, &mine);
        gtk_list_store_insert_with_values(app_dialog->env, nullptr, -1, ROW_TEXT, name, ROW_VALUE,
                                          value != nullptr ? value : "(removed)", ROW_KEY, name,
                                          ROW_WEIGHT, weight(mine), -1);
    }
    g_ptr_array_unref(names);
    select_key(app_dialog->env_view, keep);
    g_free(keep);
}

static void env_buttons(dialog *app_dialog) {
    char *name = selected_key(app_dialog->env_view);
    bool removable = false;
    if (name != nullptr) {
        bool found, mine;
        const char *value = xd_fp_perms_env(app_dialog->effective, name, &found);
        xd_fp_perms_env(app_dialog->mine, name, &mine);
        removable = value != nullptr || mine;
    }
    gtk_widget_set_sensitive(app_dialog->env_edit, name != nullptr);
    gtk_widget_set_sensitive(app_dialog->env_remove, removable);
    g_free(name);
}

static void on_env_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    env_buttons(data);
}

static void set_env(dialog *app_dialog, const char *name, const char *value) {
    bool found;
    const char *base = xd_fp_perms_env(app_dialog->base, name, &found);
    bool same = found ? g_strcmp0(base, value) == 0 : value == nullptr;
    xd_fp_override_set_env(app_dialog->override, name, value, same);
    changed(app_dialog);
}

static void edit_env(dialog *app_dialog, const char *name) {
    GtkWidget *grid = form_grid();
    GtkWidget *name_entry = gtk_entry_new();
    GtkWidget *value_entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(value_entry), TRUE);
    gtk_widget_set_size_request(value_entry, 260, -1);
    grid_row(grid, 0, "Variable _name:", name_entry);
    grid_row(grid, 1, "Variable _value:", value_entry);
    if (name != nullptr) {
        bool found;
        const char *value = xd_fp_perms_env(app_dialog->effective, name, &found);
        gtk_entry_set_text(GTK_ENTRY(name_entry), name);
        gtk_entry_set_text(GTK_ENTRY(value_entry), value != nullptr ? value : "");
        gtk_widget_set_sensitive(name_entry, FALSE);
    }
    /* XP's Environment Variables wording. */
    GtkWidget *edit_dialog = small_dialog(
        app_dialog, name == nullptr ? "New User Variable" : "Edit User Variable", grid);
    gtk_widget_show_all(edit_dialog);
    if (name != nullptr)
        gtk_widget_grab_focus(value_entry);
    while (gtk_dialog_run(GTK_DIALOG(edit_dialog)) == GTK_RESPONSE_OK) {
        char *key = entry_text(name_entry);
        if (!xd_fp_env_name_valid(key)) {
            xd_message_box(GTK_WINDOW(edit_dialog), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "A variable name is letters, digits and underscores, and doesn't "
                           "start with a digit.");
            g_free(key);
            continue;
        }
        set_env(app_dialog, key, gtk_entry_get_text(GTK_ENTRY(value_entry)));
        select_key(app_dialog->env_view, key);
        g_free(key);
        break;
    }
    gtk_widget_destroy(edit_dialog);
}

static void on_env_add([[maybe_unused]] GtkButton *button, gpointer data) {
    edit_env(data, nullptr);
}

static void on_env_edit([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    char *name = selected_key(app_dialog->env_view);
    if (name != nullptr)
        edit_env(app_dialog, name);
    g_free(name);
}

static void on_env_activated([[maybe_unused]] GtkTreeView *tree_view,
                             [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_env_edit(nullptr, data);
}

/* Remove: a variable the app sets is unset; one only the user set (or
 * unset) goes back to what the app has. */
static void on_env_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    char *name = selected_key(app_dialog->env_view);
    if (name == nullptr)
        return;
    bool found;
    const char *value = xd_fp_perms_env(app_dialog->effective, name, &found);
    if (value != nullptr) {
        set_env(app_dialog, name, nullptr);
    } else {
        xd_fp_override_set_env(app_dialog->override, name, nullptr, true);
        changed(app_dialog);
    }
    g_free(name);
    env_buttons(app_dialog);
}

static GtkWidget *environment_page(dialog *app_dialog) {
    GtkWidget *page = page_box();
    app_dialog->env = list_store();
    app_dialog->env_view = list_view(app_dialog->env, "Variable", "Value");
    GtkWidget *add = xd_fp_button("Ne_w...");
    app_dialog->env_edit = xd_fp_button("_Edit...");
    app_dialog->env_remove = xd_fp_button("_Delete");
    g_signal_connect(add, "clicked", G_CALLBACK(on_env_add), app_dialog);
    g_signal_connect(app_dialog->env_edit, "clicked", G_CALLBACK(on_env_edit), app_dialog);
    g_signal_connect(app_dialog->env_remove, "clicked", G_CALLBACK(on_env_remove), app_dialog);
    g_signal_connect(app_dialog->env_view, "row-activated", G_CALLBACK(on_env_activated),
                     app_dialog);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app_dialog->env_view)), "changed",
                     G_CALLBACK(on_env_selected), app_dialog);
    GtkWidget *buttons[] = {add, app_dialog->env_edit, app_dialog->env_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic(
                                             "En_vironment variables the application starts "
                                             "with:"),
                                         app_dialog->env_view, 200, buttons, 3),
                       TRUE, TRUE, 0);
    return page;
}

/* ---- D-Bus --------------------------------------------------------------------- */

static const char *const policies[] = {"none", "see", "talk", "own"};
static const char *const policy_names[] = {"None", "See", "Talk", "Own"};

static const char *policy_name(const char *policy) {
    for (size_t i = 0; i < G_N_ELEMENTS(policies); i++) {
        if (g_strcmp0(policies[i], policy) == 0)
            return policy_names[i];
    }
    return policy;
}

static void show_bus(dialog *app_dialog) {
    for (int bus = 0; bus < XD_FP_BUS_COUNT; bus++) {
        char *keep = selected_key(app_dialog->bus_view[bus]);
        gtk_list_store_clear(app_dialog->bus[bus]);
        GPtrArray *names = xd_fp_perms_bus_names(app_dialog->effective, (xd_fp_bus)bus);
        for (guint i = 0; i < names->len; i++) {
            const char *name = g_ptr_array_index(names, i);
            const char *policy = xd_fp_perms_bus(app_dialog->effective, (xd_fp_bus)bus, name);
            bool mine = xd_fp_perms_bus(app_dialog->mine, (xd_fp_bus)bus, name) != nullptr;
            gtk_list_store_insert_with_values(app_dialog->bus[bus], nullptr, -1, ROW_TEXT, name,
                                              ROW_VALUE, policy_name(policy), ROW_KEY, name,
                                              ROW_WEIGHT, weight(mine), -1);
        }
        g_ptr_array_unref(names);
        select_key(app_dialog->bus_view[bus], keep);
        g_free(keep);
    }
}

static void bus_buttons(dialog *app_dialog) {
    for (int bus = 0; bus < XD_FP_BUS_COUNT; bus++) {
        char *name = selected_key(app_dialog->bus_view[bus]);
        bool removable =
            name != nullptr &&
            (g_strcmp0(xd_fp_perms_bus(app_dialog->effective, (xd_fp_bus)bus, name), "none") != 0 ||
             xd_fp_perms_bus(app_dialog->mine, (xd_fp_bus)bus, name) != nullptr);
        gtk_widget_set_sensitive(app_dialog->bus_edit[bus], name != nullptr);
        gtk_widget_set_sensitive(app_dialog->bus_remove[bus], removable);
        g_free(name);
    }
}

static void on_bus_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    bus_buttons(data);
}

static void set_bus(dialog *app_dialog, xd_fp_bus bus, const char *name, const char *policy) {
    const char *base = xd_fp_perms_bus(app_dialog->base, bus, name);
    bool same = base != nullptr ? g_strcmp0(base, policy) == 0 : strcmp(policy, "none") == 0;
    xd_fp_override_set_bus(app_dialog->override, bus, name, same ? nullptr : policy);
    changed(app_dialog);
}

static xd_fp_bus button_bus(GtkWidget *widget) {
    return (xd_fp_bus)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "bus"));
}

static void edit_bus(dialog *app_dialog, xd_fp_bus bus, const char *name) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = form_grid();
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    gtk_widget_set_size_request(name_entry, 260, -1);
    GtkWidget *access = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(policies); i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(access), policies[i], policy_names[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(access), "talk");
    grid_row(grid, 0, "Bus _name:", name_entry);
    grid_row(grid, 1, "_Access:", access);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       xd_fp_label("A name such as org.freedesktop.Notifications, or the names "
                                   "under one: org.example.*. See: the name is visible; Talk: "
                                   "the application can call it; Own: it can also take the "
                                   "name; None: the name is hidden."),
                       FALSE, FALSE, 0);
    if (name != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(name_entry), name);
        gtk_widget_set_sensitive(name_entry, FALSE);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(access),
                                    xd_fp_perms_bus(app_dialog->effective, bus, name));
    }
    GtkWidget *edit_dialog = small_dialog(
        app_dialog, bus == XD_FP_SESSION_BUS ? "Session Bus Name" : "System Bus Name", box);
    gtk_widget_show_all(edit_dialog);
    while (gtk_dialog_run(GTK_DIALOG(edit_dialog)) == GTK_RESPONSE_OK) {
        char *key = entry_text(name_entry);
        const char *policy = gtk_combo_box_get_active_id(GTK_COMBO_BOX(access));
        if (!xd_fp_bus_name_valid(key) || policy == nullptr) {
            xd_message_box(GTK_WINDOW(edit_dialog), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a D-Bus name of two or more parts separated by dots, such as "
                           "org.example.Service, or org.example.* for all the names under it.");
            g_free(key);
            continue;
        }
        set_bus(app_dialog, bus, key, policy);
        select_key(app_dialog->bus_view[bus], key);
        g_free(key);
        break;
    }
    gtk_widget_destroy(edit_dialog);
}

static void on_bus_add(GtkButton *button, gpointer data) {
    edit_bus(data, button_bus(GTK_WIDGET(button)), nullptr);
}

static void on_bus_edit(GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    xd_fp_bus bus = button_bus(GTK_WIDGET(button));
    char *name = selected_key(app_dialog->bus_view[bus]);
    if (name != nullptr)
        edit_bus(app_dialog, bus, name);
    g_free(name);
}

static void on_bus_activated(GtkTreeView *tree_view, [[maybe_unused]] GtkTreePath *path,
                             [[maybe_unused]] GtkTreeViewColumn *column, gpointer data) {
    on_bus_edit(GTK_BUTTON(g_object_get_data(G_OBJECT(tree_view), "edit")), data);
}

/* Remove: a name the app may use becomes None; the user's own entry for
 * one it may not goes. */
static void on_bus_remove(GtkButton *button, gpointer data) {
    dialog *app_dialog = data;
    xd_fp_bus bus = button_bus(GTK_WIDGET(button));
    char *name = selected_key(app_dialog->bus_view[bus]);
    if (name == nullptr)
        return;
    const char *base = xd_fp_perms_bus(app_dialog->base, bus, name);
    if (base != nullptr && strcmp(base, "none") != 0 &&
        g_strcmp0(xd_fp_perms_bus(app_dialog->effective, bus, name), "none") != 0) {
        set_bus(app_dialog, bus, name, "none");
    } else {
        xd_fp_override_set_bus(app_dialog->override, bus, name, nullptr);
        changed(app_dialog);
    }
    g_free(name);
    bus_buttons(app_dialog);
}

static GtkWidget *bus_list(dialog *app_dialog, xd_fp_bus bus, const char *mnemonic) {
    app_dialog->bus[bus] = list_store();
    app_dialog->bus_view[bus] = list_view(app_dialog->bus[bus], "Name", "Access");
    GtkWidget *add = xd_fp_button(bus == XD_FP_SESSION_BUS ? "A_dd..." : "Add...");
    app_dialog->bus_edit[bus] = xd_fp_button(bus == XD_FP_SESSION_BUS ? "_Edit..." : "Edit...");
    app_dialog->bus_remove[bus] = xd_fp_button(bus == XD_FP_SESSION_BUS ? "_Remove" : "Remove");
    GtkWidget *buttons[] = {add, app_dialog->bus_edit[bus], app_dialog->bus_remove[bus]};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++)
        g_object_set_data(G_OBJECT(buttons[i]), "bus", GINT_TO_POINTER(bus));
    g_object_set_data(G_OBJECT(app_dialog->bus_view[bus]), "edit", app_dialog->bus_edit[bus]);
    g_signal_connect(add, "clicked", G_CALLBACK(on_bus_add), app_dialog);
    g_signal_connect(app_dialog->bus_edit[bus], "clicked", G_CALLBACK(on_bus_edit), app_dialog);
    g_signal_connect(app_dialog->bus_remove[bus], "clicked", G_CALLBACK(on_bus_remove), app_dialog);
    g_signal_connect(app_dialog->bus_view[bus], "row-activated", G_CALLBACK(on_bus_activated),
                     app_dialog);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(app_dialog->bus_view[bus])),
                     "changed", G_CALLBACK(on_bus_selected), app_dialog);
    return list_with_buttons(gtk_label_new_with_mnemonic(mnemonic), app_dialog->bus_view[bus], 90,
                             buttons, 3);
}

static GtkWidget *dbus_page(dialog *app_dialog) {
    GtkWidget *page = page_box();
    gtk_box_pack_start(GTK_BOX(page),
                       xd_fp_label("Besides these, an application may always use its own names "
                                   "and the desktop portals on the session bus."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bus_list(app_dialog, XD_FP_SESSION_BUS, "_Session bus:"),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), bus_list(app_dialog, XD_FP_SYSTEM_BUS, "S_ystem bus:"), TRUE,
                       TRUE, 0);
    return page;
}

/* ---- the dialog ------------------------------------------------------------------ */

static void refresh(dialog *app_dialog) {
    bool was = app_dialog->updating;
    app_dialog->updating = true;
    show_checks(app_dialog);
    show_filesystems(app_dialog);
    show_env(app_dialog);
    show_bus(app_dialog);
    app_dialog->updating = was;
    fs_buttons(app_dialog);
    env_buttons(app_dialog);
    bus_buttons(app_dialog);
}

static bool apply(dialog *app_dialog) {
    GError *error = nullptr;
    if (!xd_fp_override_save(app_dialog->override, app_dialog->path, &error)) {
        xd_fp_error(GTK_WINDOW(app_dialog->window), "The permissions could not be saved.", error);
        g_error_free(error);
        return false;
    }
    app_dialog->saved = true;
    gtk_widget_set_sensitive(app_dialog->apply, FALSE);
    return true;
}

enum { RESPONSE_DEFAULTS = 1 };

bool xd_fp_app_dialog(GtkWindow *parent, const xd_fp_app *app) {
    dialog app_dialog = {.app = app};
    app_dialog.path = xd_fp_user_override_path(app->id);
    GError *error = nullptr;
    app_dialog.override = xd_fp_override_load(app_dialog.path, &error);
    if (app_dialog.override == nullptr) {
        xd_fp_error(parent, "Your changes to this application's permissions could not be read.",
                    error);
        g_error_free(error);
        g_free(app_dialog.path);
        return false;
    }
    app_dialog.base = load_base(app);
    for (int list = 0; list < XD_FP_LIST_COUNT; list++)
        app_dialog.checks[list] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);

    app_dialog.window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Permissions", app->name);
    gtk_window_set_title(GTK_WINDOW(app_dialog.window), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(app_dialog.window), parent);
    gtk_window_set_modal(GTK_WINDOW(app_dialog.window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(app_dialog.window), FALSE);
    GtkWidget *defaults = gtk_button_new_with_mnemonic("Restore De_faults");
    gtk_widget_set_tooltip_text(defaults, "Give the application back the permissions it came "
                                          "with");
    gtk_dialog_add_action_widget(GTK_DIALOG(app_dialog.window), defaults, RESPONSE_DEFAULTS);
    gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(gtk_widget_get_parent(defaults)), defaults,
                                       TRUE);
    gtk_dialog_add_button(GTK_DIALOG(app_dialog.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(app_dialog.window), "Cancel", GTK_RESPONSE_CANCEL);
    app_dialog.apply =
        gtk_dialog_add_button(GTK_DIALOG(app_dialog.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(app_dialog.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&app_dialog),
                             gtk_label_new_with_mnemonic("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), devices_page(&app_dialog),
                             gtk_label_new_with_mnemonic("Devices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), filesystem_page(&app_dialog),
                             gtk_label_new_with_mnemonic("File System"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), environment_page(&app_dialog),
                             gtk_label_new_with_mnemonic("Environment"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), dbus_page(&app_dialog),
                             gtk_label_new_with_mnemonic("D-Bus"));
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(app_dialog.window));
    gtk_box_pack_start(GTK_BOX(content), tabs, TRUE, TRUE, 0);
    char *note = g_strdup_printf("Settings you changed are shown in bold. Changes take effect "
                                 "the next time %s starts.",
                                 app->name);
    GtkWidget *note_label = xd_fp_label(note);
    g_free(note);
    gtk_widget_set_margin_start(note_label, 8);
    gtk_widget_set_margin_end(note_label, 8);
    gtk_box_pack_start(GTK_BOX(content), note_label, FALSE, FALSE, 4);
    xd_gtk_notebook_ctrl_tab(app_dialog.window, GTK_NOTEBOOK(tabs));

    app_dialog.updating = true;
    changed(&app_dialog);
    app_dialog.updating = false;
    gtk_widget_set_sensitive(app_dialog.apply, FALSE);
    gtk_widget_show_all(app_dialog.window);
    for (;;) {
        int response = gtk_dialog_run(GTK_DIALOG(app_dialog.window));
        if (response == RESPONSE_DEFAULTS) {
            g_key_file_free(app_dialog.override);
            app_dialog.override = g_key_file_new();
            changed(&app_dialog);
            continue;
        }
        if (response == GTK_RESPONSE_APPLY) {
            apply(&app_dialog);
            continue;
        }
        if (response == GTK_RESPONSE_OK && gtk_widget_get_sensitive(app_dialog.apply) &&
            !apply(&app_dialog))
            continue;
        break;
    }
    gtk_widget_destroy(app_dialog.window);
    for (int list = 0; list < XD_FP_LIST_COUNT; list++)
        g_hash_table_unref(app_dialog.checks[list]);
    xd_fp_perms_free(app_dialog.base);
    xd_fp_perms_free(app_dialog.mine);
    xd_fp_perms_free(app_dialog.effective);
    g_key_file_free(app_dialog.override);
    g_free(app_dialog.path);
    return app_dialog.saved;
}
