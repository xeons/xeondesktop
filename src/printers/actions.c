/* SPDX-License-Identifier: MIT */
#include "../gtkapp/gtkapp.h"
#include "auth.h"
#include "printers.h"
#include "printlib.h"

static constexpr char TITLE[] = "Printers and Faxes";

/* ---- watching ---------------------------------------------------------------------- */

typedef struct watcher {
    xd_printers_watch_fn fn;
    gpointer data;
} watcher;

static GArray *watchers;

void xd_printers_watch(xd_printers_watch_fn fn, gpointer data) {
    if (watchers == nullptr)
        watchers = g_array_new(FALSE, FALSE, sizeof(watcher));
    watcher w = {fn, data};
    g_array_append_val(watchers, w);
}

void xd_printers_unwatch(xd_printers_watch_fn fn, gpointer data) {
    for (guint i = 0; watchers != nullptr && i < watchers->len; i++) {
        watcher *w = &g_array_index(watchers, watcher, i);
        if (w->fn == fn && w->data == data) {
            g_array_remove_index(watchers, i);
            return;
        }
    }
}

void xd_printers_changed(void) {
    /* A copy: a watcher may unwatch (its window closing). */
    if (watchers == nullptr || watchers->len == 0)
        return;
    GArray *copy = g_array_copy(watchers);
    for (guint i = 0; i < copy->len; i++) {
        watcher *w = &g_array_index(copy, watcher, i);
        w->fn(w->data);
    }
    g_array_unref(copy);
}

/* ---- windows ----------------------------------------------------------------------- */

/* Open windows and windows still fetching what they show. */
static int open_windows;

void xd_printers_hold_begin(void) { open_windows++; }

void xd_printers_hold_end(void) {
    if (--open_windows == 0)
        gtk_main_quit();
}

static void on_held_destroy([[maybe_unused]] GtkWidget *w, [[maybe_unused]] gpointer data) {
    xd_printers_hold_end();
}

void xd_printers_hold(GtkWidget *window) {
    xd_printers_hold_begin();
    g_signal_connect(window, "destroy", G_CALLBACK(on_held_destroy), nullptr);
}

bool xd_printers_any_open(void) { return open_windows > 0; }

/* ---- actions ----------------------------------------------------------------------- */

void xd_printers_error(GtkWindow *parent, const GError *error) {
    if (error == nullptr ||
        (error->domain == XD_CUPS_ERROR && error->code == XD_CUPS_ERROR_CANCELED))
        return;
    xd_message_box(parent, TITLE, XD_MB_ERROR, XD_MB_OK, FALSE, error->message);
}

typedef struct change {
    GtkWidget *parent; /* a weak pointer */
    xd_printers_change_fn fn;
    gpointer data;
    GDestroyNotify free;
    xd_printers_done_fn done;
    gpointer user;
    bool ok;
    GError *error;
} change;

static void change_free(gpointer data) {
    change *ch = data;
    if (ch->parent != nullptr)
        g_object_remove_weak_pointer(G_OBJECT(ch->parent), (gpointer *)&ch->parent);
    if (ch->free != nullptr)
        ch->free(ch->data);
    g_clear_error(&ch->error);
    g_free(ch);
}

static void change_thread(GTask *task, [[maybe_unused]] gpointer src, gpointer data,
                          [[maybe_unused]] GCancellable *c) {
    change *ch = data;
    /* cupsd may want a printer administrator's password. */
    xd_cups_auth_worker_begin();
    ch->ok = ch->fn(ch->data, &ch->error);
    xd_cups_auth_worker_end();
    g_task_return_boolean(task, TRUE);
}

static void on_change_done([[maybe_unused]] GObject *src, GAsyncResult *res,
                           [[maybe_unused]] gpointer data) {
    change *ch = g_task_get_task_data(G_TASK(res));
    if (!ch->ok)
        xd_printers_error(ch->parent != nullptr ? GTK_WINDOW(ch->parent) : nullptr, ch->error);
    xd_printers_changed();
    if (ch->done != nullptr)
        ch->done(ch->ok, ch->user);
    xd_printers_hold_end();
}

void xd_printers_run(GtkWindow *parent, xd_printers_change_fn fn, gpointer data,
                     GDestroyNotify free, xd_printers_done_fn done, gpointer user) {
    change *ch = g_new0(change, 1);
    if (parent != nullptr) {
        ch->parent = GTK_WIDGET(parent);
        g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&ch->parent);
    }
    ch->fn = fn;
    ch->data = data;
    ch->free = free;
    ch->done = done;
    ch->user = user;
    /* The program stays until it's done, even if its window closes. */
    xd_printers_hold_begin();
    GTask *task = g_task_new(nullptr, nullptr, on_change_done, nullptr);
    g_task_set_task_data(task, ch, change_free);
    g_task_run_in_thread(task, change_thread);
    g_object_unref(task);
}

/* A printer and, for Pause Printing, which way. */
typedef struct named {
    char *name;
    bool flag;
} named;

static named *named_new(const char *name, bool flag) {
    named *n = g_new0(named, 1);
    n->name = g_strdup(name);
    n->flag = flag;
    return n;
}

static void named_free(gpointer data) {
    named *n = data;
    g_free(n->name);
    g_free(n);
}

static bool set_default(gpointer data, GError **error) {
    return xd_printer_set_default(((named *)data)->name, error);
}

static bool pause_printer(gpointer data, GError **error) {
    const named *n = data;
    return xd_printer_pause(n->name, n->flag, error);
}

static bool cancel_all(gpointer data, GError **error) {
    return xd_printer_cancel_all(((named *)data)->name, error);
}

static bool delete_printer(gpointer data, GError **error) {
    return xd_printer_delete(((named *)data)->name, error);
}

void xd_printer_do_set_default(GtkWindow *parent, const char *name) {
    xd_printers_run(parent, set_default, named_new(name, false), named_free, nullptr, nullptr);
}

void xd_printer_do_pause(GtkWindow *parent, const char *name, bool paused,
                         xd_printers_done_fn done, gpointer user) {
    xd_printers_run(parent, pause_printer, named_new(name, paused), named_free, done, user);
}

void xd_printer_do_cancel_all(GtkWindow *parent, const char *name) {
    char *q = g_strdup_printf("Are you sure you want to cancel all documents for %s?", name);
    int r = xd_message_box(parent, name, XD_MB_QUESTION, XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r == GTK_RESPONSE_YES)
        xd_printers_run(parent, cancel_all, named_new(name, false), named_free, nullptr, nullptr);
}

void xd_printer_do_delete(GtkWindow *parent, const char *name) {
    char *q = g_strdup_printf("Are you sure you want to delete the printer %s?", name);
    int r = xd_message_box(parent, TITLE, XD_MB_QUESTION, XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r == GTK_RESPONSE_YES)
        xd_printers_run(parent, delete_printer, named_new(name, false), named_free, nullptr,
                        nullptr);
}

/* ---- icons ------------------------------------------------------------------------- */

/* The first of `names` the icon theme has, `px` device pixels square. */
static GdkPixbuf *load_icon(const char **names, int px) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GtkIconInfo *info = gtk_icon_theme_choose_icon(theme, names, px,
                                                   GTK_ICON_LOOKUP_FORCE_SIZE);
    GdkPixbuf *pix = info != nullptr ? gtk_icon_info_load_icon(info, nullptr) : nullptr;
    if (info != nullptr)
        g_object_unref(info);
    return pix;
}

static void paint(cairo_t *cr, GdkPixbuf *pix, double x, double y, double alpha) {
    if (pix == nullptr)
        return;
    gdk_cairo_set_source_pixbuf(cr, pix, x, y);
    cairo_paint_with_alpha(cr, alpha);
    g_object_unref(pix);
}

cairo_surface_t *xd_printer_icon(const xd_printer *p, int size, int scale) {
    static const char *printer[] = {"printer", "printer-laser", nullptr};
    static const char *fax[] = {"printer-fax", "printer", nullptr};
    static const char *network[] = {"printer-network", "printer", nullptr};
    static const char *check_icons[] = {"emblem-default", "emblem-checked", "emblem-ok-symbolic",
                                        nullptr};
    static const char *pause_icons[] = {"emblem-pause", "media-playback-pause", nullptr};
    static const char *error_icons[] = {"emblem-error", "dialog-error", nullptr};

    int px = size * scale;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(s);
    const char *status = xd_printer_status_text(p->state, p->reasons, p->accepting);
    bool offline = strcmp(status, "Offline") == 0;
    const char **base = p->fax                                   ? fax
                              : xd_printer_uri_is_network(p->device_uri) ? network
                                                                       : printer;
    paint(cr, load_icon(base, px), 0, 0, offline ? 0.45 : 1.0);
    /* The emblems: about a third of the icon, at least 8 pixels. */
    int ep = MAX(8, size * 3 / 8) * scale;
    if (p->is_default)
        paint(cr, load_icon(check_icons, ep), 0, 0, 1.0);
    if (xd_printer_is_paused(p->state, p->reasons))
        paint(cr, load_icon(pause_icons, ep), px - ep, px - ep, 1.0);
    else if (strcmp(status, "Error") == 0)
        paint(cr, load_icon(error_icons, ep), px - ep, px - ep, 1.0);
    cairo_destroy(cr);
    cairo_surface_set_device_scale(s, scale, scale);
    return s;
}
