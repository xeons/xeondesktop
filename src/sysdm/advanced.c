/* SPDX-License-Identifier: MIT
 * System Properties' Advanced tab:
 *
 * - Performance Options: processor scheduling and virtual memory, which
 *   on Linux are the scheduler and swap (zram, partitions, files);
 * - Environment Variables: the user's are kept in
 *   ~/.config/environment.d/99-xeondesktop.conf (systemd reads it at
 *   logon); system variables (/etc/environment, environment.d) are shown;
 * - Startup and Recovery: the boot loader's entries as it reports them in
 *   EFI variables (the Boot Loader Interface, readable without root), one
 *   to start the next time only where logind can set it (systemd-boot),
 *   and what happens when the kernel or a program crashes.
 */
#include <stdlib.h>

#include "sysdm.h"

/* ---- Performance Options -------------------------------------------------------- */

static void on_performance([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    GtkWidget *box = sysdm_dialog(s, "Performance Options", "utilities-system-monitor", FALSE,
                                  nullptr);

    char *autogroup = sysdm_read_line("/proc/sys/kernel/sched_autogroup_enabled");
    GtkWidget *sched = sysdm_label(
        autogroup != nullptr && autogroup[0] == '1'
            ? "The kernel groups processes by session (autogroup), so the programs you use stay "
              "responsive while background jobs run."
            : "The kernel shares processor time between all processes evenly.");
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Application response", sched), FALSE, FALSE, 0);
    g_free(autogroup);

    GtkWidget *vm = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkListStore *ls = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                          G_TYPE_STRING);
    guint64 total_kb = 0;
    char *swaps = nullptr;
    if (g_file_get_contents("/proc/swaps", &swaps, nullptr, nullptr)) {
        char **lines = g_strsplit(swaps, "\n", -1);
        for (char **l = lines + (lines[0] != nullptr ? 1 : 0); *l != nullptr; l++) {
            char **f = g_strsplit_set(g_strstrip(*l), " \t", -1);
            /* Drop the empty fields runs of blanks leave. */
            GPtrArray *fields = g_ptr_array_new();
            for (char **x = f; *x != nullptr; x++) {
                if ((*x)[0] != '\0')
                    g_ptr_array_add(fields, *x);
            }
            if (fields->len >= 5) {
                const char *dev = g_ptr_array_index(fields, 0);
                const char *type = g_ptr_array_index(fields, 1);
                guint64 kb = g_ascii_strtoull(g_ptr_array_index(fields, 2), nullptr, 10);
                guint64 used = g_ascii_strtoull(g_ptr_array_index(fields, 3), nullptr, 10);
                total_kb += kb;
                char *size = g_strdup_printf("%" G_GUINT64_FORMAT " MB", kb / 1024);
                char *u = g_strdup_printf("%" G_GUINT64_FORMAT " MB", used / 1024);
                const char *kind = strstr(dev, "zram") != nullptr ? "Compressed RAM (zram)"
                                   : strcmp(type, "file") == 0     ? "Swap file"
                                                                   : "Swap partition";
                GtkTreeIter it;
                gtk_list_store_append(ls, &it);
                gtk_list_store_set(ls, &it, 0, dev, 1, kind, 2, size, 3, u, -1);
                g_free(size);
                g_free(u);
            }
            g_ptr_array_free(fields, TRUE);
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(swaps);
    }
    char *total = g_strdup_printf("Total swap space for all drives: %" G_GUINT64_FORMAT " MB",
                                  total_kb / 1024);
    gtk_box_pack_start(GTK_BOX(vm), sysdm_label(total), FALSE, FALSE, 0);
    g_free(total);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(ls));
    g_object_unref(ls);
    const char *heads[] = {"Drive", "Type", "Size", "Used"};
    for (int i = 0; i < 4; i++)
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
                                    gtk_tree_view_column_new_with_attributes(
                                        heads[i], gtk_cell_renderer_text_new(), "text", i,
                                        nullptr));
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sc, 360, 90);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    gtk_box_pack_start(GTK_BOX(vm), sc, FALSE, FALSE, 0);
    char *swappiness = sysdm_read_line("/proc/sys/vm/swappiness");
    char *sw = g_strdup_printf("Swappiness (how readily memory moves to swap): %s of 200",
                               swappiness != nullptr ? swappiness : "?");
    gtk_box_pack_start(GTK_BOX(vm), sysdm_label(sw), FALSE, FALSE, 0);
    GtkWidget *note = sysdm_label("Swap is set up by your distribution (zram-generator, fstab); "
                                  "changing it needs an administrator.");
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(vm), note, FALSE, FALSE, 0);
    g_free(swappiness);
    g_free(sw);
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("Virtual memory", vm), FALSE, FALSE, 0);
    gtk_widget_show_all(gtk_widget_get_toplevel(box));
}

/* ---- Environment Variables --------------------------------------------------------- */

enum { EV_NAME, EV_VALUE, EV_SOURCE, EV_OURS, EV_NCOLS };

typedef struct env_dlg {
    GtkWidget *dialog;
    GtkListStore *user;
    GtkWidget *user_view;
    GtkWidget *edit;
    GtkWidget *remove;
    gboolean dirty;
} env_dlg;

static char *our_file(void) {
    return g_build_filename(g_get_user_config_dir(), "environment.d", "99-xeondesktop.conf",
                            nullptr);
}

/* KEY=VALUE lines (comments and blanks skipped), as environment.d and
 * /etc/environment write them. Values may be quoted. */
static void read_env_file(const char *path, GtkListStore *store, gboolean ours) {
    char *text = nullptr;
    if (!g_file_get_contents(path, &text, nullptr, nullptr))
        return;
    char *base = g_path_get_basename(path);
    char **lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l != nullptr; l++) {
        char *line = g_strstrip(*l);
        if (line[0] == '#' || line[0] == '\0')
            continue;
        if (g_str_has_prefix(line, "export "))
            line += 7;
        char *eq = strchr(line, '=');
        if (eq == nullptr || eq == line)
            continue;
        *eq = '\0';
        char *value = g_shell_unquote(eq + 1, nullptr);
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, EV_NAME, g_strstrip(line), EV_VALUE,
                           value != nullptr ? value : eq + 1, EV_SOURCE, path, EV_OURS, ours, -1);
        g_free(value);
    }
    g_strfreev(lines);
    g_free(base);
    g_free(text);
}

static void read_env_dir(const char *dir, GtkListStore *store, const char *skip) {
    GDir *d = g_dir_open(dir, 0, nullptr);
    if (d == nullptr)
        return;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    for (const char *n = g_dir_read_name(d); n != nullptr; n = g_dir_read_name(d)) {
        if (g_str_has_suffix(n, ".conf"))
            g_ptr_array_add(names, g_build_filename(dir, n, nullptr));
    }
    g_dir_close(d);
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len; i++) {
        const char *p = g_ptr_array_index(names, i);
        if (skip == nullptr || strcmp(p, skip) != 0)
            read_env_file(p, store, FALSE);
    }
    g_ptr_array_free(names, TRUE);
}

static void sync_env_buttons(env_dlg *e) {
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    gboolean sel = gtk_tree_selection_get_selected(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(e->user_view)), &m, &it);
    gboolean ours = FALSE;
    if (sel)
        gtk_tree_model_get(m, &it, EV_OURS, &ours, -1);
    gtk_widget_set_sensitive(e->edit, sel);
    /* Variables from other environment.d files are theirs to remove. */
    gtk_widget_set_sensitive(e->remove, sel && ours);
}

static void on_env_selection([[maybe_unused]] GtkTreeSelection *s, gpointer data) {
    sync_env_buttons(data);
}

static gboolean valid_env_name(const char *n) {
    if (n[0] == '\0' || g_ascii_isdigit(n[0]))
        return FALSE;
    for (const char *p = n; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_')
            return FALSE;
    }
    return TRUE;
}

/* "New User Variable" / "Edit User Variable". Returns TRUE if OK'd. */
static gboolean edit_variable(env_dlg *e, const char *title, char **name, char **value) {
    GtkWidget *d = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(d), title);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(e->dialog));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(d), TRUE);
    gtk_dialog_add_button(GTK_DIALOG(d), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(d), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *grid = gtk_grid_new();
    gtk_container_set_border_width(GTK_CONTAINER(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    GtkWidget *ne = gtk_entry_new();
    GtkWidget *ve = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(ve), 40);
    gtk_entry_set_activates_default(GTK_ENTRY(ne), TRUE);
    gtk_entry_set_activates_default(GTK_ENTRY(ve), TRUE);
    if (*name != nullptr) {
        gtk_entry_set_text(GTK_ENTRY(ne), *name);
        gtk_widget_set_sensitive(ne, FALSE);
    }
    if (*value != nullptr)
        gtk_entry_set_text(GTK_ENTRY(ve), *value);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Variable name:"), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ne, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), sysdm_label("Variable value:"), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), ve, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), grid, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gboolean ok = FALSE;
    while (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        const char *n = gtk_entry_get_text(GTK_ENTRY(ne));
        if (!valid_env_name(n)) {
            xd_message_box(GTK_WINDOW(d), title, XD_MB_WARNING, XD_MB_OK, FALSE,
                           "A variable name may contain only letters, digits and underscores, "
                           "and can't start with a digit.");
            continue;
        }
        g_free(*name);
        g_free(*value);
        *name = g_strdup(n);
        *value = g_strdup(gtk_entry_get_text(GTK_ENTRY(ve)));
        ok = TRUE;
        break;
    }
    gtk_widget_destroy(d);
    return ok;
}

/* Our entry for a variable, if there is one. */
static gboolean find_ours(GtkListStore *store, const char *name, GtkTreeIter *out) {
    GtkTreeModel *m = GTK_TREE_MODEL(store);
    for (gboolean ok = gtk_tree_model_get_iter_first(m, out); ok;
         ok = gtk_tree_model_iter_next(m, out)) {
        char *n = nullptr;
        gboolean ours = FALSE;
        gtk_tree_model_get(m, out, EV_NAME, &n, EV_OURS, &ours, -1);
        gboolean match = ours && g_strcmp0(n, name) == 0;
        g_free(n);
        if (match)
            return TRUE;
    }
    return FALSE;
}

static void set_ours(env_dlg *e, const char *name, const char *value) {
    GtkTreeIter it;
    char *path = our_file();
    if (!find_ours(e->user, name, &it))
        gtk_list_store_append(e->user, &it);
    gtk_list_store_set(e->user, &it, EV_NAME, name, EV_VALUE, value, EV_SOURCE, path, EV_OURS,
                       TRUE, -1);
    g_free(path);
    e->dirty = TRUE;
}

static void on_env_new([[maybe_unused]] GtkButton *b, gpointer data) {
    env_dlg *e = data;
    char *name = nullptr;
    char *value = nullptr;
    if (edit_variable(e, "New User Variable", &name, &value))
        set_ours(e, name, value);
    g_free(name);
    g_free(value);
}

static void on_env_edit([[maybe_unused]] GtkButton *b, gpointer data) {
    env_dlg *e = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->user_view)),
                                         &m, &it))
        return;
    char *name = nullptr;
    char *value = nullptr;
    gtk_tree_model_get(m, &it, EV_NAME, &name, EV_VALUE, &value, -1);
    /* Editing a variable from another file overrides it in ours, which is
     * read last. */
    if (edit_variable(e, "Edit User Variable", &name, &value))
        set_ours(e, name, value);
    g_free(name);
    g_free(value);
}

static void on_env_delete([[maybe_unused]] GtkButton *b, gpointer data) {
    env_dlg *e = data;
    GtkTreeModel *m = nullptr;
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(e->user_view)),
                                        &m, &it)) {
        gtk_list_store_remove(e->user, &it);
        e->dirty = TRUE;
    }
}

static gboolean save_env(env_dlg *e, GError **err) {
    GString *out = g_string_new("# Environment variables set in System Properties.\n"
                                "# systemd applies them at the next logon.\n");
    GtkTreeModel *m = GTK_TREE_MODEL(e->user);
    GtkTreeIter it;
    for (gboolean ok = gtk_tree_model_get_iter_first(m, &it); ok;
         ok = gtk_tree_model_iter_next(m, &it)) {
        char *n = nullptr;
        char *v = nullptr;
        gboolean ours = FALSE;
        gtk_tree_model_get(m, &it, EV_NAME, &n, EV_VALUE, &v, EV_OURS, &ours, -1);
        if (ours) {
            char *q = g_shell_quote(v != nullptr ? v : "");
            g_string_append_printf(out, "%s=%s\n", n, q);
            g_free(q);
        }
        g_free(n);
        g_free(v);
    }
    char *path = our_file();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = g_file_set_contents(path, out->str, (gssize)out->len, err);
    g_free(dir);
    g_free(path);
    g_string_free(out, TRUE);
    return ok;
}

static GtkWidget *env_view(GtkListStore *store) {
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    const char *heads[] = {"Variable", "Value"};
    for (int i = 0; i < 2; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
            heads[i], gtk_cell_renderer_text_new(), "text", i, nullptr);
        gtk_tree_view_column_set_resizable(col, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);
    }
    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(tv), EV_SOURCE);
    return tv;
}

static GtkWidget *scrolled(GtkWidget *child) {
    GtkWidget *sc = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_widget_set_size_request(sc, 380, 110);
    gtk_container_add(GTK_CONTAINER(sc), child);
    return sc;
}

static void on_environment([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    env_dlg e = {0};
    GtkWidget *box = sysdm_dialog(s, "Environment Variables", "utilities-terminal", TRUE,
                                  &e.dialog);

    e.user = gtk_list_store_new(EV_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                G_TYPE_BOOLEAN);
    char *ours = our_file();
    char *user_dir = g_build_filename(g_get_user_config_dir(), "environment.d", nullptr);
    read_env_dir(user_dir, e.user, ours);
    read_env_file(ours, e.user, TRUE);
    g_free(user_dir);
    g_free(ours);
    const char *user = g_get_user_name();
    char *title = g_strdup_printf("User variables for %s", user);
    gtk_box_pack_start(GTK_BOX(box), sysdm_label(title), FALSE, FALSE, 0);
    g_free(title);
    e.user_view = env_view(e.user);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(e.user_view)), "changed",
                     G_CALLBACK(on_env_selection), &e);
    gtk_box_pack_start(GTK_BOX(box), scrolled(e.user_view), TRUE, TRUE, 0);
    e.edit = gtk_button_new_with_mnemonic("_Edit...");
    e.remove = gtk_button_new_with_mnemonic("_Delete");
    GtkWidget *nw = gtk_button_new_with_mnemonic("_New...");
    g_signal_connect(nw, "clicked", G_CALLBACK(on_env_new), &e);
    g_signal_connect(e.edit, "clicked", G_CALLBACK(on_env_edit), &e);
    g_signal_connect(e.remove, "clicked", G_CALLBACK(on_env_delete), &e);
    gtk_box_pack_start(GTK_BOX(box), sysdm_button_row(nw, e.edit, e.remove, nullptr), FALSE,
                       FALSE, 0);

    GtkListStore *sys = gtk_list_store_new(EV_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_BOOLEAN);
    read_env_file("/etc/environment", sys, FALSE);
    read_env_dir("/usr/lib/environment.d", sys, nullptr);
    read_env_dir("/etc/environment.d", sys, nullptr);
    gtk_box_pack_start(GTK_BOX(box), sysdm_label("System variables"), FALSE, FALSE, 6);
    GtkWidget *sys_view = env_view(sys);
    g_object_unref(sys);
    gtk_box_pack_start(GTK_BOX(box), scrolled(sys_view), TRUE, TRUE, 0);
    GtkWidget *sn = gtk_button_new_with_mnemonic("Ne_w...");
    GtkWidget *se = gtk_button_new_with_mnemonic("Ed_it...");
    GtkWidget *sd = gtk_button_new_with_mnemonic("De_lete");
    gtk_widget_set_sensitive(sn, FALSE);
    gtk_widget_set_sensitive(se, FALSE);
    gtk_widget_set_sensitive(sd, FALSE);
    gtk_widget_set_tooltip_text(sn, "System variables are in /etc/environment; changing them "
                                    "needs an administrator.");
    gtk_box_pack_start(GTK_BOX(box), sysdm_button_row(sn, se, sd, nullptr), FALSE, FALSE, 0);
    GtkWidget *note = sysdm_label("Changes take effect the next time you log on.");
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    sync_env_buttons(&e);
    gtk_widget_show_all(e.dialog);
    while (gtk_dialog_run(GTK_DIALOG(e.dialog)) == GTK_RESPONSE_OK) {
        GError *err = nullptr;
        if (!e.dirty || save_env(&e, &err))
            break;
        xd_message_box(GTK_WINDOW(e.dialog), "Environment Variables", XD_MB_ERROR, XD_MB_OK,
                       FALSE, err->message);
        g_error_free(err);
    }
    gtk_widget_destroy(e.dialog);
    g_object_unref(e.user);
}

/* ---- Startup and Recovery ----------------------------------------------------------- */

#define LOADER_GUID "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"

/* A Boot Loader Interface variable: UTF-16 strings after 4 attribute
 * bytes; several are NUL-separated. */
static char **loader_strings(const char *name) {
    char *path = g_strdup_printf("/sys/firmware/efi/efivars/%s-" LOADER_GUID, name);
    char *data = nullptr;
    gsize len = 0;
    gboolean ok = g_file_get_contents(path, &data, &len, nullptr);
    g_free(path);
    if (!ok || len < 6) {
        g_free(data);
        return nullptr;
    }
    glong n = (glong)((len - 4) / 2);
    gunichar2 *u16 = g_new0(gunichar2, (gsize)n + 1);
    memcpy(u16, data + 4, (gsize)n * 2);
    g_free(data);
    GPtrArray *out = g_ptr_array_new();
    glong start = 0;
    for (glong i = 0; i <= n; i++) {
        if (i == n || u16[i] == 0) {
            if (i > start) {
                char *s8 = g_utf16_to_utf8(u16 + start, i - start, nullptr, nullptr, nullptr);
                if (s8 != nullptr)
                    g_ptr_array_add(out, s8);
            }
            start = i + 1;
        }
    }
    g_free(u16);
    g_ptr_array_add(out, nullptr);
    return (char **)g_ptr_array_free(out, FALSE);
}

static char *loader_string(const char *name) {
    char **v = loader_strings(name);
    char *out = v != nullptr && v[0] != nullptr ? g_strdup(v[0]) : nullptr;
    g_strfreev(v);
    return out;
}

/* Starting another entry the next time only, through logind
 * (SetRebootToBootLoaderEntry: systemd-boot's LoaderEntryOneShot). The
 * default entry is in the boot loader's configuration, which only an
 * administrator can change. */
typedef struct boot_next {
    GtkWidget *combo, *once;
    GDBusProxy *login1;
    char *def; /* what starts otherwise */
    char *was; /* the one-off entry already set, or "" */
    gboolean syncing;
    gboolean busy;
} boot_next;

static void boot_next_free(gpointer data) {
    boot_next *n = data;
    g_clear_object(&n->login1);
    g_free(n->def);
    g_free(n->was);
    g_free(n);
}

/* logind's manager, if it can start an entry the next time only (it says
 * "challenge" when that takes an administrator's password). */
static GDBusProxy *login1_for_boot_entries(void) {
    GDBusProxy *p = g_dbus_proxy_new_for_bus_sync(
        G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, nullptr,
        "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
        nullptr, nullptr);
    if (p == nullptr)
        return nullptr;
    GVariant *r = g_dbus_proxy_call_sync(p, "CanRebootToBootLoaderEntry", nullptr,
                                         G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr);
    const char *can = nullptr;
    if (r != nullptr && g_variant_is_of_type(r, G_VARIANT_TYPE("(s)")))
        g_variant_get(r, "(&s)", &can);
    gboolean ok = g_strcmp0(can, "yes") == 0 || g_strcmp0(can, "challenge") == 0;
    if (r != nullptr)
        g_variant_unref(r);
    if (!ok)
        g_clear_object(&p);
    return p;
}

static char *login1_string(GDBusProxy *p, const char *name) {
    GVariant *v = g_dbus_proxy_get_cached_property(p, name);
    char *out = v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                    ? g_variant_dup_string(v, nullptr)
                    : g_strdup("");
    if (v != nullptr)
        g_variant_unref(v);
    return out;
}

/* Choosing another entry means the next time only: the default can't be
 * changed here. */
static void on_boot_entry_changed([[maybe_unused]] GtkComboBox *c, gpointer data) {
    boot_next *n = data;
    if (n->syncing)
        return;
    n->syncing = TRUE;
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(n->combo));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(n->once), g_strcmp0(id, n->def) != 0);
    n->syncing = FALSE;
}

/* Unticked, the default starts as usual. */
static void on_boot_once_toggled(GtkToggleButton *t, gpointer data) {
    boot_next *n = data;
    if (n->syncing || gtk_toggle_button_get_active(t))
        return;
    n->syncing = TRUE;
    if (n->def == nullptr || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(n->combo), n->def))
        gtk_combo_box_set_active(GTK_COMBO_BOX(n->combo), -1); /* not one of these */
    n->syncing = FALSE;
}

static void on_boot_next_set(GObject *src, GAsyncResult *res, gpointer data) {
    GtkWidget *dialog = data;
    GError *err = nullptr;
    GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
    /* Gone if the window was closed meanwhile. */
    boot_next *n = g_object_get_data(G_OBJECT(dialog), "boot-next");
    if (n != nullptr && r != nullptr) {
        gtk_widget_destroy(dialog);
    } else if (n != nullptr) {
        n->busy = FALSE;
        gtk_widget_set_sensitive(dialog, TRUE);
        sysdm_call_failed(GTK_WINDOW(dialog),
                          "The operating system to start next could not be set", err);
    }
    if (r != nullptr)
        g_variant_unref(r);
    g_clear_error(&err);
    g_object_unref(dialog);
}

static void on_boot_next_response(GtkDialog *d, int response, gpointer data) {
    boot_next *n = data;
    if (n->busy)
        return; /* waiting for logind (and polkit's password prompt) */
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(n->combo));
    bool once = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(n->once));
    /* "" clears a one-off entry set before. */
    const char *want = once && id != nullptr ? id : "";
    if (response != GTK_RESPONSE_OK || strcmp(want, n->was) == 0) {
        gtk_widget_destroy(GTK_WIDGET(d));
        return;
    }
    n->busy = TRUE;
    gtk_widget_set_sensitive(GTK_WIDGET(d), FALSE);
    g_dbus_proxy_call(n->login1, "SetRebootToBootLoaderEntry", g_variant_new("(s)", want),
                      G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, G_MAXINT, nullptr,
                      on_boot_next_set, g_object_ref(d));
}

static void on_startup_recovery([[maybe_unused]] GtkButton *b, gpointer data) {
    sysdm *s = data;
    GDBusProxy *login1 = login1_for_boot_entries();
    GtkWidget *dialog = nullptr;
    GtkWidget *box = sysdm_dialog(s, "Startup and Recovery", "system-reboot", login1 != nullptr,
                                  login1 != nullptr ? &dialog : nullptr);

    GtkWidget *start = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    char **entries = loader_strings("LoaderEntries");
    char *def = loader_string("LoaderEntryDefault");
    char *booted = loader_string("LoaderEntrySelected");
    char *info = loader_string("LoaderInfo");
    char *timeout = loader_string("LoaderConfigTimeout");
    char *was = login1 != nullptr ? login1_string(login1, "RebootToBootLoaderEntry") : nullptr;
    if (login1 != nullptr) {
        /* logind's list, the ones it can start; the same IDs as the EFI
         * variable's. */
        GVariant *v = g_dbus_proxy_get_cached_property(login1, "BootLoaderEntries");
        if (v != nullptr && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY) &&
            g_variant_n_children(v) > 0) {
            g_strfreev(entries);
            entries = g_variant_dup_strv(v, nullptr);
        }
        if (v != nullptr)
            g_variant_unref(v);
    }
    const char *usual = def != nullptr ? def : booted;
    GtkWidget *l = gtk_label_new_with_mnemonic("_Default operating system:");
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_pack_start(GTK_BOX(start), l, FALSE, FALSE, 0);
    GtkWidget *combo = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), combo);
    for (char **e = entries; e != nullptr && *e != nullptr; e++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), *e, *e);
    bool any = entries != nullptr && entries[0] != nullptr;
    if (!any)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo),
                                       "(the boot loader doesn't report its entries)");
    /* Nothing shown if the default isn't among the entries (the boot loader
     * didn't say, or logind lists others): showing the first would claim
     * it's the default. */
    const char *shown = was != nullptr && was[0] != '\0' ? was : usual;
    if (!any)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    else if (shown != nullptr)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), shown);
    gtk_box_pack_start(GTK_BOX(start), combo, FALSE, FALSE, 0);
    if (login1 != nullptr && any) {
        boot_next *n = g_new0(boot_next, 1);
        n->combo = combo;
        n->login1 = g_steal_pointer(&login1);
        n->def = g_strdup(usual);
        n->was = g_strdup(was);
        n->once = gtk_check_button_new_with_mnemonic("Start this one the _next time only");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(n->once), n->was[0] != '\0');
        gtk_box_pack_start(GTK_BOX(start), n->once, FALSE, FALSE, 0);
        g_signal_connect(combo, "changed", G_CALLBACK(on_boot_entry_changed), n);
        g_signal_connect(n->once, "toggled", G_CALLBACK(on_boot_once_toggled), n);
        g_object_set_data_full(G_OBJECT(dialog), "boot-next", n, boot_next_free);
        g_signal_connect(dialog, "response", G_CALLBACK(on_boot_next_response), n);
    } else {
        gtk_widget_set_sensitive(combo, FALSE);
        if (dialog != nullptr) /* logind could, but there's nothing to choose */
            g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), nullptr);
    }
    g_clear_object(&login1);
    char *line = timeout != nullptr
                     ? g_strdup_printf("Display list of operating systems for %s seconds.", timeout)
                     : g_strdup("The boot loader's menu timeout is set in its configuration.");
    gtk_box_pack_start(GTK_BOX(start), sysdm_label(line), FALSE, FALSE, 0);
    g_free(line);
    line = g_strdup_printf("Boot loader: %s. Started this time: %s.",
                           info != nullptr ? info : "unknown",
                           booted != nullptr ? booted : "unknown");
    gtk_box_pack_start(GTK_BOX(start), sysdm_label(line), FALSE, FALSE, 0);
    g_free(line);
    GtkWidget *note = sysdm_label(
        gtk_widget_get_sensitive(combo)
            ? "The default, started every other time, is set in the boot loader's "
              "configuration, which only an administrator can change (for systemd-boot, with "
              "bootctl set-default)."
            : "The boot loader's files are readable only by administrators; these come from "
              "what it reports at startup.");
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(start), note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("System startup", start), FALSE, FALSE, 0);

    GtkWidget *fail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    char *panic = sysdm_read_line("/proc/sys/kernel/panic");
    long secs = panic != nullptr ? strtol(panic, nullptr, 10) : 0;
    char *reboot_text =
        secs > 0 ? g_strdup_printf("Automatically reboot %ld seconds after a kernel panic", secs)
        : secs < 0 ? g_strdup("Automatically reboot right after a kernel panic")
                   : g_strdup("Automatically reboot after a kernel panic");
    GtkWidget *reboot = gtk_check_button_new_with_label(reboot_text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(reboot), secs != 0);
    gtk_widget_set_sensitive(reboot, FALSE);
    gtk_box_pack_start(GTK_BOX(fail), reboot, FALSE, FALSE, 0);
    char *kexec = sysdm_read_line("/sys/kernel/kexec_crash_loaded");
    GtkWidget *kdump = gtk_check_button_new_with_label("Write a kernel crash dump (kdump)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(kdump), kexec != nullptr && kexec[0] == '1');
    gtk_widget_set_sensitive(kdump, FALSE);
    gtk_box_pack_start(GTK_BOX(fail), kdump, FALSE, FALSE, 0);
    char *pattern = sysdm_read_line("/proc/sys/kernel/core_pattern");
    char *core = pattern != nullptr && strstr(pattern, "systemd-coredump") != nullptr
                     ? g_strdup("Programs that crash: memory dumps are kept by systemd-coredump "
                                "(see coredumpctl).")
                 : pattern != nullptr && pattern[0] == '|'
                     ? g_strdup_printf("Programs that crash: memory dumps go to %s.",
                                       pattern + 1)
                     : g_strdup_printf("Programs that crash: memory dumps are written as \"%s\".",
                                       pattern != nullptr ? pattern : "core");
    gtk_box_pack_start(GTK_BOX(fail), sysdm_label(core), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sysdm_group("System failure", fail), FALSE, FALSE, 0);

    g_free(panic);
    g_free(reboot_text);
    g_free(kexec);
    g_free(pattern);
    g_free(core);
    g_strfreev(entries);
    g_free(def);
    g_free(booted);
    g_free(info);
    g_free(timeout);
    g_free(was);
    gtk_widget_set_size_request(gtk_widget_get_toplevel(box), 420, -1);
    gtk_widget_show_all(gtk_widget_get_toplevel(box));
}

/* ---- the tab ------------------------------------------------------------------------- */

GtkWidget *sysdm_advanced_page(sysdm *s) {
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(page), 12);
    struct {
        const char *title, *icon, *text, *button;
        GCallback cb;
    } groups[] = {
        {"Performance", "utilities-system-monitor",
         "Performance options control how programs share the processor and how memory is "
         "backed by swap.",
         "Performance _Options...", G_CALLBACK(on_performance)},
        {"Environment Variables", "utilities-terminal",
         "Environment variables tell programs where to find certain types of information.",
         "_Environment Variables...", G_CALLBACK(on_environment)},
        {"Startup and Recovery", "system-reboot",
         "Startup and recovery options tell your computer how to start and what to do if an "
         "error causes your computer to stop.",
         "_Startup and Recovery...", G_CALLBACK(on_startup_recovery)},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(groups); i++) {
        GtkWidget *btn = sysdm_button(groups[i].button, groups[i].cb, s);
        gtk_widget_set_size_request(btn, 170, -1);
        gtk_box_pack_start(GTK_BOX(page),
                           sysdm_group_with_icon(groups[i].title, groups[i].icon, groups[i].text,
                                                 sysdm_button_row(btn, nullptr)),
                           FALSE, FALSE, 0);
    }
    return page;
}
