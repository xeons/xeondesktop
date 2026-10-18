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

static void refresh(dialog *d);

static void changed(dialog *d) {
    xd_fp_perms_free(d->mine);
    xd_fp_perms_free(d->effective);
    d->mine = xd_fp_perms_new();
    xd_fp_perms_merge(d->mine, d->override);
    d->effective = xd_fp_perms_copy(d->base);
    xd_fp_perms_merge(d->effective, d->override);
    if (d->apply != nullptr && !d->updating)
        gtk_widget_set_sensitive(d->apply, TRUE);
    refresh(d);
}

static int weight(bool mine) { return mine ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL; }

/* ---- the layers -------------------------------------------------------------- */

static void merge_bytes(xd_fp_perms *p, GBytes *bytes) {
    if (bytes == nullptr)
        return;
    GKeyFile *kf = g_key_file_new();
    gsize n;
    const char *data = g_bytes_get_data(bytes, &n);
    if (g_key_file_load_from_data(kf, data, n, G_KEY_FILE_NONE, nullptr))
        xd_fp_perms_merge(p, kf);
    g_key_file_free(kf);
}

static void merge_file(xd_fp_perms *p, const char *path) {
    GKeyFile *kf = xd_fp_override_load(path, nullptr);
    if (kf != nullptr) {
        xd_fp_perms_merge(p, kf);
        g_key_file_free(kf);
    }
}

static char *installation_dir(FlatpakInstallation *inst) {
    GFile *f = flatpak_installation_get_path(inst);
    char *path = g_file_get_path(f);
    g_object_unref(f);
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
        FlatpakInstalledRef *r = flatpak_installation_get_installed_ref(
            g_ptr_array_index(insts, i), FLATPAK_REF_KIND_RUNTIME, parts[0], parts[1], parts[2],
            nullptr, nullptr);
        if (r != nullptr) {
            bytes = flatpak_installed_ref_load_metadata(r, nullptr, nullptr);
            g_object_unref(r);
        }
    }
    g_ptr_array_unref(insts);
    g_strfreev(parts);
    return bytes;
}

/* Everything below the user's own override for the app, in flatpak's
 * order (perms.h). */
static xd_fp_perms *load_base(const xd_fp_app *app) {
    xd_fp_perms *p = xd_fp_perms_new();
    GBytes *bytes = flatpak_installed_ref_load_metadata(app->ref, nullptr, nullptr);
    GKeyFile *metadata = g_key_file_new();
    if (bytes != nullptr) {
        gsize n;
        const char *data = g_bytes_get_data(bytes, &n);
        g_key_file_load_from_data(metadata, data, n, G_KEY_FILE_NONE, nullptr);
    }
    GBytes *runtime = runtime_metadata(app, metadata);
    merge_bytes(p, runtime);
    if (runtime != nullptr)
        g_bytes_unref(runtime);
    merge_bytes(p, bytes);
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
            merge_file(p, global);
            merge_file(p, own);
            g_free(global);
            g_free(own);
            g_free(dir);
            g_object_unref(system);
        }
    }
    char *mine = xd_fp_user_override_path(app->id);
    char *dir = g_path_get_dirname(mine);
    char *global = g_build_filename(dir, "global", nullptr);
    merge_file(p, global);
    g_free(global);
    g_free(dir);
    g_free(mine);
    return p;
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
    GString *s = g_string_new("Only ");
    for (guint i = 0; conds != nullptr && i < conds->len; i++) {
        const char *c = g_ptr_array_index(conds, i);
        bool not = c[0] == '!';
        const char *bare = not ? c + 1 : c;
        const char *what = strcmp(bare, "has-wayland") == 0      ? "in a Wayland session"
                           : strcmp(bare, "has-input-device") == 0 ? "where Flatpak has input devices"
                           : strcmp(bare, "has-usb-device") == 0   ? "where Flatpak has USB devices"
                           : strcmp(bare, "has-usb-portal") == 0   ? "with the USB portal"
                           : strcmp(bare, "true") == 0             ? "always"
                           : strcmp(bare, "false") == 0            ? "never"
                                                                   : bare;
        if (i > 0)
            g_string_append(s, ", or ");
        if (not)
            g_string_append(s, "when not ");
        else if (strcmp(bare, "true") != 0 && strcmp(bare, "false") != 0)
            g_string_append(s, "when ");
        g_string_append(s, what);
    }
    g_string_append(s, ". Click to decide yourself.");
    return g_string_free(s, FALSE);
}

static void on_check(GtkToggleButton *b, gpointer data) {
    dialog *d = data;
    if (d->updating)
        return;
    xd_fp_list list = (xd_fp_list)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "list"));
    const char *name = g_object_get_data(G_OBJECT(b), "name");
    gtk_toggle_button_set_inconsistent(b, FALSE);
    bool on = gtk_toggle_button_get_active(b);
    xd_fp_state base = xd_fp_perms_state(d->base, list, name);
    /* What the app has anyway leaves no entry. */
    bool same = on ? base == XD_FP_ON : base == XD_FP_UNSET || base == XD_FP_OFF;
    xd_fp_override_set_state(d->override, list, name,
                             same ? XD_FP_UNSET : on ? XD_FP_ON : XD_FP_OFF);
    changed(d);
}

static GtkWidget *add_check(dialog *d, xd_fp_list list, const char *name) {
    const char *text = label_for(name);
    GtkWidget *b = text != nullptr ? gtk_check_button_new_with_mnemonic(text)
                                   : gtk_check_button_new_with_label(name);
    g_object_set_data(G_OBJECT(b), "list", GINT_TO_POINTER(list));
    g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name), g_free);
    g_signal_connect(b, "toggled", G_CALLBACK(on_check), d);
    g_hash_table_insert(d->checks[list], g_strdup(name), b);
    GtkWidget *grid = d->check_boxes[list];
    int n = (int)g_hash_table_size(d->checks[list]) - 1;
    /* Two columns, down then across, for the long lists. */
    int columns = list == XD_FP_SHARED ? 1 : 2;
    int known = 0;
    for (const char *const *k = xd_fp_list_names(list); *k != nullptr; k++)
        known++;
    int rows = (known + 1) / columns;
    gtk_grid_attach(GTK_GRID(grid), b, n / MAX(rows, 1), n % MAX(rows, 1), 1, 1);
    gtk_widget_show(b);
    return b;
}

static void show_checks(dialog *d) {
    for (int l = 0; l < XD_FP_LIST_COUNT; l++) {
        /* Names a newer Flatpak knows get a box too, under their own name. */
        GPtrArray *other = xd_fp_perms_other_names(d->effective, (xd_fp_list)l);
        for (guint i = 0; i < other->len; i++) {
            const char *name = g_ptr_array_index(other, i);
            if (!g_hash_table_contains(d->checks[l], name))
                add_check(d, (xd_fp_list)l, name);
        }
        g_ptr_array_unref(other);
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, d->checks[l]);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            xd_fp_state s = xd_fp_perms_state(d->effective, (xd_fp_list)l, k);
            GtkToggleButton *b = v;
            gtk_toggle_button_set_active(b, s == XD_FP_ON);
            gtk_toggle_button_set_inconsistent(b, s == XD_FP_CONDITIONAL);
            if (s == XD_FP_CONDITIONAL) {
                char *tip = conditions_text(xd_fp_perms_conditions(d->effective, (xd_fp_list)l, k));
                gtk_widget_set_tooltip_text(GTK_WIDGET(b), tip);
                g_free(tip);
            } else {
                gtk_widget_set_tooltip_text(GTK_WIDGET(b), k);
            }
            /* Bold when it's the user's doing. */
            GtkWidget *label = gtk_bin_get_child(GTK_BIN(b));
            PangoAttrList *attrs = pango_attr_list_new();
            if (xd_fp_perms_state(d->mine, (xd_fp_list)l, k) != XD_FP_UNSET)
                pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            gtk_label_set_attributes(GTK_LABEL(label), attrs);
            pango_attr_list_unref(attrs);
        }
    }
}

static GtkWidget *check_frame(dialog *d, xd_fp_list list, const char *title) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), list != XD_FP_SHARED);
    d->check_boxes[list] = grid;
    for (const char *const *n = xd_fp_list_names(list); *n != nullptr; n++)
        add_check(d, list, *n);
    return xd_fp_frame(title, grid);
}

static GtkWidget *page_box(void) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    return page;
}

static GtkWidget *general_page(dialog *d) {
    GtkWidget *page = page_box();
    GtkWidget *head = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(head), 12);
    GIcon *icon = d->app->icon != nullptr ? g_object_ref(d->app->icon)
                                          : g_themed_icon_new("application-x-executable");
    GtkWidget *img = gtk_image_new_from_gicon(icon, GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 32);
    g_object_unref(icon);
    gtk_grid_attach(GTK_GRID(head), img, 0, 0, 1, 2);
    GtkWidget *name = gtk_label_new(d->app->name);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_selectable(GTK_LABEL(name), TRUE);
    gtk_grid_attach(GTK_GRID(head), name, 1, 0, 1, 1);
    char *who = g_ascii_strdown(xd_fp_installation_label(d->app->inst), -1);
    char *where = g_strdup_printf("%s, branch %s, installed for %s", d->app->id,
                                  d->app->branch != nullptr ? d->app->branch : "?", who);
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
    gtk_box_pack_start(GTK_BOX(page), check_frame(d, XD_FP_SHARED, "Share with this computer"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), check_frame(d, XD_FP_SOCKETS, "Sockets"), FALSE, FALSE, 0);
    return page;
}

static GtkWidget *devices_page(dialog *d) {
    GtkWidget *page = page_box();
    gtk_box_pack_start(GTK_BOX(page), check_frame(d, XD_FP_DEVICES, "Devices"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), check_frame(d, XD_FP_FEATURES, "Features"), FALSE, FALSE,
                       0);
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
        char *k;
        gtk_tree_model_get(model, &it, ROW_KEY, &k, -1);
        bool match = g_strcmp0(k, key) == 0;
        g_free(k);
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
                                    GtkWidget *const *buttons, int n) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    if (label != nullptr) {
        gtk_label_set_xalign(GTK_LABEL(label), 0);
        gtk_label_set_mnemonic_widget(GTK_LABEL(label), view);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(box), xd_fp_scrolled(view, height), TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    for (int i = n; i-- > 0;)
        gtk_box_pack_end(GTK_BOX(row), buttons[i], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

/* A small modal dialog with OK and Cancel around `content`. */
static GtkWidget *small_dialog(dialog *d, const char *title, GtkWidget *content) {
    GtkWidget *w = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(w), title);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(d->window));
    gtk_window_set_modal(GTK_WINDOW(w), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(content), 10);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(w))), content, TRUE, TRUE,
                       0);
    gtk_dialog_add_button(GTK_DIALOG(w), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(w), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(w), GTK_RESPONSE_OK);
    return w;
}

static GtkWidget *grid_row(GtkWidget *grid, int row, const char *mnemonic, GtkWidget *field) {
    GtkWidget *l = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), field);
    gtk_widget_set_hexpand(field, TRUE);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), field, 1, row, 1, 1);
    return l;
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

static void show_filesystems(dialog *d) {
    char *keep = selected_key(d->fs_view);
    gtk_list_store_clear(d->fs);
    GPtrArray *all = xd_fp_perms_filesystems(d->effective);
    for (guint i = 0; i < all->len; i++) {
        const char *fs = g_ptr_array_index(all, i);
        bool found, mine;
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(d->effective, fs, &found);
        xd_fp_perms_filesystem(d->mine, fs, &mine);
        char *text = fs_text(fs);
        gtk_list_store_insert_with_values(d->fs, nullptr, -1, ROW_TEXT, text, ROW_VALUE,
                                          mode_names[mode], ROW_KEY, fs, ROW_WEIGHT, weight(mine),
                                          -1);
        g_free(text);
    }
    g_ptr_array_unref(all);
    select_key(d->fs_view, keep);
    g_free(keep);

    keep = selected_key(d->persist_view);
    gtk_list_store_clear(d->persist);
    GPtrArray *dirs = xd_fp_perms_persistent(d->effective);
    GPtrArray *mine = xd_fp_perms_persistent(d->mine);
    for (guint i = 0; i < dirs->len; i++) {
        const char *dir = g_ptr_array_index(dirs, i);
        bool own = g_ptr_array_find_with_equal_func(mine, dir, g_str_equal, nullptr);
        char *text = g_strconcat("~/", dir, nullptr);
        gtk_list_store_insert_with_values(d->persist, nullptr, -1, ROW_TEXT, text, ROW_KEY, dir,
                                          ROW_WEIGHT, weight(own), -1);
        g_free(text);
    }
    g_ptr_array_unref(mine);
    g_ptr_array_unref(dirs);
    select_key(d->persist_view, keep);
    g_free(keep);
}

static void fs_buttons(dialog *d) {
    char *fs = selected_key(d->fs_view);
    bool found = false, mine = false;
    if (fs != nullptr) {
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(d->effective, fs, &found);
        xd_fp_perms_filesystem(d->mine, fs, &mine);
        /* A filesystem the app is denied anyway has nothing to remove. */
        found = mode != XD_FP_FS_NONE || mine;
    }
    gtk_widget_set_sensitive(d->fs_edit, fs != nullptr);
    gtk_widget_set_sensitive(d->fs_remove, found);
    g_free(fs);
    char *dir = selected_key(d->persist_view);
    GPtrArray *base = xd_fp_perms_persistent(d->base);
    /* Only the user's own can go: flatpak has no "!" for these. */
    gtk_widget_set_sensitive(d->persist_remove,
                             dir != nullptr &&
                                 !g_ptr_array_find_with_equal_func(base, dir, g_str_equal, nullptr));
    g_ptr_array_unref(base);
    g_free(dir);
}

static void on_fs_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    fs_buttons(data);
}

/* Sets a filesystem's access, leaving no entry when the app has it so. */
static void set_filesystem(dialog *d, const char *fs, xd_fp_fs_mode mode) {
    bool found;
    xd_fp_fs_mode base = xd_fp_perms_filesystem(d->base, fs, &found);
    bool same = found ? base == mode : mode == XD_FP_FS_NONE;
    xd_fp_override_set_filesystem(d->override, fs, mode, same);
    changed(d);
}

static void on_browse(GtkButton *b, gpointer data) {
    GtkWidget *entry = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Browse for Folder", GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(b))),
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
static void edit_filesystem(dialog *d, const char *fs) {
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
        xd_fp_fs_mode mode = xd_fp_perms_filesystem(d->effective, fs, &found);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radio[mode]), TRUE);
    }
    GtkWidget *w = small_dialog(d, fs == nullptr ? "Add Location" : "Edit Location", box);
    gtk_widget_show_all(w);
    while (gtk_dialog_run(GTK_DIALOG(w)) == GTK_RESPONSE_OK) {
        char *text = entry_text(entry);
        if (!xd_fp_filesystem_valid(text)) {
            xd_message_box(GTK_WINDOW(w), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a folder that starts with / or ~/, or one of Flatpak's names "
                           "for folders, such as home or xdg-download.");
            g_free(text);
            continue;
        }
        xd_fp_fs_mode mode = XD_FP_FS_RW;
        for (int m = 0; m < 4; m++) {
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio[m])))
                mode = (xd_fp_fs_mode)m;
        }
        xd_fp_fs_mode ignored;
        char *canon = xd_fp_filesystem_parse(text, &ignored);
        set_filesystem(d, canon, mode);
        select_key(d->fs_view, canon);
        g_free(canon);
        g_free(text);
        break;
    }
    gtk_widget_destroy(w);
}

static void on_fs_add([[maybe_unused]] GtkButton *b, gpointer data) {
    edit_filesystem(data, nullptr);
}

static void on_fs_edit([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
    char *fs = selected_key(d->fs_view);
    if (fs != nullptr)
        edit_filesystem(d, fs);
    g_free(fs);
}

static void on_fs_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                            [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_fs_edit(nullptr, data);
}

/* Remove: the user's own entry goes; one the app has is taken away. */
static void on_fs_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
    char *fs = selected_key(d->fs_view);
    if (fs == nullptr)
        return;
    bool found;
    xd_fp_fs_mode base = xd_fp_perms_filesystem(d->base, fs, &found);
    if (found && base != XD_FP_FS_NONE) {
        set_filesystem(d, fs, XD_FP_FS_NONE);
    } else {
        xd_fp_override_set_filesystem(d->override, fs, XD_FP_FS_NONE, true);
        changed(d);
    }
    g_free(fs);
    fs_buttons(d);
}

static void on_persist_add([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
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
    GtkWidget *w = small_dialog(d, "Add Persistent Folder", box);
    gtk_widget_show_all(w);
    while (gtk_dialog_run(GTK_DIALOG(w)) == GTK_RESPONSE_OK) {
        char *text = entry_text(entry);
        const char *rel = g_str_has_prefix(text, "~/") ? text + 2 : text;
        if (*rel == '\0' || *rel == '/' || strstr(rel, "..") != nullptr) {
            xd_message_box(GTK_WINDOW(w), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a folder inside your home folder, without a leading /.");
            g_free(text);
            continue;
        }
        xd_fp_override_set_persistent(d->override, rel, true);
        changed(d);
        select_key(d->persist_view, rel);
        g_free(text);
        break;
    }
    gtk_widget_destroy(w);
}

static void on_persist_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
    char *dir = selected_key(d->persist_view);
    if (dir != nullptr) {
        xd_fp_override_set_persistent(d->override, dir, false);
        changed(d);
    }
    g_free(dir);
    fs_buttons(d);
}

static GtkWidget *filesystem_page(dialog *d) {
    GtkWidget *page = page_box();
    d->fs = list_store();
    d->fs_view = list_view(d->fs, "Location", "Access");
    GtkWidget *add = xd_fp_button("A_dd...");
    d->fs_edit = xd_fp_button("_Edit...");
    d->fs_remove = xd_fp_button("_Remove");
    g_signal_connect(add, "clicked", G_CALLBACK(on_fs_add), d);
    g_signal_connect(d->fs_edit, "clicked", G_CALLBACK(on_fs_edit), d);
    g_signal_connect(d->fs_remove, "clicked", G_CALLBACK(on_fs_remove), d);
    g_signal_connect(d->fs_view, "row-activated", G_CALLBACK(on_fs_activated), d);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->fs_view)), "changed",
                     G_CALLBACK(on_fs_selected), d);
    GtkWidget *buttons[] = {add, d->fs_edit, d->fs_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic("F_iles and folders:"),
                                         d->fs_view, 150, buttons, 3),
                       TRUE, TRUE, 0);

    d->persist = list_store();
    d->persist_view = list_view(d->persist, "Folder", nullptr);
    GtkWidget *padd = xd_fp_button("Add...");
    d->persist_remove = xd_fp_button("Remove");
    g_signal_connect(padd, "clicked", G_CALLBACK(on_persist_add), d);
    g_signal_connect(d->persist_remove, "clicked", G_CALLBACK(on_persist_remove), d);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->persist_view)), "changed",
                     G_CALLBACK(on_fs_selected), d);
    GtkWidget *pbuttons[] = {padd, d->persist_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic(
                                             "_Persistent folders (kept in the application's "
                                             "own data folder):"),
                                         d->persist_view, 70, pbuttons, 2),
                       FALSE, FALSE, 0);
    return page;
}

/* ---- Environment -------------------------------------------------------------- */

static void show_env(dialog *d) {
    char *keep = selected_key(d->env_view);
    gtk_list_store_clear(d->env);
    GPtrArray *names = xd_fp_perms_env_names(d->effective);
    for (guint i = 0; i < names->len; i++) {
        const char *name = g_ptr_array_index(names, i);
        bool found, mine;
        const char *value = xd_fp_perms_env(d->effective, name, &found);
        xd_fp_perms_env(d->mine, name, &mine);
        gtk_list_store_insert_with_values(d->env, nullptr, -1, ROW_TEXT, name, ROW_VALUE,
                                          value != nullptr ? value : "(removed)", ROW_KEY, name,
                                          ROW_WEIGHT, weight(mine), -1);
    }
    g_ptr_array_unref(names);
    select_key(d->env_view, keep);
    g_free(keep);
}

static void env_buttons(dialog *d) {
    char *name = selected_key(d->env_view);
    bool removable = false;
    if (name != nullptr) {
        bool found, mine;
        const char *value = xd_fp_perms_env(d->effective, name, &found);
        xd_fp_perms_env(d->mine, name, &mine);
        removable = value != nullptr || mine;
    }
    gtk_widget_set_sensitive(d->env_edit, name != nullptr);
    gtk_widget_set_sensitive(d->env_remove, removable);
    g_free(name);
}

static void on_env_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    env_buttons(data);
}

static void set_env(dialog *d, const char *name, const char *value) {
    bool found;
    const char *base = xd_fp_perms_env(d->base, name, &found);
    bool same = found ? g_strcmp0(base, value) == 0 : value == nullptr;
    xd_fp_override_set_env(d->override, name, value, same);
    changed(d);
}

static void edit_env(dialog *d, const char *name) {
    GtkWidget *grid = form_grid();
    GtkWidget *n = gtk_entry_new();
    GtkWidget *v = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(n), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(v), TRUE);
    gtk_widget_set_size_request(v, 260, -1);
    grid_row(grid, 0, "Variable _name:", n);
    grid_row(grid, 1, "Variable _value:", v);
    if (name != nullptr) {
        bool found;
        const char *value = xd_fp_perms_env(d->effective, name, &found);
        gtk_entry_set_text(GTK_ENTRY(n), name);
        gtk_entry_set_text(GTK_ENTRY(v), value != nullptr ? value : "");
        gtk_widget_set_sensitive(n, FALSE);
    }
    /* XP's Environment Variables wording. */
    GtkWidget *w = small_dialog(d, name == nullptr ? "New User Variable" : "Edit User Variable",
                                grid);
    gtk_widget_show_all(w);
    if (name != nullptr)
        gtk_widget_grab_focus(v);
    while (gtk_dialog_run(GTK_DIALOG(w)) == GTK_RESPONSE_OK) {
        char *key = entry_text(n);
        if (!xd_fp_env_name_valid(key)) {
            xd_message_box(GTK_WINDOW(w), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "A variable name is letters, digits and underscores, and doesn't "
                           "start with a digit.");
            g_free(key);
            continue;
        }
        set_env(d, key, gtk_entry_get_text(GTK_ENTRY(v)));
        select_key(d->env_view, key);
        g_free(key);
        break;
    }
    gtk_widget_destroy(w);
}

static void on_env_add([[maybe_unused]] GtkButton *b, gpointer data) { edit_env(data, nullptr); }

static void on_env_edit([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
    char *name = selected_key(d->env_view);
    if (name != nullptr)
        edit_env(d, name);
    g_free(name);
}

static void on_env_activated([[maybe_unused]] GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_env_edit(nullptr, data);
}

/* Remove: a variable the app sets is unset; one only the user set (or
 * unset) goes back to what the app has. */
static void on_env_remove([[maybe_unused]] GtkButton *b, gpointer data) {
    dialog *d = data;
    char *name = selected_key(d->env_view);
    if (name == nullptr)
        return;
    bool found;
    const char *value = xd_fp_perms_env(d->effective, name, &found);
    if (value != nullptr) {
        set_env(d, name, nullptr);
    } else {
        xd_fp_override_set_env(d->override, name, nullptr, true);
        changed(d);
    }
    g_free(name);
    env_buttons(d);
}

static GtkWidget *environment_page(dialog *d) {
    GtkWidget *page = page_box();
    d->env = list_store();
    d->env_view = list_view(d->env, "Variable", "Value");
    GtkWidget *add = xd_fp_button("Ne_w...");
    d->env_edit = xd_fp_button("_Edit...");
    d->env_remove = xd_fp_button("_Delete");
    g_signal_connect(add, "clicked", G_CALLBACK(on_env_add), d);
    g_signal_connect(d->env_edit, "clicked", G_CALLBACK(on_env_edit), d);
    g_signal_connect(d->env_remove, "clicked", G_CALLBACK(on_env_remove), d);
    g_signal_connect(d->env_view, "row-activated", G_CALLBACK(on_env_activated), d);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->env_view)), "changed",
                     G_CALLBACK(on_env_selected), d);
    GtkWidget *buttons[] = {add, d->env_edit, d->env_remove};
    gtk_box_pack_start(GTK_BOX(page),
                       list_with_buttons(gtk_label_new_with_mnemonic(
                                             "En_vironment variables the application starts "
                                             "with:"),
                                         d->env_view, 200, buttons, 3),
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

static void show_bus(dialog *d) {
    for (int b = 0; b < XD_FP_BUS_COUNT; b++) {
        char *keep = selected_key(d->bus_view[b]);
        gtk_list_store_clear(d->bus[b]);
        GPtrArray *names = xd_fp_perms_bus_names(d->effective, (xd_fp_bus)b);
        for (guint i = 0; i < names->len; i++) {
            const char *name = g_ptr_array_index(names, i);
            const char *policy = xd_fp_perms_bus(d->effective, (xd_fp_bus)b, name);
            bool mine = xd_fp_perms_bus(d->mine, (xd_fp_bus)b, name) != nullptr;
            gtk_list_store_insert_with_values(d->bus[b], nullptr, -1, ROW_TEXT, name, ROW_VALUE,
                                              policy_name(policy), ROW_KEY, name, ROW_WEIGHT,
                                              weight(mine), -1);
        }
        g_ptr_array_unref(names);
        select_key(d->bus_view[b], keep);
        g_free(keep);
    }
}

static void bus_buttons(dialog *d) {
    for (int b = 0; b < XD_FP_BUS_COUNT; b++) {
        char *name = selected_key(d->bus_view[b]);
        bool removable = name != nullptr &&
                         (g_strcmp0(xd_fp_perms_bus(d->effective, (xd_fp_bus)b, name), "none") != 0 ||
                          xd_fp_perms_bus(d->mine, (xd_fp_bus)b, name) != nullptr);
        gtk_widget_set_sensitive(d->bus_edit[b], name != nullptr);
        gtk_widget_set_sensitive(d->bus_remove[b], removable);
        g_free(name);
    }
}

static void on_bus_selected([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    bus_buttons(data);
}

static void set_bus(dialog *d, xd_fp_bus bus, const char *name, const char *policy) {
    const char *base = xd_fp_perms_bus(d->base, bus, name);
    bool same = base != nullptr ? g_strcmp0(base, policy) == 0 : strcmp(policy, "none") == 0;
    xd_fp_override_set_bus(d->override, bus, name, same ? nullptr : policy);
    changed(d);
}

static xd_fp_bus button_bus(GtkWidget *b) {
    return (xd_fp_bus)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "bus"));
}

static void edit_bus(dialog *d, xd_fp_bus bus, const char *name) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkWidget *grid = form_grid();
    GtkWidget *n = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(n), TRUE);
    gtk_widget_set_size_request(n, 260, -1);
    GtkWidget *access = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(policies); i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(access), policies[i], policy_names[i]);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(access), "talk");
    grid_row(grid, 0, "Bus _name:", n);
    grid_row(grid, 1, "_Access:", access);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box),
                       xd_fp_label("A name such as org.freedesktop.Notifications, or the names "
                                   "under one: org.example.*. See: the name is visible; Talk: "
                                   "the application can call it; Own: it can also take the "
                                   "name; None: the name is hidden."),
                       FALSE, FALSE, 0);
    if (name != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(n), name);
        gtk_widget_set_sensitive(n, FALSE);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(access), xd_fp_perms_bus(d->effective, bus, name));
    }
    GtkWidget *w = small_dialog(d, bus == XD_FP_SESSION_BUS ? "Session Bus Name"
                                                            : "System Bus Name",
                                box);
    gtk_widget_show_all(w);
    while (gtk_dialog_run(GTK_DIALOG(w)) == GTK_RESPONSE_OK) {
        char *key = entry_text(n);
        const char *policy = gtk_combo_box_get_active_id(GTK_COMBO_BOX(access));
        if (!xd_fp_bus_name_valid(key) || policy == nullptr) {
            xd_message_box(GTK_WINDOW(w), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a D-Bus name of two or more parts separated by dots, such as "
                           "org.example.Service, or org.example.* for all the names under it.");
            g_free(key);
            continue;
        }
        set_bus(d, bus, key, policy);
        select_key(d->bus_view[bus], key);
        g_free(key);
        break;
    }
    gtk_widget_destroy(w);
}

static void on_bus_add(GtkButton *b, gpointer data) {
    edit_bus(data, button_bus(GTK_WIDGET(b)), nullptr);
}

static void on_bus_edit(GtkButton *b, gpointer data) {
    dialog *d = data;
    xd_fp_bus bus = button_bus(GTK_WIDGET(b));
    char *name = selected_key(d->bus_view[bus]);
    if (name != nullptr)
        edit_bus(d, bus, name);
    g_free(name);
}

static void on_bus_activated(GtkTreeView *v, [[maybe_unused]] GtkTreePath *p,
                             [[maybe_unused]] GtkTreeViewColumn *c, gpointer data) {
    on_bus_edit(GTK_BUTTON(g_object_get_data(G_OBJECT(v), "edit")), data);
}

/* Remove: a name the app may use becomes None; the user's own entry for
 * one it may not goes. */
static void on_bus_remove(GtkButton *b, gpointer data) {
    dialog *d = data;
    xd_fp_bus bus = button_bus(GTK_WIDGET(b));
    char *name = selected_key(d->bus_view[bus]);
    if (name == nullptr)
        return;
    const char *base = xd_fp_perms_bus(d->base, bus, name);
    if (base != nullptr && strcmp(base, "none") != 0 &&
        g_strcmp0(xd_fp_perms_bus(d->effective, bus, name), "none") != 0) {
        set_bus(d, bus, name, "none");
    } else {
        xd_fp_override_set_bus(d->override, bus, name, nullptr);
        changed(d);
    }
    g_free(name);
    bus_buttons(d);
}

static GtkWidget *bus_list(dialog *d, xd_fp_bus bus, const char *mnemonic) {
    d->bus[bus] = list_store();
    d->bus_view[bus] = list_view(d->bus[bus], "Name", "Access");
    GtkWidget *add = xd_fp_button(bus == XD_FP_SESSION_BUS ? "A_dd..." : "Add...");
    d->bus_edit[bus] = xd_fp_button(bus == XD_FP_SESSION_BUS ? "_Edit..." : "Edit...");
    d->bus_remove[bus] = xd_fp_button(bus == XD_FP_SESSION_BUS ? "_Remove" : "Remove");
    GtkWidget *buttons[] = {add, d->bus_edit[bus], d->bus_remove[bus]};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++)
        g_object_set_data(G_OBJECT(buttons[i]), "bus", GINT_TO_POINTER(bus));
    g_object_set_data(G_OBJECT(d->bus_view[bus]), "edit", d->bus_edit[bus]);
    g_signal_connect(add, "clicked", G_CALLBACK(on_bus_add), d);
    g_signal_connect(d->bus_edit[bus], "clicked", G_CALLBACK(on_bus_edit), d);
    g_signal_connect(d->bus_remove[bus], "clicked", G_CALLBACK(on_bus_remove), d);
    g_signal_connect(d->bus_view[bus], "row-activated", G_CALLBACK(on_bus_activated), d);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(d->bus_view[bus])), "changed",
                     G_CALLBACK(on_bus_selected), d);
    return list_with_buttons(gtk_label_new_with_mnemonic(mnemonic), d->bus_view[bus], 90,
                             buttons, 3);
}

static GtkWidget *dbus_page(dialog *d) {
    GtkWidget *page = page_box();
    gtk_box_pack_start(GTK_BOX(page),
                       xd_fp_label("Besides these, an application may always use its own names "
                                   "and the desktop portals on the session bus."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), bus_list(d, XD_FP_SESSION_BUS, "_Session bus:"), TRUE, TRUE,
                       0);
    gtk_box_pack_start(GTK_BOX(page), bus_list(d, XD_FP_SYSTEM_BUS, "S_ystem bus:"), TRUE, TRUE,
                       0);
    return page;
}

/* ---- the dialog ------------------------------------------------------------------ */

static void refresh(dialog *d) {
    bool was = d->updating;
    d->updating = true;
    show_checks(d);
    show_filesystems(d);
    show_env(d);
    show_bus(d);
    d->updating = was;
    fs_buttons(d);
    env_buttons(d);
    bus_buttons(d);
}

static bool apply(dialog *d) {
    GError *error = nullptr;
    if (!xd_fp_override_save(d->override, d->path, &error)) {
        xd_fp_error(GTK_WINDOW(d->window), "The permissions could not be saved.", error);
        g_error_free(error);
        return false;
    }
    d->saved = true;
    gtk_widget_set_sensitive(d->apply, FALSE);
    return true;
}

enum { RESPONSE_DEFAULTS = 1 };

bool xd_fp_app_dialog(GtkWindow *parent, const xd_fp_app *app) {
    dialog d = {.app = app};
    d.path = xd_fp_user_override_path(app->id);
    GError *error = nullptr;
    d.override = xd_fp_override_load(d.path, &error);
    if (d.override == nullptr) {
        xd_fp_error(parent, "Your changes to this application's permissions could not be read.",
                    error);
        g_error_free(error);
        g_free(d.path);
        return false;
    }
    d.base = load_base(app);
    for (int l = 0; l < XD_FP_LIST_COUNT; l++)
        d.checks[l] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, nullptr);

    d.window = gtk_dialog_new();
    char *title = g_strdup_printf("%s Permissions", app->name);
    gtk_window_set_title(GTK_WINDOW(d.window), title);
    g_free(title);
    gtk_window_set_transient_for(GTK_WINDOW(d.window), parent);
    gtk_window_set_modal(GTK_WINDOW(d.window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d.window), FALSE);
    GtkWidget *defaults = gtk_button_new_with_mnemonic("Restore De_faults");
    gtk_widget_set_tooltip_text(defaults, "Give the application back the permissions it came "
                                          "with");
    gtk_dialog_add_action_widget(GTK_DIALOG(d.window), defaults, RESPONSE_DEFAULTS);
    gtk_button_box_set_child_secondary(GTK_BUTTON_BOX(gtk_widget_get_parent(defaults)), defaults,
                                       TRUE);
    gtk_dialog_add_button(GTK_DIALOG(d.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d.window), "Cancel", GTK_RESPONSE_CANCEL);
    d.apply = gtk_dialog_add_button(GTK_DIALOG(d.window), "_Apply", GTK_RESPONSE_APPLY);
    gtk_dialog_set_default_response(GTK_DIALOG(d.window), GTK_RESPONSE_OK);

    GtkWidget *tabs = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(tabs), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), general_page(&d),
                             gtk_label_new_with_mnemonic("General"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), devices_page(&d),
                             gtk_label_new_with_mnemonic("Devices"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), filesystem_page(&d),
                             gtk_label_new_with_mnemonic("File System"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), environment_page(&d),
                             gtk_label_new_with_mnemonic("Environment"));
    gtk_notebook_append_page(GTK_NOTEBOOK(tabs), dbus_page(&d),
                             gtk_label_new_with_mnemonic("D-Bus"));
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d.window));
    gtk_box_pack_start(GTK_BOX(content), tabs, TRUE, TRUE, 0);
    char *note = g_strdup_printf("Settings you changed are shown in bold. Changes take effect "
                                 "the next time %s starts.",
                                 app->name);
    GtkWidget *note_label = xd_fp_label(note);
    g_free(note);
    gtk_widget_set_margin_start(note_label, 8);
    gtk_widget_set_margin_end(note_label, 8);
    gtk_box_pack_start(GTK_BOX(content), note_label, FALSE, FALSE, 4);
    xd_gtk_notebook_ctrl_tab(d.window, GTK_NOTEBOOK(tabs));

    d.updating = true;
    changed(&d);
    d.updating = false;
    gtk_widget_set_sensitive(d.apply, FALSE);
    gtk_widget_show_all(d.window);
    for (;;) {
        int r = gtk_dialog_run(GTK_DIALOG(d.window));
        if (r == RESPONSE_DEFAULTS) {
            g_key_file_free(d.override);
            d.override = g_key_file_new();
            changed(&d);
            continue;
        }
        if (r == GTK_RESPONSE_APPLY) {
            apply(&d);
            continue;
        }
        if (r == GTK_RESPONSE_OK && gtk_widget_get_sensitive(d.apply) && !apply(&d))
            continue;
        break;
    }
    gtk_widget_destroy(d.window);
    for (int l = 0; l < XD_FP_LIST_COUNT; l++)
        g_hash_table_unref(d.checks[l]);
    xd_fp_perms_free(d.base);
    xd_fp_perms_free(d.mine);
    xd_fp_perms_free(d.effective);
    g_key_file_free(d.override);
    g_free(d.path);
    return d.saved;
}
