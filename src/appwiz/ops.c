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

static void op_free(op *o) {
    if (o->pulse != 0)
        g_source_remove(o->pulse);
    if (o->dialog != nullptr)
        gtk_widget_destroy(o->dialog);
    g_free(o->name);
    g_free(o->also);
    g_strfreev(o->ids);
    g_free(o->installation);
    g_free(o->remote);
    g_free(o->ref);
    g_free(o->file);
    g_clear_object(&o->cancel);
    g_free(o);
}

static op *op_new(appwiz *w, op_kind kind, const char *name, xd_appwiz_done done,
                  gpointer data) {
    op *o = g_new0(op, 1);
    o->w = w;
    o->kind = kind;
    o->name = g_strdup(name);
    o->done = done;
    o->data = data;
    o->cancel = g_cancellable_new();
    return o;
}

static void set_busy(appwiz *w, bool busy) {
    w->busy = busy;
    GdkWindow *win = gtk_widget_get_window(w->window);
    if (win == nullptr)
        return;
    GdkCursor *c = busy ? gdk_cursor_new_from_name(gdk_window_get_display(win), "progress")
                        : nullptr;
    gdk_window_set_cursor(win, c);
    if (c != nullptr)
        g_object_unref(c);
}

static void op_finish(op *o, bool ok) {
    appwiz *w = o->w;
    set_busy(w, false);
    xd_appwiz_done done = o->done;
    gpointer data = o->data;
    op_free(o);
    if (done != nullptr)
        done(w, ok, data);
}

/* ---- the progress dialog --------------------------------------------------- */

static gboolean pulse(gpointer data) {
    op *o = data;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(o->bar));
    return G_SOURCE_CONTINUE;
}

/* A fraction, or below 0 for "don't know": the bar moves back and forth. */
static void set_fraction(op *o, double fraction) {
    if (o->bar == nullptr)
        return;
    if (fraction < 0) {
        if (o->pulse == 0)
            o->pulse = g_timeout_add(100, pulse, o);
        return;
    }
    if (o->pulse != 0) {
        g_source_remove(o->pulse);
        o->pulse = 0;
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(o->bar), XD_CLAMP(fraction, 0.0, 1.0));
}

static void set_status(op *o, const char *text) {
    if (o->status != nullptr && text != nullptr)
        gtk_label_set_text(GTK_LABEL(o->status), text);
}

static void on_cancel(GtkButton *b, gpointer data) {
    op *o = data;
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    set_status(o, "Canceling...");
    g_cancellable_cancel(o->cancel);
}

static gboolean no_close([[maybe_unused]] GtkWidget *w, [[maybe_unused]] GdkEvent *e,
                         [[maybe_unused]] gpointer data) {
    return TRUE;
}

static void show_progress(op *o, const char *text) {
    GtkWidget *d = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(d), XD_APPWIZ_TITLE);
    gtk_window_set_transient_for(GTK_WINDOW(d), GTK_WINDOW(o->w->window));
    gtk_window_set_modal(GTK_WINDOW(d), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(d), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(d), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
    g_signal_connect(d, "delete-event", G_CALLBACK(no_close), nullptr);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = gtk_image_new_from_icon_name("system-software-install", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), xd_appwiz_label(text, nullptr), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);
    o->bar = gtk_progress_bar_new();
    gtk_widget_set_size_request(o->bar, 340, -1);
    gtk_box_pack_start(GTK_BOX(box), o->bar, FALSE, FALSE, 0);
    o->status = xd_appwiz_label("Starting...", nullptr);
    gtk_label_set_ellipsize(GTK_LABEL(o->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(o->status), FALSE);
    gtk_box_pack_start(GTK_BOX(box), o->status, FALSE, FALSE, 0);
    o->cancel_button = gtk_button_new_with_mnemonic("Cancel");
    gtk_widget_set_size_request(o->cancel_button, 80, -1);
    gtk_widget_set_halign(o->cancel_button, GTK_ALIGN_END);
    g_signal_connect(o->cancel_button, "clicked", G_CALLBACK(on_cancel), o);
    gtk_box_pack_start(GTK_BOX(box), o->cancel_button, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(d), box);
    o->dialog = d;
    gtk_widget_show_all(d);
    set_fraction(o, -1);
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
    op *o = data;
    if (type == PK_PROGRESS_TYPE_PERCENTAGE) {
        int pct = pk_progress_get_percentage(progress);
        /* 101 (or -1) means PackageKit doesn't know. */
        set_fraction(o, pct >= 0 && pct <= 100 ? pct / 100.0 : -1.0);
    } else if (type == PK_PROGRESS_TYPE_STATUS) {
        PkStatusEnum s = pk_progress_get_status(progress);
        if ((unsigned)s < XD_ARRAY_LEN(status_text) && status_text[s] != nullptr)
            set_status(o, status_text[s]);
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
            char *s = g_strdup_printf("%s %s...", verb, pk_package_get_name(pkg));
            set_status(o, s);
            g_free(s);
        }
    } else if (type == PK_PROGRESS_TYPE_ALLOW_CANCEL) {
        if (o->cancel_button != nullptr && !g_cancellable_is_cancelled(o->cancel))
            gtk_widget_set_sensitive(o->cancel_button, pk_progress_get_allow_cancel(progress));
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

static void report(op *o, const char *what, PkErrorEnum code, const char *message) {
    const char *hint = error_hint(code);
    char *detail = hint != nullptr ? g_strdup_printf("%s\n\n%s", hint, message != nullptr ? message : "")
                                   : g_strdup(message);
    xd_appwiz_error(o->w, what, detail);
    g_free(detail);
}

static bool was_cancelled(op *o, PkErrorEnum code, GError *error) {
    return g_cancellable_is_cancelled(o->cancel) || code == PK_ERROR_ENUM_TRANSACTION_CANCELLED ||
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

static void run_pk(op *o, PkBitfield flags, bool simulate);

static void pk_done(op *o, PkResults *r, GError *error) {
    PkErrorEnum code;
    char *message = nullptr;
    bool failed = xd_appwiz_pk_failed(r, error, &code, &message);
    bool unsigned_file = o->kind == OP_INSTALL_FILE && !o->untrusted &&
                         (code == PK_ERROR_ENUM_MISSING_GPG_SIGNATURE ||
                          code == PK_ERROR_ENUM_BAD_GPG_SIGNATURE ||
                          (r != nullptr &&
                           pk_results_get_exit_code(r) == PK_EXIT_ENUM_NEED_UNTRUSTED));
    if (o->pulse != 0) {
        g_source_remove(o->pulse);
        o->pulse = 0;
    }
    g_clear_pointer(&o->dialog, gtk_widget_destroy);
    o->bar = o->status = o->cancel_button = nullptr;
    g_clear_error(&error);
    g_clear_object(&r);
    if (unsigned_file) {
        g_free(message);
        char *q = g_strdup_printf("The package file \"%s\" isn't signed by a publisher this "
                                  "computer trusts, so there's no telling who made it or whether "
                                  "it has been changed.\n\nInstall it anyway?",
                                  o->name);
        int yes = xd_message_box(GTK_WINDOW(o->w->window), XD_APPWIZ_TITLE, XD_MB_WARNING,
                                 XD_MB_YES_NO, TRUE, q);
        g_free(q);
        if (yes != GTK_RESPONSE_YES) {
            op_finish(o, false);
            return;
        }
        o->untrusted = true;
        run_pk(o, 0, false);
        return;
    }
    if (failed && !was_cancelled(o, code, nullptr))
        report(o, failure_text(o->kind), code, message);
    g_free(message);
    op_finish(o, !failed);
}

/* The packages a simulation says change, other than the ones asked for. */
static GPtrArray *other_packages(op *o, PkResults *r) {
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *pkgs = pk_results_get_package_array(r);
    for (guint i = 0; i < pkgs->len; i++) {
        PkPackage *p = g_ptr_array_index(pkgs, i);
        const char *id = pk_package_get_id(p);
        PkInfoEnum info = pk_package_get_info(p);
        if (info != PK_INFO_ENUM_REMOVING && info != PK_INFO_ENUM_INSTALLING &&
            info != PK_INFO_ENUM_REMOVE && info != PK_INFO_ENUM_INSTALL)
            continue;
        bool asked = false;
        for (char **a = o->ids; a != nullptr && *a != nullptr && !asked; a++) {
            char *x = nullptr, *y = nullptr;
            xd_appwiz_split_package_id(*a, &x, nullptr, nullptr);
            xd_appwiz_split_package_id(id, &y, nullptr, nullptr);
            asked = g_strcmp0(x, y) == 0;
            g_free(x);
            g_free(y);
        }
        const char *name = pk_package_get_name(p);
        if (!asked && !g_ptr_array_find_with_equal_func(names, name, g_str_equal, nullptr))
            g_ptr_array_add(names, g_strdup(name));
    }
    g_ptr_array_unref(pkgs);
    return names;
}

static void append_names(GString *s, GPtrArray *names) {
    for (guint i = 0; i < names->len && i < MAX_NAMED; i++)
        g_string_append_printf(s, "%s%s", i > 0 ? ", " : "", (char *)g_ptr_array_index(names, i));
    if (names->len > MAX_NAMED)
        g_string_append_printf(s, " and %u more", names->len - MAX_NAMED);
    g_string_append(s, ".");
}

/* XP's question, with what else changes. */
static bool confirm(op *o, GPtrArray *others) {
    GString *q = g_string_new(nullptr);
    bool remove = o->kind == OP_REMOVE_PACKAGE;
    if (remove)
        g_string_append_printf(q, "Are you sure you want to remove %s from your computer?",
                               o->name);
    else
        g_string_append_printf(q, "Do you want to install %s?", o->name);
    if (o->also != nullptr)
        g_string_append_printf(q, "\n\nThese programs come with it and will be removed too: %s.",
                               o->also);
    if (others != nullptr && others->len > 0) {
        g_string_append(q, remove ? "\n\nThese packages need it and will be removed too: "
                                  : "\n\nThese packages are needed and will be installed too: ");
        append_names(q, others);
    }
    int r = xd_message_box(GTK_WINDOW(o->w->window), XD_APPWIZ_TITLE,
                           XD_MB_QUESTION, XD_MB_YES_NO, remove, q->str);
    g_string_free(q, TRUE);
    return r == GTK_RESPONSE_YES;
}

static void pk_simulated(op *o, PkResults *r, GError *error) {
    PkErrorEnum code;
    char *message = nullptr;
    GPtrArray *others = nullptr;
    if (xd_appwiz_pk_failed(r, error, &code, &message)) {
        /* A backend that can't simulate just doesn't list the extras. */
        if (code != PK_ERROR_ENUM_NOT_SUPPORTED) {
            if (!was_cancelled(o, code, error))
                report(o, failure_text(o->kind), code, message);
            g_free(message);
            g_clear_error(&error);
            g_clear_object(&r);
            op_finish(o, false);
            return;
        }
    } else {
        others = other_packages(o, r);
    }
    g_free(message);
    g_clear_error(&error);
    g_clear_object(&r);
    set_busy(o->w, false);
    bool yes = confirm(o, others);
    if (others != nullptr)
        g_ptr_array_free(others, TRUE);
    if (!yes) {
        op_finish(o, false);
        return;
    }
    set_busy(o->w, true);
    run_pk(o, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), false);
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
    reply *r = data;
    if (r->simulated)
        pk_simulated(r->o, r->results, r->error);
    else
        pk_done(r->o, r->results, r->error);
    g_free(r);
    return G_SOURCE_REMOVE;
}

static void on_reply(GObject *src, GAsyncResult *res, bool simulated, gpointer data) {
    reply *r = g_new0(reply, 1);
    r->o = data;
    r->simulated = simulated;
    r->results = pk_client_generic_finish(PK_CLIENT(src), res, &r->error);
    g_idle_add(reply_later, r);
}

static void on_simulated(GObject *src, GAsyncResult *res, gpointer data) {
    on_reply(src, res, true, data);
}

static void on_pk_done(GObject *src, GAsyncResult *res, gpointer data) {
    on_reply(src, res, false, data);
}

static void run_pk(op *o, PkBitfield flags, bool simulate) {
    PkClient *pk = o->w->pk_task;
    GAsyncReadyCallback cb = simulate ? on_simulated : on_pk_done;
    PkProgressCallback progress = simulate ? nullptr : on_pk_progress;
    if (simulate) {
        flags |= pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_SIMULATE);
    } else {
        char *text = g_strdup_printf(o->kind == OP_REMOVE_PACKAGE
                                         ? "Please wait while %s is removed from your computer."
                                         : "Please wait while %s is installed.",
                                     o->name);
        show_progress(o, text);
        g_free(text);
    }
    switch (o->kind) {
    case OP_REMOVE_PACKAGE:
        /* Removing what needs it too, as the question said; nothing that
         * merely becomes unused. */
        pk_client_remove_packages_async(pk, flags, o->ids, TRUE, FALSE, o->cancel, progress, o,
                                        cb, o);
        break;
    case OP_INSTALL_PACKAGE:
        pk_client_install_packages_async(pk, flags, o->ids, o->cancel, progress, o, cb, o);
        break;
    case OP_INSTALL_FILE:
        pk_client_install_files_async(pk, flags, o->ids, o->cancel, progress, o, cb, o);
        break;
    case OP_REMOVE_FLATPAK:
    case OP_INSTALL_FLATPAK:
    case OP_INSTALL_FLATPAKREF:
    case OP_INSTALL_BUNDLE:
        XD_PANIC("not a PackageKit operation");
    }
}

static bool start(appwiz *w) {
    if (w->busy)
        return false;
    set_busy(w, true);
    return true;
}

void xd_appwiz_remove_package(appwiz *w, const char *name, const char *package_id,
                              const char *also, xd_appwiz_done done, gpointer data) {
    if (!start(w))
        return;
    op *o = op_new(w, OP_REMOVE_PACKAGE, name, done, data);
    o->also = g_strdup(also);
    const char *ids[] = {package_id, nullptr};
    o->ids = g_strdupv((char **)(uintptr_t)ids);
    run_pk(o, 0, true);
}

void xd_appwiz_install_package(appwiz *w, const char *name, const char *package_id,
                               xd_appwiz_done done, gpointer data) {
    if (!start(w))
        return;
    op *o = op_new(w, OP_INSTALL_PACKAGE, name, done, data);
    const char *ids[] = {package_id, nullptr};
    o->ids = g_strdupv((char **)(uintptr_t)ids);
    run_pk(o, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), true);
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
    fp_progress *p = data;
    set_status(p->o, p->status);
    set_fraction(p->o, p->percent >= 0 ? p->percent / 100.0 : -1.0);
    g_free(p->status);
    g_free(p);
    return G_SOURCE_REMOVE;
}

static void fp_post(op *o, const char *status, int percent) {
    fp_progress *p = g_new0(fp_progress, 1);
    *p = (fp_progress){o, g_strdup(status), percent};
    /* Ahead of the task's completion, which frees the operation. */
    g_main_context_invoke_full(nullptr, G_PRIORITY_HIGH, fp_show_progress, p, nullptr);
}

static void on_fp_changed(FlatpakTransactionProgress *progress, gpointer data) {
    op *o = data;
    char *status = flatpak_transaction_progress_get_status(progress);
    fp_post(o, status != nullptr && status[0] != '\0' ? status : nullptr,
            flatpak_transaction_progress_get_is_estimating(progress)
                ? -1
                : flatpak_transaction_progress_get_progress(progress));
    g_free(status);
}

static void on_fp_operation(FlatpakTransaction *t, FlatpakTransactionOperation *op_,
                            FlatpakTransactionProgress *progress, gpointer data) {
    (void)t;
    op *o = data;
    FlatpakRef *ref = flatpak_ref_parse(flatpak_transaction_operation_get_ref(op_), nullptr);
    bool uninstall = flatpak_transaction_operation_get_operation_type(op_) ==
                     FLATPAK_TRANSACTION_OPERATION_UNINSTALL;
    char *s = g_strdup_printf("%s %s...", uninstall ? "Removing" : "Installing",
                              ref != nullptr ? flatpak_ref_get_name(ref)
                                             : flatpak_transaction_operation_get_ref(op_));
    fp_post(o, s, -1);
    g_free(s);
    g_clear_object(&ref);
    flatpak_transaction_progress_set_update_frequency(progress, 200);
    g_signal_connect(progress, "changed", G_CALLBACK(on_fp_changed), o);
}

/* A .flatpakref names its remote; the user picked the file, so it may be
 * added. */
static gboolean on_fp_add_remote([[maybe_unused]] FlatpakTransaction *t,
                                 [[maybe_unused]] int reason,
                                 [[maybe_unused]] const char *from_id,
                                 [[maybe_unused]] const char *name,
                                 [[maybe_unused]] const char *url,
                                 [[maybe_unused]] gpointer data) {
    return TRUE;
}

static bool fp_add(op *o, FlatpakTransaction *t, GError **error) {
    switch (o->kind) {
    case OP_REMOVE_FLATPAK:
        return flatpak_transaction_add_uninstall(t, o->ref, error);
    case OP_INSTALL_FLATPAK:
        return flatpak_transaction_add_install(t, o->remote, o->ref, nullptr, error);
    case OP_INSTALL_FLATPAKREF: {
        char *data = nullptr;
        gsize len = 0;
        if (!g_file_get_contents(o->file, &data, &len, error))
            return false;
        GBytes *bytes = g_bytes_new_take(data, len);
        bool ok = flatpak_transaction_add_install_flatpakref(t, bytes, error);
        g_bytes_unref(bytes);
        return ok;
    }
    case OP_INSTALL_BUNDLE: {
        GFile *f = g_file_new_for_path(o->file);
        bool ok = flatpak_transaction_add_install_bundle(t, f, nullptr, error);
        g_object_unref(f);
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
    op *o = data;
    GError *error = nullptr;
    FlatpakInstallation *inst = xd_appwiz_open_installation(o->installation, cancel, &error);
    FlatpakTransaction *t = inst != nullptr
                                ? flatpak_transaction_new_for_installation(inst, cancel, &error)
                                : nullptr;
    bool ok = t != nullptr;
    if (ok) {
        g_signal_connect(t, "new-operation", G_CALLBACK(on_fp_operation), o);
        g_signal_connect(t, "add-new-remote", G_CALLBACK(on_fp_add_remote), o);
        ok = fp_add(o, t, &error) && flatpak_transaction_run(t, cancel, &error);
    }
    g_clear_object(&t);
    g_clear_object(&inst);
    if (ok)
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, error);
}

static void on_fp_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    op *o = data;
    GError *error = nullptr;
    bool ok = g_task_propagate_boolean(G_TASK(res), &error);
    if (!ok && !g_cancellable_is_cancelled(o->cancel) &&
        !g_error_matches(error, FLATPAK_ERROR, FLATPAK_ERROR_ABORTED)) {
        g_clear_pointer(&o->dialog, gtk_widget_destroy);
        xd_appwiz_error(o->w, failure_text(o->kind), error != nullptr ? error->message : nullptr);
    }
    g_clear_error(&error);
    op_finish(o, ok);
}

static void run_flatpak(op *o) {
    char *text = g_strdup_printf(o->kind == OP_REMOVE_FLATPAK
                                     ? "Please wait while %s is removed from your computer."
                                     : "Please wait while %s is installed.",
                                 o->name);
    show_progress(o, text);
    g_free(text);
    GTask *task = g_task_new(nullptr, o->cancel, on_fp_done, o);
    g_task_set_task_data(task, o, nullptr);
    g_task_run_in_thread(task, fp_thread);
    g_object_unref(task);
}

void xd_appwiz_remove_flatpak(appwiz *w, const char *name, const char *installation,
                              const char *ref, xd_appwiz_done done, gpointer data) {
    if (!start(w))
        return;
    op *o = op_new(w, OP_REMOVE_FLATPAK, name, done, data);
    o->installation = g_strdup(installation);
    o->ref = g_strdup(ref);
    char *q = g_strdup_printf("Are you sure you want to remove %s from your computer?", name);
    int r = xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_QUESTION, XD_MB_YES_NO,
                           TRUE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES) {
        op_finish(o, false);
        return;
    }
    run_flatpak(o);
}

void xd_appwiz_install_flatpak(appwiz *w, const char *name, const char *installation,
                               const char *remote, const char *ref, xd_appwiz_done done,
                               gpointer data) {
    if (!start(w))
        return;
    op *o = op_new(w, OP_INSTALL_FLATPAK, name, done, data);
    o->installation = g_strdup(installation);
    o->remote = g_strdup(remote);
    o->ref = g_strdup(ref);
    char *q = g_strdup_printf("Do you want to install %s from %s?", name, remote);
    int r = xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_QUESTION, XD_MB_YES_NO,
                           FALSE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES) {
        op_finish(o, false);
        return;
    }
    run_flatpak(o);
}

/* ---- package files ------------------------------------------------------------- */

void xd_appwiz_install_file(appwiz *w, const char *path, xd_appwiz_done done, gpointer data) {
    if (!start(w))
        return;
    char *base = g_path_get_basename(path);
    bool ref = g_str_has_suffix(path, ".flatpakref");
    bool bundle = g_str_has_suffix(path, ".flatpak");
    op *o = op_new(w, ref ? OP_INSTALL_FLATPAKREF : bundle ? OP_INSTALL_BUNDLE : OP_INSTALL_FILE,
                   base, done, data);
    g_free(base);
    char *q = g_strdup_printf("Do you want to install the program in \"%s\"?\n\nOnly install "
                              "programs from publishers you trust.",
                              o->name);
    int r = xd_message_box(GTK_WINDOW(w->window), XD_APPWIZ_TITLE, XD_MB_QUESTION, XD_MB_YES_NO,
                           FALSE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES) {
        op_finish(o, false);
        return;
    }
    if (ref || bundle) {
        /* Flatpaks from files go to the user's own installation: no
         * administrator needed. */
        o->installation = g_strdup("user");
        o->file = g_strdup(path);
        run_flatpak(o);
        return;
    }
    const char *files[] = {path, nullptr};
    o->ids = g_strdupv((char **)(uintptr_t)files);
    run_pk(o, pk_bitfield_value(PK_TRANSACTION_FLAG_ENUM_ONLY_TRUSTED), false);
}
