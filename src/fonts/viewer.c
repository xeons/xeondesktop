/* SPDX-License-Identifier: MIT */
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <glib/gstdio.h>

#include "../gtkapp/gtkapp.h"
#include "fontlist.h"
#include "viewer.h"

static const char *const alphabet[] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "1234567890.:,;' \" (!?) +-*/=",
};
static const int sample_sizes[] = {12, 18, 24, 36, 48, 60, 72};
#define SAMPLE "The quick brown fox jumps over the lazy dog. 1234567890"

static constexpr double MARGIN = 12;

typedef struct viewer {
    GtkWidget *window, *area;
    cairo_font_face_t *face;
    char *info[3]; /* typeface name, file size, version */
} viewer;

static void viewer_free(gpointer data) {
    viewer *v = data;
    cairo_font_face_destroy(v->face);
    for (size_t i = 0; i < G_N_ELEMENTS(v->info); i++)
        g_free(v->info[i]);
    g_free(v);
}

/* Points at 96 DPI, in logical pixels. */
static double pt_to_px(int pt) { return pt * 96.0 / 72.0; }

/* One line in the dialog's own font; returns its height. */
static double ui_text(GtkWidget *w, cairo_t *cr, const char *text, double x, double y,
                      double *right) {
    PangoLayout *l = gtk_widget_create_pango_layout(w, text);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    *right = MAX(*right, x + lw);
    return lh;
}

/* One line in the font itself, its top at y; returns its height. */
static double font_text(viewer *v, cairo_t *cr, const char *text, int pt, double x, double y,
                        double *right) {
    cairo_set_font_face(cr, v->face);
    cairo_set_font_size(cr, pt_to_px(pt));
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_text_extents_t te;
    cairo_text_extents(cr, text, &te);
    cairo_move_to(cr, x, y + fe.ascent);
    cairo_show_text(cr, text);
    *right = MAX(*right, x + te.x_advance);
    return fe.height;
}

static void rule(cairo_t *cr, double y, double width) {
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, MARGIN, y + 0.5);
    cairo_line_to(cr, width - MARGIN, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
}

/* Draws the page `width` wide; returns its height and widest line. */
static double paint(viewer *v, GtkWidget *w, cairo_t *cr, double width, double *right) {
    double y = MARGIN;
    *right = 0;
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(v->info); i++)
        if (v->info[i][0] != '\0')
            y += ui_text(w, cr, v->info[i], MARGIN, y, right) + 2;
    y += 8;
    rule(cr, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(alphabet); i++)
        y += font_text(v, cr, alphabet[i], 12, MARGIN, y, right);
    y += 8;
    rule(cr, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(sample_sizes); i++) {
        char label[8];
        g_snprintf(label, sizeof(label), "%d", sample_sizes[i]);
        /* The size on the sample's baseline. */
        cairo_set_font_face(cr, v->face);
        cairo_set_font_size(cr, pt_to_px(sample_sizes[i]));
        cairo_font_extents_t fe;
        cairo_font_extents(cr, &fe);
        PangoLayout *l = gtk_widget_create_pango_layout(w, label);
        double base = pango_layout_get_baseline(l) / (double)PANGO_SCALE;
        g_object_unref(l);
        double lh = ui_text(w, cr, label, MARGIN, y + fe.ascent - base, right);
        double fh = font_text(v, cr, SAMPLE, sample_sizes[i], MARGIN + 36, y, right);
        y += MAX(lh, fh) + 6;
    }
    return y + MARGIN;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    viewer *v = data;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    double right;
    paint(v, w, cr, gtk_widget_get_allocated_width(w), &right);
    return FALSE;
}

/* Sizes the page to what it draws, once it can make layouts. */
static void on_realize(GtkWidget *w, gpointer data) {
    viewer *v = data;
    cairo_surface_t *rec = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
    cairo_t *cr = cairo_create(rec);
    double right = 0;
    double height = paint(v, w, cr, 600, &right);
    cairo_destroy(cr);
    cairo_surface_destroy(rec);
    gtk_widget_set_size_request(w, (int)(right + MARGIN), (int)height);
}

static char *file_size_text(const char *file) {
    GStatBuf st;
    if (g_stat(file, &st) != 0)
        return g_strdup("");
    guint64 kb = ((guint64)st.st_size + 1023) / 1024;
    return g_strdup_printf("File size: %" G_GUINT64_FORMAT " KB", kb);
}

static void on_done([[maybe_unused]] GtkButton *b, gpointer window) {
    gtk_widget_destroy(window);
}

GtkWidget *xd_font_viewer_new(GtkWindow *parent, const char *file, int index) {
    int count = 0;
    FcPattern *pat = FcFreeTypeQuery((const FcChar8 *)file, (unsigned)index, nullptr, &count);
    if (pat == nullptr) {
        char *base = g_path_get_basename(file);
        char *msg = g_strdup_printf("The requested file %s was not a valid font file.", base);
        xd_message_box(parent, "Font Viewer", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
        g_free(base);
        return nullptr;
    }
    viewer *v = g_new0(viewer, 1);
    v->face = cairo_ft_font_face_create_for_pattern(pat);
    FcChar8 *s = nullptr;
    const char *family = FcPatternGetString(pat, FC_FAMILY, 0, &s) == FcResultMatch
                             ? (const char *)s
                             : "";
    char *family_copy = g_strdup(family);
    v->info[0] = g_strdup_printf("Typeface name: %s", family_copy);
    v->info[1] = file_size_text(file);
    int version = 0;
    FcPatternGetInteger(pat, FC_FONTVERSION, 0, &version);
    v->info[2] = xd_font_version_text(version);

    char *full = FcPatternGetString(pat, FC_FULLNAME, 0, &s) == FcResultMatch
                     ? g_strdup((const char *)s)
                     : nullptr;
    char *style = FcPatternGetString(pat, FC_STYLE, 0, &s) == FcResultMatch
                      ? g_strdup((const char *)s)
                      : nullptr;
    char *format = FcPatternGetString(pat, FC_FONTFORMAT, 0, &s) == FcResultMatch
                       ? g_strdup((const char *)s)
                       : nullptr;
    char *title = xd_font_display_name(full, family_copy, style,
                                       xd_font_format_label(format, file));
    FcPatternDestroy(pat);

    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window), title);
    gtk_window_set_icon_name(GTK_WINDOW(v->window), "preferences-desktop-font");
    gtk_window_set_default_size(GTK_WINDOW(v->window), 680, 560);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(v->window), parent);
    g_object_set_data_full(G_OBJECT(v->window), "xd-viewer", v, viewer_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(v->window), box);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    GtkWidget *done = gtk_button_new_with_mnemonic("_Done");
    g_signal_connect(done, "clicked", G_CALLBACK(on_done), v->window);
    gtk_box_pack_start(GTK_BOX(bar), done, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    v->area = gtk_drawing_area_new();
    g_signal_connect(v->area, "draw", G_CALLBACK(on_draw), v);
    g_signal_connect(v->area, "realize", G_CALLBACK(on_realize), v);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), v->area);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(v->window);
    gtk_widget_grab_focus(done);

    g_free(title);
    g_free(full);
    g_free(style);
    g_free(format);
    g_free(family_copy);
    return v->window;
}
