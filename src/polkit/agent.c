/* SPDX-License-Identifier: MIT
 * xd-polkit: the session's polkit authentication agent.
 *
 * When a program asks for something only an administrator may do (rename
 * the computer, change the power profile, mount a disk...), polkitd asks
 * the agent of the user's session to confirm it with a password. This one
 * asks in a Windows XP-style "Run As" dialog: the reason, whose password
 * (when several administrators qualify), and the password itself, checked
 * by polkit's own helper through PAM.
 *
 *   xd-polkit              agent for this login session (started with it)
 *   xd-polkit --process PID   agent for one process only, as pkttyagent
 *                             --process; for testing beside another
 *                             desktop's agent
 */
#define POLKIT_AGENT_I_KNOW_API_IS_SUBJECT_TO_CHANGE
#include <polkitagent/polkitagent.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#include "../gtkapp/gtkapp.h"

/* ---- one authentication request ------------------------------------------------- */

typedef struct request {
    GTask *task;
    char *cookie;
    GList *identities; /* PolkitIdentity, owned */
    GCancellable *cancellable;
    gulong cancel_handler;
    PolkitAgentSession *session;

    GtkWidget *dialog;
    GtkWidget *user;     /* combo of identities */
    GtkWidget *password;
    GtkWidget *status;   /* error / info line */
    GtkWidget *ok;
    gboolean done;
} request;

static void request_free(request *auth_request) {
    if (auth_request->session != nullptr) {
        /* Cancelling emits "completed": on_completed would then touch the
         * dialog finish() has just destroyed. */
        g_signal_handlers_disconnect_by_data(auth_request->session, auth_request);
        polkit_agent_session_cancel(auth_request->session);
        g_clear_object(&auth_request->session);
    }
    if (auth_request->cancel_handler != 0)
        g_cancellable_disconnect(auth_request->cancellable, auth_request->cancel_handler);
    g_clear_object(&auth_request->cancellable);
    g_list_free_full(auth_request->identities, g_object_unref);
    g_free(auth_request->cookie);
    g_free(auth_request);
}

/* Ends the request one way or another; the dialog goes too. */
static void finish(request *auth_request, gboolean ok, const char *why) {
    if (auth_request->done)
        return;
    auth_request->done = TRUE;
    if (ok)
        g_task_return_boolean(auth_request->task, TRUE);
    else
        g_task_return_new_error(auth_request->task, POLKIT_ERROR, POLKIT_ERROR_CANCELLED, "%s",
                                why != nullptr ? why : "Cancelled");
    g_clear_object(&auth_request->task);
    if (auth_request->dialog != nullptr) {
        GtkWidget *widget = auth_request->dialog;
        auth_request->dialog = nullptr;
        gtk_widget_destroy(widget);
    }
    request_free(auth_request);
}

static void set_status(request *auth_request, const char *text, gboolean error) {
    gtk_label_set_text(GTK_LABEL(auth_request->status), text != nullptr ? text : "");
    GtkStyleContext *sc = gtk_widget_get_style_context(auth_request->status);
    if (error)
        gtk_style_context_add_class(sc, "error");
    else
        gtk_style_context_remove_class(sc, "error");
}

static void set_busy(request *auth_request, gboolean busy) {
    gtk_widget_set_sensitive(auth_request->password, !busy);
    gtk_widget_set_sensitive(auth_request->user,
                             !busy && g_list_length(auth_request->identities) > 1);
    gtk_widget_set_sensitive(auth_request->ok, !busy);
}

/* ---- the PAM conversation, through polkit's helper ---- */

static void on_request(PolkitAgentSession *session, const char *prompt, gboolean echo_on,
                       gpointer data) {
    (void)prompt;
    (void)echo_on;
    request *auth_request = data;
    /* The password was typed before OK; answer the helper's prompt. */
    polkit_agent_session_response(session, gtk_entry_get_text(GTK_ENTRY(auth_request->password)));
}

static void on_show_error(PolkitAgentSession *session, const char *text, gpointer data) {
    (void)session;
    set_status(data, text, TRUE);
}

static void on_show_info(PolkitAgentSession *session, const char *text, gpointer data) {
    (void)session;
    set_status(data, text, FALSE);
}

static void on_completed(PolkitAgentSession *session, gboolean gained, gpointer data) {
    (void)session;
    request *auth_request = data;
    g_clear_object(&auth_request->session);
    if (gained) {
        finish(auth_request, TRUE, nullptr);
        return;
    }
    /* Wrong password: let them try again, as Windows does. */
    set_busy(auth_request, FALSE);
    set_status(auth_request,
               "The password is incorrect. Letters in passwords must be typed using the correct "
               "case.",
               TRUE);
    gtk_editable_select_region(GTK_EDITABLE(auth_request->password), 0, -1);
    gtk_widget_grab_focus(auth_request->password);
}

static PolkitIdentity *chosen_identity(request *auth_request) {
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(auth_request->user));
    GList *list = g_list_nth(auth_request->identities, (guint)MAX(i, 0));
    return list != nullptr ? list->data : nullptr;
}

static void authenticate(request *auth_request) {
    PolkitIdentity *id = chosen_identity(auth_request);
    if (id == nullptr)
        return;
    set_busy(auth_request, TRUE);
    set_status(auth_request, "Checking the password...", FALSE);
    auth_request->session = polkit_agent_session_new(id, auth_request->cookie);
    g_signal_connect(auth_request->session, "request", G_CALLBACK(on_request), auth_request);
    g_signal_connect(auth_request->session, "show-error", G_CALLBACK(on_show_error), auth_request);
    g_signal_connect(auth_request->session, "show-info", G_CALLBACK(on_show_info), auth_request);
    g_signal_connect(auth_request->session, "completed", G_CALLBACK(on_completed), auth_request);
    polkit_agent_session_initiate(auth_request->session);
}

static void on_response(GtkDialog *dialog_widget, int response, gpointer data) {
    (void)dialog_widget;
    request *auth_request = data;
    if (response == GTK_RESPONSE_OK)
        authenticate(auth_request);
    else
        finish(auth_request, FALSE, "The user cancelled");
}

/* polkitd withdrew the request (the program gave up or went away). */
static void on_cancelled(GCancellable *cancellable, gpointer data) {
    (void)cancellable;
    request *auth_request = data;
    auth_request->cancel_handler = 0; /* can't disconnect from inside the handler */
    finish(auth_request, FALSE, "Cancelled by the system");
}

/* "xeon (Xeon)" for a unix-user identity; nullptr for others. */
static char *identity_label(PolkitIdentity *id) {
    if (!POLKIT_IS_UNIX_USER(id))
        return nullptr;
    uid_t uid = (uid_t)polkit_unix_user_get_uid(POLKIT_UNIX_USER(id));
    struct passwd *pw = getpwuid(uid);
    if (pw == nullptr)
        return g_strdup_printf("User %u", (unsigned)uid);
    char full[256] = "";
    if (pw->pw_gecos != nullptr) {
        g_strlcpy(full, pw->pw_gecos, sizeof(full));
        char *comma = strchr(full, ',');
        if (comma != nullptr)
            *comma = '\0';
    }
    if (uid == 0)
        return g_strdup("root (Administrator)");
    return full[0] != '\0' && strcmp(full, pw->pw_name) != 0
               ? g_strdup_printf("%s (%s)", pw->pw_name, full)
               : g_strdup(pw->pw_name);
}

static GtkWidget *wrap_label(const char *text) {
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 48);
    return widget;
}

static void show_dialog(request *auth_request, const char *action_id, const char *message,
                        PolkitDetails *details) {
    GtkWidget *widget = gtk_dialog_new();
    auth_request->dialog = widget;
    gtk_window_set_title(GTK_WINDOW(widget), "Run As");
    gtk_window_set_icon_name(GTK_WINDOW(widget), "dialog-password");
    gtk_window_set_resizable(GTK_WINDOW(widget), FALSE);
    gtk_window_set_keep_above(GTK_WINDOW(widget), TRUE);
    auth_request->ok = gtk_dialog_add_button(GTK_DIALOG(widget), "OK", GTK_RESPONSE_OK);
    gtk_dialog_add_button(GTK_DIALOG(widget), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(widget), GTK_RESPONSE_OK);
    g_signal_connect(widget, "response", G_CALLBACK(on_response), auth_request);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(widget))), box, TRUE, TRUE,
                       0);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(text), wrap_label(message), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text),
                       wrap_label("To continue, type an administrator's password and click OK."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 44);
    GtkWidget *ul = gtk_label_new_with_mnemonic("_User name:");
    gtk_label_set_xalign(GTK_LABEL(ul), 0);
    gtk_grid_attach(GTK_GRID(grid), ul, 0, 0, 1, 1);
    auth_request->user = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(auth_request->user, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(ul), auth_request->user);
    /* The current user first when they qualify, as XP's Run As does. */
    int active = 0;
    int i = 0;
    for (GList *list = auth_request->identities; list != nullptr; list = list->next, i++) {
        char *label = identity_label(list->data);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(auth_request->user),
                                       label != nullptr ? label : "(group)");
        if (POLKIT_IS_UNIX_USER(list->data) &&
            (uid_t)polkit_unix_user_get_uid(POLKIT_UNIX_USER(list->data)) == getuid())
            active = i;
        g_free(label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(auth_request->user), active);
    gtk_widget_set_sensitive(auth_request->user, g_list_length(auth_request->identities) > 1);
    gtk_grid_attach(GTK_GRID(grid), auth_request->user, 1, 0, 1, 1);
    GtkWidget *pl = gtk_label_new_with_mnemonic("_Password:");
    gtk_label_set_xalign(GTK_LABEL(pl), 0);
    gtk_grid_attach(GTK_GRID(grid), pl, 0, 1, 1, 1);
    auth_request->password = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(auth_request->password), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(auth_request->password), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_activates_default(GTK_ENTRY(auth_request->password), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(pl), auth_request->password);
    gtk_grid_attach(GTK_GRID(grid), auth_request->password, 1, 1, 1, 1);
    auth_request->status = wrap_label("");
    gtk_grid_attach(GTK_GRID(grid), auth_request->status, 1, 2, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);

    /* What exactly is being allowed, for the curious. */
    GtkWidget *more = gtk_expander_new_with_mnemonic("_Details");
    GString *det = g_string_new(nullptr);
    g_string_append_printf(det, "Action: %s", action_id);
    if (details != nullptr) {
        char **keys = polkit_details_get_keys(details);
        for (char **key = keys; key != nullptr && *key != nullptr; key++)
            g_string_append_printf(det, "\n%s: %s", *key, polkit_details_lookup(details, *key));
        g_strfreev(keys);
    }
    GtkWidget *dl = wrap_label(det->str);
    gtk_label_set_selectable(GTK_LABEL(dl), TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(dl), "dim-label");
    gtk_container_add(GTK_CONTAINER(more), dl);
    g_string_free(det, TRUE);
    gtk_box_pack_start(GTK_BOX(box), more, FALSE, FALSE, 0);

    gtk_widget_show_all(widget);
    gtk_window_present(GTK_WINDOW(widget));
    gtk_widget_grab_focus(auth_request->password);
}

/* ---- the listener ---------------------------------------------------------------- */

#define XD_TYPE_LISTENER (xd_listener_get_type())
G_DECLARE_FINAL_TYPE(XdListener, xd_listener, XD, LISTENER, PolkitAgentListener)

struct _XdListener {
    PolkitAgentListener parent;
};

G_DEFINE_TYPE(XdListener, xd_listener, POLKIT_AGENT_TYPE_LISTENER)

static void initiate(PolkitAgentListener *listener, const char *action_id, const char *message,
                     const char *icon_name, PolkitDetails *details, const char *cookie,
                     GList *identities, GCancellable *cancellable, GAsyncReadyCallback callback,
                     gpointer user_data) {
    (void)icon_name;
    request *auth_request = g_new0(request, 1);
    auth_request->task = g_task_new(listener, cancellable, callback, user_data);
    auth_request->cookie = g_strdup(cookie);
    auth_request->identities =
        g_list_copy_deep(identities, (GCopyFunc)(void (*)(void))g_object_ref, nullptr);
    if (cancellable != nullptr && g_cancellable_is_cancelled(cancellable)) {
        finish(auth_request, FALSE, "Cancelled by the system");
        return;
    }
    show_dialog(auth_request, action_id, message, details);
    if (cancellable != nullptr) {
        auth_request->cancellable = g_object_ref(cancellable);
        auth_request->cancel_handler =
            g_cancellable_connect(cancellable, G_CALLBACK(on_cancelled), auth_request, nullptr);
    }
}

static gboolean initiate_finish(PolkitAgentListener *listener, GAsyncResult *res,
                                GError **error) {
    (void)listener;
    return g_task_propagate_boolean(G_TASK(res), error);
}

static void xd_listener_class_init(XdListenerClass *klass) {
    PolkitAgentListenerClass *lc = POLKIT_AGENT_LISTENER_CLASS(klass);
    lc->initiate_authentication = initiate;
    lc->initiate_authentication_finish = initiate_finish;
}

static void xd_listener_init(XdListener *self) { (void)self; }

/* ---- main ------------------------------------------------------------------------- */

int main(int argc, char **argv) {
    gint64 process = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--process") == 0 && i + 1 < argc) {
            process = g_ascii_strtoll(argv[++i], nullptr, 10);
        } else {
            fprintf(stderr, "usage: xd-polkit [--process PID]\n");
            return EXIT_FAILURE;
        }
    }
    xd_gtk_app_preinit("org.xeondesktop.PolicyKitAgent");
    gtk_init(&argc, &argv);
    xd_gtk_app_follow_theme();

    GError *err = nullptr;
    PolkitSubject *subject =
        process > 0 ? polkit_unix_process_new_for_owner((gint)process, 0, (gint)getuid())
                    : polkit_unix_session_new_for_process_sync((gint)getpid(), nullptr, &err);
    if (subject == nullptr) {
        fprintf(stderr, "xd-polkit: not in a login session: %s\n",
                err != nullptr ? err->message : "unknown");
        g_clear_error(&err);
        return EXIT_FAILURE;
    }
    PolkitAgentListener *listener = g_object_new(XD_TYPE_LISTENER, nullptr);
    gpointer handle = polkit_agent_listener_register(listener, POLKIT_AGENT_REGISTER_FLAGS_NONE,
                                                     subject, nullptr, nullptr, &err);
    g_object_unref(subject);
    if (handle == nullptr) {
        /* Typically another desktop's agent already serves this session
         * (a nested development session). */
        fprintf(stderr, "xd-polkit: cannot register: %s\n", err != nullptr ? err->message : "");
        g_clear_error(&err);
        g_object_unref(listener);
        return EXIT_FAILURE;
    }
    fprintf(stderr, "xd-polkit: registered for %s\n",
            process > 0 ? "one process" : "this session");
    gtk_main();
    polkit_agent_listener_unregister(handle);
    g_object_unref(listener);
    return EXIT_SUCCESS;
}
