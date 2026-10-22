/* SPDX-License-Identifier: MIT
 * Installing and removing, for Add or Remove Programs: ask first (with
 * what else changes, from a PackageKit simulation), then a progress
 * dialog driven by PackageKit's transaction signals or Flatpak's progress.
 * PackageKit and Flatpak's system helper ask polkit, never us.
 */
#include <string.h>

#include "../core/common.h"
#include "../gtkapp/gtkapp.h"
#include "appwiz.h"

/* At most this many packages are named in a question. */
static constexpr guint MAX_NAMED = 12;

typedef enum op_kind {
    OP_REMOVE_PACKAGE,
    OP_INSTALL_PACKAGE,
    OP_INSTALL_FILE,
    OP_REMOVE_FLATPAK,
    OP_INSTALL_FLATPAK,
    OP_INSTALL_FLATPAKREF,
    OP_INSTALL_BUNDLE,
} op_kind;

typedef struct op {
    appwiz *w;
    op_kind kind;
    char *name;             /* what the user picked */
    char *also;             /* the package's other programs */
    char **ids;             /* package ids or files */
    char *installation, *remote, *ref, *file;
    bool untrusted;         /* the user agreed to an unsigned package file */
    xd_appwiz_done done;
    gpointer data;
    GCancellable *cancel;
    /* the progress dialog */
    GtkWidget *dialog, *bar, *status, *cancel_button;
    guint pulse;
} op;

static void op_free(op *operation) {
    if (operation->pulse != 0)
        g_source_remove(operation->pulse);
    if (operation->dialog != nullptr)
        gtk_widget_destroy(operation->dialog);
    g_free(operation->name);
    g_free(operation->also);
    g_strfreev(operation->ids);
    g_free(operation->installation);
    g_free(operation->remote);
    g_free(operation->ref);
    g_free(operation->file);
    g_clear_object(&operation->cancel);
    g_free(operation);
}

static op *op_new(appwiz *state, op_kind kind, const char *name, xd_appwiz_done done,
                  gpointer data) {
    op *operation = g_new0(op, 1);
    operation->w = state;
    operation->kind = kind;
    operation->name = g_strdup(name);
    operation->done = done;
    operation->data = data;
    operation->cancel = g_cancellable_new();
    return operation;
}

static void set_busy(appwiz *state, bool busy) {
    state->busy = busy;
    GdkWindow *win = gtk_widget_get_window(state->window);
    if (win == nullptr)
        return;
    GdkCursor *cursor =
        busy ? gdk_cursor_new_from_name(gdk_window_get_display(win), "progress") : nullptr;
    gdk_window_set_cursor(win, cursor);
    if (cursor != nullptr)
        g_object_unref(cursor);
}

static void op_finish(op *operation, bool ok) {
    appwiz *state = operation->w;
    set_busy(state, false);
    xd_appwiz_done done = operation->done;
    gpointer data = operation->data;
    op_free(operation);
    if (done != nullptr)
        done(state, ok, data);
}

/* ---- the progress dialog --------------------------------------------------- */

static gboolean pulse(gpointer data) {
    op *operation = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(operation->bar));
    return G_SOURCE_CONTINUE;
}

/* A fraction, or below 0 for "don't know": the bar moves back and forth. */
static void set_fraction(op *operation, double fraction) {
    if (operation->bar == nullptr)
        return;
    if (fraction < 0) {
        if (operation->pulse == 0)
            operation->pulse = g_timeout_add(100, pulse, operation);
        return;
    }
    if (operation->pulse != 0) {
        g_source_remove(operation->pulse);
        operation->pulse = 0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(operation->bar), XD_CLAMP(fraction, 0.0, 1.0));
}

static void set_status(op *operation, const char *text) {
    if (operation->status != nullptr && text != nullptr)
        gtk_label_set_text(GTK_LABEL(operation->status), text);
}

static void on_cancel(GtkButton *button, gpointer data) {
    op *operation = data;
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    set_status(operation, "Canceling...");
    g_cancellable_cancel(operation->cancel);
}

static gboolean no_close([[maybe_unused]] GtkWidget *widget, [[maybe_unused]] GdkEvent *gdk_event,
                         [[maybe_unused]] gpointer data) {
    return TRUE;
}

static void show_progress(op *operation, const char *text) {
    GtkWidget *progress_dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(progress_dialog), XD_APPWIZ_TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(progress_dialog), GTK_WINDOW(operation->w->window));
    gtk_window_set_modal(GTK_WINDOW(progress_dialog), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(progress_dialog), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(progress_dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_position(GTK_WINDOW(progress_dialog), GTK_WIN_POS_CENTER_ON_PARENT);
    g_signal_connect(progress_dialog, "delete-event", G_CALLBACK(no_close), nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("system-software-install", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), xd_appwiz_label(text, nullptr), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    operation->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(operation->bar, 340, -1);
    gtk_box_pack_start(GTK_BOX(box), operation->bar, FALSE, FALSE, 0);
    operation->status = xd_appwiz_label("Starting...", nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(operation->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(operation->status), FALSE);
    gtk_box_pack_start(GTK_BOX(box), operation->status, FALSE, FALSE, 0);
    operation->cancel_button = gtk_button_new_with_mnemonic("Cancel");
    gtk_widget_set_size_request(operation->cancel_button, 80, -1);
    gtk_widget_set_halign(operation->cancel_button, GTK_ALIGN_END);
    g_signal_connect(operation->cancel_button, "clicked", G_CALLBACK(on_cancel), operation);
    gtk_box_pack_start(GTK_BOX(box), operation->cancel_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(progress_dialog), box);
    operation->dialog = progress_dialog;
    gtk_widget_show_all(progress_dialog);
    set_fraction(operation, -1);
}

/* ---- PackageKit -------------------------------------------------------------- */

static const char *const status_text[PK_STATUS_ENUM_LAST] = {
    [PK_STATUS_ENUM_WAIT] = "Waiting for other software changes to finish...",
    [PK_STATUS_ENUM_SETUP] = "Starting...",
    [PK_STATUS_ENUM_RUNNING] = "Working...",
    [PK_STATUS_ENUM_QUERY] = "Querying...",
    [PK_STATUS_ENUM_INFO] = "Getting information...",
    [PK_STATUS_ENUM_REMOVE] = "Removing files...",
    [PK_STATUS_ENUM_REFRESH_CACHE] = "Refreshing the software list...",
    [PK_STATUS_ENUM_DOWNLOAD] = "Downloading...",
    [PK_STATUS_ENUM_INSTALL] = "Installing...",
    [PK_STATUS_ENUM_UPDATE] = "Updating...",
    [PK_STATUS_ENUM_CLEANUP] = "Cleaning up...",
    [PK_STATUS_ENUM_DEP_RESOLVE] = "Checking what else is needed...",
    [PK_STATUS_ENUM_SIG_CHECK] = "Checking signatures...",
    [PK_STATUS_ENUM_TEST_COMMIT] = "Testing changes...",
    [PK_STATUS_ENUM_COMMIT] = "Applying changes...",
    [PK_STATUS_ENUM_FINISHED] = "Finished.",
    [PK_STATUS_ENUM_CANCEL] = "Canceling...",
    [PK_STATUS_ENUM_DOWNLOAD_REPOSITORY] = "Downloading the software list...",
    [PK_STATUS_ENUM_DOWNLOAD_PACKAGELIST] = "Downloading the software list...",
    [PK_STATUS_ENUM_LOADING_CACHE] = "Loading the software list...",
    [PK_STATUS_ENUM_WAITING_FOR_LOCK] = "Waiting for the package manager...",
    [PK_STATUS_ENUM_WAITING_FOR_AUTH] = "Waiting for permission...",
    [PK_STATUS_ENUM_COPY_FILES] = "Copying files...",
    [PK_STATUS_ENUM_RUN_HOOK] = "Running setup steps...",
};

static void on_pk_progress(PkProgress *progress, PkProgressType type, gpointer data) {
    op *operation = data;
    if (type == PK_PROGRESS_TYPE_PERCENTAGE) {
        int pct = pk_progress_get_percentage(progress);
        /* 101 (or -1) means PackageKit doesn't know. */
        set_fraction(operation, pct >= 0 && pct <= 100 ? pct / 100.0 : -1.0);
    } else if (type == PK_PROGRESS_TYPE_STATUS) {
        PkStatusEnum status = pk_progress_get_status(progress);
        if ((unsigned)status < XD_ARRAY_LEN(status_text) && status_text[status] != nullptr)
            set_status(operation, status_text[status]);
    } else if (type == PK_PROGRESS_TYPE_PACKAGE) {
        PkPackage *pkg = pk_progress_get_package(progress);
        if (pkg == nullptr)
            return;
        PkInfoEnum info = pk_package_get_info(pkg);
        const char *verb = info == PK_INFO_ENUM_REMOVING      ? "Removing"
                           : info == PK_INFO_ENUM_INSTALLING  ? "Installing"
                           : info == PK_INFO_ENUM_DOWNLOADING ? "Downloading"
                                                              : nullptr;
        if (verb != nullptr) {
            char *text = g_strdup_printf("%s %s...", verb, pk_package_get_name(pkg));
            set_status(operation, text);
            g_free(text);
        }
    } else if (type == PK_PROGRESS_TYPE_ALLOW_CANCEL) {
        if (operation->cancel_button != nullptr && !g_cancellable_is_cancelled(operation->cancel))
            gtk_widget_set_sensitive(operation->cancel_button,
                                     pk_progress_get_allow_cancel(progress));
    }
}

static const char *error_hint(PkErrorEnum code) {
    if (code == PK_ERROR_ENUM_NOT_AUTHORIZED)
        return "You don't have permission to change the programs on this computer.";
    if (code == PK_ERROR_ENUM_CANNOT_REMOVE_SYSTEM_PACKAGE)
        return "This program is part of the operating system and can't be removed.";
    if (code == PK_ERROR_ENUM_DEP_RESOLUTION_FAILED)
        return "Other programs need this one.";
    if (code == PK_ERROR_ENUM_NO_NETWORK)
        return "This computer isn't connected to a network.";
    if (code == PK_ERROR_ENUM_CANNOT_GET_LOCK)
        return "Another program is changing the software on this computer. Try again when it "
               "has finished.";
    return nullptr;
}

static void report(op *operation, const char *what, PkErrorEnum code, const char *message) {
    const char *hint = error_hint(code);
    char *detail = hint != nullptr ? g_strdup_printf("%s\n\n%s", hint, message != nullptr ? message : "")
                                   : g_strdup(message);
    xd_appwiz_error(operation->w, what, detail);
    g_free(detail);
}

static bool was_cancelled(op *operation, PkErrorEnum code, GError *error) {
    return g_cancellable_is_cancelled(operation->cancel) ||
           code == PK_ERROR_ENUM_TRANSACTION_CANCELLED ||
           g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

static const char *failure_text(op_kind kind) {
    switch (kind) {
    case OP_REMOVE_PACKAGE:
    case OP_REMOVE_FLATPAK:
        return "The program could not be removed.";
    case OP_INSTALL_PACKAGE:
    case OP_INSTALL_FILE:
    case OP_INSTALL_FLATPAK:
    case OP_INSTALL_FLATPAKREF:
    case OP_INSTALL_BUNDLE:
        break;
    }
    return "The program could not be installed.";
}

static void run_pk(op *operation, PkBitfield flags, bool simulate);

static void pk_done(op *operation, PkResults *results, GError *error) {
    PkErrorEnum code;
    char *message = nullptr;
    bool failed = xd_appwiz_pk_failed(results, error, &code, &message);
    bool unsigned_file =
        operation->kind == OP_INSTALL_FILE && !operation->untrusted &&
        (code == PK_ERROR_ENUM_MISSING_GPG_SIGNATURE || code == PK_ERROR_ENUM_BAD_GPG_SIGNATURE ||
         (results != nullptr && pk_results_get_exit_code(results) == PK_EXIT_ENUM_NEED_UNTRUSTED));
    if (operation->pulse != 0) {
        g_source_remove(operation->pulse);
        operation->pulse = 0;
    }
    g_clear_pointer(&operation->dialog, gtk_widget_destroy);
    operation->bar = operation->status = operation->cancel_button = nullptr;
    g_clear_error(&error);
    g_clear_object(&results);
    if (unsigned_file) {
        g_free(message);
        char *question_text =
            g_strdup_printf("The package file \"%s\" isn't signed by a publisher this "
                            "computer trusts, so there's no telling who made it or whether "
                            "it has been changed.\n\nInstall it anyway?",
                            operation->name);
        int yes = xd_message_box(GTK_WINDOW(operation->w->window), XD_APPWIZ_TITLE, XD_MB_WARNING,
                                 XD_MB_YES_NO, TRUE, question_text);
        g_free(question_text);
        if (yes != GTK_RESPONSE_YES) {
            op_finish(operation, false);
            return;
        }
        operation->untrusted = true;
        run_pk(operation, 0, false);
        return;
    }
    if (failed && !was_cancelled(operation, code, nullptr))
        report(operation, failure_text(operation->kind), code, message);
    g_free(message);
    op_finish(operation, !failed);
}

/* The packages a simulation says change, other than the ones asked for. */
static GPtrArray *other_packages(op *operation, PkResults *results) {
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *pkgs = pk_results_get_package_array(results);
    for (guint i = 0; i < pkgs->len; i++) {
        PkPackage *package = g_ptr_array_index(pkgs, i);
        const char *id = pk_package_get_id(package);
        PkInfoEnum info = pk_package_get_info(package);
        if (info != PK_INFO_ENUM_REMOVING && info != PK_INFO_ENUM_INSTALLING &&
            info != PK_INFO_ENUM_REMOVE && info != PK_INFO_ENUM_INSTALL)
            continue;
        bool asked = false;
        for (char **asked_id = operation->ids;
             asked_id != nullptr && *asked_id != nullptr && !asked; asked_id++) {
            char *asked_name = nullptr, *package_name = nullptr;
            xd_appwiz_split_package_id(*asked_id, &asked_name, nullptr, nullptr);
            xd_appwiz_split_package_id(id, &package_name, nullptr, nullptr);
            asked = g_strcmp0(asked_name, package_name) == 0;
            g_free(asked_name);
            g_free(package_name);
        }
        const char *name = pk_package_get_name(package);
        if (!asked && !g_ptr_array_find_with_equal_func(names, name, g_str_equal, nullptr))
            g_ptr_array_add(names, g_strdup(name));
    }
    g_ptr_array_unref(pkgs);
    return names;
}

static void append_names(GString *text, GPtrArray *names) {
    for (guint i = 0; i < names->len && i < MAX_NAMED; i++)
        g_string_append_printf(text, "%s%s", i > 0 ? ", " : "",
                               (char *)g_ptr_array_index(names, i));
    if (names->len > MAX_NAMED)
        g_string_append_printf(text, " and %u more", names->len - MAX_NAMED);
    g_string_append(text, ".");
}

/* XP's question, with what else changes. */
static bool confirm(op *operation, GPtrArray *others) {
    GString *question_text = g_string_new(nullptr);
    bool remove = operation->kind == OP_REMOVE_PACKAGE;
    if (remove)
        g_string_append_printf(question_text,
                               "Are you sure you want to remove %s from your computer?",
                               operation->name);
    else
        g_string_append_printf(question_text, "Do you want to install %s?", operation->name);
    if (operation->also != nullptr)
        g_string_append_printf(question_text,
                               "\n\nThese programs come with it and will be removed too: %s.",
                               operation->also);
    if (others != nullptr && others->len > 0) {
        g_string_append(question_text,
                        remove ? "\n\nThese packages need it and will be removed too: "
                               : "\n\nThese packages are needed and will be installed too: ");
        append_names(question_text, others);
    }
    int response = xd_message_box(GTK_WINDOW(operation->w->window), XD_APPWIZ_TITLE, XD_MB_QUESTION,
                                  XD_MB_YES_NO, remove, question_text->str);
    g_string_free(question_text, TRUE);
    return response == GTK_RESPONSE_YES;
}

static void pk_simulated(op *operation, PkResults *results, GError *error) {
    PkErrorEnum code;
    char *message = nullptr;
    GPtrArray *others = nullptr;
    if (xd_appwiz_pk_failed(results, error, &code, &message)) {
        /* A backend that can't simulate just doesn't list the extras. */
        if (code != PK_ERROR_ENUM_NOT_SUPPORTED) {
            if (!was_cancelled(operation, code, error))
                report(operation, failure_text(operation->kind), code, message);
            g_free(message);
            g_clear_error(&error);
            g_clear_object(&results);
            op_finish(operation, false);
            return;
        }
    } else {
        others = other_packages(operation, results);
    }
    g_free(message);
    g_clear_error(&error);
    g_clear_object(&results);
    set_busy(operation->w, false);
    bool yes = confirm(operation, others);
    if (others != nullptr)
        g_ptr_array_free(others, TRUE);
    if (!yes) {
        op_finish(operation, false);
        return;
    }
    set_busy(operation->w, true);
    run_pk(operation, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), false);
}

/* A PackageKit reply, handled from an idle callback: packagekit-glib2
 * finishes its task inside the transaction's Finished signal and returns
 * it again on Destroy if a nested main loop (a question, an error box)
 * lets that signal through first. */
typedef struct reply {
    op *o;
    PkResults *results;
    GError *error;
    bool simulated;
} reply;

static gboolean reply_later(gpointer data) {
    reply *pending_reply = data;
    if (pending_reply->simulated)
        pk_simulated(pending_reply->o, pending_reply->results, pending_reply->error);
    else
        pk_done(pending_reply->o, pending_reply->results, pending_reply->error);
    g_free(pending_reply);
    return G_SOURCE_REMOVE;
}

static void on_reply(GObject *src, GAsyncResult *res, bool simulated, gpointer data) {
    reply *pending_reply = g_new0(reply, 1);
    pending_reply->o = data;
    pending_reply->simulated = simulated;
    pending_reply->results = pk_client_generic_finish(PK_CLIENT(src), res, &pending_reply->error);
    g_idle_add(reply_later, pending_reply);
}

static void on_simulated(GObject *src, GAsyncResult *res, gpointer data) {
    on_reply(src, res, true, data);
}

static void on_pk_done(GObject *src, GAsyncResult *res, gpointer data) {
    on_reply(src, res, false, data);
}

static void run_pk(op *operation, PkBitfield flags, bool simulate) {
    PkClient *pk = operation->w->pk_task;
    GAsyncReadyCallback cb = simulate ? on_simulated : on_pk_done;
    PkProgressCallback progress = simulate ? nullptr : on_pk_progress;
    if (simulate) {
        flags |= pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_SIMULATE);
    } else {
        char *text = g_strdup_printf(operation->kind == OP_REMOVE_PACKAGE
                                         ? "Please wait while %s is removed from your computer."
                                         : "Please wait while %s is installed.",
                                     operation->name);
        show_progress(operation, text);
        g_free(text);
    }
    switch (operation->kind) {
    case OP_REMOVE_PACKAGE:
        /* Removing what needs it too, as the question said; nothing that
         * merely becomes unused. */
        pk_client_remove_packages_async(pk, flags, operation->ids, TRUE, FALSE, operation->cancel,
                                        progress, operation, cb, operation);
        break;
    case OP_INSTALL_PACKAGE:
        pk_client_install_packages_async(pk, flags, operation->ids, operation->cancel, progress,
                                         operation, cb, operation);
        break;
    case OP_INSTALL_FILE:
        pk_client_install_files_async(pk, flags, operation->ids, operation->cancel, progress,
                                      operation, cb, operation);
        break;
    case OP_REMOVE_FLATPAK:
    case OP_INSTALL_FLATPAK:
    case OP_INSTALL_FLATPAKREF:
    case OP_INSTALL_BUNDLE:
        XD_PANIC("not a PackageKit operation");
    }
}

static bool start(appwiz *state) {
    if (state->busy)
        return false;
    set_busy(state, true);
    return true;
}

void xd_appwiz_remove_package(appwiz *state, const char *name, const char *package_id,
                              const char *also, xd_appwiz_done done, gpointer data) {
    if (!start(state))
        return;
    op *operation = op_new(state, OP_REMOVE_PACKAGE, name, done, data);
    operation->also = g_strdup(also);
    const char *ids[] = {package_id, nullptr};
    operation->ids = g_strdupv((char **)(uintptr_t)ids);
    run_pk(operation, 0, true);
}

void xd_appwiz_install_package(appwiz *state, const char *name, const char *package_id,
                               xd_appwiz_done done, gpointer data) {
    if (!start(state))
        return;
    op *operation = op_new(state, OP_INSTALL_PACKAGE, name, done, data);
    const char *ids[] = {package_id, nullptr};
    operation->ids = g_strdupv((char **)(uintptr_t)ids);
    run_pk(operation, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), true);
}

/* ---- Flatpak ----------------------------------------------------------------- */

FlatpakInstallation *xd_appwiz_open_installation(const char *id, GCancellable *cancel,
                                                 GError **error) {
    if (id == nullptr || strcmp(id, "user") == 0)
        return flatpak_installation_new_user(cancel, error);
    return flatpak_installation_new_system_with_id(strcmp(id, "default") == 0 ? nullptr : id,
                                                   cancel, error);
}

GPtrArray *xd_appwiz_installations(GCancellable *cancel) {
    GPtrArray *all = g_ptr_array_new_with_free_func(g_object_unref);
    FlatpakInstallation *user = flatpak_installation_new_user(cancel, nullptr);
    if (user != nullptr)
        g_ptr_array_add(all, user);
    GPtrArray *system = flatpak_get_system_installations(cancel, nullptr);
    for (guint i = 0; system != nullptr && i < system->len; i++)
        g_ptr_array_add(all, g_object_ref(g_ptr_array_index(system, i)));
    if (system != nullptr)
        g_ptr_array_unref(system);
    return all;
}

/* Progress from the transaction's thread, for the main one. */
typedef struct fp_progress {
    op *o;
    char *status;
    int percent; /* -1: estimating */
} fp_progress;

static gboolean fp_show_progress(gpointer data) {
    fp_progress *progress = data;
    set_status(progress->o, progress->status);
    set_fraction(progress->o, progress->percent >= 0 ? progress->percent / 100.0 : -1.0);
    g_free(progress->status);
    g_free(progress);
    return G_SOURCE_REMOVE;
}

static void fp_post(op *operation, const char *status, int percent) {
    fp_progress *progress = g_new0(fp_progress, 1);
    *progress = (fp_progress){operation, g_strdup(status), percent};
    /* Ahead of the task's completion, which frees the operation. */
    g_main_context_invoke_full(nullptr, G_PRIORITY_HIGH, fp_show_progress, progress, nullptr);
}

static void on_fp_changed(FlatpakTransactionProgress *progress, gpointer data) {
    op *operation = data;
    char *status = flatpak_transaction_progress_get_status(progress);
    fp_post(operation, status != nullptr && status[0] != '\0' ? status : nullptr,
            flatpak_transaction_progress_get_is_estimating(progress)
                ? -1
                : flatpak_transaction_progress_get_progress(progress));
    g_free(status);
}

static void on_fp_operation(FlatpakTransaction *transaction, FlatpakTransactionOperation *op_,
                            FlatpakTransactionProgress *progress, gpointer data) {
    (void)transaction;
    op *operation = data;
    FlatpakRef *ref = flatpak_ref_parse(flatpak_transaction_operation_get_ref(op_), nullptr);
    bool uninstall = flatpak_transaction_operation_get_operation_type(op_) ==
                     FLATPAK_TRANSACTION_OPERATION_UNINSTALL;
    char *text = g_strdup_printf("%s %s...", uninstall ? "Removing" : "Installing",
                                 ref != nullptr ? flatpak_ref_get_name(ref)
                                                : flatpak_transaction_operation_get_ref(op_));
    fp_post(operation, text, -1);
    g_free(text);
    g_clear_object(&ref);
    flatpak_transaction_progress_set_update_frequency(progress, 200);
    g_signal_connect(progress, "changed", G_CALLBACK(on_fp_changed), operation);
}

/* A .flatpakref names its remote; the user picked the file, so it may be
 * added. */
static gboolean on_fp_add_remote([[maybe_unused]] FlatpakTransaction *transaction,
                                 [[maybe_unused]] int reason, [[maybe_unused]] const char *from_id,
                                 [[maybe_unused]] const char *name,
                                 [[maybe_unused]] const char *url, [[maybe_unused]] gpointer data) {
    return TRUE;
}

static bool fp_add(op *operation, FlatpakTransaction *transaction, GError **error) {
    switch (operation->kind) {
    case OP_REMOVE_FLATPAK:
        return flatpak_transaction_add_uninstall(transaction, operation->ref, error);
    case OP_INSTALL_FLATPAK:
        return flatpak_transaction_add_install(transaction, operation->remote, operation->ref,
                                               nullptr, error);
    case OP_INSTALL_FLATPAKREF: {
        char *data = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(operation->file, &data, &len, error))
            return false;
        GBytes *bytes = g_bytes_new_take(data, len);
        bool ok = flatpak_transaction_add_install_flatpakref(transaction, bytes, error);
        g_bytes_unref(bytes);
        return ok;
    }
    case OP_INSTALL_BUNDLE: {
        GFile *bundle = g_file_new_for_path(operation->file);
        bool ok = flatpak_transaction_add_install_bundle(transaction, bundle, nullptr, error);
        g_object_unref(bundle);
        return ok;
    }
    case OP_REMOVE_PACKAGE:
    case OP_INSTALL_PACKAGE:
    case OP_INSTALL_FILE:
        break;
    }
    XD_PANIC("not a Flatpak operation");
}

static void fp_thread(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                      GCancellable *cancel) {
    op *operation = data;
    GError *error = nullptr;
    FlatpakInstallation *inst =
        xd_appwiz_open_installation(operation->installation, cancel, &error);
    FlatpakTransaction *transaction =
        inst != nullptr ? flatpak_transaction_new_for_installation(inst, cancel, &error) : nullptr;
    bool ok = transaction != nullptr;
    if (ok) {
        g_signal_connect(transaction, "new-operation", G_CALLBACK(on_fp_operation), operation);
        g_signal_connect(transaction, "add-new-remote", G_CALLBACK(on_fp_add_remote), operation);
        ok = fp_add(operation, transaction, &error) &&
             flatpak_transaction_run(transaction, cancel, &error);
    }
    g_clear_object(&transaction);
    g_clear_object(&inst);
    if (ok)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void on_fp_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    op *operation = data;
    GError *error = nullptr;
    bool ok = g_task_propagate_boolean(G_TASK(res), &error);
    if (!ok && !g_cancellable_is_cancelled(operation->cancel) &&
        !g_error_matches(error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED)) {
        g_clear_pointer(&operation->dialog, gtk_widget_destroy);
        xd_appwiz_error(operation->w, failure_text(operation->kind),
                        error != nullptr ? error->message : nullptr);
    }
    g_clear_error(&error);
    op_finish(operation, ok);
}

static void run_flatpak(op *operation) {
    char *text = g_strdup_printf(operation->kind == OP_REMOVE_FLATPAK
                                     ? "Please wait while %s is removed from your computer."
                                     : "Please wait while %s is installed.",
                                 operation->name);
    show_progress(operation, text);
    g_free(text);
    GTask *task = g_task_new(nullptr, operation->cancel, on_fp_done, operation);
    g_task_set_task_data(task, operation, nullptr);
    g_task_run_in_thread(task, fp_thread);
    g_object_unref(task);
}

void xd_appwiz_remove_flatpak(appwiz *state, const char *name, const char *installation,
                              const char *ref, xd_appwiz_done done, gpointer data) {
    if (!start(state))
        return;
    op *operation = op_new(state, OP_REMOVE_FLATPAK, name, done, data);
    operation->installation = g_strdup(installation);
    operation->ref = g_strdup(ref);
    char *question_text =
        g_strdup_printf("Are you sure you want to remove %s from your computer?", name);
    int response = xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_QUESTION,
                                  XD_MB_YES_NO, TRUE, question_text);
    g_free(question_text);
    if (response != GTK_RESPONSE_YES) {
        op_finish(operation, false);
        return;
    }
    run_flatpak(operation);
}

void xd_appwiz_install_flatpak(appwiz *state, const char *name, const char *installation,
                               const char *remote, const char *ref, xd_appwiz_done done,
                               gpointer data) {
    if (!start(state))
        return;
    op *operation = op_new(state, OP_INSTALL_FLATPAK, name, done, data);
    operation->installation = g_strdup(installation);
    operation->remote = g_strdup(remote);
    operation->ref = g_strdup(ref);
    char *question_text = g_strdup_printf("Do you want to install %s from %s?", name, remote);
    int response = xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_QUESTION,
                                  XD_MB_YES_NO, FALSE, question_text);
    g_free(question_text);
    if (response != GTK_RESPONSE_YES) {
        op_finish(operation, false);
        return;
    }
    run_flatpak(operation);
}

/* ---- package files ------------------------------------------------------------- */

void xd_appwiz_install_file(appwiz *state, const char *path, xd_appwiz_done done, gpointer data) {
    if (!start(state))
        return;
    char *base = g_path_get_basename(path);
    bool ref = g_str_has_suffix(path, ".flatpakref");
    bool bundle = g_str_has_suffix(path, ".flatpak");
    op *operation = op_new(state,
                           ref      ? OP_INSTALL_FLATPAKREF
                           : bundle ? OP_INSTALL_BUNDLE
                                    : OP_INSTALL_FILE,
                           base, done, data);
    g_free(base);
    char *question_text =
        g_strdup_printf("Do you want to install the program in \"%s\"?\n\nOnly install "
                        "programs from publishers you trust.",
                        operation->name);
    int response = xd_message_box(GTK_WINDOW(state->window), XD_APPWIZ_TITLE, XD_MB_QUESTION,
                                  XD_MB_YES_NO, FALSE, question_text);
    g_free(question_text);
    if (response != GTK_RESPONSE_YES) {
        op_finish(operation, false);
        return;
    }
    if (ref || bundle) {
        /* Flatpaks from files go to the user's own installation: no
         * administrator needed. */
        operation->installation = g_strdup("user");
        operation->file = g_strdup(path);
        run_flatpak(operation);
        return;
    }
    const char *files[] = {path, nullptr};
    operation->ids = g_strdupv((char **)(uintptr_t)files);
    run_pk(operation, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), false);
}
