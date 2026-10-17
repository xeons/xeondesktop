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

static int open_windows;

static void on_held_destroy([[maybe_unused]] GtkWidget *w, [[maybe_unused]] gpointer data) {
    if (--open_windows == 0)
        gtk_main_quit();
}

void xd_printers_hold(GtkWidget *window) {
    open_windows++;
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

/* Shows the error, if any, and tells the windows. */
static bool finish(GtkWindow *parent, bool ok, GError *error) {
    if (!ok)
        xd_printers_error(parent, error);
    g_clear_error(&error);
    xd_printers_changed();
    return ok;
}

bool xd_printer_do_set_default(GtkWindow *parent, const char *name) {
    GError *error = nullptr;
    return finish(parent, xd_printer_set_default(name, &error), error);
}

bool xd_printer_do_pause(GtkWindow *parent, const char *name, bool paused) {
    xd_cups_auth_reset();
    GError *error = nullptr;
    return finish(parent, xd_printer_pause(name, paused, &error), error);
}

bool xd_printer_do_cancel_all(GtkWindow *parent, const char *name) {
    char *q = g_strdup_printf("Are you sure you want to cancel all documents for %s?", name);
    int r = xd_message_box(parent, name, XD_MB_QUESTION, XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES)
        return false;
    xd_cups_auth_reset();
    GError *error = nullptr;
    return finish(parent, xd_printer_cancel_all(name, &error), error);
}

bool xd_printer_do_delete(GtkWindow *parent, const char *name) {
    char *q = g_strdup_printf("Are you sure you want to delete the printer %s?", name);
    int r = xd_message_box(parent, TITLE, XD_MB_QUESTION, XD_MB_YES_NO, TRUE, q);
    g_free(q);
    if (r != GTK_RESPONSE_YES)
        return false;
    xd_cups_auth_reset();
    GError *error = nullptr;
    return finish(parent, xd_printer_delete(name, &error), error);
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
