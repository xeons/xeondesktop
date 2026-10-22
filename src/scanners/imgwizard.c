/* SPDX-License-Identifier: MIT
 * XP's Scanner and Camera Wizard. The same five pages for both kinds of
 * device: Welcome; Choose Scanning Preferences (picture type, resolution,
 * Preview) or Choose Pictures to Copy (the camera's thumbnails); Picture
 * Name and Destination; Scanning or Copying; Completing. Scanning and
 * copying block, so they run in a worker thread; a timer shows their
 * progress on the main loop.
 */
#include <glib/gstdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../cpl/wizard.h"
#include "../gtkapp/gtkapp.h"
#include "scanners.h"

enum { PAGE_WELCOME, PAGE_CHOOSE, PAGE_NAME, PAGE_WORK, PAGE_DONE };
enum { P_THUMB, P_NAME, P_FOLDER, P_COUNT };

static constexpr int PREVIEW_W = 170;
static constexpr int PREVIEW_H = 220;

/* A camera shared by the wizard and its jobs: closed with the last. */
typedef struct camref {
    gint refs;
    xd_camera *cam;
} camref;

static camref *camref_ref(camref *camera_ref) {
    if (camera_ref != nullptr)
        g_atomic_int_inc(&camera_ref->refs);
    return camera_ref;
}

static void camref_unref(camref *camera_ref) {
    if (camera_ref != nullptr && g_atomic_int_dec_and_test(&camera_ref->refs)) {
        xd_camera_close(camera_ref->cam);
        g_free(camera_ref);
    }
}

typedef enum job_kind : uint8_t {
    JOB_INFO,    /* the scanner's modes and resolutions */
    JOB_PREVIEW, /* a preview scan */
    JOB_SCAN,    /* the scan, saved */
    JOB_LIST,    /* open the camera, list its pictures */
    JOB_COPY,    /* copy the chosen pictures */
} job_kind;

/* One piece of work for the thread: its inputs, then its results. The
 * wizard and the thread each hold a reference. */
typedef struct job {
    gint refs, progress, cancel;
    job_kind kind;
    char *device, *port, *dir, *name;
    xd_picture_type type;
    int dpi;
    camref *cam;
    GPtrArray *chosen; /* xd_camera_photo*, to copy (no thumbnails) */
    /* results */
    xd_scanner_info *info;
    GdkPixbuf *pixbuf;
    GPtrArray *photos; /* xd_camera_photo* */
    GPtrArray *saved;  /* char*: the files written */
    GError *error;
} job;

static job *job_ref(job *work) {
    g_atomic_int_inc(&work->refs);
    return work;
}

static void job_unref(job *work) {
    if (!g_atomic_int_dec_and_test(&work->refs))
        return;
    g_free(work->device);
    g_free(work->port);
    g_free(work->dir);
    g_free(work->name);
    camref_unref(work->cam);
    if (work->chosen != nullptr)
        g_ptr_array_unref(work->chosen);
    xd_scanner_info_free(work->info);
    g_clear_object(&work->pixbuf);
    if (work->photos != nullptr)
        g_ptr_array_unref(work->photos);
    if (work->saved != nullptr)
        g_ptr_array_unref(work->saved);
    g_clear_error(&work->error);
    g_free(work);
}

typedef struct imgwiz {
    xd_wizard *w;
    xd_imaging_device dev; /* a copy */
    /* Choose Scanning Preferences */
    GtkWidget *types[3], *dpi, *preview, *preview_button, *preview_status;
    xd_scanner_info *info;
    /* Choose Pictures to Copy */
    GtkListStore *photos;
    GtkWidget *photo_view, *photo_status, *select_all, *clear_all;
    bool listed;
    camref *cam;
    /* Picture Name and Destination */
    GtkWidget *name, *folder;
    /* Scanning / Copying */
    GtkWidget *progress, *progress_label;
    /* Completing */
    GtkWidget *done_label;
    char *saved_dir;
    int saved_count;

    job *running;
    guint poll;
    GCancellable *cancel;
} imgwiz;

static GtkWindow *win(imgwiz *imaging_wizard) {
    return GTK_WINDOW(xd_wizard_window(imaging_wizard->w));
}

static GtkWidget *text(const char *label_text) {
    GtkWidget *widget = gtk_label_new(label_text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_yalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 56);
    return widget;
}

/* ---- the worker ---------------------------------------------------------------------- */

/* The pictures folder: XDG's, else ~/Pictures. */
static char *pictures_dir(void) {
    const char *special_dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    return special_dir != nullptr ? g_strdup(special_dir)
                                  : g_build_filename(g_get_home_dir(), "Pictures", nullptr);
}

static bool make_dir(const char *dir, GError **error) {
    if (g_mkdir_with_parents(dir, 0755) == 0)
        return true;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The folder %s could not be created.", dir);
    return false;
}

static void do_scan(job *work) {
    work->pixbuf = xd_scanner_scan(work->device, work->type, work->dpi, work->kind == JOB_PREVIEW,
                                   &work->progress, &work->cancel, &work->error);
    if (work->pixbuf == nullptr || work->kind != JOB_SCAN || !make_dir(work->dir, &work->error))
        return;
    char *file = xd_scan_next_file(work->dir, work->name, "png");
    if (file == nullptr) {
        g_set_error(&work->error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "There are too many pictures called %s in %s.", work->name, work->dir);
        return;
    }
    if (gdk_pixbuf_save(work->pixbuf, file, "png", &work->error, nullptr))
        g_ptr_array_add(work->saved, file);
    else
        g_free(file);
}

static void do_copy(job *work) {
    if (!make_dir(work->dir, &work->error))
        return;
    for (guint i = 0; i < work->chosen->len && !g_atomic_int_get(&work->cancel); i++) {
        const xd_camera_photo *camera_photo = g_ptr_array_index(work->chosen, i);
        /* XP renamed them: "Picture 001.jpg", keeping the kind. */
        const char *dot = strrchr(camera_photo->name, '.');
        char *ext = g_ascii_strdown(dot != nullptr ? dot + 1 : "jpg", -1);
        char *file = xd_scan_next_file(work->dir, work->name, ext);
        g_free(ext);
        if (file == nullptr || !xd_camera_download(work->cam->cam, camera_photo->folder,
                                                   camera_photo->name, file, &work->error)) {
            if (file == nullptr)
                g_set_error(&work->error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "There are too many pictures called %s in %s.", work->name, work->dir);
            g_free(file);
            return;
        }
        g_ptr_array_add(work->saved, file);
        g_atomic_int_set(&work->progress, (int)(1000 * (i + 1) / work->chosen->len));
    }
    if (g_atomic_int_get(&work->cancel) && work->error == nullptr)
        g_set_error_literal(&work->error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled.");
}

static void worker(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                   [[maybe_unused]] GCancellable *cancellable) {
    job *work = data;
    switch (work->kind) {
    case JOB_INFO:
        work->info = xd_scanner_info_get(work->device, &work->error);
        break;
    case JOB_PREVIEW:
    case JOB_SCAN:
        do_scan(work);
        break;
    case JOB_LIST:
        if (work->cam->cam == nullptr)
            work->cam->cam = xd_camera_open(work->device, work->port, &work->error);
        if (work->cam->cam != nullptr)
            work->photos = xd_camera_list_photos(work->cam->cam, &work->cancel, &work->error);
        break;
    case JOB_COPY:
        do_copy(work);
        break;
    default:
        break;
    }
    g_task_return_boolean(task, TRUE);
}

static void job_done(imgwiz *imaging_wizard, job *work);

static void on_job_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    if (!g_task_propagate_boolean(G_TASK(res), nullptr))
        return; /* the wizard is gone */
    imgwiz *imaging_wizard = data;
    job *work = g_task_get_task_data(G_TASK(res));
    if (imaging_wizard->poll != 0) {
        g_source_remove(imaging_wizard->poll);
        imaging_wizard->poll = 0;
    }
    if (imaging_wizard->running == work) {
        imaging_wizard->running = nullptr;
        job_done(imaging_wizard, work);
        job_unref(work);
    }
}

static gboolean on_poll(gpointer data) {
    imgwiz *imaging_wizard = data;
    if (imaging_wizard->running != nullptr && imaging_wizard->progress != nullptr)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(imaging_wizard->progress),
                                      g_atomic_int_get(&imaging_wizard->running->progress) /
                                          1000.0);
    return G_SOURCE_CONTINUE;
}

static job *new_job(imgwiz *imaging_wizard, job_kind kind) {
    job *work = g_new0(job, 1);
    work->refs = 1;
    work->kind = kind;
    work->device = g_strdup(imaging_wizard->dev.id);
    work->port = g_strdup(imaging_wizard->dev.port);
    work->saved = g_ptr_array_new_with_free_func(g_free);
    return work;
}

static void start_job(imgwiz *imaging_wizard, job *work) {
    imaging_wizard->running = work;
    GTask *task = g_task_new(nullptr, imaging_wizard->cancel, on_job_done, imaging_wizard);
    g_task_set_task_data(task, job_ref(work), (GDestroyNotify)job_unref);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    imaging_wizard->poll = g_timeout_add(100, on_poll, imaging_wizard);
}

/* ---- Choose Scanning Preferences ------------------------------------------------------ */

static xd_picture_type chosen_type(imgwiz *imaging_wizard) {
    for (int i = 0; i < 3; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(imaging_wizard->types[i])))
            return (xd_picture_type)i;
    }
    return XD_PICTURE_COLOR;
}

static int chosen_dpi(imgwiz *imaging_wizard) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(imaging_wizard->dpi));
    return id != nullptr ? atoi(id) : 150;
}

static void busy(imgwiz *imaging_wizard, bool is_busy) {
    xd_wizard_set_next_sensitive(imaging_wizard->w, !is_busy);
    xd_wizard_set_back_sensitive(imaging_wizard->w, !is_busy);
    if (imaging_wizard->preview_button != nullptr)
        gtk_widget_set_sensitive(imaging_wizard->preview_button,
                                 !is_busy && imaging_wizard->info != nullptr);
}

static void cb_preview([[maybe_unused]] GtkWidget *widget, gpointer data) {
    imgwiz *imaging_wizard = data;
    if (imaging_wizard->running != nullptr)
        return;
    job *work = new_job(imaging_wizard, JOB_PREVIEW);
    work->type = chosen_type(imaging_wizard);
    gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status), "Scanning a preview...");
    busy(imaging_wizard, true);
    start_job(imaging_wizard, work);
}

static GtkWidget *scan_choose_page(imgwiz *imaging_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(left), text("Picture type:"), FALSE, FALSE, 0);
    static const char *const labels[] = {"_Color picture", "_Grayscale picture",
                                         "_Black and white picture or text"};
    static const char *const icons[] = {"color-management", "image-x-generic",
                                        "x-office-document"};
    for (int i = 0; i < 3; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(row),
                           gtk_image_new_from_icon_name(icons[i], GTK_ICON_SIZE_LARGE_TOOLBAR),
                           FALSE, FALSE, 0);
        imaging_wizard->types[i] = gtk_radio_button_new_with_mnemonic_from_widget(
            i > 0 ? GTK_RADIO_BUTTON(imaging_wizard->types[0]) : nullptr, labels[i]);
        gtk_box_pack_start(GTK_BOX(row), imaging_wizard->types[i], FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(left), row, FALSE, FALSE, 0);
    }
    GtkWidget *dl = gtk_label_new_with_mnemonic("_Resolution (DPI):");
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_widget_set_margin_top(dl, 12);
    imaging_wizard->dpi = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(dl), imaging_wizard->dpi);
    gtk_box_pack_start(GTK_BOX(left), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), imaging_wizard->dpi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left, TRUE, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    imaging_wizard->preview = gtk_image_new();
    gtk_widget_set_size_request(imaging_wizard->preview, PREVIEW_W, PREVIEW_H);
    gtk_container_add(GTK_CONTAINER(frame), imaging_wizard->preview);
    gtk_box_pack_start(GTK_BOX(right), frame, FALSE, FALSE, 0);
    imaging_wizard->preview_status = text("");
    gtk_label_set_max_width_chars(GTK_LABEL(imaging_wizard->preview_status), 24);
    gtk_box_pack_start(GTK_BOX(right), imaging_wizard->preview_status, FALSE, FALSE, 0);
    imaging_wizard->preview_button = gtk_button_new_with_mnemonic("_Preview");
    gtk_widget_set_halign(imaging_wizard->preview_button, GTK_ALIGN_END);
    g_signal_connect(imaging_wizard->preview_button, "clicked", G_CALLBACK(cb_preview),
                     imaging_wizard);
    gtk_box_pack_start(GTK_BOX(right), imaging_wizard->preview_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), right, FALSE, FALSE, 0);
    return box;
}

static void show_info(imgwiz *imaging_wizard) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(imaging_wizard->dpi));
    int best = 0;
    for (guint i = 0; i < imaging_wizard->info->resolutions->len; i++) {
        int dpi = g_array_index(imaging_wizard->info->resolutions, int, i);
        char id[16];
        g_snprintf(id, sizeof(id), "%d", dpi);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(imaging_wizard->dpi), id, id);
        /* 150 dpi, or the nearest: XP's default for pictures. */
        int cur = g_array_index(imaging_wizard->info->resolutions, int, (guint)best);
        if (abs(dpi - 150) < abs(cur - 150))
            best = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(imaging_wizard->dpi), best);
    /* Types the scanner can't do are greyed. */
    for (int i = 0; i < 3; i++) {
        bool one_bit = false;
        bool can = imaging_wizard->info->modes->len == 0 ||
                   xd_scan_mode_for((xd_picture_type)i,
                                    (const char *const *)imaging_wizard->info->modes->pdata,
                                    imaging_wizard->info->modes->len, &one_bit) != nullptr;
        gtk_widget_set_sensitive(imaging_wizard->types[i], can);
    }
}

/* ---- Choose Pictures to Copy ---------------------------------------------------------- */

static void cb_select_all([[maybe_unused]] GtkWidget *widget, gpointer data) {
    imgwiz *imaging_wizard = data;
    gtk_icon_view_select_all(GTK_ICON_VIEW(imaging_wizard->photo_view));
}

static void cb_clear_all([[maybe_unused]] GtkWidget *widget, gpointer data) {
    imgwiz *imaging_wizard = data;
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(imaging_wizard->photo_view));
}

static GtkWidget *camera_choose_page(imgwiz *imaging_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box),
                       text("Pictures with a selection are copied. To leave a picture out, "
                            "click it with Ctrl held down."),
                       FALSE, FALSE, 0);
    imaging_wizard->photos =
        gtk_list_store_new(P_COUNT, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);
    imaging_wizard->photo_view =
        gtk_icon_view_new_with_model(GTK_TREE_MODEL(imaging_wizard->photos));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(imaging_wizard->photo_view),
                                     GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(imaging_wizard->photo_view), P_THUMB);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(imaging_wizard->photo_view), P_NAME);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(imaging_wizard->photo_view), 100);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), imaging_wizard->photo_view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    imaging_wizard->photo_status = text("");
    gtk_box_pack_start(GTK_BOX(row), imaging_wizard->photo_status, TRUE, TRUE, 0);
    imaging_wizard->clear_all = gtk_button_new_with_mnemonic("C_lear All");
    g_signal_connect(imaging_wizard->clear_all, "clicked", G_CALLBACK(cb_clear_all),
                     imaging_wizard);
    gtk_box_pack_end(GTK_BOX(row), imaging_wizard->clear_all, FALSE, FALSE, 0);
    imaging_wizard->select_all = gtk_button_new_with_mnemonic("_Select All");
    g_signal_connect(imaging_wizard->select_all, "clicked", G_CALLBACK(cb_select_all),
                     imaging_wizard);
    gtk_box_pack_end(GTK_BOX(row), imaging_wizard->select_all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

static void show_photos(imgwiz *imaging_wizard, GPtrArray *photos) {
    gtk_list_store_clear(imaging_wizard->photos);
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GdkPixbuf *fallback = gtk_icon_theme_load_icon(theme, "image-x-generic", 64, GTK_ICON_LOOKUP_FORCE_SIZE,
                                                   nullptr);
    for (guint i = 0; i < photos->len; i++) {
        const xd_camera_photo *camera_photo = g_ptr_array_index(photos, i);
        gtk_list_store_insert_with_values(
            imaging_wizard->photos, nullptr, -1, P_THUMB,
            camera_photo->thumbnail != nullptr ? camera_photo->thumbnail : fallback, P_NAME,
            camera_photo->name, P_FOLDER, camera_photo->folder, -1);
    }
    if (fallback != nullptr)
        g_object_unref(fallback);
    gtk_icon_view_select_all(GTK_ICON_VIEW(imaging_wizard->photo_view));
    char *status = g_strdup_printf("%u picture(s) on the camera.", photos->len);
    gtk_label_set_text(GTK_LABEL(imaging_wizard->photo_status),
                       photos->len > 0 ? status : "The camera has no pictures.");
    g_free(status);
}

/* ---- Picture Name and Destination ------------------------------------------------------ */

static GtkWidget *name_page(imgwiz *imaging_wizard) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *l1 = gtk_label_new_with_mnemonic(
        "1. _Type a name for this group of pictures:");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    imaging_wizard->name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(imaging_wizard->name), "Picture");
    gtk_entry_set_activates_default(GTK_ENTRY(imaging_wizard->name), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), imaging_wizard->name);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), imaging_wizard->name, 0, 1, 2, 1);

    GtkWidget *l2 = gtk_label_new("2. File format:");
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_widget_set_margin_top(l2, 8);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 2, 2, 1);
    GtkWidget *fmt = gtk_label_new(imaging_wizard->dev.camera ? "As on the camera (JPEG, RAW, ...)"
                                                              : "PNG (lossless)");
    gtk_label_set_xalign(GTK_LABEL(fmt), 0);
    gtk_widget_set_margin_start(fmt, 16);
    gtk_grid_attach(GTK_GRID(grid), fmt, 0, 3, 2, 1);

    GtkWidget *l3 = gtk_label_new_with_mnemonic("3. _Choose a place to save this group of "
                                                "pictures:");
    gtk_label_set_xalign(GTK_LABEL(l3), 0);
    gtk_widget_set_margin_top(l3, 8);
    imaging_wizard->folder =
        gtk_file_chooser_button_new("Choose a Folder", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    char *dir = pictures_dir();
    g_mkdir_with_parents(dir, 0755);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(imaging_wizard->folder), dir);
    g_free(dir);
    gtk_widget_set_hexpand(imaging_wizard->folder, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l3), imaging_wizard->folder);
    gtk_grid_attach(GTK_GRID(grid), l3, 0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), imaging_wizard->folder, 0, 5, 2, 1);
    GtkWidget *eg = text("Pictures are numbered: Picture 001, Picture 002, ...");
    gtk_style_context_add_class(gtk_widget_get_style_context(eg), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), eg, 0, 6, 2, 1);
    return grid;
}

static char *chosen_dir(imgwiz *imaging_wizard) {
    char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(imaging_wizard->folder));
    return dir != nullptr ? dir : pictures_dir();
}

/* ---- Scanning / Copying and Completing ------------------------------------------------- */

static GtkWidget *work_page(imgwiz *imaging_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    imaging_wizard->progress_label = text("");
    gtk_box_pack_start(GTK_BOX(box), imaging_wizard->progress_label, FALSE, FALSE, 0);
    imaging_wizard->progress = gtk_progress_bar_new();
    gtk_widget_set_margin_top(imaging_wizard->progress, 20);
    gtk_box_pack_start(GTK_BOX(box), imaging_wizard->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text("To stop, click Cancel."), FALSE, FALSE, 0);
    return box;
}

static void cb_open_folder([[maybe_unused]] GtkWidget *widget, gpointer data) {
    imgwiz *imaging_wizard = data;
    if (imaging_wizard->saved_dir == nullptr)
        return;
    char *uri = g_filename_to_uri(imaging_wizard->saved_dir, nullptr, nullptr);
    if (uri != nullptr)
        gtk_show_uri_on_window(win(imaging_wizard), uri, GDK_CURRENT_TIME, nullptr);
    g_free(uri);
}

static GtkWidget *done_page(imgwiz *imaging_wizard) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    imaging_wizard->done_label = text("");
    gtk_box_pack_start(GTK_BOX(box), imaging_wizard->done_label, FALSE, FALSE, 0);
    GtkWidget *open = gtk_button_new_with_mnemonic("_Open the folder");
    gtk_widget_set_halign(open, GTK_ALIGN_START);
    g_signal_connect(open, "clicked", G_CALLBACK(cb_open_folder), imaging_wizard);
    gtk_box_pack_start(GTK_BOX(box), open, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return box;
}

static void error_box(imgwiz *imaging_wizard, const char *msg) {
    xd_message_box(win(imaging_wizard), "Scanner and Camera Wizard", XD_MB_ERROR, XD_MB_OK, FALSE,
                   msg);
}

/* ---- the results ------------------------------------------------------------------------ */

static void job_done(imgwiz *imaging_wizard, job *work) {
    busy(imaging_wizard, false);
    bool cancelled = g_error_matches(work->error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    switch (work->kind) {
    case JOB_INFO:
        if (work->info == nullptr) {
            gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status), work->error->message);
            xd_wizard_set_next_sensitive(imaging_wizard->w, false);
            return;
        }
        imaging_wizard->info = work->info;
        work->info = nullptr;
        show_info(imaging_wizard);
        gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status), "");
        gtk_widget_set_sensitive(imaging_wizard->preview_button, TRUE);
        return;
    case JOB_PREVIEW:
        if (work->pixbuf == nullptr) {
            gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status), work->error->message);
            return;
        }
        {
            int width = gdk_pixbuf_get_width(work->pixbuf),
                height = gdk_pixbuf_get_height(work->pixbuf);
            double scale = MIN((double)PREVIEW_W / width, (double)PREVIEW_H / height);
            GdkPixbuf *small =
                gdk_pixbuf_scale_simple(work->pixbuf, MAX(1, (int)(width * scale)),
                                        MAX(1, (int)(height * scale)), GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(imaging_wizard->preview), small);
            g_object_unref(small);
        }
        gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status), "");
        return;
    case JOB_LIST:
        imaging_wizard->listed = work->photos != nullptr;
        if (work->photos == nullptr) {
            gtk_label_set_text(GTK_LABEL(imaging_wizard->photo_status), work->error->message);
            xd_wizard_set_next_sensitive(imaging_wizard->w, false);
            return;
        }
        show_photos(imaging_wizard, work->photos);
        return;
    case JOB_SCAN:
    case JOB_COPY:
        imaging_wizard->saved_count += (int)work->saved->len;
        if (work->error != nullptr) {
            if (!cancelled)
                error_box(imaging_wizard, work->error->message);
            /* Back to the name and place, to try again. */
            xd_wizard_go(imaging_wizard->w, PAGE_NAME);
            return;
        }
        g_free(imaging_wizard->saved_dir);
        imaging_wizard->saved_dir = g_strdup(work->dir);
        xd_wizard_go(imaging_wizard->w, PAGE_DONE);
        return;
    default:
        return;
    }
}

/* ---- going through it --------------------------------------------------------------------- */

static int next_page([[maybe_unused]] xd_wizard *page_wizard, int page, gpointer data) {
    imgwiz *imaging_wizard = data;
    if (page == PAGE_CHOOSE && imaging_wizard->dev.camera) {
        GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(imaging_wizard->photo_view));
        bool any = sel != nullptr;
        g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
        if (!any) {
            error_box(imaging_wizard, "Select the pictures to copy.");
            return -1;
        }
    }
    if (page == PAGE_NAME) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(imaging_wizard->name));
        if (!xd_scan_name_valid(name)) {
            error_box(imaging_wizard,
                      "Type a name for the pictures. It can't contain a slash (/).");
            return -1;
        }
        char *dir = chosen_dir(imaging_wizard);
        bool ok = g_mkdir_with_parents(dir, 0755) == 0 && g_access(dir, W_OK) == 0;
        if (!ok) {
            char *msg = g_strdup_printf("Pictures can't be saved in %s. Choose another folder.",
                                        dir);
            error_box(imaging_wizard, msg);
            g_free(msg);
        }
        g_free(dir);
        if (!ok)
            return -1;
    }
    return page + 1;
}

static GPtrArray *selected_photos(imgwiz *imaging_wizard) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)xd_camera_photo_free);
    GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(imaging_wizard->photo_view));
    sel = g_list_reverse(sel); /* in the camera's order */
    for (GList *list = sel; list != nullptr; list = list->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(imaging_wizard->photos), &iter, list->data))
            continue;
        xd_camera_photo *camera_photo = g_new0(xd_camera_photo, 1);
        gtk_tree_model_get(GTK_TREE_MODEL(imaging_wizard->photos), &iter, P_NAME,
                           &camera_photo->name, P_FOLDER, &camera_photo->folder, -1);
        g_ptr_array_add(out, camera_photo);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return out;
}

static void enter_page([[maybe_unused]] xd_wizard *page_wizard, int page, gpointer data) {
    imgwiz *imaging_wizard = data;
    if (page == PAGE_CHOOSE && !imaging_wizard->dev.camera && imaging_wizard->info == nullptr &&
        imaging_wizard->running == nullptr) {
        busy(imaging_wizard, true);
        gtk_label_set_text(GTK_LABEL(imaging_wizard->preview_status),
                           "Connecting to the scanner...");
        start_job(imaging_wizard, new_job(imaging_wizard, JOB_INFO));
    } else if (page == PAGE_CHOOSE && imaging_wizard->dev.camera && !imaging_wizard->listed &&
               imaging_wizard->running == nullptr) {
        busy(imaging_wizard, true);
        gtk_label_set_text(GTK_LABEL(imaging_wizard->photo_status),
                           "Reading the camera's pictures...");
        job *work = new_job(imaging_wizard, JOB_LIST);
        work->cam = camref_ref(imaging_wizard->cam);
        start_job(imaging_wizard, work);
    } else if (page == PAGE_WORK) {
        job *work = new_job(imaging_wizard, imaging_wizard->dev.camera ? JOB_COPY : JOB_SCAN);
        work->dir = chosen_dir(imaging_wizard);
        work->name = g_strdup(gtk_entry_get_text(GTK_ENTRY(imaging_wizard->name)));
        if (imaging_wizard->dev.camera) {
            work->cam = camref_ref(imaging_wizard->cam);
            work->chosen = selected_photos(imaging_wizard);
            char *status =
                g_strdup_printf("Copying %u picture(s) to %s...", work->chosen->len, work->dir);
            gtk_label_set_text(GTK_LABEL(imaging_wizard->progress_label), status);
            g_free(status);
        } else {
            work->type = chosen_type(imaging_wizard);
            work->dpi = chosen_dpi(imaging_wizard);
            char *status =
                g_strdup_printf("Scanning a picture at %d dpi into %s...", work->dpi, work->dir);
            gtk_label_set_text(GTK_LABEL(imaging_wizard->progress_label), status);
            g_free(status);
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(imaging_wizard->progress), 0);
        busy(imaging_wizard, true);
        start_job(imaging_wizard, work);
    } else if (page == PAGE_DONE) {
        char *status = g_strdup_printf(
            "You have successfully copied %d picture(s) to:\n\n%s", imaging_wizard->saved_count,
            imaging_wizard->saved_dir != nullptr ? imaging_wizard->saved_dir : "");
        gtk_label_set_text(GTK_LABEL(imaging_wizard->done_label), status);
        g_free(status);
        /* Done is done: the pictures are saved. */
        xd_wizard_set_back_sensitive(imaging_wizard->w, false);
    }
}

static void imgwiz_free(gpointer data) {
    imgwiz *imaging_wizard = data;
    g_cancellable_cancel(imaging_wizard->cancel);
    g_object_unref(imaging_wizard->cancel);
    if (imaging_wizard->poll != 0)
        g_source_remove(imaging_wizard->poll);
    if (imaging_wizard->running != nullptr) {
        /* The thread stops at its next chance and drops its reference. */
        g_atomic_int_set(&imaging_wizard->running->cancel, 1);
        job_unref(imaging_wizard->running);
    }
    camref_unref(imaging_wizard->cam);
    xd_scanner_info_free(imaging_wizard->info);
    /* The view goes with the window; let go of its model first, or GTK
     * complains while tearing down its selection. */
    if (imaging_wizard->photo_view != nullptr)
        gtk_icon_view_set_model(GTK_ICON_VIEW(imaging_wizard->photo_view), nullptr);
    if (imaging_wizard->photos != nullptr)
        g_object_unref(imaging_wizard->photos);
    g_free(imaging_wizard->dev.id);
    g_free(imaging_wizard->dev.port);
    g_free(imaging_wizard->dev.vendor);
    g_free(imaging_wizard->dev.model);
    g_free(imaging_wizard->dev.type);
    g_free(imaging_wizard->saved_dir);
    g_free(imaging_wizard);
}

void xd_imaging_wizard_open(GtkWindow *parent, const xd_imaging_device *dev) {
    imgwiz *imaging_wizard = g_new0(imgwiz, 1);
    imaging_wizard->dev = (xd_imaging_device){
        .camera = dev->camera,
        .id = g_strdup(dev->id),
        .port = g_strdup(dev->port),
        .vendor = g_strdup(dev->vendor),
        .model = g_strdup(dev->model),
        .type = g_strdup(dev->type),
    };
    imaging_wizard->cancel = g_cancellable_new();
    if (dev->camera) {
        imaging_wizard->cam = g_new0(camref, 1);
        imaging_wizard->cam->refs = 1;
    }
    const char *icon = dev->camera ? "camera-photo" : "scanner";
    imaging_wizard->w =
        xd_wizard_new(parent, "Scanner and Camera Wizard", icon, imaging_wizard, imgwiz_free);
    xd_scanners_hold(xd_wizard_window(imaging_wizard->w));
    char *welcome = g_strdup_printf(
        "This wizard helps you copy pictures from %s to your computer.\n\n%s",
        dev->model,
        dev->camera ? "You choose the pictures to copy, and where to put them."
                    : "You choose the kind of picture, look at a preview, and the picture "
                      "is scanned into a folder of your choice.");
    GtkWidget *wbody = text(welcome);
    g_free(welcome);
    xd_wizard_add_exterior(imaging_wizard->w, "Welcome to the Scanner and Camera Wizard", wbody,
                           false);
    if (dev->camera)
        xd_wizard_add_interior(imaging_wizard->w, "Choose Pictures to Copy",
                               "Pictures with a selection will be copied.",
                               camera_choose_page(imaging_wizard), false);
    else
        xd_wizard_add_interior(imaging_wizard->w, "Choose Scanning Preferences",
                               "Choose your scanning preferences, and then click Preview to "
                               "see how your picture will appear.",
                               scan_choose_page(imaging_wizard), false);
    xd_wizard_add_interior(imaging_wizard->w, "Picture Name and Destination",
                           "Select a name and destination for your pictures.",
                           name_page(imaging_wizard), false);
    xd_wizard_add_interior(imaging_wizard->w, dev->camera ? "Copying Pictures" : "Scanning Picture",
                           "Please wait while the wizard copies your pictures.",
                           work_page(imaging_wizard), false);
    xd_wizard_add_exterior(imaging_wizard->w, "Completing the Scanner and Camera Wizard",
                           done_page(imaging_wizard), true);
    xd_wizard_set_next_func(imaging_wizard->w, next_page);
    xd_wizard_set_enter_func(imaging_wizard->w, enter_page);
    xd_wizard_show(imaging_wizard->w);
}
