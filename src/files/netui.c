/* SPDX-License-Identifier: MIT
 * Network dialogs. See netui.h.
 */
#include "netui.h"

#include "../cpl/wizard.h"
#include "../gtkapp/gtkapp.h"
#include "netdrives.h"
#include "places.h"

static constexpr GtkDialogFlags MODAL_DIALOG =
    (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT);

static GtkWidget *wrapped_label(const char *text) {
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 56);
    return label;
}

static GtkWidget *mnemonic_label(const char *text, GtkWidget *target) {
    GtkWidget *label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_mnemonic_widget(GTK_LABEL(label), target);
    return label;
}

/* A window's weak pointer, cleared when it goes. */
static void watch_window(GtkWindow **slot, GtkWindow *window) {
    *slot = window;
    if (window != nullptr)
        g_object_add_weak_pointer(G_OBJECT(window), (gpointer *)slot);
}

static void unwatch_window(GtkWindow **slot) {
    if (*slot != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
    *slot = nullptr;
}

/* ---- Connect to server ------------------------------------------------------------- */

/* What the operation's questions need: whose window, which server. */
typedef struct op_context {
    GtkWindow *parent; /* weak */
    char *host;
    char *user; /* the box's user name to start with */
} op_context;

static void op_context_free(gpointer user_data) {
    op_context *context = user_data;
    unwatch_window(&context->parent);
    g_free(context->host);
    g_free(context->user);
    g_free(context);
}

typedef struct asking {
    GMountOperation *op;
    GtkWidget *dialog, *user, *password, *remember;
    GAskPasswordFlags flags;
    gulong aborted_id;
} asking;

static void asking_destroyed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    asking *password_prompt = data;
    g_signal_handler_disconnect(password_prompt->op, password_prompt->aborted_id);
    g_object_unref(password_prompt->op);
    g_free(password_prompt);
}

static void asking_response(GtkDialog *dialog, int response, gpointer data) {
    asking *password_prompt = data;
    if (response == GTK_RESPONSE_OK)
        xd_net_login_reply(
            password_prompt->op, password_prompt->flags,
            password_prompt->user != nullptr ? gtk_entry_get_text(GTK_ENTRY(password_prompt->user))
                                             : "",
            password_prompt->password != nullptr
                ? gtk_entry_get_text(GTK_ENTRY(password_prompt->password))
                : "",
            password_prompt->remember != nullptr &&
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(password_prompt->remember)));
    else
        g_mount_operation_reply(password_prompt->op, G_MOUNT_OPERATION_ABORTED);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* gvfs gave up waiting (the mount was cancelled): the box goes. */
static void asking_aborted([[maybe_unused]] GMountOperation *op, gpointer data) {
    asking *password_prompt = data;
    gtk_widget_destroy(password_prompt->dialog);
}

/* XP's "Connect to server": the server's question, User name (DOMAIN\user
 * where a domain is wanted), Password and Remember my password. */
static void ask_password(GMountOperation *op, const char *message, const char *default_user,
                         const char *default_domain, GAskPasswordFlags flags,
                         [[maybe_unused]] gpointer data) {
    /* Ours answers; GTK's own box mustn't come up too. */
    g_signal_stop_emission_by_name(op, "ask-password");
    if (xd_net_answer_preset(op, flags))
        return;
    op_context *context = g_object_get_data(G_OBJECT(op), "xd-net-context");
    asking *password_prompt = g_new0(asking, 1);
    password_prompt->op = g_object_ref(op);
    password_prompt->flags = flags;

    char *title = g_strdup_printf(
        "Connect to %s", context != nullptr && context->host != nullptr ? context->host : "server");
    password_prompt->dialog = gtk_dialog_new_with_buttons(
        title, context != nullptr ? context->parent : nullptr, MODAL_DIALOG, "OK", GTK_RESPONSE_OK,
        "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    g_free(title);
    gtk_window_set_resizable(GTK_WINDOW(password_prompt->dialog), FALSE);
    gtk_window_set_icon_name(GTK_WINDOW(password_prompt->dialog), "dialog-password");
    gtk_dialog_set_default_response(GTK_DIALOG(password_prompt->dialog), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *keys = gtk_image_new_from_icon_name("dialog-password", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_valign(keys, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), keys, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top),
                       wrapped_label(message != nullptr && message[0] != '\0'
                                         ? message
                                         : "Connecting to the server"),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    int row = 0;
    if (flags & G_ASK_PASSWORD_NEED_USERNAME) {
        password_prompt->user = gtk_entry_new();
        gtk_entry_set_width_chars(GTK_ENTRY(password_prompt->user), 28);
        gtk_entry_set_activates_default(GTK_ENTRY(password_prompt->user), TRUE);
        const char *user =
            context != nullptr && context->user != nullptr ? context->user : default_user;
        char *shown = (flags & G_ASK_PASSWORD_NEED_DOMAIN) && default_domain != nullptr &&
                              default_domain[0] != '\0' && user != nullptr && user[0] != '\0' &&
                              strchr(user, '\\') == nullptr
                          ? g_strdup_printf("%s\\%s", default_domain, user)
                          : g_strdup(user != nullptr ? user : "");
        gtk_entry_set_text(GTK_ENTRY(password_prompt->user), shown);
        g_free(shown);
        gtk_grid_attach(GTK_GRID(grid), mnemonic_label("_User name:", password_prompt->user), 0,
                        row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), password_prompt->user, 1, row++, 1, 1);
    }
    if (flags & G_ASK_PASSWORD_NEED_PASSWORD) {
        password_prompt->password = gtk_entry_new();
        gtk_entry_set_visibility(GTK_ENTRY(password_prompt->password), FALSE);
        gtk_entry_set_input_purpose(GTK_ENTRY(password_prompt->password),
                                    GTK_INPUT_PURPOSE_PASSWORD);
        gtk_entry_set_activates_default(GTK_ENTRY(password_prompt->password), TRUE);
        gtk_grid_attach(GTK_GRID(grid), mnemonic_label("_Password:", password_prompt->password), 0,
                        row, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), password_prompt->password, 1, row++, 1, 1);
    }
    if (flags & G_ASK_PASSWORD_SAVING_SUPPORTED) {
        /* Kept by gvfs in the Secret Service (a keyring), never by us. */
        password_prompt->remember = gtk_check_button_new_with_mnemonic("_Remember my password");
        gtk_grid_attach(GTK_GRID(grid), password_prompt->remember, 1, row++, 1, 1);
    }
    gtk_widget_set_margin_start(grid, 60);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(
        GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(password_prompt->dialog))), box);

    password_prompt->aborted_id =
        g_signal_connect(op, "aborted", G_CALLBACK(asking_aborted), password_prompt);
    g_signal_connect(password_prompt->dialog, "response", G_CALLBACK(asking_response),
                     password_prompt);
    g_signal_connect(password_prompt->dialog, "destroy", G_CALLBACK(asking_destroyed),
                     password_prompt);
    gtk_widget_show_all(password_prompt->dialog);
    /* The password when the user name is known already. */
    GtkWidget *focus = password_prompt->user != nullptr &&
                               gtk_entry_get_text_length(GTK_ENTRY(password_prompt->user)) == 0
                           ? password_prompt->user
                           : password_prompt->password;
    if (focus != nullptr)
        gtk_widget_grab_focus(focus);
}

GMountOperation *xd_net_mount_operation_new(GtkWindow *parent, const char *uri, const char *user,
                                            const char *password) {
    GMountOperation *op = gtk_mount_operation_new(parent);
    op_context *context = g_new0(op_context, 1);
    watch_window(&context->parent, parent);
    context->host = uri != nullptr ? xd_net_host(uri) : nullptr;
    context->user = g_strdup(user);
    g_object_set_data_full(G_OBJECT(op), "xd-net-context", context, op_context_free);
    if (user != nullptr && password != nullptr)
        xd_net_operation_preset(op, user, password);
    g_signal_connect(op, "ask-password", G_CALLBACK(ask_password), nullptr);
    return op;
}

typedef struct mount_job {
    GtkWindow *parent; /* weak */
    gboolean had_parent;
    XdNetMountedFn done;
    gpointer data;
} mount_job;

static void mounted(GObject *src, GAsyncResult *res, gpointer data) {
    mount_job *net_mount = data;
    GError *err = nullptr;
    if (!g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err) &&
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))
        g_clear_error(&err);
    if (net_mount->done != nullptr && (!net_mount->had_parent || net_mount->parent != nullptr))
        net_mount->done(G_FILE(src), err, net_mount->data);
    g_clear_error(&err);
    unwatch_window(&net_mount->parent);
    g_free(net_mount);
}

void xd_net_mount(GtkWindow *parent, GFile *location, const char *user, const char *password,
                  XdNetMountedFn done, gpointer data) {
    mount_job *net_mount = g_new0(mount_job, 1);
    watch_window(&net_mount->parent, parent);
    net_mount->had_parent = parent != nullptr;
    net_mount->done = done;
    net_mount->data = data;
    char *uri = g_file_get_uri(location);
    GMountOperation *op = xd_net_mount_operation_new(parent, uri, user, password);
    g_free(uri);
    g_file_mount_enclosing_volume(location, G_MOUNT_MOUNT_NONE, op, nullptr, mounted, net_mount);
    g_object_unref(op);
}

/* ---- Map Network Drive -------------------------------------------------------------- */

typedef struct map_state {
    xd_wizard *w;
    GtkWindow *parent; /* weak */
    gboolean had_parent;
    GApplication *app;
    XdNetOpenFn open;
    gpointer data;
    GtkWidget *folder, *reconnect;
    char *user, *password; /* Connect As */
    char *uri;             /* being connected */
    gboolean busy;
} map_state;

static void map_state_free(gpointer user_data) {
    map_state *map_wizard = user_data;
    unwatch_window(&map_wizard->parent);
    if (map_wizard->app != nullptr)
        g_application_release(map_wizard->app);
    g_free(map_wizard->user);
    if (map_wizard->password != nullptr)
        memset(map_wizard->password, 0, strlen(map_wizard->password));
    g_free(map_wizard->password);
    g_free(map_wizard->uri);
    g_free(map_wizard);
}

static GtkWindow *map_window(map_state *map_wizard) {
    return GTK_WINDOW(xd_wizard_window(map_wizard->w));
}

static void map_set_busy(map_state *map_wizard, gboolean busy) {
    map_wizard->busy = busy;
    xd_wizard_set_next_sensitive(map_wizard->w, !busy);
    gtk_widget_set_sensitive(map_wizard->folder, !busy);
    GdkWindow *win = gtk_widget_get_window(GTK_WIDGET(map_window(map_wizard)));
    if (win != nullptr) {
        GdkCursor *cursor =
            busy ? gdk_cursor_new_from_name(gdk_window_get_display(win), "progress") : nullptr;
        gdk_window_set_cursor(win, cursor);
        g_clear_object(&cursor);
    }
}

static void map_mounted(GFile *location, const GError *error, gpointer data) {
    map_state *map_wizard = data;
    map_set_busy(map_wizard, FALSE);
    if (error != nullptr) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *msg = g_strdup_printf("The mapped network drive could not be created because "
                                        "the following error has occurred:\n\n%s",
                                        error->message);
            xd_message_box(map_window(map_wizard), "Map Network Drive", XD_MB_ERROR, XD_MB_OK,
                           FALSE, msg);
            g_free(msg);
        }
        return;
    }
    char *config = xd_net_config_path();
    GError *err = nullptr;
    if (!xd_net_drive_add(config, map_wizard->uri, map_wizard->user,
                          gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(map_wizard->reconnect)),
                          &err)) {
        xd_message_box(map_window(map_wizard), "Map Network Drive", XD_MB_WARNING, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
    }
    g_free(config);
    /* XP opens the new drive in a window. */
    if (map_wizard->open != nullptr && (!map_wizard->had_parent || map_wizard->parent != nullptr))
        map_wizard->open(location, map_wizard->data);
    gtk_widget_destroy(GTK_WIDGET(map_window(map_wizard)));
}

static bool map_finish([[maybe_unused]] xd_wizard *wizard_dialog, gpointer data) {
    map_state *map_wizard = data;
    if (map_wizard->busy)
        return false;
    const char *text =
        gtk_entry_get_text(GTK_ENTRY(gtk_bin_get_child(GTK_BIN(map_wizard->folder))));
    GError *err = nullptr;
    char *uri = xd_net_parse_folder(text, TRUE, &err);
    if (uri == nullptr) {
        xd_message_box(map_window(map_wizard), "Map Network Drive", XD_MB_ERROR, XD_MB_OK, FALSE,
                       err->message);
        g_error_free(err);
        return false;
    }
    char *scheme = g_uri_parse_scheme(uri);
    gboolean supported = xd_net_scheme_supported(scheme);
    g_free(scheme);
    if (!supported) {
        char *msg = xd_net_unsupported_text(uri);
        xd_message_box(map_window(map_wizard), "Map Network Drive", XD_MB_ERROR, XD_MB_OK, FALSE,
                       msg);
        g_free(msg);
        g_free(uri);
        return false;
    }
    g_free(map_wizard->uri);
    map_wizard->uri = uri;
    map_set_busy(map_wizard, TRUE);
    GFile *file = g_file_new_for_uri(uri);
    xd_net_mount(map_window(map_wizard), file, map_wizard->user, map_wizard->password, map_mounted,
                 map_wizard);
    g_object_unref(file);
    return false; /* closed once connected */
}

static void on_browse_network([[maybe_unused]] GtkButton *button, gpointer data) {
    GtkWidget *entry = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Browse For Folder", GTK_WINDOW(gtk_widget_get_toplevel(entry)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Cancel", GTK_RESPONSE_CANCEL, "OK",
        GTK_RESPONSE_ACCEPT, nullptr);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(dlg), FALSE);
    if (xd_net_scheme_supported("smb"))
        gtk_file_chooser_set_current_folder_uri(GTK_FILE_CHOOSER(dlg), XD_WINDOWS_NETWORK_URI);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dlg));
        GFile *file = uri != nullptr ? g_file_new_for_uri(uri) : nullptr;
        if (file != nullptr && !g_file_is_native(file)) {
            char *shown = xd_net_display_path(uri);
            gtk_entry_set_text(GTK_ENTRY(entry), shown);
            g_free(shown);
        }
        g_clear_object(&file);
        g_free(uri);
    }
    gtk_widget_destroy(dlg);
}

/* Connect As...: another user's name and password for this drive. */
static gboolean on_connect_as([[maybe_unused]] GtkLinkButton *link, gpointer data) {
    map_state *map_wizard = data;
    GtkWidget *dlg =
        gtk_dialog_new_with_buttons("Connect As...", map_window(map_wizard), MODAL_DIALOG, "OK",
                                    GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    char *who = g_strdup_printf("By default, you will connect to the network folder as %s. To "
                                "connect as another user, enter their user name and password "
                                "below.",
                                g_get_user_name());
    gtk_box_pack_start(GTK_BOX(box), wrapped_label(who), FALSE, FALSE, 0);
    g_free(who);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    GtkWidget *user = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(user), 28);
    gtk_entry_set_text(GTK_ENTRY(user), map_wizard->user != nullptr ? map_wizard->user : "");
    gtk_entry_set_activates_default(GTK_ENTRY(user), TRUE);
    GtkWidget *password = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(password), FALSE);
    gtk_entry_set_input_purpose(GTK_ENTRY(password), GTK_INPUT_PURPOSE_PASSWORD);
    gtk_entry_set_text(GTK_ENTRY(password),
                       map_wizard->password != nullptr ? map_wizard->password : "");
    gtk_entry_set_activates_default(GTK_ENTRY(password), TRUE);
    gtk_grid_attach(GTK_GRID(grid), mnemonic_label("_User name:", user), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), user, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), mnemonic_label("_Password:", password), 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), password, 1, 1, 1, 1);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), box);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        const char *user_name = gtk_entry_get_text(GTK_ENTRY(user));
        g_free(map_wizard->user);
        map_wizard->user = user_name[0] != '\0' ? g_strdup(user_name) : nullptr;
        if (map_wizard->password != nullptr)
            memset(map_wizard->password, 0, strlen(map_wizard->password));
        g_free(map_wizard->password);
        map_wizard->password = map_wizard->user != nullptr
                                   ? g_strdup(gtk_entry_get_text(GTK_ENTRY(password)))
                                   : nullptr;
    }
    gtk_widget_destroy(dlg);
    return TRUE; /* not a web link */
}

/* The folders offered in the list: those mapped and placed before. */
static void fill_known_folders(GtkComboBoxText *combo) {
    char *config = xd_net_config_path();
    GPtrArray *drives = xd_net_drives_load(config);
    GPtrArray *places = xd_net_places_load(config);
    g_free(config);
    for (guint i = 0; i < drives->len + places->len; i++) {
        const char *uri = i < drives->len
                              ? ((XdNetDrive *)g_ptr_array_index(drives, i))->uri
                              : ((XdNetPlace *)g_ptr_array_index(places, i - drives->len))->uri;
        char *shown = xd_net_display_path(uri);
        gtk_combo_box_text_append_text(combo, shown);
        g_free(shown);
    }
    g_ptr_array_unref(drives);
    g_ptr_array_unref(places);
}

void xd_net_map_drive(GtkWindow *parent, GApplication *app, const char *folder, XdNetOpenFn open,
                      gpointer data) {
    map_state *map_wizard = g_new0(map_state, 1);
    watch_window(&map_wizard->parent, parent);
    map_wizard->had_parent = parent != nullptr;
    map_wizard->open = open;
    map_wizard->data = data;
    if (app != nullptr) {
        map_wizard->app = app;
        g_application_hold(app);
    }
    map_wizard->w =
        xd_wizard_new(parent, "Map Network Drive", "folder-remote", map_wizard, map_state_free);
    xd_wizard_set_finish_func(map_wizard->w, map_finish);

    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(body),
                       wrapped_label("This wizard can help you connect to a shared network "
                                     "folder, so that you can open it from My Computer."),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body),
                       wrapped_label("Specify the folder that you want to connect to:"), FALSE,
                       FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    map_wizard->folder = gtk_combo_box_text_new_with_entry();
    GtkWidget *entry = gtk_bin_get_child(GTK_BIN(map_wizard->folder));
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 30);
    fill_known_folders(GTK_COMBO_BOX_TEXT(map_wizard->folder));
    if (folder != nullptr)
        gtk_entry_set_text(GTK_ENTRY(entry), folder);
    gtk_widget_set_hexpand(map_wizard->folder, TRUE);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_network), entry);
    gtk_grid_attach(GTK_GRID(grid), mnemonic_label("F_older:", entry), 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), map_wizard->folder, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), browse, 2, 0, 1, 1);
    GtkWidget *example = gtk_label_new("Example: \\\\server\\share, smb://server/share or "
                                       "nfs://server/path");
    gtk_label_set_xalign(GTK_LABEL(example), 0);
    gtk_label_set_line_wrap(GTK_LABEL(example), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(example), 40);
    gtk_style_context_add_class(gtk_widget_get_style_context(example), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), example, 1, 1, 2, 1);
    map_wizard->reconnect = gtk_check_button_new_with_mnemonic("Reconnect at _logon");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(map_wizard->reconnect), TRUE);
    gtk_grid_attach(GTK_GRID(grid), map_wizard->reconnect, 1, 2, 2, 1);
    GtkWidget *connect_as =
        gtk_link_button_new_with_label("connect-as:", "Connect using a _different user name.");
    gtk_button_set_use_underline(GTK_BUTTON(connect_as), TRUE);
    gtk_widget_set_tooltip_text(connect_as, nullptr);
    gtk_widget_set_halign(connect_as, GTK_ALIGN_START);
    g_signal_connect(connect_as, "activate-link", G_CALLBACK(on_connect_as), map_wizard);
    gtk_grid_attach(GTK_GRID(grid), connect_as, 1, 3, 2, 1);
    gtk_box_pack_start(GTK_BOX(body), grid, FALSE, FALSE, 0);

    xd_wizard_add_exterior(map_wizard->w, nullptr, body, true);
    xd_wizard_show(map_wizard->w);
    gtk_widget_grab_focus(entry);
}

/* ---- disconnecting ------------------------------------------------------------------ */

typedef struct unmount_job {
    GtkWindow *parent; /* weak */
    char *name;
    GApplication *app; /* held until it's done (the desktop's dialog has no window) */
} unmount_job;

static void unmounted(GObject *src, GAsyncResult *res, gpointer data) {
    unmount_job *unmount_request = data;
    GError *err = nullptr;
    if (!g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err)) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            char *msg = g_strdup_printf("%s could not be disconnected.\n\n%s",
                                        unmount_request->name, err->message);
            xd_message_box(unmount_request->parent, "Disconnect Network Drive", XD_MB_ERROR,
                           XD_MB_OK, FALSE, msg);
            g_free(msg);
        }
        g_error_free(err);
    }
    unwatch_window(&unmount_request->parent);
    if (unmount_request->app != nullptr) {
        g_application_release(unmount_request->app);
        g_object_unref(unmount_request->app);
    }
    g_free(unmount_request->name);
    g_free(unmount_request);
}

void xd_net_disconnect(GtkWindow *parent, GFile *item) {
    /* Forgotten whether or not it's connected now, as XP's Disconnect
     * forgets a mapping; also those mapped to folders inside it. */
    char *config = xd_net_config_path();
    char *uri = g_file_get_uri(item);
    GPtrArray *drives = xd_net_drives_load(config);
    for (guint i = 0; i < drives->len; i++) {
        XdNetDrive *net_drive = g_ptr_array_index(drives, i);
        GFile *file = g_file_new_for_uri(net_drive->uri);
        if (xd_net_same_uri(net_drive->uri, uri) || g_file_has_prefix(file, item)) {
            GError *err = nullptr;
            if (!xd_net_drive_remove(config, net_drive->uri, &err)) {
                xd_message_box(parent, "Disconnect Network Drive", XD_MB_ERROR, XD_MB_OK, FALSE,
                               err->message);
                g_error_free(err);
            }
        }
        g_object_unref(file);
    }
    g_ptr_array_unref(drives);
    g_free(config);

    GMount *mount = xd_network_mount(item);
    if (mount != nullptr) {
        unmount_job *unmount_request = g_new0(unmount_job, 1);
        watch_window(&unmount_request->parent, parent);
        unmount_request->name = g_mount_get_name(mount);
        unmount_request->app = g_application_get_default();
        if (unmount_request->app != nullptr) {
            g_object_ref(unmount_request->app);
            g_application_hold(unmount_request->app);
        }
        GMountOperation *op = gtk_mount_operation_new(parent);
        g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, op, nullptr, unmounted,
                                       unmount_request);
        g_object_unref(op);
        g_object_unref(mount);
    }
    g_free(uri);
}

enum { DCOL_ICON, DCOL_NAME, DCOL_PATH, DCOL_URI, DCOL_COUNT };

typedef struct disconnect_state {
    GtkWidget *dialog, *view;
    GApplication *app;
} disconnect_state;

static void disconnect_response(GtkDialog *dialog, int response, gpointer data) {
    disconnect_state *disconnect_dialog = data;
    if (response == GTK_RESPONSE_OK) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(disconnect_dialog->view));
        GtkTreeModel *model = nullptr;
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
        for (GList *list = rows; list != nullptr; list = list->next) {
            GtkTreeIter it;
            char *uri = nullptr;
            if (gtk_tree_model_get_iter(model, &it, list->data))
                gtk_tree_model_get(model, &it, DCOL_URI, &uri, -1);
            if (uri != nullptr) {
                GFile *file = g_file_new_for_uri(uri);
                xd_net_disconnect(parent, file);
                g_object_unref(file);
            }
            g_free(uri);
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void disconnect_destroyed([[maybe_unused]] GtkWidget *widget, gpointer data) {
    disconnect_state *disconnect_dialog = data;
    if (disconnect_dialog->app != nullptr)
        g_application_release(disconnect_dialog->app);
    g_free(disconnect_dialog);
}

/* Double-click or Enter on a drive: OK. */
static void disconnect_row_activated([[maybe_unused]] GtkTreeView *view,
                                     [[maybe_unused]] GtkTreePath *path,
                                     [[maybe_unused]] GtkTreeViewColumn *col, gpointer data) {
    disconnect_state *disconnect_dialog = data;
    gtk_dialog_response(GTK_DIALOG(disconnect_dialog->dialog), GTK_RESPONSE_OK);
}

static void disconnect_selection_changed(GtkTreeSelection *sel, gpointer data) {
    disconnect_state *disconnect_dialog = data;
    gtk_dialog_set_response_sensitive(GTK_DIALOG(disconnect_dialog->dialog), GTK_RESPONSE_OK,
                                      gtk_tree_selection_count_selected_rows(sel) > 0);
}

void xd_net_disconnect_dialog(GtkWindow *parent, GApplication *app) {
    GtkListStore *store = gtk_list_store_new(DCOL_COUNT, G_TYPE_ICON, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    /* The mapped drives, connected or not, and the shares open now. */
    GPtrArray *items = xd_network_items(nullptr);
    for (guint i = 0; i < items->len; i++) {
        XdNetworkItem *network_item = g_ptr_array_index(items, i);
        if (!network_item->connection)
            continue;
        char *uri = g_file_get_uri(network_item->file);
        gtk_list_store_insert_with_values(store, nullptr, -1, DCOL_ICON, network_item->icon,
                                          DCOL_NAME, network_item->name, DCOL_PATH,
                                          network_item->location, DCOL_URI, uri, -1);
        g_free(uri);
    }
    g_ptr_array_unref(items);

    disconnect_state *disconnect_dialog = g_new0(disconnect_state, 1);
    if (app != nullptr) {
        disconnect_dialog->app = app;
        g_application_hold(app);
    }
    disconnect_dialog->dialog =
        gtk_dialog_new_with_buttons("Disconnect Network Drives", parent, MODAL_DIALOG, "OK",
                                    GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL, nullptr);
    gtk_window_set_icon_name(GTK_WINDOW(disconnect_dialog->dialog), "folder-remote");
    gtk_window_set_default_size(GTK_WINDOW(disconnect_dialog->dialog), 440, 300);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_box_pack_start(GTK_BOX(box),
                       wrapped_label(gtk_tree_model_iter_n_children(GTK_TREE_MODEL(store),
                                                                    nullptr) > 0
                                         ? "Select the network drive(s) you want to "
                                           "disconnect, and then click OK."
                                         : "There are no network drives to disconnect."),
                       FALSE, FALSE, 0);
    disconnect_dialog->view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(col, "Network Drive");
    GtkCellRenderer *icon = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon, FALSE);
    gtk_tree_view_column_add_attribute(col, icon, "gicon", DCOL_ICON);
    GtkCellRenderer *text = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, text, TRUE);
    gtk_tree_view_column_add_attribute(col, text, "text", DCOL_NAME);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(disconnect_dialog->view), col);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(disconnect_dialog->view),
        gtk_tree_view_column_new_with_attributes("Network Path", gtk_cell_renderer_text_new(),
                                                 "text", DCOL_PATH, nullptr));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(disconnect_dialog->view));
    gtk_tree_selection_set_mode(sel, GTK_SELECTION_MULTIPLE);
    g_signal_connect(sel, "changed", G_CALLBACK(disconnect_selection_changed), disconnect_dialog);
    g_signal_connect(disconnect_dialog->view, "row-activated", G_CALLBACK(disconnect_row_activated),
                     disconnect_dialog);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), disconnect_dialog->view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(disconnect_dialog->dialog))),
                       box, TRUE, TRUE, 0);
    gtk_dialog_set_default_response(GTK_DIALOG(disconnect_dialog->dialog), GTK_RESPONSE_OK);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(disconnect_dialog->dialog), GTK_RESPONSE_OK,
                                      FALSE);
    g_signal_connect(disconnect_dialog->dialog, "response", G_CALLBACK(disconnect_response),
                     disconnect_dialog);
    g_signal_connect(disconnect_dialog->dialog, "destroy", G_CALLBACK(disconnect_destroyed),
                     disconnect_dialog);
    gtk_widget_show_all(disconnect_dialog->dialog);
    /* The first drive selected, so OK and the arrow keys work at once. */
    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(
            gtk_tree_view_get_model(GTK_TREE_VIEW(disconnect_dialog->view)), &first))
        gtk_tree_selection_select_iter(sel, &first);
    gtk_widget_grab_focus(disconnect_dialog->view);
}

/* ---- Add Network Place --------------------------------------------------------------- */

enum { PAGE_WELCOME, PAGE_ADDRESS, PAGE_NAME, PAGE_DONE };

typedef struct place_state {
    xd_wizard *w;
    GtkWindow *parent; /* weak */
    gboolean had_parent;
    GApplication *app;
    XdNetOpenFn open;
    gpointer data;
    GtkWidget *address, *name, *name_intro, *done_name, *open_check;
    char *uri;
} place_state;

static void place_state_free(gpointer user_data) {
    place_state *place_wizard = user_data;
    unwatch_window(&place_wizard->parent);
    if (place_wizard->app != nullptr)
        g_application_release(place_wizard->app);
    g_free(place_wizard->uri);
    g_free(place_wizard);
}

static GtkWindow *place_window(place_state *place_wizard) {
    return GTK_WINDOW(xd_wizard_window(place_wizard->w));
}

static int place_next([[maybe_unused]] xd_wizard *wizard_dialog, int page, gpointer data) {
    place_state *place_wizard = data;
    if (page == PAGE_ADDRESS) {
        GError *err = nullptr;
        char *uri =
            xd_net_parse_folder(gtk_entry_get_text(GTK_ENTRY(place_wizard->address)), FALSE, &err);
        if (uri == nullptr) {
            xd_message_box(place_window(place_wizard), "Add Network Place", XD_MB_ERROR, XD_MB_OK,
                           FALSE, err->message);
            g_error_free(err);
            return -1;
        }
        char *scheme = g_uri_parse_scheme(uri);
        gboolean supported = xd_net_scheme_supported(scheme);
        g_free(scheme);
        if (!supported) {
            char *msg = xd_net_unsupported_text(uri);
            xd_message_box(place_window(place_wizard), "Add Network Place", XD_MB_ERROR, XD_MB_OK,
                           FALSE, msg);
            g_free(msg);
            g_free(uri);
            return -1;
        }
        /* A new address gets its own suggested name. */
        if (g_strcmp0(uri, place_wizard->uri) != 0) {
            char *name = xd_net_drive_name(uri);
            gtk_entry_set_text(GTK_ENTRY(place_wizard->name), name);
            g_free(name);
        }
        g_free(place_wizard->uri);
        place_wizard->uri = uri;
        char *shown = xd_net_display_path(uri);
        char *intro = g_strdup_printf("Create a name for this shortcut that will help you easily "
                                      "identify this network place:\n%s",
                                      shown);
        gtk_label_set_text(GTK_LABEL(place_wizard->name_intro), intro);
        g_free(intro);
        g_free(shown);
        return PAGE_NAME;
    }
    if (page == PAGE_NAME) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(place_wizard->name));
        char *trimmed = g_strstrip(g_strdup(name));
        gboolean blank = trimmed[0] == '\0';
        g_free(trimmed);
        if (blank || strpbrk(name, "\\/:*?\"<>|") != nullptr) {
            /* The characters Windows keeps out of names. */
            xd_message_box(place_window(place_wizard), "Add Network Place", XD_MB_ERROR, XD_MB_OK,
                           FALSE,
                           "Type a name for this network place. A name can't contain any of "
                           "the following characters:\n        \\ / : * ? \" < > |");
            return -1;
        }
        char *config = xd_net_config_path();
        GError *err = nullptr;
        gboolean ok = xd_net_place_add(config, place_wizard->uri, name, &err);
        g_free(config);
        if (!ok) {
            xd_message_box(place_window(place_wizard), "Add Network Place", XD_MB_ERROR, XD_MB_OK,
                           FALSE, err->message);
            g_error_free(err);
            return -1;
        }
        gtk_label_set_text(GTK_LABEL(place_wizard->done_name), name);
        /* The place is made: going back would make another. */
        return PAGE_DONE;
    }
    return page + 1;
}

static void place_enter(xd_wizard *wizard_dialog, int page, gpointer data) {
    place_state *place_wizard = data;
    if (page == PAGE_ADDRESS)
        gtk_widget_grab_focus(place_wizard->address);
    else if (page == PAGE_NAME)
        gtk_widget_grab_focus(place_wizard->name);
    else if (page == PAGE_DONE)
        xd_wizard_set_back_sensitive(wizard_dialog, false);
}

static bool place_finish([[maybe_unused]] xd_wizard *wizard_dialog, gpointer data) {
    place_state *place_wizard = data;
    if (place_wizard->uri != nullptr && place_wizard->open != nullptr &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(place_wizard->open_check)) &&
        (!place_wizard->had_parent || place_wizard->parent != nullptr)) {
        GFile *file = g_file_new_for_uri(place_wizard->uri);
        place_wizard->open(file, place_wizard->data);
        g_object_unref(file);
    }
    return true;
}

static void on_browse_place([[maybe_unused]] GtkButton *button, gpointer data) {
    on_browse_network(nullptr, data);
}

void xd_net_add_place(GtkWindow *parent, GApplication *app, XdNetOpenFn open, gpointer data) {
    place_state *place_wizard = g_new0(place_state, 1);
    watch_window(&place_wizard->parent, parent);
    place_wizard->had_parent = parent != nullptr;
    place_wizard->open = open;
    place_wizard->data = data;
    if (app != nullptr) {
        place_wizard->app = app;
        g_application_hold(app);
    }
    place_wizard->w = xd_wizard_new(parent, "Add Network Place Wizard", "folder-remote",
                                    place_wizard, place_state_free);
    xd_wizard_set_next_func(place_wizard->w, place_next);
    xd_wizard_set_enter_func(place_wizard->w, place_enter);
    xd_wizard_set_finish_func(place_wizard->w, place_finish);

    GtkWidget *welcome = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_box_pack_start(GTK_BOX(welcome),
                       wrapped_label("This wizard helps you create a shortcut to a shared "
                                     "network folder, a Web site, or an FTP site, so that it "
                                     "appears in My Network Places."),
                       FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(welcome), wrapped_label("To continue, click Next."), FALSE, FALSE,
                     0);
    xd_wizard_add_exterior(place_wizard->w, "Welcome to the Add Network Place Wizard", welcome,
                           false);

    GtkWidget *address = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(address), 6);
    gtk_grid_set_column_spacing(GTK_GRID(address), 8);
    place_wizard->address = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(place_wizard->address), TRUE);
    gtk_widget_set_hexpand(place_wizard->address, TRUE);
    GtkWidget *browse = gtk_button_new_with_mnemonic("B_rowse...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_place), place_wizard->address);
    gtk_grid_attach(GTK_GRID(address),
                    mnemonic_label("_Internet or network address:", place_wizard->address), 0, 0, 2,
                    1);
    gtk_grid_attach(GTK_GRID(address), place_wizard->address, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(address), browse, 1, 1, 1, 1);
    GtkWidget *examples = wrapped_label("Examples:\n"
                                        "    \\\\server\\share (a shared network folder)\n"
                                        "    smb://server/share\n"
                                        "    nfs://server/path\n"
                                        "    ftp://ftp.example.org\n"
                                        "    sftp://user@server/folder\n"
                                        "    davs://server/folder (a Web folder)");
    gtk_widget_set_margin_top(examples, 8);
    gtk_grid_attach(GTK_GRID(address), examples, 0, 2, 2, 1);
    xd_wizard_add_interior(place_wizard->w, "What is the address of this network place?",
                           "Type the address of the Web site, FTP site, or network location "
                           "that this shortcut will open.",
                           address, false);

    GtkWidget *naming = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    place_wizard->name_intro = wrapped_label("");
    gtk_box_pack_start(GTK_BOX(naming), place_wizard->name_intro, FALSE, FALSE, 0);
    place_wizard->name = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(place_wizard->name), TRUE);
    gtk_box_pack_start(GTK_BOX(naming),
                       mnemonic_label("_Type a name for this network place:", place_wizard->name),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(naming), place_wizard->name, FALSE, FALSE, 0);
    xd_wizard_add_interior(place_wizard->w, "What do you want to name this place?", nullptr, naming,
                           false);

    GtkWidget *done = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_box_pack_start(GTK_BOX(done),
                       wrapped_label("You have successfully created this network place:"),
                       FALSE, FALSE, 0);
    place_wizard->done_name = wrapped_label("");
    gtk_widget_set_margin_start(place_wizard->done_name, 16);
    gtk_box_pack_start(GTK_BOX(done), place_wizard->done_name, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(done),
                       wrapped_label("A shortcut to this location will now appear in My "
                                     "Network Places."),
                       FALSE, FALSE, 0);
    place_wizard->open_check =
        gtk_check_button_new_with_mnemonic("_Open this network place when I click Finish");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(place_wizard->open_check), TRUE);
    gtk_box_pack_start(GTK_BOX(done), place_wizard->open_check, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(done), wrapped_label("To close this wizard, click Finish."), FALSE,
                     FALSE, 0);
    xd_wizard_add_exterior(place_wizard->w, "Completing the Add Network Place Wizard", done, true);
    xd_wizard_show(place_wizard->w);
}
