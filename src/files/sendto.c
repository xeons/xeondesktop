/* SPDX-License-Identifier: MIT
 * Send To. See sendto.h.
 */
#include "sendto.h"

#if XD_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include "../gtkapp/gtkapp.h"
#include "names.h"
#include "places.h"

/* What the menu's commands act on, kept with the menu. */
typedef struct send_ctx {
    GtkWindow *parent; /* weak */
    GList *files;
    XdFileOpDone done;
    gpointer user;
} send_ctx;

static void ctx_free(gpointer p) {
    send_ctx *c = p;
    if (c->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(c->parent), (gpointer *)&c->parent);
    g_list_free_full(c->files, g_object_unref);
    g_free(c);
}

static send_ctx *ctx_of(GtkWidget *item) {
    return g_object_get_data(G_OBJECT(gtk_widget_get_parent(item)), "xd-send-to");
}

/* ---- Compressed (zipped) Folder ------------------------------------------------- */

#if XD_HAVE_LIBARCHIVE

typedef struct zip_job {
    GPtrArray *sources; /* GFile* */
    GFile *zip;
    char *error;
    char *warning; /* the first thing left out, the zip made anyway */
    GtkWindow *parent; /* weak */
    gboolean had_parent;
    XdFileOpDone done;
    gpointer user;
    GApplication *app; /* held while it runs */
} zip_job;

static void zip_job_free(zip_job *z) {
    if (z->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(z->parent), (gpointer *)&z->parent);
    if (z->app != nullptr) {
        g_application_release(z->app);
        g_object_unref(z->app);
    }
    g_ptr_array_unref(z->sources);
    g_object_unref(z->zip);
    g_free(z->error);
    g_free(z->warning);
    g_free(z);
}

static char *archive_problem(struct archive *a) {
    const char *msg = archive_error_string(a);
    return g_strdup(msg != nullptr ? msg : "The file could not be written.");
}

/* Adds a file or folder (everything in it) to the zip, named from the
 * folder it's in down, as XP's zipped folders are. */
static gboolean add_tree(struct archive *out, const char *path, char **error, char **warning) {
    char *dir = g_path_get_dirname(path);
    size_t strip = strcmp(dir, "/") == 0 ? 1 : strlen(dir) + 1;
    g_free(dir);
    struct archive *disk = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(disk);
    archive_read_disk_set_symlink_physical(disk); /* links stay links */
    gboolean ok = archive_read_disk_open(disk, path) == ARCHIVE_OK;
    if (!ok)
        *error = archive_problem(disk);
    enum { BUF_SIZE = 64 * 1024 };
    char *buf = g_malloc(BUF_SIZE);
    while (ok) {
        struct archive_entry *entry = archive_entry_new();
        int r = archive_read_next_header2(disk, entry);
        if (r == ARCHIVE_EOF) {
            archive_entry_free(entry);
            break;
        }
        if (r < ARCHIVE_WARN) {
            *error = archive_problem(disk);
            archive_entry_free(entry);
            ok = FALSE;
            break;
        }
        /* Something here couldn't be read (a folder's listing, say): the
         * entry still goes in, and the user hears what's missing. */
        if (r == ARCHIVE_WARN && *warning == nullptr)
            *warning = archive_problem(disk);
        if (archive_read_disk_descend(disk) < ARCHIVE_WARN && *warning == nullptr)
            *warning = archive_problem(disk);
        const char *name = archive_entry_pathname(entry);
        if (name != nullptr && strlen(name) > strip) {
            char *rel = g_strdup(name + strip);
            archive_entry_copy_pathname(entry, rel);
            g_free(rel);
        }
        if (archive_write_header(out, entry) < ARCHIVE_WARN) {
            *error = archive_problem(out);
            ok = FALSE;
        } else if (archive_entry_filetype(entry) == AE_IFREG && archive_entry_size(entry) > 0) {
            la_ssize_t n;
            while (ok && (n = archive_read_data(disk, buf, BUF_SIZE)) > 0)
                if (archive_write_data(out, buf, (size_t)n) < 0) {
                    *error = archive_problem(out);
                    ok = FALSE;
                }
            if (ok && n < 0) {
                *error = archive_problem(disk);
                ok = FALSE;
            }
        }
        archive_entry_free(entry);
    }
    g_free(buf);
    archive_read_close(disk);
    archive_read_free(disk);
    return ok;
}

static void zip_worker(GTask *task, [[maybe_unused]] gpointer source, gpointer data,
                       [[maybe_unused]] GCancellable *cancel) {
    zip_job *z = data;
    char *zip_path = g_file_get_path(z->zip);
    /* Made here, never opened over: the name was free when it was chosen,
     * but something else may have taken it since. */
    int fd = open(zip_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0) {
        z->error = g_strdup(g_strerror(errno));
        g_free(zip_path);
        g_task_return_boolean(task, FALSE);
        return;
    }
    struct archive *out = archive_write_new();
    archive_write_set_format_zip(out);
    gboolean ok = archive_write_open_fd(out, fd) == ARCHIVE_OK;
    if (!ok)
        z->error = archive_problem(out);
    for (guint i = 0; ok && i < z->sources->len; i++) {
        char *path = g_file_get_path(g_ptr_array_index(z->sources, i));
        ok = path != nullptr && add_tree(out, path, &z->error, &z->warning);
        g_free(path);
    }
    if (archive_write_close(out) != ARCHIVE_OK && ok) {
        z->error = archive_problem(out);
        ok = FALSE;
    }
    archive_write_free(out);
    if (close(fd) != 0 && ok) {
        z->error = g_strdup(g_strerror(errno));
        ok = FALSE;
    }
    if (!ok)
        g_file_delete(z->zip, nullptr, nullptr); /* not half a zip */
    g_free(zip_path);
    g_task_return_boolean(task, ok);
}

static void zip_finished([[maybe_unused]] GObject *source, GAsyncResult *res, gpointer data) {
    zip_job *z = data;
    gboolean ok = g_task_propagate_boolean(G_TASK(res), nullptr);
    gboolean window_gone = z->had_parent && z->parent == nullptr;
    if (!ok && !window_gone) {
        char *msg = g_strdup_printf("Cannot create the compressed (zipped) folder: %s",
                                    z->error != nullptr ? z->error : "an unknown error.");
        xd_message_box(z->parent, "Compressed (zipped) Folders Error", XD_MB_ERROR, XD_MB_OK,
                       FALSE, msg);
        g_free(msg);
    } else if (ok && !window_gone && z->warning != nullptr) {
        char *msg = g_strdup_printf("Some items could not be added to the compressed (zipped) "
                                    "folder: %s",
                                    z->warning);
        xd_message_box(z->parent, "Compressed (zipped) Folders", XD_MB_WARNING, XD_MB_OK, FALSE,
                       msg);
        g_free(msg);
    }
    if (ok && !window_gone && z->done != nullptr) {
        GList *one = g_list_append(nullptr, z->zip);
        z->done(one, z->user);
        g_list_free(one);
    }
    zip_job_free(z);
}

/* "photo.jpg" makes "photo.zip" beside it, as XP names it after the
 * (first) item; "photo (2).zip" when that's taken. */
static void send_to_zip(GtkWidget *item, [[maybe_unused]] gpointer data) {
    send_ctx *c = ctx_of(item);
    if (c == nullptr || c->files == nullptr)
        return;
    GFile *first = c->files->data;
    GFile *dir = g_file_get_parent(first);
    char *base = g_file_get_basename(first);
    if (dir == nullptr || base == nullptr || !g_file_is_native(first)) {
        g_clear_object(&dir);
        g_free(base);
        return;
    }
    gboolean is_dir =
        g_file_query_file_type(first, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr) ==
        G_FILE_TYPE_DIRECTORY;
    base[is_dir ? strlen(base) : xd_name_base_len(base)] = '\0';
    char *name = xd_unique_name(dir, base, ".zip");
    zip_job *z = g_new0(zip_job, 1);
    z->sources = g_ptr_array_new_with_free_func(g_object_unref);
    for (GList *l = c->files; l != nullptr; l = l->next)
        g_ptr_array_add(z->sources, g_object_ref(l->data));
    z->zip = g_file_get_child(dir, name);
    z->parent = c->parent;
    z->had_parent = c->parent != nullptr;
    if (z->parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(z->parent), (gpointer *)&z->parent);
    z->done = c->done;
    z->user = c->user;
    /* Closing the window mustn't stop the process halfway through. */
    z->app = g_application_get_default();
    if (z->app != nullptr) {
        g_object_ref(z->app);
        g_application_hold(z->app);
    }
    GTask *task = g_task_new(nullptr, nullptr, zip_finished, z);
    g_task_set_task_data(task, z, nullptr);
    g_task_run_in_thread(task, zip_worker);
    g_object_unref(task);
    g_free(name);
    g_free(base);
    g_object_unref(dir);
}

#endif

/* ---- Desktop, My Documents, drives ----------------------------------------------- */

static GFile *user_dir(GUserDirectory which, const char *fallback) {
    const char *path = g_get_user_special_dir(which);
    if (path != nullptr)
        return g_file_new_for_path(path);
    char *p = g_build_filename(g_get_home_dir(), fallback, nullptr);
    GFile *f = g_file_new_for_path(p);
    g_free(p);
    return f;
}

static void send_to_desktop(GtkWidget *item, [[maybe_unused]] gpointer data) {
    send_ctx *c = ctx_of(item);
    GFile *desktop = user_dir(G_USER_DIRECTORY_DESKTOP, "Desktop");
    g_file_make_directory_with_parents(desktop, nullptr, nullptr);
    xd_fileop_start(c->parent, XD_FILEOP_LINK, c->files, desktop, c->done, c->user);
    g_object_unref(desktop);
}

/* My Documents and the drives get copies, which Edit > Undo can take
 * back. */
static void send_copy(GtkWidget *item, gpointer data) {
    send_ctx *c = ctx_of(item);
    GFile *dest = data;
    xd_fileop_start_undoable(c->parent, XD_FILEOP_COPY, c->files, dest, c->done, c->user);
}

/* ---- Mail Recipient ---------------------------------------------------------------- */

/* xdg-email attaches files for these mail programs (Thunderbird by its
 * own command line, the others through mailto:'s attach); others would
 * open a message without them, so Mail Recipient isn't offered. */
static gboolean mail_takes_attachments(void) {
    static const char *const known[] = {"thunderbird", "icedove", "evolution", "kmail"};
    GAppInfo *app = g_app_info_get_default_for_uri_scheme("mailto");
    if (app == nullptr)
        return FALSE;
    const char *exe = g_app_info_get_executable(app);
    char *base = exe != nullptr ? g_path_get_basename(exe) : nullptr;
    gboolean ok = FALSE;
    for (size_t i = 0; base != nullptr && i < G_N_ELEMENTS(known) && !ok; i++)
        ok = g_str_has_prefix(base, known[i]);
    g_free(base);
    g_object_unref(app);
    char *helper = ok ? g_find_program_in_path("xdg-email") : nullptr;
    ok = helper != nullptr;
    g_free(helper);
    return ok;
}

/* Files only, as attachments; a folder can't be one. */
static gboolean all_regular_files(GList *files) {
    for (GList *l = files; l != nullptr; l = l->next) {
        char *path = g_file_get_path(l->data);
        gboolean regular = path != nullptr && g_file_test(path, G_FILE_TEST_IS_REGULAR);
        g_free(path);
        if (!regular)
            return FALSE;
    }
    return files != nullptr;
}

/* A new message with the files attached, titled as XP's "Emailing: x". */
static void send_mail(GtkWidget *item, [[maybe_unused]] gpointer data) {
    send_ctx *c = ctx_of(item);
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    GString *subject = g_string_new("Emailing: ");
    g_ptr_array_add(argv, g_strdup("xdg-email"));
    for (GList *l = c->files; l != nullptr; l = l->next) {
        char *base = g_file_get_basename(l->data);
        char *shown = g_filename_display_name(base);
        g_string_append_printf(subject, "%s%s", l == c->files ? "" : ", ", shown);
        g_free(shown);
        g_free(base);
        g_ptr_array_add(argv, g_strdup("--attach"));
        g_ptr_array_add(argv, g_file_get_path(l->data));
    }
    g_ptr_array_add(argv, g_strdup("--subject"));
    g_ptr_array_add(argv, g_string_free(subject, FALSE));
    g_ptr_array_add(argv, nullptr);
    GError *err = nullptr;
    if (!g_spawn_async(nullptr, (char **)argv->pdata, nullptr, G_SPAWN_SEARCH_PATH, nullptr,
                       nullptr, nullptr, &err)) {
        xd_message_box(c->parent, "Mail Recipient", XD_MB_ERROR, XD_MB_OK, FALSE, err->message);
        g_error_free(err);
    }
    g_ptr_array_unref(argv);
}

/* ---- the menu ------------------------------------------------------------------------ */

static GtkWidget *add_item(GtkWidget *menu, const char *label, const char *icon, GCallback cb,
                           gpointer data, GDestroyNotify free_data) {
    GtkWidget *item = gtk_menu_item_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_add(GTK_CONTAINER(box), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU));
    gtk_container_add(GTK_CONTAINER(box), gtk_label_new(label));
    gtk_container_add(GTK_CONTAINER(item), box);
    if (free_data != nullptr)
        g_object_set_data_full(G_OBJECT(item), "xd-send-data", data, free_data);
    g_signal_connect(item, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

GtkWidget *xd_send_to_menu(GtkWindow *parent, GList *files, XdFileOpDone done, gpointer user) {
    GtkWidget *menu = gtk_menu_new();
    send_ctx *c = g_new0(send_ctx, 1);
    c->parent = parent;
    if (parent != nullptr)
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&c->parent);
    for (GList *l = files; l != nullptr; l = l->next)
        c->files = g_list_prepend(c->files, g_object_ref(l->data));
    c->files = g_list_reverse(c->files);
    c->done = done;
    c->user = user;
    g_object_set_data_full(G_OBJECT(menu), "xd-send-to", c, ctx_free);

    gboolean native = TRUE;
    for (GList *l = files; l != nullptr; l = l->next)
        native = native && g_file_is_native(l->data);
#if XD_HAVE_LIBARCHIVE
    GtkWidget *zip = add_item(menu, "Compressed (zipped) Folder", "package-x-generic",
                              G_CALLBACK(send_to_zip), nullptr, nullptr);
    gtk_widget_set_sensitive(zip, native);
#endif
    add_item(menu, "Desktop (create shortcut)", "user-desktop", G_CALLBACK(send_to_desktop),
             nullptr, nullptr);
    if (mail_takes_attachments()) {
        GtkWidget *mail = add_item(menu, "Mail Recipient", "mail-message-new",
                                   G_CALLBACK(send_mail), nullptr, nullptr);
        gtk_widget_set_sensitive(mail, all_regular_files(files));
    }
    add_item(menu, "My Documents", "folder-documents", G_CALLBACK(send_copy),
             user_dir(G_USER_DIRECTORY_DOCUMENTS, "Documents"), g_object_unref);
    /* The removable drives that are mounted, from GIO's volume monitor. */
    GPtrArray *drives = xd_computer_drives();
    for (guint i = 0; i < drives->len; i++) {
        XdDrive *d = g_ptr_array_index(drives, i);
        /* (A locked encrypted drive has nowhere to copy to yet.) */
        if (g_strcmp0(d->kind, "Removable Disk") != 0 || !g_file_is_native(d->root))
            continue;
        GVolume *unmounted = xd_computer_volume(d->root);
        if (unmounted != nullptr) {
            g_object_unref(unmounted);
            continue;
        }
        add_item(menu, d->name, "drive-removable-media", G_CALLBACK(send_copy),
                 g_object_ref(d->root), g_object_unref);
    }
    g_ptr_array_unref(drives);
    gtk_widget_show_all(menu);
    return menu;
}
