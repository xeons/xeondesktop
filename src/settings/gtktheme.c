/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <glib/gstdio.h>

#include "gtktheme.h"

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
