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
    viewer *font_viewer = data;
    g_clear_object(&font_viewer->fonts);
    if (font_viewer->config != nullptr)
        FcConfigDestroy(font_viewer->config);
    if (font_viewer->desc != nullptr)
        pango_font_description_free(font_viewer->desc);
    for (size_t i = 0; i < G_N_ELEMENTS(font_viewer->info); i++)
        g_free(font_viewer->info[i]);
    for (size_t i = 0; i < G_N_ELEMENTS(font_viewer->lines); i++)
        g_free(font_viewer->lines[i]);
    g_free(font_viewer->sample);
    g_free(font_viewer->title);
    g_free(font_viewer);
}

/* `text` without the punctuation the font lacks (a Noto font for one
 * script leaves the full stop to others), or nullptr when it lacks
 * anything else. */
static char *covered_sentence(FcCharSet *cs, const char *text) {
    GString *out = g_string_new(nullptr);
    for (const char *position = text; *position != '\0'; position = g_utf8_next_char(position)) {
        gunichar character = g_utf8_get_char(position);
        if (character == ' ' || FcCharSetHasChar(cs, character)) {
            g_string_append_unichar(out, character);
        } else if (!g_unichar_ispunct(character)) {
            g_string_free(out, TRUE);
            return nullptr;
        }
    }
    return g_strstrip(g_string_free(out, FALSE));
}

/* Whether the font has every character of `text` but spaces. */
static bool covers(FcCharSet *cs, const char *text) {
    for (const char *position = text; *position != '\0'; position = g_utf8_next_char(position)) {
        gunichar character = g_utf8_get_char(position);
        if (character != ' ' && !FcCharSetHasChar(cs, character))
            return false;
    }
    return true;
}

/* Up to three lines of the font's own characters, 26 to a line: those
 * past Latin-1 first (a font for another script has a few Latin ones
 * too), any it has when it has nothing else. Lone combining marks and
 * what doesn't print are left out. */
static void own_lines(viewer *font_viewer, FcCharSet *cs) {
    GString *line[G_N_ELEMENTS(font_viewer->lines)] = {0};
    for (size_t i = 0; i < G_N_ELEMENTS(line); i++)
        line[i] = g_string_new(nullptr);
    for (int pass = 0; pass < 2 && line[0]->len == 0; pass++) {
        size_t count = 0;
        FcChar32 map[FC_CHARSET_MAP_SIZE];
        FcChar32 next = 0;
        for (FcChar32 base = FcCharSetFirstPage(cs, map, &next);
             base != FC_CHARSET_DONE && count < G_N_ELEMENTS(line) * 26;
             base = FcCharSetNextPage(cs, map, &next)) {
            for (unsigned i = 0; i < FC_CHARSET_MAP_SIZE * 32 && count < G_N_ELEMENTS(line) * 26;
                 i++) {
                if ((map[i / 32] & (1u << (i % 32))) == 0)
                    continue;
                gunichar character = base + i;
                if ((pass == 0 && (character < 0x100 || g_unichar_ispunct(character))) ||
                    !g_unichar_isgraph(character) || g_unichar_ismark(character))
                    continue;
                g_string_append_unichar(line[count / 26], character);
                count++;
            }
        }
    }
    for (size_t i = 0; i < G_N_ELEMENTS(line); i++)
        font_viewer->lines[i] = g_string_free(line[i], FALSE);
}

/* What the page shows: Windows' alphabet and "The quick brown fox" for a
 * font with the Latin alphabet; for one without, as Windows 7's viewer
 * did, its own characters and a sentence in a language it covers (Pango's
 * sample for that language), else its characters again. */
static void choose_samples(viewer *font_viewer, FcPattern *pat) {
    FcCharSet *cs = nullptr;
    if (FcPatternGetCharSet(pat, FC_CHARSET, 0, &cs) != FcResultMatch ||
        covers(cs, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")) {
        for (size_t i = 0; i < G_N_ELEMENTS(font_viewer->lines); i++)
            font_viewer->lines[i] = g_strdup(alphabet[i]);
        font_viewer->sample = g_strdup(SAMPLE);
        return;
    }
    own_lines(font_viewer, cs);
    FcLangSet *ls = nullptr;
    if (FcPatternGetLangSet(pat, FC_LANG, 0, &ls) == FcResultMatch) {
        FcStrSet *langs = FcLangSetGetLangs(ls);
        FcStrList *it = FcStrListCreate(langs);
        for (FcChar8 *lang;
             font_viewer->sample == nullptr && (lang = FcStrListNext(it)) != nullptr;) {
            const char *text =
                pango_language_get_sample_string(pango_language_from_string((const char *)lang));
            if (text != nullptr)
                font_viewer->sample = covered_sentence(cs, text);
        }
        FcStrListDone(it);
        FcStrSetDestroy(langs);
    }
    if (font_viewer->sample == nullptr)
        font_viewer->sample = g_strdup(font_viewer->lines[0]);
}

/* Points at 96 DPI, in logical pixels. */
static double pt_to_px(int pt) { return pt * 96.0 / 72.0; }

/* One line in the dialog's own font (`ui`); returns its height. */
static double ui_text(PangoContext *ui, cairo_t *cairo, const char *text, double x, double y,
                      double *right) {
    PangoLayout *layout = pango_layout_new(ui);
    pango_layout_set_text(layout, text, -1);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
    *right = MAX(*right, x + lw);
    return lh;
}

/* `text` in the font at `pt` points, laid out for `cairo`. */
static PangoLayout *font_layout(viewer *font_viewer, cairo_t *cairo, const char *text, int pt) {
    PangoContext *ctx = pango_font_map_create_context(font_viewer->fonts);
    pango_cairo_update_context(cairo, ctx);
    PangoFontDescription *description = pango_font_description_copy(font_viewer->desc);
    double px = pt_to_px(pt);
    pango_font_description_set_absolute_size(description, px * PANGO_SCALE);
    PangoLayout *layout = pango_layout_new(ctx);
    g_object_unref(ctx);
    pango_layout_set_font_description(layout, description);
    pango_font_description_free(description);
    pango_layout_set_text(layout, text, -1);
    return layout;
}

/* One line in the font itself, its top at y; returns its height. */
static double font_text(viewer *font_viewer, cairo_t *cairo, const char *text, int pt, double x,
                        double y, double *right) {
    PangoLayout *layout = font_layout(font_viewer, cairo, text, pt);
    int lw = 0, lh = 0;
    pango_layout_get_pixel_size(layout, &lw, &lh);
    cairo_move_to(cairo, x, y);
    pango_cairo_show_layout(cairo, layout);
    g_object_unref(layout);
    *right = MAX(*right, x + lw);
    return lh;
}

static void rule(cairo_t *cairo, double y, double width) {
    cairo_set_source_rgb(cairo, 0.6, 0.6, 0.6);
    cairo_set_line_width(cairo, 1);
    cairo_move_to(cairo, MARGIN, y + 0.5);
    cairo_line_to(cairo, width - MARGIN, y + 0.5);
    cairo_stroke(cairo);
    cairo_set_source_rgb(cairo, 0, 0, 0);
}

/* Draws the page `width` wide (logical pixels), the details in `ui`;
 * returns its height and widest line. The window and the printed page
 * are the same. */
static double paint(viewer *font_viewer, PangoContext *ui, cairo_t *cairo, double width,
                    double *right) {
    double y = MARGIN;
    *right = 0;
    cairo_set_source_rgb(cairo, 0, 0, 0);
    for (size_t i = 0; i < G_N_ELEMENTS(font_viewer->info); i++)
        if (font_viewer->info[i][0] != '\0')
            y += ui_text(ui, cairo, font_viewer->info[i], MARGIN, y, right) + 2;
    y += 8;
    rule(cairo, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(font_viewer->lines); i++)
        if (font_viewer->lines[i][0] != '\0')
            y += font_text(font_viewer, cairo, font_viewer->lines[i], 12, MARGIN, y, right);
    y += 8;
    rule(cairo, y, width);
    y += 10;
    for (size_t i = 0; i < G_N_ELEMENTS(sample_sizes); i++) {
        char label[8];
        g_snprintf(label, sizeof(label), "%d", sample_sizes[i]);
        /* The size on the sample's baseline. */
        PangoLayout *fl = font_layout(font_viewer, cairo, font_viewer->sample, sample_sizes[i]);
        double ascent = pango_layout_get_baseline(fl) / (double)PANGO_SCALE;
        g_object_unref(fl);
        PangoLayout *layout = pango_layout_new(ui);
        pango_layout_set_text(layout, label, -1);
        double base = pango_layout_get_baseline(layout) / (double)PANGO_SCALE;
        g_object_unref(layout);
        double lh = ui_text(ui, cairo, label, MARGIN, y + ascent - base, right);
        double fh = font_text(font_viewer, cairo, font_viewer->sample, sample_sizes[i], MARGIN + 36,
                              y, right);
        y += MAX(lh, fh) + 6;
    }
    return y + MARGIN;
}

static const PangoFontDescription *widget_font(GtkWidget *widget) {
    return pango_context_get_font_description(gtk_widget_get_pango_context(widget));
}

/* The page's size when drawn `width` wide: height, and the widest line. */
static double measure(viewer *font_viewer, PangoContext *ui, double width, double *right) {
    cairo_surface_t *rec = cairo_recording_surface_create(CAIRO_CONTENT_COLOR_ALPHA, nullptr);
    cairo_t *cairo = cairo_create(rec);
    double height = paint(font_viewer, ui, cairo, width, right);
    cairo_destroy(cairo);
    cairo_surface_destroy(rec);
    return height;
}

static gboolean on_draw(GtkWidget *widget, cairo_t *cairo, gpointer data) {
    viewer *font_viewer = data;
    cairo_set_source_rgb(cairo, 1, 1, 1);
    cairo_paint(cairo);
    double right;
    paint(font_viewer, gtk_widget_get_pango_context(widget), cairo,
          gtk_widget_get_allocated_width(widget), &right);
    return FALSE;
}

/* Sizes the page to what it draws, once it can make layouts. */
static void on_realize(GtkWidget *widget, gpointer data) {
    double right = 0;
    double height = measure(data, gtk_widget_get_pango_context(widget), 600, &right);
    gtk_widget_set_size_request(widget, (int)(right + MARGIN), (int)height);
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
    viewer *font_viewer = g_new0(viewer, 1);
    font_viewer->config = FcConfigCreate();
    FcConfigAppFontAddFile(font_viewer->config, (const FcChar8 *)file);
    font_viewer->fonts = pango_cairo_font_map_new();
    pango_fc_font_map_set_config(PANGO_FC_FONT_MAP(font_viewer->fonts), font_viewer->config);
    font_viewer->desc = pango_fc_font_description_from_pattern(pat, FALSE);
    choose_samples(font_viewer, pat);
    FcChar8 *family_value = nullptr;
    const char *family = FcPatternGetString(pat, FC_FAMILY, 0, &family_value) == FcResultMatch
                             ? (const char *)family_value
                             : "";
    char *family_copy = g_strdup(family);
    font_viewer->info[0] = g_strdup_printf("Typeface name: %s", family_copy);
    font_viewer->info[1] = file_size_text(file);
    int version = 0;
    FcPatternGetInteger(pat, FC_FONTVERSION, 0, &version);
    font_viewer->info[2] = xd_font_version_text(version);

    char *full = FcPatternGetString(pat, FC_FULLNAME, 0, &family_value) == FcResultMatch
                     ? g_strdup((const char *)family_value)
                     : nullptr;
    char *style = FcPatternGetString(pat, FC_STYLE, 0, &family_value) == FcResultMatch
                      ? g_strdup((const char *)family_value)
                      : nullptr;
    char *format = FcPatternGetString(pat, FC_FONTFORMAT, 0, &family_value) == FcResultMatch
                       ? g_strdup((const char *)family_value)
                       : nullptr;
    font_viewer->title =
        xd_font_display_name(full, family_copy, style, xd_font_format_label(format, file));
    FcPatternDestroy(pat);
    g_free(full);
    g_free(style);
    g_free(format);
    g_free(family_copy);
    return font_viewer;
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
    cairo_t *cairo = gtk_print_context_get_cairo_context(ctx);
    /* Points on paper, logical pixels (1/96 inch) in the page. */
    double scale = 72.0 / 96.0;
    double width = gtk_print_context_get_width(ctx) / scale;
    double height = gtk_print_context_get_height(ctx) / scale;
    /* The details in the window's font at 96 DPI, as on the screen. */
    PangoContext *pc = pango_cairo_create_context(cairo);
    pango_cairo_context_set_resolution(pc, 96);
    pango_context_set_font_description(pc, job->ui);
    /* Sizes stay true on paper: the long samples run off the page's edge,
     * as in the window; only a page taller than the paper shrinks. */
    double right = 0;
    double need = measure(job->v, pc, width, &right);
    double fit = MIN(1.0, height / need);
    cairo_rectangle(cairo, 0, 0, gtk_print_context_get_width(ctx),
                    gtk_print_context_get_height(ctx));
    cairo_clip(cairo);
    cairo_scale(cairo, scale * fit, scale * fit);
    paint(job->v, pc, cairo, width / fit, &right);
    g_object_unref(pc);
}

static void print(GtkWindow *parent, viewer *font_viewer, const PangoFontDescription *ui) {
    print_job job = {font_viewer, pango_font_description_copy(ui)};
    GtkPrintOperation *op = gtk_print_operation_new();
    gtk_print_operation_set_n_pages(op, 1);
    gtk_print_operation_set_unit(op, GTK_UNIT_POINTS);
    gtk_print_operation_set_job_name(op, font_viewer->title);
    g_signal_connect(op, "draw-page", G_CALLBACK(draw_page), &job);
    GError *error = nullptr;
    GtkPrintOperationResult result_code =
        gtk_print_operation_run(op, GTK_PRINT_OPERATION_ACTION_PRINT_DIALOG, parent, &error);
    if (result_code == GTK_PRINT_OPERATION_RESULT_ERROR) {
        char *msg = g_strdup_printf("%s could not be printed.\n\n%s", font_viewer->title,
                                    error != nullptr ? error->message : "");
        xd_message_box(parent, "Font Viewer", XD_MB_ERROR, XD_MB_OK, FALSE, msg);
        g_free(msg);
    }
    g_clear_error(&error);
    g_object_unref(op);
    pango_font_description_free(job.ui);
}

void xd_font_print(GtkWindow *parent, const char *file, int index) {
    viewer *font_viewer = viewer_load(parent, file, index);
    if (font_viewer == nullptr)
        return;
    /* The details in the window's font, as the viewer shows them. */
    PangoFontDescription *ui = parent != nullptr
                                   ? pango_font_description_copy(widget_font(GTK_WIDGET(parent)))
                                   : pango_font_description_from_string("Sans 9");
    print(parent, font_viewer, ui);
    pango_font_description_free(ui);
    viewer_free(font_viewer);
}

/* ---- the window --------------------------------------------------------------------- */

static void on_done([[maybe_unused]] GtkButton *button, gpointer window) {
    gtk_widget_destroy(window);
}

static void on_print([[maybe_unused]] GtkButton *button, gpointer data) {
    viewer *font_viewer = data;
    print(GTK_WINDOW(font_viewer->window), font_viewer, widget_font(font_viewer->area));
}

GtkWidget *xd_font_viewer_new(GtkWindow *parent, const char *file, int index) {
    viewer *font_viewer = viewer_load(parent, file, index);
    if (font_viewer == nullptr)
        return nullptr;
    font_viewer->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(font_viewer->window), font_viewer->title);
    gtk_window_set_icon_name(GTK_WINDOW(font_viewer->window), "preferences-desktop-font");
    gtk_window_set_default_size(GTK_WINDOW(font_viewer->window), 680, 560);
    if (parent != nullptr)
        gtk_window_set_transient_for(GTK_WINDOW(font_viewer->window), parent);
    g_object_set_data_full(G_OBJECT(font_viewer->window), "xd-viewer", font_viewer, viewer_free);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(font_viewer->window), box);
    /* XP's two buttons over the page. */
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    GtkWidget *done = gtk_button_new_with_mnemonic("_Done");
    g_signal_connect(done, "clicked", G_CALLBACK(on_done), font_viewer->window);
    gtk_box_pack_start(GTK_BOX(bar), done, FALSE, FALSE, 0);
    GtkWidget *print_button = gtk_button_new_with_mnemonic("_Print...");
    g_signal_connect(print_button, "clicked", G_CALLBACK(on_print), font_viewer);
    gtk_box_pack_start(GTK_BOX(bar), print_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    font_viewer->area = gtk_drawing_area_new();
    g_signal_connect(font_viewer->area, "draw", G_CALLBACK(on_draw), font_viewer);
    g_signal_connect(font_viewer->area, "realize", G_CALLBACK(on_realize), font_viewer);
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroll), font_viewer->area);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(font_viewer->window);
    gtk_widget_grab_focus(done);
    return font_viewer->window;
}
