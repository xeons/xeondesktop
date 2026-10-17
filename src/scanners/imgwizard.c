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

static camref *camref_ref(camref *c) {
    if (c != nullptr)
        g_atomic_int_inc(&c->refs);
    return c;
}

static void camref_unref(camref *c) {
    if (c != nullptr && g_atomic_int_dec_and_test(&c->refs)) {
        xd_camera_close(c->cam);
        g_free(c);
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

static job *job_ref(job *j) {
    g_atomic_int_inc(&j->refs);
    return j;
}

static void job_unref(job *j) {
    if (!g_atomic_int_dec_and_test(&j->refs))
        return;
    g_free(j->device);
    g_free(j->port);
    g_free(j->dir);
    g_free(j->name);
    camref_unref(j->cam);
    if (j->chosen != nullptr)
        g_ptr_array_unref(j->chosen);
    xd_scanner_info_free(j->info);
    g_clear_object(&j->pixbuf);
    if (j->photos != nullptr)
        g_ptr_array_unref(j->photos);
    if (j->saved != nullptr)
        g_ptr_array_unref(j->saved);
    g_clear_error(&j->error);
    g_free(j);
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

static GtkWindow *win(imgwiz *a) { return GTK_WINDOW(xd_wizard_window(a->w)); }

static GtkWidget *text(const char *s) {
    GtkWidget *l = gtk_label_new(s);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_yalign(GTK_LABEL(l), 0);
    gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 56);
    return l;
}

/* ---- the worker ---------------------------------------------------------------------- */

/* The pictures folder: XDG's, else ~/Pictures. */
static char *pictures_dir(void) {
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
    return d != nullptr ? g_strdup(d) : g_build_filename(g_get_home_dir(), "Pictures", nullptr);
}

static bool make_dir(const char *dir, GError **error) {
    if (g_mkdir_with_parents(dir, 0755) == 0)
        return true;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "The folder %s could not be created.", dir);
    return false;
}

static void do_scan(job *j) {
    j->pixbuf = xd_scanner_scan(j->device, j->type, j->dpi, j->kind == JOB_PREVIEW, &j->progress,
                                &j->cancel, &j->error);
    if (j->pixbuf == nullptr || j->kind != JOB_SCAN || !make_dir(j->dir, &j->error))
        return;
    char *file = xd_scan_next_file(j->dir, j->name, "png");
    if (file == nullptr) {
        g_set_error(&j->error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                    "There are too many pictures called %s in %s.", j->name, j->dir);
        return;
    }
    if (gdk_pixbuf_save(j->pixbuf, file, "png", &j->error, nullptr))
        g_ptr_array_add(j->saved, file);
    else
        g_free(file);
}

static void do_copy(job *j) {
    if (!make_dir(j->dir, &j->error))
        return;
    for (guint i = 0; i < j->chosen->len && !g_atomic_int_get(&j->cancel); i++) {
        const xd_camera_photo *p = g_ptr_array_index(j->chosen, i);
        /* XP renamed them: "Picture 001.jpg", keeping the kind. */
        const char *dot = strrchr(p->name, '.');
        char *ext = g_ascii_strdown(dot != nullptr ? dot + 1 : "jpg", -1);
        char *file = xd_scan_next_file(j->dir, j->name, ext);
        g_free(ext);
        if (file == nullptr ||
            !xd_camera_download(j->cam->cam, p->folder, p->name, file, &j->error)) {
            if (file == nullptr)
                g_set_error(&j->error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                            "There are too many pictures called %s in %s.", j->name, j->dir);
            g_free(file);
            return;
        }
        g_ptr_array_add(j->saved, file);
        g_atomic_int_set(&j->progress, (int)(1000 * (i + 1) / j->chosen->len));
    }
    if (g_atomic_int_get(&j->cancel) && j->error == nullptr)
        g_set_error_literal(&j->error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Cancelled.");
}

static void worker(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                   [[maybe_unused]] GCancellable *c) {
    job *j = data;
    switch (j->kind) {
    case JOB_INFO:
        j->info = xd_scanner_info_get(j->device, &j->error);
        break;
    case JOB_PREVIEW:
    case JOB_SCAN:
        do_scan(j);
        break;
    case JOB_LIST:
        if (j->cam->cam == nullptr)
            j->cam->cam = xd_camera_open(j->device, j->port, &j->error);
        if (j->cam->cam != nullptr)
            j->photos = xd_camera_list_photos(j->cam->cam, &j->cancel, &j->error);
        break;
    case JOB_COPY:
        do_copy(j);
        break;
    default:
        break;
    }
    g_task_return_boolean(task, TRUE);
}

static void job_done(imgwiz *a, job *j);

static void on_job_done([[maybe_unused]] GObject *src, GAsyncResult *res, gpointer data) {
    if (!g_task_propagate_boolean(G_TASK(res), nullptr))
        return; /* the wizard is gone */
    imgwiz *a = data;
    job *j = g_task_get_task_data(G_TASK(res));
    if (a->poll != 0) {
        g_source_remove(a->poll);
        a->poll = 0;
    }
    if (a->running == j) {
        a->running = nullptr;
        job_done(a, j);
        job_unref(j);
    }
}

static gboolean on_poll(gpointer data) {
    imgwiz *a = data;
    if (a->running != nullptr && a->progress != nullptr)
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress),
                                      g_atomic_int_get(&a->running->progress) / 1000.0);
    return G_SOURCE_CONTINUE;
}

static job *new_job(imgwiz *a, job_kind kind) {
    job *j = g_new0(job, 1);
    j->refs = 1;
    j->kind = kind;
    j->device = g_strdup(a->dev.id);
    j->port = g_strdup(a->dev.port);
    j->saved = g_ptr_array_new_with_free_func(g_free);
    return j;
}

static void start_job(imgwiz *a, job *j) {
    a->running = j;
    GTask *task = g_task_new(nullptr, a->cancel, on_job_done, a);
    g_task_set_task_data(task, job_ref(j), (GDestroyNotify)job_unref);
    g_task_run_in_thread(task, worker);
    g_object_unref(task);
    a->poll = g_timeout_add(100, on_poll, a);
}

/* ---- Choose Scanning Preferences ------------------------------------------------------ */

static xd_picture_type chosen_type(imgwiz *a) {
    for (int i = 0; i < 3; i++) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(a->types[i])))
            return (xd_picture_type)i;
    }
    return XD_PICTURE_COLOR;
}

static int chosen_dpi(imgwiz *a) {
    const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(a->dpi));
    return id != nullptr ? atoi(id) : 150;
}

static void busy(imgwiz *a, bool b) {
    xd_wizard_set_next_sensitive(a->w, !b);
    xd_wizard_set_back_sensitive(a->w, !b);
    if (a->preview_button != nullptr)
        gtk_widget_set_sensitive(a->preview_button, !b && a->info != nullptr);
}

static void cb_preview([[maybe_unused]] GtkWidget *w, gpointer data) {
    imgwiz *a = data;
    if (a->running != nullptr)
        return;
    job *j = new_job(a, JOB_PREVIEW);
    j->type = chosen_type(a);
    gtk_label_set_text(GTK_LABEL(a->preview_status), "Scanning a preview...");
    busy(a, true);
    start_job(a, j);
}

static GtkWidget *scan_choose_page(imgwiz *a) {
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
        a->types[i] = gtk_radio_button_new_with_mnemonic_from_widget(
            i > 0 ? GTK_RADIO_BUTTON(a->types[0]) : nullptr, labels[i]);
        gtk_box_pack_start(GTK_BOX(row), a->types[i], FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(left), row, FALSE, FALSE, 0);
    }
    GtkWidget *dl = gtk_label_new_with_mnemonic("_Resolution (DPI):");
    gtk_label_set_xalign(GTK_LABEL(dl), 0);
    gtk_widget_set_margin_top(dl, 12);
    a->dpi = gtk_combo_box_text_new();
    gtk_label_set_mnemonic_widget(GTK_LABEL(dl), a->dpi);
    gtk_box_pack_start(GTK_BOX(left), dl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(left), a->dpi, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), left, TRUE, TRUE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *frame = gtk_frame_new(nullptr);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    a->preview = gtk_image_new();
    gtk_widget_set_size_request(a->preview, PREVIEW_W, PREVIEW_H);
    gtk_container_add(GTK_CONTAINER(frame), a->preview);
    gtk_box_pack_start(GTK_BOX(right), frame, FALSE, FALSE, 0);
    a->preview_status = text("");
    gtk_label_set_max_width_chars(GTK_LABEL(a->preview_status), 24);
    gtk_box_pack_start(GTK_BOX(right), a->preview_status, FALSE, FALSE, 0);
    a->preview_button = gtk_button_new_with_mnemonic("_Preview");
    gtk_widget_set_halign(a->preview_button, GTK_ALIGN_END);
    g_signal_connect(a->preview_button, "clicked", G_CALLBACK(cb_preview), a);
    gtk_box_pack_start(GTK_BOX(right), a->preview_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), right, FALSE, FALSE, 0);
    return box;
}

static void show_info(imgwiz *a) {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(a->dpi));
    int best = 0;
    for (guint i = 0; i < a->info->resolutions->len; i++) {
        int dpi = g_array_index(a->info->resolutions, int, i);
        char id[16];
        g_snprintf(id, sizeof(id), "%d", dpi);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(a->dpi), id, id);
        /* 150 dpi, or the nearest: XP's default for pictures. */
        int cur = g_array_index(a->info->resolutions, int, (guint)best);
        if (abs(dpi - 150) < abs(cur - 150))
            best = (int)i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(a->dpi), best);
    /* Types the scanner can't do are greyed. */
    for (int i = 0; i < 3; i++) {
        bool one_bit = false;
        bool can = a->info->modes->len == 0 ||
                   xd_scan_mode_for((xd_picture_type)i,
                                    (const char *const *)a->info->modes->pdata,
                                    a->info->modes->len, &one_bit) != nullptr;
        gtk_widget_set_sensitive(a->types[i], can);
    }
}

/* ---- Choose Pictures to Copy ---------------------------------------------------------- */

static void cb_select_all([[maybe_unused]] GtkWidget *w, gpointer data) {
    imgwiz *a = data;
    gtk_icon_view_select_all(GTK_ICON_VIEW(a->photo_view));
}

static void cb_clear_all([[maybe_unused]] GtkWidget *w, gpointer data) {
    imgwiz *a = data;
    gtk_icon_view_unselect_all(GTK_ICON_VIEW(a->photo_view));
}

static GtkWidget *camera_choose_page(imgwiz *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box),
                       text("Pictures with a selection are copied. To leave a picture out, "
                            "click it with Ctrl held down."),
                       FALSE, FALSE, 0);
    a->photos = gtk_list_store_new(P_COUNT, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_STRING);
    a->photo_view = gtk_icon_view_new_with_model(GTK_TREE_MODEL(a->photos));
    gtk_icon_view_set_selection_mode(GTK_ICON_VIEW(a->photo_view), GTK_SELECTION_MULTIPLE);
    gtk_icon_view_set_pixbuf_column(GTK_ICON_VIEW(a->photo_view), P_THUMB);
    gtk_icon_view_set_text_column(GTK_ICON_VIEW(a->photo_view), P_NAME);
    gtk_icon_view_set_item_width(GTK_ICON_VIEW(a->photo_view), 100);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 190);
    gtk_container_add(GTK_CONTAINER(scroll), a->photo_view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->photo_status = text("");
    gtk_box_pack_start(GTK_BOX(row), a->photo_status, TRUE, TRUE, 0);
    a->clear_all = gtk_button_new_with_mnemonic("C_lear All");
    g_signal_connect(a->clear_all, "clicked", G_CALLBACK(cb_clear_all), a);
    gtk_box_pack_end(GTK_BOX(row), a->clear_all, FALSE, FALSE, 0);
    a->select_all = gtk_button_new_with_mnemonic("_Select All");
    g_signal_connect(a->select_all, "clicked", G_CALLBACK(cb_select_all), a);
    gtk_box_pack_end(GTK_BOX(row), a->select_all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    return box;
}

static void show_photos(imgwiz *a, GPtrArray *photos) {
    gtk_list_store_clear(a->photos);
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GdkPixbuf *fallback = gtk_icon_theme_load_icon(theme, "image-x-generic", 64, GTK_ICON_LOOKUP_FORCE_SIZE,
                                                   nullptr);
    for (guint i = 0; i < photos->len; i++) {
        const xd_camera_photo *p = g_ptr_array_index(photos, i);
        gtk_list_store_insert_with_values(a->photos, nullptr, -1, P_THUMB,
                                          p->thumbnail != nullptr ? p->thumbnail : fallback,
                                          P_NAME, p->name, P_FOLDER, p->folder, -1);
    }
    if (fallback != nullptr)
        g_object_unref(fallback);
    gtk_icon_view_select_all(GTK_ICON_VIEW(a->photo_view));
    char *s = g_strdup_printf("%u picture(s) on the camera.", photos->len);
    gtk_label_set_text(GTK_LABEL(a->photo_status), photos->len > 0 ? s : "The camera has no pictures.");
    g_free(s);
}

/* ---- Picture Name and Destination ------------------------------------------------------ */

static GtkWidget *name_page(imgwiz *a) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    GtkWidget *l1 = gtk_label_new_with_mnemonic(
        "1. _Type a name for this group of pictures:");
    gtk_label_set_xalign(GTK_LABEL(l1), 0);
    a->name = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(a->name), "Picture");
    gtk_entry_set_activates_default(GTK_ENTRY(a->name), TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l1), a->name);
    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), a->name, 0, 1, 2, 1);

    GtkWidget *l2 = gtk_label_new("2. File format:");
    gtk_label_set_xalign(GTK_LABEL(l2), 0);
    gtk_widget_set_margin_top(l2, 8);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 2, 2, 1);
    GtkWidget *fmt = gtk_label_new(a->dev.camera ? "As on the camera (JPEG, RAW, ...)"
                                                 : "PNG (lossless)");
    gtk_label_set_xalign(GTK_LABEL(fmt), 0);
    gtk_widget_set_margin_start(fmt, 16);
    gtk_grid_attach(GTK_GRID(grid), fmt, 0, 3, 2, 1);

    GtkWidget *l3 = gtk_label_new_with_mnemonic("3. _Choose a place to save this group of "
                                                "pictures:");
    gtk_label_set_xalign(GTK_LABEL(l3), 0);
    gtk_widget_set_margin_top(l3, 8);
    a->folder = gtk_file_chooser_button_new("Choose a Folder",
                                            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    char *dir = pictures_dir();
    g_mkdir_with_parents(dir, 0755);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(a->folder), dir);
    g_free(dir);
    gtk_widget_set_hexpand(a->folder, TRUE);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l3), a->folder);
    gtk_grid_attach(GTK_GRID(grid), l3, 0, 4, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), a->folder, 0, 5, 2, 1);
    GtkWidget *eg = text("Pictures are numbered: Picture 001, Picture 002, ...");
    gtk_style_context_add_class(gtk_widget_get_style_context(eg), "dim-label");
    gtk_grid_attach(GTK_GRID(grid), eg, 0, 6, 2, 1);
    return grid;
}

static char *chosen_dir(imgwiz *a) {
    char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(a->folder));
    return dir != nullptr ? dir : pictures_dir();
}

/* ---- Scanning / Copying and Completing ------------------------------------------------- */

static GtkWidget *work_page(imgwiz *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    a->progress_label = text("");
    gtk_box_pack_start(GTK_BOX(box), a->progress_label, FALSE, FALSE, 0);
    a->progress = gtk_progress_bar_new();
    gtk_widget_set_margin_top(a->progress, 20);
    gtk_box_pack_start(GTK_BOX(box), a->progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), text("To stop, click Cancel."), FALSE, FALSE, 0);
    return box;
}

static void cb_open_folder([[maybe_unused]] GtkWidget *w, gpointer data) {
    imgwiz *a = data;
    if (a->saved_dir == nullptr)
        return;
    char *uri = g_filename_to_uri(a->saved_dir, nullptr, nullptr);
    if (uri != nullptr)
        gtk_show_uri_on_window(win(a), uri, GDK_CURRENT_TIME, nullptr);
    g_free(uri);
}

static GtkWidget *done_page(imgwiz *a) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    a->done_label = text("");
    gtk_box_pack_start(GTK_BOX(box), a->done_label, FALSE, FALSE, 0);
    GtkWidget *open = gtk_button_new_with_mnemonic("_Open the folder");
    gtk_widget_set_halign(open, GTK_ALIGN_START);
    g_signal_connect(open, "clicked", G_CALLBACK(cb_open_folder), a);
    gtk_box_pack_start(GTK_BOX(box), open, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(box), text("To close this wizard, click Finish."), FALSE, FALSE, 0);
    return box;
}

static void error_box(imgwiz *a, const char *msg) {
    xd_message_box(win(a), "Scanner and Camera Wizard", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
}

/* ---- the results ------------------------------------------------------------------------ */

static void job_done(imgwiz *a, job *j) {
    busy(a, false);
    bool cancelled = g_error_matches(j->error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    switch (j->kind) {
    case JOB_INFO:
        if (j->info == nullptr) {
            gtk_label_set_text(GTK_LABEL(a->preview_status), j->error->message);
            xd_wizard_set_next_sensitive(a->w, false);
            return;
        }
        a->info = j->info;
        j->info = nullptr;
        show_info(a);
        gtk_label_set_text(GTK_LABEL(a->preview_status), "");
        gtk_widget_set_sensitive(a->preview_button, TRUE);
        return;
    case JOB_PREVIEW:
        if (j->pixbuf == nullptr) {
            gtk_label_set_text(GTK_LABEL(a->preview_status), j->error->message);
            return;
        }
        {
            int w = gdk_pixbuf_get_width(j->pixbuf), h = gdk_pixbuf_get_height(j->pixbuf);
            double s = MIN((double)PREVIEW_W / w, (double)PREVIEW_H / h);
            GdkPixbuf *small = gdk_pixbuf_scale_simple(j->pixbuf, MAX(1, (int)(w * s)),
                                                       MAX(1, (int)(h * s)),
                                                       GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(a->preview), small);
            g_object_unref(small);
        }
        gtk_label_set_text(GTK_LABEL(a->preview_status), "");
        return;
    case JOB_LIST:
        a->listed = j->photos != nullptr;
        if (j->photos == nullptr) {
            gtk_label_set_text(GTK_LABEL(a->photo_status), j->error->message);
            xd_wizard_set_next_sensitive(a->w, false);
            return;
        }
        show_photos(a, j->photos);
        return;
    case JOB_SCAN:
    case JOB_COPY:
        a->saved_count += (int)j->saved->len;
        if (j->error != nullptr) {
            if (!cancelled)
                error_box(a, j->error->message);
            /* Back to the name and place, to try again. */
            xd_wizard_go(a->w, PAGE_NAME);
            return;
        }
        g_free(a->saved_dir);
        a->saved_dir = g_strdup(j->dir);
        xd_wizard_go(a->w, PAGE_DONE);
        return;
    default:
        return;
    }
}

/* ---- going through it --------------------------------------------------------------------- */

static int next_page([[maybe_unused]] xd_wizard *w, int page, gpointer data) {
    imgwiz *a = data;
    if (page == PAGE_CHOOSE && a->dev.camera) {
        GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(a->photo_view));
        bool any = sel != nullptr;
        g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
        if (!any) {
            error_box(a, "Select the pictures to copy.");
            return -1;
        }
    }
    if (page == PAGE_NAME) {
        const char *name = gtk_entry_get_text(GTK_ENTRY(a->name));
        if (!xd_scan_name_valid(name)) {
            error_box(a, "Type a name for the pictures. It can't contain a slash (/).");
            return -1;
        }
        char *dir = chosen_dir(a);
        bool ok = g_mkdir_with_parents(dir, 0755) == 0 && g_access(dir, W_OK) == 0;
        if (!ok) {
            char *msg = g_strdup_printf("Pictures can't be saved in %s. Choose another folder.",
                                        dir);
            error_box(a, msg);
            g_free(msg);
        }
        g_free(dir);
        if (!ok)
            return -1;
    }
    return page + 1;
}

static GPtrArray *selected_photos(imgwiz *a) {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)xd_camera_photo_free);
    GList *sel = gtk_icon_view_get_selected_items(GTK_ICON_VIEW(a->photo_view));
    sel = g_list_reverse(sel); /* in the camera's order */
    for (GList *l = sel; l != nullptr; l = l->next) {
        GtkTreeIter iter;
        if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(a->photos), &iter, l->data))
            continue;
        xd_camera_photo *p = g_new0(xd_camera_photo, 1);
        gtk_tree_model_get(GTK_TREE_MODEL(a->photos), &iter, P_NAME, &p->name, P_FOLDER,
                           &p->folder, -1);
        g_ptr_array_add(out, p);
    }
    g_list_free_full(sel, (GDestroyNotify)gtk_tree_path_free);
    return out;
}

static void enter_page([[maybe_unused]] xd_wizard *w, int page, gpointer data) {
    imgwiz *a = data;
    if (page == PAGE_CHOOSE && !a->dev.camera && a->info == nullptr && a->running == nullptr) {
        busy(a, true);
        gtk_label_set_text(GTK_LABEL(a->preview_status), "Connecting to the scanner...");
        start_job(a, new_job(a, JOB_INFO));
    } else if (page == PAGE_CHOOSE && a->dev.camera && !a->listed && a->running == nullptr) {
        busy(a, true);
        gtk_label_set_text(GTK_LABEL(a->photo_status), "Reading the camera's pictures...");
        job *j = new_job(a, JOB_LIST);
        j->cam = camref_ref(a->cam);
        start_job(a, j);
    } else if (page == PAGE_WORK) {
        job *j = new_job(a, a->dev.camera ? JOB_COPY : JOB_SCAN);
        j->dir = chosen_dir(a);
        j->name = g_strdup(gtk_entry_get_text(GTK_ENTRY(a->name)));
        if (a->dev.camera) {
            j->cam = camref_ref(a->cam);
            j->chosen = selected_photos(a);
            char *s = g_strdup_printf("Copying %u picture(s) to %s...", j->chosen->len, j->dir);
            gtk_label_set_text(GTK_LABEL(a->progress_label), s);
            g_free(s);
        } else {
            j->type = chosen_type(a);
            j->dpi = chosen_dpi(a);
            char *s = g_strdup_printf("Scanning a picture at %d dpi into %s...", j->dpi, j->dir);
            gtk_label_set_text(GTK_LABEL(a->progress_label), s);
            g_free(s);
        }
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->progress), 0);
        busy(a, true);
        start_job(a, j);
    } else if (page == PAGE_DONE) {
        char *s = g_strdup_printf("You have successfully copied %d picture(s) to:\n\n%s",
                                  a->saved_count, a->saved_dir != nullptr ? a->saved_dir : "");
        gtk_label_set_text(GTK_LABEL(a->done_label), s);
        g_free(s);
        /* Done is done: the pictures are saved. */
        xd_wizard_set_back_sensitive(a->w, false);
    }
}

static void imgwiz_free(gpointer data) {
    imgwiz *a = data;
    g_cancellable_cancel(a->cancel);
    g_object_unref(a->cancel);
    if (a->poll != 0)
        g_source_remove(a->poll);
    if (a->running != nullptr) {
        /* The thread stops at its next chance and drops its reference. */
        g_atomic_int_set(&a->running->cancel, 1);
        job_unref(a->running);
    }
    camref_unref(a->cam);
    xd_scanner_info_free(a->info);
    /* The view goes with the window; let go of its model first, or GTK
     * complains while tearing down its selection. */
    if (a->photo_view != nullptr)
        gtk_icon_view_set_model(GTK_ICON_VIEW(a->photo_view), nullptr);
    if (a->photos != nullptr)
        g_object_unref(a->photos);
    g_free(a->dev.id);
    g_free(a->dev.port);
    g_free(a->dev.vendor);
    g_free(a->dev.model);
    g_free(a->dev.type);
    g_free(a->saved_dir);
    g_free(a);
}

void xd_imaging_wizard_open(GtkWindow *parent, const xd_imaging_device *dev) {
    imgwiz *a = g_new0(imgwiz, 1);
    a->dev = (xd_imaging_device){
        .camera = dev->camera,
        .id = g_strdup(dev->id),
        .port = g_strdup(dev->port),
        .vendor = g_strdup(dev->vendor),
        .model = g_strdup(dev->model),
        .type = g_strdup(dev->type),
    };
    a->cancel = g_cancellable_new();
    if (dev->camera) {
        a->cam = g_new0(camref, 1);
        a->cam->refs = 1;
    }
    const char *icon = dev->camera ? "camera-photo" : "scanner";
    a->w = xd_wizard_new(parent, "Scanner and Camera Wizard", icon, a, imgwiz_free);
    xd_scanners_hold(xd_wizard_window(a->w));
    char *welcome = g_strdup_printf(
        "This wizard helps you copy pictures from %s to your computer.\n\n%s",
        dev->model,
        dev->camera ? "You choose the pictures to copy, and where to put them."
                    : "You choose the kind of picture, look at a preview, and the picture "
                      "is scanned into a folder of your choice.");
    GtkWidget *wbody = text(welcome);
    g_free(welcome);
    xd_wizard_add_exterior(a->w, "Welcome to the Scanner and Camera Wizard", wbody, false);
    if (dev->camera)
        xd_wizard_add_interior(a->w, "Choose Pictures to Copy",
                               "Pictures with a selection will be copied.",
                               camera_choose_page(a), false);
    else
        xd_wizard_add_interior(a->w, "Choose Scanning Preferences",
                               "Choose your scanning preferences, and then click Preview to "
                               "see how your picture will appear.",
                               scan_choose_page(a), false);
    xd_wizard_add_interior(a->w, "Picture Name and Destination",
                           "Select a name and destination for your pictures.", name_page(a),
                           false);
    xd_wizard_add_interior(a->w, dev->camera ? "Copying Pictures" : "Scanning Picture",
                           "Please wait while the wizard copies your pictures.", work_page(a),
                           false);
    xd_wizard_add_exterior(a->w, "Completing the Scanner and Camera Wizard", done_page(a), true);
    xd_wizard_set_next_func(a->w, next_page);
    xd_wizard_set_enter_func(a->w, enter_page);
    xd_wizard_show(a->w);
}
