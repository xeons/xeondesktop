/* SPDX-License-Identifier: MIT
 * The SSH Keys tab: the key pairs in ~/.ssh and which of them ssh-agent
 * holds. ssh-keygen and ssh-add do the work (sshkeys.h); they run in the
 * background, as a large RSA key takes a while to make.
 */
#include <errno.h>
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "keys.h"
#include "keyslib.h"
#include "sshkeys.h"

enum { SK_ICON, SK_NAME, SK_COMMENT, SK_TYPE, SK_AGENT, SK_INDEX, SK_NCOLS };

typedef struct ssh_page {
    keys_app *app;
    GtkWidget *page, *view, *details, *agent_note, *new_key, *passphrase, *del, *copy, *agent;
    GtkListStore *store;
    GPtrArray *keys; /* xd_ssh_key */
    bool have_agent;
    bool running;    /* a tool is at work */
    GCancellable *cancel;
} ssh_page;

static GtkWindow *parent(ssh_page *page) { return GTK_WINDOW(page->app->window); }

static xd_ssh_key *selected(ssh_page *page) {
    GtkTreeModel *model = nullptr;
    GtkTreeIter it;
    if (page->keys == nullptr ||
        !gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
                                         &model, &it))
        return nullptr;
    int i = -1;
    gtk_tree_model_get(model, &it, SK_INDEX, &i, -1);
    return i >= 0 && (guint)i < page->keys->len ? page->keys->pdata[i] : nullptr;
}

static void sync_buttons(ssh_page *page) {
    xd_ssh_key *key = selected(page);
    bool idle = !page->running;
    gtk_widget_set_sensitive(page->new_key, idle);
    gtk_widget_set_sensitive(page->passphrase, idle && key != nullptr && key->has_private);
    gtk_widget_set_sensitive(page->del, idle && key != nullptr);
    gtk_widget_set_sensitive(page->copy, key != nullptr);
    gtk_button_set_label(GTK_BUTTON(page->agent),
                         key != nullptr && key->in_agent ? "Remove from _Agent" : "Add to _Agent");
    gtk_widget_set_sensitive(page->agent, idle && page->have_agent && key != nullptr &&
                                              (key->has_private || key->in_agent));
    if (key == nullptr) {
        gtk_label_set_text(GTK_LABEL(page->details), "");
        return;
    }
    char *text = g_strdup_printf("Fingerprint: %s\nKey: %s, %d bits. %s", key->fingerprint,
                                 key->type, key->bits,
                                 !key->has_private ? "Only the public key is here."
                                 : key->encrypted  ? "Its private key is protected by a passphrase."
                                                   : "Its private key has no passphrase.");
    gtk_label_set_text(GTK_LABEL(page->details), text);
    g_free(text);
}

static void refresh(ssh_page *page) {
    xd_ssh_key *was = selected(page);
    char *keep = was != nullptr ? g_strdup(was->name) : nullptr;
    gtk_list_store_clear(page->store);
    if (page->keys != nullptr)
        g_ptr_array_unref(page->keys);
    page->keys = xd_ssh_list_keys(page->app->ssh_dir);
    GError *error = nullptr;
    char **fps = xd_ssh_agent_fingerprints(&error);
    page->have_agent = fps != nullptr;
    xd_ssh_mark_agent_keys(page->keys, fps);
    GtkTreeIter select;
    bool found = false;
    for (guint i = 0; i < page->keys->len; i++) {
        xd_ssh_key *key = page->keys->pdata[i];
        GtkTreeIter it;
        gtk_list_store_insert_with_values(
            page->store, &it, -1, SK_ICON,
            key->has_private ? "application-x-pem-key" : "text-x-generic", SK_NAME, key->name,
            SK_COMMENT, key->comment, SK_TYPE, key->type, SK_AGENT, key->in_agent ? "Yes" : "",
            SK_INDEX, (int)i, -1);
        if (keep != nullptr && strcmp(keep, key->name) == 0) {
            select = it;
            found = true;
        }
    }
    if (found)
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)),
                                       &select);
    if (fps == nullptr) {
        gtk_label_set_text(GTK_LABEL(page->agent_note),
                           "No SSH agent is running, so keys can't be loaded into one.");
    } else {
        guint key_count = g_strv_length(fps);
        char *text =
            key_count == 0 ? g_strdup("The SSH agent is running. It holds no keys.")
            : key_count == 1
                ? g_strdup("The SSH agent is running. It holds 1 key.")
                : g_strdup_printf("The SSH agent is running. It holds %u keys.", key_count);
        gtk_label_set_text(GTK_LABEL(page->agent_note), text);
        g_free(text);
    }
    g_strfreev(fps);
    g_clear_error(&error);
    g_free(keep);
    sync_buttons(page);
}

/* ---- running ssh-keygen and ssh-add ------------------------------------------------- */

typedef struct ssh_job {
    ssh_page *p;
    char *failed; /* what to say if it fails */
    char *silent; /* why, if the tool doesn't say (nullable) */
    char *input;  /* the answers: cleared when done */
} ssh_job;

static void job_free(ssh_job *tool_job) {
    memset(tool_job->input, 0, strlen(tool_job->input));
    g_free(tool_job->input);
    g_free(tool_job->failed);
    g_free(tool_job->silent);
    g_free(tool_job);
}

static void on_tool_done(GObject *source, GAsyncResult *res, gpointer data) {
    ssh_job *tool_job = data;
    char *err = nullptr;
    GError *error = nullptr;
    bool ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), res, nullptr, &err,
                                                   &error);
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(error);
        g_free(err);
        job_free(tool_job);
        return;
    }
    ssh_page *page = tool_job->p;
    page->running = false;
    keys_busy(page->page, false);
    if (!ok) {
        keys_error(parent(page), tool_job->failed, error);
    } else if (!g_subprocess_get_successful(G_SUBPROCESS(source))) {
        /* ssh-add says nothing when askpass gives up after a wrong
         * passphrase. */
        bool said = err != nullptr && g_strstrip(err)[0] != '\0';
        char *why = !said && tool_job->silent != nullptr ? g_strdup(tool_job->silent)
                                                         : xd_ssh_error_text(err);
        GError *tool_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, why);
        keys_error(parent(page), tool_job->failed, tool_error);
        g_error_free(tool_error);
        g_free(why);
    }
    g_clear_error(&error);
    g_free(err);
    job_free(tool_job);
    refresh(page);
}

/* Runs ssh-keygen or ssh-add with these answers to its questions. */
static void run_tool(ssh_page *page, char **argv, const char *const *answers, const char *failed,
                     const char *silent) {
    GError *error = nullptr;
    GSubprocess *proc = xd_ssh_spawn((const char *const *)argv, page->app->askpass, &error);
    if (proc == nullptr) {
        keys_error(parent(page), failed, error);
        g_error_free(error);
        return;
    }
    ssh_job *tool_job = g_new0(ssh_job, 1);
    tool_job->p = page;
    tool_job->failed = g_strdup(failed);
    tool_job->silent = g_strdup(silent);
    tool_job->input = xd_ssh_answers(answers);
    page->running = true;
    sync_buttons(page);
    keys_busy(page->page, true);
    g_subprocess_communicate_utf8_async(proc, tool_job->input, page->cancel, on_tool_done,
                                        tool_job);
    g_object_unref(proc);
}

/* ---- New ------------------------------------------------------------------------------- */

static const struct {
    const char *label, *type;
    int bits;
} key_types[] = {
    {"Ed25519 (recommended)", "ed25519", 0},
    {"RSA, 4096 bits", "rsa", 4096},
    {"ECDSA, 256 bits", "ecdsa", 256},
};

typedef struct new_dlg {
    ssh_page *p;
    GtkWidget *type, *name;
    bool name_edited, setting;
} new_dlg;

static void on_type_changed(GtkComboBox *combo, gpointer data) {
    new_dlg *new_key = data;
    if (new_key->name_edited)
        return;
    int type_index = gtk_combo_box_get_active(combo);
    char *name = xd_ssh_new_key_name(new_key->p->app->ssh_dir,
                                     key_types[type_index < 0 ? 0 : type_index].type);
    new_key->setting = true;
    gtk_entry_set_text(GTK_ENTRY(new_key->name), name);
    new_key->setting = false;
    g_free(name);
}

static void on_name_edited([[maybe_unused]] GtkEditable *editable, gpointer data) {
    new_dlg *new_key = data;
    if (!new_key->setting)
        new_key->name_edited = true;
}

static void on_new(ssh_page *page) {
    GtkWidget *box = nullptr;
    GtkWidget *dialog_widget = keys_dialog(parent(page), "New SSH Key", "C_reate", &box);
    gtk_box_pack_start(GTK_BOX(box),
                       keys_label("A new key pair is made in your SSH folder. Give servers its "
                                  "public key (Copy Public Key); the private key stays here."),
                       FALSE, FALSE, 0);
    new_dlg new_key = {.p = page};
    GtkWidget *grid = keys_grid();
    new_key.type = gtk_combo_box_text_new();
    for (size_t i = 0; i < G_N_ELEMENTS(key_types); i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(new_key.type), key_types[i].label);
    new_key.name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(new_key.name), TRUE);
    GtkWidget *comment = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(comment), TRUE);
    char *who = g_strdup_printf("%s@%s", g_get_user_name(), g_get_host_name());
    gtk_entry_set_text(GTK_ENTRY(comment), who);
    g_free(who);
    GtkWidget *pass = keys_password_entry();
    GtkWidget *again = keys_password_entry();
    keys_grid_row(grid, 0, "_Type:", new_key.type);
    keys_grid_row(grid, 1, "_File name:", new_key.name);
    keys_grid_row(grid, 2, "_Comment:", comment);
    keys_grid_row(grid, 3, "Pass_phrase:", pass);
    keys_grid_row(grid, 4, "Co_nfirm passphrase:", again);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    GtkWidget *note = keys_label("Without a passphrase, anyone who gets a copy of the private key "
                                 "can use it.");
    gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    g_signal_connect(new_key.type, "changed", G_CALLBACK(on_type_changed), &new_key);
    g_signal_connect(new_key.name, "changed", G_CALLBACK(on_name_edited), &new_key);
    gtk_combo_box_set_active(GTK_COMBO_BOX(new_key.type), 0);
    gtk_widget_show_all(dialog_widget);
    gtk_widget_grab_focus(pass);

    while (gtk_dialog_run(GTK_DIALOG(dialog_widget)) == GTK_RESPONSE_OK) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(new_key.name));
        if (!xd_ssh_valid_key_name(name)) {
            xd_message_box(GTK_WINDOW(dialog_widget), "New SSH Key", XD_MB_ERROR, XD_MB_OK, FALSE,
                           "Type a file name for the key, without slashes. It can't start with "
                           "a dot, end in .pub, or be one of the files SSH keeps there "
                           "(config, known_hosts, authorized_keys).");
            continue;
        }
        char *path = g_build_filename(page->app->ssh_dir, name, nullptr);
        char *pub = g_strconcat(path, ".pub", nullptr);
        bool exists = g_file_test(path, G_FILE_TEST_EXISTS) || g_file_test(pub, G_FILE_TEST_EXISTS);
        g_free(pub);
        if (exists) {
            char *text = g_strdup_printf("A key named %s already exists. Type another name.", name);
            xd_message_box(GTK_WINDOW(dialog_widget), "New SSH Key", XD_MB_ERROR, XD_MB_OK, FALSE,
                           text);
            g_free(text);
            g_free(path);
            continue;
        }
        if (!keys_check_match(GTK_WINDOW(dialog_widget), "New SSH Key", pass, again)) {
            g_free(path);
            continue;
        }
        /* ssh wants the folder its own: 0700. */
        if (g_mkdir_with_parents(page->app->ssh_dir, 0700) != 0) {
            GIOErrorEnum code = g_io_error_from_errno(errno);
            GError *error =
                g_error_new(G_IO_ERROR, (int)code, "%s: %s", page->app->ssh_dir, g_strerror(errno));
            keys_error(GTK_WINDOW(dialog_widget), "The SSH folder could not be made.", error);
            g_error_free(error);
            g_free(path);
            continue;
        }
        int type_index = XD_CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(new_key.type)), 0,
                                  (int)G_N_ELEMENTS(key_types) - 1);
        const char *passphrase = gtk_entry_get_text(GTK_ENTRY(pass));
        bool empty = passphrase[0] == '\0';
        char **argv = xd_ssh_keygen_argv(key_types[type_index].type, key_types[type_index].bits,
                                         gtk_entry_get_text(GTK_ENTRY(comment)), path, empty);
        /* It asks twice: the passphrase, then again. */
        const char *answers[] = {passphrase, passphrase, nullptr};
        run_tool(page, argv, empty ? nullptr : answers, "The key could not be made.", nullptr);
        g_strfreev(argv);
        g_free(path);
        break;
    }
    gtk_widget_destroy(dialog_widget);
}

/* ---- Change Passphrase ------------------------------------------------------------- */

static void on_passphrase(ssh_page *page) {
    xd_ssh_key *key = selected(page);
    if (key == nullptr || !key->has_private)
        return;
    GtkWidget *box = nullptr;
    GtkWidget *dialog_widget = keys_dialog(parent(page), "Change Passphrase", nullptr, &box);
    char *text =
        g_strdup_printf("Change the passphrase that protects the private key %s.", key->name);
    gtk_box_pack_start(GTK_BOX(box), keys_label(text), FALSE, FALSE, 0);
    g_free(text);
    GtkWidget *grid = keys_grid();
    GtkWidget *old = nullptr;
    int row = 0;
    if (key->encrypted) {
        old = keys_password_entry();
        keys_grid_row(grid, row++, "_Old passphrase:", old);
    }
    GtkWidget *pass = keys_password_entry();
    GtkWidget *again = keys_password_entry();
    keys_grid_row(grid, row++, "_New passphrase:", pass);
    keys_grid_row(grid, row, "Con_firm new passphrase:", again);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog_widget);
    gtk_widget_grab_focus(old != nullptr ? old : pass);
    while (gtk_dialog_run(GTK_DIALOG(dialog_widget)) == GTK_RESPONSE_OK) {
        if (!keys_check_match(GTK_WINDOW(dialog_widget), "Change Passphrase", pass, again))
            continue;
        const char *now = gtk_entry_get_text(GTK_ENTRY(pass));
        bool empty = now[0] == '\0';
        char **argv = xd_ssh_passwd_argv(key->path, empty);
        /* The old one (if any), then the new one twice unless -N "". */
        const char *answers[4] = {nullptr};
        int answer_count = 0;
        if (old != nullptr)
            answers[answer_count++] = gtk_entry_get_text(GTK_ENTRY(old));
        if (!empty) {
            answers[answer_count++] = now;
            answers[answer_count++] = now;
        }
        run_tool(page, argv, answers, "The passphrase could not be changed.", nullptr);
        g_strfreev(argv);
        break;
    }
    gtk_widget_destroy(dialog_widget);
}

/* ---- Delete, Copy Public Key ------------------------------------------------------------ */

/* Sends the key's files to the Recycle Bin, the private half first (a
 * pair is never left with only it). Where there's no Recycle Bin, Windows
 * offers to delete them permanently. */
static void remove_key(ssh_page *page, const xd_ssh_key *key) {
    const char *paths[] = {key->has_private ? key->path : nullptr, key->pub_path};
    bool permanent = false;
    for (size_t i = 0; i < G_N_ELEMENTS(paths); i++) {
        if (paths[i] == nullptr)
            continue;
        GFile *file = g_file_new_for_path(paths[i]);
        GError *error = nullptr;
        bool ok =
            permanent ? g_file_delete(file, nullptr, &error) : g_file_trash(file, nullptr, &error);
        if (!ok && !permanent && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
            g_clear_error(&error);
            char *message = g_strdup_printf("%s can't be moved to the Recycle Bin. Do you want to "
                                            "delete it permanently?",
                                            key->name);
            bool yes = xd_message_box(parent(page), "Confirm Key Delete", XD_MB_WARNING,
                                      XD_MB_YES_NO, TRUE, message) == GTK_RESPONSE_YES;
            g_free(message);
            if (!yes) {
                g_object_unref(file);
                return;
            }
            permanent = true;
            ok = g_file_delete(file, nullptr, &error);
        }
        g_object_unref(file);
        if (!ok) {
            keys_error(parent(page), "The key could not be deleted.", error);
            g_error_free(error);
            return;
        }
    }
}

static void on_delete(ssh_page *page) {
    xd_ssh_key *key = selected(page);
    if (key == nullptr || page->running)
        return;
    char *message = g_strdup_printf(
        key->has_private
            ? "Are you sure you want to send the key pair \"%s\" to the Recycle Bin?\n\nServers "
              "that know only this key won't let you log on any more."
            : "Are you sure you want to send the public key \"%s\" to the Recycle Bin?",
        key->name);
    bool yes = xd_message_box(parent(page), "Confirm Key Delete", XD_MB_WARNING, XD_MB_YES_NO, TRUE,
                              message) == GTK_RESPONSE_YES;
    g_free(message);
    if (yes)
        remove_key(page, key);
    refresh(page);
}

static void on_copy(ssh_page *page) {
    xd_ssh_key *key = selected(page);
    if (key == nullptr)
        return;
    char *text = nullptr;
    GError *error = nullptr;
    if (!g_file_get_contents(key->pub_path, &text, nullptr, &error)) {
        keys_error(parent(page), "The public key could not be read.", error);
        g_error_free(error);
        return;
    }
    keys_copy(g_strstrip(text));
    g_free(text);
}

/* ---- the agent ------------------------------------------------------------------------------ */

static void on_agent(ssh_page *page) {
    xd_ssh_key *key = selected(page);
    if (key == nullptr || page->running)
        return;
    if (key->in_agent) {
        /* By its public key: no passphrase needed. */
        char **argv = xd_ssh_add_argv(key->pub_path, true);
        run_tool(page, argv, nullptr, "The key could not be removed from the SSH agent.", nullptr);
        g_strfreev(argv);
        return;
    }
    char *pass = nullptr;
    if (key->encrypted) {
        char *text = g_strdup_printf("Type the passphrase for the key %s:", key->name);
        pass = keys_ask_passphrase(parent(page), "Add to Agent", text);
        g_free(text);
        if (pass == nullptr)
            return;
    }
    char **argv = xd_ssh_add_argv(key->path, false);
    const char *answers[] = {pass, nullptr};
    run_tool(page, argv, pass != nullptr ? answers : nullptr,
             "The key could not be added to the SSH agent.", "The passphrase is incorrect.");
    g_strfreev(argv);
    if (pass != nullptr) {
        memset(pass, 0, strlen(pass));
        g_free(pass);
    }
}

/* ---- the page ----------------------------------------------------------------------------- */

static void on_selection([[maybe_unused]] GtkTreeSelection *tree_selection, gpointer data) {
    sync_buttons(data);
}

static gboolean on_view_key([[maybe_unused]] GtkWidget *widget, GdkEventKey *ev, gpointer data) {
    if (ev->keyval == GDK_KEY_Delete) {
        on_delete(data);
        return TRUE;
    }
    if (ev->keyval == GDK_KEY_F5) {
        refresh(data);
        return TRUE;
    }
    return FALSE;
}

static void on_map([[maybe_unused]] GtkWidget *widget, gpointer data) {
    /* Keys made or loaded elsewhere meanwhile. */
    ssh_page *page = data;
    if (!page->running)
        refresh(page);
}

static void on_destroy([[maybe_unused]] GtkWidget *widget, gpointer data) {
    ssh_page *page = data;
    g_cancellable_cancel(page->cancel);
    g_object_unref(page->cancel);
    if (page->keys != nullptr)
        g_ptr_array_unref(page->keys);
    g_object_unref(page->store);
    g_free(page);
}

GtkWidget *ssh_page_new(keys_app *app) {
    ssh_page *page = g_new0(ssh_page, 1);
    page->app = app;
    page->cancel = g_cancellable_new();
    page->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(page->page), 10);

    GtkWidget *intro = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("application-x-pem-key", GTK_ICON_SIZE_DND);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(intro), icon, FALSE, FALSE, 0);
    char *text = g_strdup_printf("SSH keys log you on to other computers without a password. "
                                 "These key pairs are in your SSH folder (%s):",
                                 app->ssh_dir);
    gtk_box_pack_start(GTK_BOX(intro), keys_label(text), TRUE, TRUE, 0);
    g_free(text);
    gtk_box_pack_start(GTK_BOX(page->page), intro, FALSE, FALSE, 0);

    page->store = gtk_list_store_new(SK_NCOLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                     G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    page->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->store));
    GtkTreeViewColumn *column = keys_text_column(page->view, "Name", SK_NAME, 140);
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(column, pix, FALSE);
    gtk_tree_view_column_add_attribute(column, pix, "icon-name", SK_ICON);
    gtk_cell_layout_reorder(GTK_CELL_LAYOUT(column), pix, 0);
    keys_text_column(page->view, "Comment", SK_COMMENT, 175);
    keys_text_column(page->view, "Type", SK_TYPE, 80);
    keys_text_column(page->view, "In Agent", SK_AGENT, 70);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->view)), "changed",
                     G_CALLBACK(on_selection), page);
    g_signal_connect(page->view, "key-press-event", G_CALLBACK(on_view_key), page);
    gtk_box_pack_start(GTK_BOX(page->page), keys_scrolled(page->view, 480, 150), TRUE, TRUE, 0);

    page->details = keys_label("");
    gtk_label_set_selectable(GTK_LABEL(page->details), TRUE);
    gtk_label_set_lines(GTK_LABEL(page->details), 2);
    gtk_widget_set_size_request(page->details, -1, 34);
    gtk_box_pack_start(GTK_BOX(page->page), page->details, FALSE, FALSE, 0);

    page->new_key = keys_button("_New...", G_CALLBACK(on_new), page);
    page->passphrase = keys_button("C_hange Passphrase...", G_CALLBACK(on_passphrase), page);
    page->copy = keys_button("_Copy Public Key", G_CALLBACK(on_copy), page);
    page->del = keys_button("_Delete", G_CALLBACK(on_delete), page);
    gtk_box_pack_start(
        GTK_BOX(page->page),
        keys_button_row(page->new_key, page->passphrase, page->copy, page->del, nullptr), FALSE,
        FALSE, 0);

    GtkWidget *frame = gtk_frame_new("SSH Agent");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(row), 8);
    page->agent_note = keys_label("");
    page->agent = keys_button("Add to _Agent", G_CALLBACK(on_agent), page);
    gtk_widget_set_valign(page->agent, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), page->agent_note, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), page->agent, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(frame), row);
    gtk_box_pack_start(GTK_BOX(page->page), frame, FALSE, FALSE, 0);

    g_signal_connect(page->page, "map", G_CALLBACK(on_map), page);
    g_signal_connect(page->page, "destroy", G_CALLBACK(on_destroy), page);
    /* The list is read when the tab is first shown. */
    sync_buttons(page);
    return page->page;
}
