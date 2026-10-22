/* SPDX-License-Identifier: MIT
 * xd-desk: Display Properties.
 *
 * Tabs follow Windows 2000 (Background, Screen Saver, Appearance, Effects,
 * Settings) after XP's Themes. Changes are kept in `pending` (and the
 * sound scheme a theme brings in `pending_sounds`) and written to
 * desktop.ini (and sounds.ini) on Apply/OK; the compositor and shell pick
 * the file up immediately.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>

#include "../core/alloc.h"
#include "../saver/savers.h"
#include "../settings/settings.h"
#include "../settings/soundplay.h"
#include "../settings/themepkg.h"
#include "../settings/themes.h"
#include "../settings/themeset.h"
#include "../ui/image.h"
#include "../ui/ui.h"
#include "common.h"
#include "desk_advanced.h"
#include "desk_monitor.h"
#include "desk_nightlight.h"
#include "desk_saver.h"

enum {
    PAGE_THEMES,
    PAGE_BACKGROUND,
    PAGE_SCREENSAVER,
    PAGE_APPEARANCE,
    PAGE_EFFECTS,
    PAGE_SETTINGS,
    PAGE_LINUX
};

/* What xd-choose-file is choosing. */
typedef enum chooser_kind {
    CHOOSE_WALLPAPER,
    CHOOSE_ICON,
    CHOOSE_STYLE,
    CHOOSE_THEME,      /* the Themes tab's Browse... */
    CHOOSE_THEME_SAVE, /* ...and Save As... */
} chooser_kind;

/* What an item of the Themes tab's list stands for. */
typedef enum theme_kind {
    THEME_CURRENT, /* "My Current Theme", or the last one "(Modified)" */
    THEME_XP,      /* Windows XP: Luna, Default (blue), Bliss (ours: Rolling Hills) */
    THEME_CLASSIC, /* Windows Classic */
    THEME_PACKAGE,
    THEME_BROWSE,
} theme_kind;
typedef struct theme_item {
    theme_kind kind;
    guint package; /* THEME_PACKAGE: in `packages` */
} theme_item;

/* One monitor in the Settings tab. */
typedef struct disp_head {
    xd_display_setting set;
    char description[128];
    xd_display_mode *modes;
    size_t mode_count;
} disp_head;

typedef struct desk {
    xd_ui *ui;
    xd_window *win;
    xd_settings saved;
    xd_settings pending;
    xd_widget *apply;
    int page;
    /* The sound scheme (sounds.ini), which a theme may change, as loaded
     * and as pending. */
    xd_sound_scheme saved_sounds;
    xd_sound_scheme pending_sounds;

    /* Themes */
    xd_widget *theme_combo;
    xd_widget *theme_save;
    xd_widget *theme_delete;
    xd_widget *theme_preview;
    GArray *theme_items; /* theme_item, as the list shows them */
    bool theme_import;   /* Browse... is importing a .theme for the Themes tab */

    /* Background */
    char **wallpapers; /* paths; index 0 = (None) = nullptr */
    size_t wallpaper_count, wallpaper_cap;
    xd_widget *wallpaper_list;
    xd_widget *wallpaper_mode;
    xd_widget *bg_preview;
    cairo_surface_t *preview_image;
    int preview_full_w; /* the file's real width, for Center/Tile */
    char preview_path[1024];
    xd_widget *browse;
    xd_widget *customize;
    /* xd-choose-file, for Browse (a wallpaper or a visual style) and
     * Change Icon. GLib reaps the programs we start (GSubprocess). */
    GSubprocess *chooser;
    int chooser_fd; /* its stdout, the chooser's stream's */
    xd_ui_source *chooser_src;
    xd_strbuf chooser_out;
    chooser_kind choosing;

    /* Desktop Items (Customize Desktop...), edited apart from `pending`
     * until its OK. */
    xd_window *items;
    xd_desktop_icon_settings items_edit;
    xd_widget *items_checks[4];
    xd_widget *items_list;
    int items_sel; /* xd_desk_icon */
    xd_widget *change_icon;
    xd_widget *restore_icon;

    /* Screen Saver */
    xd_widget *saver_preview;
    xd_widget *saver_combo;
    xd_widget *saver_preview_btn;
    xd_widget *saver_settings_btn;
    saver_setup *saver_setup; /* its Settings dialog, while open */
    xd_widget *saver_password;
    xd_widget *saver_wait;
    int wait_values[24];
    size_t wait_count;
    xd_saver *preview_saver;
    char preview_saver_id[64];
    int preview_saver_w, preview_saver_h;
    struct timespec preview_last;
    xd_ui_source *anim;

    /* Appearance */
    xd_scheme_list schemes;
    /* Installed theme packages (xd_installed_package), listed after the
     * built-in styles in Windows and buttons; the pending one loaded for
     * the preview, and what it was loaded from. */
    GPtrArray *packages;
    xd_pkg *preview_pkg;
    char *preview_pkg_path;
    char *preview_pkg_variant;
    xd_widget *style_browse;
    /* xd-theme-import converting a visual style, while it runs: into
     * import_part, renamed to import_dest once it succeeded. */
    GSubprocess *import;
    int import_fd;
    xd_ui_source *import_src;
    xd_ui_source *import_wait;
    xd_strbuf import_out;
    char *import_source;
    char *import_part;
    char *import_dest;
    char *import_name;
    xd_widget *style_combo;
    xd_widget *scheme_combo;
    xd_widget *font_combo;
    xd_widget *appearance_preview;
    xd_widget *advanced;
    advanced *adv; /* Advanced Appearance, while open */
    nightlight_dialog *night; /* Night Light (Linux tab), while open */

    /* Window Color and Appearance (Aero), editing `pending` as it goes so
     * the preview follows; Cancel puts these back. */
    xd_window *color;
    uint32_t color_was;
    bool transparency_was;
    bool mixer_shown;
    xd_hsv mixer; /* the mixer's own values: a grey keeps its hue */
    xd_widget *swatches;
    xd_widget *color_name;
    xd_widget *transparency;
    xd_widget *intensity;
    xd_widget *mixer_toggle;
    xd_widget *mixer_widgets[9]; /* label, slider and gradient for each */

    /* Effects */
    xd_widget *transition_check;
    xd_widget *transition_combo;
    xd_widget *antialias_combo; /* Font smoothing */
    xd_widget *subpixel_combo;
    xd_widget *hinting_combo;
    xd_widget *scaling_combo; /* Scale theme images */
    xd_widget *large_icons;
    GPtrArray *icon_themes; /* xd_installed_theme, as the Icons list shows them */
    GPtrArray *gtk_themes;  /* ...and Program style's, after its first item */
    GPtrArray *qt_styles;   /* ...and the Qt one's */
    xd_widget *icon_combo;
    xd_widget *gtk_combo;
    xd_widget *qt_combo;

    /* Settings */
    disp_head *heads;       /* as edited */
    size_t head_count;
    disp_head *saved_heads; /* as applied and kept */
    size_t saved_count;
    bool display_dirty;
    int sel;
    xd_rect monitors_rect;
    xd_widget *monitors;
    xd_widget *display_combo;
    xd_widget *res_slider;
    xd_widget *res_label;
    xd_widget *scale_combo;
    xd_widget *refresh_combo;
    int refresh_modes[64];
    size_t refresh_count;
    xd_widget *orient_combo;
    xd_widget *extend_check;
    xd_widget *display_note;
    xd_widget *display_advanced;
    /* Advanced (desk_monitor.c), while it's open or about to: the
     * monitor's name and the refresh rates (modes) it was given. */
    monitor_dialog *monitor;
    bool monitor_probing; /* asking the compositor about variable refresh */
    char monitor_name[64];
    int monitor_modes[64];
    size_t monitor_mode_count;
    bool dragging;
    double drag_x0, drag_y0, drag_dx, drag_dy;
    /* Monitor Settings confirmation */
    xd_window *confirm;
    xd_widget *confirm_label;
    xd_ui_source *confirm_timer;
    int confirm_left;
    bool close_after_apply;
} desk;

/* ---- helpers --------------------------------------------------------------------- */

static bool sounds_equal(const xd_sound_scheme *left, const xd_sound_scheme *right) {
    if (left->base != right->base)
        return false;
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++) {
        if (g_strcmp0(left->sounds[i], right->sounds[i]) != 0)
            return false;
    }
    return true;
}

static void copy_sounds(xd_sound_scheme *to, const xd_sound_scheme *from) {
    xd_sound_scheme_free(to);
    xd_sound_scheme_init(to, from->base);
    for (size_t i = 0; i < XD_SOUND_EVENT_COUNT; i++)
        (void)xd_sound_scheme_set(to, (xd_sound_event)i, from->sounds[i]);
}

static void changed(desk *properties) {
    xd_widget_set_enabled(
        properties->apply,
        properties->display_dirty || !xd_settings_equal(&properties->saved, &properties->pending) ||
            !sounds_equal(&properties->saved_sounds, &properties->pending_sounds));
    xd_widget_redraw(properties->apply);
}

static void update_animation(desk *properties);
static void fill_theme_list(desk *properties);

static void on_page(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->page = xd_widget_selected(widget);
    update_animation(properties);
    /* What the other tabs changed shows as the theme "(Modified)". */
    if (properties->page == PAGE_THEMES && properties->theme_combo != nullptr) {
        fill_theme_list(properties);
        xd_widget_redraw(properties->theme_combo);
        xd_widget_redraw(properties->theme_preview);
    }
}

/* Pending appearance as a theme for previews: with the pending theme
 * package (loaded for it), not the one the desktop draws with now. None
 * under High Contrast, as the desktop does. */
static void pending_theme(const desk *properties, double scale, xd_theme *theme) {
    xd_scheme scheme;
    xd_settings_scheme(&properties->pending, &scheme);
    const xd_pkg *pkg =
        properties->pending.access.high_contrast ? nullptr : properties->preview_pkg;
    xd_theme_init_package(theme, properties->pending.style, &scheme, scale, pkg,
                          xd_settings_image_filter(&properties->pending, pkg),
                          properties->pending.integer_scaling);
}

/* A Windows 2000-style monitor: beige body, dark screen bezel, stand.
 * Returns the screen rect for the caller to fill. */
static xd_rect draw_monitor(cairo_t *cairo, xd_rect rect, const xd_theme *theme, double aspect) {
    int px = xd_tpx(theme, 1);
    int stand_h = xd_tpx(theme, 18);
    xd_rect body = {rect.x, rect.y, rect.w, rect.h - stand_h};
    /* Keep the screen's aspect ratio: shrink the body to fit. */
    int bezel = xd_tpx(theme, 10);
    int sw = body.w - 2 * bezel;
    int sh = (int)lround(sw / aspect);
    if (sh > body.h - 2 * bezel) {
        sh = body.h - 2 * bezel;
        sw = (int)lround(sh * aspect);
    }
    body.w = sw + 2 * bezel;
    body.h = sh + 2 * bezel;
    body.x = rect.x + (rect.w - body.w) / 2;

    xd_rgba face = xd_rgba_hex(0xD4D0C8);
    xd_rgba light = xd_rgba_hex(0xFFFFFF);
    xd_rgba shadow = xd_rgba_hex(0x808080);
    xd_rgba dark = xd_rgba_hex(0x404040);
    /* Stand: neck and foot. */
    int neck_w = body.w / 5;
    xd_rect neck = {body.x + (body.w - neck_w) / 2, body.y + body.h, neck_w, stand_h / 2};
    xd_rect foot = {body.x + body.w / 4, neck.y + neck.h, body.w / 2, stand_h / 2};
    xd_fill_rect(cairo, neck, face);
    xd_draw_bevel(cairo, neck, px, light, dark);
    xd_fill_rect(cairo, foot, face);
    xd_draw_bevel(cairo, foot, px, light, dark);

    xd_fill_rect(cairo, body, face);
    xd_rect in = xd_draw_bevel(cairo, body, px, light, dark);
    xd_draw_bevel(cairo, in, px, face, shadow);
    xd_rect screen = {body.x + bezel, body.y + bezel, sw, sh};
    xd_draw_bevel(cairo, xd_rect_inset(screen, -px, -px), px, shadow, light);
    /* Power light. */
    xd_fill_rect(cairo,
                 (xd_rect){body.x + body.w - bezel - xd_tpx(theme, 4),
                           body.y + body.h - bezel / 2 - px, xd_tpx(theme, 3), xd_tpx(theme, 2)},
                 xd_rgba_hex(0x30C030));
    return screen;
}

static double primary_aspect(desk *properties) {
    const xd_output_info *outs = nullptr;
    size_t count = xd_ui_outputs(properties->ui, &outs);
    if (count > 0 && outs[0].logical_w > 0 && outs[0].logical_h > 0)
        return (double)outs[0].logical_w / outs[0].logical_h;
    return 4.0 / 3.0;
}

/* ---- Background ----------------------------------------------------------------- */

static bool is_image(xd_str name) {
    static const char *const exts[] = {".jpg", ".jpeg", ".png", ".webp", ".bmp"};
    for (size_t i = 0; i < XD_ARRAY_LEN(exts); i++) {
        xd_str extension = xd_str_from_cstr(exts[i]);
        if (name.len > extension.len &&
            xd_str_eq_nocase(xd_str_sub(name, name.len - extension.len, extension.len), extension))
            return true;
    }
    return false;
}

static void add_wallpaper(desk *properties, const char *path) {
    for (size_t i = 1; i < properties->wallpaper_count; i++) {
        if (strcmp(properties->wallpapers[i], path) == 0)
            return;
    }
    if (properties->wallpaper_count == properties->wallpaper_cap) {
        properties->wallpaper_cap =
            properties->wallpaper_cap == 0 ? 64 : properties->wallpaper_cap * 2;
        properties->wallpapers =
            xd_realloc_array(properties->wallpapers, properties->wallpaper_cap, sizeof(char *));
    }
    properties->wallpapers[properties->wallpaper_count++] = xd_str_dup_cstr(xd_str_from_cstr(path));
}

/* Scans a wallpaper directory (two levels: themes keep images in subdirs). */
static void scan_wallpapers(desk *properties, const char *dir, int depth) {
    DIR *dh = opendir(dir);
    if (dh == nullptr)
        return;
    struct dirent *de;
    xd_strbuf path = {0};
    while ((de = readdir(dh)) != nullptr) {
        xd_str name = xd_str_from_cstr(de->d_name);
        if (name.len == 0 || name.ptr[0] == '.')
            continue;
        xd_strbuf_clear(&path);
        xd_strbuf_appendf(&path, "%s/%s", dir, de->d_name);
        struct stat st;
        if (stat(xd_strbuf_cstr(&path), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && depth < 2)
            scan_wallpapers(properties, xd_strbuf_cstr(&path), depth + 1);
        else if (S_ISREG(st.st_mode) && is_image(name))
            add_wallpaper(properties, xd_strbuf_cstr(&path));
    }
    xd_strbuf_free(&path);
    closedir(dh);
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

static int compare_wallpapers(const void *left, const void *right) {
    return strcoll(base_name(*(char *const *)left), base_name(*(char *const *)right));
}

static void load_wallpapers(desk *properties) {
    add_wallpaper(properties, ""); /* (None) */
    static const char *const dirs[] = {"/usr/share/backgrounds", "/usr/share/wallpapers",
                                       "/usr/local/share/backgrounds"};
    for (size_t i = 0; i < XD_ARRAY_LEN(dirs); i++)
        scan_wallpapers(properties, dirs[i], 0);
    const char *home = getenv("HOME");
    if (home != nullptr) {
        xd_strbuf folder_path = {0};
        xd_strbuf_appendf(&folder_path, "%s/Pictures/Wallpapers", home);
        scan_wallpapers(properties, xd_strbuf_cstr(&folder_path), 1);
        xd_strbuf_free(&folder_path);
    }
    /* Keep the current one listed even if it lives elsewhere. */
    if (properties->pending.wallpaper[0] != '\0')
        add_wallpaper(properties, properties->pending.wallpaper);
    if (properties->wallpaper_count > 2)
        qsort(properties->wallpapers + 1, properties->wallpaper_count - 1, sizeof(char *),
              compare_wallpapers);
}

/* The pending wallpaper over the desktop colour, as the desktop paints
 * it, into `screen` (the Background and Themes tabs' previews). */
static void paint_pending_desktop(desk *properties, cairo_t *cairo, xd_rect screen,
                                  const xd_theme *pt) {
    xd_fill_rect(cairo, screen, xd_sys(pt, XD_COLOR_BACKGROUND));
    if (strcmp(properties->preview_path, properties->pending.wallpaper) != 0) {
        if (properties->preview_image != nullptr)
            cairo_surface_destroy(properties->preview_image);
        properties->preview_image = properties->pending.wallpaper[0] != '\0'
                                        ? xd_image_load(properties->pending.wallpaper, 480, 360)
                                        : nullptr;
        int full_h;
        if (!xd_image_size(properties->pending.wallpaper, &properties->preview_full_w, &full_h))
            properties->preview_full_w = 0;
        snprintf(properties->preview_path, sizeof(properties->preview_path), "%s",
                 properties->pending.wallpaper);
    }
    /* Center and Tile show the picture at its real size relative to the
     * screen; the preview copy was loaded smaller, so scale it back up. */
    const xd_output_info *outs = nullptr;
    double scale = 0.25;
    if (xd_ui_outputs(properties->ui, &outs) > 0 && outs[0].mode_w > 0)
        scale = (double)screen.w / outs[0].mode_w;
    if (properties->preview_image != nullptr && properties->preview_full_w > 0)
        scale *= (double)properties->preview_full_w /
                 cairo_image_surface_get_width(properties->preview_image);
    xd_image_paint_wallpaper(cairo, properties->preview_image, screen,
                             properties->pending.wallpaper_mode, scale);
}

static void draw_bg_preview(xd_widget *widget, cairo_t *cairo, xd_rect rect, const xd_theme *theme,
                            void *user) {
    (void)widget;
    desk *properties = user;
    xd_rect screen = draw_monitor(cairo, rect, theme, primary_aspect(properties));
    xd_theme pt;
    pending_theme(properties, theme->scale, &pt);
    paint_pending_desktop(properties, cairo, screen, &pt);
}

static void on_wallpaper(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i < 0 || (size_t)i >= properties->wallpaper_count)
        return;
    snprintf(properties->pending.wallpaper, sizeof(properties->pending.wallpaper), "%s",
             properties->wallpapers[i]);
    xd_widget_redraw(properties->bg_preview);
    changed(properties);
}

static void on_wallpaper_mode(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && i < XD_WALLPAPER_MODE_COUNT)
        properties->pending.wallpaper_mode = (xd_wallpaper_mode)i;
    xd_widget_redraw(properties->bg_preview);
    changed(properties);
}

/* Selects a picture by path, adding it to the list if it's new. */
static void choose_wallpaper(desk *properties, const char *path) {
    size_t before = properties->wallpaper_count;
    add_wallpaper(properties, path);
    if (properties->wallpaper_count > before)
        xd_widget_add_item(properties->wallpaper_list, base_name(path));
    for (size_t i = 0; i < properties->wallpaper_count; i++) {
        if (strcmp(properties->wallpapers[i], path) == 0)
            xd_widget_select(properties->wallpaper_list, (int)i);
    }
    snprintf(properties->pending.wallpaper, sizeof(properties->pending.wallpaper), "%s", path);
    xd_widget_redraw(properties->bg_preview);
    changed(properties);
}

static void items_chose_icon(desk *properties, const char *path);
static void items_update_buttons(desk *properties);
static void style_chosen(desk *properties, const char *path);
static void update_style_browse(desk *properties);
static void theme_save_to(desk *properties, const char *path);
static void update_theme_buttons(desk *properties);

/* `program` beside this one (a build tree, or the install's bindir), else
 * from PATH, with `args`. A GSubprocess, so that GLib reaps it. */
static GSubprocess *spawn_own(const char *program, const char *const *args,
                              GSubprocessFlags flags, GError **error) {
    char *exe = nullptr;
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    if (self != nullptr) {
        char *dir = g_path_get_dirname(self);
        exe = g_build_filename(dir, program, nullptr);
        g_free(dir);
        g_free(self);
        if (!g_file_test(exe, G_FILE_TEST_IS_EXECUTABLE))
            g_clear_pointer(&exe, g_free);
    }
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, exe != nullptr ? exe : (char *)(uintptr_t)program);
    for (size_t i = 0; args != nullptr && args[i] != nullptr; i++)
        g_ptr_array_add(argv, (char *)(uintptr_t)args[i]);
    g_ptr_array_add(argv, nullptr);
    GSubprocess *sp =
        g_subprocess_newv((const char *const *)(void *)argv->pdata, flags, error);
    g_ptr_array_free(argv, TRUE);
    g_free(exe);
    return sp;
}

/* Starts it and forgets it. */
static void launch_own(const char *program, const char *const *args) {
    GError *error = nullptr;
    GSubprocess *sp = spawn_own(program, args, G_SUBPROCESS_FLAGS_NONE, &error);
    if (sp == nullptr) {
        fprintf(stderr, "xd-desk: %s\n", error->message);
        g_error_free(error);
        return;
    }
    g_object_unref(sp);
}

/* The read end of a subprocess's stdout, to watch in libxdui's loop. */
static int stdout_fd(GSubprocess *sp) {
    return g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(g_subprocess_get_stdout_pipe(sp)));
}

static void close_stdout(GSubprocess **sp) {
    g_input_stream_close(g_subprocess_get_stdout_pipe(*sp), nullptr, nullptr);
    g_clear_object(sp);
}

static void chooser_output(void *user) {
    desk *properties = user;
    char buf[1024];
    ssize_t bytes_read = read(properties->chooser_fd, buf, sizeof(buf));
    if (bytes_read > 0) {
        xd_strbuf_append(&properties->chooser_out, (xd_str){buf, (size_t)bytes_read});
        return;
    }
    if (bytes_read < 0 && errno == EINTR)
        return;
    /* The chooser closed: its answer is complete. */
    xd_ui_remove_source(properties->chooser_src);
    properties->chooser_src = nullptr;
    close_stdout(&properties->chooser);
    properties->chooser_fd = -1;
    xd_str path = xd_str_trim(xd_strbuf_view(&properties->chooser_out));
    xd_widget_set_enabled(properties->browse, true);
    update_style_browse(properties);
    if (path.len > 0) {
        char *chosen_path = xd_str_dup_cstr(path);
        if (properties->choosing == CHOOSE_WALLPAPER) {
            choose_wallpaper(properties, chosen_path);
        } else if (properties->choosing == CHOOSE_STYLE) {
            style_chosen(properties, chosen_path);
        } else if (properties->choosing == CHOOSE_THEME) {
            /* Chosen as a theme once it's installed (select_installed). */
            properties->theme_import = true;
            style_chosen(properties, chosen_path);
            /* Unless an import goes on in the background, it's done (or
             * failed with a message). */
            if (properties->import == nullptr)
                properties->theme_import = false;
        } else if (properties->choosing == CHOOSE_THEME_SAVE) {
            theme_save_to(properties, chosen_path);
        } else if (properties->items != nullptr) {
            items_chose_icon(properties, chosen_path);
        }
        xd_free(chosen_path);
    }
    /* The Themes list leaves Browse... for what it shows now. */
    if (properties->choosing == CHOOSE_THEME || properties->choosing == CHOOSE_THEME_SAVE) {
        fill_theme_list(properties);
        xd_widget_redraw(properties->theme_combo);
    }
    update_theme_buttons(properties);
    xd_strbuf_clear(&properties->chooser_out);
    if (properties->items != nullptr)
        items_update_buttons(properties);
}

/* Runs xd-choose-file; its answer comes to chooser_output. */
static bool start_chooser(desk *properties, const char *const *args, chooser_kind what) {
    if (properties->chooser_src != nullptr)
        return false;
    GError *error = nullptr;
    properties->chooser = spawn_own("xd-choose-file", args, G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error);
    if (properties->chooser == nullptr) {
        fprintf(stderr, "xd-desk: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    properties->chooser_fd = stdout_fd(properties->chooser);
    properties->chooser_src =
        xd_ui_add_fd(properties->ui, properties->chooser_fd, chooser_output, properties);
    properties->choosing = what;
    xd_widget_set_enabled(properties->browse, false);
    update_style_browse(properties);
    update_theme_buttons(properties);
    if (properties->items != nullptr)
        items_update_buttons(properties);
    return true;
}

static void on_browse([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    char folder[1024] = "/usr/share/backgrounds";
    const char *home = getenv("HOME");
    if (home != nullptr)
        snprintf(folder, sizeof(folder), "%s/Pictures", home);
    const char *args[] = {"--images", "--title", "Browse", "--folder", folder, nullptr};
    start_chooser(properties, args, CHOOSE_WALLPAPER);
}

/* ---- Desktop Items --------------------------------------------------------------- */

/* XP's Customize Desktop...: which special icons the desktop shows and
 * their pictures. OK puts the choices into the property sheet, which
 * Apply saves, as in XP. The Web tab (Active Desktop) and Desktop
 * Cleanup aren't there. */

static constexpr int ITEMS_LIST_W = 304;
static constexpr int ITEMS_LIST_H = 98;

static const char *const item_labels[XD_DESK_ICON_COUNT] = {
    [XD_DESK_ICON_COMPUTER] = "My Computer",
    [XD_DESK_ICON_DOCUMENTS] = "My Documents",
    [XD_DESK_ICON_NETWORK] = "My Network Places",
    [XD_DESK_ICON_TRASH_FULL] = "Recycle Bin (full)",
    [XD_DESK_ICON_TRASH_EMPTY] = "Recycle Bin (empty)",
};

/* My Network Places needs GIO's network:// (gvfs); the shell leaves the
 * icon out without it, so its box is greyed here. */
static bool network_available(void) {
    const gchar *const *schemes = g_vfs_get_supported_uri_schemes(g_vfs_get_default());
    return schemes != nullptr && g_strv_contains(schemes, "network");
}

static void items_update_buttons(desk *properties) {
    bool custom = properties->items_edit.icons[properties->items_sel][0] != '\0';
    xd_widget_set_enabled(properties->change_icon, properties->chooser_src == nullptr);
    xd_widget_set_enabled(properties->restore_icon, custom);
}

static void items_chose_icon(desk *properties, const char *path) {
    char (*slot)[sizeof(properties->items_edit.icons[0])] =
        &properties->items_edit.icons[properties->items_sel];
    if (path[0] == '/' && strlen(path) < sizeof(*slot))
        snprintf(*slot, sizeof(*slot), "%s", path);
    xd_widget_redraw(properties->items_list);
    items_update_buttons(properties);
}

/* The icons in a row, as XP's list view: each picture over its name, the
 * selected one highlighted. */
static void draw_items_list([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect bounds,
                            const xd_theme *theme, void *user) {
    desk *properties = user;
    xd_rect in = xd_theme_draw_field(theme, cairo, bounds, false);
    xd_field_colors fc = xd_theme_field_colors(theme);
    int cell = in.w / XD_DESK_ICON_COUNT;
    int px = xd_tpx(theme, 32);
    for (int i = 0; i < XD_DESK_ICON_COUNT; i++) {
        xd_rect cell_rect = {in.x + i * cell, in.y, cell, in.h};
        const char *names[2];
        size_t count = 0;
        if (properties->items_edit.icons[i][0] != '\0')
            names[count++] = properties->items_edit.icons[i];
        names[count++] = xd_desk_icon_default((xd_desk_icon)i);
        cairo_surface_t *img = xd_icons_get_any(xd_ui_icons(properties->ui), names, count, px);
        int ix = cell_rect.x + (cell - px) / 2, iy = cell_rect.y + xd_tpx(theme, 6);
        bool sel = i == properties->items_sel;
        if (img != nullptr) {
            cairo_set_source_surface(cairo, img, ix, iy);
            cairo_paint(cairo);
            if (sel) {
                cairo_set_source_rgba(cairo, fc.selection.r, fc.selection.g, fc.selection.b, 0.5);
                cairo_mask_surface(cairo, img, ix, iy);
            }
        }
        xd_text_style st =
            xd_text(theme, XD_FONT_ICON, sel ? fc.selection_text : fc.text, XD_ALIGN_CENTER);
        PangoLayout *layout = xd_text_layout(cairo, item_labels[i], &st);
        int text_w = cell - xd_tpx(theme, 4);
        pango_layout_set_width(layout, text_w * PANGO_SCALE);
        pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
        pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
        PangoRectangle ink, logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        int tx = cell_rect.x + xd_tpx(theme, 2), ty = iy + px + xd_tpx(theme, 4);
        if (sel) {
            xd_fill_rect(cairo,
                         (xd_rect){tx + logical.x - xd_tpx(theme, 1), ty,
                                   logical.width + xd_tpx(theme, 2), logical.height},
                         fc.selection);
        }
        cairo_set_source_rgba(cairo, st.color.r, st.color.g, st.color.b, st.color.a);
        cairo_move_to(cairo, tx, ty);
        pango_cairo_show_layout(cairo, layout);
        g_object_unref(layout);
    }
}

static void on_items_pointer(xd_widget *widget, xd_pointer_event ev, double x,
                             [[maybe_unused]] double y, void *user) {
    desk *properties = user;
    if (ev != XD_POINTER_PRESS)
        return;
    double column = floor(x / (ITEMS_LIST_W / (double)XD_DESK_ICON_COUNT));
    properties->items_sel = XD_CLAMP((int)column, 0, XD_DESK_ICON_COUNT - 1);
    xd_widget_redraw(widget);
    items_update_buttons(properties);
}

static void on_change_icon([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    /* Windows lists the icons in a file; here any picture will do. */
    const char *args[] = {"--images", "--title", "Change Icon", "--folder", "/usr/share/icons",
                          nullptr};
    start_chooser(properties, args, CHOOSE_ICON);
}

static void on_restore_icon([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    properties->items_edit.icons[properties->items_sel][0] = '\0';
    xd_widget_redraw(properties->items_list);
    items_update_buttons(properties);
}

static void items_close(desk *properties) {
    if (properties->items == nullptr)
        return;
    xd_window *items = properties->items;
    properties->items = nullptr;
    xd_window_close(items);
    xd_widget_set_enabled(properties->customize, true);
}

static void on_items_ok([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    xd_desktop_icon_settings *desktop_icon_settings = &properties->items_edit;
    desktop_icon_settings->documents = xd_checkbox_checked(properties->items_checks[0]);
    desktop_icon_settings->computer = xd_checkbox_checked(properties->items_checks[1]);
    desktop_icon_settings->network = xd_checkbox_checked(properties->items_checks[2]);
    desktop_icon_settings->browser = xd_checkbox_checked(properties->items_checks[3]);
    properties->pending.desktop_icons = *desktop_icon_settings;
    items_close(properties);
    changed(properties);
}

static void on_items_cancel([[maybe_unused]] xd_widget *widget, void *user) { items_close(user); }

static void on_customize([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->items != nullptr)
        return;
    properties->items_edit = properties->pending.desktop_icons;
    properties->items_sel = XD_DESK_ICON_COMPUTER;
    xd_window *win = properties->items =
        xd_window_create(properties->ui, "Desktop Items", "xd-desk", 362, 404);
    static const char *const tabs[] = {"General"};
    xd_tabs(win, (xd_rect){7, 7, 348, 358}, tabs, XD_ARRAY_LEN(tabs), nullptr, nullptr);

    xd_groupbox(win, (xd_rect){17, 35, 328, 210}, "Desktop icons");
    const xd_desktop_icon_settings *desktop_icon_settings = &properties->items_edit;
    properties->items_checks[0] =
        xd_checkbox(win, (xd_rect){29, 55, 145, 18}, "My Documents",
                    desktop_icon_settings->documents, nullptr, properties);
    properties->items_checks[1] = xd_checkbox(win, (xd_rect){29, 75, 145, 18}, "My Computer",
                                              desktop_icon_settings->computer, nullptr, properties);
    properties->items_checks[2] = xd_checkbox(win, (xd_rect){180, 55, 155, 18}, "My Network Places",
                                              desktop_icon_settings->network, nullptr, properties);
    xd_widget_set_enabled(properties->items_checks[2], network_available());
    /* XP's Internet Explorer: here the default web browser, by its name. */
    GAppInfo *browser = g_app_info_get_default_for_uri_scheme("http");
    properties->items_checks[3] =
        xd_checkbox(win, (xd_rect){180, 75, 155, 18},
                    browser != nullptr ? g_app_info_get_name(browser) : "Web Browser",
                    desktop_icon_settings->browser, nullptr, properties);
    xd_widget_set_enabled(properties->items_checks[3], browser != nullptr);
    g_clear_object(&browser);
    properties->items_list =
        xd_custom(win, (xd_rect){29, 101, ITEMS_LIST_W, ITEMS_LIST_H}, draw_items_list, properties);
    xd_custom_set_pointer(properties->items_list, on_items_pointer);
    properties->change_icon =
        xd_button(win, (xd_rect){157, 209, 85, 23}, "Change Icon...", on_change_icon, properties);
    properties->restore_icon =
        xd_button(win, (xd_rect){248, 209, 85, 23}, "Restore Default", on_restore_icon, properties);

    xd_groupbox(win, (xd_rect){17, 253, 328, 102}, "Desktop cleanup");
    xd_label(win, (xd_rect){29, 273, 304, 16},
             "Desktop cleanup moves unused desktop items to a folder.", XD_LABEL_WRAP);
    xd_widget_set_enabled(xd_checkbox(win, (xd_rect){29, 297, 304, 18},
                                      "Run Desktop Cleanup Wizard every 60 days", false, nullptr,
                                      nullptr),
                          false);
    xd_widget_set_enabled(
        xd_button(win, (xd_rect){219, 323, 114, 23}, "Clean Desktop Now", nullptr, nullptr),
        false);

    xd_widget *ok = xd_button(win, (xd_rect){199, 373, 75, 23}, "OK", on_items_ok, properties);
    xd_widget *cancel =
        xd_button(win, (xd_rect){280, 373, 75, 23}, "Cancel", on_items_cancel, properties);
    xd_window_set_default(win, ok);
    xd_window_set_cancel(win, cancel);
    xd_window_set_focus(win, properties->items_checks[0]);
    items_update_buttons(properties);
    xd_widget_set_enabled(properties->customize, false);
    xd_window_show(win);
}

static void build_background(desk *properties) {
    xd_window *win = properties->win;
    properties->bg_preview =
        xd_custom(win, (xd_rect){105, 42, 194, 166}, draw_bg_preview, properties);
    xd_widget_set_page(properties->bg_preview, PAGE_BACKGROUND);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 218, 360, 16},
                                "Select a picture to use as wallpaper:", 0),
                       PAGE_BACKGROUND);
    properties->wallpaper_list =
        xd_listbox(win, (xd_rect){20, 236, 268, 132}, on_wallpaper, properties);
    xd_widget_set_page(properties->wallpaper_list, PAGE_BACKGROUND);
    int current = 0;
    for (size_t i = 0; i < properties->wallpaper_count; i++) {
        const char *wallpaper = properties->wallpapers[i];
        xd_widget_add_item(properties->wallpaper_list,
                           wallpaper[0] == '\0' ? "(None)" : base_name(wallpaper));
        if (strcmp(wallpaper, properties->pending.wallpaper) == 0)
            current = (int)i;
    }
    xd_widget_select(properties->wallpaper_list, current);

    properties->browse =
        xd_button(win, (xd_rect){298, 236, 86, 23}, "Browse...", on_browse, properties);
    xd_widget_set_page(properties->browse, PAGE_BACKGROUND);
    xd_widget_set_page(xd_label(win, (xd_rect){298, 330, 86, 16}, "Picture Display:", 0),
                       PAGE_BACKGROUND);
    properties->wallpaper_mode =
        xd_combo(win, (xd_rect){298, 348, 86, 21}, false, on_wallpaper_mode, properties);
    xd_widget_set_page(properties->wallpaper_mode, PAGE_BACKGROUND);
    static const char *const modes[] = {"Center", "Tile", "Stretch", "Fill", "Fit"};
    for (size_t i = 0; i < XD_ARRAY_LEN(modes); i++)
        xd_widget_add_item(properties->wallpaper_mode, modes[i]);
    xd_widget_select(properties->wallpaper_mode, (int)properties->pending.wallpaper_mode);
    /* XP's Desktop tab: under the list. */
    properties->customize = xd_button(win, (xd_rect){20, 376, 120, 23}, "Customize Desktop...",
                                      on_customize, properties);
    xd_widget_set_page(properties->customize, PAGE_BACKGROUND);
}

/* ---- running helpers ---------------------------------------------------------- */

/* One of our programs: next to this one (a build tree), else from PATH.
 * With out_fd, its stdout comes back through a pipe. */

static double seconds_since(struct timespec *last) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double dt = last->tv_sec == 0 ? 0.0
                                  : (double)(now.tv_sec - last->tv_sec) +
                                        (double)(now.tv_nsec - last->tv_nsec) / 1e9;
    *last = now;
    return dt;
}

/* ---- Screen Saver ------------------------------------------------------------------ */

static const char *pending_saver(const desk *properties) {
    return xd_saver_find(properties->pending.screensaver) != nullptr
               ? properties->pending.screensaver
               : "";
}

static void draw_saver_preview(xd_widget *widget, cairo_t *cairo, xd_rect rect,
                               const xd_theme *theme, void *user) {
    (void)widget;
    desk *properties = user;
    xd_rect screen = draw_monitor(cairo, rect, theme, primary_aspect(properties));
    const char *id = pending_saver(properties);
    if (id[0] == '\0') {
        /* (None): the desktop as it is. */
        xd_theme pt;
        pending_theme(properties, theme->scale, &pt);
        xd_fill_rect(cairo, screen, xd_sys(&pt, XD_COLOR_BACKGROUND));
        return;
    }
    if (properties->preview_saver == nullptr || strcmp(properties->preview_saver_id, id) != 0 ||
        properties->preview_saver_w != screen.w || properties->preview_saver_h != screen.h) {
        if (properties->preview_saver != nullptr)
            xd_saver_destroy(properties->preview_saver);
        /* A miniature: sized as if the real screen were shrunk to fit. */
        double scale = fmax(0.2, theme->scale * screen.w / 1024.0);
        properties->preview_saver =
            xd_saver_create(id, &properties->pending.savers, screen.w, screen.h, scale);
        snprintf(properties->preview_saver_id, sizeof(properties->preview_saver_id), "%s", id);
        properties->preview_saver_w = screen.w;
        properties->preview_saver_h = screen.h;
        properties->preview_last = (struct timespec){0};
    }
    if (properties->preview_saver == nullptr)
        return;
    cairo_save(cairo);
    cairo_rectangle(cairo, screen.x, screen.y, screen.w, screen.h);
    cairo_clip(cairo);
    cairo_translate(cairo, screen.x, screen.y);
    xd_saver_frame(properties->preview_saver, cairo, seconds_since(&properties->preview_last));
    cairo_restore(cairo);
}

static void animate(void *user) {
    desk *properties = user;
    xd_widget_redraw(properties->saver_preview);
}

/* The preview runs only while its page shows. */
static void update_animation(desk *properties) {
    bool want =
        properties->page == PAGE_SCREENSAVER && xd_saver_animated(pending_saver(properties));
    if (want && properties->anim == nullptr)
        properties->anim = xd_ui_add_timer(properties->ui, 50, true, animate, properties);
    else if (!want && properties->anim != nullptr) {
        xd_ui_remove_source(properties->anim);
        properties->anim = nullptr;
    }
}

static const int wait_minutes[] = {1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 90, 120, 180, 240};
static const int monitor_minutes[] = {0, 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300};

static void saver_controls(desk *properties) {
    bool on = pending_saver(properties)[0] != '\0';
    xd_widget_set_enabled(properties->saver_settings_btn,
                          properties->saver_setup == nullptr &&
                              xd_saver_has_settings(pending_saver(properties)));
    xd_widget_set_enabled(properties->saver_preview_btn, on);
    xd_widget_set_enabled(properties->saver_password, on);
    xd_widget_set_enabled(properties->saver_wait, on);
}

static void on_saver(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    const xd_saver_info *savers = nullptr;
    size_t count = xd_savers(&savers);
    if (i <= 0 || (size_t)(i - 1) >= count)
        properties->pending.screensaver[0] = '\0';
    else
        snprintf(properties->pending.screensaver, sizeof(properties->pending.screensaver), "%s",
                 savers[i - 1].id);
    saver_controls(properties);
    update_animation(properties);
    xd_widget_redraw(properties->saver_preview);
    changed(properties);
}

static void on_saver_password(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.screensaver_password = xd_checkbox_checked(widget);
    changed(properties);
}

static void on_saver_wait(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && (size_t)i < properties->wait_count)
        properties->pending.screensaver_wait = properties->wait_values[i];
    changed(properties);
}

static void on_monitor_off(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && (size_t)i < XD_ARRAY_LEN(monitor_minutes))
        properties->pending.monitor_off = monitor_minutes[i];
    changed(properties);
}

/* Like Windows' savers, whose Settings wrote their own registry keys on
 * OK: saved at once, apart from the rest of Display Properties. */
static void on_saver_setup_done(const xd_saver_settings *sv, void *user) {
    desk *properties = user;
    properties->saver_setup = nullptr;
    if (sv != nullptr) {
        xd_settings now;
        xd_settings_load(&now);
        now.savers = *sv;
        if (!xd_settings_save(&now))
            fprintf(stderr, "xd-desk: cannot write settings\n");
        properties->saved.savers = properties->pending.savers = *sv;
        /* The preview starts again with them. */
        if (properties->preview_saver != nullptr)
            xd_saver_destroy(properties->preview_saver);
        properties->preview_saver = nullptr;
        xd_widget_redraw(properties->saver_preview);
    }
    saver_controls(properties);
    xd_window_set_focus(properties->win, properties->saver_settings_btn);
    changed(properties);
}

static void on_saver_settings([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->saver_setup != nullptr)
        return;
    properties->saver_setup =
        saver_setup_open(properties->ui, pending_saver(properties), &properties->pending.savers,
                         on_saver_setup_done, properties);
    saver_controls(properties);
}

static void on_saver_preview_button([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    const char *id = pending_saver(properties);
    if (id[0] == '\0')
        return;
    /* Preview runs unprotected, like Windows' Preview button. */
    const char *args[] = {"--saver", id, "--no-lock", nullptr};
    launch_own("xd-lock", args);
}

static const char *minutes_label(int delay_minutes, char *buf, size_t cap) {
    if (delay_minutes == 0)
        snprintf(buf, cap, "Never");
    else if (delay_minutes < 60)
        snprintf(buf, cap, "After %d min%s", delay_minutes, delay_minutes == 1 ? "" : "s");
    else
        snprintf(buf, cap, "After %d hour%s", delay_minutes / 60, delay_minutes == 60 ? "" : "s");
    return buf;
}

static void build_screensaver(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_SCREENSAVER;
    properties->saver_preview =
        xd_custom(win, (xd_rect){105, 42, 194, 166}, draw_saver_preview, properties);
    xd_widget_set_page(properties->saver_preview, page);
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 216, 364, 92}, "Screen Saver"), page);
    properties->saver_combo =
        xd_combo(win, (xd_rect){32, 238, 170, 21}, false, on_saver, properties);
    xd_widget_set_page(properties->saver_combo, page);
    xd_widget_add_item(properties->saver_combo, "(None)");
    const xd_saver_info *savers = nullptr;
    size_t count = xd_savers(&savers);
    int current = 0;
    for (size_t i = 0; i < count; i++) {
        xd_widget_add_item(properties->saver_combo, savers[i].name);
        if (strcmp(savers[i].id, properties->pending.screensaver) == 0)
            current = (int)i + 1;
    }
    xd_widget_select(properties->saver_combo, current);

    properties->saver_settings_btn =
        xd_button(win, (xd_rect){210, 237, 80, 23}, "Settings", on_saver_settings, properties);
    xd_widget_set_page(properties->saver_settings_btn, page);
    properties->saver_preview_btn =
        xd_button(win, (xd_rect){296, 237, 80, 23}, "Preview", on_saver_preview_button, properties);
    xd_widget_set_page(properties->saver_preview_btn, page);
    properties->saver_password =
        xd_checkbox(win, (xd_rect){32, 272, 150, 20}, "Password protected",
                    properties->pending.screensaver_password, on_saver_password, properties);
    xd_widget_set_page(properties->saver_password, page);

    xd_widget_set_page(xd_label(win, (xd_rect){202, 274, 32, 16}, "Wait:", 0), page);
    properties->saver_wait =
        xd_combo(win, (xd_rect){236, 271, 82, 21}, false, on_saver_wait, properties);
    xd_widget_set_page(properties->saver_wait, page);
    /* The usual choices, plus whatever desktop.ini has. */
    properties->wait_count = 0;
    bool listed = false;
    for (size_t i = 0; i < XD_ARRAY_LEN(wait_minutes); i++) {
        if (!listed && wait_minutes[i] > properties->pending.screensaver_wait) {
            properties->wait_values[properties->wait_count++] =
                properties->pending.screensaver_wait;
            listed = true;
        }
        if (wait_minutes[i] == properties->pending.screensaver_wait)
            listed = true;
        properties->wait_values[properties->wait_count++] = wait_minutes[i];
    }
    if (!listed)
        properties->wait_values[properties->wait_count++] = properties->pending.screensaver_wait;
    int wait_sel = 0;
    for (size_t i = 0; i < properties->wait_count; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", properties->wait_values[i]);
        xd_widget_add_item(properties->saver_wait, buf);
        if (properties->wait_values[i] == properties->pending.screensaver_wait)
            wait_sel = (int)i;
    }
    xd_widget_select(properties->saver_wait, wait_sel);
    xd_widget_set_page(xd_label(win, (xd_rect){324, 274, 56, 16}, "minutes", 0), page);

    xd_widget_set_page(
        xd_groupbox(win, (xd_rect){20, 316, 364, 64}, "Energy saving features of monitor"), page);
    xd_widget_set_page(
        xd_image(win, (xd_rect){32, 338, 32, 32}, "preferences-system-power-management"), page);
    xd_widget_set_page(xd_label(win, (xd_rect){74, 342, 110, 16}, "Turn off monitor:", 0), page);
    xd_widget *monitor =
        xd_combo(win, (xd_rect){186, 339, 150, 21}, false, on_monitor_off, properties);
    xd_widget_set_page(monitor, page);
    int mon_sel = 0;
    int best = INT32_MAX;
    for (size_t i = 0; i < XD_ARRAY_LEN(monitor_minutes); i++) {
        char buf[32];
        xd_widget_add_item(monitor, minutes_label(monitor_minutes[i], buf, sizeof(buf)));
        int diff = abs(monitor_minutes[i] - properties->pending.monitor_off);
        if (diff < best) {
            best = diff;
            mon_sel = (int)i;
        }
    }
    xd_widget_select(monitor, mon_sel);
    saver_controls(properties);
}

/* ---- Appearance ---------------------------------------------------------------- */

static void draw_appearance_preview(xd_widget *widget, cairo_t *cairo, xd_rect rect,
                                    const xd_theme *theme, void *user) {
    (void)widget;
    desk *properties = user;
    xd_theme pt;
    pending_theme(properties, theme->scale, &pt);
    int px = xd_tpx(theme, 1);

    /* Sunken well like Windows' preview box. */
    xd_rect well = xd_theme_draw_field(theme, cairo, rect, false);
    xd_fill_rect(cairo, well, xd_sys(&pt, XD_COLOR_BACKGROUND));

    /* Inactive window behind, active window in front, message box on top. */
    xd_frame inactive = {.title = "Inactive Window",
                         .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_rect r1 = {well.x + xd_tpx(theme, 10), well.y + xd_tpx(theme, 8), well.w - xd_tpx(theme, 60),
                  xd_tpx(theme, 90)};
    xd_theme_draw_frame(&pt, cairo, r1, &inactive);
    xd_insets in1 = xd_theme_frame_insets(&pt, &inactive);
    xd_fill_rect(cairo, xd_rect_inset4(r1, in1.left, in1.top, in1.right, in1.bottom),
                 xd_theme_dialog_color(&pt));

    xd_frame active = {.title = "Active Window",
                       .active = true,
                       .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_rect r2 = {well.x + xd_tpx(theme, 24), well.y + xd_tpx(theme, 36),
                  well.w - xd_tpx(theme, 48), well.h - xd_tpx(theme, 44)};
    xd_theme_draw_frame(&pt, cairo, r2, &active);
    xd_insets in2 = xd_theme_frame_insets(&pt, &active);
    xd_rect client = xd_rect_inset4(r2, in2.left, in2.top, in2.right, in2.bottom);
    xd_fill_rect(cairo, client, xd_theme_dialog_color(&pt));
    xd_rect text_area = xd_rect_inset(client, xd_tpx(&pt, 6), xd_tpx(&pt, 6));
    xd_rect field = {text_area.x, text_area.y, text_area.w / 2, xd_tpx(&pt, 40)};
    xd_rect field_in = xd_theme_draw_field(&pt, cairo, field, false);
    xd_text_style st = xd_text(&pt, XD_FONT_MESSAGE, xd_sys(&pt, XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cairo, xd_rect_inset4(field_in, xd_tpx(&pt, 3), 0, 0, 0), "Window Text", &st);

    xd_frame box = {.title = "Message Box", .active = true, .dialog = true,
                    .buttons = XD_CAPBTN_CLOSE};
    xd_insets ib = xd_theme_frame_insets(&pt, &box);
    int bw = xd_tpx(&pt, 150);
    int bh = ib.top + ib.bottom + xd_tpx(&pt, 52);
    xd_rect r3 = {client.x + client.w - bw - xd_tpx(&pt, 8),
                  XD_MIN(client.y + xd_tpx(&pt, 8), well.y + well.h - bh - px), bw, bh};
    xd_theme_draw_frame(&pt, cairo, r3, &box);
    xd_rect bc = xd_rect_inset4(r3, ib.left, ib.top, ib.right, ib.bottom);
    xd_fill_rect(cairo, bc, xd_theme_dialog_color(&pt));
    xd_draw_text(cairo,
                 (xd_rect){bc.x + xd_tpx(&pt, 8), bc.y + xd_tpx(&pt, 4), bc.w, xd_tpx(&pt, 16)},
                 "Message Text", &st);
    int okw = xd_tpx(&pt, 60);
    xd_theme_draw_button(
        &pt, cairo,
        (xd_rect){bc.x + (bc.w - okw) / 2, bc.y + xd_tpx(&pt, 24), okw, xd_tpx(&pt, 22)},
        XD_STATE_DEFAULT, "OK");
}

/* Which Windows XP style colour variant installed scheme `i` is, if any. */
static xd_luna_color scheme_luna_color(const desk *properties, size_t i) {
    xd_settings probe;
    xd_settings_defaults(&probe);
    snprintf(probe.scheme, sizeof(probe.scheme), "%s", properties->schemes.stems[i]);
    xd_scheme scheme;
    xd_settings_scheme(&probe, &scheme);
    return scheme.luna_color;
}

/* As in XP, the Color scheme list holds the colour variants of Windows XP
 * style (Default (blue), Olive Green, Silver) or, for the other styles, the
 * classic schemes after the built-in Windows Standard. */
static bool scheme_listed(const desk *properties, size_t i) {
    return (scheme_luna_color(properties, i) != XD_LUNA_NONE) ==
           (properties->pending.style == XD_STYLE_LUNA);
}

/* The installed scheme shown as combo item `item`, or -1 for the built-in
 * Windows Standard. */
static ptrdiff_t listed_scheme(const desk *properties, int item) {
    int count = properties->pending.style == XD_STYLE_LUNA ? 0 : 1;
    for (size_t i = 0; i < properties->schemes.count; i++) {
        if (scheme_listed(properties, i) && count++ == item)
            return (ptrdiff_t)i;
    }
    return -1;
}

/* ---- theme packages: Windows and buttons, and their variants ------------------------- */

/* The pending package, if it's installed. */
static const xd_installed_package *pending_package(const desk *properties) {
    if (properties->pending.theme_package[0] == '\0')
        return nullptr;
    for (guint i = 0; i < properties->packages->len; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        if (strcmp(package->name, properties->pending.theme_package) == 0)
            return package;
    }
    return nullptr;
}

/* Its pending variant: ThemeVariant's, else its first, the default. */
static const xd_pkg_variant_info *pending_variant(const desk *properties,
                                                  const xd_installed_package *package) {
    for (size_t i = 0; i < package->info.variant_count; i++) {
        if (strcmp(package->info.variants[i].id, properties->pending.theme_variant) == 0)
            return &package->info.variants[i];
    }
    return &package->info.variants[0];
}

/* Loads the pending package for the preview, unless it has been. */
static void update_preview_package(desk *properties) {
    const xd_installed_package *package = pending_package(properties);
    const char *path = package != nullptr ? package->path : nullptr;
    const char *variant = package != nullptr ? pending_variant(properties, package)->id : nullptr;
    if (g_strcmp0(path, properties->preview_pkg_path) == 0 &&
        g_strcmp0(variant, properties->preview_pkg_variant) == 0)
        return;
    g_clear_pointer(&properties->preview_pkg, xd_pkg_free);
    g_free(properties->preview_pkg_path);
    g_free(properties->preview_pkg_variant);
    properties->preview_pkg_path = g_strdup(path);
    properties->preview_pkg_variant = g_strdup(variant);
    if (package == nullptr)
        return;
    char *names = xd_theme_names_path();
    GError *error = nullptr;
    properties->preview_pkg = xd_pkg_load(path, variant, names, &error);
    g_free(names);
    if (properties->preview_pkg == nullptr) {
        /* The preview shows its base style. */
        fprintf(stderr, "xd-desk: %s\n", error->message);
        g_error_free(error);
    }
}

/* Its colours, as Color scheme lists them: each variant name once, in
 * theme.xml's order. `out` holds variant_count. */
static size_t package_colors(const xd_installed_package *package, const char **out) {
    size_t count = 0;
    for (size_t i = 0; i < package->info.variant_count; i++) {
        bool seen = false;
        for (size_t k = 0; k < count && !seen; k++)
            seen = strcmp(out[k], package->info.variants[i].name) == 0;
        if (!seen)
            out[count++] = package->info.variants[i].name;
    }
    return count;
}

/* The sizes colour `color` comes in, as Font size lists them. */
static size_t package_sizes(const xd_installed_package *package, const char *color,
                            const char **out) {
    size_t count = 0;
    for (size_t i = 0; i < package->info.variant_count; i++) {
        if (strcmp(package->info.variants[i].name, color) == 0)
            out[count++] = package->info.variants[i].size;
    }
    return count;
}

/* Colour `color` in `size`, else in its first size. */
static const xd_pkg_variant_info *find_variant(const xd_installed_package *package,
                                               const char *color, const char *size) {
    const xd_pkg_variant_info *any = nullptr;
    for (size_t i = 0; i < package->info.variant_count; i++) {
        const xd_pkg_variant_info *variant = &package->info.variants[i];
        if (strcmp(variant->name, color) != 0)
            continue;
        if (strcmp(variant->size, size) == 0)
            return variant;
        if (any == nullptr)
            any = variant;
    }
    return any;
}

/* Chooses the variant; its size, if it's one of ours, sets the scheme's
 * font size too, for what the package leaves to the scheme. */
static void set_variant(desk *properties, const xd_pkg_variant_info *variant) {
    snprintf(properties->pending.theme_variant, sizeof(properties->pending.theme_variant), "%s",
             variant->id);
    static const char *const sizes[] = {"Normal", "Large Fonts", "Extra Large Fonts"};
    for (size_t i = 0; i < XD_ARRAY_LEN(sizes); i++) {
        if (g_ascii_strcasecmp(variant->size, sizes[i]) == 0 &&
            (xd_font_size)i != properties->pending.font_size) {
            properties->pending.font_size = (xd_font_size)i;
            properties->pending.custom_scheme = (xd_scheme_custom){0};
        }
    }
}

/* Windows and buttons: the built-in styles, then the installed packages
 * by name. */
static void fill_styles(desk *properties) {
    xd_widget_clear_items(properties->style_combo);
    for (int i = 0; i < XD_STYLE_COUNT; i++)
        xd_widget_add_item(properties->style_combo, xd_visual_style_name((xd_visual_style)i));
    int current = (int)properties->pending.style;
    for (guint i = 0; i < properties->packages->len; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        const char *name = package->info.name[0] != '\0' ? package->info.name : package->name;
        /* Told apart by their file names where they share a name with a
         * built-in style (7's is "Aero" too) or another package. */
        bool shared = false;
        for (int k = 0; k < XD_STYLE_COUNT && !shared; k++)
            shared = g_ascii_strcasecmp(name, xd_visual_style_name((xd_visual_style)k)) == 0;
        for (guint k = 0; k < properties->packages->len && !shared; k++) {
            const xd_installed_package *other = g_ptr_array_index(properties->packages, k);
            shared = k != i && strcmp(name, other->info.name[0] != '\0' ? other->info.name
                                                                        : other->name) == 0;
        }
        char *label = shared ? g_strdup_printf("%s (%s)", name, package->name) : g_strdup(name);
        xd_widget_add_item(properties->style_combo, label);
        g_free(label);
        if (strcmp(package->name, properties->pending.theme_package) == 0)
            current = XD_STYLE_COUNT + (int)i;
    }
    xd_widget_select(properties->style_combo, current);
}

static void fill_schemes(desk *properties) {
    xd_widget_clear_items(properties->scheme_combo);
    const xd_installed_package *pkg = pending_package(properties);
    if (pkg != nullptr) {
        /* A package's colours, as XP lists a visual style's. */
        const char **colors = g_new(const char *, pkg->info.variant_count);
        size_t count = package_colors(pkg, colors);
        const char *current = pending_variant(properties, pkg)->name;
        for (size_t i = 0; i < count; i++) {
            xd_widget_add_item(properties->scheme_combo, colors[i]);
            if (strcmp(colors[i], current) == 0)
                xd_widget_select(properties->scheme_combo, (int)i);
        }
        g_free(colors);
        return;
    }
    bool luna = properties->pending.style == XD_STYLE_LUNA;
    if (!luna)
        xd_widget_add_item(properties->scheme_combo, "Windows Standard (built in)");
    int count = luna ? 0 : 1;
    int current = 0;
    for (size_t i = 0; i < properties->schemes.count; i++) {
        if (!scheme_listed(properties, i))
            continue;
        xd_widget_add_item(properties->scheme_combo, properties->schemes.names[i]);
        /* A classic scheme left under Windows XP style shows as Default
         * (blue), which is what its frames look like. */
        if (strcmp(properties->schemes.stems[i], properties->pending.scheme) == 0 ||
            (luna && current == 0 && scheme_luna_color(properties, i) == XD_LUNA_BLUE))
            current = count;
        count++;
    }
    xd_widget_select(properties->scheme_combo, current);
}

static const char *const font_sizes[] = {"Normal", "Large Fonts", "Extra Large Fonts"};

static void fill_font_sizes(desk *properties) {
    xd_widget_clear_items(properties->font_combo);
    const xd_installed_package *pkg = pending_package(properties);
    if (pkg != nullptr) {
        /* The sizes the package's colour comes in. */
        const xd_pkg_variant_info *variant = pending_variant(properties, pkg);
        const char **sizes = g_new(const char *, pkg->info.variant_count);
        size_t count = package_sizes(pkg, variant->name, sizes);
        for (size_t i = 0; i < count; i++) {
            xd_widget_add_item(properties->font_combo, sizes[i]);
            if (strcmp(sizes[i], variant->size) == 0)
                xd_widget_select(properties->font_combo, (int)i);
        }
        g_free(sizes);
        return;
    }
    for (size_t i = 0; i < XD_ARRAY_LEN(font_sizes); i++)
        xd_widget_add_item(properties->font_combo, font_sizes[i]);
    xd_widget_select(properties->font_combo, (int)properties->pending.font_size);
}

/* The look of the preview and the lists below Windows and buttons changed. */
static void appearance_changed(desk *properties) {
    update_preview_package(properties);
    fill_schemes(properties);
    fill_font_sizes(properties);
    xd_widget_redraw(properties->scheme_combo);
    xd_widget_redraw(properties->font_combo);
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

static void set_style(desk *properties, xd_visual_style style) {
    bool was_luna = properties->pending.style == XD_STYLE_LUNA;
    properties->pending.style = style;
    /* Switching between Windows XP style and the others switches scheme
     * lists, and the scheme to the new list's first: Default (blue), or
     * Windows Standard. */
    if (was_luna != (properties->pending.style == XD_STYLE_LUNA)) {
        properties->pending.scheme[0] = '\0';
        properties->pending.custom_scheme = (xd_scheme_custom){0};
        ptrdiff_t first = listed_scheme(properties, 0);
        if (properties->pending.style == XD_STYLE_LUNA && first >= 0)
            snprintf(properties->pending.scheme, sizeof(properties->pending.scheme), "%s",
                     properties->schemes.stems[first]);
    }
}

static void on_style(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i < 0)
        return;
    if (i < XD_STYLE_COUNT) {
        if (properties->pending.theme_package[0] == '\0' &&
            (xd_visual_style)i == properties->pending.style)
            return;
        properties->pending.theme_package[0] = '\0';
        properties->pending.theme_variant[0] = '\0';
        set_style(properties, (xd_visual_style)i);
    } else {
        if ((guint)(i - XD_STYLE_COUNT) >= properties->packages->len)
            return;
        const xd_installed_package *package =
            g_ptr_array_index(properties->packages, (guint)(i - XD_STYLE_COUNT));
        if (strcmp(package->name, properties->pending.theme_package) == 0)
            return;
        /* Its base style draws what it doesn't, and is what the desktop's
         * own choices (the Start menu, the taskbar's) go by. */
        snprintf(properties->pending.theme_package, sizeof(properties->pending.theme_package), "%s",
                 package->name);
        set_style(properties, package->info.base);
        set_variant(properties, &package->info.variants[0]);
    }
    appearance_changed(properties);
}

static void on_scheme(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i < 0)
        return;
    const xd_installed_package *pkg = pending_package(properties);
    if (pkg != nullptr) {
        const char **colors = g_new(const char *, pkg->info.variant_count);
        size_t count = package_colors(pkg, colors);
        if ((size_t)i < count)
            set_variant(properties,
                        find_variant(pkg, colors[i], pending_variant(properties, pkg)->size));
        g_free(colors);
        appearance_changed(properties);
        return;
    }
    ptrdiff_t k = listed_scheme(properties, i);
    /* A scheme sets every item, as in Windows: Advanced's changes go. */
    properties->pending.custom_scheme = (xd_scheme_custom){0};
    if (k < 0)
        properties->pending.scheme[0] = '\0';
    else
        snprintf(properties->pending.scheme, sizeof(properties->pending.scheme), "%s",
                 properties->schemes.stems[k]);
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

static void on_font_size(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    const xd_installed_package *pkg = pending_package(properties);
    if (pkg != nullptr) {
        const char *color = pending_variant(properties, pkg)->name;
        const char **sizes = g_new(const char *, pkg->info.variant_count);
        size_t count = package_sizes(pkg, color, sizes);
        if (i >= 0 && (size_t)i < count)
            set_variant(properties, find_variant(pkg, color, sizes[i]));
        g_free(sizes);
        appearance_changed(properties);
        return;
    }
    if (i >= 0 && i <= XD_FONT_EXTRA_LARGE && (xd_font_size)i != properties->pending.font_size) {
        /* Windows' Large and Extra Large are schemes of their own. */
        properties->pending.font_size = (xd_font_size)i;
        properties->pending.custom_scheme = (xd_scheme_custom){0};
    }
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

/* ---- Browse...: a visual style or a theme package ---------------------------------- */

/* Windows' error box, owned by Display Properties. */
static void message_ok(xd_widget *widget, [[maybe_unused]] void *user) {
    xd_window_close(xd_widget_window(widget));
}

static void error_box(desk *properties, const char *text) {
    /* Lines the label wraps to, roughly (an 8pt font's average width). */
    glong chars = g_utf8_strlen(text, -1);
    int lines = 1 + (int)((double)chars * 5.4 / 312);
    for (const char *cursor = text; *cursor != '\0'; cursor++)
        lines += *cursor == '\n';
    int text_h = XD_MAX(lines * 14, 32);
    int height = 14 + text_h + 16 + 23 + 12;
    xd_window *box = xd_window_create(properties->ui, "Display Properties", "xd-desk", 380, height);
    xd_image(box, (xd_rect){12, 14, 32, 32}, "dialog-error");
    xd_label(box, (xd_rect){56, 14, 312, text_h}, text, XD_LABEL_WRAP);
    xd_widget *ok =
        xd_button(box, (xd_rect){152, height - 12 - 23, 75, 23}, "OK", message_ok, nullptr);
    xd_window_set_default(box, ok);
    xd_window_set_cancel(box, ok);
    xd_window_set_focus(box, ok);
    xd_window_show(box);
    xd_sound_play_event(XD_SOUND_CRITICAL_STOP);
}

static void update_style_browse(desk *properties) {
    if (properties->style_browse != nullptr)
        xd_widget_set_enabled(properties->style_browse,
                              properties->chooser_src == nullptr && properties->import == nullptr);
    update_theme_buttons(properties);
}

static void theme_choose_installed(desk *properties, const char *name);

/* After installing package `name`: the list read again, and it chosen. */
static void select_installed(desk *properties, const char *name) {
    char **dirs = xd_theme_package_dirs();
    g_ptr_array_unref(properties->packages);
    properties->packages = xd_theme_packages_list((const char *const *)dirs);
    g_strfreev(dirs);
    /* Read again even if it's the pending one: it may be a new version. */
    g_clear_pointer(&properties->preview_pkg_path, g_free);
    if (strcmp(properties->pending.theme_package, name) == 0)
        properties->pending.theme_package[0] = '\0';
    fill_styles(properties);
    for (guint i = 0; i < properties->packages->len; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        if (strcmp(package->name, name) == 0) {
            xd_widget_select(properties->style_combo, XD_STYLE_COUNT + (int)i);
            on_style(properties->style_combo, properties);
        }
    }
    xd_widget_redraw(properties->style_combo);
    appearance_changed(properties);
    if (properties->theme_import) {
        /* Browse... in the Themes tab: chosen as a theme, with its
         * wallpaper, sounds and cursors. */
        properties->theme_import = false;
        theme_choose_installed(properties, name);
    }
    fill_theme_list(properties);
    xd_widget_redraw(properties->theme_combo);
}

/* The name to install `stem` under in `dir`: the stem, unless a folder of
 * that name is there (it would win over the zip): then "stem (2)"... */
static char *install_name(const char *dir, const char *stem) {
    const char *trimmed = stem;
    while (*trimmed == '.')
        trimmed++;
    char *name = g_strdup(*trimmed != '\0' ? trimmed : "Visual style");
    for (int count = 2; count < 100; count++) {
        char *folder = g_build_filename(dir, name, nullptr);
        bool taken = g_file_test(folder, G_FILE_TEST_IS_DIR);
        g_free(folder);
        if (!taken)
            break;
        g_free(name);
        name = g_strdup_printf("%s (%d)", *trimmed != '\0' ? trimmed : "Visual style", count);
    }
    return name;
}

/* A package chosen with Browse...: copied into the user's themes, unless
 * it's already one of the installed ones. */
static void install_package(desk *properties, const char *path, const char *stem) {
    GError *error = nullptr;
    xd_pkg_info info;
    if (!xd_pkg_read_info(path, &info, &error)) {
        char *text = g_strdup_printf("\"%s\" is not a theme package.\n\n%s", stem,
                                     error->message);
        error_box(properties, text);
        g_free(text);
        g_error_free(error);
        return;
    }
    xd_pkg_info_clear(&info);
    char **dirs = xd_theme_package_dirs();
    char *found = xd_theme_package_find((const char *const *)dirs, stem);
    g_strfreev(dirs);
    char *real_found = found != nullptr ? realpath(found, nullptr) : nullptr;
    char *real_path = realpath(path, nullptr);
    bool installed = real_found != nullptr && real_path != nullptr &&
                     strcmp(real_found, real_path) == 0;
    free(real_found);
    free(real_path);
    g_free(found);
    if (installed) {
        select_installed(properties, stem);
        return;
    }
    char *dir = xd_theme_package_user_dir();
    char *name = install_name(dir, stem);
    char *file = g_strconcat(name, ".xdtheme", nullptr);
    char *dest = g_build_filename(dir, file, nullptr);
    char *part_file = g_strconcat(".", file, ".part", nullptr);
    char *part = g_build_filename(dir, part_file, nullptr);
    GFile *src = g_file_new_for_path(path);
    GFile *tmp = g_file_new_for_path(part);
    /* Into place in one step: the desktop may be reading the old one. */
    bool ok = g_mkdir_with_parents(dir, 0755) == 0 &&
              g_file_copy(src, tmp, G_FILE_COPY_OVERWRITE, nullptr, nullptr, nullptr, &error) &&
              g_rename(part, dest) == 0;
    if (ok) {
        select_installed(properties, name);
    } else {
        (void)g_remove(part);
        char *text = g_strdup_printf("\"%s\" cannot be installed in %s.\n\n%s", stem, dir,
                                     error != nullptr ? error->message : g_strerror(errno));
        error_box(properties, text);
        g_free(text);
    }
    g_clear_error(&error);
    g_object_unref(src);
    g_object_unref(tmp);
    g_free(part);
    g_free(part_file);
    g_free(dest);
    g_free(file);
    g_free(name);
    g_free(dir);
}

/* xd-theme-import: beside this program (installed), in a source tree's
 * tools/ (a build tree), else from PATH. */
static char *importer_path(void) {
    char *self = g_file_read_link("/proc/self/exe", nullptr);
    if (self != nullptr) {
        char *dir = g_path_get_dirname(self);
        char *path = g_build_filename(dir, "xd-theme-import", nullptr);
        g_free(dir);
        g_free(self);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }
    xd_strbuf data = {0};
    if (xd_data_dir(&data)) {
        char *path = g_build_filename(xd_strbuf_cstr(&data), "..", "tools", "xd-theme-import",
                                      nullptr);
        xd_strbuf_free(&data);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE))
            return path;
        g_free(path);
    }
    xd_strbuf_free(&data);
    return g_strdup("xd-theme-import");
}

static void import_finished(desk *properties, bool ok) {
    xd_ui_set_busy(properties->ui, false);
    GError *error = nullptr;
    xd_pkg_info info;
    if (ok && xd_pkg_read_info(properties->import_part, &info, &error)) {
        xd_pkg_info_clear(&info);
        ok = g_rename(properties->import_part, properties->import_dest) == 0;
    } else {
        ok = false;
    }
    if (ok) {
        select_installed(properties, properties->import_name);
    } else {
        properties->theme_import = false;
        (void)g_remove(properties->import_part);
        /* xd-theme-import says why on its last line ("needs pefile..."). */
        char **lines = g_strsplit(xd_strbuf_cstr(&properties->import_out), "\n", -1);
        const char *why = nullptr;
        for (char **line = lines; *line != nullptr; line++) {
            g_strstrip(*line);
            if (**line != '\0')
                why = *line;
        }
        if (why != nullptr && g_str_has_prefix(why, "xd-theme-import: "))
            why += strlen("xd-theme-import: ");
        /* The file's name is in the box already. */
        size_t source_len = strlen(properties->import_source);
        if (why != nullptr && strncmp(why, properties->import_source, source_len) == 0 &&
            strncmp(why + source_len, ": ", 2) == 0)
            why += source_len + 2;
        if (why == nullptr)
            why = error != nullptr ? error->message : "The conversion failed.";
        char *base = g_path_get_basename(properties->import_source);
        /* As a sentence: "Not a visual style." */
        char *reason = g_strdup(why);
        reason[0] = g_ascii_toupper(reason[0]);
        bool stop = g_str_has_suffix(reason, ".") || g_str_has_suffix(reason, ")");
        /* "Not a visual style" would only say the first line again. */
        bool same = g_ascii_strcasecmp(reason, "Not a visual style") == 0;
        char *text = g_strdup_printf("\"%s\" cannot be used as a visual style.%s%s%s", base,
                                     same ? "" : "\n\n", same ? "" : reason,
                                     same || stop ? "" : ".");
        error_box(properties, text);
        g_free(text);
        g_free(reason);
        g_free(base);
        g_strfreev(lines);
    }
    g_clear_error(&error);
    xd_strbuf_clear(&properties->import_out);
    g_clear_pointer(&properties->import_source, g_free);
    g_clear_pointer(&properties->import_part, g_free);
    g_clear_pointer(&properties->import_dest, g_free);
    g_clear_pointer(&properties->import_name, g_free);
    update_style_browse(properties);
}

static void import_exited(GObject *source, GAsyncResult *res, gpointer user) {
    desk *properties = user;
    GSubprocess *sp = G_SUBPROCESS(source);
    bool ok = g_subprocess_wait_finish(sp, res, nullptr) && g_subprocess_get_successful(sp);
    xd_ui_remove_source(properties->import_wait);
    properties->import_wait = nullptr;
    g_clear_object(&properties->import);
    import_finished(properties, ok);
}

/* GLib's main context, for g_subprocess_wait_async's answer: libxdui's
 * loop isn't GLib's. */
static void import_pump([[maybe_unused]] void *user) {
    for (int i = 0; i < 16 && g_main_context_iteration(nullptr, FALSE); i++) {
    }
}

static void import_output(void *user) {
    desk *properties = user;
    char buf[1024];
    ssize_t bytes_read = read(properties->import_fd, buf, sizeof(buf));
    if (bytes_read > 0) {
        xd_strbuf_append(&properties->import_out, (xd_str){buf, (size_t)bytes_read});
        return;
    }
    if (bytes_read < 0 && errno == EINTR)
        return;
    /* Its output is complete; it exits. */
    xd_ui_remove_source(properties->import_src);
    properties->import_src = nullptr;
    g_input_stream_close(g_subprocess_get_stdout_pipe(properties->import), nullptr, nullptr);
    properties->import_fd = -1;
    g_subprocess_wait_async(properties->import, nullptr, import_exited, properties);
    properties->import_wait = xd_ui_add_timer(properties->ui, 50, true, import_pump, properties);
}

/* Converts a .msstyles or .theme with xd-theme-import, in the background,
 * into the user's themes. */
static void start_import(desk *properties, const char *path, const char *stem) {
    char *dir = xd_theme_package_user_dir();
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        char *text = g_strdup_printf("Display Properties cannot create %s.\n\n%s", dir,
                                     g_strerror(errno));
        error_box(properties, text);
        g_free(text);
        g_free(dir);
        return;
    }
    properties->import_name = install_name(dir, stem);
    char *file = g_strconcat(properties->import_name, ".xdtheme", nullptr);
    char *part = g_strconcat(".", file, ".part", nullptr);
    properties->import_dest = g_build_filename(dir, file, nullptr);
    properties->import_part = g_build_filename(dir, part, nullptr);
    properties->import_source = g_strdup(path);
    g_free(part);
    g_free(file);
    g_free(dir);
    char *importer = importer_path();
    const char *argv[] = {importer, path, properties->import_part, nullptr};
    GError *error = nullptr;
    properties->import = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &error);
    g_free(importer);
    if (properties->import == nullptr) {
        xd_strbuf_append(&properties->import_out,
                         xd_str_from_cstr("Display Properties cannot run xd-theme-import, "
                                          "which converts Windows visual styles: "));
        xd_strbuf_append(&properties->import_out, xd_str_from_cstr(error->message));
        g_error_free(error);
        import_finished(properties, false);
        return;
    }
    properties->import_fd = stdout_fd(properties->import);
    properties->import_src =
        xd_ui_add_fd(properties->ui, properties->import_fd, import_output, properties);
    xd_ui_set_busy(properties->ui, true);
    update_style_browse(properties);
}

/* What Browse... chose. */
static void style_chosen(desk *properties, const char *path) {
    char *base = g_path_get_basename(path);
    char *lower = g_ascii_strdown(base, -1);
    char *dot = strrchr(base, '.');
    char *stem = g_strndup(base, dot != nullptr && dot != base ? (gsize)(dot - base) : strlen(base));
    if (g_str_has_suffix(lower, ".msstyles") || g_str_has_suffix(lower, ".theme")) {
        start_import(properties, path, stem);
    } else if (g_str_has_suffix(lower, ".xdtheme")) {
        install_package(properties, path, stem);
    } else {
        char *text = g_strdup_printf("\"%s\" is not a visual style. Choose a Windows visual "
                                     "style (.msstyles), a Windows theme (.theme) or a "
                                     "XeonDesktop theme package (.xdtheme).",
                                     base);
        error_box(properties, text);
        g_free(text);
    }
    g_free(stem);
    g_free(lower);
    g_free(base);
}

static void on_style_browse([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    const char *home = getenv("HOME");
    const char *args[] = {"--styles", "--title", "Open Visual Style", "--folder",
                          home != nullptr ? home : "/", nullptr};
    start_chooser(properties, args, CHOOSE_STYLE);
}

/* ---- Themes ---------------------------------------------------------------------- */

/* XP's Themes tab: a theme sets the visual style with its colours, the
 * wallpaper, the sound scheme and the pointers together. Ours are the
 * theme packages that bring more than a style, and XP's two built in. */

/* Windows XP's Bliss: ours is Rolling Hills, in the source tree's data/
 * or installed beside the other wallpapers. "" if it isn't there. */
static void xp_wallpaper(char *out, size_t cap) {
    out[0] = '\0';
    xd_strbuf data = {0};
    if (xd_data_dir(&data)) {
        static const char *const places[] = {"wallpapers", "../wallpapers/xeondesktop"};
        for (size_t i = 0; i < XD_ARRAY_LEN(places) && out[0] == '\0'; i++) {
            char *path = g_build_filename(xd_strbuf_cstr(&data), places[i], "Rolling Hills.jpg",
                                          nullptr);
            char *canon = g_canonicalize_filename(path, nullptr);
            if (g_file_test(canon, G_FILE_TEST_IS_REGULAR) && strlen(canon) < cap)
                memcpy(out, canon, strlen(canon) + 1);
            g_free(canon);
            g_free(path);
        }
    }
    xd_strbuf_free(&data);
}

/* The installed scheme that is Windows XP style's Default (blue). */
static const char *luna_blue_scheme(const desk *properties) {
    for (size_t i = 0; i < properties->schemes.count; i++) {
        if (scheme_luna_color(properties, i) == XD_LUNA_BLUE)
            return properties->schemes.stems[i];
    }
    return "";
}

static bool builtin_matches(const desk *properties, theme_kind kind) {
    const xd_settings *settings = &properties->pending;
    if (settings->theme_package[0] != '\0' || settings->font_size != XD_FONT_NORMAL ||
        settings->custom_scheme.color_mask != 0 || settings->custom_scheme.metric_mask != 0 ||
        settings->custom_scheme.font_mask != 0 || settings->mouse.cursor_theme[0] != '\0')
        return false;
    if (kind == THEME_CLASSIC)
        return settings->style == XD_STYLE_CLASSIC && settings->scheme[0] == '\0' &&
               settings->wallpaper[0] == '\0';
    char wallpaper[sizeof(settings->wallpaper)];
    xp_wallpaper(wallpaper, sizeof(wallpaper));
    return settings->style == XD_STYLE_LUNA &&
           strcmp(settings->scheme, luna_blue_scheme(properties)) == 0 &&
           strcmp(settings->wallpaper, wallpaper) == 0 &&
           (wallpaper[0] == '\0' || settings->wallpaper_mode == XD_WALLPAPER_STRETCH);
}

static const char *package_label(const xd_installed_package *package) {
    return package->info.name[0] != '\0' ? package->info.name : package->name;
}

static const xd_installed_package *theme_package_at(const desk *properties, const theme_item *it) {
    return it->kind == THEME_PACKAGE && it->package < properties->packages->len
               ? g_ptr_array_index(properties->packages, it->package)
               : nullptr;
}

static const theme_item *selected_theme(const desk *properties) {
    int i = properties->theme_combo != nullptr ? xd_widget_selected(properties->theme_combo) : -1;
    return i >= 0 && (guint)i < properties->theme_items->len
               ? &g_array_index(properties->theme_items, theme_item, (guint)i)
               : nullptr;
}

/* Delete: a package of the user's own (a .xdtheme in their themes, which
 * Browse... and Save As... put there), not the one applied. */
static bool theme_deletable(const desk *properties, const xd_installed_package *package) {
    if (package == nullptr || !g_str_has_suffix(package->path, ".xdtheme") ||
        strcmp(package->name, properties->saved.theme_package) == 0)
        return false;
    char *dir = xd_theme_package_user_dir();
    char *parent = g_path_get_dirname(package->path);
    bool own = strcmp(dir, parent) == 0;
    g_free(parent);
    g_free(dir);
    return own;
}

static void update_theme_buttons(desk *properties) {
    if (properties->theme_combo == nullptr)
        return;
    bool idle = properties->chooser_src == nullptr && properties->import == nullptr;
    const theme_item *it = selected_theme(properties);
    xd_widget_set_enabled(properties->theme_save, idle);
    xd_widget_set_enabled(properties->theme_delete,
                          idle && it != nullptr &&
                              theme_deletable(properties, theme_package_at(properties, it)));
    xd_widget_set_enabled(properties->theme_combo, idle);
    xd_widget_redraw(properties->theme_save);
    xd_widget_redraw(properties->theme_delete);
}

static void add_theme_item(desk *properties, const char *label, theme_kind kind, guint package) {
    theme_item it = {.kind = kind, .package = package};
    g_array_append_val(properties->theme_items, it);
    xd_widget_add_item(properties->theme_combo, label);
}

/* The list, with the theme the pending settings are selected; when they
 * are none of them, first "My Current Theme", or the theme they started
 * from "(Modified)", as XP shows them. */
static void fill_theme_list(desk *properties) {
    if (properties->theme_combo == nullptr)
        return;
    xd_widget_clear_items(properties->theme_combo);
    g_array_set_size(properties->theme_items, 0);
    int current = -1;
    const xd_installed_package *base = nullptr;
    for (guint i = 0; i < properties->packages->len && current < 0; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        if (!xd_theme_set_has_extras(&package->info))
            continue;
        if (strcmp(package->name, properties->pending.theme_package) == 0)
            base = package;
        if (xd_theme_set_matches(package, nullptr, &properties->pending,
                                 &properties->pending_sounds))
            current = (int)i;
    }
    bool xp = builtin_matches(properties, THEME_XP),
         classic = builtin_matches(properties, THEME_CLASSIC);
    if (current < 0 && !xp && !classic) {
        char *label = nullptr;
        if (base != nullptr)
            label = g_strdup_printf("%s (Modified)", package_label(base));
        else if (properties->pending.theme_package[0] == '\0' &&
                 properties->pending.style == XD_STYLE_LUNA)
            label = g_strdup("Windows XP (Modified)");
        else if (properties->pending.theme_package[0] == '\0' &&
                 properties->pending.style == XD_STYLE_CLASSIC)
            label = g_strdup("Windows Classic (Modified)");
        else
            label = g_strdup("My Current Theme");
        add_theme_item(properties, label, THEME_CURRENT, 0);
        g_free(label);
    }
    int select = 0;
    if (xp)
        select = (int)properties->theme_items->len;
    add_theme_item(properties, "Windows XP", THEME_XP, 0);
    if (classic)
        select = (int)properties->theme_items->len;
    add_theme_item(properties, "Windows Classic", THEME_CLASSIC, 0);
    for (guint i = 0; i < properties->packages->len; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        if (!xd_theme_set_has_extras(&package->info))
            continue;
        if ((int)i == current)
            select = (int)properties->theme_items->len;
        add_theme_item(properties, package_label(package), THEME_PACKAGE, i);
    }
    add_theme_item(properties, "Browse...", THEME_BROWSE, 0);
    xd_widget_select(properties->theme_combo, select);
    update_theme_buttons(properties);
}

/* After a theme changed `pending`: the other tabs show its choices. */
static void theme_applied(desk *properties) {
    if (properties->pending.wallpaper[0] != '\0') {
        char *path = g_strdup(properties->pending.wallpaper); /* choose_wallpaper writes it there */
        choose_wallpaper(properties, path);
        g_free(path);
    } else {
        xd_widget_select(properties->wallpaper_list, 0);
        xd_widget_redraw(properties->bg_preview);
    }
    xd_widget_select(properties->wallpaper_mode, (int)properties->pending.wallpaper_mode);
    fill_styles(properties);
    xd_widget_redraw(properties->style_combo);
    appearance_changed(properties);
    fill_theme_list(properties);
    xd_widget_redraw(properties->theme_combo);
    xd_widget_redraw(properties->theme_preview);
    changed(properties);
}

static void choose_builtin(desk *properties, theme_kind kind) {
    bool xp = kind == THEME_XP;
    properties->pending.theme_package[0] = '\0';
    properties->pending.theme_variant[0] = '\0';
    set_style(properties, xp ? XD_STYLE_LUNA : XD_STYLE_CLASSIC);
    snprintf(properties->pending.scheme, sizeof(properties->pending.scheme), "%s",
             xp ? luna_blue_scheme(properties) : "");
    properties->pending.font_size = XD_FONT_NORMAL;
    properties->pending.custom_scheme = (xd_scheme_custom){0};
    /* XP's Windows XP.theme: Bliss, stretched; Windows Classic.theme: no
     * wallpaper. Both: Windows' own pointers, and the sounds as they are. */
    if (xp)
        xp_wallpaper(properties->pending.wallpaper, sizeof(properties->pending.wallpaper));
    else
        properties->pending.wallpaper[0] = '\0';
    properties->pending.wallpaper_mode = XD_WALLPAPER_STRETCH;
    properties->pending.mouse.cursor_theme[0] = '\0';
    theme_applied(properties);
}

/* A package as a theme: its style and first variant as Appearance's
 * Windows and buttons chooses them, then what it brings besides, its
 * files unpacked for the settings to point at. */
static void choose_package_theme(desk *properties, const xd_installed_package *package) {
    xd_settings before = properties->pending;
    snprintf(properties->pending.theme_package, sizeof(properties->pending.theme_package), "%s",
             package->name);
    set_style(properties, package->info.base);
    set_variant(properties, &package->info.variants[0]);
    GError *error = nullptr;
    bool sounds_set = false;
    if (!xd_theme_set_extras(package, nullptr, true, &properties->pending,
                             &properties->pending_sounds, &sounds_set, &error)) {
        properties->pending = before;
        char *text = g_strdup_printf("The theme \"%s\" cannot be used.\n\n%s",
                                     package_label(package), error->message);
        error_box(properties, text);
        g_free(text);
        g_error_free(error);
    }
    theme_applied(properties);
}

static void theme_choose_installed(desk *properties, const char *name) {
    for (guint i = 0; i < properties->packages->len; i++) {
        const xd_installed_package *package = g_ptr_array_index(properties->packages, i);
        if (strcmp(package->name, name) == 0)
            choose_package_theme(properties, package);
    }
}

static void on_theme(xd_widget *widget, void *user) {
    desk *properties = user;
    (void)widget;
    const theme_item *it = selected_theme(properties);
    if (it == nullptr)
        return;
    switch (it->kind) {
    case THEME_CURRENT:
        break;
    case THEME_XP:
    case THEME_CLASSIC:
        choose_builtin(properties, it->kind);
        break;
    case THEME_PACKAGE:
        choose_package_theme(properties, theme_package_at(properties, it));
        break;
    case THEME_BROWSE: {
        /* As Appearance's Browse...: a Windows theme (.theme) is
         * converted, a package installed; either is then chosen. */
        const char *home = getenv("HOME");
        const char *args[] = {"--themes", "--title", "Open Theme", "--folder",
                              home != nullptr ? home : "/", nullptr};
        if (!start_chooser(properties, args, CHOOSE_THEME)) {
            fill_theme_list(properties);
            xd_widget_redraw(properties->theme_combo);
        }
        break;
    }
    }
}

static void on_theme_save([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    char *dir = xd_theme_package_user_dir();
    (void)g_mkdir_with_parents(dir, 0755);
    const char *args[] = {"--themes", "--title", "Save As", "--folder", dir,
                          "--save", "My Theme.xdtheme", nullptr};
    start_chooser(properties, args, CHOOSE_THEME_SAVE);
    g_free(dir);
}

/* Save As...'s answer: the pending look written as a package; saved
 * among the user's themes, it's listed and chosen. */
static void theme_save_to(desk *properties, const char *chosen) {
    char *path = g_str_has_suffix(chosen, ".xdtheme") ? g_strdup(chosen)
                                                      : g_strconcat(chosen, ".xdtheme", nullptr);
    char *base = g_path_get_basename(path);
    char *stem = g_strndup(base, strlen(base) - strlen(".xdtheme"));
    const xd_installed_package *pkg = pending_package(properties);
    GError *error = nullptr;
    if (stem[0] == '\0' || stem[0] == '.') {
        char *text = g_strdup_printf("\"%s\" is not a name a theme can be saved as.", base);
        error_box(properties, text);
        g_free(text);
    } else if (!xd_theme_set_save(path, stem, &properties->pending, &properties->pending_sounds,
                                  pkg != nullptr ? pkg->path : nullptr,
                                  pkg != nullptr ? pending_variant(properties, pkg)->id : nullptr,
                                  nullptr, &error)) {
        char *text = g_strdup_printf("The theme cannot be saved as \"%s\".\n\n%s", base,
                                     error->message);
        error_box(properties, text);
        g_free(text);
        g_error_free(error);
    } else {
        char *dir = xd_theme_package_user_dir();
        char *parent = g_path_get_dirname(path);
        if (strcmp(dir, parent) == 0) {
            properties->theme_import = true;
            select_installed(properties, stem);
        }
        g_free(parent);
        g_free(dir);
    }
    g_free(stem);
    g_free(base);
    g_free(path);
}

/* Delete: the package to the Trash. */
static void on_theme_delete([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    const theme_item *it = selected_theme(properties);
    const xd_installed_package *package =
        it != nullptr ? theme_package_at(properties, it) : nullptr;
    if (!theme_deletable(properties, package))
        return;
    GFile *file = g_file_new_for_path(package->path);
    GError *error = nullptr;
    if (!g_file_trash(file, nullptr, &error)) {
        char *text = g_strdup_printf("The theme \"%s\" cannot be deleted.\n\n%s",
                                     package_label(package), error->message);
        error_box(properties, text);
        g_free(text);
        g_error_free(error);
    }
    g_object_unref(file);
    bool pending = strcmp(package->name, properties->pending.theme_package) == 0;
    char **dirs = xd_theme_package_dirs();
    g_ptr_array_unref(properties->packages);
    properties->packages = xd_theme_packages_list((const char *const *)dirs);
    g_strfreev(dirs);
    /* Chosen but not applied: back to what is. */
    if (pending) {
        properties->pending.theme_package[0] = '\0';
        set_style(properties, properties->saved.style);
    }
    g_clear_pointer(&properties->preview_pkg_path, g_free);
    fill_styles(properties);
    appearance_changed(properties);
    fill_theme_list(properties);
    xd_widget_redraw(properties->theme_combo);
    xd_widget_redraw(properties->theme_preview);
}

/* Sample: the desktop as the theme makes it: its wallpaper, icons and a
 * window in its style. */
static void draw_theme_preview([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                               const xd_theme *theme, void *user) {
    desk *properties = user;
    xd_theme pt;
    pending_theme(properties, theme->scale, &pt);
    xd_rect well = xd_theme_draw_field(theme, cairo, rect, false);
    cairo_save(cairo);
    cairo_rectangle(cairo, well.x, well.y, well.w, well.h);
    cairo_clip(cairo);
    paint_pending_desktop(properties, cairo, well, &pt);

    /* My Computer and the Recycle Bin, down the left as on the desktop. */
    int px = xd_tpx(theme, 32);
    static const xd_desk_icon icons[] = {XD_DESK_ICON_COMPUTER, XD_DESK_ICON_TRASH_EMPTY};
    static const char *const labels[] = {"My Computer", "Recycle Bin"};
    for (size_t i = 0; i < XD_ARRAY_LEN(icons); i++) {
        const char *names[2];
        size_t count = 0;
        if (properties->pending.desktop_icons.icons[icons[i]][0] != '\0')
            names[count++] = properties->pending.desktop_icons.icons[icons[i]];
        names[count++] = xd_desk_icon_default(icons[i]);
        int cell = xd_tpx(theme, 70);
        int ix = well.x + xd_tpx(theme, 8) + (cell - px) / 2;
        int iy = well.y + xd_tpx(theme, 8) + (int)i * xd_tpx(theme, 64);
        cairo_surface_t *img = xd_icons_get_any(xd_ui_icons(properties->ui), names, count, px);
        if (img != nullptr) {
            cairo_set_source_surface(cairo, img, ix, iy);
            cairo_paint(cairo);
        }
        xd_text_style st = xd_text(&pt, XD_FONT_ICON, xd_rgba_hex(0xFFFFFF), XD_ALIGN_CENTER);
        st.shadow = true;
        st.shadow_color = xd_rgba_hex(0x000000);
        xd_draw_text(cairo,
                     (xd_rect){well.x + xd_tpx(theme, 8), iy + px + xd_tpx(theme, 2), cell,
                               xd_tpx(theme, 16)},
                     labels[i], &st);
    }

    /* An active window, its text in a box, over the right of the desktop. */
    xd_frame active = {.title = "Active Window",
                       .active = true,
                       .buttons = XD_CAPBTN_CLOSE | XD_CAPBTN_MAXIMIZE | XD_CAPBTN_MINIMIZE};
    xd_rect win = {well.x + xd_tpx(theme, 96), well.y + xd_tpx(theme, 24),
                   well.w - xd_tpx(theme, 120), well.h - xd_tpx(theme, 60)};
    xd_theme_draw_frame(&pt, cairo, win, &active);
    xd_insets in = xd_theme_frame_insets(&pt, &active);
    xd_rect client = xd_rect_inset4(win, in.left, in.top, in.right, in.bottom);
    xd_fill_rect(cairo, client, xd_theme_dialog_color(&pt));
    xd_rect field = xd_rect_inset(client, xd_tpx(&pt, 8), xd_tpx(&pt, 8));
    xd_rect field_in = xd_theme_draw_field(&pt, cairo, field, false);
    xd_text_style st = xd_text(&pt, XD_FONT_MESSAGE, xd_sys(&pt, XD_COLOR_WINDOW_TEXT),
                               XD_ALIGN_START);
    xd_draw_text(cairo,
                 (xd_rect){field_in.x + xd_tpx(&pt, 4), field_in.y + xd_tpx(&pt, 2),
                           field_in.w - xd_tpx(&pt, 8), xd_tpx(&pt, 16)},
                 "Window Text", &st);
    cairo_restore(cairo);
}

static void build_theme_tab(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_THEMES;
    properties->theme_items = g_array_new(FALSE, TRUE, sizeof(theme_item));
    xd_widget_set_page(xd_label(win, (xd_rect){20, 40, 364, 30},
                                "A theme is a background plus a set of sounds, icons, and other "
                                "elements to help you personalize your computer with one click.",
                                XD_LABEL_WRAP),
                       page);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 80, 200, 16}, "Theme:", 0), page);
    properties->theme_combo =
        xd_combo(win, (xd_rect){20, 98, 206, 21}, false, on_theme, properties);
    xd_widget_set_page(properties->theme_combo, page);
    properties->theme_save =
        xd_button(win, (xd_rect){232, 97, 75, 23}, "Save As...", on_theme_save, properties);
    xd_widget_set_page(properties->theme_save, page);
    properties->theme_delete =
        xd_button(win, (xd_rect){309, 97, 75, 23}, "Delete", on_theme_delete, properties);
    xd_widget_set_page(properties->theme_delete, page);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 130, 200, 16}, "Sample:", 0), page);
    properties->theme_preview =
        xd_custom(win, (xd_rect){20, 148, 364, 244}, draw_theme_preview, properties);
    xd_widget_set_page(properties->theme_preview, page);
    fill_theme_list(properties);
}

/* ---- Advanced Appearance ------------------------------------------------------------ */

static void on_advanced_done(const xd_scheme_custom *custom, void *user) {
    desk *properties = user;
    properties->adv = nullptr;
    xd_widget_set_enabled(properties->advanced, properties->color == nullptr);
    if (custom == nullptr)
        return;
    properties->pending.custom_scheme = *custom;
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

static void open_advanced(desk *properties) {
    if (properties->adv != nullptr)
        return;
    properties->adv =
        advanced_open(properties->ui, &properties->pending, on_advanced_done, properties);
    xd_widget_set_enabled(properties->advanced, false);
}

/* Windows 7's "Advanced appearance settings..." link, at the bottom of
 * Window Color and Appearance. */
static void draw_advanced_link([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                               const xd_theme *theme, [[maybe_unused]] void *user) {
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE, xd_rgba_hex(0x0066CC), XD_ALIGN_START);
    xd_draw_text(cairo, rect, "Advanced appearance settings...", &st);
}

static void on_advanced_link([[maybe_unused]] xd_widget *widget, xd_pointer_event ev,
                             [[maybe_unused]] double x, [[maybe_unused]] double y, void *user) {
    if (ev == XD_POINTER_RELEASE)
        open_advanced(user);
}

/* ---- Window Color and Appearance (Aero) ------------------------------------------- */

/* Windows 7's Personalization > Window Color: the glass colour of the
 * frames, taskbar and Start menu. */
static constexpr int SWATCH_COLUMNS = 8;
static constexpr int SWATCH_PITCH = 52; /* logical px per swatch, its margin included */
static constexpr int SWATCH_MARGIN = 4;
static constexpr int COLOR_W = 456;
static constexpr int COLOR_H = 414;

static uint32_t glass_rgb(uint32_t argb) { return argb & 0xFFFFFFu; }

/* The swatch with the pending colour, or -1 (a colour mixed by hand). */
static int current_swatch(const desk *properties) {
    for (int i = 0; i < XD_GLASS_SWATCH_COUNT; i++) {
        if (glass_rgb(xd_glass_swatches[i].argb) == glass_rgb(properties->pending.glass_color))
            return i;
    }
    return -1;
}

static void color_changed(desk *properties) {
    int i = current_swatch(properties);
    char text[64];
    snprintf(text, sizeof(text), "Current color: %s", i >= 0 ? xd_glass_swatches[i].name : "Custom");
    xd_widget_set_text(properties->color_name, text);
    xd_widget_redraw(properties->swatches);
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

/* A swatch: the colour under a glassy sheen, as Windows 7 draws them. */
static void draw_swatch(const xd_theme *theme, cairo_t *cairo, xd_rect rect, uint32_t argb) {
    double rad = xd_tpx(theme, 3);
    xd_rgba color = xd_rgba_hex(glass_rgb(argb));
    xd_path_rounded(cairo, rect.x + 0.5, rect.y + 0.5, rect.w - 1, rect.h - 1, rad, rad, rad, rad);
    xd_set_color(cairo, color);
    cairo_fill_preserve(cairo);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, rect.y, 0, rect.y + rect.h);
    cairo_pattern_add_color_stop_rgba(pattern, 0, 1, 1, 1, 0.55);
    cairo_pattern_add_color_stop_rgba(pattern, 0.48, 1, 1, 1, 0.2);
    cairo_pattern_add_color_stop_rgba(pattern, 0.48, 1, 1, 1, 0.0);
    cairo_pattern_add_color_stop_rgba(pattern, 1, 1, 1, 1, 0.12);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    cairo_set_line_width(cairo, 1);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0.45);
    cairo_stroke(cairo);
    xd_path_rounded(cairo, rect.x + 1.5, rect.y + 1.5, rect.w - 3, rect.h - 3, rad - 1, rad - 1,
                    rad - 1, rad - 1);
    cairo_set_source_rgba(cairo, 1, 1, 1, 0.6);
    cairo_stroke(cairo);
}

static void draw_swatches([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect bounds,
                          const xd_theme *theme, void *user) {
    desk *properties = user;
    int sel = current_swatch(properties);
    int pitch = xd_tpx(theme, SWATCH_PITCH), margin = xd_tpx(theme, SWATCH_MARGIN);
    for (int i = 0; i < XD_GLASS_SWATCH_COUNT; i++) {
        xd_rect cell = {bounds.x + (i % SWATCH_COLUMNS) * pitch,
                        bounds.y + (i / SWATCH_COLUMNS) * pitch, pitch, pitch};
        if (i == sel) {
            /* Selected: the light-blue highlight of Windows 7's lists. */
            double rad = xd_tpx(theme, 3);
            xd_rect highlight = xd_rect_inset(cell, xd_tpx(theme, 1), xd_tpx(theme, 1));
            xd_path_rounded(cairo, highlight.x + 0.5, highlight.y + 0.5, highlight.w - 1,
                            highlight.h - 1, rad, rad, rad, rad);
            xd_set_color(cairo, xd_rgba_hex(0xDCEBFC));
            cairo_fill_preserve(cairo);
            cairo_set_line_width(cairo, 1);
            xd_set_color(cairo, xd_rgba_hex(0x7DA2CE));
            cairo_stroke(cairo);
        }
        draw_swatch(theme, cairo, xd_rect_inset(cell, margin, margin), xd_glass_swatches[i].argb);
    }
}

static void mixer_from_color(desk *properties) {
    properties->mixer = xd_rgba_to_hsv(xd_rgba_hex(glass_rgb(properties->pending.glass_color)));
    xd_slider_set(properties->mixer_widgets[1], 0, 359, (int)lround(properties->mixer.h) % 360);
    xd_slider_set(properties->mixer_widgets[4], 0, 100, (int)lround(properties->mixer.s * 100));
    xd_slider_set(properties->mixer_widgets[7], 0, 100, (int)lround(properties->mixer.v * 100));
    for (size_t i = 2; i < XD_ARRAY_LEN(properties->mixer_widgets); i += 3)
        xd_widget_redraw(properties->mixer_widgets[i]);
}

/* Choosing a swatch takes its colour and its intensity, as in Windows 7. */
static void on_swatch_pointer(xd_widget *widget, xd_pointer_event ev, double x, double y,
                              void *user) {
    desk *properties = user;
    if (ev != XD_POINTER_PRESS || x < 0 || y < 0)
        return;
    int col = (int)(x / SWATCH_PITCH), row = (int)(y / SWATCH_PITCH);
    int i = row * SWATCH_COLUMNS + col;
    if (col >= SWATCH_COLUMNS || i >= XD_GLASS_SWATCH_COUNT)
        return;
    properties->pending.glass_color = xd_glass_swatches[i].argb;
    xd_slider_set(properties->intensity, 0, 255, (int)(properties->pending.glass_color >> 24));
    mixer_from_color(properties);
    xd_widget_redraw(widget);
    color_changed(properties);
}

static void on_transparency(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.glass_transparency = xd_checkbox_checked(widget);
    color_changed(properties);
}

static void on_intensity(xd_widget *widget, void *user) {
    desk *properties = user;
    uint32_t alpha = (uint32_t)XD_CLAMP(xd_slider_value(widget), 0, 255);
    properties->pending.glass_color = (alpha << 24) | glass_rgb(properties->pending.glass_color);
    color_changed(properties);
}

static void on_mixer([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    properties->mixer = (xd_hsv){xd_slider_value(properties->mixer_widgets[1]),
                                 xd_slider_value(properties->mixer_widgets[4]) / 100.0,
                                 xd_slider_value(properties->mixer_widgets[7]) / 100.0};
    xd_rgba color = xd_rgba_from_hsv(properties->mixer, 1.0);
    uint32_t rgb = ((uint32_t)lround(color.r * 255) << 16) |
                   ((uint32_t)lround(color.g * 255) << 8) | (uint32_t)lround(color.b * 255);
    properties->pending.glass_color = (properties->pending.glass_color & 0xFF000000u) | rgb;
    /* The other sliders' gradients run through the new colour. */
    for (size_t i = 2; i < XD_ARRAY_LEN(properties->mixer_widgets); i += 3)
        xd_widget_redraw(properties->mixer_widgets[i]);
    color_changed(properties);
}

/* Under each mixer slider, the colours it runs through. */
static void draw_mixer_gradient(xd_widget *widget, cairo_t *cairo, xd_rect rect,
                                [[maybe_unused]] const xd_theme *theme, void *user) {
    desk *properties = user;
    int which = 0;
    while (which < 3 && properties->mixer_widgets[which * 3 + 2] != widget)
        which++;
    cairo_pattern_t *pattern = cairo_pattern_create_linear(rect.x, 0, rect.x + rect.w, 0);
    for (int k = 0; k <= 12; k++) {
        double fraction = k / 12.0;
        xd_hsv hsv = properties->mixer;
        if (which == 0)
            hsv = (xd_hsv){fraction * 360, 1, 1};
        else if (which == 1)
            hsv.s = fraction;
        else
            hsv.v = fraction;
        xd_rgba color = xd_rgba_from_hsv(hsv, 1);
        cairo_pattern_add_color_stop_rgb(pattern, fraction, color.r, color.g, color.b);
    }
    cairo_rectangle(cairo, rect.x, rect.y, rect.w, rect.h);
    cairo_set_source(cairo, pattern);
    cairo_fill(cairo);
    cairo_pattern_destroy(pattern);
}

static void show_mixer(desk *properties, bool shown) {
    properties->mixer_shown = shown;
    for (size_t i = 0; i < XD_ARRAY_LEN(properties->mixer_widgets); i++)
        xd_widget_set_visible(properties->mixer_widgets[i], shown);
    xd_widget_redraw(properties->mixer_toggle);
}

/* Windows 7's expander: a round button with a chevron, and its label. */
static void draw_mixer_toggle([[maybe_unused]] xd_widget *widget, cairo_t *cairo, xd_rect rect,
                              const xd_theme *theme, void *user) {
    desk *properties = user;
    double rad = xd_tpx(theme, 8);
    double cx = rect.x + rad, cy = rect.y + rect.h / 2.0;
    cairo_arc(cairo, cx, cy, rad - 0.5, 0, 2 * G_PI);
    cairo_pattern_t *pattern = cairo_pattern_create_linear(0, cy - rad, 0, cy + rad);
    cairo_pattern_add_color_stop_rgb(pattern, 0, 1, 1, 1);
    cairo_pattern_add_color_stop_rgb(pattern, 1, 0.86, 0.88, 0.91);
    cairo_set_source(cairo, pattern);
    cairo_fill_preserve(cairo);
    cairo_pattern_destroy(pattern);
    cairo_set_line_width(cairo, 1);
    xd_set_color(cairo, xd_rgba_hex(0x8E9AAC));
    cairo_stroke(cairo);
    /* Down to show, up to hide. */
    double half_width = rad * 0.4, dir = properties->mixer_shown ? -1 : 1;
    cairo_move_to(cairo, cx - half_width, cy - dir * half_width / 2);
    cairo_line_to(cairo, cx, cy + dir * half_width / 2);
    cairo_line_to(cairo, cx + half_width, cy - dir * half_width / 2);
    cairo_set_line_width(cairo, xd_tpx(theme, 1) * 1.5);
    xd_set_color(cairo, xd_rgba_hex(0x404A5A));
    cairo_stroke(cairo);
    xd_text_style st = xd_text(theme, XD_FONT_MESSAGE, xd_rgba_hex(0x000000), XD_ALIGN_START);
    int tx = rect.x + (int)(2 * rad) + xd_tpx(theme, 6);
    xd_draw_text(cairo, (xd_rect){tx, rect.y, rect.x + rect.w - tx, rect.h},
                 properties->mixer_shown ? "Hide color mixer" : "Show color mixer", &st);
}

static void on_mixer_toggle([[maybe_unused]] xd_widget *widget, xd_pointer_event ev,
                            [[maybe_unused]] double x, [[maybe_unused]] double y, void *user) {
    if (ev == XD_POINTER_PRESS)
        show_mixer(user, !((desk *)user)->mixer_shown);
}

static void color_close(desk *properties) {
    if (properties->color == nullptr)
        return;
    xd_window *color = properties->color;
    properties->color = nullptr;
    xd_window_close(color);
    xd_widget_set_enabled(properties->advanced, properties->adv == nullptr);
}

static void on_color_save([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    color_close(properties);
    changed(properties);
}

static void on_color_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.glass_color = properties->color_was;
    properties->pending.glass_transparency = properties->transparency_was;
    xd_widget_redraw(properties->appearance_preview);
    color_close(properties);
    changed(properties);
}

static void on_window_color([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->color != nullptr)
        return;
    properties->color_was = properties->pending.glass_color;
    properties->transparency_was = properties->pending.glass_transparency;
    xd_window *win = properties->color = xd_window_create(
        properties->ui, "Window Color and Appearance", "xd-desk", COLOR_W, COLOR_H);
    xd_label(win, (xd_rect){20, 14, 416, 18},
             "Change the color of your window borders, Start menu, and taskbar", XD_LABEL_BOLD);
    properties->swatches =
        xd_custom(win,
                  (xd_rect){20, 42, SWATCH_COLUMNS * SWATCH_PITCH,
                            (XD_GLASS_SWATCH_COUNT / SWATCH_COLUMNS) * SWATCH_PITCH},
                  draw_swatches, properties);
    xd_custom_set_pointer(properties->swatches, on_swatch_pointer);
    properties->color_name = xd_label(win, (xd_rect){24, 154, 300, 16}, "", 0);
    properties->transparency =
        xd_checkbox(win, (xd_rect){24, 178, 300, 18}, "Enable transparency",
                    properties->pending.glass_transparency, on_transparency, properties);
    xd_label(win, (xd_rect){24, 208, 100, 16}, "Color intensity:", 0);
    properties->intensity =
        xd_slider(win, (xd_rect){124, 202, 300, 26}, 0, 255,
                  (int)(properties->pending.glass_color >> 24), on_intensity, properties);
    properties->mixer_toggle =
        xd_custom(win, (xd_rect){24, 238, 200, 20}, draw_mixer_toggle, properties);
    xd_custom_set_pointer(properties->mixer_toggle, on_mixer_toggle);
    static const char *const names[] = {"Hue:", "Saturation:", "Brightness:"};
    for (int i = 0; i < 3; i++) {
        int y = 264 + i * 32;
        properties->mixer_widgets[i * 3] = xd_label(win, (xd_rect){44, y + 6, 76, 16}, names[i], 0);
        properties->mixer_widgets[i * 3 + 1] = xd_slider(
            win, (xd_rect){124, y, 300, 26}, 0, i == 0 ? 359 : 100, 0, on_mixer, properties);
        properties->mixer_widgets[i * 3 + 2] =
            xd_custom(win, (xd_rect){130, y + 26, 288, 4}, draw_mixer_gradient, properties);
    }
    mixer_from_color(properties);
    show_mixer(properties, false);
    color_changed(properties);

    xd_widget *save = xd_button(win, (xd_rect){COLOR_W - 200, COLOR_H - 34, 100, 23},
                                "Save changes", on_color_save, properties);
    xd_widget *cancel = xd_button(win, (xd_rect){COLOR_W - 92, COLOR_H - 34, 75, 23}, "Cancel",
                                  on_color_cancel, properties);
    xd_window_set_default(win, save);
    xd_window_set_cancel(win, cancel);
    xd_widget *link =
        xd_custom(win, (xd_rect){20, COLOR_H - 30, 200, 16}, draw_advanced_link, properties);
    xd_custom_set_pointer(link, on_advanced_link);
    xd_window_set_focus(win, properties->transparency);
    xd_widget_set_enabled(properties->advanced, false);
    xd_window_show(win);
}

/* XP's Advanced opens the editor; in Aero, Windows 7's Window Color and
 * Appearance, which links to it. */
static void on_advanced([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->pending.style == XD_STYLE_AERO)
        on_window_color(widget, properties);
    else
        open_advanced(properties);
}

static void build_appearance(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_APPEARANCE;
    properties->appearance_preview =
        xd_custom(win, (xd_rect){20, 40, 364, 186}, draw_appearance_preview, properties);
    xd_widget_set_page(properties->appearance_preview, page);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 238, 200, 16}, "Windows and buttons:", 0), page);
    properties->style_combo =
        xd_combo(win, (xd_rect){20, 256, 220, 21}, false, on_style, properties);
    xd_widget_set_page(properties->style_combo, page);
    char **dirs = xd_theme_package_dirs();
    properties->packages = xd_theme_packages_list((const char *const *)dirs);
    g_strfreev(dirs);
    fill_styles(properties);
    update_preview_package(properties);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 286, 200, 16}, "Color scheme:", 0), page);
    properties->scheme_combo =
        xd_combo(win, (xd_rect){20, 304, 220, 21}, false, on_scheme, properties);
    xd_widget_set_page(properties->scheme_combo, page);
    fill_schemes(properties);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 334, 200, 16}, "Font size:", 0), page);
    properties->font_combo =
        xd_combo(win, (xd_rect){20, 352, 220, 21}, false, on_font_size, properties);
    xd_widget_set_page(properties->font_combo, page);
    fill_font_sizes(properties);

    properties->advanced =
        xd_button(win, (xd_rect){298, 352, 86, 23}, "Advanced", on_advanced, properties);
    xd_widget_set_page(properties->advanced, page);
    /* Beside Windows and buttons, as XP's Themes tab offers Browse... in
     * its list: a Windows visual style (.msstyles, or a .theme naming
     * one), converted into a theme package, or a package. Last in the tab
     * order, after Advanced. */
    properties->style_browse =
        xd_button(win, (xd_rect){298, 255, 86, 23}, "Browse...", on_style_browse, properties);
    xd_widget_set_page(properties->style_browse, page);
}

/* ---- Effects ------------------------------------------------------------------------ */

/* Font smoothing's lists, in the order shown (GNOME's names), and the
 * values they stand for. */
static const xd_font_antialias antialias_items[] = {XD_SMOOTH_NONE, XD_SMOOTH_GRAYSCALE,
                                                    XD_SMOOTH_SUBPIXEL};
static const char *const antialias_labels[] = {"None", "Standard (grayscale)",
                                               "Subpixel (for LCD screens)"};
static const char *const subpixel_labels[] = {"RGB", "BGR", "Vertical RGB", "Vertical BGR"};
static const xd_font_hinting hinting_items[] = {XD_HINT_NONE, XD_HINT_SLIGHT, XD_HINT_MEDIUM,
                                                XD_HINT_FULL};
static const char *const hinting_labels[] = {"None", "Slight", "Medium", "Full"};

static void on_smoothing([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    int aa = xd_widget_selected(properties->antialias_combo);
    int hint = xd_widget_selected(properties->hinting_combo);
    int order = xd_widget_selected(properties->subpixel_combo);
    if (aa >= 0 && (size_t)aa < XD_ARRAY_LEN(antialias_items))
        properties->pending.font_smoothing.antialias = antialias_items[aa];
    if (hint >= 0 && (size_t)hint < XD_ARRAY_LEN(hinting_items))
        properties->pending.font_smoothing.hinting = hinting_items[hint];
    if (order >= 0 && (size_t)order < XD_ARRAY_LEN(subpixel_labels))
        properties->pending.font_smoothing.order = (xd_subpixel_order)order;
    /* The order only matters to subpixel smoothing. */
    xd_widget_set_enabled(properties->subpixel_combo,
                          properties->pending.font_smoothing.antialias == XD_SMOOTH_SUBPIXEL);
    changed(properties);
}

static void on_transition(xd_widget *widget, void *user) {
    (void)widget;
    desk *properties = user;
    bool on = xd_checkbox_checked(properties->transition_check);
    xd_widget_set_enabled(properties->transition_combo, on);
    properties->pending.menu_transitions = on;
    properties->pending.menu_transition = xd_widget_selected(properties->transition_combo) == 1
                                              ? XD_TRANSITION_SCROLL
                                              : XD_TRANSITION_FADE;
    changed(properties);
}

static void on_large_icons(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.large_icons = xd_checkbox_checked(widget);
    changed(properties);
}

static void on_full_drag(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.full_window_drag = xd_checkbox_checked(widget);
    changed(properties);
}

/* Scale theme images: Automatic, then the filters in scale.h's order. */
static void on_scaling(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i < 0 || i > XD_FILTER_COUNT)
        return;
    properties->pending.image_scaling = i == 0 ? XD_FILTER_COUNT : (xd_image_filter)(i - 1);
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

static void on_integer_scaling(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.integer_scaling = xd_checkbox_checked(widget);
    xd_widget_redraw(properties->appearance_preview);
    changed(properties);
}

static void on_keyboard_cues(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->pending.hide_keyboard_cues = xd_checkbox_checked(widget);
    changed(properties);
}

/* The Linux tab: the freedesktop icon theme everything draws from, the GTK
 * theme GTK programs get, and the widget style of Qt programs (which take
 * the rest from the GTK theme). Windows had none of them. */
static const char *theme_dir_at(GPtrArray *list, int item) {
    /* Item 0 is the default. */
    if (item <= 0 || (guint)item > list->len)
        return "";
    return ((xd_installed_theme *)g_ptr_array_index(list, (guint)item - 1))->dir;
}

static void on_icon_theme(xd_widget *widget, void *user) {
    desk *properties = user;
    snprintf(properties->pending.icon_theme, sizeof(properties->pending.icon_theme), "%s",
             theme_dir_at(properties->icon_themes, xd_widget_selected(widget)));
    changed(properties);
}

static void on_gtk_theme(xd_widget *widget, void *user) {
    desk *properties = user;
    snprintf(properties->pending.gtk_theme, sizeof(properties->pending.gtk_theme), "%s",
             theme_dir_at(properties->gtk_themes, xd_widget_selected(widget)));
    changed(properties);
}

static void on_qt_style(xd_widget *widget, void *user) {
    desk *properties = user;
    snprintf(properties->pending.qt_style, sizeof(properties->pending.qt_style), "%s",
             theme_dir_at(properties->qt_styles, xd_widget_selected(widget)));
    changed(properties);
}

/* Fills a theme list after its default item and selects `dir` ("" = the
 * default). A theme that isn't installed (any more) is listed by its
 * directory, so that the setting stays as it is until changed. */
static void fill_themes(xd_widget *combo, GPtrArray *list, const char *dir) {
    int sel = 0;
    for (guint i = 0; dir[0] != '\0' && sel == 0 && i < list->len; i++) {
        if (strcmp(((xd_installed_theme *)g_ptr_array_index(list, i))->dir, dir) == 0)
            sel = (int)i + 1;
    }
    if (dir[0] != '\0' && sel == 0) {
        xd_installed_theme *installed_theme = g_new0(xd_installed_theme, 1);
        installed_theme->dir = g_strdup(dir);
        installed_theme->name = g_strdup(dir);
        g_ptr_array_add(list, installed_theme);
        sel = (int)list->len;
    }
    for (guint i = 0; i < list->len; i++)
        xd_widget_add_item(combo, ((xd_installed_theme *)g_ptr_array_index(list, i))->name);
    xd_widget_select(combo, sel);
}

static void on_night_light_done(const xd_nightlight_settings *nightlight_settings, void *user) {
    desk *properties = user;
    properties->night = nullptr;
    if (nightlight_settings != nullptr)
        properties->pending.night_light = *nightlight_settings;
    changed(properties);
}

static void on_night_light([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->night == nullptr)
        properties->night = nightlight_dialog_open(properties->ui, &properties->pending.night_light,
                                                   on_night_light_done, properties);
}

static void on_color_management([[maybe_unused]] xd_widget *widget, [[maybe_unused]] void *user) {
    launch_own("xd-color", nullptr);
}

static void build_themes(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_LINUX;
    char **dirs = xd_icon_theme_dirs();
    properties->icon_themes = xd_icon_themes_list((const char *const *)dirs);
    g_strfreev(dirs);
    dirs = xd_gtk_theme_dirs();
    properties->gtk_themes = xd_gtk_themes_list((const char *const *)dirs);
    g_strfreev(dirs);
    dirs = xd_qt_plugin_dirs();
    properties->qt_styles = xd_qt_styles_list((const char *const *)dirs);
    g_strfreev(dirs);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 40, 364, 116}, "Icons and program style"),
                       page);
    xd_widget_set_page(xd_label(win, (xd_rect){32, 65, 130, 16}, "Icons:", 0), page);
    properties->icon_combo =
        xd_combo(win, (xd_rect){164, 62, 208, 21}, false, on_icon_theme, properties);
    xd_widget_set_page(properties->icon_combo, page);
    /* The default ($XD_ICON_THEME, else Breeze), as the list names it. */
    xd_settings def = properties->pending;
    def.icon_theme[0] = '\0';
    const char *def_dir = xd_settings_icon_theme(&def);
    const char *def_name = def_dir;
    for (guint i = 0; i < properties->icon_themes->len; i++) {
        const xd_installed_theme *installed_theme = g_ptr_array_index(properties->icon_themes, i);
        if (strcmp(installed_theme->dir, def_dir) == 0)
            def_name = installed_theme->name;
    }
    char *label = g_strdup_printf("Default (%s)", def_name);
    xd_widget_add_item(properties->icon_combo, label);
    g_free(label);
    fill_themes(properties->icon_combo, properties->icon_themes, properties->pending.icon_theme);

    xd_widget_set_page(xd_label(win, (xd_rect){32, 95, 130, 16}, "Program style (GTK):", 0), page);
    properties->gtk_combo =
        xd_combo(win, (xd_rect){164, 92, 208, 21}, false, on_gtk_theme, properties);
    xd_widget_set_page(properties->gtk_combo, page);
    xd_widget_add_item(properties->gtk_combo, "XeonDesktop (matches the style)");
    fill_themes(properties->gtk_combo, properties->gtk_themes, properties->pending.gtk_theme);

    xd_widget_set_page(xd_label(win, (xd_rect){32, 125, 130, 16}, "Program style (Qt):", 0), page);
    properties->qt_combo =
        xd_combo(win, (xd_rect){164, 122, 208, 21}, false, on_qt_style, properties);
    xd_widget_set_page(properties->qt_combo, page);
    xd_widget_add_item(properties->qt_combo, "Automatic (matches the style)");
    fill_themes(properties->qt_combo, properties->qt_styles, properties->pending.qt_style);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 166, 364, 28},
                                "Qt programs that are already running keep their style until "
                                "they are started again.",
                                XD_LABEL_WRAP),
                       page);

    /* Colour: the night light (desk_nightlight.c) and colord's profiles
     * (xd-color), which XP kept in Settings > Advanced > Color Management. */
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 204, 364, 92}, "Color"), page);
    xd_widget_set_page(xd_label(win, (xd_rect){32, 224, 340, 28},
                                "Warmer colors at night, and the color profiles of monitors, "
                                "printers and scanners.",
                                XD_LABEL_WRAP),
                       page);
    xd_widget_set_page(
        xd_button(win, (xd_rect){32, 260, 110, 23}, "Night Light...", on_night_light, properties),
        page);
    xd_widget_set_page(xd_button(win, (xd_rect){150, 260, 130, 23}, "Color Management...",
                                 on_color_management, properties),
                       page);
}

static void build_effects(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_EFFECTS;
    /* XP's Effects dialog, its combo boxes under the boxes they belong to. */
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 40, 364, 166}, "Visual effects"), page);
    properties->transition_check =
        xd_checkbox(win, (xd_rect){32, 62, 340, 20},
                    "Use the following transition effect for menus and tooltips:",
                    properties->pending.menu_transitions, on_transition, properties);
    xd_widget_set_page(properties->transition_check, page);
    properties->transition_combo =
        xd_combo(win, (xd_rect){52, 84, 150, 21}, false, on_transition, properties);
    xd_widget_set_page(properties->transition_combo, page);
    xd_widget_add_item(properties->transition_combo, "Fade Effect");
    xd_widget_add_item(properties->transition_combo, "Scroll Effect");
    xd_widget_select(properties->transition_combo,
                     properties->pending.menu_transition == XD_TRANSITION_SCROLL ? 1 : 0);
    xd_widget_set_enabled(properties->transition_combo, properties->pending.menu_transitions);

    /* XP's Shell Icon Size: the desktop's icons and the XP Start menu's
     * program list. */
    properties->large_icons =
        xd_checkbox(win, (xd_rect){32, 112, 340, 20}, "Use large icons",
                    properties->pending.large_icons, on_large_icons, properties);
    xd_widget_set_page(properties->large_icons, page);
    /* Off: moving or sizing a window drags its outline (the compositor's). */
    xd_widget_set_page(xd_checkbox(win, (xd_rect){32, 136, 340, 20},
                                   "Show window contents while dragging",
                                   properties->pending.full_window_drag, on_full_drag, properties),
                       page);
    /* Check boxes draw one line: XP's wrapped label goes on in a label. */
    xd_widget_set_page(xd_checkbox(win, (xd_rect){32, 160, 340, 20},
                                   "Hide underlined letters for keyboard navigation until I press",
                                   properties->pending.hide_keyboard_cues, on_keyboard_cues,
                                   properties),
                       page);
    xd_widget_set_page(xd_label(win, (xd_rect){50, 178, 322, 16}, "the Alt key", 0), page);

    /* Windows had one choice here ("Smooth edges of screen fonts"); these
     * are FreeType's, as fontconfig and cairo take them, and go to other
     * programs too (GNOME's settings and GTK's settings.ini). */
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 214, 364, 112}, "Font smoothing"), page);
    const xd_font_smoothing *fs = &properties->pending.font_smoothing;
    xd_widget_set_page(xd_label(win, (xd_rect){32, 239, 130, 16}, "Antialiasing:", 0), page);
    properties->antialias_combo =
        xd_combo(win, (xd_rect){164, 236, 208, 21}, false, on_smoothing, properties);
    xd_widget_set_page(properties->antialias_combo, page);
    for (size_t i = 0; i < XD_ARRAY_LEN(antialias_labels); i++) {
        xd_widget_add_item(properties->antialias_combo, antialias_labels[i]);
        if (antialias_items[i] == fs->antialias)
            xd_widget_select(properties->antialias_combo, (int)i);
    }
    xd_widget_set_page(xd_label(win, (xd_rect){32, 269, 130, 16}, "Subpixel order:", 0), page);
    properties->subpixel_combo =
        xd_combo(win, (xd_rect){164, 266, 208, 21}, false, on_smoothing, properties);
    xd_widget_set_page(properties->subpixel_combo, page);
    for (size_t i = 0; i < XD_ARRAY_LEN(subpixel_labels); i++)
        xd_widget_add_item(properties->subpixel_combo, subpixel_labels[i]);
    xd_widget_select(properties->subpixel_combo, (int)fs->order);
    xd_widget_set_enabled(properties->subpixel_combo, fs->antialias == XD_SMOOTH_SUBPIXEL);
    xd_widget_set_page(xd_label(win, (xd_rect){32, 299, 130, 16}, "Hinting:", 0), page);
    properties->hinting_combo =
        xd_combo(win, (xd_rect){164, 296, 208, 21}, false, on_smoothing, properties);
    xd_widget_set_page(properties->hinting_combo, page);
    for (size_t i = 0; i < XD_ARRAY_LEN(hinting_labels); i++) {
        xd_widget_add_item(properties->hinting_combo, hinting_labels[i]);
        if (hinting_items[i] == fs->hinting)
            xd_widget_select(properties->hinting_combo, (int)i);
    }

    /* How a theme package's pictures are scaled up for high DPI
     * (docs/THEMES.md): Windows only stretched them. */
    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 334, 364, 66}, "Theme images"), page);
    xd_widget_set_page(xd_label(win, (xd_rect){32, 357, 130, 16}, "Scale theme images:", 0), page);
    properties->scaling_combo =
        xd_combo(win, (xd_rect){164, 354, 208, 21}, false, on_scaling, properties);
    xd_widget_set_page(properties->scaling_combo, page);
    xd_widget_add_item(properties->scaling_combo, "Automatic");
    for (int filter_index = 0; filter_index < XD_FILTER_COUNT; filter_index++)
        xd_widget_add_item(properties->scaling_combo,
                           xd_image_filter_name((xd_image_filter)filter_index));
    xd_widget_select(properties->scaling_combo, properties->pending.image_scaling == XD_FILTER_COUNT
                                                    ? 0
                                                    : (int)properties->pending.image_scaling + 1);
    xd_widget_set_page(xd_checkbox(win, (xd_rect){32, 378, 340, 20},
                                   "Use whole-number scaling when the scale is a whole number",
                                   properties->pending.integer_scaling, on_integer_scaling,
                                   properties),
                       page);
    build_themes(properties);
}

/* ---- Settings ------------------------------------------------------------------ */

static const double scales[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.25, 2.5, 3.0};
static const char *const orientations[] = {"Landscape", "Portrait", "Landscape (flipped)",
                                           "Portrait (flipped)"};

static void free_heads(disp_head *heads, size_t count) {
    for (size_t i = 0; i < count; i++)
        xd_free(heads[i].modes);
    xd_free(heads);
}

static disp_head *copy_heads(const disp_head *src, size_t count) {
    disp_head *out = xd_alloc_array(XD_MAX(count, 1), sizeof(*out));
    for (size_t i = 0; i < count; i++) {
        out[i] = src[i];
        out[i].modes = xd_alloc_array(XD_MAX(src[i].mode_count, 1), sizeof(*out[i].modes));
        memcpy(out[i].modes, src[i].modes, src[i].mode_count * sizeof(*src[i].modes));
    }
    return out;
}

/* Reads the compositor's current configuration into d->heads. */
static void load_heads(desk *properties) {
    free_heads(properties->heads, properties->head_count);
    properties->heads = nullptr;
    properties->head_count = 0;
    const xd_display_head *heads = nullptr;
    size_t count = 0;
    if (!xd_ui_display_heads(properties->ui, &heads, &count))
        return;
    properties->heads = xd_alloc_array(XD_MAX(count, 1), sizeof(*properties->heads));
    for (size_t i = 0; i < count; i++) {
        const xd_display_head *display_head = &heads[i];
        disp_head *dh = &properties->heads[i];
        *dh = (disp_head){0};
        snprintf(dh->set.name, sizeof(dh->set.name), "%s", display_head->name);
        snprintf(dh->description, sizeof(dh->description), "%s", display_head->description);
        dh->set.enabled = display_head->enabled;
        dh->set.x = display_head->x;
        dh->set.y = display_head->y;
        dh->set.scale = display_head->scale > 0 ? display_head->scale : 1.0;
        dh->set.transform = display_head->transform;
        dh->set.adaptive_sync = display_head->adaptive_sync;
        dh->mode_count = display_head->mode_count;
        dh->modes = xd_alloc_array(XD_MAX(display_head->mode_count, 1), sizeof(*dh->modes));
        memcpy(dh->modes, display_head->modes,
               display_head->mode_count * sizeof(*display_head->modes));
        dh->set.mode = display_head->mode;
        if (dh->set.mode < 0) {
            /* Off (or a custom mode): offer the preferred one. */
            dh->set.mode = 0;
            for (size_t mode_index = 0; mode_index < display_head->mode_count; mode_index++) {
                if (display_head->modes[mode_index].preferred)
                    dh->set.mode = (int)mode_index;
            }
        }
    }
    properties->head_count = count;
}

static disp_head *sel_head(desk *properties) {
    if (properties->sel < 0 || (size_t)properties->sel >= properties->head_count)
        return nullptr;
    return &properties->heads[properties->sel];
}

static const xd_display_mode *head_mode(const disp_head *head) {
    if (head->set.mode < 0 || (size_t)head->set.mode >= head->mode_count)
        return nullptr;
    return &head->modes[head->set.mode];
}

/* Size on the desktop, in logical units. */
static void head_size(const disp_head *head, int *width, int *hh) {
    const xd_display_mode *display_mode = head_mode(head);
    int pw = display_mode != nullptr ? display_mode->width : 1024;
    int ph = display_mode != nullptr ? display_mode->height : 768;
    if (head->set.transform == XD_TRANSFORM_90 || head->set.transform == XD_TRANSFORM_270) {
        int tmp = pw;
        pw = ph;
        ph = tmp;
    }
    double scale = head->set.scale > 0 ? head->set.scale : 1.0;
    *width = (int)lround(pw / scale);
    *hh = (int)lround(ph / scale);
}

static bool heads_equal(const disp_head *left, const disp_head *right, size_t count) {
    for (size_t i = 0; i < count; i++) {
        const xd_display_setting *left_setting = &left[i].set;
        const xd_display_setting *right_setting = &right[i].set;
        if (left_setting->enabled != right_setting->enabled ||
            (left_setting->enabled &&
             (left_setting->mode != right_setting->mode || left_setting->x != right_setting->x ||
              left_setting->y != right_setting->y || left_setting->scale != right_setting->scale ||
              left_setting->transform != right_setting->transform ||
              left_setting->adaptive_sync != right_setting->adaptive_sync)))
            return false;
    }
    return true;
}

static void display_changed(desk *properties) {
    properties->display_dirty =
        properties->saved_count != properties->head_count ||
        !heads_equal(properties->heads, properties->saved_heads, properties->head_count);
    xd_widget_redraw(properties->monitors);
    changed(properties);
}

/* Moves the layout so it starts at (0, 0), as the compositor would. */
static void normalize(desk *properties) {
    int minx = INT32_MAX;
    int miny = INT32_MAX;
    for (size_t i = 0; i < properties->head_count; i++) {
        if (!properties->heads[i].set.enabled)
            continue;
        minx = XD_MIN(minx, properties->heads[i].set.x);
        miny = XD_MIN(miny, properties->heads[i].set.y);
    }
    if (minx == INT32_MAX)
        return;
    for (size_t i = 0; i < properties->head_count; i++) {
        properties->heads[i].set.x -= minx;
        properties->heads[i].set.y -= miny;
    }
}

/* After a monitor changes size, monitors beyond its right or bottom edge
 * follow it so the desktop stays joined up. */
static void reflow(desk *properties, size_t changed_i, int old_w, int old_h) {
    disp_head *resized = &properties->heads[changed_i];
    int width;
    int height;
    head_size(resized, &width, &height);
    int right = resized->set.x + old_w;
    int bottom = resized->set.y + old_h;
    for (size_t i = 0; i < properties->head_count; i++) {
        disp_head *other = &properties->heads[i];
        if (i == changed_i || !other->set.enabled)
            continue;
        if (other->set.x >= right)
            other->set.x += width - old_w;
        if (other->set.y >= bottom)
            other->set.y += height - old_h;
    }
    normalize(properties);
}

/* Distinct resolutions of a head, smallest first. */
static size_t resolutions(const disp_head *head, int *ws, int *hs, size_t cap) {
    size_t count = 0;
    for (size_t mode_index = 0; mode_index < head->mode_count; mode_index++) {
        bool seen = false;
        for (size_t j = 0; j < count; j++)
            seen = seen || (ws[j] == head->modes[mode_index].width &&
                            hs[j] == head->modes[mode_index].height);
        if (!seen && count < cap) {
            ws[count] = head->modes[mode_index].width;
            hs[count] = head->modes[mode_index].height;
            count++;
        }
    }
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0 && (long)ws[j] * hs[j] < (long)ws[j - 1] * hs[j - 1]; j--) {
            int tw = ws[j];
            int th = hs[j];
            ws[j] = ws[j - 1];
            hs[j] = hs[j - 1];
            ws[j - 1] = tw;
            hs[j - 1] = th;
        }
    }
    return count;
}

static void show_head(desk *properties);

static void set_mode_keeping_layout(desk *properties, int mode) {
    disp_head *head = sel_head(properties);
    if (head == nullptr || mode < 0 || (size_t)mode >= head->mode_count || mode == head->set.mode)
        return;
    int ow;
    int oh;
    head_size(head, &ow, &oh);
    head->set.mode = mode;
    reflow(properties, (size_t)properties->sel, ow, oh);
}

static void on_resolution(xd_widget *widget, void *user) {
    desk *properties = user;
    disp_head *head = sel_head(properties);
    if (head == nullptr)
        return;
    int ws[128];
    int hs[128];
    size_t count = resolutions(head, ws, hs, XD_ARRAY_LEN(ws));
    int i = xd_slider_value(widget);
    if (i < 0 || (size_t)i >= count)
        return;
    /* That size at the refresh rate closest to the current one. */
    const xd_display_mode *cur = head_mode(head);
    int want = cur != nullptr ? cur->refresh_mhz : 60000;
    int best = -1;
    for (size_t mode_index = 0; mode_index < head->mode_count; mode_index++) {
        if (head->modes[mode_index].width != ws[i] || head->modes[mode_index].height != hs[i])
            continue;
        if (best < 0 || abs(head->modes[mode_index].refresh_mhz - want) <
                            abs(head->modes[best].refresh_mhz - want))
            best = (int)mode_index;
    }
    set_mode_keeping_layout(properties, best);
    show_head(properties);
    display_changed(properties);
}

static void on_refresh(xd_widget *widget, void *user) {
    desk *properties = user;
    int i = xd_widget_selected(widget);
    if (i >= 0 && (size_t)i < properties->refresh_count)
        set_mode_keeping_layout(properties, properties->refresh_modes[i]);
    display_changed(properties);
}

static void on_scale(xd_widget *widget, void *user) {
    desk *properties = user;
    disp_head *head = sel_head(properties);
    int i = xd_widget_selected(widget);
    if (head == nullptr || i < 0 || (size_t)i >= XD_ARRAY_LEN(scales))
        return;
    int ow;
    int oh;
    head_size(head, &ow, &oh);
    head->set.scale = scales[i];
    reflow(properties, (size_t)properties->sel, ow, oh);
    display_changed(properties);
}

static void on_orientation(xd_widget *widget, void *user) {
    desk *properties = user;
    disp_head *head = sel_head(properties);
    int i = xd_widget_selected(widget);
    if (head == nullptr || i < 0 || i > 3)
        return;
    int ow;
    int oh;
    head_size(head, &ow, &oh);
    head->set.transform = (xd_display_transform)i;
    reflow(properties, (size_t)properties->sel, ow, oh);
    display_changed(properties);
}

static size_t enabled_count(const desk *properties) {
    size_t count = 0;
    for (size_t i = 0; i < properties->head_count; i++)
        count += properties->heads[i].set.enabled ? 1 : 0;
    return count;
}

static void on_extend(xd_widget *widget, void *user) {
    desk *properties = user;
    disp_head *head = sel_head(properties);
    if (head == nullptr)
        return;
    bool on = xd_checkbox_checked(widget);
    if (!on && enabled_count(properties) <= 1) {
        xd_checkbox_set(widget, true); /* the last monitor stays on */
        return;
    }
    if (on && !head->set.enabled) {
        /* Joins the desktop on the right. */
        int right = 0;
        for (size_t i = 0; i < properties->head_count; i++) {
            if (!properties->heads[i].set.enabled)
                continue;
            int hw;
            int hh;
            head_size(&properties->heads[i], &hw, &hh);
            right = XD_MAX(right, properties->heads[i].set.x + hw);
        }
        head->set.x = right;
        head->set.y = 0;
    }
    head->set.enabled = on;
    normalize(properties);
    show_head(properties);
    display_changed(properties);
}

/* "60 Hertz", as XP's refresh rates. */
static void format_rate(int mhz, char *buf, size_t cap) {
    if (mhz > 0)
        snprintf(buf, cap, "%.3g Hertz", mhz / 1000.0);
    else
        snprintf(buf, cap, "Default");
}

/* Fills the controls from the selected monitor. */
static void show_head(desk *properties) {
    disp_head *head = sel_head(properties);
    bool have = head != nullptr;
    xd_widget *controls[] = {properties->res_slider,    properties->scale_combo,
                             properties->refresh_combo, properties->orient_combo,
                             properties->extend_check,  properties->display_advanced};
    for (size_t i = 0; i < XD_ARRAY_LEN(controls); i++)
        xd_widget_set_enabled(
            controls[i], have && (head->set.enabled || controls[i] == properties->extend_check));
    if (!have) {
        xd_widget_set_text(properties->res_label, "");
        return;
    }
    int ws[128];
    int hs[128];
    size_t count = resolutions(head, ws, hs, XD_ARRAY_LEN(ws));
    const xd_display_mode *cur = head_mode(head);
    int idx = 0;
    for (size_t i = 0; i < count; i++) {
        if (cur != nullptr && ws[i] == cur->width && hs[i] == cur->height)
            idx = (int)i;
    }
    xd_slider_set(properties->res_slider, 0, XD_MAX((int)count - 1, 0), idx);
    char buf[128];
    if (cur != nullptr)
        snprintf(buf, sizeof(buf), "%d by %d pixels", cur->width, cur->height);
    else
        snprintf(buf, sizeof(buf), "Unknown");
    xd_widget_set_text(properties->res_label, buf);

    /* Refresh rates of this resolution, fastest first. */
    xd_widget_clear_items(properties->refresh_combo);
    properties->refresh_count = 0;
    for (size_t mode_index = 0; mode_index < head->mode_count &&
                                properties->refresh_count < XD_ARRAY_LEN(properties->refresh_modes);
         mode_index++) {
        if (cur == nullptr || head->modes[mode_index].width != cur->width ||
            head->modes[mode_index].height != cur->height)
            continue;
        size_t at = properties->refresh_count++;
        while (at > 0 && head->modes[properties->refresh_modes[at - 1]].refresh_mhz <
                             head->modes[mode_index].refresh_mhz) {
            properties->refresh_modes[at] = properties->refresh_modes[at - 1];
            at--;
        }
        properties->refresh_modes[at] = (int)mode_index;
    }
    int rsel = 0;
    for (size_t i = 0; i < properties->refresh_count; i++) {
        format_rate(head->modes[properties->refresh_modes[i]].refresh_mhz, buf, sizeof(buf));
        xd_widget_add_item(properties->refresh_combo, buf);
        if (properties->refresh_modes[i] == head->set.mode)
            rsel = (int)i;
    }
    if (properties->refresh_count == 0)
        xd_widget_add_item(properties->refresh_combo, "Default");
    xd_widget_select(properties->refresh_combo, rsel);

    int ssel = 0;
    for (size_t i = 0; i < XD_ARRAY_LEN(scales); i++) {
        if (fabs(scales[i] - head->set.scale) < fabs(scales[ssel] - head->set.scale))
            ssel = (int)i;
    }
    xd_widget_select(properties->scale_combo, ssel);
    xd_widget_select(properties->orient_combo, (int)head->set.transform);
    xd_checkbox_set(properties->extend_check, head->set.enabled);
    xd_widget_set_enabled(properties->extend_check,
                          !head->set.enabled || enabled_count(properties) > 1);
}

/* The monitor map: desktop layout fitted into the widget (logical units). */
typedef struct map_geom {
    double s, ox, oy;
    int minx, miny;
} map_geom;

static xd_rect head_map_rect(const desk *properties, size_t i, const map_geom *geometry,
                             int *disabled_x) {
    const disp_head *head = &properties->heads[i];
    int width;
    int hh;
    head_size(head, &width, &hh);
    int x = head->set.x;
    int y = head->set.y;
    if (!head->set.enabled) {
        /* Off monitors sit beside the desktop, greyed. */
        x = *disabled_x;
        y = 0;
        *disabled_x += width + 200;
    }
    return (xd_rect){(int)lround(geometry->ox + (x - geometry->minx) * geometry->s),
                     (int)lround(geometry->oy + (y - geometry->miny) * geometry->s),
                     (int)lround(width * geometry->s), (int)lround(hh * geometry->s)};
}

static map_geom map_geometry(const desk *properties, int width, int height, int *first_disabled_x) {
    map_geom geometry = {.s = 1};
    int minx = INT32_MAX;
    int miny = INT32_MAX;
    int maxx = INT32_MIN;
    int maxy = INT32_MIN;
    for (size_t i = 0; i < properties->head_count; i++) {
        const disp_head *head = &properties->heads[i];
        if (!head->set.enabled)
            continue;
        int head_width;
        int hh;
        head_size(head, &head_width, &hh);
        minx = XD_MIN(minx, head->set.x);
        miny = XD_MIN(miny, head->set.y);
        maxx = XD_MAX(maxx, head->set.x + head_width);
        maxy = XD_MAX(maxy, head->set.y + hh);
    }
    if (minx == INT32_MAX) {
        minx = 0;
        miny = 0;
        maxx = 1024;
        maxy = 768;
    }
    *first_disabled_x = maxx + 200;
    int extra = maxx;
    for (size_t i = 0; i < properties->head_count; i++) {
        if (properties->heads[i].set.enabled)
            continue;
        int head_width;
        int hh;
        head_size(&properties->heads[i], &head_width, &hh);
        extra += 200 + head_width;
        maxy = XD_MAX(maxy, miny + hh);
    }
    maxx = extra;
    double span_w = XD_MAX(maxx - minx, 1);
    double span_h = XD_MAX(maxy - miny, 1);
    geometry.s = fmin((width - 20) / span_w, (height - 20) / span_h);
    geometry.ox = (width - span_w * geometry.s) / 2;
    geometry.oy = (height - span_h * geometry.s) / 2;
    geometry.minx = minx;
    geometry.miny = miny;
    return geometry;
}

static void draw_monitors(xd_widget *widget, cairo_t *cairo, xd_rect bounds, const xd_theme *theme,
                          void *user) {
    (void)widget;
    desk *properties = user;
    xd_fill_rect(cairo, bounds, xd_theme_tab_page_color(theme));
    if (properties->head_count == 0)
        return;
    int disabled_x = 0;
    map_geom geometry = map_geometry(properties, properties->monitors_rect.w,
                                     properties->monitors_rect.h, &disabled_x);
    xd_font_spec big = theme->scheme.fonts[XD_FONT_CAPTION];
    big.size_pt *= 2.2;
    double scale = theme->scale;
    for (size_t pass = 0; pass < 2; pass++) {
        int dx = disabled_x;
        for (size_t i = 0; i < properties->head_count; i++) {
            /* The selected monitor last, on top while dragged. */
            bool selected = (int)i == properties->sel;
            xd_rect head_rect = head_map_rect(properties, i, &geometry, &dx);
            if (selected != (pass == 1))
                continue;
            if (selected && properties->dragging) {
                head_rect.x += (int)lround(properties->drag_dx);
                head_rect.y += (int)lround(properties->drag_dy);
            }
            xd_rect dm = {bounds.x + (int)lround(head_rect.x * scale),
                          bounds.y + (int)lround(head_rect.y * scale),
                          (int)lround(head_rect.w * scale), (int)lround(head_rect.h * scale)};
            bool on = properties->heads[i].set.enabled;
            xd_fill_rect(cairo, dm, on ? xd_rgba_hex(0x3A6EA5) : xd_rgba_hex(0xA0A0A0));
            xd_draw_bevel(cairo, dm, xd_tpx(theme, selected ? 2 : 1),
                          selected ? xd_rgba_hex(0x000000) : xd_rgba_hex(0xFFFFFF),
                          xd_rgba_hex(0x000000));
            char num[24];
            snprintf(num, sizeof(num), "%zu", i + 1);
            xd_text_style st =
                xd_text(theme, XD_FONT_CAPTION, on ? xd_rgba_hex(0xFFFFFF) : xd_rgba_hex(0x606060),
                        XD_ALIGN_CENTER);
            st.font = &big;
            xd_draw_text(cairo, dm, num, &st);
        }
    }
}

/* Places a dragged monitor against the nearest edge of another, as
 * Windows does: monitors always touch. */
static void snap(desk *properties, size_t moved, int x, int y) {
    disp_head *moved_head = &properties->heads[moved];
    int mw;
    int mh;
    head_size(moved_head, &mw, &mh);
    long best = LONG_MAX;
    int bx = x;
    int by = y;
    for (size_t i = 0; i < properties->head_count; i++) {
        const disp_head *other = &properties->heads[i];
        if (i == moved || !other->set.enabled)
            continue;
        int ow;
        int oh;
        head_size(other, &ow, &oh);
        int cy = XD_CLAMP(y, other->set.y - mh + 1, other->set.y + oh - 1);
        int cx = XD_CLAMP(x, other->set.x - mw + 1, other->set.x + ow - 1);
        /* Line edges up when they're close. */
        if (abs(cy - other->set.y) < 64)
            cy = other->set.y;
        if (abs(cx - other->set.x) < 64)
            cx = other->set.x;
        int cand[4][2] = {{other->set.x + ow, cy},
                          {other->set.x - mw, cy},
                          {cx, other->set.y + oh},
                          {cx, other->set.y - mh}};
        for (size_t candidate = 0; candidate < 4; candidate++) {
            long dx = cand[candidate][0] - x;
            long dy = cand[candidate][1] - y;
            long dist = dx * dx + dy * dy;
            if (dist < best) {
                best = dist;
                bx = cand[candidate][0];
                by = cand[candidate][1];
            }
        }
    }
    moved_head->set.x = bx;
    moved_head->set.y = by;
    normalize(properties);
}

static void on_monitors_pointer([[maybe_unused]] xd_widget *widget, xd_pointer_event ev, double x,
                                double y, void *user) {
    desk *properties = user;
    int disabled_x = 0;
    map_geom geometry = map_geometry(properties, properties->monitors_rect.w,
                                     properties->monitors_rect.h, &disabled_x);
    if (ev == XD_POINTER_PRESS) {
        int dx = disabled_x;
        for (size_t i = 0; i < properties->head_count; i++) {
            xd_rect rect = head_map_rect(properties, i, &geometry, &dx);
            if (x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h) {
                properties->sel = (int)i;
                xd_widget_select(properties->display_combo, properties->sel);
                show_head(properties);
                properties->dragging =
                    properties->heads[i].set.enabled && enabled_count(properties) > 1;
                properties->drag_x0 = x;
                properties->drag_y0 = y;
                properties->drag_dx = properties->drag_dy = 0;
            }
        }
        xd_widget_redraw(properties->monitors);
        return;
    }
    if (!properties->dragging)
        return;
    properties->drag_dx = x - properties->drag_x0;
    properties->drag_dy = y - properties->drag_y0;
    if (ev == XD_POINTER_RELEASE) {
        properties->dragging = false;
        disp_head *head = sel_head(properties);
        if (head != nullptr && (fabs(properties->drag_dx) > 2 || fabs(properties->drag_dy) > 2)) {
            int nx = head->set.x + (int)lround(properties->drag_dx / geometry.s);
            int ny = head->set.y + (int)lround(properties->drag_dy / geometry.s);
            snap(properties, (size_t)properties->sel, nx, ny);
            display_changed(properties);
        }
    }
    xd_widget_redraw(properties->monitors);
}

static void on_display(xd_widget *widget, void *user) {
    desk *properties = user;
    properties->sel = xd_widget_selected(widget);
    show_head(properties);
    xd_widget_redraw(properties->monitors);
}

static void fill_display_combo(desk *properties) {
    xd_widget_clear_items(properties->display_combo);
    for (size_t i = 0; i < properties->head_count; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%zu. %s on %s", i + 1,
                 properties->heads[i].description[0] != '\0' ? properties->heads[i].description
                                                             : "Monitor",
                 properties->heads[i].set.name);
        xd_widget_add_item(properties->display_combo, buf);
    }
    if (properties->sel < 0 || (size_t)properties->sel >= properties->head_count)
        properties->sel = properties->head_count > 0 ? 0 : -1;
    xd_widget_select(properties->display_combo, properties->sel);
}

/* The compositor reported a new configuration (ours applied, a monitor
 * plugged in, or another program's change). */
static void on_display_change(void *user) {
    desk *properties = user;
    if (properties->display_dirty || properties->confirm != nullptr)
        return; /* keep the user's unapplied changes */
    load_heads(properties);
    if (properties->saved_heads == nullptr || properties->saved_count != properties->head_count) {
        free_heads(properties->saved_heads, properties->saved_count);
        properties->saved_heads = copy_heads(properties->heads, properties->head_count);
        properties->saved_count = properties->head_count;
    }
    fill_display_combo(properties);
    show_head(properties);
    xd_widget_redraw(properties->monitors);
}

/* ---- Advanced (the monitor's properties) ---------------------------------------- */

static bool apply(desk *properties);

static void on_monitor_done(monitor_result outcome, const monitor_props *monitor_settings,
                            void *user) {
    desk *properties = user;
    if (outcome != MONITOR_APPLY)
        properties->monitor = nullptr;
    if (monitor_settings == nullptr)
        return;
    /* The monitor it was opened for, wherever the selection is now. */
    int was = properties->sel;
    for (size_t i = 0; i < properties->head_count; i++) {
        if (strcmp(properties->heads[i].set.name, properties->monitor_name) == 0)
            properties->sel = (int)i;
    }
    disp_head *head = sel_head(properties);
    if (head != nullptr && strcmp(head->set.name, properties->monitor_name) == 0) {
        if (monitor_settings->rate >= 0 &&
            (size_t)monitor_settings->rate < properties->monitor_mode_count)
            set_mode_keeping_layout(properties, properties->monitor_modes[monitor_settings->rate]);
        head->set.adaptive_sync = monitor_settings->vrr;
    }
    properties->sel = was;
    show_head(properties);
    display_changed(properties);
    if (outcome == MONITOR_APPLY)
        apply(properties);
}

/* The compositor answered whether the monitor could have a variable
 * refresh rate: now the dialog can show it. */
static void on_vrr_probed(bool ok, void *user) {
    desk *properties = user;
    properties->monitor_probing = false;
    disp_head *head = sel_head(properties);
    if (head == nullptr || strcmp(head->set.name, properties->monitor_name) != 0 ||
        properties->monitor != nullptr)
        return;
    const char *rates[XD_ARRAY_LEN(properties->refresh_modes)];
    char labels[XD_ARRAY_LEN(properties->refresh_modes)][32];
    monitor_props props = {.vrr = head->set.adaptive_sync};
    properties->monitor_mode_count = properties->refresh_count;
    for (size_t i = 0; i < properties->refresh_count; i++) {
        properties->monitor_modes[i] = properties->refresh_modes[i];
        format_rate(head->modes[properties->refresh_modes[i]].refresh_mhz, labels[i],
                    sizeof(labels[i]));
        rates[i] = labels[i];
        if (properties->refresh_modes[i] == head->set.mode)
            props.rate = (int)i;
    }
    if (properties->refresh_count == 0) {
        rates[0] = "Default";
        properties->monitor_mode_count = 0;
    }
    properties->monitor = monitor_dialog_open(
        properties->ui, head->description[0] != '\0' ? head->description : "Monitor", rates,
        XD_MAX(properties->refresh_count, 1), props, ok || head->set.adaptive_sync, on_monitor_done,
        properties);
}

/* Advanced: first asks the compositor (a test configuration, changing
 * nothing) whether this monitor would take a variable refresh rate. */
static void on_display_advanced([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    disp_head *head = sel_head(properties);
    if (head == nullptr || !head->set.enabled || properties->monitor != nullptr ||
        properties->monitor_probing)
        return;
    snprintf(properties->monitor_name, sizeof(properties->monitor_name), "%s", head->set.name);
    xd_display_setting *sets = xd_alloc_array(XD_MAX(properties->head_count, 1), sizeof(*sets));
    for (size_t i = 0; i < properties->head_count; i++)
        sets[i] = properties->heads[i].set;
    sets[properties->sel].adaptive_sync = true;
    properties->monitor_probing = true;
    xd_ui_display_test(properties->ui, sets, properties->head_count, on_vrr_probed, properties);
    xd_free(sets);
}

/* Identify: the shell shows each monitor's number on that monitor, as
 * this tab numbers them (monitors that are off show nothing). */
static void on_identify([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("a(su)"));
    for (size_t i = 0; i < properties->head_count; i++) {
        if (properties->heads[i].set.enabled)
            g_variant_builder_add(&builder, "(su)", properties->heads[i].set.name,
                                  (guint32)(i + 1));
    }
    GVariant *args = g_variant_new("(a(su))", &builder);
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
    if (bus == nullptr) {
        g_variant_unref(g_variant_ref_sink(args));
        return;
    }
    /* The shell answers at once; waiting for it keeps the call from being
     * lost with the connection. */
    GVariant *ret = g_dbus_connection_call_sync(
        bus, "org.xeondesktop.Shell", "/org/xeondesktop/Shell", "org.xeondesktop.Shell",
        "Identify", args, nullptr, G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, nullptr);
    if (ret != nullptr)
        g_variant_unref(ret);
    g_object_unref(bus);
}

static void build_settings(desk *properties) {
    xd_window *win = properties->win;
    const int page = PAGE_SETTINGS;
    xd_widget_set_page(xd_label(win, (xd_rect){20, 40, 364, 28},
                                "Drag the monitor icons to match the physical arrangement of your "
                                "monitors.",
                                XD_LABEL_WRAP),
                       page);
    properties->monitors_rect = (xd_rect){20, 70, 364, 112};
    properties->monitors = xd_custom(win, properties->monitors_rect, draw_monitors, properties);
    xd_custom_set_pointer(properties->monitors, on_monitors_pointer);
    xd_widget_set_page(properties->monitors, page);
    xd_widget_set_page(xd_label(win, (xd_rect){20, 192, 50, 16}, "Display:", 0), page);
    properties->display_combo =
        xd_combo(win, (xd_rect){72, 189, 312, 21}, false, on_display, properties);
    xd_widget_set_page(properties->display_combo, page);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){20, 218, 176, 82}, "Screen area"), page);
    xd_widget_set_page(xd_label(win, (xd_rect){30, 240, 30, 16}, "Less", 0), page);
    xd_widget_set_page(xd_label(win, (xd_rect){156, 240, 32, 16}, "More", 0), page);
    properties->res_slider =
        xd_slider(win, (xd_rect){58, 236, 98, 30}, 0, 0, 0, on_resolution, properties);
    xd_widget_set_page(properties->res_slider, page);
    properties->res_label = xd_label(win, (xd_rect){30, 274, 158, 16}, "", 0);
    xd_widget_set_page(properties->res_label, page);

    xd_widget_set_page(xd_groupbox(win, (xd_rect){206, 218, 178, 82}, "Scale"), page);
    properties->scale_combo =
        xd_combo(win, (xd_rect){218, 240, 154, 21}, false, on_scale, properties);
    xd_widget_set_page(properties->scale_combo, page);
    for (size_t i = 0; i < XD_ARRAY_LEN(scales); i++) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d%%%s", (int)lround(scales[i] * 100),
                 i == 0 ? " (normal size)" : "");
        xd_widget_add_item(properties->scale_combo, buf);
    }
    xd_widget_set_page(xd_label(win, (xd_rect){218, 266, 160, 30}, "Makes text and windows larger.",
                                XD_LABEL_WRAP | XD_LABEL_DIM),
                       page);

    xd_widget_set_page(xd_label(win, (xd_rect){20, 312, 80, 16}, "Refresh rate:", 0), page);
    properties->refresh_combo =
        xd_combo(win, (xd_rect){104, 309, 92, 21}, false, on_refresh, properties);
    xd_widget_set_page(properties->refresh_combo, page);
    xd_widget_set_page(xd_label(win, (xd_rect){206, 312, 72, 16}, "Orientation:", 0), page);
    properties->orient_combo =
        xd_combo(win, (xd_rect){280, 309, 104, 21}, false, on_orientation, properties);
    xd_widget_set_page(properties->orient_combo, page);
    for (size_t i = 0; i < XD_ARRAY_LEN(orientations); i++)
        xd_widget_add_item(properties->orient_combo, orientations[i]);

    properties->extend_check =
        xd_checkbox(win, (xd_rect){20, 340, 280, 20}, "Extend my desktop onto this monitor.", true,
                    on_extend, properties);
    xd_widget_set_page(properties->extend_check, page);
    /* Windows 2000's row of buttons (Troubleshoot... has nothing to run). */
    xd_widget *identify =
        xd_button(win, (xd_rect){228, 368, 75, 23}, "Identify", on_identify, properties);
    xd_widget_set_page(identify, page);
    properties->display_advanced =
        xd_button(win, (xd_rect){309, 368, 75, 23}, "Advanced", on_display_advanced, properties);
    xd_widget_set_page(properties->display_advanced, page);
    properties->display_note =
        xd_label(win, (xd_rect){20, 364, 200, 36}, "", XD_LABEL_WRAP | XD_LABEL_DIM);
    xd_widget_set_page(properties->display_note, page);

    xd_ui_on_display_change(properties->ui, on_display_change, properties);
    properties->sel = 0;
    on_display_change(properties);
    if (properties->head_count == 0) {
        xd_widget_set_text(properties->display_note,
                           "This compositor doesn't let programs change display settings.");
        xd_widget_set_enabled(properties->display_combo, false);
        xd_widget_set_enabled(identify, false);
    }
}

/* ---- Monitor Settings confirmation -------------------------------------------- */

static void apply_display(desk *properties, const disp_head *heads, size_t count, bool confirm);

static void confirm_close(desk *properties) {
    if (properties->confirm_timer != nullptr) {
        xd_ui_remove_source(properties->confirm_timer);
        properties->confirm_timer = nullptr;
    }
    if (properties->confirm != nullptr) {
        xd_window *confirm_window = properties->confirm;
        properties->confirm = nullptr;
        xd_window_close(confirm_window);
    }
}

static void confirm_keep([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    free_heads(properties->saved_heads, properties->saved_count);
    properties->saved_heads = copy_heads(properties->heads, properties->head_count);
    properties->saved_count = properties->head_count;
    properties->display_dirty = false;
    confirm_close(properties);
    changed(properties);
    if (properties->close_after_apply)
        xd_window_close(properties->win);
}

static void confirm_revert([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    confirm_close(properties);
    properties->close_after_apply = false;
    /* Back to how it was; the change event then reloads the controls. */
    free_heads(properties->heads, properties->head_count);
    properties->heads = copy_heads(properties->saved_heads, properties->saved_count);
    properties->head_count = properties->saved_count;
    properties->display_dirty = false;
    apply_display(properties, properties->saved_heads, properties->saved_count, false);
    show_head(properties);
    changed(properties);
}

static void confirm_tick(void *user) {
    desk *properties = user;
    if (--properties->confirm_left <= 0) {
        confirm_revert(nullptr, properties); /* also removes this timer */
        return;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "Reverting to the previous settings in %d second%s.",
             properties->confirm_left, properties->confirm_left == 1 ? "" : "s");
    xd_widget_set_text(properties->confirm_label, buf);
}

static void confirm_open(desk *properties) {
    properties->confirm = xd_window_create(properties->ui, "Monitor Settings", "xd-desk", 330, 124);
    xd_image(properties->confirm, (xd_rect){14, 14, 32, 32}, "dialog-question");
    xd_label(properties->confirm, (xd_rect){58, 14, 260, 32},
             "Your desktop has been reconfigured. Do you want to keep these settings?",
             XD_LABEL_WRAP);
    properties->confirm_left = 15;
    properties->confirm_label =
        xd_label(properties->confirm, (xd_rect){58, 52, 260, 16},
                 "Reverting to the previous settings in 15 seconds.", XD_LABEL_DIM);
    xd_widget *yes =
        xd_button(properties->confirm, (xd_rect){84, 86, 75, 23}, "Yes", confirm_keep, properties);
    xd_widget *no = xd_button(properties->confirm, (xd_rect){171, 86, 75, 23}, "No", confirm_revert,
                              properties);
    xd_window_set_default(properties->confirm, yes);
    xd_window_set_cancel(properties->confirm, no);
    xd_window_set_focus(properties->confirm, yes);
    xd_window_show(properties->confirm);
    /* Removed by confirm_close (or the last tick). */
    properties->confirm_timer =
        xd_ui_add_timer(properties->ui, 1000, true, confirm_tick, properties);
}

typedef struct apply_ctx {
    desk *d;
    bool confirm;
} apply_ctx;

static void on_display_applied(bool ok, void *user) {
    apply_ctx *ctx = user;
    desk *properties = ctx->d;
    bool confirm = ctx->confirm;
    xd_free(ctx);
    if (!ok) {
        xd_widget_set_text(properties->display_note,
                           "The display settings couldn't be applied. The monitor may not "
                           "support them.");
        properties->close_after_apply = false;
        return;
    }
    xd_widget_set_text(properties->display_note, "");
    if (confirm)
        confirm_open(properties);
}

static void apply_display(desk *properties, const disp_head *heads, size_t count, bool confirm) {
    xd_display_setting *sets = xd_alloc_array(XD_MAX(count, 1), sizeof(*sets));
    for (size_t i = 0; i < count; i++)
        sets[i] = heads[i].set;
    apply_ctx *ctx = XD_NEW(apply_ctx);
    ctx->d = properties;
    ctx->confirm = confirm;
    xd_ui_display_apply(properties->ui, sets, count, on_display_applied, ctx);
    xd_free(sets);
}

/* ---- buttons ------------------------------------------------------------------- */

static bool apply(desk *properties) {
    if (properties->display_dirty && properties->confirm == nullptr)
        apply_display(properties, properties->heads, properties->head_count, true);
    if (!sounds_equal(&properties->saved_sounds, &properties->pending_sounds)) {
        /* A theme's sounds: sounds.ini, as Sounds and Audio Devices
         * writes it. */
        if (!xd_sound_scheme_save(&properties->pending_sounds))
            return false;
        copy_sounds(&properties->saved_sounds, &properties->pending_sounds);
        changed(properties);
    }
    if (xd_settings_equal(&properties->saved, &properties->pending))
        return true;
    /* Only what changed here: another program may have saved since. */
    xd_settings now;
    xd_settings_load(&now);
    xd_settings_merge(&properties->saved, &properties->pending, &now);
    if (!xd_settings_save(&now)) {
        fprintf(stderr, "xd-desk: cannot write settings\n");
        return false;
    }
    /* Themes chosen here also go where other desktops' settings tools put
     * them, for GTK programs that don't get ours through GTK_THEME (those
     * D-Bus or a terminal starts, GTK4's). Only when changed here, so that
     * themes set with those tools aren't overwritten behind the user's
     * back. */
    bool gtk = strcmp(properties->saved.gtk_theme, properties->pending.gtk_theme) != 0;
    bool icons = strcmp(properties->saved.icon_theme, properties->pending.icon_theme) != 0;
    if (gtk || icons)
        xd_gtk_settings_export(g_get_user_config_dir(), nullptr,
                               gtk ? xd_settings_gtk_theme(&now) : nullptr,
                               icons ? xd_settings_icon_theme(&now) : nullptr);
    properties->saved = properties->pending = now;
    /* Re-theme this dialog too, as Windows does after Apply. */
    xd_scheme scheme;
    xd_settings_scheme(&properties->pending, &scheme);
    xd_ui_set_theme(properties->ui, properties->pending.style, &scheme);
    changed(properties);
    return true;
}

static void on_ok([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    if (properties->confirm != nullptr || properties->items != nullptr ||
        properties->color != nullptr || properties->saver_setup != nullptr ||
        properties->adv != nullptr || properties->night != nullptr ||
        properties->monitor != nullptr || properties->monitor_probing)
        return; /* answer Monitor Settings, Desktop Items, Window Color, a saver's
                 * Settings, Advanced Appearance, Night Light or the monitor's
                 * properties first */
    if (properties->display_dirty) {
        /* Closes once the new display settings are kept. */
        properties->close_after_apply = true;
        apply(properties);
        return;
    }
    if (apply(properties))
        xd_window_close(properties->win);
}

static void on_cancel([[maybe_unused]] xd_widget *widget, void *user) {
    desk *properties = user;
    items_close(properties);
    if (properties->adv != nullptr)
        advanced_cancel(properties->adv);
    color_close(properties);
    if (properties->saver_setup != nullptr)
        saver_setup_cancel(properties->saver_setup);
    if (properties->night != nullptr)
        nightlight_dialog_cancel(properties->night);
    if (properties->monitor != nullptr)
        monitor_dialog_cancel(properties->monitor);
    xd_window_close(properties->win);
}

static void on_apply([[maybe_unused]] xd_widget *widget, void *user) { apply(user); }

int main(int argc, char **argv) {
    static desk properties;
    properties.chooser_fd = -1;
    properties.import_fd = -1;
    properties.ui = xd_ui_connect(argc, argv);
    if (properties.ui == nullptr)
        return EXIT_FAILURE;
    xd_settings_load(&properties.saved);
    properties.pending = properties.saved;
    xd_sound_scheme_load(&properties.saved_sounds);
    xd_sound_scheme_init(&properties.pending_sounds, XD_SOUNDS_DEFAULT);
    copy_sounds(&properties.pending_sounds, &properties.saved_sounds);
    xd_scheme_list_load(&properties.schemes);
    load_wallpapers(&properties);

    int page = PAGE_THEMES; /* XP's desk.cpl opens on Themes */
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--page") == 0) {
            int64_t parsed;
            if (xd_str_parse_i64(xd_str_from_cstr(argv[i + 1]), 0, PAGE_LINUX, &parsed))
                page = (int)parsed;
        }
    }

    properties.win = xd_window_create(properties.ui, "Display Properties", "xd-desk", 404, 448);
    /* XP's tabs (Windows 2000's Background is its Desktop), 2000's Effects, and
     * one for what only Linux has. */
    static const char *const tabs[] = {"Themes",  "Desktop",  "Screen Saver", "Appearance",
                                       "Effects", "Settings", "Linux"};
    xd_widget *tab = xd_tabs(properties.win, (xd_rect){7, 7, 390, 401}, tabs, XD_ARRAY_LEN(tabs),
                             on_page, &properties);
    build_background(&properties);
    build_screensaver(&properties);
    build_appearance(&properties);
    build_theme_tab(&properties);
    build_effects(&properties);
    build_settings(&properties);
    xd_widget_select(tab, page);
    properties.page = page;
    update_animation(&properties);

    xd_widget *ok =
        xd_button(properties.win, (xd_rect){160, 416, 75, 23}, "OK", on_ok, &properties);
    xd_widget *cancel =
        xd_button(properties.win, (xd_rect){241, 416, 75, 23}, "Cancel", on_cancel, &properties);
    properties.apply =
        xd_button(properties.win, (xd_rect){322, 416, 75, 23}, "Apply", on_apply, &properties);
    xd_window_set_default(properties.win, ok);
    xd_window_set_cancel(properties.win, cancel);
    xd_window_set_focus(properties.win, tab);
    changed(&properties);
    xd_window_show(properties.win);

    xd_ui_run(properties.ui);
    xd_ui_destroy(properties.ui);
    if (properties.preview_image != nullptr)
        cairo_surface_destroy(properties.preview_image);
    if (properties.preview_saver != nullptr)
        xd_saver_destroy(properties.preview_saver);
    if (properties.chooser != nullptr)
        close_stdout(&properties.chooser);
    if (properties.import != nullptr) {
        /* Closed mid-import: the half-written package goes. */
        g_subprocess_force_exit(properties.import);
        close_stdout(&properties.import);
        (void)g_remove(properties.import_part);
    }
    xd_strbuf_free(&properties.import_out);
    g_free(properties.import_source);
    g_free(properties.import_part);
    g_free(properties.import_dest);
    g_free(properties.import_name);
    g_ptr_array_unref(properties.packages);
    g_array_unref(properties.theme_items);
    xd_sound_scheme_free(&properties.saved_sounds);
    xd_sound_scheme_free(&properties.pending_sounds);
    xd_pkg_free(properties.preview_pkg);
    g_free(properties.preview_pkg_path);
    g_free(properties.preview_pkg_variant);
    xd_strbuf_free(&properties.chooser_out);
    free_heads(properties.heads, properties.head_count);
    free_heads(properties.saved_heads, properties.saved_count);
    for (size_t i = 0; i < properties.wallpaper_count; i++)
        xd_free(properties.wallpapers[i]);
    xd_free(properties.wallpapers);
    xd_scheme_list_free(&properties.schemes);
    g_ptr_array_unref(properties.icon_themes);
    g_ptr_array_unref(properties.gtk_themes);
    g_ptr_array_unref(properties.qt_styles);
    return EXIT_SUCCESS;
}
