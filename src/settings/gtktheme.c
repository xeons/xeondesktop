/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <glib/gstdio.h>

#include "gtktheme.h"
#include "qtpalette.h"

static char *theme_dir(void) {
    return g_build_filename(g_get_user_data_dir(), "themes", XD_GTK_THEME_NAME, nullptr);
}

void xd_gtk_theme_css_path(xd_strbuf *out) {
    char *dir = theme_dir();
    char *css = g_build_filename(dir, "gtk-3.0", "gtk.css", nullptr);
    xd_strbuf_clear(out);
    xd_strbuf_append(out, xd_str_from_cstr(css));
    g_free(css);
    g_free(dir);
}

char *xd_gtk_theme_qt_palette_path(void) {
    char *dir = theme_dir();
    char *path = g_build_filename(dir, XD_QT_PALETTE_FILE, nullptr);
    g_free(dir);
    return path;
}

static bool write_file(const char *path, xd_str text, xd_strbuf *err) {
    GError *gerr = nullptr;
    /* g_file_set_contents writes a temporary file and renames it, so GTK
     * apps watching the file never read a half-written theme. */
    if (!g_file_set_contents(path, text.ptr, (gssize)text.len, &gerr)) {
        if (err != nullptr)
            xd_strbuf_appendf(err, "%s", gerr != nullptr ? gerr->message : path);
        g_clear_error(&gerr);
        return false;
    }
    return true;
}

/* Push buttons drawn by our own theme engine (the colours measured off
 * Windows XP), saved for GTK's border-image so GTK buttons look exactly
 * like ours. 75x23, Windows' dialog button, at 1x and 2x. */
static const struct {
    const char *name;
    uint8_t state;
} button_images[] = {
    {"button", XD_STATE_NORMAL},           {"button-hot", XD_STATE_HOT},
    {"button-pressed", XD_STATE_PRESSED},  {"button-default", XD_STATE_DEFAULT},
    {"button-disabled", XD_STATE_DISABLED},
};

static bool write_button_png(xd_visual_style style, const xd_scheme *scheme, const char *dir,
                             const char *name, uint8_t state, int scale, xd_strbuf *err) {
    xd_theme t;
    xd_theme_init(&t, style, scheme, scale);
    int w = 75 * scale;
    int h = 23 * scale;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surf);
    xd_theme_draw_button(&t, cr, (xd_rect){0, 0, w, h}, state, "");
    cairo_destroy(cr);
    char *file = scale == 1 ? g_strdup_printf("%s.png", name) : g_strdup_printf("%s@2.png", name);
    char *path = g_build_filename(dir, file, nullptr);
    char *tmp = g_strdup_printf("%s.tmp", path);
    /* Written aside and renamed, like the CSS. */
    bool ok = cairo_surface_write_to_png(surf, tmp) == CAIRO_STATUS_SUCCESS &&
              g_rename(tmp, path) == 0;
    if (!ok && err != nullptr)
        xd_strbuf_appendf(err, "cannot write %s", path);
    cairo_surface_destroy(surf);
    g_free(tmp);
    g_free(path);
    g_free(file);
    return ok;
}

bool xd_gtk_theme_install(xd_visual_style style, const xd_scheme *scheme, xd_strbuf *err) {
    char *dir = theme_dir();
    char *gtk3 = g_build_filename(dir, "gtk-3.0", nullptr);
    bool ok = g_mkdir_with_parents(gtk3, 0755) == 0;
    if (!ok && err != nullptr)
        xd_strbuf_appendf(err, "cannot create %s", gtk3);

    if (ok) {
        xd_theme t;
        xd_theme_init(&t, style, scheme, 1.0);
        /* Images first, so the new CSS never refers to missing files. */
        char *assets = g_build_filename(gtk3, "assets", nullptr);
        ok = g_mkdir_with_parents(assets, 0755) == 0;
        if (!ok && err != nullptr)
            xd_strbuf_appendf(err, "cannot create %s", assets);
        g_free(assets);
        for (int i = 0; ok && i < XD_GTK_ASSET_COUNT; i++) {
            xd_strbuf svg = {0};
            xd_theme_gtk_asset(&t, (xd_gtk_asset)i, &svg);
            char *path = g_build_filename(gtk3, xd_gtk_assets[i], nullptr);
            ok = write_file(path, xd_strbuf_view(&svg), err);
            g_free(path);
            xd_strbuf_free(&svg);
        }
        char *assets_dir = g_build_filename(gtk3, "assets", nullptr);
        for (size_t i = 0; ok && i < G_N_ELEMENTS(button_images); i++) {
            for (int scale = 1; ok && scale <= 2; scale++)
                ok = write_button_png(style, scheme, assets_dir, button_images[i].name,
                                      button_images[i].state, scale, err);
        }
        g_free(assets_dir);
        /* Qt programs read theirs at start, like the CSS. */
        if (ok) {
            xd_strbuf json = {0};
            xd_qt_palette_json(&t, &json);
            char *path = xd_gtk_theme_qt_palette_path();
            ok = write_file(path, xd_strbuf_view(&json), err);
            g_free(path);
            xd_strbuf_free(&json);
        }
        xd_strbuf css = {0};
        if (ok) {
            xd_theme_gtk_css(&t, &css);
            char *path = g_build_filename(gtk3, "gtk.css", nullptr);
            ok = write_file(path, xd_strbuf_view(&css), err);
            g_free(path);
        }
        xd_strbuf_free(&css);
    }
    if (ok) {
        static const char index_theme[] = "[Desktop Entry]\n"
                                          "Type=X-GNOME-Metatheme\n"
                                          "Name=" XD_GTK_THEME_NAME "\n"
                                          "Comment=Generated from the XeonDesktop appearance\n"
                                          "\n[X-GNOME-Metatheme]\n"
                                          "GtkTheme=" XD_GTK_THEME_NAME "\n";
        char *path = g_build_filename(dir, "index.theme", nullptr);
        ok = write_file(path, (xd_str){index_theme, sizeof(index_theme) - 1}, err);
        g_free(path);
    }
    g_free(gtk3);
    g_free(dir);
    return ok;
}
