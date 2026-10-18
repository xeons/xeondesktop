/* SPDX-License-Identifier: MIT */
#include <fontconfig/fontconfig.h>
#include <fontconfig/fcfreetype.h>
#include <glib/gstdio.h>
#include <pango/pangofc-fontmap.h>

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
    /* The font alone: Pango with a fontconfig of this file only, so the
     * text is shaped (Arabic, Indic...) but nothing comes from another font
     * (what it lacks shows as boxes, as in Windows). */
    FcConfig *config;
    PangoFontMap *fonts;
    PangoFontDescription *desc;
    char *info[3]; /* typeface name, file size, version */
    char *title;   /* "Arial (TrueType)" */
    char *lines[3]; /* the character lines; "" for none */
    char *sample;   /* the line at each size */
} viewer;

static void viewer_free(gpointer data) {
    viewer *v = data;
    g_clear_object(&v->fonts);
    if (v->config != nullptr)
        FcConfigDestroy(v->config);
    if (v->desc != nullptr)
        pango_font_description_free(v->desc);
    for (size_t i = 0; i < G_N_ELEMENTS(v->info); i++)
        g_free(v->info[i]);
    for (size_t i = 0; i < G_N_ELEMENTS(v->lines); i++)
        g_free(v->lines[i]);
    g_free(v->sample);
    g_free(v->title);
    g_free(v);
}

/* `text` without the punctuation the font lacks (a Noto font for one
 * script leaves the full stop to others), or nullptr when it lacks
 * anything else. */
static char *covered_sentence(FcCharSet *cs, const char *text) {
    GString *out = g_string_new(nullptr);
    for (const char *p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c == ' ' || FcCharSetHasChar(cs, c)) {
            g_string_append_unichar(out, c);
        } else if (!g_unichar_ispunct(c)) {
            g_string_free(out, TRUE);
            return nullptr;
        }
    }
    return g_strstrip(g_string_free(out, FALSE));
}

/* Whether the font has every character of `text` but spaces. */
static bool covers(FcCharSet *cs, const char *text) {
    for (const char *p = text; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (c != ' ' && !FcCharSetHasChar(cs, c))
            return false;
    }
    return true;
}

/* Up to three lines of the font's own characters, 26 to a line: those
 * past Latin-1 first (a font for another script has a few Latin ones
 * too), any it has when it has nothing else. Lone combining marks and
 * what doesn't print are left out. */
static void own_lines(viewer *v, FcCharSet *cs) {
    GString *line[G_N_ELEMENTS(v->lines)] = {0};
    for (size_t i = 0; i < G_N_ELEMENTS(line); i++)
        line[i] = g_string_new(nullptr);
    for (int pass = 0; pass < 2 && line[0]->len == 0; pass++) {
        size_t n = 0;
        FcChar32 map[FC_CHARSET_MAP_SIZE];
        FcChar32 next = 0;
        for (FcChar32 base = FcCharSetFirstPage(cs, map, &next);
             base != FC_CHARSET_DONE && n < G_N_ELEMENTS(line) * 26;
             base = FcCharSetNextPage(cs, map, &next)) {
            for (unsigned i = 0; i < FC_CHARSET_MAP_SIZE * 32 && n < G_N_ELEMENTS(line) * 26; i++) {
                if ((map[i / 32] & (1u << (i % 32))) == 0)
                    continue;
                gunichar c = base + i;
                if ((pass == 0 && (c < 0x100 || g_unichar_ispunct(c))) || !g_unichar_isgraph(c) ||
                    g_unichar_ismark(c))
                    continue;
                g_string_append_unichar(line[n / 26], c);
                n++;
            }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(line); i++)
        v->lines[i] = g_string_free(line[i], FALSE);
}

/* What the page shows: Windows' alphabet and "The quick brown fox" for a
 * font with the Latin alphabet; for one without, as Windows 7's viewer
 * did, its own characters and a sentence in a language it covers (Pango's
 * sample for that language), else its characters again. */
static void choose_samples(viewer *v, FcPattern *pat) {
    FcCharSet *cs = nullptr;
    if (FcPatternGetCharSet(pat, FC_CHARSET, 0, &cs) != FcResultMatch ||
        covers(cs, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
        for (size_t i = 0; i < G_N_ELEMENTS(v->lines); i++)
            v->lines[i] = g_strdup(alphabet[i]);
        v->sample = g_strdup(SAMPLE);
        return;
    }
    own_lines(v, cs);
    FcLangSet *ls = nullptr;
    if (FcPatternGetLangSet(pat, FC_LANG, 0, &ls) == FcResultMatch) {
        FcStrSet *langs = FcLangSetGetLangs(ls);
        FcStrList *it = FcStrListCreate(langs);
        for (FcChar8 *lang; v->sample == nullptr && (lang = FcStrListNext(it)) != nullptr;) {
            const char *text =
                pango_language_get_sample_string(pango_language_from_string((const char *)lang));
            if (text != nullptr)
                v->sample = covered_sentence(cs, text);
        }
        FcStrListDone(it);
        FcStrSetDestroy(langs);
    }
    if (v->sample == nullptr)
        v->sample = g_strdup(v->lines[0]);
}

/* Points at 96 DPI, in logical pixels. */
static double pt_to_px(int pt) { return pt * 96.0 / 72.0; }

/* One line in the dialog's own font (`ui`); returns its height. */
static double ui_text(PangoContext *ui, cairo_t *cr, const char *text, double x,
                      double y, double *right) {
    PangoLayout *l = pango_layout_new(ui);
    pango_layout_set_text(l, text, -1);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    *right = MAX(*right, x + lw);
    return lh;
}

/* `text` in the font at `pt` points, laid out for `cr`. */
static PangoLayout *font_layout(viewer *v, cairo_t *cr, const char *text, int pt) {
    PangoContext *ctx = pango_font_map_create_context(v->fonts);
    pango_cairo_update_context(cr, ctx);
    PangoFontDescription *d = pango_font_description_copy(v->desc);
    double px = pt_to_px(pt);
    pango_font_description_set_absolute_size(d, px * PANGO_SCALE);
    PangoLayout *l = pango_layout_new(ctx);
    g_object_unref(ctx);
    pango_layout_set_font_description(l, d);
    pango_font_description_free(d);
    pango_layout_set_text(l, text, -1);
    return l;
}

/* One line in the font itself, its top at y; returns its height. */
static double font_text(viewer *v, cairo_t *cr, const char *text, int pt, double x, double y,
                        double *right) {
    PangoLayout *l = font_layout(v, cr, text, pt);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(l, &lw, &lh);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    *right = MAX(*right, x + lw);
    return lh;
}

static void rule(cairo_t *cr, double y, double width) {
    cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, MARGIN, y + 0.5);
    cairo_line_to(cr, width - MARGIN, y + 0.5);
    cairo_stroke(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
}

/* Draws the page `width` wide (logical pixels), the details in `ui`;
 * returns its height and widest line. The window and the printed page
 * are the same. */
static double paint(viewer *v, PangoContext *ui, cairo_t *cr, double width,
                    double *right) {
    double y = MARGIN;
    *right = 0;
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(v->info); i++)
        if (v->info[i][0] != '\0')
            y += ui_text(ui, cr, v->info[i], MARGIN, y, right) + 2;
    y += 8;
    rule(cr, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(v->lines); i++)
        if (v->lines[i][0] != '\0')
            y += font_text(v, cr, v->lines[i], 12, MARGIN, y, right);
    y += 8;
    rule(cr, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(sample_sizes); i++) {
        char label[8];
        g_snprintf(label, sizeof(label), "%d", sample_sizes[i]);
        /* The size on the sample's baseline. */
        PangoLayout *fl = font_layout(v, cr, v->sample, sample_sizes[i]);
        double ascent = pango_layout_get_baseline(fl) / (double)PANGO_SCALE;
        g_object_unref(fl);
        PangoLayout *l = pango_layout_new(ui);
        pango_layout_set_text(l, label, -1);
        double base = pango_layout_get_baseline(l) / (double)PANGO_SCALE;
        g_object_unref(l);
        double lh = ui_text(ui, cr, label, MARGIN, y + ascent - base, right);
        double fh = font_text(v, cr, v->sample, sample_sizes[i], MARGIN + 36, y, right);
        y += MAX(lh, fh) + 6;
    }
    return y + MARGIN;
}

static const PangoFontDescription *widget_font(GtkWidget *w) {
    return pango_context_get_font_description(gtk_widget_get_pango_context(w));
}

/* The page's size when drawn `width` wide: height, and the widest line. */
static double measure(viewer *v, PangoContext *ui, double width, double *right) {
    cairo_surface_t *rec = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
    cairo_t *cr = cairo_create(rec);
    double height = paint(v, ui, cr, width, right);
    cairo_destroy(cr);
    cairo_surface_destroy(rec);
    return height;
}

static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer data) {
    viewer *v = data;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    double right;
    paint(v, gtk_widget_get_pango_context(w), cr, gtk_widget_get_allocated_width(w), &right);
    return FALSE;
}

/* Sizes the page to what it draws, once it can make layouts. */
static void on_realize(GtkWidget *w, gpointer data) {
    double right = 0;
    double height = measure(data, gtk_widget_get_pango_context(w), 600, &right);
    gtk_widget_set_size_request(w, (int)(right + MARGIN), (int)height);
}

static char *file_size_text(const char *file) {
    GStatBuf st;
    if (g_stat(file, &st) != 0)
        return g_strdup("");
    guint64 kb = ((guint64)st.st_size + 1023) / 1024;
    return g_strdup_printf("File size: %" G_GUINT64_FORMAT " KB", kb);
}

/* Face `index` of `file`, with its details; nullptr (after telling the
 * user) when it isn't a font fontconfig can read. */
static viewer *viewer_load(GtkWindow *parent, const char *file, int index) {
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
    v->config = FcConfigCreate();
    FcConfigAppFontAddFile(v->config, (const FcChar8 *)file);
    v->fonts = pango_cairo_font_map_new();
    pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(v->fonts), v->config);
    v->desc = pango_fc_font_description_from_pattern(pat, FALSE);
    choose_samples(v, pat);
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
    v->title = xd_font_display_name(full, family_copy, style, xd_font_format_label(format, file));
    FcPatternDestroy(pat);
    g_free(full);
    g_free(style);
    g_free(format);
    g_free(family_copy);
    return v;
}

/* ---- printing ----------------------------------------------------------------------- */

typedef struct print_job {
    viewer *v;
    PangoFontDescription *ui;
} print_job;

/* The viewer's page on paper. */
static void draw_page([[maybe_unused]] GtkPrintOperation *op, GtkPrintContext *ctx,
                      [[maybe_unused]] int page, gpointer data) {
    print_job *job = data;
    cairo_t *cr = gtk_print_context_get_cairo_context(ctx);
    /* Points on paper, logical pixels (1/96 inch) in the page. */
    double scale = 72.0 / 96.0;
    double width = gtk_print_context_get_width(ctx) / scale;
    double height = gtk_print_context_get_height(ctx) / scale;
    /* The details in the window's font at 96 DPI, as on the screen. */
    PangoContext *pc = pango_cairo_create_context(cr);
    pango_cairo_context_set_resolution(pc, 96);
    pango_context_set_font_description(pc, job->ui);
    /* Sizes stay true on paper: the long samples run off the page's edge,
     * as in the window; only a page taller than the paper shrinks. */
    double right = 0;
    double need = measure(job->v, pc, width, &right);
    double fit = MIN(1.0, height / need);
    cairo_rectangle(cr, 0, 0, gtk_print_context_get_width(ctx), gtk_print_context_get_height(ctx));
    cairo_clip(cr);
    cairo_scale(cr, scale * fit, scale * fit);
    paint(job->v, pc, cr, width / fit, &right);
    g_object_unref(pc);
}

static void print(GtkWindow *parent, viewer *v, const PangoFontDescription *ui) {
    print_job job = {v, pango_font_description_copy(ui)};
    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_n_pages(op, 1);
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_job_name(op, v->title);
    g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), &job);
    GError *error = nullptr;
    GtkPrintOperationResult r = gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG,
                                                        parent, &error);
    if (r == GTK_PRINT_OPERATION_RESULT_ERROR) {
        char *msg = g_strdup_printf("%s could not be printed.\n\n%s", v->title,
                                    error != nullptr ? error->message : "");
        xd_message_box(parent, "Font Viewer", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
    }
    g_clear_error(&error);
    g_object_unref(op);
    pango_font_description_free(job.ui);
}

void xd_font_print(GtkWindow *parent, const char *file, int index) {
    viewer *v = viewer_load(parent, file, index);
    if (v == nullptr)
        return;
    /* The details in the window's font, as the viewer shows them. */
    PangoFontDescription *ui = parent != nullptr
                                   ? pango_font_description_copy(widget_font(GTK_WIDGET(parent)))
                                   : pango_font_description_from_string("Sans 9");
    print(parent, v, ui);
    pango_font_description_free(ui);
    viewer_free(v);
}

/* ---- the window --------------------------------------------------------------------- */

static void on_done([[maybe_unused]] GtkButton *b, gpointer window) {
    gtk_widget_destroy(window);
}

static void on_print([[maybe_unused]] GtkButton *b, gpointer data) {
    viewer *v = data;
    print(GTK_WINDOW(v->window), v, widget_font(v->area));
}

GtkWidget *xd_font_viewer_new(GtkWindow *parent, const char *file, int index) {
    viewer *v = viewer_load(parent, file, index);
    if (v == nullptr)
        return nullptr;
    v->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(v->window), v->title);
    gtk_window_set_icon_name(GTK_WINDOW(v->window), "preferences-desktop-font");
    gtk_window_set_default_size(GTK_WINDOW(v->window), 680, 560);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(v->window), parent);
    g_object_set_data_full(G_OBJECT(v->window), "xd-viewer", v, viewer_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(v->window), box);
    /* XP's two buttons over the page. */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    GtkWidget *done = gtk_button_new_with_mnemonic("_Done");
    g_signal_connect(done, "clicked", G_CALLBACK(on_done), v->window);
    gtk_box_pack_start(GTK_BOX(bar), done, FALSE, FALSE, 0);
    GtkWidget *print_button = gtk_button_new_with_mnemonic("_Print...");
    g_signal_connect(print_button, "clicked", G_CALLBACK(on_print), v);
    gtk_box_pack_start(GTK_BOX(bar), print_button, FALSE, FALSE, 0);
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
    return v->window;
}
