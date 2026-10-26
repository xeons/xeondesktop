/* SPDX-License-Identifier: MIT
 * The Remotes tab: the user's and the system installations' remotes,
 * through libflatpak. Changing a system installation's goes through
 * Flatpak's system helper, which asks polkit (and so the user, for an
 * administrator's password); the call runs in a thread meanwhile.
 */
#include <stdlib.h>

#include "flatpak.h"

enum { COL_NAME, COL_TITLE, COL_URL, COL_WHERE, COL_STATUS, COL_INST, COL_DISABLED, COL_COUNT };

typedef struct remotes {
    GtkWidget *window, *view, *add, *remove, *toggle;
    GtkListStore *store;
    bool busy;
} remotes;

typedef enum { OP_ADD, OP_REMOVE, OP_ENABLE, OP_DISABLE } op_kind;

typedef struct op {
    remotes *r;
    op_kind kind;
    FlatpakInstallation *inst;
    FlatpakRemote *remote; /* OP_ADD */
    char *name;
} op;

static void op_free(op *operation) {
    g_object_unref(operation->inst);
    g_clear_object(&operation->remote);
    g_free(operation->name);
    g_free(operation);
}

static void fill(remotes *remotes_tab) {
    char *keep = nullptr;
    bool keep_user = false;
    GtkTreeIter it;
    GtkTreeModel *model;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(remotes_tab->view));
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        FlatpakInstallation *inst;
        gtk_tree_model_get(model, &it, COL_NAME, &keep, COL_INST, &inst, -1);
        keep_user = flatpak_installation_get_is_user(inst);
        g_object_unref(inst);
    }
    gtk_list_store_clear(remotes_tab->store);
    GPtrArray *insts = xd_fp_installations();
    bool found = false;
    GtkTreeIter select;
    for (guint i = 0; i < insts->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(insts, i);
        GError *error = nullptr;
        GPtrArray *list = flatpak_installation_list_remotes(inst, nullptr, &error);
        if (list == nullptr) {
            g_printerr("xd-flatpak: %s\n", error->message);
            g_error_free(error);
            continue;
        }
        for (guint j = 0; j < list->len; j++) {
            FlatpakRemote *remote = g_ptr_array_index(list, j);
            const char *name = flatpak_remote_get_name(remote);
            char *title = flatpak_remote_get_title(remote);
            char *url = flatpak_remote_get_url(remote);
            bool disabled = flatpak_remote_get_disabled(remote);
            gtk_list_store_insert_with_values(
                remotes_tab->store, &it, -1, COL_NAME, name, COL_TITLE,
                title != nullptr ? title : "", COL_URL, url != nullptr ? url : "", COL_WHERE,
                xd_fp_installation_label(inst), COL_STATUS, disabled ? "Disabled" : "Enabled",
                COL_INST, inst, COL_DISABLED, disabled, -1);
            if (!found && g_strcmp0(keep, name) == 0 &&
                keep_user == flatpak_installation_get_is_user(inst)) {
                select = it;
                found = true;
            }
            g_free(title);
            g_free(url);
        }
        g_ptr_array_unref(list);
    }
    g_ptr_array_unref(insts);
    g_free(keep);
    if (found)
        gtk_tree_selection_select_iter(sel, &select);
    else if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(remotes_tab->store), &it))
        gtk_tree_selection_select_iter(sel, &it);
}

/* The selected remote's name and installation (unref it). */
static bool selected(remotes *remotes_tab, char **name, FlatpakInstallation **inst,
                     bool *disabled) {
    GtkTreeIter it;
    GtkTreeModel *model;
    if (!gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(remotes_tab->view)), &model, &it))
        return false;
    gboolean off;
    gtk_tree_model_get(model, &it, COL_NAME, name, COL_INST, inst, COL_DISABLED, &off, -1);
    if (disabled != nullptr)
        *disabled = off;
    return true;
}

static void buttons(remotes *remotes_tab) {
    char *name = nullptr;
    FlatpakInstallation *inst = nullptr;
    bool disabled = false;
    bool any = selected(remotes_tab, &name, &inst, &disabled);
    gtk_widget_set_sensitive(remotes_tab->add, !remotes_tab->busy);
    gtk_widget_set_sensitive(remotes_tab->remove, any && !remotes_tab->busy);
    gtk_widget_set_sensitive(remotes_tab->toggle, any && !remotes_tab->busy);
    gtk_button_set_label(GTK_BUTTON(remotes_tab->toggle), any && disabled ? "_Enable" : "Di_sable");
    g_free(name);
    if (inst != nullptr)
        g_object_unref(inst);
}

static void on_selected([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    buttons(data);
}

/* ---- running a change ------------------------------------------------------------ */

static void op_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                      GCancellable *cancel) {
    op *operation = data;
    GError *error = nullptr;
    bool ok = false;
    switch (operation->kind) {
    case OP_ADD:
        ok = flatpak_installation_add_remote(operation->inst, operation->remote, FALSE, cancel,
                                             &error);
        break;
    case OP_REMOVE:
        ok = flatpak_installation_remove_remote(operation->inst, operation->name, cancel, &error);
        break;
    case OP_ENABLE:
    case OP_DISABLE: {
        FlatpakRemote *remote = flatpak_installation_get_remote_by_name(
            operation->inst, operation->name, cancel, &error);
        if (remote != nullptr) {
            flatpak_remote_set_disabled(remote, operation->kind == OP_DISABLE);
            ok = flatpak_installation_modify_remote(operation->inst, remote, cancel, &error);
            g_object_unref(remote);
        }
        break;
    }
    }
    if (ok)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void op_done([[maybe_unused]] GObject *source, GAsyncResult *result, gpointer data) {
    op *operation = data;
    remotes *remotes_tab = operation->r;
    GError *error = nullptr;
    remotes_tab->busy = false;
    GdkWindow *win = gtk_widget_get_window(remotes_tab->window);
    if (win != nullptr)
        gdk_window_set_cursor(win, nullptr);
    if (!g_task_propagate_boolean(G_TASK(result), &error)) {
        const char *what = operation->kind == OP_ADD      ? "The remote could not be added."
                           : operation->kind == OP_REMOVE ? "The remote could not be removed."
                           : operation->kind == OP_ENABLE ? "The remote could not be enabled."
                                                          : "The remote could not be disabled.";
        /* Cancelled at polkit's password prompt: nothing to say. */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
            !g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED))
            xd_fp_error(GTK_WINDOW(remotes_tab->window), what, error);
        g_error_free(error);
    }
    fill(remotes_tab);
    buttons(remotes_tab);
}

static void run(remotes *remotes_tab, op_kind kind, FlatpakInstallation *inst, const char *name,
                FlatpakRemote *remote) {
    op *operation = g_new0(op, 1);
    *operation = (op){remotes_tab, kind, g_object_ref(inst),
                      remote != nullptr ? g_object_ref(remote) : nullptr, g_strdup(name)};
    remotes_tab->busy = true;
    buttons(remotes_tab);
    GdkWindow *win = gtk_widget_get_window(remotes_tab->window);
    if (win != nullptr) {
        GdkCursor *wait = gdk_cursor_new_from_name(gdk_window_get_display(win), "progress");
        gdk_window_set_cursor(win, wait);
        if (wait != nullptr)
            g_object_unref(wait);
    }
    GTask *task = g_task_new(nullptr, nullptr, op_done, operation);
    g_task_set_task_data(task, operation, (GDestroyNotify)op_free);
    g_task_run_in_thread(task, op_thread);
    g_object_unref(task);
}

/* ---- Add ------------------------------------------------------------------------ */

typedef struct add_dialog {
    GtkWidget *window, *from_file, *file, *browse, *from_url, *url, *name, *where;
    bool name_typed;
} add_dialog;

static void add_sensitivity(add_dialog *add_remote) {
    bool file = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_remote->from_file));
    gtk_widget_set_sensitive(add_remote->file, file);
    gtk_widget_set_sensitive(add_remote->browse, file);
    gtk_widget_set_sensitive(add_remote->url, !file);
}

static void on_source([[maybe_unused]] GtkToggleButton *toggle, gpointer data) {
    add_sensitivity(data);
}

/* The name follows the file's, until the user types one. */
static void on_file_changed(GtkEditable *editable, gpointer data) {
    add_dialog *add_remote = data;
    if (add_remote->name_typed)
        return;
    char *base = g_path_get_basename(gtk_entry_get_text(GTK_ENTRY(editable)));
    char *dot = g_str_has_suffix(base, ".flatpakrepo") ? strrchr(base, '.') : nullptr;
    if (dot != nullptr)
        *dot = '\0';
    g_signal_handlers_block_matched(add_remote->name, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr,
                                    add_remote);
    gtk_entry_set_text(GTK_ENTRY(add_remote->name), strcmp(base, ".") == 0 ? "" : base);
    g_signal_handlers_unblock_matched(add_remote->name, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr,
                                      add_remote);
    g_free(base);
}

static void on_name_changed([[maybe_unused]] GtkEditable *editable, gpointer data) {
    ((add_dialog *)data)->name_typed = true;
}

static void on_browse([[maybe_unused]] GtkButton *button, gpointer data) {
    add_dialog *add_remote = data;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Open", GTK_WINDOW(add_remote->window), GTK_FILE_CHOOSER_ACTION_OPEN, "Open", "Cancel");
    GtkFileFilter *repo = gtk_file_filter_new();
    gtk_file_filter_set_name(repo, "Flatpak repository files (*.flatpakrepo)");
    gtk_file_filter_add_pattern(repo, "*.flatpakrepo");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), repo);
    GtkFileFilter *any = gtk_file_filter_new();
    gtk_file_filter_set_name(any, "All files");
    gtk_file_filter_add_pattern(any, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), any);
    const char *downloads = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    if (downloads != nullptr)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), downloads);
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        if (path != nullptr)
            gtk_entry_set_text(GTK_ENTRY(add_remote->file), path);
        g_free(path);
    }
    g_object_unref(chooser);
}

/* The remote the dialog describes, or nullptr after saying why not. */
static FlatpakRemote *make_remote(add_dialog *add_remote, const char *name) {
    GtkWindow *parent = GTK_WINDOW(add_remote->window);
    GError *error = nullptr;
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add_remote->from_file))) {
        char *path = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(add_remote->file))));
        char *data = nullptr;
        gsize length = 0;
        FlatpakRemote *remote = nullptr;
        if (*path == '\0') {
            xd_message_box(parent, XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type the name of a .flatpakrepo file, or click Browse to find one.");
        } else if (!g_file_get_contents(path, &data, &length, &error)) {
            xd_fp_error(parent, "The file could not be read.", error);
            g_error_free(error);
        } else {
            GBytes *bytes = g_bytes_new_take(data, length);
            remote = flatpak_remote_new_from_file(name, bytes, &error);
            g_bytes_unref(bytes);
            if (remote == nullptr) {
                xd_fp_error(parent, "This isn't a Flatpak repository file.", error);
                g_error_free(error);
            }
        }
        g_free(path);
        return remote;
    }
    char *url = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(add_remote->url))));
    char *scheme = g_uri_parse_scheme(url);
    FlatpakRemote *remote = nullptr;
    if (scheme == nullptr) {
        xd_message_box(parent, XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK, FALSE,
                       "Type the repository's address, such as https://example.org/repo/.");
    } else {
        remote = flatpak_remote_new(name);
        flatpak_remote_set_url(remote, url);
        /* Without a .flatpakrepo file there's no key to check it with. */
        flatpak_remote_set_gpg_verify(remote, FALSE);
    }
    g_free(scheme);
    g_free(url);
    return remote;
}

static void on_add([[maybe_unused]] GtkButton *button, gpointer data) {
    remotes *remotes_tab = data;
    add_dialog add_remote = {0};
    add_remote.window = gtk_dialog_new();
    gtk_window_set_title(GTK_WINDOW(add_remote.window), "Add Remote");
    gtk_window_set_transient_for(GTK_WINDOW(add_remote.window), GTK_WINDOW(remotes_tab->window));
    gtk_window_set_modal(GTK_WINDOW(add_remote.window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(add_remote.window), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    add_remote.from_file = gtk_radio_button_new_with_mnemonic(nullptr, "From a repository _file "
                                                                       "(.flatpakrepo):");
    gtk_box_pack_start(GTK_BOX(box), add_remote.from_file, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(row, 20);
    add_remote.file = gtk_entry_new();
    gtk_widget_set_size_request(add_remote.file, 280, -1);
    add_remote.browse = gtk_button_new_with_mnemonic("_Browse...");
    gtk_box_pack_start(GTK_BOX(row), add_remote.file, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), add_remote.browse, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    add_remote.from_url = gtk_radio_button_new_with_mnemonic_from_widget(
        GTK_RADIO_BUTTON(add_remote.from_file), "From an _address:");
    gtk_box_pack_start(GTK_BOX(box), add_remote.from_url, FALSE, FALSE, 0);
    add_remote.url = gtk_entry_new();
    gtk_widget_set_margin_start(add_remote.url, 20);
    gtk_box_pack_start(GTK_BOX(box), add_remote.url, FALSE, FALSE, 0);
    GtkWidget *note = xd_fp_label("A remote added by its address has no key, so Flatpak can't "
                                  "check that what comes from it is genuine. Prefer the "
                                  "repository's .flatpakrepo file.");
    gtk_widget_set_margin_start(note, 20);
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 8);
    add_remote.name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(add_remote.name), TRUE);
    gtk_widget_set_hexpand(add_remote.name, TRUE);
    GtkWidget *widget = gtk_label_new_with_mnemonic("_Name:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), add_remote.name);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), add_remote.name, 1, 0, 1, 1);
    add_remote.where = gtk_combo_box_text_new();
    GPtrArray *insts = xd_fp_installations();
    for (guint i = 0; i < insts->len; i++) {
        FlatpakInstallation *inst = g_ptr_array_index(insts, i);
        char id[16];
        g_snprintf(id, sizeof(id), "%u", i);
        const char *label = xd_fp_installation_label(inst);
        char *text = flatpak_installation_get_is_user(inst)
                         ? g_strdup(label)
                         : g_strdup_printf("%s (needs an administrator)", label);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(add_remote.where), id, text);
        g_free(text);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(add_remote.where), 0);
    widget = gtk_label_new_with_mnemonic("Add _for:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), add_remote.where);
    gtk_grid_attach(GTK_GRID(grid), widget, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), add_remote.where, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(add_remote.window))), box,
                       TRUE, TRUE, 0);
    gtk_dialog_add_button(GTK_DIALOG(add_remote.window), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(add_remote.window), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(add_remote.window), GTK_RESPONSE_OK);
    g_signal_connect(add_remote.from_file, "toggled", G_CALLBACK(on_source), &add_remote);
    g_signal_connect(add_remote.file, "changed", G_CALLBACK(on_file_changed), &add_remote);
    g_signal_connect(add_remote.name, "changed", G_CALLBACK(on_name_changed), &add_remote);
    g_signal_connect(add_remote.browse, "clicked", G_CALLBACK(on_browse), &add_remote);
    add_sensitivity(&add_remote);
    gtk_widget_show_all(add_remote.window);

    while (gtk_dialog_run(GTK_DIALOG(add_remote.window)) == GTK_RESPONSE_OK) {
        char *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(add_remote.name))));
        /* flatpak remote-add's rule: no slashes, and not empty. */
        if (*name == '\0' || strchr(name, '/') != nullptr || name[0] == '.' ||
            g_utf8_strchr(name, -1, ' ') != nullptr) {
            xd_message_box(GTK_WINDOW(add_remote.window), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK,
                           FALSE,
                           "Type a name for the remote, without spaces or slashes, such as "
                           "flathub.");
            g_free(name);
            continue;
        }
        const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(add_remote.where));
        FlatpakInstallation *inst = id != nullptr && (guint)atoi(id) < insts->len
                                        ? g_ptr_array_index(insts, (guint)atoi(id))
                                        : nullptr;
        FlatpakRemote *existing = inst != nullptr ? flatpak_installation_get_remote_by_name(
                                                        inst, name, nullptr, nullptr)
                                                  : nullptr;
        if (existing != nullptr) {
            g_object_unref(existing);
            xd_message_box(GTK_WINDOW(add_remote.window), XD_FLATPAK_TITLE, XD_MB_ERROR, XD_MB_OK,
                           FALSE, "There is already a remote with this name. Type another name.");
            g_free(name);
            continue;
        }
        FlatpakRemote *remote = inst != nullptr ? make_remote(&add_remote, name) : nullptr;
        if (remote != nullptr) {
            run(remotes_tab, OP_ADD, inst, name, remote);
            g_object_unref(remote);
            g_free(name);
            break;
        }
        g_free(name);
    }
    g_ptr_array_unref(insts);
    gtk_widget_destroy(add_remote.window);
}

/* ---- Remove, Enable and Disable --------------------------------------------- */

static void on_remove([[maybe_unused]] GtkButton *button, gpointer data) {
    remotes *remotes_tab = data;
    char *name;
    FlatpakInstallation *inst;
    if (!selected(remotes_tab, &name, &inst, nullptr))
        return;
    char *message = g_strdup_printf("Are you sure you want to remove the remote \"%s\"?\n\n"
                                    "Applications can't be installed or updated from it any more.",
                                    name);
    if (xd_message_box(GTK_WINDOW(remotes_tab->window), "Confirm Remote Delete", XD_MB_QUESTION,
                       XD_MB_YES_NO, TRUE, message) == GTK_RESPONSE_YES)
        run(remotes_tab, OP_REMOVE, inst, name, nullptr);
    g_free(message);
    g_free(name);
    g_object_unref(inst);
}

static void on_toggle([[maybe_unused]] GtkButton *button, gpointer data) {
    remotes *remotes_tab = data;
    char *name;
    FlatpakInstallation *inst;
    bool disabled;
    if (!selected(remotes_tab, &name, &inst, &disabled))
        return;
    run(remotes_tab, disabled ? OP_ENABLE : OP_DISABLE, inst, name, nullptr);
    g_free(name);
    g_object_unref(inst);
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) { g_free(data); }

GtkWidget *xd_fp_remotes_page(GtkWidget *window) {
    remotes *remotes_tab = g_new0(remotes, 1);
    remotes_tab->window = window;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page), 10);
    g_signal_connect(page, "destroy", G_CALLBACK(on_destroy), remotes_tab);
    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *img = gtk_image_new_from_icon_name("network-server", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(img, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(intro),
                       xd_fp_label("Remotes are the repositories Flatpak installs and updates "
                                   "applications from. A disabled remote is kept, but not used."),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), intro, FALSE, FALSE, 0);
    GtkWidget *widget = gtk_label_new_with_mnemonic("_Remote repositories:");
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_box_pack_start(GTK_BOX(page), widget, FALSE, FALSE, 0);
    remotes_tab->store =
        gtk_list_store_new(COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                           G_TYPE_STRING, FLATPAK_TYPE_INSTALLATION, G_TYPE_BOOLEAN);
    remotes_tab->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(remotes_tab->store));
    g_object_unref(remotes_tab->store);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), remotes_tab->view);
    xd_fp_text_column(remotes_tab->view, "Name", COL_NAME, -1);
    xd_fp_text_column(remotes_tab->view, "Title", COL_TITLE, -1);
    xd_fp_text_column(remotes_tab->view, "Address", COL_URL, -1);
    for (int column_id = COL_WHERE; column_id <= COL_STATUS; column_id++) {
        GtkTreeViewColumn *col = xd_fp_text_column(
            remotes_tab->view, column_id == COL_WHERE ? "For" : "Status", column_id, -1);
        gtk_tree_view_column_set_expand(col, FALSE);
        gtk_tree_view_column_set_min_width(col, 64);
    }
    gtk_box_pack_start(GTK_BOX(page), xd_fp_scrolled(remotes_tab->view, 220), TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    remotes_tab->add = xd_fp_button("A_dd...");
    remotes_tab->remove = xd_fp_button("Remo_ve");
    remotes_tab->toggle = xd_fp_button("Di_sable");
    g_signal_connect(remotes_tab->add, "clicked", G_CALLBACK(on_add), remotes_tab);
    g_signal_connect(remotes_tab->remove, "clicked", G_CALLBACK(on_remove), remotes_tab);
    g_signal_connect(remotes_tab->toggle, "clicked", G_CALLBACK(on_toggle), remotes_tab);
    gtk_box_pack_end(GTK_BOX(row), remotes_tab->toggle, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), remotes_tab->remove, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), remotes_tab->add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(remotes_tab->view)), "changed",
                     G_CALLBACK(on_selected), remotes_tab);
    fill(remotes_tab);
    buttons(remotes_tab);
    return page;
}
