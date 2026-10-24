/* SPDX-License-Identifier: MIT */
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "gtktheme.h"
#include "../theme/gtkpkg.h"
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

/* NAME.png at 1x, NAME@2.png at 2x, in `dir`. */
static bool write_png(cairo_surface_t *surf, const char *dir, const char *name, int scale,
                      xd_strbuf *err) {
    char *file = scale == 1 ? g_strdup_printf("%s.png", name) : g_strdup_printf("%s@2.png", name);
    char *path = g_build_filename(dir, file, nullptr);
    char *tmp = g_strdup_printf("%s.tmp", path);
    /* Written aside and renamed, like the CSS. */
    bool ok = cairo_surface_write_to_png(surf, tmp) == CAIRO_STATUS_SUCCESS &&
              g_rename(tmp, path) == 0;
    if (!ok && err != nullptr)
        xd_strbuf_appendf(err, "cannot write %s", path);
    g_free(tmp);
    g_free(path);
    g_free(file);
    return ok;
}

static bool write_button_png(xd_visual_style style, const xd_scheme *scheme, const char *dir,
                             const char *name, uint8_t state, int scale, xd_strbuf *err) {
    xd_theme theme;
    xd_theme_init(&theme, style, scheme, scale);
    int width = 75 * scale;
    int height = 23 * scale;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    cairo_t *cairo = cairo_create(surf);
    xd_theme_draw_button(&theme, cairo, (xd_rect){0, 0, width, height}, state, "");
    cairo_destroy(cairo);
    bool ok = write_png(surf, dir, name, scale, err);
    cairo_surface_destroy(surf);
    return ok;
}

/* Emptied first: another package's images are stale. An image the package
 * can't draw is left out (GTK then draws nothing there). */
bool xd_gtk_theme_package_images(xd_visual_style style, const xd_scheme *scheme,
                                 const char *gtk3, xd_strbuf *err) {
    xd_theme base;
    xd_theme_init(&base, style, scheme, 1.0);
    const xd_theme *theme = &base;
    char *dir = g_build_filename(gtk3, XD_GTK_PKG_ASSETS, nullptr);
    GDir *folder_handle = g_dir_open(dir, 0, nullptr);
    if (folder_handle != nullptr) {
        for (const char *file_name; (file_name = g_dir_read_name(folder_handle)) != nullptr;)
            if (g_str_has_suffix(file_name, ".png")) {
                char *path = g_build_filename(dir, file_name, nullptr);
                (void)g_unlink(path);
                g_free(path);
            }
        g_dir_close(folder_handle);
    }
    bool ok = true;
    if (theme->pkg != nullptr) {
        ok = g_mkdir_with_parents(dir, 0755) == 0;
        if (!ok && err != nullptr)
            xd_strbuf_appendf(err, "cannot create %s", dir);
        GArray *images = xd_gtk_pkg_images(theme);
        for (int scale = 1; ok && scale <= 2; scale++) {
            xd_theme ts;
            xd_theme_init(&ts, style, scheme, scale);
            for (guint i = 0; ok && i < images->len; i++) {
                const xd_gtk_pkg_image *img = &g_array_index(images, xd_gtk_pkg_image, i);
                cairo_surface_t *surf = xd_gtk_pkg_image_draw(&ts, img);
                if (surf == nullptr)
                    continue;
                ok = write_png(surf, dir, img->name, scale, err);
                cairo_surface_destroy(surf);
            }
        }
        g_array_unref(images);
    }
    g_free(dir);
    return ok;
}

/* The theme's own settings, which GTK reads with it: dialogs that say how
 * (message boxes among them) put the affirmative button first, Yes before
 * No and OK before Cancel, as Windows does. */
static bool write_settings_ini(const char *gtk3, xd_strbuf *err) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_boolean(kf, "Settings", "gtk-alternative-button-order", TRUE);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, nullptr);
    char *path = g_build_filename(gtk3, "settings.ini", nullptr);
    bool ok = write_file(path, (xd_str){data, len}, err);
    g_free(path);
    g_free(data);
    g_key_file_free(kf);
    return ok;
}

static bool write_index(const char *dir, const char *name, xd_strbuf *err) {
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_string(kf, "Desktop Entry", "Type", "X-GNOME-Metatheme");
    g_key_file_set_string(kf, "Desktop Entry", "Name", name);
    g_key_file_set_string(kf, "Desktop Entry", "Comment",
                          "Generated from the XeonDesktop appearance");
    g_key_file_set_string(kf, "X-GNOME-Metatheme", "GtkTheme", name);
    gsize len = 0;
    char *data = g_key_file_to_data(kf, &len, nullptr);
    char *path = g_build_filename(dir, "index.theme", nullptr);
    bool ok = write_file(path, (xd_str){data, len}, err);
    g_free(path);
    g_free(data);
    g_key_file_free(kf);
    return ok;
}

/* The same theme under a second name (its CSS imports ours), for
 * xd_gtk_theme_nudge to switch to and back. */
static bool install_alt(xd_strbuf *err) {
    char *dir = g_build_filename(g_get_user_data_dir(), "themes", XD_GTK_THEME_ALT_NAME, nullptr);
    char *gtk3 = g_build_filename(dir, "gtk-3.0", nullptr);
    bool ok = g_mkdir_with_parents(gtk3, 0755) == 0;
    if (!ok && err != nullptr)
        xd_strbuf_appendf(err, "cannot create %s", gtk3);
    if (ok) {
        static const char css[] = "@import url(\"../../" XD_GTK_THEME_NAME "/gtk-3.0/gtk.css\");\n";
        char *path = g_build_filename(gtk3, "gtk.css", nullptr);
        ok = write_file(path, (xd_str){css, sizeof(css) - 1}, err);
        g_free(path);
    }
    if (ok)
        ok = write_settings_ini(gtk3, err);
    if (ok)
        ok = write_index(dir, XD_GTK_THEME_ALT_NAME, err);
    g_free(gtk3);
    g_free(dir);
    return ok;
}

bool xd_gtk_theme_install(xd_visual_style style, const xd_scheme *scheme, bool *changed,
                          xd_strbuf *err) {
    if (changed != nullptr)
        *changed = false;
    char *dir = theme_dir();
    char *gtk3 = g_build_filename(dir, "gtk-3.0", nullptr);
    bool ok = g_mkdir_with_parents(gtk3, 0755) == 0;
    if (!ok && err != nullptr)
        xd_strbuf_appendf(err, "cannot create %s", gtk3);

    if (ok) {
        xd_theme theme;
        xd_theme_init(&theme, style, scheme, 1.0);
        /* Images first, so the new CSS never refers to missing files. */
        char *assets = g_build_filename(gtk3, "assets", nullptr);
        ok = g_mkdir_with_parents(assets, 0755) == 0;
        if (!ok && err != nullptr)
            xd_strbuf_appendf(err, "cannot create %s", assets);
        g_free(assets);
        for (int i = 0; ok && i < XD_GTK_ASSET_COUNT; i++) {
            xd_strbuf svg = {0};
            xd_theme_gtk_asset(&theme, (xd_gtk_asset)i, &svg);
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
        if (ok)
            ok = xd_gtk_theme_package_images(style, scheme, gtk3, err);
        /* Qt programs read theirs at start, like the CSS. */
        if (ok) {
            xd_strbuf json = {0};
            xd_qt_palette_json(&theme, &json);
            char *path = xd_gtk_theme_qt_palette_path();
            ok = write_file(path, xd_strbuf_view(&json), err);
            g_free(path);
            xd_strbuf_free(&json);
        }
        if (ok)
            ok = write_settings_ini(gtk3, err);
        xd_strbuf css = {0};
        if (ok) {
            xd_theme_gtk_css(&theme, &css);
            char *path = g_build_filename(gtk3, "gtk.css", nullptr);
            if (changed != nullptr) {
                char *old = nullptr;
                gsize len = 0;
                *changed = !g_file_get_contents(path, &old, &len, nullptr) ||
                           !xd_str_eq((xd_str){old, len}, xd_strbuf_view(&css));
                g_free(old);
            }
            ok = write_file(path, xd_strbuf_view(&css), err);
            g_free(path);
        }
        xd_strbuf_free(&css);
    }
    if (ok)
        ok = write_index(dir, XD_GTK_THEME_NAME, err);
    if (ok)
        ok = install_alt(err);
    g_free(gtk3);
    g_free(dir);
    return ok;
}

bool xd_gtk_theme_set_color_scheme(GSettingsBackend *backend, bool dark) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src != nullptr ? g_settings_schema_source_lookup(
                                                   src, "org.gnome.desktop.interface", TRUE)
                                             : nullptr;
    if (schema == nullptr)
        return false;
    bool done = false;
    if (g_settings_schema_has_key(schema, "color-scheme")) {
        GSettings *gsettings = g_settings_new_full(schema, backend, nullptr);
        const char *want = dark ? "prefer-dark" : "default";
        char *now = g_settings_get_string(gsettings, "color-scheme");
        if (strcmp(now, want) != 0) {
            done = g_settings_set_string(gsettings, "color-scheme", want);
            g_settings_sync();
        }
        g_free(now);
        g_object_unref(gsettings);
    }
    g_settings_schema_unref(schema);
    return done;
}

bool xd_gtk_theme_nudge(GSettingsBackend *backend) {
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src != nullptr ? g_settings_schema_source_lookup(
                                                   src, "org.gnome.desktop.interface", TRUE)
                                             : nullptr;
    if (schema == nullptr)
        return false;
    bool done = false;
    if (g_settings_schema_has_key(schema, "gtk-theme")) {
        GSettings *gsettings = g_settings_new_full(schema, backend, nullptr);
        GVariant *set = g_settings_get_user_value(gsettings, "gtk-theme");
        char *now = g_settings_get_string(gsettings, "gtk-theme");
        bool ours = strcmp(now, XD_GTK_THEME_NAME) == 0;
        /* A theme chosen there with other tools (GNOME Tweaks, nwg-look)
         * stays: only ours, or none at all, is switched. */
        if (set == nullptr || ours || strcmp(now, XD_GTK_THEME_ALT_NAME) == 0) {
            done = g_settings_set_string(gsettings, "gtk-theme",
                                         ours ? XD_GTK_THEME_ALT_NAME : XD_GTK_THEME_NAME);
            g_settings_sync();
        }
        g_free(now);
        if (set != nullptr)
            g_variant_unref(set);
        g_object_unref(gsettings);
    }
    g_settings_schema_unref(schema);
    return done;
}
