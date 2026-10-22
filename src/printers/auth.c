/* SPDX-License-Identifier: MIT */
#include <cups/cups.h>
#include <gtk/gtk.h>
#include <string.h>

#include "auth.h"

static constexpr int MAX_TRIES = 3;

static GtkWindow *active_window(void) {
    GList *all = gtk_window_list_toplevels();
    GtkWindow *found = nullptr;
    for (GList *list = all; list != nullptr && found == nullptr; list = list->next) {
        if (gtk_window_is_active(list->data))
            found = list->data;
    }
    g_list_free(all);
    return found;
}

static GtkWidget *grid_label(const char *mnemonic, GtkWidget *target) {
    GtkWidget *widget = gtk_label_new_with_mnemonic(mnemonic);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(widget), target);
    return widget;
}

/* The server's name for the box. A local socket's "host" is its path:
 * this computer. */
static void server_name(http_t *http, char *out, size_t size) {
    char host[256] = "";
    if (http != nullptr)
        httpGetHostname(http, host, (int)sizeof(host));
    g_strlcpy(out, host[0] == '\0' || host[0] == '/' ? "localhost" : host, size);
}

/* The box itself, on the main thread. `user` comes in as the name to
 * offer and goes out as the one typed; `pass` gets the password. False
 * if it was cancelled. */
static bool password_box(const char *server, bool retry, char *user, size_t user_size,
                         char *pass, size_t pass_size) {
    GtkWidget *dlg = gtk_dialog_new();
    char *title = g_strdup_printf("Connect to %s", server);
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    g_free(title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWindow *parent = active_window();
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    char *text = g_strdup_printf(
        retry ? "The user name or password is incorrect. Type them again to connect to %s."
              : "Connecting to %s. Changing printers needs the user name and password of "
                "a printer administrator.",
        server);
    GtkWidget *msg = gtk_label_new(text);
    g_free(text);
    gtk_label_set_line_wrap(GTK_LABEL(msg), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(msg), 44);
    gtk_label_set_xalign(GTK_LABEL(msg), 0);
    gtk_box_pack_start(GTK_BOX(top), msg, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name), user);
    gtk_entry_set_activates_default(GTK_ENTRY(name), TRUE);
    gtk_widget_set_hexpand(name, TRUE);
    GtkWidget *secret = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(secret), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(secret), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(secret), TRUE);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_User name:", name), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), grid_label("_Password:", secret), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), secret, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    gtk_widget_grab_focus(secret);

    bool ok = gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK;
    if (ok) {
        g_strlcpy(user, gtk_entry_get_text(GTK_ENTRY(name)), user_size);
        g_strlcpy(pass, gtk_entry_get_text(GTK_ENTRY(secret)), pass_size);
    }
    /* Not left in the entry's buffer either. */
    gtk_entry_set_text(GTK_ENTRY(secret), "");
    gtk_widget_destroy(dlg);
    return ok;
}

/* ---- a worker thread's requests ------------------------------------------------------ */

/* A worker's question, answered on the main thread while the worker
 * waits. */
typedef struct question {
    GMutex lock;
    GCond answered;
    bool done, ok, retry;
    char server[256], user[256], pass[256];
} question;

static gboolean answer(gpointer data) {
    question *asked = data;
    bool ok = password_box(asked->server, asked->retry, asked->user, sizeof(asked->user),
                           asked->pass, sizeof(asked->pass));
    g_mutex_lock(&asked->lock);
    asked->ok = ok;
    asked->done = true;
    g_cond_signal(&asked->answered);
    g_mutex_unlock(&asked->lock);
    return G_SOURCE_REMOVE;
}

/* libcups keeps the user name and the password callback per thread; the
 * worker's tries and password are its own too. */
static thread_local int worker_tries;
static thread_local char worker_password[256];

static const char *ask_from_worker([[maybe_unused]] const char *prompt, http_t *http,
                                   [[maybe_unused]] const char *method,
                                   [[maybe_unused]] const char *resource,
                                   [[maybe_unused]] void *data) {
    explicit_bzero(worker_password, sizeof(worker_password));
    if (++worker_tries > MAX_TRIES)
        return nullptr;
    question asked = {.retry = worker_tries > 1};
    g_mutex_init(&asked.lock);
    g_cond_init(&asked.answered);
    server_name(http, asked.server, sizeof(asked.server));
    g_strlcpy(asked.user, cupsUser(), sizeof(asked.user));
    g_main_context_invoke(nullptr, answer, &asked);
    g_mutex_lock(&asked.lock);
    while (!asked.done)
        g_cond_wait(&asked.answered, &asked.lock);
    g_mutex_unlock(&asked.lock);
    g_mutex_clear(&asked.lock);
    g_cond_clear(&asked.answered);
    const char *result = nullptr;
    if (asked.ok) {
        if (asked.user[0] != '\0')
            cupsSetUser(asked.user);
        g_strlcpy(worker_password, asked.pass, sizeof(worker_password));
        result = worker_password;
    } else {
        worker_tries = MAX_TRIES;
    }
    explicit_bzero(asked.pass, sizeof(asked.pass));
    return result;
}

static const char *no_password([[maybe_unused]] const char *prompt,
                               [[maybe_unused]] http_t *http,
                               [[maybe_unused]] const char *method,
                               [[maybe_unused]] const char *resource,
                               [[maybe_unused]] void *data) {
    return nullptr;
}

/* The main thread changes nothing (the windows make their changes in
 * workers): its reads never ask, on the terminal or otherwise. */
void xd_cups_auth_init(void) { cupsSetPasswordCB2(no_password, nullptr); }

void xd_cups_auth_worker_begin(void) {
    worker_tries = 0;
    explicit_bzero(worker_password, sizeof(worker_password));
    cupsSetPasswordCB2(ask_from_worker, nullptr);
}

void xd_cups_auth_worker_end(void) {
    explicit_bzero(worker_password, sizeof(worker_password));
    cupsSetPasswordCB2(no_password, nullptr);
}
